/* SPDX-License-Identifier: GPL-2.0-only */
/* The C library's streams (DESIGN D27 section 5.2), over picolibc's buffered files (tinystdio's bufio):
 *  - stdout: 2,048 bytes, line-buffered on a terminal and fully buffered otherwise; stdin: 1,024 bytes, stdout
 *    flushed before it reads (bufio does that); both taken by malloc at their first use, so with their 8-byte headers
 *    they fit the allocator's first 4 KB chunk. stderr is unbuffered. A write that fails with EPIPE ends the
 *    program with status 141, as libz's oflush does.
 *  - fopen/fdopen's files (picolibc's fopen calls this fdopen) are kept in a list, so exit flushes every open stream
 *    as C asks (picolibc's own fdopen leaves that to fclose).
 *  - Nothing here is reached by a program that uses no stream: exit flushes through __zx_stdio_flush, which the
 *    first use sets (start.c). */
#include "stdio_private.h"
#include <errno.h>
#include <unistd.h>

extern void (*__zx_stdio_flush)(void);

static ssize_t zx_write(int fd, const void *buf, size_t n)
{
    ssize_t r = write(fd, buf, n);
    if (r < 0 && errno == EPIPE) _exit(141);            /* the reader is gone: end, as SIGPIPE would */
    return r;
}

struct zx_file { struct __file_bufio bf; struct zx_file *next; char buf[BUFSIZ]; };
static struct zx_file *open_files;

/* C's fflush: a stream, or with NULL every stream open for writing (picolibc's own fflush took NULL as a stream and
 * called through address 0's neighbours: found by Lua's io.popen, which flushes everything first, DESIGN D52 9) */
int fflush(FILE *stream)
{
    if (!stream) {
        int r = 0;
        if (fflush(stdout)) r = EOF;
        if (fflush(stderr)) r = EOF;
        for (struct zx_file *z = open_files; z; z = z->next)
            if ((z->bf.xfile.cfile.file.flags & __SWR) && fflush(&z->bf.xfile.cfile.file)) r = EOF;
        return r;
    }
    int ret = 0;
    __flockfile(stream);
    if (stream->flush)
        ret = (stream->flush)(stream);
    __funlock_return(stream, ret);
}

static void flush_all(void)
{
    fflush(stdout);
    for (struct zx_file *z = open_files; z; z = z->next)
        fflush(&z->bf.xfile.cfile.file);
}

/* the first character in or out of stdin or stdout: its buffer, its mode, then the ordinary bufio functions */
static int first_put(char c, FILE *f);
static int first_get(FILE *f);
static char fallback[2];                /* no memory for a buffer: unbuffered */
static __attribute__((noinline)) void first_use(FILE *f)
{
    struct __file_bufio *bf = (struct __file_bufio *)f;
    if (!bf->buf) {                     /* setvbuf may have given one */
        int size = f == stdout ? 2048 : 1024;
        bf->buf = malloc((size_t)size);
        bf->size = size;
        if (bf->buf) bf->bflags |= __BALL;
        else { bf->buf = &fallback[f == stdout]; bf->size = 1; }
        if (f == stdout && isatty(1)) bf->bflags |= __BLBF;
    }
    f->put = __bufio_put;
    f->get = __bufio_get;
    __zx_stdio_flush = flush_all;
}
static int first_put(char c, FILE *f) { first_use(f); return f->put(c, f); }
static int first_get(FILE *f) { first_use(f); return f->get(f); }

#define ZX_STREAM(_fd, _put, _get, _buf, _size, _rw)                                                       \
    { .xfile = FDEV_SETUP_EXT(_put, _get, __bufio_flush, __bufio_close, __bufio_seek, __bufio_setvbuf,    \
                              (_rw) | __SBUF),                                                             \
      .ptr = (void *)(intptr_t)(_fd), .dir = 0, .bflags = 0, .pos = 0, .buf = _buf, .size = _size,         \
      .len = 0, .off = 0, { .read_int = read }, { .write_int = zx_write }, { .lseek_int = lseek },          \
      { .close_int = close } }
static struct __file_bufio zx_stdin = ZX_STREAM(0, first_put, first_get, 0, 0, __SRD);
static struct __file_bufio zx_stdout = ZX_STREAM(1, first_put, first_get, 0, 0, __SWR);
static char errbuf[1];
static struct __file_bufio zx_stderr = ZX_STREAM(2, __bufio_put, __bufio_get, errbuf, 1, __SWR);
FILE *const stdin = &zx_stdin.xfile.cfile.file;
FILE *const stdout = &zx_stdout.xfile.cfile.file;
FILE *const stderr = &zx_stderr.xfile.cfile.file;

static int zx_fclose(FILE *f)
{
    for (struct zx_file **p = &open_files; *p; p = &(*p)->next)
        if (&(*p)->bf.xfile.cfile.file == f) { *p = (*p)->next; break; }
    return __bufio_close(f);            /* flushes, closes the descriptor, frees the zx_file (it starts with bf) */
}

FILE *fdopen(int fd, const char *mode)
{
    int open_flags, stdio_flags = __stdio_flags(mode, &open_flags);
    if (!stdio_flags) return 0;
    struct zx_file *z = calloc(1, sizeof *z);
    if (!z) { close(fd); return 0; }
    z->bf = (struct __file_bufio)FDEV_SETUP_BUFIO(fd, z->buf, BUFSIZ, read, zx_write, lseek, close, stdio_flags, __BFALL);
    z->bf.xfile.cfile.close = zx_fclose;
    z->next = open_files;
    open_files = z;
    __zx_stdio_flush = flush_all;
    FILE *f = &z->bf.xfile.cfile.file;
    if (open_flags & O_APPEND) (void)fseeko(f, 0, SEEK_END);
    return f;
}
