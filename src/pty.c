// pty.c — pseudo-terminal spawn / resize / write
#include "terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

bool pty_spawn(Pty *p, const char *shell, int cols, int rows) {
    struct winsize ws = { .ws_row = rows, .ws_col = cols };

    pid_t pid = forkpty(&p->fd, NULL, NULL, &ws);
    if (pid < 0) {
        perror("forkpty");
        return false;
    }

    if (pid == 0) { // child: become a login shell
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        setenv("TERM_PROGRAM", "terminal", 1);
        setenv("TERM_PROGRAM_VERSION", TERMINAL_VERSION, 1);
        unsetenv("LINES");
        unsetenv("COLUMNS");

        const char *base = strrchr(shell, '/');
        base = base ? base + 1 : shell;
        char argv0[256];
        snprintf(argv0, sizeof argv0, "-%s", base); // leading dash = login shell

        signal(SIGPIPE, SIG_DFL);
        execl(shell, argv0, (char *)NULL);
        perror("execl");
        _exit(127);
    }

    p->pid = pid;
    return true;
}

void pty_resize(Pty *p, int cols, int rows, int pxw, int pxh) {
    struct winsize ws = {
        .ws_row = rows, .ws_col = cols,
        .ws_xpixel = pxw, .ws_ypixel = pxh,
    };
    ioctl(p->fd, TIOCSWINSZ, &ws);
}

void pty_write(Pty *p, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(p->fd, buf, len);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}
