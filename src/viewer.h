// viewer.h — image + 3D model viewer overlay
#ifndef VIEWER_H
#define VIEWER_H

#include <stdbool.h>

typedef struct Viewer Viewer;

enum { VIEW_OK, VIEW_FAIL, VIEW_UNSUPPORTED };

Viewer *viewer_new(void); // requires a current GL context
void viewer_free(Viewer *v);
int  viewer_open(Viewer *v, const char *path); // VIEW_* result
void viewer_close(Viewer *v);
bool viewer_active(Viewer *v);
bool viewer_animating(Viewer *v); // needs continuous redraw
const char *viewer_status(Viewer *v);

void viewer_frame(Viewer *v, int fb_w, int fb_h, double dt, float scale);
void viewer_mouse_button(Viewer *v, bool press, double x, double y);
void viewer_mouse_move(Viewer *v, double x, double y);
void viewer_scroll(Viewer *v, double dy);

#endif
