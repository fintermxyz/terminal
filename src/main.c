// main.c — window, event loop, tabs, input encoding, pty reader threads
#include "terminal.h"
#include "editor.h"
#include "links.h"
#include "player.h"
#include "viewer.h"

#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>

void mac_open_install(void (*cb)(const char *path)); // mac_open.m
void mac_fullsize_content(void *glfw_window);          // mac_open.m
void mac_hide_window_buttons(void *glfw_window);
void mac_titlebar_buttons(void *glfw_window, float *right, float *bottom);
void mac_defaults_menu_install(void);                  // mac_defaults.m

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <libproc.h>
#include <locale.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// ---------------------------------------------------------------- app state

#define MAX_TABS 32
#define RBUF_SIZE (1 << 20)

// a tab is one of: a shell session (term + pty + reader thread), a text
// editor, or a music player
typedef struct Tab Tab;
struct Tab {
    Term *term;
    Pty pty;
    pthread_t reader;
    Editor *ed;
    Player *pl;
    Tab *runner; // editor tabs: the shell tab its Run button reuses

    // reader thread -> main thread buffer, guarded by app.lock
    char *rbuf;
    size_t rlen;
    bool dead;
    bool typed; // something is typed on the command line (Tab completes, not links)
};

typedef struct {
    Config cfg;
    Tab *tabs[MAX_TABS];
    int ntabs, cur;
    Term *term; // the active tab's term and pty; NULL on editor/player tabs
    Pty *pty;
    int cols, rows;           // grid size shared by every shell tab
    int notch_x, notch_h;     // window-button cutout, fb px
    double quit_armed_t;
    Renderer *rend;
    Plugins *plugins;
    Viewer *viewer;
    GLFWwindow *win;
    float scale;

    pthread_mutex_t lock;
    bool needs_redraw;
} App;

static App app;

// ---------------------------------------------------------------- defaults

static void config_defaults(Config *c) {
    snprintf(c->font_path, sizeof c->font_path, "/System/Library/Fonts/Menlo.ttc");
    c->font_size = 14.0;
    c->cols = 100;
    c->rows = 30;
    c->padding = 8;
    c->scrollback = 5000;
    const char *sh = getenv("SHELL");
    snprintf(c->shell, sizeof c->shell, "%s", sh && *sh ? sh : "/bin/zsh");
    snprintf(c->cursor_style, sizeof c->cursor_style, "block");
    c->fg = 0xc0caf5;
    c->bg = 0x1a1b26;
    c->cursor_color = 0xc0caf5;
    static const uint32_t pal[16] = {
        0x15161e, 0xf7768e, 0x9ece6a, 0xe0af68, 0x7aa2f7, 0xbb9af7, 0x7dcfff, 0xa9b1d6,
        0x414868, 0xf7768e, 0x9ece6a, 0xe0af68, 0x7aa2f7, 0xbb9af7, 0x7dcfff, 0xc0caf5,
    };
    memcpy(c->palette, pal, sizeof pal);
}

// ---------------------------------------------------------------- pty thread

static void *pty_reader(void *arg) {
    Tab *tab = arg;
    char buf[65536];
    for (;;) {
        ssize_t n = read(tab->pty.fd, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            pthread_mutex_lock(&app.lock);
            tab->dead = true;
            pthread_mutex_unlock(&app.lock);
            glfwPostEmptyEvent();
            return NULL;
        }
        pthread_mutex_lock(&app.lock);
        size_t space = RBUF_SIZE - tab->rlen;
        size_t take = (size_t)n < space ? (size_t)n : space;
        memcpy(tab->rbuf + tab->rlen, buf, take);
        tab->rlen += take;
        pthread_mutex_unlock(&app.lock);
        glfwPostEmptyEvent(); // wake the render loop instantly
    }
}

static void remove_tab(int i); // fwd

// drain pending pty output of every tab into its terminal; tabs whose shell
// has exited are removed here
static void drain_pty(void) {
    static char local[RBUF_SIZE];
    for (int i = 0; i < app.ntabs; i++) {
        Tab *tab = app.tabs[i];
        if (!tab->term) continue;
        pthread_mutex_lock(&app.lock);
        size_t n = tab->rlen;
        if (n) {
            memcpy(local, tab->rbuf, n);
            tab->rlen = 0;
        }
        bool dead = tab->dead;
        pthread_mutex_unlock(&app.lock);

        if (n) {
            term_feed(tab->term, local, n);
            plugins_emit_output(app.plugins, local, n);
            app.needs_redraw = true; // active pane or a tab label may change
        }
        if (dead) remove_tab(i--);
    }
}

// ---------------------------------------------------------------- term cbs

static Tab *tab_of(Term *t) {
    for (int i = 0; i < app.ntabs; i++)
        if (app.tabs[i]->term == t) return app.tabs[i];
    return NULL;
}

static void cb_response(Term *t, const char *buf, int len) {
    Tab *tab = tab_of(t);
    if (tab) pty_write(&tab->pty, buf, (size_t)len);
}

static void cb_bell(Term *t) {
    (void)t;
    if (app.plugins) plugins_emit_bell(app.plugins);
}

static void cb_title(Term *t, const char *title) {
    if (app.win && t == app.term && !viewer_active(app.viewer))
        glfwSetWindowTitle(app.win, title);
    if (app.plugins) plugins_emit_title(app.plugins, title);
}

// ---------------------------------------------------------------- file open

static int shell_tab_index(void); // fwd
static void select_tab(int i);
static int find_tab_for(const char *path);
static bool open_editor_tab(const char *path);
static bool open_player_tab(const char *path);

// binary files nothing else understands are handed to the shell's pager
static void open_in_pager(const char *path) {
    int i = shell_tab_index();
    if (i < 0) return;
    select_tab(i);
    char cmd[4200] = " less -R -- '";
    size_t j = strlen(cmd);
    for (const char *s = path; *s && j < sizeof cmd - 8; s++) {
        if (*s == '\'') { memcpy(cmd + j, "'\\''", 4); j += 4; }
        else cmd[j++] = *s;
    }
    cmd[j++] = '\'';
    cmd[j++] = '\r';
    pty_write(app.pty, cmd, j);
}

static bool is_audio(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return false;
    char ext[8] = "";
    for (int i = 0; i < 7 && dot[1 + i]; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    char pat[12];
    snprintf(pat, sizeof pat, " %s ", ext);
    return strstr(" mp3 m4a wav aiff aif flac aac caf ", pat) != NULL;
}

// entry point for CLI args, Finder open events, and terminal.view() from Lua:
// images and 3D models open in the viewer, music in a player tab, text and
// code in an editor tab, anything else in the pager
int app_open_file(const char *path) {
    glfwFocusWindow(app.win);
    app.needs_redraw = true;
    int existing = find_tab_for(path);
    if (existing >= 0) { select_tab(existing); return VIEW_OK; }

    int r = viewer_open(app.viewer, path);
    if (r == VIEW_OK) {
        glfwSetWindowTitle(app.win, viewer_status(app.viewer));
        return r;
    }
    if (r == VIEW_FAIL) return r;
    if (is_audio(path)) return open_player_tab(path) ? VIEW_OK : VIEW_FAIL;
    if (open_editor_tab(path)) return VIEW_OK;
    open_in_pager(path);
    return VIEW_UNSUPPORTED;
}

static void update_window_title(void); // fwd

static void close_viewer(void) {
    viewer_close(app.viewer);
    update_window_title();
    app.needs_redraw = true;
}

// Finder can deliver open requests during glfwInit(), before the window and
// tabs exist; queue them until startup finishes.
#define MAX_PENDING 32
static char *pending_open[MAX_PENDING];
static int npending;

static void open_file_cb(const char *path) {
    if (app.ntabs) app_open_file(path);
    else if (npending < MAX_PENDING) pending_open[npending++] = strdup(path);
}

// ---------------------------------------------------------------- input

static void send_input(const char *buf, size_t len) {
    const char *out;
    size_t outlen;
    if (!plugins_filter_input(app.plugins, buf, len, &out, &outlen)) return;
    Tab *tab = app.tabs[app.cur];
    for (size_t i = 0; i < outlen; i++) // enter, ctrl-c, ctrl-u leave it empty
        tab->typed = !(out[i] == '\r' || out[i] == 0x03 || out[i] == 0x15);
    if (app.term->view_off) { // typing snaps back to live view
        app.term->view_off = 0;
        app.needs_redraw = true;
    }
    pty_write(app.pty, out, outlen);
}

static void char_cb(GLFWwindow *w, unsigned int cp) {
    (void)w;
    if (viewer_active(app.viewer)) return; // keys belong to the viewer
    Tab *tab = app.tabs[app.cur];
    if (tab->ed) { editor_char(tab->ed, cp); app.needs_redraw = true; return; }
    if (!tab->term) return;
    char buf[8];
    int n = 0;
    if (cp < 0x80) buf[n++] = (char)cp;
    else if (cp < 0x800) {
        buf[n++] = (char)(0xc0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        buf[n++] = (char)(0xe0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        buf[n++] = (char)(0x80 | (cp & 0x3f));
    } else {
        buf[n++] = (char)(0xf0 | (cp >> 18));
        buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3f));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3f));
        buf[n++] = (char)(0x80 | (cp & 0x3f));
    }
    send_input(buf, (size_t)n);
}

static const char *key_name(int key) {
    switch (key) {
    case GLFW_KEY_ENTER: return "enter";
    case GLFW_KEY_TAB: return "tab";
    case GLFW_KEY_BACKSPACE: return "backspace";
    case GLFW_KEY_ESCAPE: return "escape";
    case GLFW_KEY_UP: return "up";
    case GLFW_KEY_DOWN: return "down";
    case GLFW_KEY_LEFT: return "left";
    case GLFW_KEY_RIGHT: return "right";
    case GLFW_KEY_HOME: return "home";
    case GLFW_KEY_END: return "end";
    case GLFW_KEY_PAGE_UP: return "pageup";
    case GLFW_KEY_PAGE_DOWN: return "pagedown";
    case GLFW_KEY_INSERT: return "insert";
    case GLFW_KEY_DELETE: return "delete";
    case GLFW_KEY_SPACE: return "space";
    case GLFW_KEY_MINUS: return "-";
    case GLFW_KEY_EQUAL: return "=";
    default:
        if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12) {
            static const char *f[] = { "f1","f2","f3","f4","f5","f6",
                                       "f7","f8","f9","f10","f11","f12" };
            return f[key - GLFW_KEY_F1];
        }
        if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
            static char letters[26][2];
            int i = key - GLFW_KEY_A;
            letters[i][0] = (char)('a' + i);
            return letters[i];
        }
        if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9) {
            static char digits[10][2];
            int i = key - GLFW_KEY_0;
            digits[i][0] = (char)('0' + i);
            return digits[i];
        }
        return NULL;
    }
}

static void arrows(char *dst, char c, int mods) {
    // mods per xterm: 1 + (shift=1, alt=2, ctrl=4)
    int m = 1 + ((mods & GLFW_MOD_SHIFT) ? 1 : 0) +
                ((mods & GLFW_MOD_ALT) ? 2 : 0) +
                ((mods & GLFW_MOD_CONTROL) ? 4 : 0);
    if (m == 1)
        sprintf(dst, app.term->app_cursor ? "\x1bO%c" : "\x1b[%c", c);
    else
        sprintf(dst, "\x1b[1;%d%c", m, c);
}

static void do_paste(void) {
    const char *s = glfwGetClipboardString(app.win);
    if (!s) return;
    if (app.term->bracketed_paste) {
        send_input("\x1b[200~", 6);
        send_input(s, strlen(s));
        send_input("\x1b[201~", 6);
    } else {
        send_input(s, strlen(s));
    }
}

static void zoom_to(double pt); // fwd
static void relayout(void);     // fwd
static bool new_tab(void);
static void close_tab(int i);
static void remove_tab(int i);
static int tab_at(double mx, double my);
static void run_editor(Tab *tab);
static void request_quit(void);

// shortcuts that work the same on every kind of tab
static bool global_shortcut(int key, int mods) {
    if (key == GLFW_KEY_TAB && (mods & GLFW_MOD_CONTROL)) { // ctrl+tab / ctrl+shift+tab
        if (app.ntabs > 1)
            select_tab((app.cur + ((mods & GLFW_MOD_SHIFT) ? -1 : 1) + app.ntabs) % app.ntabs);
        return true;
    }
    if (!(mods & GLFW_MOD_SUPER)) return false;
    switch (key) {
    case GLFW_KEY_Q: request_quit(); return true;
    case GLFW_KEY_T: new_tab(); return true;
    case GLFW_KEY_LEFT_BRACKET:  // cmd+shift+[ / ]
    case GLFW_KEY_RIGHT_BRACKET:
        if (!(mods & GLFW_MOD_SHIFT)) return false;
        if (app.ntabs > 1)
            select_tab((app.cur + (key == GLFW_KEY_LEFT_BRACKET ? -1 : 1) + app.ntabs) % app.ntabs);
        return true;
    case GLFW_KEY_1: case GLFW_KEY_2: case GLFW_KEY_3:
    case GLFW_KEY_4: case GLFW_KEY_5: case GLFW_KEY_6:
    case GLFW_KEY_7: case GLFW_KEY_8:
        select_tab(key - GLFW_KEY_1);
        return true;
    case GLFW_KEY_9: select_tab(app.ntabs - 1); return true; // last tab
    case GLFW_KEY_EQUAL:
        zoom_to(render_font_size(app.rend) + 1);
        return true;
    case GLFW_KEY_MINUS:
        zoom_to(render_font_size(app.rend) - 1);
        return true;
    case GLFW_KEY_0:
        zoom_to(app.cfg.font_size);
        return true;
    default: return false;
    }
}

// ---------------------------------------------------------------- file links

// links show everywhere, including inside programs like Claude Code or vim;
// those keep their own keys and clicks (see link_key / mouse_btn_cb)
static bool links_live(void) {
    return app.term && !viewer_active(app.viewer);
}

// the process in the foreground of the active shell: the shell itself at a
// prompt, otherwise the program it is running
static pid_t foreground_pid(void) {
    pid_t pg = tcgetpgrp(app.pty->fd);
    return pg > 0 ? pg : app.pty->pid;
}

static bool at_shell_prompt(void) {
    return !app.term->alt_active && foreground_pid() == app.pty->pid;
}

// paths resolve against the running program's directory (Claude Code prints
// paths relative to its project), falling back to the shell's
static int links_refresh(void) {
    if (!links_live()) { links_select(-1); return 0; }
    struct proc_vnodepathinfo vpi;
    pid_t pid = foreground_pid();
    if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof vpi) <= 0) pid = app.pty->pid;
    return links_update(app.term, pid);
}

// files open in the viewer / an editor tab; directories cd the shell there
static void open_link(int i) {
    const Link *l = links_get(i);
    if (!l) return;
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s", l->path);
    bool dir = l->dir;
    links_select(-1);
    app.needs_redraw = true;
    if (!dir) { app_open_file(path); return; }
    if (app.tabs[app.cur]->typed || !at_shell_prompt()) return; // only cd at an empty prompt
    char cmd[PATH_MAX * 4 + 16] = "cd '";
    size_t j = strlen(cmd);
    for (const char *p = path; *p && j < sizeof cmd - 8; p++) {
        if (*p == '\'') { memcpy(cmd + j, "'\\''", 4); j += 4; }
        else cmd[j++] = *p;
    }
    cmd[j++] = '\'';
    cmd[j++] = '\r';
    send_input(cmd, j);
}

static bool is_modifier_key(int key) {
    return key == GLFW_KEY_LEFT_SHIFT || key == GLFW_KEY_RIGHT_SHIFT ||
           key == GLFW_KEY_LEFT_CONTROL || key == GLFW_KEY_RIGHT_CONTROL ||
           key == GLFW_KEY_LEFT_ALT || key == GLFW_KEY_RIGHT_ALT ||
           key == GLFW_KEY_LEFT_SUPER || key == GLFW_KEY_RIGHT_SUPER;
}

// Tab on an empty shell prompt, or cmd+O anywhere (inside Claude Code, vim, …),
// selects the file link nearest the bottom; then Tab / arrows move between
// links, Enter opens, Esc (or any other key) lets go. Returns true when the
// key was used.
static bool link_key(int key, int mods) {
    int n = links_refresh();
    int sel = links_selected();
    bool shift = mods & GLFW_MOD_SHIFT;
    if (sel < 0) {
        bool cmd_o = key == GLFW_KEY_O && (mods & GLFW_MOD_SUPER) && !(mods & ~GLFW_MOD_SUPER);
        bool tab = key == GLFW_KEY_TAB && !(mods & ~GLFW_MOD_SHIFT) &&
                   !app.tabs[app.cur]->typed && at_shell_prompt();
        if (!n || !(cmd_o || tab)) return false;
        links_select(shift ? 0 : n - 1);
        app.needs_redraw = true;
        return true;
    }
    if (is_modifier_key(key)) return false;
    int step = 0;
    switch (key) {
    case GLFW_KEY_TAB: step = shift ? 1 : -1; break;
    case GLFW_KEY_UP: case GLFW_KEY_LEFT: step = -1; break;
    case GLFW_KEY_DOWN: case GLFW_KEY_RIGHT: step = 1; break;
    case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: open_link(sel); return true;
    case GLFW_KEY_ESCAPE: links_select(-1); app.needs_redraw = true; return true;
    default: links_select(-1); app.needs_redraw = true; return false;
    }
    links_select((sel + step + n) % n);
    app.needs_redraw = true;
    return true;
}

static int link_hover = -1;
static GLFWcursor *hand_cursor;

static void set_link_hover(int i) {
    if (i == link_hover) return;
    link_hover = i;
    if (!hand_cursor) hand_cursor = glfwCreateStandardCursor(GLFW_HAND_CURSOR);
    glfwSetCursor(app.win, i >= 0 ? hand_cursor : NULL);
    app.needs_redraw = true;
}

// selection tint goes under the glyphs, underlines on top
static void draw_link_tint(int pad) {
    const Link *l = links_get(links_selected());
    if (!l) return;
    int cw = render_cell_w(app.rend), ch = render_cell_h(app.rend);
    float x = (float)(render_row_x(app.rend, pad, pad, l->y) + l->x0 * cw);
    render_fill(app.rend, x - 2 * app.scale, (float)(pad + l->y * ch),
                (float)((l->x1 - l->x0) * cw) + 4 * app.scale, (float)ch,
                app.cfg.palette[4], 0.35f);
}

static void draw_link_lines(int pad) {
    int cw = render_cell_w(app.rend), ch = render_cell_h(app.rend);
    int sel = links_selected();
    for (int i = 0; i < links_count(); i++) {
        const Link *l = links_get(i);
        bool hot = i == sel || i == link_hover;
        float x = (float)(render_row_x(app.rend, pad, pad, l->y) + l->x0 * cw);
        float y = (float)(pad + l->y * ch + render_baseline(app.rend)) + 2 * app.scale;
        render_fill(app.rend, x, y, (float)((l->x1 - l->x0) * cw),
                    (hot ? 2.0f : 1.0f) * app.scale, app.cfg.palette[4], hot ? 1.0f : 0.45f);
    }
}

static void key_cb(GLFWwindow *w, int key, int scancode, int action, int mods) {
    (void)w; (void)scancode;
    if (action == GLFW_RELEASE) return;

    if (viewer_active(app.viewer)) {
        if (key == GLFW_KEY_Q && (mods & GLFW_MOD_SUPER))
            request_quit();
        else if (key == GLFW_KEY_ESCAPE || key == GLFW_KEY_Q ||
                 (key == GLFW_KEY_W && (mods & GLFW_MOD_SUPER)))
            close_viewer();
        return;
    }

    if (global_shortcut(key, mods)) return;

    Tab *tab = app.tabs[app.cur];
    if (tab->ed) {
        int act = editor_key(tab->ed, key, mods);
        if (act == ED_CLOSE) remove_tab(app.cur);
        else if (act == ED_RUN) run_editor(tab);
        app.needs_redraw = true;
        return;
    }
    if (tab->pl) {
        if (key == GLFW_KEY_ESCAPE || (key == GLFW_KEY_W && (mods & GLFW_MOD_SUPER)))
            remove_tab(app.cur);
        else if (!(mods & GLFW_MOD_SUPER))
            player_key(tab->pl, key);
        app.needs_redraw = true;
        return;
    }

    if (link_key(key, mods)) return;

    const char *name = key_name(key);

    // module bindings & on("key") get first crack
    if (name) {
        int lmods = ((mods & GLFW_MOD_CONTROL) ? 1 : 0) |
                    ((mods & GLFW_MOD_ALT) ? 2 : 0) |
                    ((mods & GLFW_MOD_SHIFT) ? 4 : 0) |
                    ((mods & GLFW_MOD_SUPER) ? 8 : 0);
        if (plugins_emit_key(app.plugins, name, lmods)) return;
    }

    // built-in cmd shortcuts
    if (mods & GLFW_MOD_SUPER) {
        switch (key) {
        case GLFW_KEY_V: do_paste(); return;
        case GLFW_KEY_W: close_tab(app.cur); return;
        case GLFW_KEY_K: // clear screen + scrollback like Terminal.app
            send_input("\x0c", 1);
            return;
        default: return; // swallow other cmd combos
        }
    }

    // scrollback paging
    if ((mods & GLFW_MOD_SHIFT) &&
        (key == GLFW_KEY_PAGE_UP || key == GLFW_KEY_PAGE_DOWN)) {
        term_scroll_view(app.term,
                         key == GLFW_KEY_PAGE_UP ? app.term->rows - 1
                                                 : -(app.term->rows - 1));
        app.needs_redraw = true;
        return;
    }

    char seq[32] = "";
    switch (key) {
    case GLFW_KEY_ENTER: case GLFW_KEY_KP_ENTER: strcpy(seq, "\r"); break;
    case GLFW_KEY_BACKSPACE:
        strcpy(seq, (mods & GLFW_MOD_ALT) ? "\x1b\x7f" : "\x7f");
        break;
    case GLFW_KEY_TAB:
        strcpy(seq, (mods & GLFW_MOD_SHIFT) ? "\x1b[Z" : "\t");
        break;
    case GLFW_KEY_ESCAPE: strcpy(seq, "\x1b"); break;
    case GLFW_KEY_UP: arrows(seq, 'A', mods); break;
    case GLFW_KEY_DOWN: arrows(seq, 'B', mods); break;
    case GLFW_KEY_RIGHT: arrows(seq, 'C', mods); break;
    case GLFW_KEY_LEFT: arrows(seq, 'D', mods); break;
    case GLFW_KEY_HOME: strcpy(seq, "\x1b[H"); break;
    case GLFW_KEY_END: strcpy(seq, "\x1b[F"); break;
    case GLFW_KEY_INSERT: strcpy(seq, "\x1b[2~"); break;
    case GLFW_KEY_DELETE: strcpy(seq, "\x1b[3~"); break;
    case GLFW_KEY_PAGE_UP: strcpy(seq, "\x1b[5~"); break;
    case GLFW_KEY_PAGE_DOWN: strcpy(seq, "\x1b[6~"); break;
    case GLFW_KEY_F1: strcpy(seq, "\x1bOP"); break;
    case GLFW_KEY_F2: strcpy(seq, "\x1bOQ"); break;
    case GLFW_KEY_F3: strcpy(seq, "\x1bOR"); break;
    case GLFW_KEY_F4: strcpy(seq, "\x1bOS"); break;
    case GLFW_KEY_F5: strcpy(seq, "\x1b[15~"); break;
    case GLFW_KEY_F6: strcpy(seq, "\x1b[17~"); break;
    case GLFW_KEY_F7: strcpy(seq, "\x1b[18~"); break;
    case GLFW_KEY_F8: strcpy(seq, "\x1b[19~"); break;
    case GLFW_KEY_F9: strcpy(seq, "\x1b[20~"); break;
    case GLFW_KEY_F10: strcpy(seq, "\x1b[21~"); break;
    case GLFW_KEY_F11: strcpy(seq, "\x1b[23~"); break;
    case GLFW_KEY_F12: strcpy(seq, "\x1b[24~"); break;
    default:
        // ctrl+letter -> C0 control codes (char callback won't fire for these)
        if ((mods & GLFW_MOD_CONTROL) && !(mods & GLFW_MOD_SUPER)) {
            if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
                seq[0] = (char)(key - GLFW_KEY_A + 1);
                seq[1] = 0;
            } else if (key == GLFW_KEY_SPACE || key == GLFW_KEY_2) {
                seq[0] = 0; // NUL
                send_input(seq, 1);
                return;
            } else if (key == GLFW_KEY_LEFT_BRACKET) strcpy(seq, "\x1b");
            else if (key == GLFW_KEY_RIGHT_BRACKET) { seq[0] = 0x1d; seq[1] = 0; }
            else if (key == GLFW_KEY_BACKSLASH) { seq[0] = 0x1c; seq[1] = 0; }
            else if (key == GLFW_KEY_MINUS) { seq[0] = 0x1f; seq[1] = 0; }
        }
        break;
    }
    if (seq[0]) send_input(seq, strlen(seq));
}

// ---------------------------------------------------------------- mouse

static void report_mouse(int btn, int x, int y, bool press, bool motion) {
    Term *t = app.term;
    if (!t->mouse_mode) return;
    if (motion && t->mouse_mode < 1002) return;

    char buf[48];
    if (t->mouse_sgr) {
        int b = btn + (motion ? 32 : 0);
        snprintf(buf, sizeof buf, "\x1b[<%d;%d;%d%c", b, x + 1, y + 1,
                 press || motion ? 'M' : 'm');
    } else {
        int b = press || motion ? btn + (motion ? 32 : 0) : 3;
        if (x > 222 || y > 222) return; // legacy encoding limit
        snprintf(buf, sizeof buf, "\x1b[M%c%c%c", 32 + b, 33 + x, 33 + y);
    }
    pty_write(app.pty, buf, strlen(buf));
}

static void cell_at_cursor(double mx, double my, int *cx, int *cy) {
    int cw = render_cell_w(app.rend), ch = render_cell_h(app.rend);
    int pad = (int)(app.cfg.padding * app.scale);
    int y = (int)((my * app.scale - pad) / ch);
    if (y < 0) y = 0;
    int x = (int)((mx * app.scale - render_row_x(app.rend, pad, pad, y)) / cw);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= app.term->cols) x = app.term->cols - 1;
    if (y >= app.term->rows) y = app.term->rows - 1;
    *cx = x; *cy = y;
}

static int pressed_btn = -1;

static void mouse_btn_cb(GLFWwindow *w, int button, int action, int mods) {
    double mx, my;
    glfwGetCursorPos(w, &mx, &my);
    if (viewer_active(app.viewer)) {
        viewer_mouse_button(app.viewer, action == GLFW_PRESS, mx, my);
        app.needs_redraw = true;
        return;
    }
    int hit = tab_at(mx, my);
    if (hit >= 0 && pressed_btn < 0) {
        if (action == GLFW_PRESS) {
            if (button == GLFW_MOUSE_BUTTON_MIDDLE) close_tab(hit);
            else select_tab(hit);
        }
        return;
    }
    Tab *tab = app.tabs[app.cur];
    if (tab->ed && button == GLFW_MOUSE_BUTTON_LEFT) {
        float fx = (float)(mx * app.scale), fy = (float)(my * app.scale);
        if (editor_mouse(tab->ed, action == GLFW_PRESS, false, fx, fy) == ED_RUN) run_editor(tab);
        app.needs_redraw = true;
        return;
    }
    if (tab->pl && action == GLFW_PRESS) {
        player_click(tab->pl, (float)(mx * app.scale), (float)(my * app.scale));
        app.needs_redraw = true;
        return;
    }
    if (!tab->term) return;
    int cx, cy;
    cell_at_cursor(mx, my, &cx, &cy);

    // a click on a file link selects it, a second click (or cmd+click) opens
    // it; apps that read the mouse only give links up on cmd+click
    static bool link_click;
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_RELEASE && link_click) { link_click = false; return; }
        int li = links_refresh() ? links_at(cx, cy) : -1;
        if (action == GLFW_PRESS && li >= 0 && (!app.term->mouse_mode || (mods & GLFW_MOD_SUPER))) {
            link_click = true;
            if ((mods & GLFW_MOD_SUPER) || li == links_selected()) open_link(li);
            else links_select(li);
            app.needs_redraw = true;
            return;
        }
        if (action == GLFW_PRESS && links_selected() >= 0) {
            links_select(-1);
            app.needs_redraw = true;
        }
    }

    int b = button == GLFW_MOUSE_BUTTON_LEFT ? 0
          : button == GLFW_MOUSE_BUTTON_MIDDLE ? 1 : 2;
    if (action == GLFW_PRESS) { pressed_btn = b; report_mouse(b, cx, cy, true, false); }
    else { pressed_btn = -1; report_mouse(b, cx, cy, false, false); }
}

static void cursor_pos_cb(GLFWwindow *w, double mx, double my) {
    (void)w;
    if (viewer_active(app.viewer)) {
        viewer_mouse_move(app.viewer, mx, my);
        app.needs_redraw = true;
        return;
    }
    Tab *tab = app.tabs[app.cur];
    if (tab->ed) {
        set_link_hover(-1);
        if (glfwGetMouseButton(app.win, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
            editor_mouse(tab->ed, true, true, (float)(mx * app.scale), (float)(my * app.scale));
            app.needs_redraw = true;
        }
        return;
    }
    if (!tab->term) { set_link_hover(-1); return; }
    int hx, hy;
    cell_at_cursor(mx, my, &hx, &hy);
    set_link_hover(tab_at(mx, my) < 0 && links_refresh() ? links_at(hx, hy) : -1);
    if (pressed_btn < 0 && app.term->mouse_mode != 1003) return;
    int cx, cy;
    cell_at_cursor(mx, my, &cx, &cy);
    static int last_x = -1, last_y = -1;
    if (cx == last_x && cy == last_y) return;
    last_x = cx; last_y = cy;
    report_mouse(pressed_btn < 0 ? 3 : pressed_btn, cx, cy, false, true);
}

static void scroll_cb(GLFWwindow *w, double dx, double dy) {
    (void)w; (void)dx;
    if (dy == 0) return;
    if (viewer_active(app.viewer)) {
        viewer_scroll(app.viewer, dy);
        app.needs_redraw = true;
        return;
    }
    Tab *tab = app.tabs[app.cur];
    if (tab->ed) { editor_scroll(tab->ed, dy); app.needs_redraw = true; return; }
    if (!tab->term) return;
    Term *t = app.term;
    int lines = (int)(dy * 3);
    if (lines == 0) lines = dy > 0 ? 1 : -1;

    if (t->mouse_mode) { // report wheel to the app
        double mx, my;
        glfwGetCursorPos(app.win, &mx, &my);
        int cx, cy;
        cell_at_cursor(mx, my, &cx, &cy);
        int b = dy > 0 ? 64 : 65;
        int n = lines > 0 ? lines : -lines;
        for (int i = 0; i < n; i++) report_mouse(b, cx, cy, true, false);
    } else if (t->alt_active) { // alt screen: wheel -> arrow keys
        const char *seq = dy > 0 ? (t->app_cursor ? "\x1bOA" : "\x1b[A")
                                 : (t->app_cursor ? "\x1bOB" : "\x1b[B");
        int n = lines > 0 ? lines : -lines;
        for (int i = 0; i < n; i++) pty_write(app.pty, seq, 3);
    } else {
        term_scroll_view(t, lines);
        app.needs_redraw = true;
    }
}

// ---------------------------------------------------------------- resize

// height of the tab strip along the bottom; it only appears with 2+ tabs
static int strip_h(void) {
    return app.ntabs > 1 ? render_cell_h(app.rend) + (int)(12 * app.scale) : 0;
}

// every tab shares one grid size, so switching tabs never reflows
static void apply_grid_size(int cols, int rows) {
    bool changed = false;
    app.cols = cols;
    app.rows = rows;
    for (int i = 0; i < app.ntabs; i++) {
        Tab *tab = app.tabs[i];
        if (!tab->term) continue;
        if (cols == tab->term->cols && rows == tab->term->rows) continue;
        term_resize(tab->term, cols, rows);
        pty_resize(&tab->pty, cols, rows,
                   cols * render_cell_w(app.rend), rows * render_cell_h(app.rend));
        changed = true;
    }
    if (changed) plugins_emit_resize(app.plugins, cols, rows);
    app.needs_redraw = true;
}

// rows that sit beside the traffic lights start after them; the grid is
// narrowed so those shifted rows still fit without clipping
static int update_notch(int pad) {
    float right, bottom;
    mac_titlebar_buttons(app.win, &right, &bottom);
    int nx = right > 0 ? (int)((right + 8) * app.scale) : 0;
    app.notch_x = nx;
    app.notch_h = (int)(bottom * app.scale);
    render_set_notch(app.rend, nx, app.notch_h);
    return nx > pad ? nx : pad;
}

static void fb_size_cb(GLFWwindow *w, int fbw, int fbh) {
    (void)w;
    int pad = (int)(app.cfg.padding * app.scale);
    int left = update_notch(pad);
    int cols = (fbw - left - pad) / render_cell_w(app.rend);
    int rows = (fbh - 2 * pad - strip_h()) / render_cell_h(app.rend);
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;
    apply_grid_size(cols, rows);
    app.needs_redraw = true;
}

// cmd +/-/0: keep the window, change the font, rewrap the grid to fit.
// Bounded to ZOOM_MIN..ZOOM_MAX pt, and zooming in stops once the window
// would hold fewer than ZOOM_MIN_COLS x ZOOM_MIN_ROWS cells.
#define ZOOM_MIN 8.0
#define ZOOM_MAX 48.0
#define ZOOM_MIN_COLS 40
#define ZOOM_MIN_ROWS 10

static void zoom_to(double pt) {
    double old = render_font_size(app.rend);
    if (pt < ZOOM_MIN) pt = ZOOM_MIN;
    if (pt > ZOOM_MAX) pt = ZOOM_MAX;
    if (pt == old || !render_set_font_size(app.rend, pt)) return;
    if (pt > old) {
        int fbw, fbh;
        glfwGetFramebufferSize(app.win, &fbw, &fbh);
        int pad = (int)(app.cfg.padding * app.scale);
        int cols = (fbw - update_notch(pad) - pad) / render_cell_w(app.rend);
        int rows = (fbh - 2 * pad - strip_h()) / render_cell_h(app.rend);
        if (cols < ZOOM_MIN_COLS || rows < ZOOM_MIN_ROWS) {
            render_set_font_size(app.rend, old); // too big for this window
            return;
        }
    }
    relayout();
}

static void refresh_cb(GLFWwindow *w) {
    (void)w;
    app.needs_redraw = true;
}

// ---------------------------------------------------------------- tabs

static void relayout(void) {
    int fbw, fbh;
    glfwGetFramebufferSize(app.win, &fbw, &fbh);
    fb_size_cb(app.win, fbw, fbh);
}

static void update_window_title(void) {
    if (!app.ntabs || viewer_active(app.viewer)) return;
    Tab *tab = app.tabs[app.cur];
    const char *t = tab->ed ? editor_path(tab->ed)
                  : tab->pl ? player_title(tab->pl)
                  : tab->term->title[0] ? tab->term->title : "terminal";
    glfwSetWindowTitle(app.win, t);
}

static void select_tab(int i) {
    if (i < 0 || i >= app.ntabs) return;
    Tab *tab = app.tabs[i];
    app.cur = i;
    app.term = tab->term;
    app.pty = tab->term ? &tab->pty : NULL;
    plugins_retarget(app.plugins, app.term, app.pty);
    update_window_title();
    if (app.term) app.term->dirty = true;
    app.needs_redraw = true;
}

static void add_tab(Tab *tab) {
    app.tabs[app.ntabs++] = tab;
    select_tab(app.ntabs - 1);
    relayout(); // the strip appears with the second tab
}

// directory a new shell should start in: the active shell's, or the folder of
// the file in the active editor
static bool active_dir(char *out, size_t cap) {
    if (!app.ntabs) return false;
    Tab *tab = app.tabs[app.cur];
    if (tab->term) {
        struct proc_vnodepathinfo vpi;
        if (proc_pidinfo(tab->pty.pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof vpi) <= 0) return false;
        snprintf(out, cap, "%s", vpi.pvi_cdir.vip_path);
        return true;
    }
    const char *path = tab->ed ? editor_path(tab->ed) : NULL;
    const char *slash = path ? strrchr(path, '/') : NULL;
    if (!slash) return false;
    snprintf(out, cap, "%.*s", (int)(slash - path), path);
    return true;
}

static Tab *spawn_shell_tab(const char *dir) {
    if (app.ntabs == MAX_TABS) return NULL;
    Tab *tab = calloc(1, sizeof *tab);
    tab->rbuf = malloc(RBUF_SIZE);
    TermCallbacks cb = { .response = cb_response, .bell = cb_bell, .title = cb_title };
    tab->term = term_new(app.cols, app.rows, &app.cfg, cb);

    int here = open(".", O_RDONLY);
    if (dir) chdir(dir);
    bool ok = pty_spawn(&tab->pty, app.cfg.shell, app.cols, app.rows);
    if (here >= 0) { fchdir(here); close(here); }
    if (!ok) {
        term_free(tab->term);
        free(tab->rbuf);
        free(tab);
        return NULL;
    }
    pthread_create(&tab->reader, NULL, pty_reader, tab);
    add_tab(tab);
    return tab;
}

static bool new_tab(void) {
    char dir[PATH_MAX];
    return spawn_shell_tab(active_dir(dir, sizeof dir) ? dir : NULL) != NULL;
}

static int shell_tab_index(void) {
    if (app.ntabs && app.tabs[app.cur]->term) return app.cur;
    for (int i = 0; i < app.ntabs; i++)
        if (app.tabs[i]->term) return i;
    return new_tab() ? app.cur : -1;
}

static int find_tab_for(const char *path) {
    char want[PATH_MAX];
    if (!realpath(path, want)) return -1;
    for (int i = 0; i < app.ntabs; i++) {
        char have[PATH_MAX];
        const char *p = app.tabs[i]->ed ? editor_path(app.tabs[i]->ed) : NULL;
        if (p && realpath(p, have) && strcmp(have, want) == 0) return i;
    }
    return -1;
}

static bool open_editor_tab(const char *path) {
    if (app.ntabs == MAX_TABS) return false;
    char abs[PATH_MAX], err[256];
    Editor *ed = editor_open(realpath(path, abs) ? abs : path, err, sizeof err);
    if (!ed) {
        fprintf(stderr, "terminal: not opening %s in the editor: %s\n", path, err);
        return false;
    }
    Tab *tab = calloc(1, sizeof *tab);
    tab->ed = ed;
    add_tab(tab);
    return true;
}

static bool open_player_tab(const char *path) {
    if (app.ntabs == MAX_TABS) return false;
    Player *pl = player_open(path);
    if (!pl) {
        fprintf(stderr, "terminal: can't play %s\n", path);
        return false;
    }
    Tab *tab = calloc(1, sizeof *tab);
    tab->pl = pl;
    add_tab(tab);
    return true;
}

static bool tab_alive(Tab *t) {
    for (int i = 0; i < app.ntabs; i++)
        if (app.tabs[i] == t) return true;
    return false;
}

// save, then run the file in a shell tab: the editor's previous run tab if
// its shell is idle, otherwise a new one in the file's folder
static void run_editor(Tab *tab) {
    char cmd[8192];
    if (!editor_run_cmd(tab->ed, cmd, sizeof cmd)) return;
    if (editor_modified(tab->ed) && !editor_save(tab->ed)) return;

    Tab *run = tab->runner;
    if (!run || !tab_alive(run) || tcgetpgrp(run->pty.fd) != run->pty.pid) {
        char dir[PATH_MAX];
        const char *path = editor_path(tab->ed), *slash = strrchr(path, '/');
        snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
        Tab *ed_tab = tab;
        run = spawn_shell_tab(dir);
        if (!run) return;
        ed_tab->runner = run;
    } else {
        for (int i = 0; i < app.ntabs; i++)
            if (app.tabs[i] == run) select_tab(i);
    }
    size_t n = strlen(cmd);
    cmd[n++] = '\r';
    pty_write(&run->pty, cmd, n);
}

// ask a tab to close: shells get SIGHUP and are removed by drain_pty() once
// they exit; editors with unsaved changes ask first
static void close_tab(int i) {
    if (i < 0 || i >= app.ntabs) return;
    Tab *tab = app.tabs[i];
    if (tab->term) kill(tab->pty.pid, SIGHUP);
    else if (tab->ed && editor_modified(tab->ed)) {
        select_tab(i);
        editor_key(tab->ed, GLFW_KEY_ESCAPE, 0); // shows the unsaved-changes prompt
    } else remove_tab(i);
}

static void remove_tab(int i) {
    Tab *tab = app.tabs[i];
    if (tab->term) {
        pthread_join(tab->reader, NULL);
        close(tab->pty.fd);
        waitpid(tab->pty.pid, NULL, WNOHANG);
        term_free(tab->term);
        free(tab->rbuf);
    }
    editor_free(tab->ed);
    player_free(tab->pl);
    for (int k = 0; k < app.ntabs; k++)
        if (app.tabs[k]->runner == tab) app.tabs[k]->runner = NULL;
    free(tab);
    memmove(&app.tabs[i], &app.tabs[i + 1], sizeof(Tab *) * (size_t)(app.ntabs - i - 1));
    app.ntabs--;

    if (app.ntabs == 0) {
        app.term = NULL;
        app.pty = NULL;
        plugins_retarget(app.plugins, NULL, NULL);
        glfwSetWindowShouldClose(app.win, GLFW_TRUE);
        return;
    }
    int next = app.cur > i || app.cur == app.ntabs ? app.cur - 1 : app.cur;
    select_tab(next);
    relayout(); // the strip goes away at one tab
}

// ⌘Q and the close button: unsaved editors get one warning first
static void request_quit(void) {
    double now = glfwGetTime();
    for (int i = 0; i < app.ntabs; i++) {
        Editor *ed = app.tabs[i]->ed;
        if (!ed || !editor_modified(ed)) continue;
        if (app.quit_armed_t > 0 && now - app.quit_armed_t < 3.0) break; // second press: quit
        app.quit_armed_t = now;
        select_tab(i);
        editor_key(ed, GLFW_KEY_ESCAPE, 0); // arms and shows its prompt
        fprintf(stderr, "terminal: %s has unsaved changes; quit again to discard\n", editor_name(ed));
        glfwSetWindowShouldClose(app.win, GLFW_FALSE);
        return;
    }
    glfwSetWindowShouldClose(app.win, GLFW_TRUE);
}

static void close_cb(GLFWwindow *w) {
    (void)w;
    glfwSetWindowShouldClose(app.win, GLFW_FALSE);
    request_quit();
}

// editors: file name; players: song title; shells: the running program, or
// the folder name while the shell is idle
static void tab_label(Tab *tab, char *out, size_t cap) {
    if (tab->ed) {
        snprintf(out, cap, "%s%s", editor_name(tab->ed), editor_modified(tab->ed) ? " ●" : "");
        return;
    }
    if (tab->pl) {
        snprintf(out, cap, "♪ %s", player_title(tab->pl));
        return;
    }
    pid_t fg = tcgetpgrp(tab->pty.fd);
    if (fg > 0 && fg != tab->pty.pid && proc_name(fg, out, (uint32_t)cap) > 0) return;
    struct proc_vnodepathinfo vpi;
    if (proc_pidinfo(tab->pty.pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof vpi) > 0) {
        const char *home = getenv("HOME");
        const char *path = vpi.pvi_cdir.vip_path;
        if (home && strcmp(path, home) == 0) { snprintf(out, cap, "~"); return; }
        const char *base = strrchr(path, '/');
        snprintf(out, cap, "%s", base && base[1] ? base + 1 : path);
        return;
    }
    const char *sh = strrchr(app.cfg.shell, '/');
    snprintf(out, cap, "%s", sh ? sh + 1 : app.cfg.shell);
}

static uint32_t mix(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        float ca = (float)((a >> sh) & 255), cb = (float)((b >> sh) & 255);
        out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << sh;
    }
    return out;
}

// codepoints in a UTF-8 string; trims it to max codepoints ending in "~"
static int utf8_fit(char *s, int max) {
    int n = 0;
    for (int i = 0; s[i]; i++) {
        if ((s[i] & 0xc0) == 0x80) continue;
        if (n == max - 1 && max >= 2) {
            int rest = 0;
            for (int j = i; s[j]; j++) rest += (s[j] & 0xc0) != 0x80;
            if (rest > 1) { s[i] = '~'; s[i + 1] = 0; return max; }
        }
        if (n == max) { s[i] = 0; return n; }
        n++;
    }
    return n;
}

// equal-width tabs across the bottom edge; the active one sits flush with
// the terminal background and carries an accent line
static void draw_tabs(int fbw, int fbh) {
    int sh = strip_h();
    if (!sh) return;
    Renderer *r = app.rend;
    uint32_t bg = app.cfg.bg, fg = app.cfg.fg, accent = app.cfg.palette[4];
    int cw = render_cell_w(r), ch = render_cell_h(r);
    float s = app.scale, y0 = (float)(fbh - sh), tw = (float)fbw / app.ntabs;

    render_fill(r, 0, y0, (float)fbw, (float)sh, mix(bg, fg, 0.05f), 1.0f);
    render_fill(r, 0, y0, (float)fbw, 1.0f * s, mix(bg, fg, 0.14f), 1.0f);

    for (int i = 0; i < app.ntabs; i++) {
        float x = i * tw;
        bool active = i == app.cur;
        if (active) {
            render_fill(r, x, y0, tw, (float)sh, bg, 1.0f);
            render_fill(r, x, y0 + sh - 2.0f * s, tw, 2.0f * s, accent, 1.0f);
        } else if (i > 0 && i - 1 != app.cur) {
            render_fill(r, x, y0 + sh * 0.28f, 1.0f * s, sh * 0.44f, mix(bg, fg, 0.18f), 1.0f);
        }

        char name[256], label[300];
        tab_label(app.tabs[i], name, sizeof name);
        if (i < 9) snprintf(label, sizeof label, "%d  %s", i + 1, name); // ⌘1–9 hint
        else snprintf(label, sizeof label, "%s", name);
        int maxc = (int)(tw / cw) - 2;
        if (maxc < 1) continue;
        int len = utf8_fit(label, maxc);
        float tx = x + (tw - (float)(len * cw)) / 2;
        float ty = y0 + (float)(sh - ch) / 2;
        render_text(r, tx, ty, label, active ? fg : mix(bg, fg, 0.55f));
    }
}

// index of the tab under a window-space point, or -1 outside the strip
static int tab_at(double mx, double my) {
    int sh = strip_h();
    if (!sh) return -1;
    int fbw, fbh;
    glfwGetFramebufferSize(app.win, &fbw, &fbh);
    if (my * app.scale < fbh - sh) return -1;
    int i = (int)(mx * app.scale * app.ntabs / fbw);
    return i < 0 ? 0 : i >= app.ntabs ? app.ntabs - 1 : i;
}

// ---------------------------------------------------------------- main

int main(int argc, char **argv) {
    setlocale(LC_CTYPE, "en_US.UTF-8");
    signal(SIGPIPE, SIG_IGN);

    // CLI: `terminal <file>` — resolve before the chdir below
    char cli_file[PATH_MAX] = "";
    if (argc > 1 && argv[1][0] != '-' && !realpath(argv[1], cli_file))
        snprintf(cli_file, sizeof cli_file, "%s", argv[1]);

    config_defaults(&app.cfg);
    app.plugins = plugins_new(&app.cfg); // runs init.lua, may mutate cfg

    // first shell starts at config.working_directory (~ expanded), else $HOME
    const char *home = getenv("HOME");
    const char *wd = app.cfg.working_directory;
    char wdbuf[1024];
    if (wd[0] == '~' && home) {
        snprintf(wdbuf, sizeof wdbuf, "%s%s", home, wd + 1);
        wd = wdbuf;
    }
    if (!wd[0] || chdir(wd) != 0) {
        if (wd[0]) fprintf(stderr, "terminal: can't cd to %s, using $HOME\n", wd);
        if (home) chdir(home);
    }

    // must be installed before glfwInit(): that's where the launch-time
    // "open documents" event is dispatched
    mac_open_install(open_file_cb);

    if (!glfwInit()) {
        fprintf(stderr, "terminal: glfwInit failed\n");
        return 1;
    }
    mac_defaults_menu_install(); // glfwInit() has built the menu bar
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);

    // probe content scale from the primary monitor before sizing the window
    float sx = 1, sy = 1;
    GLFWmonitor *mon = glfwGetPrimaryMonitor();
    if (mon) glfwGetMonitorContentScale(mon, &sx, &sy);
    app.scale = sx > 0 ? sx : 1;

    // need a context before FreeType sizing? no — but renderer needs GL.
    // create a provisional window using estimated metrics, then snap to grid.
    GLFWwindow *win = glfwCreateWindow(800, 600, "terminal", NULL, NULL);
    if (!win) {
        fprintf(stderr, "terminal: window creation failed\n");
        return 1;
    }
    app.win = win;
    mac_fullsize_content(win); // terminal grid extends under the title bar
    if (app.cfg.hide_window_buttons) mac_hide_window_buttons(win);
    glfwMakeContextCurrent(win);
    glfwSwapInterval(1);

    float csx, csy;
    glfwGetWindowContentScale(win, &csx, &csy);
    if (csx > 0) app.scale = csx;

    app.rend = render_new(&app.cfg, app.scale);
    if (!app.rend) return 1;
    app.viewer = viewer_new();

    // snap window to cols x rows
    int pad = (int)(app.cfg.padding * app.scale);
    int pxw = app.cfg.cols * render_cell_w(app.rend) + pad + update_notch(pad);
    int pxh = app.cfg.rows * render_cell_h(app.rend) + 2 * pad;
    glfwSetWindowSize(win, (int)(pxw / app.scale), (int)(pxh / app.scale));

    pthread_mutex_init(&app.lock, NULL);
    app.cols = app.cfg.cols;
    app.rows = app.cfg.rows;

    // first tab; also fits the grid to the real framebuffer, title-bar strip included
    if (!spawn_shell_tab(NULL)) return 1;

    plugins_start(app.plugins, app.term, app.pty);

    // CLI: `terminal <file>` opens the viewer over the shell
    if (cli_file[0]) app_open_file(cli_file);
    for (int i = 0; i < npending; i++) {
        app_open_file(pending_open[i]);
        free(pending_open[i]);
    }

    glfwSetCharCallback(win, char_cb);
    glfwSetKeyCallback(win, key_cb);
    glfwSetScrollCallback(win, scroll_cb);
    glfwSetMouseButtonCallback(win, mouse_btn_cb);
    glfwSetCursorPosCallback(win, cursor_pos_cb);
    glfwSetFramebufferSizeCallback(win, fb_size_cb);
    glfwSetWindowRefreshCallback(win, refresh_cb);
    glfwSetWindowCloseCallback(win, close_cb);

    app.needs_redraw = true;
    double last_t = glfwGetTime();

    while (!glfwWindowShouldClose(win)) {
        bool in_viewer = viewer_active(app.viewer);
        bool want_frame = in_viewer
            ? (app.needs_redraw || viewer_animating(app.viewer))
            : (app.needs_redraw || (app.term && app.term->dirty));
        Tab *shown = app.ntabs ? app.tabs[app.cur] : NULL;
        bool ticking = !in_viewer && shown && shown->pl && player_playing(shown->pl);

        // wake instantly on pty data (posted empty event) or UI events;
        // otherwise sleep — zero CPU when idle. A playing song ticks at 30 fps.
        if (want_frame) glfwPollEvents();
        else if (ticking) { glfwWaitEventsTimeout(1.0 / 30); app.needs_redraw = true; }
        else glfwWaitEvents();
        if (!app.ntabs) break;

        drain_pty();
        if (!app.ntabs) break; // last shell exited

        double now = glfwGetTime();
        double dt = now - last_t;
        last_t = now;

        in_viewer = viewer_active(app.viewer);
        want_frame = in_viewer
            ? (app.needs_redraw || viewer_animating(app.viewer))
            : (app.needs_redraw || (app.term && app.term->dirty));

        if (want_frame) {
            int fbw, fbh;
            glfwGetFramebufferSize(win, &fbw, &fbh);
            if (in_viewer) {
                viewer_frame(app.viewer, fbw, fbh, dt, app.scale);
            } else {
                Tab *tab = app.tabs[app.cur];
                int pad = (int)(app.cfg.padding * app.scale);
                int area_h = fbh - strip_h();
                render_begin(app.rend, fbw, fbh, app.cfg.bg);
                if (tab->ed)
                    editor_draw(tab->ed, app.rend, &app.cfg, 0, 0, fbw, area_h,
                                app.notch_x, app.notch_h, app.scale);
                else if (tab->pl)
                    player_draw(tab->pl, app.rend, &app.cfg, 0, 0, fbw, area_h, app.scale);
                else {
                    links_refresh();
                    draw_link_tint(pad);
                    render_term_at(app.rend, app.term, pad, pad, true);
                    draw_link_lines(pad);
                    app.term->dirty = false;
                }
                draw_tabs(fbw, fbh);
                render_end(app.rend, fbw, fbh);
            }
            glfwSwapBuffers(win);
            app.needs_redraw = false;
        }
    }

    for (int i = 0; i < app.ntabs; i++) {
        Tab *tab = app.tabs[i];
        if (tab->term) {
            kill(tab->pty.pid, SIGHUP);
            pthread_cancel(tab->reader);
        }
        player_free(tab->pl);
    }
    plugins_free(app.plugins);
    render_free(app.rend);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
