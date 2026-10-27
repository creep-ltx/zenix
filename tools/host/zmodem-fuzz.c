/* SPDX-License-Identifier: GPL-2.0-only */
/* The Zmodem engine (user/xfer/) and the download rules under libFuzzer: whatever a board sends, rz and sz must end
 * without touching memory they do not own. The first byte chooses: bit 0 send (sz) or receive (rz), bit 1 -e, bit 2 a
 * part is there to resume from (the rules ask ZCRC). The rest is all the board says; after it the line is gone. Files
 * live in memory (a few names, a fake file system for rzrules). Built by tools/host/telnet-test.sh fuzz. */
#include <stdlib.h>
#include <string.h>
#include "../../user/xfer/xfer.h"
#include "../../user/xfer/rzrules.h"

static const unsigned char *in;
static unsigned long inn, inp;
static int partthere;
static unsigned char sendfile[3000];

static long get(void *u, unsigned char *b, long max, long ms)
{
    unsigned long k;
    (void)u; (void)ms;
    if (inp >= inn)
        return -1;                      /* the line is gone */
    k = inn - inp < (unsigned long)max ? inn - inp : (unsigned long)max;
    if (k > 700)
        k = 700;                        /* in pieces, as a network gives them */
    memcpy(b, in + inp, k);
    inp += k;
    return (long)k;
}
static void put(void *u, const unsigned char *b, long n) { (void)u; (void)b; if (n < 0) abort(); }
static void show(void *u, const char *name, unsigned long d, unsigned long s, int w)
{
    (void)u; (void)d; (void)s;
    if (!name || strlen(name) > 300 || w < 0 || w > 3)
        abort();
}
static void rest(void *u, const unsigned char *b, long n) { (void)u; (void)b; if (n < 0) abort(); }

/* a file system of two names: "part" files exist when the first byte says so */
static int look(void *u, const char *p, unsigned long *size)
{
    size_t n = strlen(p);
    (void)u;
    if (n > RZ_NAMEMAX + 15)
        abort();                        /* a path longer than the rules promise */
    if (partthere && n > 5 && !strcmp(p + n - 5, ".part")) {
        *size = 10;
        return 1;
    }
    return 0;
}
static int dummy;
static void *fopen_(void *u, const char *p, int how) { (void)u; (void)p; (void)how; return &dummy; }
static long fread_(void *u, void *h, unsigned char *b, long n) { (void)u; (void)h; memset(b, 'p', n); return n; }
static long fwrite_(void *u, void *h, const unsigned char *b, long n) { (void)u; (void)h; (void)b; return n; }
static void fclose_(void *u, void *h) { (void)u; if (h != &dummy) abort(); }
static int frename(void *u, const char *a, const char *b) { (void)u; (void)a; (void)b; return 0; }
static void fremove(void *u, const char *p) { (void)u; (void)p; }
static struct rz_fs fs = { 0, look, fopen_, fread_, fwrite_, fclose_, frename, fremove, 0 };
static struct rz_dl dl;

static void *wopen(void *u, const char *name, unsigned long size, char *made, int madelen, unsigned long *have)
{
    (void)u;
    return rz_wopen(&dl, name, size, made, madelen, have);
}
static void *wresume(void *u, void *f, int a, unsigned long crc, char *made, int madelen, unsigned long *have)
{
    (void)u;
    return rz_wresume(&dl, f, a, crc, made, madelen, have);
}
static long wwrite(void *u, void *f, const unsigned char *b, long n) { (void)u; return rz_wwrite(&dl, f, b, n); }
static void wclose(void *u, void *f, int ok) { (void)u; rz_wclose(&dl, f, ok); }

static unsigned long rpos;
static void *ropen(void *u, const char *p, unsigned long *size) { (void)u; (void)p; rpos = 0; *size = sizeof sendfile; return &rpos; }
static long rread(void *u, void *f, unsigned char *b, long n)
{
    (void)u; (void)f;
    if (n < 0 || rpos > sizeof sendfile)
        abort();
    if ((unsigned long)n > sizeof sendfile - rpos)
        n = (long)(sizeof sendfile - rpos);
    memcpy(b, sendfile + rpos, n);
    rpos += (unsigned long)n;
    return n;
}
static int rseek(void *u, void *f, unsigned long p) { (void)u; (void)f; if (p > sizeof sendfile) return -1; rpos = p; return 0; }
static void rclose(void *u, void *f) { (void)u; (void)f; }

int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n)
{
    static const char *const paths[] = { "dir/a.bin", "b" };
    XferIO io;
    if (n < 1 || n > 200000)
        return 0;
    memset(&io, 0, sizeof io);
    io.get = get; io.put = put; io.show = show; io.rest = rest;
    io.wopen = wopen; io.wwrite = wwrite; io.wclose = wclose; io.wresume = wresume;
    io.ropen = ropen; io.rread = rread; io.rseek = rseek; io.rclose = rclose;
    io.escctl = (d[0] >> 1) & 1;
    partthere = (d[0] >> 2) & 1;
    memset(&dl, 0, sizeof dl);
    dl.fs = &fs;
    in = d + 1;
    inn = n - 1;
    inp = 0;
    if (d[0] & 1)
        zm_send(&io, paths, 2, 0, 0);
    else
        zm_receive(&io, 0, 0);
    return 0;
}
