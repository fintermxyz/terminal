// editor.h — built-in text editor, shown as a tab
#ifndef EDITOR_H
#define EDITOR_H

#include "terminal.h"

typedef struct Editor Editor;

enum { ED_NONE, ED_CLOSE, ED_RUN };

// NULL (with a reason in err) for unreadable or binary files
Editor *editor_open(const char *path, char *err, size_t errcap);
void editor_free(Editor *e);

const char *editor_path(Editor *e);
const char *editor_name(Editor *e);
bool editor_modified(Editor *e);
bool editor_save(Editor *e);

// shell command that runs this file, or false when it isn't a script
bool editor_run_cmd(Editor *e, char *out, size_t cap);

// input; key handlers return an ED_* action for the host
void editor_char(Editor *e, uint32_t cp);
int  editor_key(Editor *e, int glfw_key, int glfw_mods);
int  editor_mouse(Editor *e, bool press, bool drag, float x, float y); // fb px
void editor_scroll(Editor *e, double dy);

// draws into the rect (fb px); notch_x/notch_h keep the header text clear of
// the window buttons
void editor_draw(Editor *e, Renderer *r, const Config *cfg, int x, int y,
                 int w, int h, int notch_x, int notch_h, float scale);

#endif
