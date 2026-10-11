// viewer.c — in-window file viewer: images (stb_image) + 3D meshes (.obj/.stl/.ply)
#include "terminal.h"
#include "viewer.h"

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "stb_image.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- mat4

typedef float mat4[16]; // column-major

static void m4_identity(mat4 m) {
    memset(m, 0, sizeof(mat4));
    m[0] = m[5] = m[10] = m[15] = 1;
}

static void m4_mul(mat4 out, const mat4 a, const mat4 b) {
    mat4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++)
            r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] +
                            a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
    memcpy(out, r, sizeof(mat4));
}

static void m4_perspective(mat4 m, float fovy, float aspect, float znear, float zfar) {
    memset(m, 0, sizeof(mat4));
    float f = 1.0f / tanf(fovy / 2);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = -1;
    m[14] = (2 * zfar * znear) / (znear - zfar);
}

static void v3_sub(float *o, const float *a, const float *b) {
    o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2];
}
static void v3_cross(float *o, const float *a, const float *b) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
static void v3_norm(float *v) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static void m4_lookat(mat4 m, const float *eye, const float *at, const float *up) {
    float f[3], s[3], u[3];
    v3_sub(f, at, eye);
    v3_norm(f);
    v3_cross(s, f, up);
    v3_norm(s);
    v3_cross(u, s, f);
    m4_identity(m);
    m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
    m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
    m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
    m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
    m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
}

// ---------------------------------------------------------------- state

typedef enum { V_NONE, V_IMAGE, V_MESH } ViewMode;

struct Viewer {
    ViewMode mode;
    char path[1024];
    char status[1280];

    // image
    GLuint img_tex;
    int img_w, img_h;
    float zoom, pan_x, pan_y; // zoom 0 = fit
    GLuint img_prog, img_vao, img_vbo;

    // mesh
    GLuint mesh_prog, mesh_vao, mesh_vbo;
    size_t mesh_verts;
    float center[3], radius;
    float yaw, pitch, dist;
    bool auto_rotate;

    // shared input
    bool dragging;
    double last_mx, last_my;
};

// ---------------------------------------------------------------- shaders

static GLuint vw_compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "viewer shader: %s\n", log);
    }
    return s;
}

static GLuint vw_program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    GLuint v = vw_compile(GL_VERTEX_SHADER, vs);
    GLuint f = vw_compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

static const char *IMG_VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos; layout(location=1) in vec2 uv;\n"
    "out vec2 v_uv;\n"
    "void main(){ gl_Position = vec4(pos,0,1); v_uv = uv; }\n";
static const char *IMG_FS =
    "#version 330 core\n"
    "in vec2 v_uv; uniform sampler2D tex; out vec4 frag;\n"
    "void main(){ frag = texture(tex, v_uv); }\n";

static const char *MESH_VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 pos; layout(location=1) in vec3 nrm;\n"
    "uniform mat4 mvp; uniform mat4 model;\n"
    "out vec3 v_nrm; out vec3 v_pos;\n"
    "void main(){\n"
    "  gl_Position = mvp * vec4(pos,1);\n"
    "  v_nrm = mat3(model) * nrm;\n"
    "  v_pos = (model * vec4(pos,1)).xyz;\n"
    "}\n";
static const char *MESH_FS =
    "#version 330 core\n"
    "in vec3 v_nrm; in vec3 v_pos;\n"
    "uniform vec3 eye;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  vec3 base = vec3(0.48, 0.63, 0.97);\n"
    "  vec3 N = normalize(v_nrm);\n"
    "  if (!gl_FrontFacing) N = -N;\n"
    "  vec3 L = normalize(vec3(0.4, 0.8, 0.6));\n"
    "  vec3 V = normalize(eye - v_pos);\n"
    "  vec3 H = normalize(L + V);\n"
    "  float diff = max(dot(N, L), 0.0);\n"
    "  float spec = pow(max(dot(N, H), 0.0), 48.0);\n"
    "  float fill = max(dot(N, normalize(vec3(-0.5, 0.2, -0.8))), 0.0) * 0.25;\n"
    "  vec3 col = base * (0.18 + 0.75*diff + fill) + vec3(0.9)*spec*0.5;\n"
    "  frag = vec4(col, 1.0);\n"
    "}\n";

// ---------------------------------------------------------------- helpers

typedef struct { float *data; size_t len, cap; } FArr;

static void fa_push(FArr *a, float v) {
    if (a->len == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 4096;
        a->data = realloc(a->data, a->cap * sizeof(float));
    }
    a->data[a->len++] = v;
}

static void fa_push3(FArr *a, float x, float y, float z) {
    fa_push(a, x); fa_push(a, y); fa_push(a, z);
}

// ---------------------------------------------------------------- obj

// emits interleaved pos(3)+normal(3) triangles into out
static bool load_obj(const char *path, FArr *out) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    FArr pos = {0}, nrm = {0};
    // face buffer: up to 64-gon fan
    long vidx[64], nidx[64];

    char line[4096];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == 'v' && line[1] == ' ') {
            float x, y, z;
            if (sscanf(line + 2, "%f %f %f", &x, &y, &z) == 3) fa_push3(&pos, x, y, z);
        } else if (line[0] == 'v' && line[1] == 'n') {
            float x, y, z;
            if (sscanf(line + 3, "%f %f %f", &x, &y, &z) == 3) fa_push3(&nrm, x, y, z);
        } else if (line[0] == 'f' && (line[1] == ' ' || line[1] == '\t')) {
            int n = 0;
            char *s = line + 1;
            while (*s && n < 64) {
                while (*s == ' ' || *s == '\t') s++;
                if (!*s || *s == '\n' || *s == '\r') break;
                long v = strtol(s, &s, 10);
                long vn = 0;
                if (*s == '/') {
                    s++;
                    if (*s != '/') strtol(s, &s, 10); // texcoord: skip
                    if (*s == '/') { s++; vn = strtol(s, &s, 10); }
                }
                size_t np = pos.len / 3, nn = nrm.len / 3;
                vidx[n] = v < 0 ? (long)np + v : v - 1;
                nidx[n] = vn < 0 ? (long)nn + vn : vn - 1;
                if (vn == 0) nidx[n] = -1;
                n++;
                while (*s && *s != ' ' && *s != '\t' && *s != '\n') s++;
            }
            for (int i = 1; i + 1 < n; i++) { // fan triangulation
                long tri_v[3] = { vidx[0], vidx[i], vidx[i + 1] };
                long tri_n[3] = { nidx[0], nidx[i], nidx[i + 1] };
                // face normal fallback
                float fnx = 0, fny = 0, fnz = 1;
                bool need_fn = tri_n[0] < 0 || tri_n[1] < 0 || tri_n[2] < 0;
                if (need_fn) {
                    float *a = pos.data + tri_v[0] * 3;
                    float *b = pos.data + tri_v[1] * 3;
                    float *c = pos.data + tri_v[2] * 3;
                    float e1[3], e2[3], fn[3];
                    v3_sub(e1, b, a);
                    v3_sub(e2, c, a);
                    v3_cross(fn, e1, e2);
                    v3_norm(fn);
                    fnx = fn[0]; fny = fn[1]; fnz = fn[2];
                }
                for (int k = 0; k < 3; k++) {
                    long vi = tri_v[k];
                    if (vi < 0 || (size_t)vi * 3 + 2 >= pos.len) continue;
                    fa_push3(out, pos.data[vi * 3], pos.data[vi * 3 + 1], pos.data[vi * 3 + 2]);
                    long ni = tri_n[k];
                    if (ni >= 0 && (size_t)ni * 3 + 2 < nrm.len)
                        fa_push3(out, nrm.data[ni * 3], nrm.data[ni * 3 + 1], nrm.data[ni * 3 + 2]);
                    else
                        fa_push3(out, fnx, fny, fnz);
                }
            }
        }
    }
    fclose(f);
    free(pos.data);
    free(nrm.data);
    return out->len >= 18; // at least one triangle
}

// ---------------------------------------------------------------- stl

static bool load_stl(const char *path, FArr *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char head[84];
    if (fread(head, 1, 84, f) == 84) {
        uint32_t count;
        memcpy(&count, head + 80, 4);
        if ((long)(84 + (uint64_t)count * 50) == size) { // binary STL
            for (uint32_t i = 0; i < count; i++) {
                float rec[12];
                uint16_t attr;
                if (fread(rec, 4, 12, f) != 12 || fread(&attr, 2, 1, f) != 1) break;
                float *fn = rec;
                for (int k = 0; k < 3; k++) {
                    float *v = rec + 3 + k * 3;
                    fa_push3(out, v[0], v[1], v[2]);
                    fa_push3(out, fn[0], fn[1], fn[2]);
                }
            }
            fclose(f);
            return out->len >= 18;
        }
    }

    // ascii STL
    fseek(f, 0, SEEK_SET);
    char line[512];
    float fn[3] = { 0, 0, 1 };
    float tri[9];
    int vcount = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (strncmp(s, "facet normal", 12) == 0) {
            sscanf(s + 12, "%f %f %f", &fn[0], &fn[1], &fn[2]);
            vcount = 0;
        } else if (strncmp(s, "vertex", 6) == 0 && vcount < 3) {
            sscanf(s + 6, "%f %f %f", &tri[vcount * 3], &tri[vcount * 3 + 1],
                   &tri[vcount * 3 + 2]);
            if (++vcount == 3)
                for (int k = 0; k < 3; k++) {
                    fa_push3(out, tri[k * 3], tri[k * 3 + 1], tri[k * 3 + 2]);
                    fa_push3(out, fn[0], fn[1], fn[2]);
                }
        }
    }
    fclose(f);
    return out->len >= 18;
}

// ---------------------------------------------------------------- ply

// PLY: ascii, binary_little_endian and binary_big_endian. Uses vertex x/y/z
// and face vertex_indices (polygons fan-triangulated, flat-shaded); every
// other property and element is read and skipped.

enum { PT_I8, PT_U8, PT_I16, PT_U16, PT_I32, PT_U32, PT_F32, PT_F64, PT_BAD };

static int ply_type(const char *t) {
    static const struct { const char *name; int type; } map[] = {
        {"char", PT_I8}, {"int8", PT_I8}, {"uchar", PT_U8}, {"uint8", PT_U8},
        {"short", PT_I16}, {"int16", PT_I16}, {"ushort", PT_U16}, {"uint16", PT_U16},
        {"int", PT_I32}, {"int32", PT_I32}, {"uint", PT_U32}, {"uint32", PT_U32},
        {"float", PT_F32}, {"float32", PT_F32}, {"double", PT_F64}, {"float64", PT_F64},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strcmp(t, map[i].name) == 0) return map[i].type;
    return PT_BAD;
}

typedef struct { int type, count_type; bool list; char name[32]; } PlyProp;
typedef struct { char name[32]; long count; int nprops; PlyProp props[32]; } PlyElem;

// one scalar of the given type; fmt: 0 ascii, 1 little endian, 2 big endian
static bool ply_read(FILE *f, int fmt, int type, double *out) {
    if (fmt == 0) return fscanf(f, "%lf", out) == 1;
    static const int size[] = { 1, 1, 2, 2, 4, 4, 4, 8 };
    unsigned char b[8];
    int n = size[type];
    if (fread(b, 1, (size_t)n, f) != (size_t)n) return false;
    bool host_le = *(const unsigned char *)&(uint16_t){1} == 1;
    if ((fmt == 1) != host_le)
        for (int i = 0; i < n / 2; i++) { unsigned char t = b[i]; b[i] = b[n - 1 - i]; b[n - 1 - i] = t; }
    switch (type) {
    case PT_I8:  *out = (int8_t)b[0]; break;
    case PT_U8:  *out = b[0]; break;
    case PT_I16: { int16_t v; memcpy(&v, b, 2); *out = v; } break;
    case PT_U16: { uint16_t v; memcpy(&v, b, 2); *out = v; } break;
    case PT_I32: { int32_t v; memcpy(&v, b, 4); *out = v; } break;
    case PT_U32: { uint32_t v; memcpy(&v, b, 4); *out = v; } break;
    case PT_F32: { float v; memcpy(&v, b, 4); *out = v; } break;
    case PT_F64: { double v; memcpy(&v, b, 8); *out = v; } break;
    default: return false;
    }
    return true;
}

static bool load_ply(const char *path, FArr *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char line[512];
    if (!fgets(line, sizeof line, f) || strncmp(line, "ply", 3) != 0) { fclose(f); return false; }

    int fmt = -1, nel = 0;
    PlyElem el[16];
    while (fgets(line, sizeof line, f)) {
        char a[32] = "", b[32] = "", c[32] = "", d[32] = "", e[32] = "";
        sscanf(line, "%31s %31s %31s %31s %31s", a, b, c, d, e);
        if (strcmp(a, "format") == 0)
            fmt = strcmp(b, "ascii") == 0 ? 0 : strcmp(b, "binary_little_endian") == 0 ? 1
                : strcmp(b, "binary_big_endian") == 0 ? 2 : -1;
        else if (strcmp(a, "element") == 0 && nel < 16) {
            PlyElem *x = &el[nel++];
            memset(x, 0, sizeof *x);
            snprintf(x->name, sizeof x->name, "%s", b);
            x->count = atol(c);
        } else if (strcmp(a, "property") == 0 && nel > 0 && el[nel - 1].nprops < 32) {
            PlyProp *pr = &el[nel - 1].props[el[nel - 1].nprops++];
            if (strcmp(b, "list") == 0) {
                pr->list = true;
                pr->count_type = ply_type(c);
                pr->type = ply_type(d);
                snprintf(pr->name, sizeof pr->name, "%s", e);
            } else {
                pr->type = ply_type(b);
                snprintf(pr->name, sizeof pr->name, "%s", c);
            }
            if (pr->type == PT_BAD || (pr->list && pr->count_type == PT_BAD)) { fclose(f); return false; }
        } else if (strcmp(a, "end_header") == 0) break;
    }
    if (fmt < 0) { fclose(f); return false; }

    FArr pos = {0};
    bool ok = true;
    for (int ei = 0; ei < nel && ok; ei++) {
        PlyElem *x = &el[ei];
        bool is_vertex = strcmp(x->name, "vertex") == 0;
        bool is_face = strcmp(x->name, "face") == 0;
        for (long r = 0; r < x->count && ok; r++) {
            float xyz[3] = { 0, 0, 0 };
            for (int pi = 0; pi < x->nprops && ok; pi++) {
                PlyProp *pr = &x->props[pi];
                double val;
                if (!pr->list) {
                    ok = ply_read(f, fmt, pr->type, &val);
                    if (is_vertex && !pr->name[1] && pr->name[0] >= 'x' && pr->name[0] <= 'z')
                        xyz[pr->name[0] - 'x'] = (float)val;
                    continue;
                }
                double cnt;
                if (!(ok = ply_read(f, fmt, pr->count_type, &cnt))) break;
                int n = (int)cnt;
                long idx[256];
                for (int k = 0; k < n && ok; k++) {
                    ok = ply_read(f, fmt, pr->type, &val);
                    if (k < 256) idx[k] = (long)val;
                }
                bool faces = is_face && (strcmp(pr->name, "vertex_indices") == 0 ||
                                         strcmp(pr->name, "vertex_index") == 0);
                if (!faces || n > 256) continue;
                size_t nv = pos.len / 3;
                for (int k = 1; k + 1 < n; k++) { // fan triangulation, flat normals
                    long t[3] = { idx[0], idx[k], idx[k + 1] };
                    if (t[0] < 0 || t[1] < 0 || t[2] < 0 ||
                        (size_t)t[0] >= nv || (size_t)t[1] >= nv || (size_t)t[2] >= nv) continue;
                    float e1[3], e2[3], fn[3];
                    v3_sub(e1, pos.data + t[1] * 3, pos.data + t[0] * 3);
                    v3_sub(e2, pos.data + t[2] * 3, pos.data + t[0] * 3);
                    v3_cross(fn, e1, e2);
                    v3_norm(fn);
                    for (int j = 0; j < 3; j++) {
                        float *pv = pos.data + t[j] * 3;
                        fa_push3(out, pv[0], pv[1], pv[2]);
                        fa_push3(out, fn[0], fn[1], fn[2]);
                    }
                }
            }
            if (is_vertex && ok) fa_push3(&pos, xyz[0], xyz[1], xyz[2]);
        }
    }
    fclose(f);
    free(pos.data);
    return out->len >= 18;
}

// ---------------------------------------------------------------- open

static const char *ext_of(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot ? dot + 1 : "";
}

static bool ext_in(const char *e, const char *list) {
    char low[16];
    size_t i = 0;
    for (; e[i] && i < 15; i++) low[i] = (char)tolower((unsigned char)e[i]);
    low[i] = 0;
    char pat[20];
    snprintf(pat, sizeof pat, " %s ", low);
    return strstr(list, pat) != NULL;
}

Viewer *viewer_new(void) {
    Viewer *v = calloc(1, sizeof(Viewer));
    v->img_prog = vw_program(IMG_VS, IMG_FS);
    v->mesh_prog = vw_program(MESH_VS, MESH_FS);
    glGenVertexArrays(1, &v->img_vao);
    glGenBuffers(1, &v->img_vbo);
    glBindVertexArray(v->img_vao);
    glBindBuffer(GL_ARRAY_BUFFER, v->img_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glGenVertexArrays(1, &v->mesh_vao);
    glGenBuffers(1, &v->mesh_vbo);
    glBindVertexArray(v->mesh_vao);
    glBindBuffer(GL_ARRAY_BUFFER, v->mesh_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *)12);
    glBindVertexArray(0);
    return v;
}

void viewer_free(Viewer *v) {
    if (!v) return;
    viewer_close(v);
    free(v);
}

void viewer_close(Viewer *v) {
    if (v->img_tex) { glDeleteTextures(1, &v->img_tex); v->img_tex = 0; }
    v->mesh_verts = 0;
    v->mode = V_NONE;
}

bool viewer_active(Viewer *v) { return v && v->mode != V_NONE; }
bool viewer_animating(Viewer *v) {
    return v && v->mode == V_MESH && (v->auto_rotate || v->dragging);
}
const char *viewer_status(Viewer *v) { return v->status; }

static bool open_image(Viewer *v, const char *path) {
    int w, h, n;
    stbi_set_flip_vertically_on_load(0);
    unsigned char *px = stbi_load(path, &w, &h, &n, 4);
    if (!px) {
        fprintf(stderr, "terminal: image load failed: %s\n", stbi_failure_reason());
        return false;
    }
    if (v->img_tex) glDeleteTextures(1, &v->img_tex);
    glGenTextures(1, &v->img_tex);
    glBindTexture(GL_TEXTURE_2D, v->img_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    stbi_image_free(px);
    v->img_w = w;
    v->img_h = h;
    v->zoom = 0; // fit
    v->pan_x = v->pan_y = 0;
    v->mode = V_IMAGE;
    snprintf(v->status, sizeof v->status, "%s — %dx%d", path, w, h);
    return true;
}

static bool open_mesh(Viewer *v, const char *path, bool (*load)(const char *, FArr *)) {
    FArr verts = {0};
    bool ok = load(path, &verts);
    if (!ok) {
        free(verts.data);
        fprintf(stderr, "terminal: mesh load failed: %s\n", path);
        return false;
    }

    // bounding box -> center + radius
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (size_t i = 0; i < verts.len; i += 6)
        for (int k = 0; k < 3; k++) {
            float val = verts.data[i + k];
            if (val < mn[k]) mn[k] = val;
            if (val > mx[k]) mx[k] = val;
        }
    for (int k = 0; k < 3; k++) v->center[k] = (mn[k] + mx[k]) / 2;
    float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
    v->radius = sqrtf(dx * dx + dy * dy + dz * dz) / 2;
    if (v->radius < 1e-6f) v->radius = 1;

    glBindVertexArray(v->mesh_vao);
    glBindBuffer(GL_ARRAY_BUFFER, v->mesh_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.len * sizeof(float)),
                 verts.data, GL_STATIC_DRAW);
    glBindVertexArray(0);
    v->mesh_verts = verts.len / 6;
    free(verts.data);

    v->yaw = 0.6f;
    v->pitch = 0.35f;
    v->dist = v->radius * 2.6f;
    v->auto_rotate = true;
    v->mode = V_MESH;
    snprintf(v->status, sizeof v->status, "%s — %zu triangles", path,
             v->mesh_verts / 3);
    return true;
}

int viewer_open(Viewer *v, const char *path) {
    const char *e = ext_of(path);
    snprintf(v->path, sizeof v->path, "%s", path);
    if (ext_in(e, " png jpg jpeg gif bmp tga psd hdr pic pnm pgm ppm "))
        return open_image(v, path) ? VIEW_OK : VIEW_FAIL;
    if (ext_in(e, " obj "))
        return open_mesh(v, path, load_obj) ? VIEW_OK : VIEW_FAIL;
    if (ext_in(e, " stl "))
        return open_mesh(v, path, load_stl) ? VIEW_OK : VIEW_FAIL;
    if (ext_in(e, " ply "))
        return open_mesh(v, path, load_ply) ? VIEW_OK : VIEW_FAIL;
    return VIEW_UNSUPPORTED; // caller may hand it to the shell (pager etc.)
}

// ---------------------------------------------------------------- input

void viewer_mouse_button(Viewer *v, bool press, double x, double y) {
    v->dragging = press;
    v->last_mx = x;
    v->last_my = y;
    if (press && v->mode == V_MESH) v->auto_rotate = false;
}

void viewer_mouse_move(Viewer *v, double x, double y) {
    if (!v->dragging) return;
    double dx = x - v->last_mx, dy = y - v->last_my;
    v->last_mx = x;
    v->last_my = y;
    if (v->mode == V_MESH) {
        v->yaw += (float)dx * 0.008f;
        v->pitch += (float)dy * 0.008f;
        if (v->pitch > 1.55f) v->pitch = 1.55f;
        if (v->pitch < -1.55f) v->pitch = -1.55f;
    } else if (v->mode == V_IMAGE) {
        v->pan_x += (float)dx;
        v->pan_y += (float)dy;
    }
}

void viewer_scroll(Viewer *v, double dy) {
    if (v->mode == V_MESH) {
        v->dist *= dy > 0 ? 0.92f : 1.087f;
        float lo = v->radius * 1.05f, hi = v->radius * 20;
        if (v->dist < lo) v->dist = lo;
        if (v->dist > hi) v->dist = hi;
    } else if (v->mode == V_IMAGE) {
        if (v->zoom == 0) v->zoom = 1; // leave fit mode at current-ish scale
        v->zoom *= dy > 0 ? 1.1f : 0.909f;
        if (v->zoom < 0.05f) v->zoom = 0.05f;
        if (v->zoom > 64) v->zoom = 64;
    }
}

// returns true if the key closed the viewer
bool viewer_key_close(Viewer *v, int key_escape_or_q) {
    (void)key_escape_or_q;
    viewer_close(v);
    return true;
}

// ---------------------------------------------------------------- frame

void viewer_frame(Viewer *v, int fb_w, int fb_h, double dt, float scale) {
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.075f, 0.078f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (v->mode == V_IMAGE) {
        glDisable(GL_DEPTH_TEST);
        float z = v->zoom;
        if (z == 0) { // fit
            float zx = (float)fb_w / v->img_w, zy = (float)fb_h / v->img_h;
            z = zx < zy ? zx : zy;
            if (z > 1) z = 1; // don't upscale in fit mode
        }
        float w = v->img_w * z, h = v->img_h * z;
        float cx = fb_w / 2.0f + v->pan_x * scale, cy = fb_h / 2.0f + v->pan_y * scale;
        float x0 = (cx - w / 2) / fb_w * 2 - 1, x1 = (cx + w / 2) / fb_w * 2 - 1;
        float y0 = 1 - (cy - h / 2) / fb_h * 2, y1 = 1 - (cy + h / 2) / fb_h * 2;
        float quad[] = {
            x0, y0, 0, 0,  x1, y0, 1, 0,  x0, y1, 0, 1,
            x1, y0, 1, 0,  x1, y1, 1, 1,  x0, y1, 0, 1,
        };
        glUseProgram(v->img_prog);
        glBindVertexArray(v->img_vao);
        glBindBuffer(GL_ARRAY_BUFFER, v->img_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STREAM_DRAW);
        glBindTexture(GL_TEXTURE_2D, v->img_tex);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    } else if (v->mode == V_MESH) {
        if (v->auto_rotate) v->yaw += (float)dt * 0.5f;

        float eye[3] = {
            v->center[0] + v->dist * cosf(v->pitch) * sinf(v->yaw),
            v->center[1] + v->dist * sinf(v->pitch),
            v->center[2] + v->dist * cosf(v->pitch) * cosf(v->yaw),
        };
        float up[3] = { 0, 1, 0 };
        mat4 proj, view, mvp, model;
        m4_identity(model);
        m4_perspective(proj, 45.0f * 3.14159265f / 180.0f,
                       (float)fb_w / (float)fb_h,
                       v->radius * 0.01f, v->radius * 60.0f);
        m4_lookat(view, eye, v->center, up);
        m4_mul(mvp, proj, view);

        glEnable(GL_DEPTH_TEST);
        glUseProgram(v->mesh_prog);
        glUniformMatrix4fv(glGetUniformLocation(v->mesh_prog, "mvp"), 1, GL_FALSE, mvp);
        glUniformMatrix4fv(glGetUniformLocation(v->mesh_prog, "model"), 1, GL_FALSE, model);
        glUniform3fv(glGetUniformLocation(v->mesh_prog, "eye"), 1, eye);
        glBindVertexArray(v->mesh_vao);
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)v->mesh_verts);
        glDisable(GL_DEPTH_TEST);
    }
    glBindVertexArray(0);
}
