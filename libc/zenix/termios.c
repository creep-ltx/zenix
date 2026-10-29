/* SPDX-License-Identifier: GPL-2.0-only */
/* The terminal (DESIGN D27 5.4): termios over the kernel's two tty modes (SYS_ttymode: cooked, or TTY_RAW with
 * TTY_NOSIG), so that nothing a program asks for is silently ignored:
 *  - ICANON, ECHO and ISIG all on: the kernel's cooked mode (its line editing and echo, ^C and ^Z signals);
 *  - any of them off: raw mode, plus TTY_NOSIG when ISIG is off (^C is then a byte); if ICANON is still on, read edits
 *    the line here (erase, kill, ^D at the start of a line, the newline ends it) and echoes only when ECHO is on: a
 *    password prompt (ICANON on, ECHO off) reads a line with nothing shown.
 * VMIN 1 and VTIME 0 only (others EINVAL); the other bits are kept and answered back but change nothing. The kernel
 * has no call that reports the mode, so tcgetattr answers what this process set last (cooked at its start). getpass is
 * zpw.c's read_secret (raw, no echo), after which the mode set last is put back. */
#include <errno.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "zshare.h"

extern ssize_t (*__zx_tty_read)(int fd, void *buf, size_t n);     /* sys.c's read calls it when set */

static struct termios cur;
static int have;                        /* cur holds the settings (else: the defaults, cooked) */
static void defaults(void)
{
    if (have) return;
    have = 1;
    memset(&cur, 0, sizeof cur);
    cur.c_iflag = ICRNL | IXON;
    cur.c_oflag = OPOST | ONLCR;
    cur.c_cflag = CS8 | CREAD | B38400;
    cur.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    cur.c_ispeed = cur.c_ospeed = B38400;
    static const cc_t cc[] = { [VINTR] = 3, [VQUIT] = 28, [VERASE] = 127, [VKILL] = 21, [VEOF] = 4, [VTIME] = 0,
                               [VMIN] = 1, [VSTART] = 17, [VSTOP] = 19, [VSUSP] = 26, [VREPRINT] = 18,
                               [VDISCARD] = 15, [VWERASE] = 23, [VLNEXT] = 22 };
    memcpy(cur.c_cc, cc, sizeof cc);
}

/* ---- the line editing read does in raw mode with ICANON on: as the kernel's own (kernel/tty.c), lines of at most
 * 511 characters (a longer one is refused, with the kernel's words), the finished line handed out in pieces */
#define LMAX 512
static char line[LMAX];
static int llen, lpos, lready;
static void echo(int fd, const char *s, int n) { if (cur.c_lflag & ECHO) sys_write(fd, s, (u32)n); }
static ssize_t edit_read(int fd, void *buf, size_t n)
{
    if (sys_isatty(fd) != 1) { int r = sys_read(fd, buf, (u32)n); return r < 0 ? -1 : r; }   /* a file, a pipe */
    if (!lready) {
        int over = 0;
        llen = 0;
        for (;;) {
            unsigned char c;
            int r = sys_read(fd, &c, 1);
            if (r < 0) { llen = 0; return -1; }         /* a signal's handler (EINTR): the line so far is dropped */
            if (r == 0) { if (!llen) return 0; break; }
            if (c == '\r' || c == '\n') {
                if (cur.c_lflag & (ECHO | ECHONL)) sys_write(fd, "\n", 1);
                if (over) {
                    static const char msg[] = "tty: the line was too long (more than 511 characters): not taken\n";
                    sys_write(fd, msg, sizeof msg - 1);
                    over = llen = 0;
                    continue;
                }
                line[llen++] = '\n';
                break;
            }
            if (c == cur.c_cc[VEOF]) {                  /* ^D: the end at the start of a line, else the line so far */
                if (over) { over = llen = 0; continue; }
                if (!llen) return 0;
                break;
            }
            if (c == cur.c_cc[VERASE] || c == 8 || c == 127) {
                if (llen && !over) { llen--; echo(fd, "\b \b", 3); }
                continue;
            }
            if (c == cur.c_cc[VKILL]) {
                for (; llen; llen--) echo(fd, "\b \b", 3);
                over = 0;
                continue;
            }
            if (over || llen >= LMAX - 1) {             /* full: the bell, and the line will be refused */
                sys_write(fd, "\a", 1);
                over = 1;
                continue;
            }
            line[llen++] = (char)c;
            if (c >= 32 || c == '\t') echo(fd, (char *)&c, 1);
        }
        lready = 1;
        lpos = 0;
    }
    size_t k = (size_t)(llen - lpos) < n ? (size_t)(llen - lpos) : n;
    memcpy(buf, line + lpos, k);
    lpos += (int)k;
    if (lpos >= llen) lready = 0;
    return (ssize_t)k;
}

static void apply(void)
{
    tcflag_t l = cur.c_lflag;
    if ((l & (ICANON | ECHO | ISIG)) == (ICANON | ECHO | ISIG)) {
        sys_ttymode(0);
        __zx_tty_read = 0;
    } else {
        sys_ttymode(TTY_RAW | (l & ISIG ? 0 : TTY_NOSIG));
        __zx_tty_read = l & ICANON ? edit_read : 0;
    }
    lready = 0;
}

int tcgetattr(int fd, struct termios *t)
{
    if (!isatty(fd)) return -1;         /* ENOTTY, or EBADF */
    defaults();
    *t = cur;
    return 0;
}
int tcsetattr(int fd, int when, const struct termios *t)
{
    if (!isatty(fd)) return -1;
    if (when < TCSANOW || when > TCSAFLUSH || t->c_cc[VMIN] != 1 || t->c_cc[VTIME] != 0) { errno = EINVAL; return -1; }
    defaults();
    cur = *t;
    apply();
    return 0;
}
void cfmakeraw(struct termios *t)
{
    t->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~(tcflag_t)OPOST;
    t->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}
speed_t cfgetispeed(const struct termios *t) { return t->c_ispeed; }
speed_t cfgetospeed(const struct termios *t) { return t->c_ospeed; }
int cfsetispeed(struct termios *t, speed_t s) { t->c_ispeed = s; return 0; }
int cfsetospeed(struct termios *t, speed_t s) { t->c_ospeed = s; return 0; }

/* a password: the prompt on standard output, a line from standard input with nothing echoed (zpw.c's read_secret,
 * the one login and su use); 0 on ^C or the end of input */
char *getpass(const char *prompt)
{
    static char pass[128];
    int r = read_secret(prompt, pass, sizeof pass);
    if (have) apply();                  /* read_secret leaves the cooked mode: the one set last again */
    if (r < 0) { errno = EINTR; return 0; }
    return pass;
}
