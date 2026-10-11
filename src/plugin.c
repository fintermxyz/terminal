// plugin.c — LuaJIT module host: config, events, keybindings
#include "terminal.h"

#include <dirent.h>
#include <limits.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>

struct Plugins {
    lua_State *L;
    Config *cfg;
    Term *term;
    Pty *pty;
    char reply_buf[8192]; // storage for input-filter replacement strings
    char dev_modules[1024]; // absolute path of ./modules at launch, "" if none
};

static Plugins *self(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "terminal_host");
    Plugins *p = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return p;
}

// ---------------------------------------------------------------- lua api

static int api_send(lua_State *L) { // terminal.send(str) -> write to pty
    Plugins *p = self(L);
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    if (p->pty) pty_write(p->pty, s, len);
    return 0;
}

static int api_write(lua_State *L) { // terminal.write(str) -> feed display
    Plugins *p = self(L);
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    if (p->term) term_feed(p->term, s, len);
    return 0;
}

static int api_log(lua_State *L) {
    fprintf(stderr, "[module] %s\n", luaL_checkstring(L, 1));
    return 0;
}

static int api_on(lua_State *L) { // terminal.on(event, fn)
    luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, "terminal_handlers");
    lua_pushvalue(L, 1);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, 1);
        lua_pushvalue(L, -2);
        lua_rawset(L, -4);
    }
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, (int)lua_objlen(L, -2) + 1);
    return 0;
}

static int api_bind(lua_State *L) { // terminal.bind("cmd+shift+k", fn)
    luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_getfield(L, LUA_REGISTRYINDEX, "terminal_binds");
    // normalize: lowercase
    const char *spec = lua_tostring(L, 1);
    char norm[128];
    size_t j = 0;
    for (size_t i = 0; spec[i] && j < sizeof norm - 1; i++)
        norm[j++] = (char)((spec[i] >= 'A' && spec[i] <= 'Z') ? spec[i] + 32 : spec[i]);
    norm[j] = 0;
    lua_pushstring(L, norm);
    lua_pushvalue(L, 2);
    lua_rawset(L, -3);
    return 0;
}

static int api_title(lua_State *L) { // terminal.set_title(str)
    Plugins *p = self(L);
    if (p->term) {
        snprintf(p->term->title, sizeof p->term->title, "%s", luaL_checkstring(L, 1));
        if (p->term->cb.title) p->term->cb.title(p->term, p->term->title);
    }
    return 0;
}

static int api_size(lua_State *L) { // terminal.size() -> cols, rows
    Plugins *p = self(L);
    lua_pushinteger(L, p->term ? p->term->cols : p->cfg->cols);
    lua_pushinteger(L, p->term ? p->term->rows : p->cfg->rows);
    return 2;
}

static int api_view(lua_State *L) { // terminal.view(path) -> bool
    Plugins *p = self(L);
    const char *path = luaL_checkstring(L, 1);
    lua_pushboolean(L, p->term && app_open_file(path) == 0);
    return 1;
}

// ---------------------------------------------------------------- config

static uint32_t parse_color(const char *s, uint32_t fallback) {
    if (!s) return fallback;
    if (s[0] == '#') s++;
    unsigned v;
    if (sscanf(s, "%6x", &v) == 1) return v;
    return fallback;
}

static void cfg_get_str(lua_State *L, const char *key, char *dst, size_t cap) {
    lua_getfield(L, -1, key);
    if (lua_isstring(L, -1)) snprintf(dst, cap, "%s", lua_tostring(L, -1));
    lua_pop(L, 1);
}

static void cfg_get_num(lua_State *L, const char *key, double *dst) {
    lua_getfield(L, -1, key);
    if (lua_isnumber(L, -1)) *dst = lua_tonumber(L, -1);
    lua_pop(L, 1);
}

static void cfg_get_int(lua_State *L, const char *key, int *dst) {
    double d = *dst;
    cfg_get_num(L, key, &d);
    *dst = (int)d;
}

static void cfg_get_bool(lua_State *L, const char *key, bool *dst) {
    lua_getfield(L, -1, key);
    if (lua_isboolean(L, -1)) *dst = lua_toboolean(L, -1);
    lua_pop(L, 1);
}

static void cfg_get_color(lua_State *L, const char *key, uint32_t *dst) {
    lua_getfield(L, -1, key);
    if (lua_isstring(L, -1)) *dst = parse_color(lua_tostring(L, -1), *dst);
    lua_pop(L, 1);
}

static void read_config_table(Plugins *p) {
    lua_State *L = p->L;
    lua_getglobal(L, "terminal");
    lua_getfield(L, -1, "config");
    if (lua_istable(L, -1)) {
        Config *c = p->cfg;
        cfg_get_str(L, "font", c->font_path, sizeof c->font_path);
        cfg_get_num(L, "font_size", &c->font_size);
        cfg_get_int(L, "cols", &c->cols);
        cfg_get_int(L, "rows", &c->rows);
        cfg_get_int(L, "padding", &c->padding);
        cfg_get_int(L, "scrollback", &c->scrollback);
        cfg_get_str(L, "shell", c->shell, sizeof c->shell);
        cfg_get_str(L, "cursor_style", c->cursor_style, sizeof c->cursor_style);
        cfg_get_str(L, "working_directory", c->working_directory, sizeof c->working_directory);
        cfg_get_bool(L, "hide_window_buttons", &c->hide_window_buttons);
        cfg_get_color(L, "foreground", &c->fg);
        cfg_get_color(L, "background", &c->bg);
        cfg_get_color(L, "cursor_color", &c->cursor_color);
        lua_getfield(L, -1, "palette");
        if (lua_istable(L, -1)) {
            for (int i = 0; i < 16; i++) {
                lua_rawgeti(L, -1, i + 1);
                if (lua_isstring(L, -1))
                    c->palette[i] = parse_color(lua_tostring(L, -1), c->palette[i]);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        // env = { NAME = "value" } — exported to every shell this app spawns
        lua_getfield(L, -1, "env");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2)) {
                if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1))
                    setenv(lua_tostring(L, -2), lua_tostring(L, -1), 1);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
}

// ---------------------------------------------------------------- loading

static void run_file(lua_State *L, const char *path) {
    if (luaL_loadfile(L, path) || lua_pcall(L, 0, 0, 0)) {
        fprintf(stderr, "terminal: lua error in %s: %s\n", path,
                lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        fprintf(stderr, "terminal: loaded %s\n", path);
    }
}

static void load_dir(lua_State *L, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 4 && strcmp(e->d_name + n - 4, ".lua") == 0) {
            char path[1024];
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            run_file(L, path);
        }
    }
    closedir(d);
}

static void config_dir(char *dst, size_t cap, const char *sub) {
    const char *home = getenv("HOME");
    snprintf(dst, cap, "%s/.config/terminal%s", home ? home : ".", sub);
}

// ---------------------------------------------------------------- events

// pushes the handler list for event onto the stack and returns its length.
// Returns 0 with NOTHING left on the stack when there are no handlers —
// callers must only pop the list when the count is > 0.
static int handlers_for(lua_State *L, const char *event) {
    lua_getfield(L, LUA_REGISTRYINDEX, "terminal_handlers");
    lua_getfield(L, -1, event);
    lua_remove(L, -2);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    int n = (int)lua_objlen(L, -1);
    if (n == 0) lua_pop(L, 1);
    return n;
}

static void report_err(lua_State *L, const char *event) {
    fprintf(stderr, "terminal: error in on(\"%s\"): %s\n", event,
            lua_tostring(L, -1));
    lua_pop(L, 1);
}

// ---------------------------------------------------------------- public

Plugins *plugins_new(Config *cfg) {
    Plugins *p = calloc(1, sizeof(Plugins));
    p->cfg = cfg;
    lua_State *L = p->L = luaL_newstate();
    luaL_openlibs(L);

    lua_pushlightuserdata(L, p);
    lua_setfield(L, LUA_REGISTRYINDEX, "terminal_host");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "terminal_handlers");
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "terminal_binds");

    // resolve now: main() changes directory before plugins_start runs
    char dev[PATH_MAX];
    if (realpath("modules", dev)) snprintf(p->dev_modules, sizeof p->dev_modules, "%s", dev);

    // build the `terminal` global
    lua_newtable(L);
    static const luaL_Reg api[] = {
        { "send", api_send }, { "write", api_write }, { "on", api_on },
        { "bind", api_bind }, { "log", api_log }, { "set_title", api_title },
        { "size", api_size }, { "view", api_view }, { NULL, NULL },
    };
    for (const luaL_Reg *f = api; f->name; f++) {
        lua_pushcfunction(L, f->func);
        lua_setfield(L, -2, f->name);
    }
    lua_pushstring(L, TERMINAL_VERSION);
    lua_setfield(L, -2, "version");
    lua_newtable(L); // terminal.config (empty; init.lua fills it)
    lua_setfield(L, -2, "config");
    lua_setglobal(L, "terminal");

    // user config: ~/.config/terminal/init.lua
    char path[1024];
    config_dir(path, sizeof path, "/init.lua");
    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        run_file(L, path);
        read_config_table(p);
    }
    return p;
}

void plugins_free(Plugins *p) {
    if (!p) return;
    lua_close(p->L);
    free(p);
}

// point terminal.send / write / size at another tab's session
void plugins_retarget(Plugins *p, Term *t, Pty *pty) {
    p->term = t;
    p->pty = pty;
}

void plugins_start(Plugins *p, Term *t, Pty *pty) {
    p->term = t;
    p->pty = pty;

    char dir[1024];
    config_dir(dir, sizeof dir, "/modules");
    load_dir(p->L, dir);
    if (p->dev_modules[0]) load_dir(p->L, p->dev_modules); // repo-local modules for development

    lua_State *L = p->L;
    int n = handlers_for(L, "startup");
    if (n == 0) return;
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i);
        if (lua_pcall(L, 0, 0, 0)) report_err(L, "startup");
    }
    lua_pop(L, 1);
}

void plugins_emit_output(Plugins *p, const char *buf, size_t len) {
    lua_State *L = p->L;
    int n = handlers_for(L, "output");
    if (n == 0) return;
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i);
        lua_pushlstring(L, buf, len);
        if (lua_pcall(L, 1, 0, 0)) report_err(L, "output");
    }
    lua_pop(L, 1);
}

bool plugins_filter_input(Plugins *p, const char *buf, size_t len,
                          const char **out, size_t *outlen) {
    lua_State *L = p->L;
    *out = buf;
    *outlen = len;
    int n = handlers_for(L, "input");
    if (n == 0) return true;
    bool pass = true;
    for (int i = 1; i <= n && pass; i++) {
        lua_rawgeti(L, -1, i);
        lua_pushlstring(L, *out, *outlen);
        if (lua_pcall(L, 1, 1, 0)) { report_err(L, "input"); continue; }
        if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
            pass = false; // swallowed
        } else if (lua_isstring(L, -1)) { // replaced
            size_t rl;
            const char *rs = lua_tolstring(L, -1, &rl);
            if (rl > sizeof p->reply_buf) rl = sizeof p->reply_buf;
            memcpy(p->reply_buf, rs, rl);
            *out = p->reply_buf;
            *outlen = rl;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return pass;
}

bool plugins_emit_key(Plugins *p, const char *keyname, int mods) {
    lua_State *L = p->L;

    // 1) explicit binds: "cmd+shift+k" style spec
    char spec[128] = "";
    if (mods & 1) strcat(spec, "ctrl+");
    if (mods & 2) strcat(spec, "alt+");
    if (mods & 4) strcat(spec, "shift+");
    if (mods & 8) strcat(spec, "cmd+");
    strncat(spec, keyname, sizeof spec - strlen(spec) - 1);

    lua_getfield(L, LUA_REGISTRYINDEX, "terminal_binds");
    lua_getfield(L, -1, spec);
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0)) report_err(L, "bind");
        lua_pop(L, 1);
        return true;
    }
    lua_pop(L, 2);

    // 2) generic on("key") handlers: return true to consume
    bool consumed = false;
    int n = handlers_for(L, "key");
    if (n == 0) return false;
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i);
        lua_pushstring(L, keyname);
        lua_pushstring(L, spec);
        if (lua_pcall(L, 2, 1, 0)) { report_err(L, "key"); continue; }
        if (lua_toboolean(L, -1)) consumed = true;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return consumed;
}

void plugins_emit_title(Plugins *p, const char *title) {
    lua_State *L = p->L;
    int n = handlers_for(L, "title");
    if (n == 0) return;
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i);
        lua_pushstring(L, title);
        if (lua_pcall(L, 1, 0, 0)) report_err(L, "title");
    }
    lua_pop(L, 1);
}

void plugins_emit_resize(Plugins *p, int cols, int rows) {
    lua_State *L = p->L;
    int n = handlers_for(L, "resize");
    if (n == 0) return;
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i);
        lua_pushinteger(L, cols);
        lua_pushinteger(L, rows);
        if (lua_pcall(L, 2, 0, 0)) report_err(L, "resize");
    }
    lua_pop(L, 1);
}

void plugins_emit_bell(Plugins *p) {
    lua_State *L = p->L;
    int n = handlers_for(L, "bell");
    if (n == 0) return;
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, -1, i);
        if (lua_pcall(L, 0, 0, 0)) report_err(L, "bell");
    }
    lua_pop(L, 1);
}
