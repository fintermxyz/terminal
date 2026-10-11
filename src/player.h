// player.h — music player tab: playback (mac_audio.m) + drawing (player.c)
#ifndef PLAYER_H
#define PLAYER_H

#include "terminal.h"

typedef struct Player Player;

// starts playing immediately; NULL when the file can't be decoded
Player *player_open(const char *path);
void player_free(Player *p);
void player_toggle(Player *p);
void player_seek(Player *p, double seconds);
double player_pos(Player *p);
double player_dur(Player *p);
bool player_playing(Player *p);
const char *player_title(Player *p);
const char *player_artist(Player *p);
const char *player_album(Player *p);
const char *player_name(Player *p);

// space play/pause, left/right seek 5s, click the bar to seek
void player_key(Player *p, int glfw_key);
void player_click(Player *p, float x, float y); // fb px
void player_draw(Player *p, Renderer *r, const Config *cfg, int x, int y,
                 int w, int h, float scale);

#endif
