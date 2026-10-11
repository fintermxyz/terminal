// editor.c — built-in text editor: codepoint lines, selection, snapshot undo,
// light syntax colouring, and a Run button for scripts
#include "editor.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TABW 4
#define MAX_UNDO 200

// ---------------------------------------------------------------- languages

typedef struct {
    const char *name;
    const char *exts;      // " py pyw "
    const char *line_cmt;  // "#", "//", "--" or NULL
    const char *kw;        // " if else ... "
    const char *run;       // printf format; %s = quoted path. NULL: not runnable
} Lang;

static const Lang LANGS[] = {
    { "Python", " py pyw ", "#",
      " def class if elif else for while return import from as with try except finally raise "
      "pass break continue in is not and or lambda yield None True False async await global nonlocal del assert ",
      "python3 %s" },
    { "Shell", " sh bash zsh fish ", "#",
      " if then else elif fi for while do done case esac function in return local export echo exit ",
      NULL /* by extension below */ },
    { "JavaScript", " js mjs cjs jsx ", "//",
      " function const let var if else for while return import from export default class new this "
      "async await try catch finally throw typeof of in null undefined true false switch case break ",
      "node %s" },
    { "TypeScript", " ts tsx ", "//",
      " function const let var if else for while return import from export default class new this "
      "async await try catch finally throw typeof of in null undefined true false interface type enum ",
      "npx --yes tsx %s" },
    { "Ruby", " rb ", "#",
      " def end class module if elsif else unless while do return require yield nil true false self begin rescue ",
      "ruby %s" },
    { "Lua", " lua ", "--",
      " function local end if then else elseif for while do return nil true false and or not in repeat until ",
      "lua %s" },
    { "Perl", " pl pm ", "#", " my sub if else elsif for foreach while return use print ", "perl %s" },
    { "PHP", " php ", "//", " function if else foreach for while return echo class new public private ", "php %s" },
    { "R", " r ", "#", " function if else for while return TRUE FALSE NULL library ", "Rscript %s" },
    { "Go", " go ", "//",
      " func package import if else for range return var const type struct interface map chan go defer nil true false ",
      "go run %s" },
    { "Rust", " rs ", "//",
      " fn let mut if else for while loop return use mod pub struct enum impl trait match self Self true false ",
      "rustc -o /tmp/terminal-run %s && /tmp/terminal-run" },
    { "Swift", " swift ", "//",
      " func let var if else for while return import struct class enum protocol guard nil true false self ",
      "swift %s" },
    { "C", " c h ", "//",
      " int char void if else for while return struct typedef static const unsigned sizeof include define "
      "float double long short bool true false NULL switch case break enum ",
      "cc -o /tmp/terminal-run %s && /tmp/terminal-run" },
    { "C++", " cpp cc cxx hpp hh ", "//",
      " int char void if else for while return struct class typedef static const auto namespace using "
      "template public private include true false nullptr new delete ",
      "c++ -std=c++17 -o /tmp/terminal-run %s && /tmp/terminal-run" },
    { "Java", " java ", "//",
      " class public private static void int if else for while return new import package true false null ",
      "java %s" },
    { "HTML", " html htm ", NULL, NULL, "open %s" },
    { "Markdown", " md markdown ", NULL, NULL, NULL },
    { "JSON", " json ", NULL, " true false null ", NULL },
    { "YAML", " yaml yml ", "#", " true false null ", NULL },
    { "TOML", " toml ", "#", " true false ", NULL },
    { "SQL", " sql ", "--", " select from where insert into update delete create table and or not null join on ", NULL },
    { "CSS", " css scss ", "//", NULL, NULL },
};
static const Lang PLAIN = { "Plain Text", "", NULL, NULL, NULL };

static bool word_in(const char *list, const char *w) {
    if (!list) return false;
    char pat[72];
    snprintf(pat, sizeof pat, " %s ", w);
    return strstr(list, pat) != NULL;
}

static const Lang *lang_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot || strchr(dot, '/')) return &PLAIN;
    char ext[16];
    size_t i = 0;
    for (const char *p = dot + 1; *p && i < 15; p++) ext[i++] = (char)tolower((unsigned char)*p);
    ext[i] = 0;
    for (size_t k = 0; k < sizeof LANGS / sizeof LANGS[0]; k++)
        if (word_in(LANGS[k].exts, ext)) return &LANGS[k];
    return &PLAIN;
}

// ---------------------------------------------------------------- buffer

typedef struct { uint32_t *c; int len, cap; } Line;
typedef struct { Line *lines; int n, cx, cy; } Snap;

struct Editor {
    char path[4096], name[256];
    const Lang *lang;
    Line *lines;
    int n, cap;
    int cx, cy;          // cursor (codepoint index, line)
    int ax, ay;          // selection anchor
    bool sel;
    int want_vx;         // sticky visual column for up/down
    int top, left;       // scroll (line, visual column)
    bool modified, use_tabs, crlf;

    Snap undo[MAX_UNDO], redo[MAX_UNDO];
    int nundo, nredo;
    int last_kind;       // 1 insert, 2 delete; consecutive same-kind edits merge
    double last_t;

    char msg[160];
    double msg_t;
    double close_armed_t;

    // layout from the last draw, for mouse hit-testing
    float bx, by, cw, ch;        // text origin and cell size
    float run_x0, run_x1, run_y1; // run button
    int vis_rows, vis_cols;
    bool dragging;
};

static void line_reserve(Line *l, int need) {
    if (need <= l->cap) return;
    l->cap = need < 16 ? 16 : need * 2;
    l->c = realloc(l->c, sizeof(uint32_t) * (size_t)l->cap);
}

static void line_insert(Line *l, int at, const uint32_t *cps, int n) {
    line_reserve(l, l->len + n);
    memmove(l->c + at + n, l->c + at, sizeof(uint32_t) * (size_t)(l->len - at));
    memcpy(l->c + at, cps, sizeof(uint32_t) * (size_t)n);
    l->len += n;
}

static void line_erase(Line *l, int at, int n) {
    memmove(l->c + at, l->c + at + n, sizeof(uint32_t) * (size_t)(l->len - at - n));
    l->len -= n;
}

static void lines_insert(Editor *e, int at) { // new empty line at index
    if (e->n == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 64;
        e->lines = realloc(e->lines, sizeof(Line) * (size_t)e->cap);
    }
    memmove(e->lines + at + 1, e->lines + at, sizeof(Line) * (size_t)(e->n - at));
    e->lines[at] = (Line){0};
    e->n++;
}

static void lines_remove(Editor *e, int at) {
    free(e->lines[at].c);
    memmove(e->lines + at, e->lines + at + 1, sizeof(Line) * (size_t)(e->n - at - 1));
    e->n--;
}

// ---------------------------------------------------------------- utf-8

static int utf8_dec(const unsigned char *s, size_t len, uint32_t *cp) {
    unsigned c = s[0];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (n == 0 || (size_t)n > len) { *cp = 0xfffd; return 1; }
    uint32_t v = n == 1 ? c : n == 2 ? (c & 0x1f) : n == 3 ? (c & 0x0f) : (c & 0x07);
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) { *cp = 0xfffd; return 1; }
        v = (v << 6) | (s[i] & 0x3f);
    }
    *cp = v;
    return n;
}

static int utf8_enc(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xe0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[2] = (char)(0x80 | (cp & 0x3f)); return 3;
    }
    o[0] = (char)(0xf0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

// ---------------------------------------------------------------- open/save

Editor *editor_open(const char *path, char *err, size_t errcap) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errcap, "can't open %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > 32L << 20) { fclose(f); snprintf(err, errcap, "file is over 32 MB"); return NULL; }
    unsigned char *buf = malloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (memchr(buf, 0, got < 8192 ? got : 8192)) { // NUL bytes: binary
        free(buf);
        snprintf(err, errcap, "binary file");
        return NULL;
    }

    Editor *e = calloc(1, sizeof *e);
    snprintf(e->path, sizeof e->path, "%s", path);
    const char *base = strrchr(path, '/');
    snprintf(e->name, sizeof e->name, "%s", base ? base + 1 : path);
    e->lang = lang_for(path);
    lines_insert(e, 0);
    for (size_t i = 0; i < got;) {
        if (buf[i] == '\n') { lines_insert(e, e->n); i++; continue; }
        if (buf[i] == '\r' && i + 1 < got && buf[i + 1] == '\n') { e->crlf = true; i++; continue; }
        uint32_t cp;
        i += (size_t)utf8_dec(buf + i, got - i, &cp);
        Line *l = &e->lines[e->n - 1];
        line_insert(l, l->len, &cp, 1);
        if (cp == '\t' && l->len == 1) e->use_tabs = true;
    }
    free(buf);
    return e;
}

static void snap_free(Snap *s) {
    for (int i = 0; i < s->n; i++) free(s->lines[i].c);
    free(s->lines);
    s->lines = NULL;
}

void editor_free(Editor *e) {
    if (!e) return;
    for (int i = 0; i < e->n; i++) free(e->lines[i].c);
    free(e->lines);
    for (int i = 0; i < e->nundo; i++) snap_free(&e->undo[i]);
    for (int i = 0; i < e->nredo; i++) snap_free(&e->redo[i]);
    free(e);
}

const char *editor_path(Editor *e) { return e->path; }
const char *editor_name(Editor *e) { return e->name; }
bool editor_modified(Editor *e) { return e->modified; }

static void set_msg(Editor *e, const char *m) {
    snprintf(e->msg, sizeof e->msg, "%s", m);
    e->msg_t = glfwGetTime();
}

bool editor_save(Editor *e) {
    char tmp[4200];
    snprintf(tmp, sizeof tmp, "%s.terminal-save", e->path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { set_msg(e, "Couldn't save: the folder isn't writable"); return false; }
    char u[4];
    for (int y = 0; y < e->n; y++) {
        for (int x = 0; x < e->lines[y].len; x++)
            fwrite(u, 1, (size_t)utf8_enc(e->lines[y].c[x], u), f);
        if (y + 1 < e->n) fputs(e->crlf ? "\r\n" : "\n", f);
    }
    bool ok = fclose(f) == 0;
    struct stat st; // keep the original's permissions (e.g. +x on scripts)
    if (ok && stat(e->path, &st) == 0) chmod(tmp, st.st_mode & 07777);
    if (!ok || rename(tmp, e->path) != 0) {
        remove(tmp);
        set_msg(e, "Couldn't save: write failed");
        return false;
    }
    e->modified = false;
    set_msg(e, "Saved");
    return true;
}

// ---------------------------------------------------------------- undo

static void snap_take(Editor *e, Snap *s) {
    s->n = e->n;
    s->cx = e->cx;
    s->cy = e->cy;
    s->lines = malloc(sizeof(Line) * (size_t)e->n);
    for (int i = 0; i < e->n; i++) {
        Line *src = &e->lines[i];
        s->lines[i] = (Line){ malloc(sizeof(uint32_t) * (size_t)(src->len ? src->len : 1)), src->len, src->len };
        memcpy(s->lines[i].c, src->c, sizeof(uint32_t) * (size_t)src->len);
    }
}

static void snap_restore(Editor *e, Snap *s) { // takes ownership of s's lines
    for (int i = 0; i < e->n; i++) free(e->lines[i].c);
    free(e->lines);
    e->lines = s->lines;
    e->n = e->cap = s->n;
    e->cx = s->cx;
    e->cy = s->cy;
    s->lines = NULL;
    e->sel = false;
}

static void push_snap(Snap *stack, int *n, Snap s) {
    if (*n == MAX_UNDO) {
        snap_free(&stack[0]);
        memmove(stack, stack + 1, sizeof(Snap) * (MAX_UNDO - 1));
        (*n)--;
    }
    stack[(*n)++] = s;
}

// call before every edit; typing runs merge into one undo step
static void begin_edit(Editor *e, int kind) {
    double now = glfwGetTime();
    if (kind == 0 || kind != e->last_kind || now - e->last_t > 1.0) {
        Snap s;
        snap_take(e, &s);
        push_snap(e->undo, &e->nundo, s);
        for (int i = 0; i < e->nredo; i++) snap_free(&e->redo[i]);
        e->nredo = 0;
    }
    e->last_kind = kind;
    e->last_t = now;
    e->modified = true;
}

static void undo_redo(Editor *e, bool redo) {
    Snap *from = redo ? e->redo : e->undo, *to = redo ? e->undo : e->redo;
    int *nfrom = redo ? &e->nredo : &e->nundo, *nto = redo ? &e->nundo : &e->nredo;
    if (*nfrom == 0) return;
    Snap cur;
    snap_take(e, &cur);
    push_snap(to, nto, cur);
    snap_restore(e, &from[--*nfrom]);
    e->last_kind = 0;
    e->modified = true;
}

// ---------------------------------------------------------------- cursor

static int vcol(const Line *l, int idx) { // visual column of codepoint index
    int v = 0;
    for (int i = 0; i < idx && i < l->len; i++) v = l->c[i] == '\t' ? (v / TABW + 1) * TABW : v + 1;
    return v;
}

// codepoint index for a visual column; inside a tab, the nearer edge wins
static int idx_at_vcol(const Line *l, int target) {
    int v = 0;
    for (int i = 0; i < l->len; i++) {
        int nv = l->c[i] == '\t' ? (v / TABW + 1) * TABW : v + 1;
        if (target < nv) return (target - v) * 2 < nv - v ? i : i + 1;
        v = nv;
    }
    return l->len;
}

static void clamp_cursor(Editor *e) {
    if (e->cy >= e->n) e->cy = e->n - 1;
    if (e->cy < 0) e->cy = 0;
    if (e->cx > e->lines[e->cy].len) e->cx = e->lines[e->cy].len;
    if (e->cx < 0) e->cx = 0;
}

static bool is_word(uint32_t c) { return c == '_' || c > 127 || isalnum((int)c); }

static void sel_bounds(Editor *e, int *x0, int *y0, int *x1, int *y1) {
    if (e->ay < e->cy || (e->ay == e->cy && e->ax <= e->cx)) {
        *x0 = e->ax; *y0 = e->ay; *x1 = e->cx; *y1 = e->cy;
    } else {
        *x0 = e->cx; *y0 = e->cy; *x1 = e->ax; *y1 = e->ay;
    }
}

static bool has_sel(Editor *e) { return e->sel && (e->ax != e->cx || e->ay != e->cy); }

static void delete_range(Editor *e, int x0, int y0, int x1, int y1) {
    if (y0 == y1) {
        line_erase(&e->lines[y0], x0, x1 - x0);
    } else {
        Line *a = &e->lines[y0], *b = &e->lines[y1];
        a->len = x0;
        line_insert(a, a->len, b->c + x1, b->len - x1);
        for (int y = y1; y > y0; y--) lines_remove(e, y);
    }
    e->cx = x0;
    e->cy = y0;
    e->sel = false;
}

static void delete_sel(Editor *e) {
    int x0, y0, x1, y1;
    sel_bounds(e, &x0, &y0, &x1, &y1);
    delete_range(e, x0, y0, x1, y1);
}

// inserts UTF-32 text (may contain '\n') at the cursor
static void insert_text(Editor *e, const uint32_t *t, int n) {
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && t[j] != '\n') j++;
        line_insert(&e->lines[e->cy], e->cx, t + i, j - i);
        e->cx += j - i;
        if (j < n) { // newline: split
            Line *l = &e->lines[e->cy];
            lines_insert(e, e->cy + 1);
            l = &e->lines[e->cy];
            line_insert(&e->lines[e->cy + 1], 0, l->c + e->cx, l->len - e->cx);
            l->len = e->cx;
            e->cy++;
            e->cx = 0;
            j++;
        }
        i = j;
    }
}

static char *sel_utf8(Editor *e) {
    int x0, y0, x1, y1;
    sel_bounds(e, &x0, &y0, &x1, &y1);
    size_t cap = 256, len = 0;
    char *out = malloc(cap);
    for (int y = y0; y <= y1; y++) {
        Line *l = &e->lines[y];
        int a = y == y0 ? x0 : 0, b = y == y1 ? x1 : l->len;
        for (int x = a; x <= b; x++) {
            if (len + 8 > cap) out = realloc(out, cap *= 2);
            if (x < b) len += (size_t)utf8_enc(l->c[x], out + len);
            else if (y < y1) out[len++] = '\n';
        }
    }
    out[len] = 0;
    return out;
}

static void paste_utf8(Editor *e, const char *s) {
    size_t len = strlen(s);
    uint32_t *t = malloc(sizeof(uint32_t) * (len + 1));
    int n = 0;
    for (size_t i = 0; i < len;) {
        uint32_t cp;
        i += (size_t)utf8_dec((const unsigned char *)s + i, len - i, &cp);
        if (cp != '\r') t[n++] = cp;
    }
    insert_text(e, t, n);
    free(t);
}

// ---------------------------------------------------------------- input

void editor_char(Editor *e, uint32_t cp) {
    begin_edit(e, 1);
    if (has_sel(e)) delete_sel(e);
    e->sel = false;
    insert_text(e, &cp, 1);
    e->want_vx = vcol(&e->lines[e->cy], e->cx);
}

static void move_to(Editor *e, int x, int y, bool extend) {
    if (extend && !e->sel) { e->sel = true; e->ax = e->cx; e->ay = e->cy; }
    if (!extend) e->sel = false;
    e->cy = y;
    e->cx = x;
    clamp_cursor(e);
}

static void toggle_comment(Editor *e) {
    const char *tok = e->lang->line_cmt;
    if (!tok) return;
    int y0 = e->cy, y1 = e->cy;
    if (has_sel(e)) {
        int x0, x1;
        sel_bounds(e, &x0, &y0, &x1, &y1);
        if (x1 == 0 && y1 > y0) y1--;
    }
    int tl = (int)strlen(tok);
    bool all = true; // every non-blank line already commented -> uncomment
    for (int y = y0; y <= y1; y++) {
        Line *l = &e->lines[y];
        int i = 0;
        while (i < l->len && (l->c[i] == ' ' || l->c[i] == '\t')) i++;
        if (i == l->len) continue;
        bool c = l->len - i >= tl;
        for (int k = 0; c && k < tl; k++) c = l->c[i + k] == (uint32_t)tok[k];
        if (!c) { all = false; break; }
    }
    begin_edit(e, 0);
    for (int y = y0; y <= y1; y++) {
        Line *l = &e->lines[y];
        int i = 0;
        while (i < l->len && (l->c[i] == ' ' || l->c[i] == '\t')) i++;
        if (i == l->len) continue;
        if (all) {
            int n = tl + (i + tl < l->len && l->c[i + tl] == ' ' ? 1 : 0);
            line_erase(l, i, n);
            if (y == e->cy) e->cx = e->cx > i + n ? e->cx - n : (e->cx > i ? i : e->cx);
        } else {
            uint32_t buf[8];
            for (int k = 0; k < tl; k++) buf[k] = (uint32_t)tok[k];
            buf[tl] = ' ';
            line_insert(l, i, buf, tl + 1);
            if (y == e->cy && e->cx >= i) e->cx += tl + 1;
        }
    }
    clamp_cursor(e);
}

int editor_key(Editor *e, int key, int mods) {
    bool sh = mods & GLFW_MOD_SHIFT, cmd = mods & GLFW_MOD_SUPER, alt = mods & GLFW_MOD_ALT;
    Line *l = &e->lines[e->cy];

    if (cmd) {
        switch (key) {
        case GLFW_KEY_S: editor_save(e); return ED_NONE;
        case GLFW_KEY_R: return ED_RUN;
        case GLFW_KEY_W: return editor_key(e, GLFW_KEY_ESCAPE, 0) == ED_CLOSE ? ED_CLOSE : ED_NONE;
        case GLFW_KEY_Z: undo_redo(e, sh); return ED_NONE;
        case GLFW_KEY_A:
            e->sel = true; e->ax = 0; e->ay = 0;
            e->cy = e->n - 1; e->cx = e->lines[e->cy].len;
            return ED_NONE;
        case GLFW_KEY_C: case GLFW_KEY_X: {
            if (!has_sel(e)) { // whole line, like most code editors
                e->sel = true; e->ax = 0; e->ay = e->cy;
                if (e->cy + 1 < e->n) { e->cy++; e->cx = 0; } else e->cx = l->len;
            }
            char *s = sel_utf8(e);
            glfwSetClipboardString(NULL, s);
            free(s);
            if (key == GLFW_KEY_X) { begin_edit(e, 0); delete_sel(e); }
            else e->sel = false;
            return ED_NONE;
        }
        case GLFW_KEY_V: {
            const char *s = glfwGetClipboardString(NULL);
            if (!s) return ED_NONE;
            begin_edit(e, 0);
            if (has_sel(e)) delete_sel(e);
            paste_utf8(e, s);
            return ED_NONE;
        }
        case GLFW_KEY_SLASH: toggle_comment(e); return ED_NONE;
        case GLFW_KEY_LEFT: { // first non-blank, then column 0
            int i = 0;
            while (i < l->len && (l->c[i] == ' ' || l->c[i] == '\t')) i++;
            move_to(e, e->cx == i ? 0 : i, e->cy, sh);
            e->want_vx = vcol(&e->lines[e->cy], e->cx);
            return ED_NONE;
        }
        case GLFW_KEY_RIGHT: move_to(e, l->len, e->cy, sh); e->want_vx = 1 << 30; return ED_NONE;
        case GLFW_KEY_UP: move_to(e, 0, 0, sh); e->want_vx = 0; return ED_NONE;
        case GLFW_KEY_DOWN:
            move_to(e, e->lines[e->n - 1].len, e->n - 1, sh);
            e->want_vx = vcol(&e->lines[e->cy], e->cx);
            return ED_NONE;
        case GLFW_KEY_BACKSPACE:
            begin_edit(e, 0);
            if (has_sel(e)) delete_sel(e);
            else { line_erase(l, 0, e->cx); e->cx = 0; }
            return ED_NONE;
        default: return ED_NONE;
        }
    }

    switch (key) {
    case GLFW_KEY_ESCAPE: {
        if (has_sel(e)) { e->sel = false; return ED_NONE; }
        double now = glfwGetTime();
        if (!e->modified || (e->close_armed_t > 0 && now - e->close_armed_t < 3.0)) return ED_CLOSE;
        e->close_armed_t = now;
        set_msg(e, "Unsaved changes. Press ⌘S to save, or Esc again to discard them.");
        return ED_NONE;
    }
    case GLFW_KEY_LEFT: case GLFW_KEY_RIGHT: {
        int d = key == GLFW_KEY_LEFT ? -1 : 1;
        if (has_sel(e) && !sh) { // collapse to the selection's edge
            int x0, y0, x1, y1;
            sel_bounds(e, &x0, &y0, &x1, &y1);
            move_to(e, d < 0 ? x0 : x1, d < 0 ? y0 : y1, false);
        } else {
            int x = e->cx, y = e->cy;
            if (alt) { // word jump
                if (d < 0) {
                    if (x == 0 && y > 0) { y--; x = e->lines[y].len; }
                    while (x > 0 && !is_word(e->lines[y].c[x - 1])) x--;
                    while (x > 0 && is_word(e->lines[y].c[x - 1])) x--;
                } else {
                    if (x == e->lines[y].len && y + 1 < e->n) { y++; x = 0; }
                    while (x < e->lines[y].len && !is_word(e->lines[y].c[x])) x++;
                    while (x < e->lines[y].len && is_word(e->lines[y].c[x])) x++;
                }
            } else if (d < 0) {
                if (x > 0) x--; else if (y > 0) { y--; x = e->lines[y].len; }
            } else {
                if (x < e->lines[y].len) x++; else if (y + 1 < e->n) { y++; x = 0; }
            }
            move_to(e, x, y, sh);
        }
        e->want_vx = vcol(&e->lines[e->cy], e->cx);
        return ED_NONE;
    }
    case GLFW_KEY_UP: case GLFW_KEY_DOWN: case GLFW_KEY_PAGE_UP: case GLFW_KEY_PAGE_DOWN: {
        int step = key == GLFW_KEY_UP ? -1 : key == GLFW_KEY_DOWN ? 1
                 : (key == GLFW_KEY_PAGE_UP ? -1 : 1) * (e->vis_rows > 1 ? e->vis_rows - 1 : 1);
        int y = e->cy + step;
        if (y < 0) { move_to(e, 0, 0, sh); return ED_NONE; }
        if (y >= e->n) { move_to(e, e->lines[e->n - 1].len, e->n - 1, sh); return ED_NONE; }
        move_to(e, idx_at_vcol(&e->lines[y], e->want_vx), y, sh);
        return ED_NONE;
    }
    case GLFW_KEY_HOME: move_to(e, 0, e->cy, sh); e->want_vx = 0; return ED_NONE;
    case GLFW_KEY_END: move_to(e, l->len, e->cy, sh); e->want_vx = 1 << 30; return ED_NONE;

    case GLFW_KEY_BACKSPACE:
        begin_edit(e, 2);
        if (has_sel(e)) { delete_sel(e); break; }
        if (alt) { // word
            int x = e->cx;
            while (x > 0 && !is_word(l->c[x - 1])) x--;
            while (x > 0 && is_word(l->c[x - 1])) x--;
            if (x == e->cx && e->cy > 0) { delete_range(e, e->lines[e->cy - 1].len, e->cy - 1, 0, e->cy); break; }
            delete_range(e, x, e->cy, e->cx, e->cy);
            break;
        }
        if (e->cx > 0) {
            int n = 1; // in leading spaces, remove back to the previous indent stop
            bool lead = true;
            for (int i = 0; i < e->cx; i++) if (l->c[i] != ' ') lead = false;
            if (lead && !e->use_tabs && e->cx % TABW == 0) n = TABW;
            delete_range(e, e->cx - n, e->cy, e->cx, e->cy);
        } else if (e->cy > 0) {
            delete_range(e, e->lines[e->cy - 1].len, e->cy - 1, 0, e->cy);
        }
        break;
    case GLFW_KEY_DELETE:
        begin_edit(e, 2);
        if (has_sel(e)) delete_sel(e);
        else if (e->cx < l->len) delete_range(e, e->cx, e->cy, e->cx + 1, e->cy);
        else if (e->cy + 1 < e->n) delete_range(e, e->cx, e->cy, 0, e->cy + 1);
        break;
    case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: {
        begin_edit(e, 0);
        if (has_sel(e)) delete_sel(e);
        l = &e->lines[e->cy];
        uint32_t t[256];
        int n = 0, i = 0;
        t[n++] = '\n';
        while (i < l->len && i < e->cx && (l->c[i] == ' ' || l->c[i] == '\t') && n < 200) t[n++] = l->c[i++];
        int p = e->cx - 1; // open block: indent one more level
        while (p >= 0 && l->c[p] == ' ') p--;
        if (p >= 0 && (l->c[p] == ':' || l->c[p] == '{' || l->c[p] == '(' || l->c[p] == '['))
            for (int k = 0; k < (e->use_tabs ? 1 : TABW); k++) t[n++] = e->use_tabs ? '\t' : ' ';
        insert_text(e, t, n);
        break;
    }
    case GLFW_KEY_TAB:
        begin_edit(e, 0);
        if (sh) { // dedent
            int n = 0;
            while (n < l->len && n < TABW && l->c[n] == ' ') n++;
            if (n == 0 && l->len && l->c[0] == '\t') n = 1;
            line_erase(l, 0, n);
            e->cx = e->cx > n ? e->cx - n : 0;
        } else if (e->use_tabs) {
            uint32_t t = '\t';
            insert_text(e, &t, 1);
        } else {
            uint32_t sp[TABW] = { ' ', ' ', ' ', ' ' };
            insert_text(e, sp, TABW - vcol(l, e->cx) % TABW);
        }
        break;
    default: return ED_NONE;
    }
    e->sel = false;
    clamp_cursor(e);
    e->want_vx = vcol(&e->lines[e->cy], e->cx);
    return ED_NONE;
}

static void hit_cell(Editor *e, float x, float y, int *cx, int *cy) {
    int row = (int)((y - e->by) / e->ch);
    int line = e->top + (row < 0 ? 0 : row);
    if (line >= e->n) line = e->n - 1;
    int v = (int)((x - e->bx) / e->cw + 0.5f) + e->left;
    *cy = line;
    *cx = idx_at_vcol(&e->lines[line], v < 0 ? 0 : v);
}

int editor_mouse(Editor *e, bool press, bool drag, float x, float y) {
    if (press && !drag && e->run_x1 > e->run_x0 && x >= e->run_x0 && x <= e->run_x1 && y <= e->run_y1)
        return ED_RUN;
    if (press && !drag) {
        if (y < e->by) return ED_NONE;
        int cx, cy;
        hit_cell(e, x, y, &cx, &cy);
        move_to(e, cx, cy, false);
        e->sel = true; // anchor here; becomes a selection if dragged
        e->ax = e->cx; e->ay = e->cy;
        e->dragging = true;
    } else if (drag && e->dragging) {
        int cx, cy;
        hit_cell(e, x, y, &cx, &cy);
        e->cx = cx; e->cy = cy;
    } else if (!press) {
        e->dragging = false;
    }
    e->want_vx = vcol(&e->lines[e->cy], e->cx);
    return ED_NONE;
}

void editor_scroll(Editor *e, double dy) {
    int lines = (int)(dy * 3);
    if (lines == 0) lines = dy > 0 ? 1 : -1;
    e->top -= lines;
    int max = e->n - (e->vis_rows > 1 ? e->vis_rows / 2 : 1);
    if (e->top > max) e->top = max;
    if (e->top < 0) e->top = 0;
}

bool editor_run_cmd(Editor *e, char *out, size_t cap) {
    char q[4400] = "'"; // single-quoted path
    size_t j = 1;
    for (const char *s = e->path; *s && j < sizeof q - 6; s++) {
        if (*s == '\'') { memcpy(q + j, "'\\''", 4); j += 4; } else q[j++] = *s;
    }
    q[j++] = '\'';
    q[j] = 0;

    // a shebang wins: run it the way the script asks to be run
    Line *l = &e->lines[0];
    if (l->len > 2 && l->c[0] == '#' && l->c[1] == '!') {
        char interp[512];
        int n = 0;
        for (int i = 2; i < l->len && n < (int)sizeof interp - 4; i++) n += utf8_enc(l->c[i], interp + n);
        interp[n] = 0;
        snprintf(out, cap, "%s %s", interp, q);
        return true;
    }
    const char *ext = strrchr(e->path, '.');
    if (strcmp(e->lang->name, "Shell") == 0 && ext) { // shells: run with the one it's written for
        snprintf(out, cap, "%s %s", ext + 1, q);
        return true;
    }
    if (!e->lang->run) return false;
    snprintf(out, cap, e->lang->run, q);
    return true;
}

// ---------------------------------------------------------------- drawing

static uint32_t mixc(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        float ca = (float)((a >> sh) & 255), cb = (float)((b >> sh) & 255);
        out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << sh;
    }
    return out;
}

static void draw_cp(Renderer *r, float x, float y, uint32_t cp, uint32_t col) {
    char u[5];
    u[utf8_enc(cp, u)] = 0;
    render_text(r, x, y, u, col);
}

// per-codepoint colours for one line: comments, strings, numbers, keywords
static void colorize(const Editor *e, const Line *l, const Config *cfg, uint32_t *col) {
    const Lang *lg = e->lang;
    uint32_t c_kw = cfg->palette[5], c_str = cfg->palette[2], c_num = cfg->palette[3],
             c_cmt = mixc(cfg->bg, cfg->fg, 0.45f), c_fn = cfg->palette[4];
    int tl = lg->line_cmt ? (int)strlen(lg->line_cmt) : 0;
    bool md = strcmp(lg->name, "Markdown") == 0;
    for (int i = 0; i < l->len; i++) col[i] = cfg->fg;
    if (md) { // headings and list bullets
        int i = 0;
        while (i < l->len && l->c[i] == ' ') i++;
        if (i < l->len && l->c[i] == '#') for (int k = i; k < l->len; k++) col[k] = c_fn;
        else if (i + 1 < l->len && (l->c[i] == '-' || l->c[i] == '*') && l->c[i + 1] == ' ') col[i] = c_kw;
        return;
    }
    for (int i = 0; i < l->len;) {
        uint32_t c = l->c[i];
        if (tl) { // line comment
            bool m = l->len - i >= tl;
            for (int k = 0; m && k < tl; k++) m = l->c[i + k] == (uint32_t)lg->line_cmt[k];
            if (m) { for (; i < l->len; i++) col[i] = c_cmt; break; }
        }
        if (c == '/' && i + 1 < l->len && l->c[i + 1] == '*') { // block comment, this line
            int j = i + 2;
            while (j + 1 < l->len && !(l->c[j] == '*' && l->c[j + 1] == '/')) j++;
            j = j + 2 < l->len ? j + 2 : l->len;
            for (; i < j; i++) col[i] = c_cmt;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            int j = i + 1;
            while (j < l->len && l->c[j] != c) j += l->c[j] == '\\' ? 2 : 1;
            j = j < l->len ? j + 1 : l->len;
            for (; i < j; i++) col[i] = c_str;
            continue;
        }
        if (isdigit((int)c) && (i == 0 || !is_word(l->c[i - 1]))) {
            while (i < l->len && (is_word(l->c[i]) || l->c[i] == '.')) col[i++] = c_num;
            continue;
        }
        if (is_word(c)) {
            int j = i;
            char w[64];
            int n = 0;
            while (j < l->len && is_word(l->c[j])) { if (n < 63 && l->c[j] < 128) w[n++] = (char)l->c[j]; j++; }
            w[n] = 0;
            uint32_t wc = word_in(lg->kw, w) ? c_kw : (j < l->len && l->c[j] == '(') ? c_fn : cfg->fg;
            for (; i < j; i++) col[i] = wc;
            continue;
        }
        i++;
    }
}

void editor_draw(Editor *e, Renderer *r, const Config *cfg, int x, int y,
                 int w, int h, int notch_x, int notch_h, float s) {
    float cw = (float)render_cell_w(r), ch = (float)render_cell_h(r);
    uint32_t bg = cfg->bg, fg = cfg->fg, accent = cfg->palette[4];
    uint32_t dim = mixc(bg, fg, 0.42f), faint = mixc(bg, fg, 0.06f);
    float pad = 8 * s;

    // header: file name beside the window buttons, Run button on the right
    float hh = ch + 14 * s;
    if (hh < notch_h + 6 * s) hh = notch_h + 6 * s;
    render_fill(r, (float)x, (float)y, (float)w, hh, faint, 1.0f);
    render_fill(r, (float)x, y + hh - 1 * s, (float)w, 1 * s, mixc(bg, fg, 0.14f), 1.0f);
    float tx = (float)(notch_x > x + pad ? notch_x : x + pad), ty = y + (hh - ch) / 2;
    tx += render_text(r, tx, ty, e->name, fg);
    if (e->modified) { draw_cp(r, tx + cw * 0.5f, ty, 0x25cf, accent); tx += cw * 1.5f; } // ●
    render_text(r, tx + cw, ty, e->lang->name, dim);

    char cmd[8192];
    e->run_x0 = e->run_x1 = 0;
    if (editor_run_cmd(e, cmd, sizeof cmd)) {
        const char *label = "▶ Run  ⌘R"; // 10 cells
        float bw = 10 * cw + 20 * s, bh = ch + 6 * s;
        float bx = x + w - bw - pad, by = y + (hh - bh) / 2;
        render_fill(r, bx, by, bw, bh, accent, 1.0f);
        render_text(r, bx + 10 * s, by + 3 * s, label, bg);
        e->run_x0 = bx; e->run_x1 = bx + bw; e->run_y1 = by + bh;
    }

    // status line along the bottom of the editor area
    float sh = ch + 10 * s, sy = y + h - sh;
    render_fill(r, (float)x, sy, (float)w, sh, faint, 1.0f);
    char st[256];
    bool fresh = e->msg[0] && glfwGetTime() - e->msg_t < 4.0;
    if (fresh) snprintf(st, sizeof st, "%s", e->msg);
    else snprintf(st, sizeof st, "Ln %d, Col %d    %d lines%s", e->cy + 1,
                  vcol(&e->lines[e->cy], e->cx) + 1, e->n, e->crlf ? "    CRLF" : "");
    render_text(r, x + pad, sy + 5 * s, st, fresh ? fg : dim);
    const char *keys = "⌘S save   ⌘/ comment   Esc close";
    render_text(r, x + w - pad - 36 * cw, sy + 5 * s, keys, dim);

    // body
    float body_y = y + hh + 6 * s, body_h = sy - body_y - 4 * s;
    int rows = (int)(body_h / ch);
    if (rows < 1) rows = 1;
    char digits[16];
    int gw = snprintf(digits, sizeof digits, "%d", e->n);
    if (gw < 3) gw = 3;
    float gx = x + pad, text_x = gx + (gw + 2) * cw;
    int cols = (int)((x + w - pad - text_x) / cw);
    if (cols < 1) cols = 1;
    e->vis_rows = rows;
    e->vis_cols = cols;

    // keep the cursor in view
    if (e->cy < e->top) e->top = e->cy;
    if (e->cy >= e->top + rows) e->top = e->cy - rows + 1;
    int cvx = vcol(&e->lines[e->cy], e->cx);
    if (cvx < e->left) e->left = cvx;
    if (cvx >= e->left + cols) e->left = cvx - cols + 1;
    e->bx = text_x; e->by = body_y; e->cw = cw; e->ch = ch;

    int sx0, sy0, sx1, sy1;
    bool sel = has_sel(e);
    if (sel) sel_bounds(e, &sx0, &sy0, &sx1, &sy1);
    uint32_t selc = mixc(bg, accent, 0.30f);
    static uint32_t *cols_buf;
    static int cols_cap;

    for (int row = 0; row < rows; row++) {
        int ln = e->top + row;
        if (ln >= e->n) break;
        Line *l = &e->lines[ln];
        float ly = body_y + row * ch;
        if (ln == e->cy && !sel) render_fill(r, text_x - cw * 0.5f, ly, x + w - text_x, ch, faint, 1.0f);

        char num[16];
        int nl = snprintf(num, sizeof num, "%d", ln + 1);
        render_text(r, gx + (gw - nl) * cw, ly, num, ln == e->cy ? fg : mixc(bg, fg, 0.3f));

        if (sel && ln >= sy0 && ln <= sy1) {
            int a = ln == sy0 ? vcol(l, sx0) : 0;
            int b = ln == sy1 ? vcol(l, sx1) : vcol(l, l->len) + 1;
            a -= e->left; b -= e->left;
            if (a < 0) a = 0;
            if (b > cols) b = cols;
            if (b > a) render_fill(r, text_x + a * cw, ly, (b - a) * cw, ch, selc, 1.0f);
        }

        if (l->len > cols_cap) { cols_cap = l->len * 2; cols_buf = realloc(cols_buf, sizeof(uint32_t) * (size_t)cols_cap); }
        colorize(e, l, cfg, cols_buf);
        int v = 0;
        for (int i = 0; i < l->len; i++) {
            uint32_t c = l->c[i];
            int nv = c == '\t' ? (v / TABW + 1) * TABW : v + 1;
            int col = v - e->left;
            if (col >= cols) break;
            if (col >= 0 && c > ' ') draw_cp(r, text_x + col * cw, ly, c, cols_buf[i]);
            v = nv;
        }
    }

    // cursor bar
    int crow = e->cy - e->top, ccol = cvx - e->left;
    if (crow >= 0 && crow < rows && ccol >= 0 && ccol <= cols)
        render_fill(r, text_x + ccol * cw - 1 * s, body_y + crow * ch, 2 * s, ch, cfg->cursor_color, 1.0f);
}
