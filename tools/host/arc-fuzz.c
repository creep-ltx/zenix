// SPDX-License-Identifier: GPL-2.0-only
/* Fuzzing the LhA and LZX readers (DESIGN D33) under ASAN and UBSAN: tools/arc-host-test.py builds it, or by hand. Each round takes one of the
 * seed archives, damages it (bit flips, bytes set, a piece cut out or repeated, the end cut off, biased to the headers), lists it and unpacks every
 * member through a sink that keeps only a count. A crash stops the run under the sanitizers; a round over its time is a hang: saved and fatal.
 * Prints its counts at the end, the line the test looks for. usage: arc-fuzz ROUNDS SEED OUTFILE file... */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "arcread.h"

static unsigned char *data;
static unsigned long dsize;
static unsigned long long rstate;
static unsigned rnd(void) { rstate = rstate * 6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)(rstate >> 33); }

static unsigned long live, peak;
void *arc_alloc(unsigned long n)
{
    unsigned long *p = calloc(1, n + 16);
    if (!p) return 0;
    p[0] = n;
    live += n;
    if (live > peak) peak = live;
    return (char *)p + 16;
}
void arc_free(void *q)
{
    unsigned long *p = (void *)((char *)q - 16);
    live -= p[0];
    free(p);
}

static long rd(void *ctx, unsigned long off, void *buf, long n)
{
    (void)ctx;
    if (off >= dsize) return 0;
    if ((unsigned long)n > dsize - off) n = (long)(dsize - off);
    memcpy(buf, data + off, (size_t)n);
    return n;
}
static unsigned long long outbytes, roundbytes;
static int wr(void *ctx, const void *b, long n)
{
    const volatile unsigned char *p = b;
    (void)ctx;
    if (n > 0) { (void)p[0]; (void)p[n - 1]; }     /* the sanitizer looks at what it is given */
    outbytes += (unsigned long long)n;
    roundbytes += (unsigned long long)n;
    return roundbytes < 16u * 1024 * 1024;         /* a limit as the commands have one */
}
static const struct arcsrc *gsrc;
static int (*xf)(const struct arcsrc *, const struct arcent *, const struct arcsink *);
static unsigned long nmem, nok, ncrc, nerr;
static int add(void *ctx, const struct arcent *e)
{
    struct arcsink s = { wr, 0 };
    int r;
    (void)ctx;
    if (strlen(e->name) > 255) abort();
    nmem++;
    r = xf(gsrc, e, &s);
    if (r == 0) nok++;
    else if (r == ARC_ECRC) ncrc++;
    else nerr++;
    return roundbytes < 16u * 1024 * 1024;
}
static const char *outfile;

static void onalarm(int sig)
{
    FILE *f = fopen(outfile, "wb");
    (void)sig;
    if (f) { fwrite(data, 1, dsize, f); fclose(f); }
    printf("arc-fuzz: HANG, the input saved\n");
    fflush(stdout);
    _exit(3);
}

static void damage(unsigned char *d, unsigned long *n)
{
    unsigned k, times = 1 + rnd() % 6;
    for (k = 0; k < times && *n; k++) {
        unsigned kind = rnd() % 8;
        /* half the damage goes to the first bytes and to what follows: headers are small */
        unsigned long at = rnd() % 2 ? rnd() % (*n < 400 ? *n : 400) : rnd() % *n;
        switch (kind) {
        case 0: case 1: d[at] ^= (unsigned char)(1u << (rnd() % 8)); break;
        case 2: d[at] = (unsigned char)rnd(); break;
        case 3: d[at] = rnd() % 2 ? 0xFF : 0; break;
        case 4: {                               /* a run of one byte */
            unsigned long m = 1 + rnd() % 64, i;
            unsigned char v = (unsigned char)rnd();
            for (i = at; i < *n && i < at + m; i++) d[i] = v;
            break; }
        case 5: {                               /* a piece taken out */
            unsigned long m = 1 + rnd() % 256;
            if (at + m < *n) { memmove(d + at, d + at + m, *n - at - m); *n -= m; }
            break; }
        case 6: {                               /* a piece over another */
            unsigned long from = rnd() % *n, m = 1 + rnd() % 256;
            if (from + m < *n && at + m < *n) memmove(d + at, d + from, m);
            break; }
        default:
            if (!(rnd() % 4)) *n = 1 + at;      /* the end cut off */
            else d[at] = (unsigned char)(d[at] + 1);
        }
    }
}

/* Damage to a header mostly fails its checksum, and what is behind that check would get little testing: half the rounds put the checksums right
 * again, as someone crafting an archive would. */
static unsigned long le32at(const unsigned char *p) { return p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24; }
static void repair(int lzx)
{
    unsigned long p = lzx ? 10 : 0;
    if (lzx) {
        while (p + 31 <= dsize) {
            unsigned char *h = data + p;
            unsigned long n = 31ul + h[30] + h[14], c;
            if (p + n > dsize) return;
            h[26] = h[27] = h[28] = h[29] = 0;
            c = arc_crc32(0, h, (long)n);
            h[26] = (unsigned char)c; h[27] = (unsigned char)(c >> 8); h[28] = (unsigned char)(c >> 16); h[29] = (unsigned char)(c >> 24);
            if (le32at(h + 6) > dsize) return;
            p += n + le32at(h + 6);
        }
    } else {
        while (p + 22 <= dsize && data[p]) {
            unsigned char *h = data + p;
            unsigned long n = 2ul + h[0], k, pk = le32at(h + 7);
            unsigned sum = 0;
            if (h[20] > 1 || p + n > dsize) return;
            for (k = 2; k < n; k++) sum += h[k];
            h[1] = (unsigned char)sum;
            if (pk > dsize) return;
            p += n + pk;
        }
    }
}

int main(int argc, char **argv)
{
    unsigned long rounds, r, i, top = 0;
    int nf = argc - 4;
    unsigned char **seed;
    unsigned long *ssize;
    int *islzx;
    if (argc < 5) return 2;
    rounds = strtoul(argv[1], 0, 10);
    rstate = strtoull(argv[2], 0, 10) * 2 + 1;
    outfile = argv[3];
    seed = calloc((size_t)nf, sizeof *seed);
    ssize = calloc((size_t)nf, sizeof *ssize);
    islzx = calloc((size_t)nf, sizeof *islzx);
    for (i = 0; i < (unsigned long)nf; i++) {
        FILE *f = fopen(argv[4 + i], "rb");
        size_t l = strlen(argv[4 + i]);
        if (!f) { printf("arc-fuzz: cannot open %s\n", argv[4 + i]); return 2; }
        fseek(f, 0, SEEK_END);
        ssize[i] = (unsigned long)ftell(f);
        rewind(f);
        seed[i] = malloc(ssize[i] + 1);
        if (fread(seed[i], 1, ssize[i], f) != ssize[i]) return 2;
        fclose(f);
        islzx[i] = l > 4 && !strcmp(argv[4 + i] + l - 4, ".lzx");
        if (ssize[i] > top) top = ssize[i];
    }
    data = malloc(top + 1);
    signal(SIGALRM, onalarm);
    for (r = 0; r < rounds; r++) {
        unsigned k = rnd() % (unsigned)nf;
        struct arcsrc s = { rd, 0, 0 };
        int e;
        memcpy(data, seed[k], ssize[k]);
        dsize = ssize[k];
        damage(data, &dsize);
        if (rnd() & 1) repair(islzx[k]);
        s.size = dsize;
        gsrc = &s;
        roundbytes = 0;
        alarm(30);
        arc_list_refused = (int)(rnd() & 1);
        xf = islzx[k] ? lzx_extract : lha_extract;
        e = islzx[k] ? lzx_list(&s, add, 0) : lha_list(&s, add, 0);
        (void)e;
        lzx_end();
        arc_freetables();
        alarm(0);
        if (live) { printf("arc-fuzz: %lu bytes not given back\n", live); return 3; }
    }
    printf("arc-fuzz: %lu rounds, %lu members, %lu whole, %lu checksums wrong, %lu errors, %llu bytes unpacked, at most %lu bytes held: no crash, no hang\n",
           rounds, nmem, nok, ncrc, nerr, outbytes, peak);
    for (i = 0; i < (unsigned long)nf; i++) free(seed[i]);
    free(seed);
    free(ssize);
    free(islzx);
    free(data);
    return 0;
}
