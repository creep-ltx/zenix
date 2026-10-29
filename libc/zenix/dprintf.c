/* SPDX-License-Identifier: GPL-2.0-only */
/* dprintf and vdprintf (POSIX; tinystdio has none): C's formatter straight to a file descriptor, through a 128-byte
 * buffer on the stack, so a program that writes this way links no stream (DESIGN D27 section 6: Zenix's own
 * programs' way of printing). EPIPE ends the program with 141, as the streams and libz do. */
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>

struct fdout { FILE f; int fd, n, err; char buf[128]; };

static int fdout_flush(FILE *f)
{
    struct fdout *o = (struct fdout *)f;
    for (int off = 0; off < o->n; ) {
        ssize_t r = write(o->fd, o->buf + off, (size_t)(o->n - off));
        if (r <= 0) {
            if (r < 0 && errno == EPIPE) _exit(141);
            o->err = 1;
            break;
        }
        off += (int)r;
    }
    o->n = 0;
    return o->err ? EOF : 0;
}
static int fdout_put(char c, FILE *f)
{
    struct fdout *o = (struct fdout *)f;
    o->buf[o->n++] = c;
    if (o->n == (int)sizeof o->buf) fdout_flush(f);
    return (unsigned char)c;
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
    struct fdout o = { .f = FDEV_SETUP_STREAM(fdout_put, NULL, fdout_flush, __SWR), .fd = fd };
    int r = vfprintf(&o.f, fmt, ap);
    fdout_flush(&o.f);
    return o.err ? -1 : r;
}
int dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vdprintf(fd, fmt, ap);
    va_end(ap);
    return r;
}
