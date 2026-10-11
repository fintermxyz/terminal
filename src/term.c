// term.c — terminal state machine: grid, scrollback, VT100/xterm parser
#include "terminal.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define ROW(t, y) ((t)->grid + (size_t)(y) * (t)->cols)

// ---------------------------------------------------------------- palette

static void build_palette(Term *t) {
    for (int i = 0; i < 16; i++) t->palette256[i] = t->cfg->palette[i];
    // 6x6x6 color cube
    static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
    for (int i = 0; i < 216; i++)
        t->palette256[16 + i] = rgb(lv[i / 36], lv[(i / 6) % 6], lv[i % 6]);
    // grayscale ramp
    for (int i = 0; i < 24; i++) {
        uint8_t v = (uint8_t)(8 + i * 10);
        t->palette256[232 + i] = rgb(v, v, v);
    }
}

// ---------------------------------------------------------------- cells

static Cell blank_cell(Term *t) {
    Cell c = { .cp = 0, .fg = t->fg, .bg = t->bg, .attrs = 0 };
    return c;
}

static void clear_region(Term *t, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= t->cols) x1 = t->cols - 1;
    if (y1 >= t->rows) y1 = t->rows - 1;
    Cell b = blank_cell(t);
    for (int y = y0; y <= y1; y++) {
        Cell *row = ROW(t, y);
        for (int x = x0; x <= x1; x++) row[x] = b;
    }
    t->dirty = true;
}

// ---------------------------------------------------------------- scrollback

static void sb_push(Term *t, const Cell *row) {
    if (t->sb_cap == 0) return;
    Cell *dst;
    if (t->sb_len < t->sb_cap) {
        dst = malloc(sizeof(Cell) * t->cols);
        t->sb[(t->sb_head + t->sb_len) % t->sb_cap] = dst;
        t->sb_len++;
    } else {
        dst = t->sb[t->sb_head];
        dst = realloc(dst, sizeof(Cell) * t->cols);
        t->sb[t->sb_head] = dst;
        t->sb_head = (t->sb_head + 1) % t->sb_cap;
    }
    memcpy(dst, row, sizeof(Cell) * t->cols);
}

// n lines back in history: 1 = most recent saved line
static const Cell *sb_line(Term *t, int n) {
    if (n < 1 || n > t->sb_len) return NULL;
    return t->sb[(t->sb_head + t->sb_len - n) % t->sb_cap];
}

// ---------------------------------------------------------------- scrolling

static void scroll_up(Term *t, int top, int n) {
    int bot = t->scroll_bot;
    if (n > bot - top + 1) n = bot - top + 1;
    if (n <= 0) return;

    if (!t->alt_active && top == 0 && t->scroll_top == 0)
        for (int i = 0; i < n; i++) sb_push(t, ROW(t, i));

    memmove(ROW(t, top), ROW(t, top + n),
            sizeof(Cell) * (size_t)(bot - top + 1 - n) * t->cols);
    clear_region(t, 0, bot - n + 1, t->cols - 1, bot);
    t->dirty = true;
}

static void scroll_down(Term *t, int top, int n) {
    int bot = t->scroll_bot;
    if (n > bot - top + 1) n = bot - top + 1;
    if (n <= 0) return;
    memmove(ROW(t, top + n), ROW(t, top),
            sizeof(Cell) * (size_t)(bot - top + 1 - n) * t->cols);
    clear_region(t, 0, top, t->cols - 1, top + n - 1);
    t->dirty = true;
}

// ---------------------------------------------------------------- cursor

static void cursor_to(Term *t, int x, int y) {
    t->cx = x < 0 ? 0 : x >= t->cols ? t->cols - 1 : x;
    t->cy = y < 0 ? 0 : y >= t->rows ? t->rows - 1 : y;
    t->wrap_pending = false;
    t->dirty = true;
}

static void linefeed(Term *t) {
    if (t->cy == t->scroll_bot) scroll_up(t, t->scroll_top, 1);
    else if (t->cy < t->rows - 1) t->cy++;
    t->dirty = true;
}

static void reverse_linefeed(Term *t) {
    if (t->cy == t->scroll_top) scroll_down(t, t->scroll_top, 1);
    else if (t->cy > 0) t->cy--;
    t->dirty = true;
}

// ---------------------------------------------------------------- reset

static void term_reset(Term *t) {
    t->fg = t->cfg->fg;
    t->bg = t->cfg->bg;
    t->fg_idx = -1;
    t->attrs = 0;
    t->cx = t->cy = 0;
    t->scroll_top = 0;
    t->scroll_bot = t->rows - 1;
    t->cursor_visible = true;
    t->app_cursor = t->app_keypad = false;
    t->bracketed_paste = false;
    t->autowrap = true;
    t->wrap_pending = false;
    t->insert_mode = false;
    t->mouse_mode = 0;
    t->mouse_sgr = false;
    t->charset_graphics[0] = t->charset_graphics[1] = false;
    t->active_charset = 0;
    t->state = P_GROUND;
    t->utf_rem = 0;
    t->alt_active = false;
    t->grid = t->main_grid;
    clear_region(t, 0, 0, t->cols - 1, t->rows - 1);
    Cell b = blank_cell(t);
    for (int i = 0; i < t->rows * t->cols; i++) t->alt_grid[i] = b;
}

// ---------------------------------------------------------------- lifecycle

Term *term_new(int cols, int rows, const Config *cfg, TermCallbacks cb) {
    Term *t = calloc(1, sizeof(Term));
    t->cols = cols;
    t->rows = rows;
    t->cfg = cfg;
    t->cb = cb;
    t->main_grid = calloc((size_t)cols * rows, sizeof(Cell));
    t->alt_grid = calloc((size_t)cols * rows, sizeof(Cell));
    t->grid = t->main_grid;
    t->sb_cap = cfg->scrollback;
    t->sb = t->sb_cap ? calloc((size_t)t->sb_cap, sizeof(Cell *)) : NULL;
    build_palette(t);
    snprintf(t->title, sizeof t->title, "terminal");
    term_reset(t);
    return t;
}

void term_free(Term *t) {
    if (!t) return;
    for (int i = 0; i < t->sb_len; i++)
        free(t->sb[(t->sb_head + i) % t->sb_cap]);
    free(t->sb);
    free(t->main_grid);
    free(t->alt_grid);
    free(t);
}

void term_resize(Term *t, int cols, int rows) {
    if (cols == t->cols && rows == t->rows) return;
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;

    Cell *nmain = calloc((size_t)cols * rows, sizeof(Cell));
    Cell *nalt = calloc((size_t)cols * rows, sizeof(Cell));
    Cell b = blank_cell(t);
    for (int i = 0; i < cols * rows; i++) nmain[i] = nalt[i] = b;

    int copy_rows = rows < t->rows ? rows : t->rows;
    int copy_cols = cols < t->cols ? cols : t->cols;
    // if shrinking vertically, drop rows from the top (push them to scrollback)
    int drop = t->rows - rows;
    if (drop < 0) drop = 0;
    if (!t->alt_active)
        for (int y = 0; y < drop && y <= t->cy; y++) sb_push(t, ROW(t, y));
    int src_off = t->cy >= rows ? t->cy - rows + 1 : 0;

    for (int y = 0; y < copy_rows; y++) {
        int sy = y + src_off;
        if (sy >= t->rows) break;
        memcpy(nmain + (size_t)y * cols,
               t->main_grid + (size_t)sy * t->cols, sizeof(Cell) * copy_cols);
        memcpy(nalt + (size_t)y * cols,
               t->alt_grid + (size_t)sy * t->cols, sizeof(Cell) * copy_cols);
    }

    free(t->main_grid);
    free(t->alt_grid);
    t->main_grid = nmain;
    t->alt_grid = nalt;
    t->grid = t->alt_active ? t->alt_grid : t->main_grid;
    t->cols = cols;
    t->rows = rows;
    t->cy -= src_off;
    if (t->cx >= cols) t->cx = cols - 1;
    if (t->cy >= rows) t->cy = rows - 1;
    if (t->cy < 0) t->cy = 0;
    t->scroll_top = 0;
    t->scroll_bot = rows - 1;
    t->view_off = 0;
    t->wrap_pending = false;
    t->dirty = true;
}

// ---------------------------------------------------------------- view

const Cell *term_visible_row(Term *t, int y) {
    if (t->view_off > 0 && !t->alt_active) {
        int from_sb = t->view_off - y; // how deep into history this row is
        if (from_sb >= 1) {
            const Cell *l = sb_line(t, from_sb);
            if (l) return l;
        }
        y = y - t->view_off;
        if (y < 0 || y >= t->rows) return NULL;
    }
    return ROW(t, y);
}

void term_scroll_view(Term *t, int delta) {
    if (t->alt_active) return;
    t->view_off += delta;
    if (t->view_off > t->sb_len) t->view_off = t->sb_len;
    if (t->view_off < 0) t->view_off = 0;
    t->dirty = true;
}

int term_view_offset(Term *t) { return t->view_off; }

// ---------------------------------------------------------------- charsets

// DEC special graphics: 0x60..0x7e -> line drawing glyphs
static const uint32_t dec_graphics[] = {
    0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0, 0x00b1,
    0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c, 0x23ba,
    0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,
    0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
};

// ---------------------------------------------------------------- put char

static void put_cp(Term *t, uint32_t cp) {
    if (t->charset_graphics[t->active_charset] && cp >= 0x60 && cp <= 0x7e)
        cp = dec_graphics[cp - 0x60];

    int w = 1;
    if (cp >= 0x80) {
        w = wcwidth((wchar_t)cp);
        if (w < 0) w = 1;
        if (w == 0) return; // combining marks: not composed (yet)
    }

    if (t->wrap_pending && t->autowrap) {
        t->wrap_pending = false;
        t->cx = 0;
        linefeed(t);
    }
    if (w == 2 && t->cx == t->cols - 1 && t->autowrap) {
        t->cx = 0;
        linefeed(t);
    }

    if (t->insert_mode) {
        Cell *row = ROW(t, t->cy);
        memmove(row + t->cx + w, row + t->cx,
                sizeof(Cell) * (size_t)(t->cols - t->cx - w));
    }

    Cell *c = ROW(t, t->cy) + t->cx;
    // overwriting half of a wide glyph: blank its partner
    if (c->attrs & ATTR_WIDEDUMMY && t->cx > 0) (c - 1)->cp = ' ';
    if (c->attrs & ATTR_WIDE && t->cx + 1 < t->cols) {
        (c + 1)->cp = ' ';
        (c + 1)->attrs &= (uint16_t)~ATTR_WIDEDUMMY;
    }

    c->cp = cp;
    c->fg = t->fg;
    c->bg = t->bg;
    c->attrs = t->attrs;
    if (w == 2) {
        c->attrs |= ATTR_WIDE;
        if (t->cx + 1 < t->cols) {
            Cell *d = c + 1;
            *d = blank_cell(t);
            d->attrs = t->attrs | ATTR_WIDEDUMMY;
        }
    }

    if (t->cx + w >= t->cols) {
        t->cx = t->cols - 1;
        t->wrap_pending = true;
    } else {
        t->cx += w;
    }
    t->dirty = true;
}

// ---------------------------------------------------------------- SGR

static void apply_sgr(Term *t) {
    if (t->nparams == 0) { t->params[t->nparams++] = 0; }
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        switch (p) {
        case 0:
            t->fg = t->cfg->fg; t->bg = t->cfg->bg;
            t->fg_idx = -1; t->attrs = 0;
            break;
        case 1: t->attrs |= ATTR_BOLD;
            if (t->fg_idx >= 0 && t->fg_idx < 8)
                t->fg = t->palette256[t->fg_idx + 8];
            break;
        case 2: t->attrs |= ATTR_DIM; break;
        case 3: t->attrs |= ATTR_ITALIC; break;
        case 4: t->attrs |= ATTR_UNDERLINE; break;
        case 7: t->attrs |= ATTR_REVERSE; break;
        case 9: t->attrs |= ATTR_STRIKE; break;
        case 21:
        case 22: t->attrs &= (uint16_t)~(ATTR_BOLD | ATTR_DIM);
            if (t->fg_idx >= 8 && t->fg_idx < 16) {} // keep explicit bright
            break;
        case 23: t->attrs &= (uint16_t)~ATTR_ITALIC; break;
        case 24: t->attrs &= (uint16_t)~ATTR_UNDERLINE; break;
        case 27: t->attrs &= (uint16_t)~ATTR_REVERSE; break;
        case 29: t->attrs &= (uint16_t)~ATTR_STRIKE; break;
        case 39: t->fg = t->cfg->fg; t->fg_idx = -1; break;
        case 49: t->bg = t->cfg->bg; break;
        case 38: case 48: {
            bool is_fg = (p == 38);
            uint32_t col = 0;
            if (i + 1 < t->nparams && t->params[i + 1] == 5 && i + 2 < t->nparams) {
                int idx = t->params[i + 2] & 255;
                col = t->palette256[idx];
                if (is_fg) t->fg_idx = idx;
                i += 2;
            } else if (i + 1 < t->nparams && t->params[i + 1] == 2 && i + 4 < t->nparams) {
                col = rgb((uint8_t)t->params[i + 2], (uint8_t)t->params[i + 3],
                          (uint8_t)t->params[i + 4]);
                if (is_fg) t->fg_idx = -1;
                i += 4;
            } else break;
            if (is_fg) t->fg = col; else t->bg = col;
            break;
        }
        default:
            if (p >= 30 && p <= 37) {
                int idx = p - 30;
                if (t->attrs & ATTR_BOLD) idx += 8;
                t->fg = t->palette256[idx];
                t->fg_idx = p - 30;
            } else if (p >= 40 && p <= 47) {
                t->bg = t->palette256[p - 40];
            } else if (p >= 90 && p <= 97) {
                t->fg = t->palette256[p - 90 + 8];
                t->fg_idx = p - 90 + 8;
            } else if (p >= 100 && p <= 107) {
                t->bg = t->palette256[p - 100 + 8];
            }
            break;
        }
    }
}

// ---------------------------------------------------------------- modes

static void enter_alt(Term *t) {
    if (t->alt_active) return;
    t->alt_active = true;
    t->grid = t->alt_grid;
    t->dirty = true;
}

static void leave_alt(Term *t) {
    if (!t->alt_active) return;
    t->alt_active = false;
    t->grid = t->main_grid;
    t->dirty = true;
}

static void set_mode(Term *t, bool priv, int mode, bool on) {
    if (priv) {
        switch (mode) {
        case 1: t->app_cursor = on; break;
        case 6: break; // DECOM origin mode: unsupported (rare)
        case 7: t->autowrap = on; break;
        case 12: break; // cursor blink
        case 25: t->cursor_visible = on; t->dirty = true; break;
        case 47:
        case 1047:
            if (on) enter_alt(t); else leave_alt(t);
            break;
        case 1048:
            if (on) { t->saved_cx = t->cx; t->saved_cy = t->cy; }
            else cursor_to(t, t->saved_cx, t->saved_cy);
            break;
        case 1049:
            if (on) {
                t->saved_cx = t->cx; t->saved_cy = t->cy;
                enter_alt(t);
                clear_region(t, 0, 0, t->cols - 1, t->rows - 1);
                cursor_to(t, 0, 0);
            } else {
                leave_alt(t);
                cursor_to(t, t->saved_cx, t->saved_cy);
            }
            break;
        case 1000: t->mouse_mode = on ? 1000 : 0; break;
        case 1002: t->mouse_mode = on ? 1002 : 0; break;
        case 1003: t->mouse_mode = on ? 1003 : 0; break;
        case 1005: break;
        case 1006: t->mouse_sgr = on; break;
        case 2004: t->bracketed_paste = on; break;
        default: break;
        }
    } else {
        if (mode == 4) t->insert_mode = on;
    }
}

// ---------------------------------------------------------------- CSI

static int param(Term *t, int i, int def) {
    if (i >= t->nparams || t->params[i] <= 0) return def;
    return t->params[i];
}

static void respond(Term *t, const char *s) {
    if (t->cb.response) t->cb.response(t, s, (int)strlen(s));
}

static void csi_dispatch(Term *t, char final) {
    int n = param(t, 0, 1);
    switch (final) {
    case 'A': cursor_to(t, t->cx, t->cy - n); break; // CUU
    case 'B': case 'e': cursor_to(t, t->cx, t->cy + n); break; // CUD
    case 'C': case 'a': cursor_to(t, t->cx + n, t->cy); break; // CUF
    case 'D': cursor_to(t, t->cx - n, t->cy); break; // CUB
    case 'E': cursor_to(t, 0, t->cy + n); break; // CNL
    case 'F': cursor_to(t, 0, t->cy - n); break; // CPL
    case 'G': case '`': cursor_to(t, n - 1, t->cy); break; // CHA
    case 'd': cursor_to(t, t->cx, n - 1); break; // VPA
    case 'H': case 'f': cursor_to(t, param(t, 1, 1) - 1, n - 1); break; // CUP
    case 'I': // CHT: forward tabs
        while (n-- > 0) cursor_to(t, ((t->cx / 8) + 1) * 8, t->cy);
        break;
    case 'Z': // CBT: backward tabs
        while (n-- > 0) cursor_to(t, t->cx == 0 ? 0 : ((t->cx - 1) / 8) * 8, t->cy);
        break;
    case 'J': { // ED
        int m = param(t, 0, 0);
        if (m == 0) {
            clear_region(t, t->cx, t->cy, t->cols - 1, t->cy);
            clear_region(t, 0, t->cy + 1, t->cols - 1, t->rows - 1);
        } else if (m == 1) {
            clear_region(t, 0, 0, t->cols - 1, t->cy - 1);
            clear_region(t, 0, t->cy, t->cx, t->cy);
        } else if (m == 2 || m == 3) {
            clear_region(t, 0, 0, t->cols - 1, t->rows - 1);
            if (m == 3) { // also wipe scrollback
                for (int i = 0; i < t->sb_len; i++)
                    free(t->sb[(t->sb_head + i) % t->sb_cap]);
                t->sb_len = 0; t->sb_head = 0; t->view_off = 0;
            }
        }
        t->wrap_pending = false;
        break;
    }
    case 'K': { // EL
        int m = param(t, 0, 0);
        if (m == 0) clear_region(t, t->cx, t->cy, t->cols - 1, t->cy);
        else if (m == 1) clear_region(t, 0, t->cy, t->cx, t->cy);
        else clear_region(t, 0, t->cy, t->cols - 1, t->cy);
        t->wrap_pending = false;
        break;
    }
    case 'L': // IL
        if (t->cy >= t->scroll_top && t->cy <= t->scroll_bot)
            scroll_down(t, t->cy, n);
        break;
    case 'M': // DL
        if (t->cy >= t->scroll_top && t->cy <= t->scroll_bot)
            scroll_up(t, t->cy, n);
        break;
    case 'P': { // DCH
        Cell *row = ROW(t, t->cy);
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        memmove(row + t->cx, row + t->cx + n,
                sizeof(Cell) * (size_t)(t->cols - t->cx - n));
        clear_region(t, t->cols - n, t->cy, t->cols - 1, t->cy);
        break;
    }
    case '@': { // ICH
        Cell *row = ROW(t, t->cy);
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        memmove(row + t->cx + n, row + t->cx,
                sizeof(Cell) * (size_t)(t->cols - t->cx - n));
        clear_region(t, t->cx, t->cy, t->cx + n - 1, t->cy);
        break;
    }
    case 'X': // ECH
        clear_region(t, t->cx, t->cy, t->cx + n - 1, t->cy);
        break;
    case 'S': scroll_up(t, t->scroll_top, n); break;  // SU
    case 'T': scroll_down(t, t->scroll_top, n); break; // SD
    case 'r': { // DECSTBM
        int top = param(t, 0, 1) - 1;
        int bot = param(t, 1, t->rows) - 1;
        if (top < 0) top = 0;
        if (bot >= t->rows) bot = t->rows - 1;
        if (top < bot) {
            t->scroll_top = top;
            t->scroll_bot = bot;
            cursor_to(t, 0, 0);
        }
        break;
    }
    case 'm': apply_sgr(t); break;
    case 'h': for (int i = 0; i < t->nparams; i++) set_mode(t, t->csi_priv, t->params[i], true); break;
    case 'l': for (int i = 0; i < t->nparams; i++) set_mode(t, t->csi_priv, t->params[i], false); break;
    case 'c': // DA
        if (!t->csi_priv) respond(t, "\x1b[?62;22c");
        break;
    case 'n': { // DSR
        int m = param(t, 0, 0);
        if (m == 5) respond(t, "\x1b[0n");
        else if (m == 6) {
            char buf[32];
            snprintf(buf, sizeof buf, "\x1b[%d;%dR", t->cy + 1, t->cx + 1);
            respond(t, buf);
        }
        break;
    }
    case 's': t->saved_cx = t->cx; t->saved_cy = t->cy; break;
    case 'u': cursor_to(t, t->saved_cx, t->saved_cy); break;
    case 't': break; // window ops: ignored
    case 'q': break; // DECSCUSR cursor style: ignored (config-driven)
    default: break;
    }
}

// ---------------------------------------------------------------- OSC

static void osc_dispatch(Term *t) {
    t->osc_buf[t->osc_len] = 0;
    char *s = t->osc_buf;
    char *semi = strchr(s, ';');
    if (!semi) return;
    *semi = 0;
    int cmd = atoi(s);
    const char *arg = semi + 1;
    if (cmd == 0 || cmd == 2) {
        snprintf(t->title, sizeof t->title, "%s", arg);
        if (t->cb.title) t->cb.title(t, t->title);
    }
    // OSC 10/11 color queries, OSC 52 clipboard: not implemented
}

// ---------------------------------------------------------------- ESC

static void esc_dispatch(Term *t, char c) {
    switch (c) {
    case '7': t->saved_cx = t->cx; t->saved_cy = t->cy; break; // DECSC
    case '8': cursor_to(t, t->saved_cx, t->saved_cy); break;   // DECRC
    case 'D': linefeed(t); break;                              // IND
    case 'E': t->cx = 0; linefeed(t); break;                   // NEL
    case 'M': reverse_linefeed(t); break;                      // RI
    case 'c': term_reset(t); break;                            // RIS
    case '=': t->app_keypad = true; break;
    case '>': t->app_keypad = false; break;
    default: break;
    }
}

// ---------------------------------------------------------------- feed

static void handle_control(Term *t, uint8_t b) {
    switch (b) {
    case 0x07: if (t->cb.bell) t->cb.bell(t); break;
    case 0x08: if (t->cx > 0) t->cx--; t->wrap_pending = false; t->dirty = true; break;
    case 0x09: cursor_to(t, ((t->cx / 8) + 1) * 8, t->cy); break;
    case 0x0a: case 0x0b: case 0x0c: linefeed(t); break;
    case 0x0d: t->cx = 0; t->wrap_pending = false; t->dirty = true; break;
    case 0x0e: t->active_charset = 1; break; // SO
    case 0x0f: t->active_charset = 0; break; // SI
    default: break;
    }
}

void term_feed(Term *t, const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = (uint8_t)buf[i];

        // utf-8 continuation handling happens only in ground state
        if (t->state == P_GROUND && t->utf_rem > 0) {
            if ((b & 0xc0) == 0x80) {
                t->utf_cp = (t->utf_cp << 6) | (b & 0x3f);
                if (--t->utf_rem == 0) put_cp(t, t->utf_cp);
                continue;
            }
            t->utf_rem = 0; // malformed: drop and reprocess byte below
        }

        switch (t->state) {
        case P_GROUND:
            if (b == 0x1b) { t->state = P_ESC; }
            else if (b < 0x20 || b == 0x7f) { handle_control(t, b); }
            else if (b < 0x80) { put_cp(t, b); }
            else if ((b & 0xe0) == 0xc0) { t->utf_cp = b & 0x1f; t->utf_rem = 1; }
            else if ((b & 0xf0) == 0xe0) { t->utf_cp = b & 0x0f; t->utf_rem = 2; }
            else if ((b & 0xf8) == 0xf0) { t->utf_cp = b & 0x07; t->utf_rem = 3; }
            break;

        case P_ESC:
            switch (b) {
            case '[':
                t->state = P_CSI;
                t->nparams = 0;
                memset(t->params, 0, sizeof t->params);
                t->csi_priv = false;
                break;
            case ']': t->state = P_OSC; t->osc_len = 0; break;
            case 'P': case '^': case '_': case 'X': t->state = P_DCS; break;
            case '(': t->state = P_ESC_CHARSET0; break;
            case ')': t->state = P_ESC_CHARSET1; break;
            case '#': t->state = P_ESC_HASH; break; // DECALN etc
            default:
                t->state = P_GROUND;
                esc_dispatch(t, (char)b);
                break;
            }
            break;

        case P_ESC_CHARSET0:
        case P_ESC_CHARSET1: {
            int g = (t->state == P_ESC_CHARSET1) ? 1 : 0;
            t->charset_graphics[g] = (b == '0');
            t->state = P_GROUND;
            break;
        }

        case P_ESC_HASH:
            if (b == '8') { // DECALN: fill screen with 'E'
                for (int y = 0; y < t->rows; y++) {
                    Cell *row = ROW(t, y);
                    for (int x = 0; x < t->cols; x++) {
                        row[x] = blank_cell(t);
                        row[x].cp = 'E';
                    }
                }
                t->dirty = true;
            }
            t->state = P_GROUND;
            break;

        case P_CSI:
            if (b >= '0' && b <= '9') {
                if (t->nparams == 0) t->nparams = 1;
                if (t->nparams <= 16)
                    t->params[t->nparams - 1] = t->params[t->nparams - 1] * 10 + (b - '0');
            } else if (b == ';' || b == ':') {
                if (t->nparams == 0) t->nparams = 1;
                if (t->nparams < 16) t->nparams++;
            } else if (b == '?') {
                t->csi_priv = true;
            } else if (b == '<' || b == '=' || b == '>') {
                t->csi_priv = true; // treat as private, dispatch will mostly ignore
            } else if (b >= 0x20 && b <= 0x2f) {
                // intermediate bytes (e.g. CSI ... SP q): ignore
            } else if (b >= 0x40 && b <= 0x7e) {
                t->state = P_GROUND;
                csi_dispatch(t, (char)b);
            } else if (b == 0x1b) {
                t->state = P_ESC;
            } else if (b < 0x20) {
                handle_control(t, b);
            } else {
                t->state = P_GROUND;
            }
            break;

        case P_OSC:
            if (b == 0x07) { t->state = P_GROUND; osc_dispatch(t); }
            else if (b == 0x1b) { t->state = P_ESC; osc_dispatch(t); } // likely ST
            else if (t->osc_len < (int)sizeof(t->osc_buf) - 1) t->osc_buf[t->osc_len++] = (char)b;
            break;

        case P_DCS: // swallow until ST (ESC \) or BEL
            if (b == 0x07) t->state = P_GROUND;
            else if (b == 0x1b) t->state = P_ESC;
            break;
        }
    }
}
