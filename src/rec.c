// rec.c — JSON recorder + loader (see rec.h)
#include "rec.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// ---------------------------------------------------------------- recorder

struct Recorder {
    FILE  *f;
    double t0;
    bool   first;   // no event written yet (controls leading comma)
    char   path[1024];
};

static void json_escape(FILE *f, const char *buf, int len) {
    for (int i = 0; i < len; i++) {
        unsigned char b = (unsigned char)buf[i];
        switch (b) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:
            if (b < 0x20 || b >= 0x7f) fprintf(f, "\\u%04x", b);
            else fputc(b, f);
        }
    }
}

Recorder *rec_start(const char *path, int cols, int rows,
                    const char *title, double now) {
    FILE *f = fopen(path, "w");
    if (!f) return NULL;
    Recorder *r = calloc(1, sizeof *r);
    r->f = f;
    r->t0 = now;
    r->first = true;
    snprintf(r->path, sizeof r->path, "%s", path);
    fprintf(f, "{\"version\":1,\"width\":%d,\"height\":%d,\"title\":\"",
            cols, rows);
    json_escape(f, title ? title : "", title ? (int)strlen(title) : 0);
    fputs("\",\"events\":[\n", f);
    return r;
}

static void event_prefix(Recorder *r) {
    if (!r->first) fputs(",\n", r->f);
    r->first = false;
}

void rec_output(Recorder *r, double now, const char *buf, int len) {
    if (!r || len <= 0) return;
    event_prefix(r);
    fprintf(r->f, "[%.6f,\"o\",\"", now - r->t0);
    json_escape(r->f, buf, len);
    fputs("\"]", r->f);
}

void rec_resize(Recorder *r, double now, int cols, int rows) {
    if (!r) return;
    event_prefix(r);
    fprintf(r->f, "[%.6f,\"r\",\"%d %d\"]", now - r->t0, cols, rows);
}

void rec_stop(Recorder *r) {
    if (!r) return;
    fputs("\n]}\n", r->f);
    fclose(r->f);
    free(r);
}

const char *rec_path(Recorder *r) { return r ? r->path : ""; }

// ---------------------------------------------------------------- loader

// growable byte buffer
typedef struct { char *p; int len, cap; } Buf;
static void buf_put(Buf *b, char c) {
    if (b->len == b->cap) { b->cap = b->cap ? b->cap * 2 : 64; b->p = realloc(b->p, b->cap); }
    b->p[b->len++] = c;
}
static void utf8_put(Buf *b, unsigned cp) {
    if (cp < 0x80) buf_put(b, (char)cp);
    else if (cp < 0x800) {
        buf_put(b, (char)(0xc0 | (cp >> 6)));
        buf_put(b, (char)(0x80 | (cp & 0x3f)));
    } else {
        buf_put(b, (char)(0xe0 | (cp >> 12)));
        buf_put(b, (char)(0x80 | ((cp >> 6) & 0x3f)));
        buf_put(b, (char)(0x80 | (cp & 0x3f)));
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

// parse a JSON string starting at *pp (which must point at the opening quote).
// returns malloc'd bytes (NUL-terminated) and *out_len; advances *pp past close.
static char *parse_string(const char **pp, int *out_len) {
    const char *p = *pp;
    if (*p != '"') return NULL;
    p++;
    Buf b = {0};
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': buf_put(&b, '\n'); p++; break;
            case 'r': buf_put(&b, '\r'); p++; break;
            case 't': buf_put(&b, '\t'); p++; break;
            case 'b': buf_put(&b, '\b'); p++; break;
            case 'f': buf_put(&b, '\f'); p++; break;
            case '/': buf_put(&b, '/');  p++; break;
            case '"': buf_put(&b, '"');  p++; break;
            case '\\': buf_put(&b, '\\'); p++; break;
            case 'u': {
                p++;
                unsigned cp = 0;
                for (int i = 0; i < 4 && *p; i++) { cp = cp * 16 + hexval(*p); p++; }
                if (cp < 0x100) buf_put(&b, (char)cp); // our byte-exact encoding
                else utf8_put(&b, cp);
                break;
            }
            default: if (*p) { buf_put(&b, *p); p++; } break;
            }
        } else {
            buf_put(&b, *p);
            p++;
        }
    }
    if (*p == '"') p++;
    buf_put(&b, 0);
    b.len--; // don't count the NUL in the reported length
    *pp = p;
    if (out_len) *out_len = b.len;
    return b.p ? b.p : calloc(1, 1);
}

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
    return p;
}

static char *read_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = 0;
    if (size) *size = (long)rd;
    return buf;
}

RecFile *rec_load(const char *path) {
    long size;
    char *txt = read_file(path, &size);
    if (!txt) return NULL;

    RecFile *rf = calloc(1, sizeof *rf);
    rf->cols = 80;
    rf->rows = 24;

    const char *w = strstr(txt, "\"width\":");
    if (w) rf->cols = atoi(w + 8);
    const char *h = strstr(txt, "\"height\":");
    if (h) rf->rows = atoi(h + 9);
    const char *ti = strstr(txt, "\"title\":");
    if (ti) {
        const char *q = ti + 8;
        int tl = 0;
        char *s = parse_string(&q, &tl);
        if (s) { snprintf(rf->title, sizeof rf->title, "%s", s); free(s); }
    }

    const char *e = strstr(txt, "\"events\":");
    if (!e) { free(txt); return rf; }
    const char *p = strchr(e, '[');
    if (!p) { free(txt); return rf; }
    p++; // past '['

    int cap = 0;
    for (;;) {
        p = skip_ws(p);
        if (*p == ']' || *p == 0) break;
        if (*p != '[') { p++; continue; }
        p++; // into the tuple
        p = skip_ws(p);
        double t = strtod(p, (char **)&p);
        p = skip_ws(p);
        int tyl = 0;
        char *ty = parse_string(&p, &tyl);
        p = skip_ws(p);
        int dl = 0;
        char *data = parse_string(&p, &dl);
        p = skip_ws(p);
        if (*p == ']') p++;

        if (ty && data) {
            if (rf->nev == cap) {
                cap = cap ? cap * 2 : 64;
                rf->ev = realloc(rf->ev, sizeof(RecEvent) * (size_t)cap);
            }
            RecEvent *ev = &rf->ev[rf->nev++];
            ev->t = t;
            ev->type = ty[0];
            ev->data = NULL; ev->len = 0; ev->cols = ev->rows = 0;
            if (ty[0] == 'o') {
                ev->data = data; data = NULL; ev->len = dl;
            } else if (ty[0] == 'r') {
                sscanf(data, "%d %d", &ev->cols, &ev->rows);
            }
        }
        free(ty);
        free(data);
    }
    free(txt);
    return rf;
}

void rec_free(RecFile *f) {
    if (!f) return;
    for (int i = 0; i < f->nev; i++) free(f->ev[i].data);
    free(f->ev);
    free(f);
}

bool rec_newest(const char *dir, char *out, size_t outsz) {
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *de;
    time_t best = 0;
    bool found = false;
    while ((de = readdir(d))) {
        size_t nl = strlen(de->d_name);
        if (nl < 6 || strcmp(de->d_name + nl - 5, ".json") != 0) continue;
        char full[1200];
        snprintf(full, sizeof full, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (!found || st.st_mtime > best) {
            best = st.st_mtime;
            snprintf(out, outsz, "%s", full);
            found = true;
        }
    }
    closedir(d);
    return found;
}
