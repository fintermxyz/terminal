// player.c — music player tab: title block, progress bar, transport hints
#include "player.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

// progress bar rect from the last draw, for click-to-seek
static float bar_x, bar_y, bar_w, bar_h;

static uint32_t mixc(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        float ca = (float)((a >> sh) & 255), cb = (float)((b >> sh) & 255);
        out |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << sh;
    }
    return out;
}

static void fmt_time(double t, char *out, size_t cap) {
    int s = (int)(t + 0.5);
    if (s >= 3600) snprintf(out, cap, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, cap, "%d:%02d", s / 60, s % 60);
}

void player_key(Player *p, int key) {
    if (key == GLFW_KEY_SPACE || key == GLFW_KEY_K) player_toggle(p);
    else if (key == GLFW_KEY_LEFT || key == GLFW_KEY_J) player_seek(p, player_pos(p) - 5);
    else if (key == GLFW_KEY_RIGHT || key == GLFW_KEY_L) player_seek(p, player_pos(p) + 5);
    else if (key == GLFW_KEY_0 || key == GLFW_KEY_HOME) player_seek(p, 0);
}

void player_click(Player *p, float x, float y) {
    float slop = bar_h * 4; // generous vertical hit area
    if (bar_w <= 0 || y < bar_y - slop || y > bar_y + bar_h + slop) {
        player_toggle(p); // anywhere else: play/pause
        return;
    }
    float t = (x - bar_x) / bar_w;
    player_seek(p, (t < 0 ? 0 : t > 1 ? 1 : t) * player_dur(p));
}

// text centred on cx, clipped to max_cells
static void text_c(Renderer *r, float cx, float y, const char *s, uint32_t col, int max_cells) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", s);
    // count codepoints, trim to fit
    int n = 0, cut = -1;
    for (int i = 0; buf[i]; i++)
        if ((buf[i] & 0xc0) != 0x80 && ++n > max_cells && cut < 0) cut = i;
    if (cut >= 0) { buf[cut] = 0; n = max_cells; }
    render_text(r, cx - n * render_cell_w(r) / 2.0f, y, buf, col);
}

void player_draw(Player *p, Renderer *r, const Config *cfg, int x, int y,
                 int w, int h, float s) {
    float cw = (float)render_cell_w(r), ch = (float)render_cell_h(r);
    uint32_t bg = cfg->bg, fg = cfg->fg, accent = cfg->palette[4];
    uint32_t dim = mixc(bg, fg, 0.5f), track = mixc(bg, fg, 0.14f);
    float cx = x + w / 2.0f;
    int max_cells = (int)(w / cw) - 4;

    double pos = player_pos(p), dur = player_dur(p);
    bool playing = player_playing(p);

    // block centred vertically: icon, title, artist · album, bar, times, hints
    float block = ch * 9;
    float top = y + (h - block) / 2;

    // simple level bars as an icon; they move while playing
    double t = glfwGetTime();
    for (int i = 0; i < 5; i++) {
        float amp = playing ? 0.35f + 0.65f * (float)(0.5 + 0.5 * sin(t * (3.1 + i * 1.7) + i)) : 0.25f;
        float bh = ch * 1.6f * amp, bw = 4 * s;
        render_fill(r, cx + (i - 2) * 9 * s - bw / 2, top + ch * 1.6f - bh, bw, bh, accent, 1.0f);
    }

    text_c(r, cx, top + ch * 2.4f, player_title(p), fg, max_cells);
    char sub[520];
    const char *ar = player_artist(p), *al = player_album(p);
    snprintf(sub, sizeof sub, "%s%s%s", ar, ar[0] && al[0] ? "  ·  " : "", al);
    if (!sub[0]) snprintf(sub, sizeof sub, "%s", player_name(p));
    text_c(r, cx, top + ch * 3.6f, sub, dim, max_cells);

    // progress bar
    bar_w = w * 0.6f;
    if (bar_w > 640 * s) bar_w = 640 * s;
    bar_h = 4 * s;
    bar_x = cx - bar_w / 2;
    bar_y = top + ch * 5.4f;
    float frac = dur > 0 ? (float)(pos / dur) : 0;
    render_fill(r, bar_x, bar_y, bar_w, bar_h, track, 1.0f);
    render_fill(r, bar_x, bar_y, bar_w * frac, bar_h, accent, 1.0f);
    render_fill(r, bar_x + bar_w * frac - 5 * s, bar_y - 4 * s, 10 * s, bar_h + 8 * s, fg, 1.0f);

    char a[32], b[32];
    fmt_time(pos, a, sizeof a);
    fmt_time(dur, b, sizeof b);
    render_text(r, bar_x, bar_y + 10 * s, a, dim);
    render_text(r, bar_x + bar_w - strlen(b) * cw, bar_y + 10 * s, b, dim);

    text_c(r, cx, top + ch * 7.4f, playing ? "❚❚  playing" : "▶  paused", playing ? fg : dim, max_cells);
    text_c(r, cx, top + ch * 8.6f, "space play/pause   ← → seek 5s   click bar to seek",
           mixc(bg, fg, 0.35f), max_cells);
}
