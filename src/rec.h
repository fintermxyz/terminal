// rec.h — JSON state-based session recording + real-state playback
//
// A recording is a single JSON document capturing the *actual* byte stream the
// shell produced, with timestamps and resize events:
//
//   {"version":1,"width":100,"height":30,"title":"zsh","events":[
//     [0.031,"o","hello\r\n[32mok[0m"],
//     [1.204,"r","120 34"],
//     ...
//   ]}
//
// Playback re-feeds those exact bytes into a live Term at the recorded timings,
// so the grid, colors, cursor and scrollback are reconstructed for real — not
// replayed as a video. Byte-exact: every non-printable byte is escaped \u00XX
// and mapped straight back to one byte on load.
#ifndef REC_H
#define REC_H

#include <stdbool.h>
#include <stddef.h>

// ---------------------------------------------------------------- recorder

typedef struct Recorder Recorder;

// begin recording to `path`; `now` is the caller's monotonic clock (seconds).
Recorder *rec_start(const char *path, int cols, int rows,
                    const char *title, double now);
void rec_output(Recorder *r, double now, const char *buf, int len);
void rec_resize(Recorder *r, double now, int cols, int rows);
void rec_stop(Recorder *r);          // finalizes + closes the file, frees r
const char *rec_path(Recorder *r);

// ---------------------------------------------------------------- playback

typedef struct {
    double t;          // seconds from recording start
    char   type;       // 'o' = output bytes, 'r' = resize
    char  *data;       // 'o': raw bytes (owned)
    int    len;        // 'o': byte count
    int    cols, rows; // 'r'
} RecEvent;

typedef struct {
    int      cols, rows;
    char     title[256];
    RecEvent *ev;
    int      nev;
} RecFile;

RecFile *rec_load(const char *path);   // NULL on failure
void     rec_free(RecFile *f);

// newest "*.json" under `dir` -> out; returns false if none found.
bool rec_newest(const char *dir, char *out, size_t outsz);

#endif
