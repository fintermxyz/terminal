// terminal — a blazing fast, GPU-accelerated terminal emulator with LuaJIT modules
#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define TERMINAL_VERSION "0.1.0"

// ---------------------------------------------------------------- cell/grid

enum {
    ATTR_BOLD      = 1 << 0,
    ATTR_DIM       = 1 << 1,
    ATTR_ITALIC    = 1 << 2,
    ATTR_UNDERLINE = 1 << 3,
    ATTR_REVERSE   = 1 << 4,
    ATTR_STRIKE    = 1 << 5,
    ATTR_WIDE      = 1 << 6, // first cell of a double-width glyph
    ATTR_WIDEDUMMY = 1 << 7, // second cell of a double-width glyph
};

typedef struct {
    uint32_t cp;    // unicode codepoint (0 = empty)
    uint32_t fg;    // 0xRRGGBB
    uint32_t bg;    // 0xRRGGBB
    uint16_t attrs;
    uint16_t _pad;
} Cell;

// ---------------------------------------------------------------- config

typedef struct {
    char     font_path[512];
    double   font_size;
    int      cols, rows;
    int      padding;
    int      scrollback;
    char     shell[256];
    char     cursor_style[16]; // "block" | "bar" | "underline"
    uint32_t fg, bg, cursor_color;
    uint32_t palette[16];
} Config;

// ---------------------------------------------------------------- terminal

typedef enum {
    P_GROUND, P_ESC, P_CSI, P_OSC, P_DCS,
    P_ESC_CHARSET0, P_ESC_CHARSET1, P_ESC_HASH
} ParserState;

typedef struct Term Term;

typedef struct {
    void (*response)(Term *t, const char *buf, int len); // parser -> pty replies
    void (*bell)(Term *t);
    void (*title)(Term *t, const char *title);
    void *userdata;
} TermCallbacks;

struct Term {
    int cols, rows;

    Cell *main_grid;
    Cell *alt_grid;
    Cell *grid; // points at whichever is active
    bool  alt_active;

    // cursor + current SGR brush
    int      cx, cy;
    int      saved_cx, saved_cy;      // main screen save
    int      saved_alt_cx, saved_alt_cy;
    uint32_t fg, bg;
    int      fg_idx;                  // palette index of fg, -1 if rgb/default
    uint16_t attrs;

    int  scroll_top, scroll_bot;      // inclusive
    bool cursor_visible;
    bool app_cursor;                  // DECCKM
    bool app_keypad;
    bool bracketed_paste;
    bool autowrap;
    bool wrap_pending;
    bool insert_mode;
    int  mouse_mode;                  // 0, 1000, 1002, 1003
    bool mouse_sgr;

    // charset (DEC special graphics for line drawing)
    bool charset_graphics[2];
    int  active_charset;

    // parser
    ParserState state;
    int  params[16];
    int  nparams;
    bool csi_priv;
    char osc_buf[1024];
    int  osc_len;

    // utf-8 decoder
    uint32_t utf_cp;
    int      utf_rem;

    // scrollback ring buffer (main screen only)
    Cell **sb;
    int    sb_cap, sb_len, sb_head;
    int    view_off; // how many lines scrolled back (0 = live)

    char title[512];
    bool dirty;

    const Config *cfg;
    uint32_t palette256[256];
    TermCallbacks cb;
};

Term *term_new(int cols, int rows, const Config *cfg, TermCallbacks cb);
void  term_free(Term *t);
void  term_feed(Term *t, const char *buf, size_t len);
void  term_resize(Term *t, int cols, int rows);
// y in [0, rows): row as currently visible, accounting for scrollback view
const Cell *term_visible_row(Term *t, int y);
void  term_scroll_view(Term *t, int delta_lines); // + = back in history
int   term_view_offset(Term *t);

// ---------------------------------------------------------------- pty

typedef struct {
    int   fd;
    pid_t pid;
} Pty;

bool pty_spawn(Pty *p, const char *shell, int cols, int rows);
void pty_resize(Pty *p, int cols, int rows, int pxw, int pxh);
void pty_write(Pty *p, const char *buf, size_t len);

// ---------------------------------------------------------------- renderer

typedef struct Renderer Renderer;

Renderer *render_new(const Config *cfg, float content_scale);
void render_free(Renderer *r);
bool render_set_font_size(Renderer *r, double pt); // rebuilds atlas
double render_font_size(Renderer *r);
int  render_cell_w(Renderer *r);
int  render_cell_h(Renderer *r);
float render_scale(Renderer *r);
int  render_baseline(Renderer *r);
// window-button cutout, framebuffer px: grid rows starting above h begin at x
void render_set_notch(Renderer *r, int x, int h);
int  render_row_x(Renderer *r, int ox, int oy, int y);
// draws a full frame for a single term (kept for the simple/viewer path)
void render_frame(Renderer *r, Term *t, int fb_w, int fb_h);

// compositor primitives — used to lay out tabs + split panes in one draw call.
// begin: bind viewport, clear to bg, reset the instance buffer.
void render_begin(Renderer *r, int fb_w, int fb_h, uint32_t clear_bg);
// draw a term's grid with cell (0,0) at pixel (ox,oy); cursor only if focused.
void render_term_at(Renderer *r, Term *t, int ox, int oy, bool focused);
// solid rectangle (px) in the current frame
void render_fill(Renderer *r, float x, float y, float w, float h, uint32_t col, float a);
// draw a UTF-8 string with its top-left at (x,y); returns advance width (px)
float render_text(Renderer *r, float x, float y, const char *s, uint32_t col);
// upload atlas + issue the single instanced draw call
void render_end(Renderer *r, int fb_w, int fb_h);

// ---------------------------------------------------------------- plugins

typedef struct Plugins Plugins;

// runs ~/.config/terminal/init.lua against cfg defaults (before window exists)
Plugins *plugins_new(Config *cfg);
void plugins_free(Plugins *p);
// wire runtime handles, load modules/*.lua, fire "startup"
void plugins_start(Plugins *p, Term *t, Pty *pty);
void plugins_retarget(Plugins *p, Term *t, Pty *pty);
void plugins_emit_output(Plugins *p, const char *buf, size_t len);
// returns false if a module swallowed the input; may replace via out/outlen
bool plugins_filter_input(Plugins *p, const char *buf, size_t len,
                          const char **out, size_t *outlen);
// returns true if a module consumed the key (bind or on("key"))
bool plugins_emit_key(Plugins *p, const char *keyname, int mods);
void plugins_emit_title(Plugins *p, const char *title);
void plugins_emit_resize(Plugins *p, int cols, int rows);
void plugins_emit_bell(Plugins *p);

// ---------------------------------------------------------------- app

// implemented in main.c; opens a file in the built-in viewer (images, .obj,
// .stl) or hands it to the shell's pager. Returns a VIEW_* code (viewer.h).
int app_open_file(const char *path);

// ---------------------------------------------------------------- util

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

#endif
