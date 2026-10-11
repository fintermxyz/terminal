// render.c — GPU renderer: FreeType glyph atlas + one instanced draw call
#include "terminal.h"

#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include <math.h>
#include <stdio.h>
#include <string.h>

#define ATLAS_SIZE 2048
#define GLYPH_CACHE_SIZE 4096 // power of two

typedef struct {
    uint32_t key;   // cp | bold<<24 | italic<<25 (0 = empty slot)
    float u, v, w, h;          // atlas rect (normalized)
    float bx, by;              // bearing (px)
    float pw, ph;              // pixel size
} Glyph;

// one quad instance: dst rect (px), src rect (uv), rgba
typedef struct {
    float x, y, w, h;
    float u, v, uw, vh;
    float r, g, b, a;
} Inst;

struct Renderer {
    const Config *cfg;
    float scale;
    double font_pt;

    FT_Library ft;
    FT_Face face;
    FT_Face fallback[4]; // tried in order for codepoints the main font lacks
    int nfallback;
    int cell_w, cell_h, baseline;

    GLuint prog, vao, quad_vbo, inst_vbo, atlas_tex;
    GLint u_screen;

    uint8_t *atlas; // CPU copy for packing
    int pen_x, pen_y, row_h;
    bool atlas_dirty;

    Glyph cache[GLYPH_CACHE_SIZE];

    Inst *insts;
    size_t insts_len, insts_cap;

    int notch_x, notch_h; // window buttons: rows above notch_h start at notch_x
};

void render_set_notch(Renderer *r, int x, int h) { r->notch_x = x; r->notch_h = h; }

// left edge of grid row y: rows under the window buttons shift right past them
int render_row_x(Renderer *r, int ox, int oy, int y) {
    if (oy + y * r->cell_h < r->notch_h && r->notch_x > ox) return r->notch_x;
    return ox;
}

// ---------------------------------------------------------------- shaders

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 corner;\n"
    "layout(location=1) in vec4 dst;\n"
    "layout(location=2) in vec4 src;\n"
    "layout(location=3) in vec4 color;\n"
    "uniform vec2 screen;\n"
    "out vec2 v_uv; out vec4 v_color;\n"
    "void main(){\n"
    "  vec2 p = dst.xy + corner * dst.zw;\n"
    "  gl_Position = vec4(p.x/screen.x*2.0-1.0, 1.0-p.y/screen.y*2.0, 0.0, 1.0);\n"
    "  v_uv = src.xy + corner * src.zw;\n"
    "  v_color = color;\n"
    "}\n";

static const char *FS =
    "#version 330 core\n"
    "in vec2 v_uv; in vec4 v_color;\n"
    "uniform sampler2D atlas;\n"
    "out vec4 frag;\n"
    "void main(){\n"
    "  float a = texture(atlas, v_uv).r;\n"
    "  frag = vec4(v_color.rgb, v_color.a * a);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader error: %s\n", log);
        exit(1);
    }
    return s;
}

// ---------------------------------------------------------------- font

static bool load_face(Renderer *r, double pt) {
    if (r->face) { FT_Done_Face(r->face); r->face = NULL; }
    if (FT_New_Face(r->ft, r->cfg->font_path, 0, &r->face)) {
        fprintf(stderr, "terminal: cannot load font %s\n", r->cfg->font_path);
        return false;
    }
    FT_Set_Char_Size(r->face, 0, (FT_F26Dot6)(pt * 64.0),
                     (FT_UInt)(72 * r->scale), (FT_UInt)(72 * r->scale));
    r->font_pt = pt;

    // cell metrics from the face
    FT_Load_Char(r->face, 'M', FT_LOAD_RENDER);
    int adv = (int)(r->face->glyph->advance.x >> 6);
    r->cell_w = adv > 0 ? adv : (int)(pt * r->scale * 0.6);
    r->cell_h = (int)(r->face->size->metrics.height >> 6);
    if (r->cell_h <= 0) r->cell_h = (int)(pt * r->scale * 1.2);
    r->baseline = (int)(r->face->size->metrics.ascender >> 6);
    for (int i = 0; i < r->nfallback; i++)
        FT_Set_Char_Size(r->fallback[i], 0, (FT_F26Dot6)(pt * 64.0),
                         (FT_UInt)(72 * r->scale), (FT_UInt)(72 * r->scale));
    return true;
}

static void load_fallbacks(Renderer *r) {
    static const char *paths[] = {
        "/System/Library/Fonts/Apple Symbols.ttf",
        "/System/Library/Fonts/Supplemental/STIXTwoMath.otf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    };
    for (size_t i = 0; i < sizeof paths / sizeof *paths && r->nfallback < 4; i++)
        if (!FT_New_Face(r->ft, paths[i], 0, &r->fallback[r->nfallback])) r->nfallback++;
}

// the face that has cp: the configured font first, then the fallbacks
static FT_Face face_for(Renderer *r, uint32_t cp) {
    if (FT_Get_Char_Index(r->face, cp)) return r->face;
    for (int i = 0; i < r->nfallback; i++)
        if (FT_Get_Char_Index(r->fallback[i], cp)) return r->fallback[i];
    return r->face;
}

static void atlas_reset(Renderer *r) {
    memset(r->atlas, 0, (size_t)ATLAS_SIZE * ATLAS_SIZE);
    memset(r->cache, 0, sizeof r->cache);
    r->atlas[0] = 0xff; // (0,0) = solid white pixel, used for rect fills
    r->pen_x = 2;
    r->pen_y = 0;
    r->row_h = 2;
    r->atlas_dirty = true;
}

static Glyph *glyph_get(Renderer *r, uint32_t cp, bool bold, bool italic) {
    uint32_t key = cp | (bold ? 1u << 24 : 0) | (italic ? 1u << 25 : 0);
    uint32_t h = (key * 2654435761u) & (GLYPH_CACHE_SIZE - 1);
    for (uint32_t i = 0; i < GLYPH_CACHE_SIZE; i++) {
        Glyph *g = &r->cache[(h + i) & (GLYPH_CACHE_SIZE - 1)];
        if (g->key == key) return g;
        if (g->key == 0) {
            // rasterize
            FT_Int32 flags = FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT;
            FT_Face face = face_for(r, cp);
            if (FT_Load_Char(face, cp, flags)) return NULL;
            FT_GlyphSlot slot = face->glyph;
            if (bold) FT_GlyphSlot_Embolden(slot);
            if (italic) FT_GlyphSlot_Oblique(slot);
            if (bold || italic) FT_Render_Glyph(slot, FT_RENDER_MODE_LIGHT);

            FT_Bitmap *bm = &slot->bitmap;
            int w = (int)bm->width, hh = (int)bm->rows;

            if (r->pen_x + w + 1 >= ATLAS_SIZE) {
                r->pen_x = 0;
                r->pen_y += r->row_h + 1;
                r->row_h = 0;
            }
            if (r->pen_y + hh + 1 >= ATLAS_SIZE) { // atlas full: nuke & retry
                atlas_reset(r);
                return glyph_get(r, cp, bold, italic);
            }

            for (int y = 0; y < hh; y++)
                memcpy(r->atlas + (size_t)(r->pen_y + y) * ATLAS_SIZE + r->pen_x,
                       bm->buffer + (size_t)y * (size_t)bm->pitch, (size_t)w);

            g->key = key;
            g->u = (float)r->pen_x / ATLAS_SIZE;
            g->v = (float)r->pen_y / ATLAS_SIZE;
            g->w = (float)w / ATLAS_SIZE;
            g->h = (float)hh / ATLAS_SIZE;
            g->bx = (float)slot->bitmap_left;
            g->by = (float)slot->bitmap_top;
            g->pw = (float)w;
            g->ph = (float)hh;

            r->pen_x += w + 1;
            if (hh > r->row_h) r->row_h = hh;
            r->atlas_dirty = true;
            return g;
        }
    }
    return NULL; // cache totally full (shouldn't happen)
}

// ---------------------------------------------------------------- lifecycle

Renderer *render_new(const Config *cfg, float content_scale) {
    Renderer *r = calloc(1, sizeof(Renderer));
    r->cfg = cfg;
    r->scale = content_scale;

    if (FT_Init_FreeType(&r->ft)) { free(r); return NULL; }
    load_fallbacks(r);
    if (!load_face(r, cfg->font_size)) { free(r); return NULL; }

    r->atlas = malloc((size_t)ATLAS_SIZE * ATLAS_SIZE);
    atlas_reset(r);

    GLuint vs = compile(GL_VERTEX_SHADER, VS);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS);
    r->prog = glCreateProgram();
    glAttachShader(r->prog, vs);
    glAttachShader(r->prog, fs);
    glLinkProgram(r->prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    r->u_screen = glGetUniformLocation(r->prog, "screen");

    static const float corners[] = { 0,0, 1,0, 0,1, 1,0, 1,1, 0,1 };
    glGenVertexArrays(1, &r->vao);
    glBindVertexArray(r->vao);

    glGenBuffers(1, &r->quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof corners, corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    glGenBuffers(1, &r->inst_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, r->inst_vbo);
    for (int i = 0; i < 3; i++) {
        glEnableVertexAttribArray(1 + (GLuint)i);
        glVertexAttribPointer(1 + (GLuint)i, 4, GL_FLOAT, GL_FALSE,
                              sizeof(Inst), (void *)(sizeof(float) * 4 * (size_t)i));
        glVertexAttribDivisor(1 + (GLuint)i, 1);
    }

    glGenTextures(1, &r->atlas_tex);
    glBindTexture(GL_TEXTURE_2D, r->atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_SIZE, ATLAS_SIZE, 0,
                 GL_RED, GL_UNSIGNED_BYTE, r->atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    r->atlas_dirty = false;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    r->insts_cap = 8192;
    r->insts = malloc(sizeof(Inst) * r->insts_cap);
    return r;
}

void render_free(Renderer *r) {
    if (!r) return;
    if (r->face) FT_Done_Face(r->face);
    for (int i = 0; i < r->nfallback; i++) FT_Done_Face(r->fallback[i]);
    FT_Done_FreeType(r->ft);
    free(r->atlas);
    free(r->insts);
    free(r);
}

bool render_set_font_size(Renderer *r, double pt) {
    if (pt < 5 || pt > 96) return false;
    if (!load_face(r, pt)) return false;
    atlas_reset(r);
    return true;
}

double render_font_size(Renderer *r) { return r->font_pt; }
int render_cell_w(Renderer *r) { return r->cell_w; }
int render_cell_h(Renderer *r) { return r->cell_h; }
float render_scale(Renderer *r) { return r->scale; }
int render_baseline(Renderer *r) { return r->baseline; }

// ---------------------------------------------------------------- frame

static void push(Renderer *r, Inst in) {
    if (r->insts_len == r->insts_cap) {
        r->insts_cap *= 2;
        r->insts = realloc(r->insts, sizeof(Inst) * r->insts_cap);
    }
    r->insts[r->insts_len++] = in;
}

static void push_rect(Renderer *r, float x, float y, float w, float h, uint32_t col, float a) {
    // sample the white pixel at atlas (0,0)
    float half = 0.5f / ATLAS_SIZE;
    push(r, (Inst){ x, y, w, h, half, half, 0, 0,
                    ((col >> 16) & 255) / 255.0f, ((col >> 8) & 255) / 255.0f,
                    (col & 255) / 255.0f, a });
}

static void cell_colors(const Cell *c, uint32_t *fg, uint32_t *bg) {
    uint32_t f = c->fg, b = c->bg;
    if (c->attrs & ATTR_REVERSE) { uint32_t tmp = f; f = b; b = tmp; }
    if (c->attrs & ATTR_DIM)
        f = ((f >> 1) & 0x7f7f7f);
    *fg = f;
    *bg = b;
}

// ---------------------------------------------------------------- compositor

void render_begin(Renderer *r, int fb_w, int fb_h, uint32_t clear_bg) {
    r->insts_len = 0;
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(((clear_bg >> 16) & 255) / 255.0f, ((clear_bg >> 8) & 255) / 255.0f,
                 (clear_bg & 255) / 255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void render_fill(Renderer *r, float x, float y, float w, float h, uint32_t col, float a) {
    push_rect(r, x, y, w, h, col, a);
}

float render_text(Renderer *r, float x, float y, const char *s, uint32_t col) {
    float fr = ((col >> 16) & 255) / 255.0f;
    float fg = ((col >> 8) & 255) / 255.0f;
    float fb = (col & 255) / 255.0f;
    float pen = x;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp = *p;
        int extra = 0;
        if (cp >= 0xf0) { cp &= 0x07; extra = 3; }
        else if (cp >= 0xe0) { cp &= 0x0f; extra = 2; }
        else if (cp >= 0xc0) { cp &= 0x1f; extra = 1; }
        p++;
        for (int i = 0; i < extra && (*p & 0xc0) == 0x80; i++, p++)
            cp = (cp << 6) | (*p & 0x3f);
        if (cp > ' ') {
            Glyph *g = glyph_get(r, cp, false, false);
            if (g && g->pw > 0)
                push(r, (Inst){ pen + g->bx, y + (float)r->baseline - g->by,
                                g->pw, g->ph, g->u, g->v, g->w, g->h,
                                fr, fg, fb, 1.0f });
        }
        pen += r->cell_w;
    }
    return pen - x;
}

// draws one term's grid with cell (0,0) at pixel (ox,oy). the term is assumed
// pre-sized to fit; cursor is only drawn for the focused pane.
// ---------------------------------------------------------------- drawn glyphs
// Block elements and box-drawing lines are drawn as rectangles snapped to the
// cell, so they tile seamlessly (font glyphs rarely fill the full line height).

static void cell_rect(Renderer *r, float x, float y, float w, float h,
                      float fx0, float fy0, float fx1, float fy1, uint32_t col, float a) {
    float x0 = roundf(x + w * fx0), x1 = roundf(x + w * fx1);
    float y0 = roundf(y + h * fy0), y1 = roundf(y + h * fy1);
    if (x1 > x0 && y1 > y0) push_rect(r, x0, y0, x1 - x0, y1 - y0, col, a);
}

static bool draw_block(Renderer *r, uint32_t cp, float x, float y, float w, float h, uint32_t col) {
    if (cp < 0x2580 || cp > 0x259f) return false;
    if (cp == 0x2580) cell_rect(r, x, y, w, h, 0, 0, 1, 0.5f, col, 1);
    else if (cp <= 0x2588) cell_rect(r, x, y, w, h, 0, 1 - (cp - 0x2580) / 8.0f, 1, 1, col, 1);
    else if (cp <= 0x258f) cell_rect(r, x, y, w, h, 0, 0, (0x2590 - cp) / 8.0f, 1, col, 1);
    else if (cp == 0x2590) cell_rect(r, x, y, w, h, 0.5f, 0, 1, 1, col, 1);
    else if (cp <= 0x2593) cell_rect(r, x, y, w, h, 0, 0, 1, 1, col, (cp - 0x2590) * 0.25f);
    else if (cp == 0x2594) cell_rect(r, x, y, w, h, 0, 0, 1, 0.125f, col, 1);
    else if (cp == 0x2595) cell_rect(r, x, y, w, h, 0.875f, 0, 1, 1, col, 1);
    else {
        // quadrants ▖▗▘▙▚▛▜▝▞▟ as bits: 1 upper-left, 2 upper-right, 4 lower-left, 8 lower-right
        static const uint8_t q[] = { 4, 8, 1, 1|4|8, 1|8, 1|2|4, 1|2|8, 2, 2|4, 2|4|8 };
        uint8_t m = q[cp - 0x2596];
        if (m & 1) cell_rect(r, x, y, w, h, 0, 0, 0.5f, 0.5f, col, 1);
        if (m & 2) cell_rect(r, x, y, w, h, 0.5f, 0, 1, 0.5f, col, 1);
        if (m & 4) cell_rect(r, x, y, w, h, 0, 0.5f, 0.5f, 1, col, 1);
        if (m & 8) cell_rect(r, x, y, w, h, 0.5f, 0.5f, 1, 1, col, 1);
    }
    return true;
}

// box-drawing arms: low nibble left/right/up/down, 0x10 = heavy
enum { BL = 1, BR = 2, BU = 4, BD = 8, BH = 0x10 };
static uint8_t box_arms(uint32_t cp) {
    switch (cp) {
    case 0x2500: return BL|BR;           case 0x2501: return BL|BR|BH;
    case 0x2502: return BU|BD;           case 0x2503: return BU|BD|BH;
    case 0x250c: case 0x256d: return BR|BD;   case 0x250f: return BR|BD|BH;
    case 0x2510: case 0x256e: return BL|BD;   case 0x2513: return BL|BD|BH;
    case 0x2514: case 0x2570: return BR|BU;   case 0x2517: return BR|BU|BH;
    case 0x2518: case 0x256f: return BL|BU;   case 0x251b: return BL|BU|BH;
    case 0x251c: return BU|BD|BR;        case 0x2523: return BU|BD|BR|BH;
    case 0x2524: return BU|BD|BL;        case 0x252b: return BU|BD|BL|BH;
    case 0x252c: return BL|BR|BD;        case 0x2533: return BL|BR|BD|BH;
    case 0x2534: return BL|BR|BU;        case 0x253b: return BL|BR|BU|BH;
    case 0x253c: return BL|BR|BU|BD;     case 0x254b: return BL|BR|BU|BD|BH;
    case 0x2574: return BL; case 0x2575: return BU; case 0x2576: return BR; case 0x2577: return BD;
    case 0x2578: return BL|BH; case 0x2579: return BU|BH; case 0x257a: return BR|BH; case 0x257b: return BD|BH;
    default: return 0;
    }
}

static bool draw_box(Renderer *r, uint32_t cp, float x, float y, float w, float h, uint32_t col) {
    uint8_t a = box_arms(cp);
    if (!a) return false;
    float t = fmaxf(1.0f, roundf(r->scale)) * ((a & BH) ? 2 : 1);
    float x0 = roundf(x), x1 = roundf(x + w), y0 = roundf(y), y1 = roundf(y + h);
    float cx = roundf(x + (w - t) / 2), cy = roundf(y + (h - t) / 2);
    if (a & BL) push_rect(r, x0, cy, cx + t - x0, t, col, 1);
    if (a & BR) push_rect(r, cx, cy, x1 - cx, t, col, 1);
    if (a & BU) push_rect(r, cx, y0, t, cy + t - y0, col, 1);
    if (a & BD) push_rect(r, cx, cy, t, y1 - cy, col, 1);
    return true;
}

void render_term_at(Renderer *r, Term *t, int ox, int oy, bool focused) {
    const Config *cfg = r->cfg;
    int cw = r->cell_w, ch = r->cell_h;
    int view_off = term_view_offset(t);

    // --- background quads (only where bg differs from window clear color)
    for (int y = 0; y < t->rows; y++) {
        const Cell *row = term_visible_row(t, y);
        if (!row) continue;
        for (int x = 0; x < t->cols; x++) {
            uint32_t fg, bg;
            cell_colors(&row[x], &fg, &bg);
            if (bg != cfg->bg)
                push_rect(r, (float)(render_row_x(r, ox, oy, y) + x * cw), (float)(oy + y * ch),
                          (float)cw, (float)ch, bg, 1.0f);
        }
    }

    // --- cursor (under glyphs for block, so glyph shows inverted on top)
    bool cursor_shown = focused && t->cursor_visible && view_off == 0;
    bool block = strcmp(cfg->cursor_style, "block") == 0;
    float cx = (float)(render_row_x(r, ox, oy, t->cy) + t->cx * cw), cy = (float)(oy + t->cy * ch);
    if (cursor_shown && block)
        push_rect(r, cx, cy, (float)cw, (float)ch, cfg->cursor_color, 1.0f);

    // --- glyphs + decorations
    for (int y = 0; y < t->rows; y++) {
        const Cell *row = term_visible_row(t, y);
        if (!row) continue;
        int rx = render_row_x(r, ox, oy, y);
        for (int x = 0; x < t->cols; x++) {
            const Cell *c = &row[x];
            if (c->attrs & ATTR_WIDEDUMMY) continue;
            uint32_t fg, bg;
            cell_colors(c, &fg, &bg);

            bool at_cursor = cursor_shown && block && view_off == 0 &&
                             x == t->cx && y == t->cy;
            if (at_cursor) fg = cfg->bg; // glyph contrasts against cursor block

            float px = (float)(rx + x * cw), py = (float)(oy + y * ch);

            float gw = (float)cw * (c->attrs & ATTR_WIDE ? 2 : 1);
            if (draw_block(r, c->cp, px, py, gw, (float)ch, fg) ||
                draw_box(r, c->cp, px, py, gw, (float)ch, fg)) {
                // drawn as rectangles
            } else if (c->cp > ' ') {
                Glyph *g = glyph_get(r, c->cp,
                                     (c->attrs & ATTR_BOLD) != 0,
                                     (c->attrs & ATTR_ITALIC) != 0);
                if (g && g->pw > 0) {
                    float fr = ((fg >> 16) & 255) / 255.0f;
                    float fgn = ((fg >> 8) & 255) / 255.0f;
                    float fb = (fg & 255) / 255.0f;
                    push(r, (Inst){ px + g->bx, py + (float)r->baseline - g->by,
                                    g->pw, g->ph, g->u, g->v, g->w, g->h,
                                    fr, fgn, fb, 1.0f });
                }
            }
            if (c->attrs & ATTR_UNDERLINE) {
                float uy = py + (float)r->baseline + 2.0f * r->scale;
                push_rect(r, px, uy, (float)cw * (c->attrs & ATTR_WIDE ? 2 : 1),
                          1.0f * r->scale, fg, 1.0f);
            }
            if (c->attrs & ATTR_STRIKE) {
                float sy = py + (float)ch * 0.5f;
                push_rect(r, px, sy, (float)cw * (c->attrs & ATTR_WIDE ? 2 : 1),
                          1.0f * r->scale, fg, 1.0f);
            }
        }
    }

    // --- non-block cursor styles on top
    if (cursor_shown && !block) {
        if (strcmp(cfg->cursor_style, "bar") == 0)
            push_rect(r, cx, cy, 2.0f * r->scale, (float)ch, cfg->cursor_color, 1.0f);
        else // underline
            push_rect(r, cx, cy + ch - 2.0f * r->scale, (float)cw,
                      2.0f * r->scale, cfg->cursor_color, 1.0f);
    }

    // --- scrollback indicator (right edge of this pane)
    if (view_off > 0)
        push_rect(r, (float)(ox + t->cols * cw - 4 * r->scale), (float)oy,
                  3.0f * r->scale, (float)(t->rows * ch) * 0.06f, cfg->fg, 0.6f);
}

void render_end(Renderer *r, int fb_w, int fb_h) {
    if (r->atlas_dirty) {
        glBindTexture(GL_TEXTURE_2D, r->atlas_tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ATLAS_SIZE, ATLAS_SIZE,
                        GL_RED, GL_UNSIGNED_BYTE, r->atlas);
        r->atlas_dirty = false;
    }
    glUseProgram(r->prog);
    glUniform2f(r->u_screen, (float)fb_w, (float)fb_h);
    glBindVertexArray(r->vao);
    glBindTexture(GL_TEXTURE_2D, r->atlas_tex);
    glBindBuffer(GL_ARRAY_BUFFER, r->inst_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(Inst) * r->insts_len),
                 r->insts, GL_STREAM_DRAW);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, (GLsizei)r->insts_len);
}

// simple single-term frame (viewer fallback path / legacy)
void render_frame(Renderer *r, Term *t, int fb_w, int fb_h) {
    int pad = (int)(r->cfg->padding * r->scale);
    render_begin(r, fb_w, fb_h, r->cfg->bg);
    render_term_at(r, t, pad, pad, true);
    render_end(r, fb_w, fb_h);
}
