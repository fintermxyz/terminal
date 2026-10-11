// links.c — find file paths in the visible grid (see links.h)
#include "links.h"

#include <ctype.h>
#include <libproc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_LINKS 512

static Link links[MAX_LINKS];
static int nlinks;
static int sel = -1;

// what the current links were computed from
static Term *seen_term;
static uint64_t seen_hash;
static char seen_cwd[PATH_MAX];

static bool is_sep(uint32_t cp) {
    if (cp <= ' ') return true;
    if (cp >= 0x2500 && cp <= 0x259f) return true; // box drawing / blocks
    return strchr("\"'`()[]{}<>|,;=", (int)cp) != NULL && cp < 0x80;
}

static size_t put_utf8(char *o, uint32_t cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        o[0] = (char)(0xc0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        o[0] = (char)(0xe0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        o[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    o[0] = (char)(0xf0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

// drops a trailing ":12" or ":12:3" (compiler / grep locations)
static void strip_location(char *s) {
    for (int k = 0; k < 2; k++) {
        char *c = strrchr(s, ':');
        if (!c || !c[1]) return;
        for (char *d = c + 1; *d; d++)
            if (!isdigit((unsigned char)*d)) return;
        *c = 0;
    }
}

static bool resolve(const char *tok, const char *cwd, char *out, bool *dir) {
    char buf[PATH_MAX];
    const char *home = getenv("HOME");
    if (tok[0] == '/') snprintf(buf, sizeof buf, "%s", tok);
    else if (tok[0] == '~' && tok[1] == '/' && home) snprintf(buf, sizeof buf, "%s%s", home, tok + 1);
    else if (cwd[0]) snprintf(buf, sizeof buf, "%s/%s", cwd, tok);
    else return false;
    struct stat st;
    if (stat(buf, &st) != 0) return false;
    if (!realpath(buf, out)) snprintf(out, PATH_MAX, "%s", buf);
    *dir = S_ISDIR(st.st_mode);
    return true;
}

// a token spanning cells [x0, x1) of row y; cps/xs map chars to cells
static void consider(int y, const uint32_t *cps, const int *xs, int a, int b,
                     const char *cwd) {
    // trailing sentence punctuation isn't part of the name
    while (b > a && strchr(".,:;!?", (int)cps[b - 1]) && cps[b - 1] < 0x80) b--;
    if (b - a < 1 || nlinks == MAX_LINKS) return;

    char tok[PATH_MAX];
    size_t n = 0;
    bool alnum = false;
    for (int i = a; i < b && n < sizeof tok - 5; i++) {
        if (cps[i] < 0x80 && isalnum((int)cps[i])) alnum = true;
        n += put_utf8(tok + n, cps[i]);
    }
    tok[n] = 0;
    if (!alnum || tok[0] == '-' || strstr(tok, "://")) return;

    char *t = tok;
    if (strncmp(t, "file://", 7) == 0) t += 7;
    strip_location(t);
    if (!strcmp(t, ".") || !strcmp(t, "..") || !t[0]) return;

    Link *l = &links[nlinks];
    if (!resolve(t, cwd, l->path, &l->dir)) {
        // git diff headers: a/src/x.c, b/src/x.c
        if (!((t[0] == 'a' || t[0] == 'b') && t[1] == '/' && resolve(t + 2, cwd, l->path, &l->dir)))
            return;
    }
    if (!strcmp(l->path, "/")) return;
    l->y = y;
    l->x0 = xs[a];
    l->x1 = xs[b - 1] + 1;
    nlinks++;
}

static void scan(Term *t, const char *cwd) {
    char keep[PATH_MAX] = "";
    int keep_y = 0;
    if (sel >= 0) { snprintf(keep, sizeof keep, "%s", links[sel].path); keep_y = links[sel].y; }

    nlinks = 0;
    uint32_t cps[1024];
    int xs[1024];
    for (int y = 0; y < t->rows; y++) {
        const Cell *row = term_visible_row(t, y);
        if (!row) continue;
        int n = 0;
        for (int x = 0; x < t->cols && n < 1024; x++) {
            if (row[x].attrs & ATTR_WIDEDUMMY) continue;
            cps[n] = row[x].cp;
            xs[n++] = x;
        }
        for (int i = 0; i < n;) {
            while (i < n && is_sep(cps[i])) i++;
            int a = i;
            while (i < n && !is_sep(cps[i])) i++;
            if (i > a) consider(y, cps, xs, a, i, cwd);
        }
    }

    // keep the selection on the same file, preferring the same row
    sel = -1;
    if (keep[0])
        for (int i = 0; i < nlinks; i++)
            if (!strcmp(links[i].path, keep) && (sel < 0 || links[i].y == keep_y)) sel = i;
}

int links_update(Term *t, pid_t shell) {
    char cwd[PATH_MAX] = "";
    struct proc_vnodepathinfo vpi;
    if (proc_pidinfo(shell, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof vpi) > 0)
        snprintf(cwd, sizeof cwd, "%s", vpi.pvi_cdir.vip_path);

    // FNV-1a over what's on screen
    uint64_t h = 1469598103934665603ull;
    h = (h ^ (uint64_t)t->cols) * 1099511628211ull;
    h = (h ^ (uint64_t)t->rows) * 1099511628211ull;
    for (int y = 0; y < t->rows; y++) {
        const Cell *row = term_visible_row(t, y);
        for (int x = 0; row && x < t->cols; x++)
            h = (h ^ row[x].cp) * 1099511628211ull;
    }

    if (t != seen_term) sel = -1;
    if (t != seen_term || h != seen_hash || strcmp(cwd, seen_cwd)) {
        scan(t, cwd);
        seen_term = t;
        seen_hash = h;
        snprintf(seen_cwd, sizeof seen_cwd, "%s", cwd);
    }
    return nlinks;
}

int links_count(void) { return nlinks; }
const Link *links_get(int i) { return i >= 0 && i < nlinks ? &links[i] : NULL; }

int links_at(int x, int y) {
    for (int i = 0; i < nlinks; i++)
        if (links[i].y == y && x >= links[i].x0 && x < links[i].x1) return i;
    return -1;
}

void links_select(int i) { sel = i >= 0 && i < nlinks ? i : -1; }
int links_selected(void) { return sel; }
