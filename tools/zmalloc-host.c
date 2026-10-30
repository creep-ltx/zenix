/* SPDX-License-Identifier: GPL-2.0-only */
/* user/zmalloc.c on the PC (tools/zmalloc-test.sh; docs/d27-c-library.md "The allocator"): the allocator built for a
 * 32-bit PC (-m32, as the Amiga's pointers) under other names (zm_malloc ...), with the kernel's SYS_memalloc
 * imitated as kernel/mem.c gives it: at most NREG (8) regions a process, each rounded to 16 bytes with a 16-byte
 * header, so "held" below is what `ps` counts for the heap.
 * MODE -r N: N regions a process instead of 8 (what another kernel limit would give).
 *   zmalloc-host replay TRACE...                replay allocation traces (a ZMALLOC_TRACE build's /var/mt files:
 *                                               m/f/r/a lines, the region heap's h/g, netd's n/o): the bytes asked
 *                                               for and the bytes held, at the end and at the peak, the regions
 *   zmalloc-host stress [SEED]                  a million random calls checked: every block's bytes kept, no
 *                                               overlap, realloc keeps the contents, the regions all given back
 *   zmalloc-host speed [TRACE...]               the time of a small malloc/free pair (the hot path), a medium one,
 *                                               and each trace's calls replayed 200 times, in ns
 *   zmalloc-host room [SIZE]                    how much the regions hold in blocks of SIZE (100) bytes
 *   zmalloc-host bad                            the checks: a pointer not given out and a double free are reported
 *                                               and ignored; a corrupt header next to a freed block stops
 * Every block is filled with its own byte when given and checked when freed or moved: an allocator that hands out
 * overlapping memory or loses bytes in realloc fails here. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <setjmp.h>
#include <stdarg.h>

void *zm_malloc(size_t n);
void zm_free(void *p);
void *zm_realloc(void *p, size_t n);
void *zm_calloc(size_t n, size_t s);

/* ---- the kernel's regions ---- */
#define NREG 64                             /* slots; reg_max (-r, default 8: kernel/kernel.h) is the limit */
static int reg_max = 8;
static struct { void *p; uint32_t n; } reg[NREG];
static uint32_t held, held_peak, nreg, nreg_peak;
static uint32_t kcost(uint32_t n) { return ((n + 15) & ~15u) + 16; }   /* kernel/mem.c's header and rounding */
void *sys_memalloc(uint32_t n)
{
    if (!n || nreg >= (uint32_t)reg_max) return 0;
    for (int i = 0; i < NREG; i++)
        if (!reg[i].p) {
            void *p = aligned_alloc(16, (n + 15) & ~15u);
            if (!p) return 0;
            memset(p, 0xA5, n);             /* the kernel does not clear a region */
            reg[i].p = p; reg[i].n = n;
            if (getenv("ZM_VERBOSE")) fprintf(stderr, "  region +%u\n", n);
            nreg++; held += kcost(n);
            if (nreg > nreg_peak) nreg_peak = nreg;
            if (held > held_peak) held_peak = held;
            return p;
        }
    return 0;
}
int sys_memfree(void *p)
{
    for (int i = 0; i < NREG; i++)
        if (reg[i].p == p && p) {
            held -= kcost(reg[i].n); nreg--;
            if (getenv("ZM_VERBOSE")) fprintf(stderr, "  region -%u\n", reg[i].n);
            free(p); reg[i].p = 0;
            return 0;
        }
    fprintf(stderr, "FAIL: SYS_memfree of %p, not a region\n", p);
    exit(1);
}
static int reported, quiet;                 /* the allocator's reports on standard error */
int sys_write(int fd, const void *b, uint32_t n)
{
    if (fd == 2) reported++;
    return quiet ? (int)n : (int)fwrite(b, 1, n, fd == 2 ? stderr : stdout);
}
int zm_dprintf(const char *f, ...)              /* libz's dprintf(2, ...) for the allocator's reports */
{
    char b[200]; va_list a; va_start(a, f);
    int n = vsnprintf(b, sizeof b, f, a); va_end(a);
    return sys_write(2, b, (uint32_t)(n < (int)sizeof b ? n : (int)sizeof b - 1));
}
static jmp_buf stopped; static int stop_status;
void sys_exit(int c) { stop_status = c; longjmp(stopped, 1); }

/* ---- the blocks given out: their bytes are their id's ---- */
struct blk { uint32_t key; unsigned char *p; uint32_t n; unsigned char fill; };
static struct blk *bt; static uint32_t bcap, bn;
static uint32_t live, live_peak;
static uint32_t hsh(uint32_t k) { return (k * 2654435761u) & (bcap - 1); }
static struct blk *bfind(uint32_t key, int add)
{
    if (add && (bn + 1) * 2 > bcap) {
        struct blk *o = bt; uint32_t oc = bcap;
        bcap = bcap ? bcap * 2 : 1024; bt = calloc(bcap, sizeof *bt); bn = 0;
        for (uint32_t i = 0; i < oc; i++) if (o[i].p) { *bfind(o[i].key, 1) = o[i]; }
        free(o);
    }
    if (!bcap) return 0;
    for (uint32_t i = hsh(key);; i = (i + 1) & (bcap - 1)) {
        if (bt[i].p && bt[i].key == key) return &bt[i];
        if (!bt[i].p) { if (!add) return 0; bt[i].key = key; bn++; return &bt[i]; }
    }
}
static void bdel(struct blk *b)             /* open addressing: put the rest of the run back */
{
    uint32_t i = (uint32_t)(b - bt);
    b->p = 0; bn--;
    for (uint32_t j = (i + 1) & (bcap - 1); bt[j].p; j = (j + 1) & (bcap - 1)) {
        struct blk t = bt[j]; bt[j].p = 0; bn--;
        *bfind(t.key, 1) = t;
    }
}
static int bad;
static void check(struct blk *b, const char *when)
{
    for (uint32_t i = 0; i < b->n; i++)
        if (b->p[i] != b->fill) {
            printf("FAIL: a block of %u bytes changed at byte %u (%s)\n", b->n, i, when);
            bad = 1;
            return;
        }
}
static unsigned char nextfill = 1;
static void give(uint32_t key, void *p, uint32_t n)
{
    struct blk *b = bfind(key, 1);
    b->p = p; b->n = n; b->fill = nextfill++;
    if (!nextfill) nextfill = 1;
    memset(p, b->fill, n);
    live += n; if (live > live_peak) live_peak = live;
}
static void take(struct blk *b) { live -= b->n; bdel(b); }

/* ---- replay ---- */
static int replay(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return 1; }
    char line[128]; unsigned long calls = 0, fails = 0;
    while (fgets(line, sizeof line, f)) {
        uint32_t a = 0, b = 0, c = 0; char op;
        int k = sscanf(line, "%c %x %x %x", &op, &a, &b, &c);
        if (k < 2) continue;
        calls++;
        if (op == 'm' || op == 'h' || op == 'n') {          /* size, block (0: it failed there) */
            if (!b) continue;
            void *p = zm_malloc(a);
            if (!p) { fails++; continue; }
            give(b, p, a);
        } else if (op == 'a') {                             /* align, size, block */
            if (!c) continue;
            void *p = zm_malloc(b + a);
            if (!p) { fails++; continue; }
            give(c, p, b);
        } else if (op == 'f' || op == 'g' || op == 'o') {
            struct blk *x = bfind(a, 0);
            if (!x) continue;                               /* given before the trace began (or failed here) */
            check(x, "free");
            zm_free(x->p);
            take(x);
        } else if (op == 'r' && k == 4) {                   /* old, size, new */
            struct blk *x = a ? bfind(a, 0) : 0;
            if (!c) continue;
            uint32_t keep = x ? (x->n < b ? x->n : b) : 0;
            unsigned char fill = x ? x->fill : 0;
            if (x) check(x, "realloc");
            void *p = zm_realloc(x ? x->p : 0, b);
            if (!p) { fails++; continue; }
            for (uint32_t i = 0; i < keep; i++)
                if (((unsigned char *)p)[i] != fill) { printf("FAIL: realloc lost byte %u of %u\n", i, keep); bad = 1; break; }
            if (x) take(x);
            give(c, p, b);
        }
    }
    fclose(f);
    printf("%s: %lu calls%s; asked %u B at the end, %u at the peak; held %u B (%u KB) at the end, %u (%u KB) at "
           "the peak; regions %u, at most %u\n", path, calls, fails ? " (some failed here)" : "", live, live_peak,
           held, (held + 1023) / 1024, held_peak, (held_peak + 1023) / 1024, nreg, nreg_peak);
    return 0;
}

/* ---- stress ---- */
static uint32_t rs = 1;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }
static uint32_t rsize(void)
{
    uint32_t r = rnd() % 100;
    return r < 60 ? rnd() % 64 + 1 : r < 85 ? rnd() % 1024 + 1 : r < 97 ? rnd() % 20000 + 1 : rnd() % 150000 + 1;
}
static int stress(uint32_t seed)
{
    rs = seed ? seed : 1;
    enum { N = 2000 };
    static void *p[N]; static uint32_t key[N];
    uint32_t nextkey = 1; unsigned long nfail = 0;
    for (long i = 0; i < 1000000 && !bad; i++) {
        uint32_t s = rnd() % N, op = rnd() % 10;
        struct blk *x = p[s] ? bfind(key[s], 0) : 0;
        if (!x) {
            uint32_t n = rsize();
            void *q = op ? zm_malloc(n) : zm_calloc(1, n);
            if (!q) { nfail++; continue; }
            if (!op) for (uint32_t j = 0; j < n; j++) if (((unsigned char *)q)[j]) { printf("FAIL: calloc not zero\n"); return 1; }
            give(key[s] = nextkey++, p[s] = q, n);
        } else if (op < 6) {
            check(x, "free"); zm_free(x->p); take(x); p[s] = 0;
        } else {
            uint32_t n = op == 9 ? rnd() % 64 + 1 : rsize();
            uint32_t keep = x->n < n ? x->n : n; unsigned char fill = x->fill;
            check(x, "realloc");
            void *q = zm_realloc(x->p, n);
            if (!q) { nfail++; check(x, "a failed realloc"); continue; }   /* the old block stays as it was */
            for (uint32_t j = 0; j < keep; j++)
                if (((unsigned char *)q)[j] != fill) { printf("FAIL: realloc lost byte %u of %u\n", j, keep); return 1; }
            take(x);
            give(key[s] = nextkey++, p[s] = q, n);
        }
    }
    for (int s = 0; s < N; s++) {
        struct blk *x = p[s] ? bfind(key[s], 0) : 0;
        if (x) { check(x, "the end"); zm_free(x->p); take(x); }
    }
    printf("stress (seed %u, %d regions): %s; held at the peak %u KB for %u KB asked at the peak; %lu calls out of "
           "regions; at the end %u B in %u regions\n", seed, reg_max, bad ? "FAILED" : "every block kept", held_peak / 1024,
           live_peak / 1024, nfail, held, nreg);
    return bad;
}

/* ---- speed: small blocks of a few sizes given and freed in turn (list nodes, words: the hot path), medium ones,
 * and the traces' own calls over and over (no filling, no checks: only the allocator's time) ---- */
static double nsof(struct timespec *t0, struct timespec *t1, double n)
{
    return ((t1->tv_sec - t0->tv_sec) * 1e9 + (t1->tv_nsec - t0->tv_nsec)) / n;
}
static int speed(int ntr, char **tr)
{
    enum { R = 2000000 };
    static void *q[64];
    static const uint32_t sz[8] = { 12, 16, 24, 32, 40, 64, 100, 200 };
    struct timespec t0, t1;
    rs = 5;
    for (int i = 0; i < 64; i++) q[i] = zm_malloc(sz[i & 7]);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < R; i++) {
        int s = rnd() & 63;
        zm_free(q[s]);
        q[s] = zm_malloc(sz[rnd() & 7]);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("speed: a small free + malloc %.1f ns", nsof(&t0, &t1, R));
    void *big[8];
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < R / 20; i++) {
        for (int j = 0; j < 8; j++) big[j] = zm_malloc(300 + j * 500);
        for (int j = 7; j >= 0; j--) zm_free(big[j]);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("; a 300-3,800 byte malloc + free %.1f ns", nsof(&t0, &t1, R / 20 * 8));
    for (int i = 0; i < 64; i++) zm_free(q[i]);
    for (int t = 0; t < ntr; t++) {
        FILE *f = fopen(tr[t], "r");
        if (!f) { perror(tr[t]); return 1; }
        static char op[100000]; static uint32_t a[100000], b[100000], c[100000];
        int n = 0; char line[128];
        while (n < 100000 && fgets(line, sizeof line, f)) {
            a[n] = b[n] = c[n] = 0;
            if (sscanf(line, "%c %x %x %x", &op[n], &a[n], &b[n], &c[n]) >= 2) n++;
        }
        fclose(f);
        long calls = 0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int round = 0; round < 200; round++) {
            for (int i = 0; i < n; i++) {
                char o = op[i];
                if ((o == 'm' || o == 'h' || o == 'n') && b[i]) { void *p = zm_malloc(a[i]); if (p) bfind(b[i], 1)->p = p; }
                else if (o == 'f' || o == 'g' || o == 'o') { struct blk *x = bfind(a[i], 0); if (x) { zm_free(x->p); bdel(x); } }
                else if (o == 'r' && c[i]) {
                    struct blk *x = a[i] ? bfind(a[i], 0) : 0;
                    void *p = zm_realloc(x ? x->p : 0, b[i]);
                    if (x) bdel(x);
                    if (p) bfind(c[i], 1)->p = p;
                } else continue;
                calls++;
            }
            for (uint32_t i = 0; i < bcap; i++) if (bt[i].p) { zm_free(bt[i].p); bt[i].p = 0; }
            bn = 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        printf("; %s %.1f ns a call", strrchr(tr[t], '/') ? strrchr(tr[t], '/') + 1 : tr[t], nsof(&t0, &t1, calls));
    }
    printf(" (the PC: compare builds, not with the Amiga)\n");
    return 0;
}

static int badtest(void)
{
    int fail = 0;
    quiet = 1;
    char *p = zm_malloc(40), *q = zm_malloc(3000);
    static uint32_t fake[4] = { 16, 0x12345678, 0, 0 };
    reported = 0;
    zm_free(&fake[2]);
    if (reported != 1) { printf("FAIL: a free of a pointer malloc did not give out was not reported\n"); fail = 1; }
    zm_free(p); zm_free(p);
    if (reported != 2) { printf("FAIL: a small block freed twice was not reported\n"); fail = 1; }
    zm_free(q); zm_free(q);
    if (reported != 3) { printf("FAIL: a big block freed twice was not reported\n"); fail = 1; }
    if (zm_realloc(&fake[2], 10) || reported != 4) { printf("FAIL: realloc of a stray pointer\n"); fail = 1; }
    if (zm_malloc(0x7fffffffu)) { printf("FAIL: a huge request was not refused\n"); fail = 1; }
    /* a program writes past its block over the next one's header: the free that joins them stops */
    char *a = zm_malloc(1000), *b = zm_malloc(1000), *c[40];
    for (int i = 0; i < 40; i++) c[i] = zm_malloc(1000);
    zm_free(b);
    memset(a, 'x', 1000 + 8);
    if (!setjmp(stopped)) {
        zm_free(a);                         /* (on a quick list: the check comes when the lists go back) */
        for (int i = 0; i < 40; i++) zm_free(c[i]);
        printf("FAIL: a damaged header next to a freed block was not noticed\n"); fail = 1;
    } else if (stop_status != 134 || reported != 5) { printf("FAIL: the damaged header: status %d\n", stop_status); fail = 1; }
    quiet = 0;
    if (!fail) printf("bad: stray and double frees reported and ignored, a huge request refused, a damaged header "
                      "stops the program (134)\n");
    return fail;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: zmalloc-host MODE [-r N] ...: replay TRACE... | stress [SEED] | speed | room [SIZE] | bad\n"); return 2; }
    if (argc > 3 && !strcmp(argv[2], "-r")) {      /* another kernel's limit: zmalloc-host MODE -r N ... */
        reg_max = atoi(argv[3]);
        if (reg_max < 1 || reg_max > NREG) return 2;
        argv[3] = argv[1]; argv += 2; argc -= 2;
    }
    if (!strcmp(argv[1], "replay")) {
        int r = 0;
        for (int i = 2; i < argc; i++) r |= replay(argv[i]);
        return r || bad;
    }
    if (!strcmp(argv[1], "stress")) return stress(argc > 2 ? (uint32_t)strtoul(argv[2], 0, 0) : 1);
    if (!strcmp(argv[1], "speed")) return speed(argc - 2, argv + 2);
    if (!strcmp(argv[1], "room")) {                 /* how much the 8 regions hold in blocks of SIZE */
        uint32_t n = argc > 2 ? (uint32_t)atoi(argv[2]) : 100, t = 0;
        while (t < 0x10000000u && zm_malloc(n)) t += n;
        printf("room: %u KB in blocks of %u bytes, %u regions\n", t / 1024, n, nreg);
        return 0;
    }
    if (!strcmp(argv[1], "bad")) return badtest();
    return 2;
}
