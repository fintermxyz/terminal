// links.h — file paths in the visible grid, turned into clickable links
//
// Every token on screen that resolves to an existing file or directory
// (relative to the shell's working directory) becomes a link. Links are
// rescanned only when the visible grid or the shell's directory changes.
#ifndef LINKS_H
#define LINKS_H

#include "terminal.h"

#include <limits.h>

typedef struct {
    int  y, x0, x1;      // visible row, cell span [x0, x1)
    bool dir;
    char path[PATH_MAX]; // absolute
} Link;

// rescans if needed; returns the number of links on screen
int links_update(Term *t, pid_t shell);
int links_count(void);
const Link *links_get(int i);
int links_at(int x, int y); // index of the link covering cell (x, y), or -1

// one link can be selected; the selection follows its link across rescans
void links_select(int i); // -1 clears
int  links_selected(void);

#endif
