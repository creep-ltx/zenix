/* SPDX-License-Identifier: GPL-2.0-only */
/* malloc, free, calloc, realloc: libz's allocator, one source for libz and the C library (user/zshare.h); the
 * policy, decided once for every program, and its measurements: docs/d27-c-library.md "The allocator" */
#include "zshare.h"

/* ---------------------------------------------------------------- memory
 * malloc over the kernel's SYS_memalloc, which gives a process at most 16 regions (kernel/kernel.h's NREG), so most
 * blocks share regions ("chunks"). Every block has an 8-byte header: its capacity (the bytes after it, a multiple
 * of 8, at least 16) and a tag; blocks are 8-aligned. A request is rounded up to 8 bytes (up to 256: to 16), never
 * to a power of two.
 *  - The blocks of a chunk lie end to end, an end mark after the last. A free one is in the bin of its size (bins
 *    by powers of two, a bit map of the bins that hold some), its capacity repeated in its last word, and the block
 *    after it carries M_PF, so free() joins a block with both neighbours at once. malloc takes the best of the first
 *    eight that fit in its own bin, else the first block of the next bin that holds one, and leaves the rest free.
 *  - Quick lists, the hot path (list nodes, words, names, buffers): a freed small block (at most 256 bytes) goes on
 *    the list of its size (16-byte steps; malloc takes it back, or one 16 bytes bigger), a medium one (up to 8 KB) on
 *    its bin's (malloc takes the head when it is big enough, else the next bin's), O(1); a small free is a push and
 *    a look at the bit map, nothing more. Quick blocks stay blocks of their chunk: all go back to their chunks,
 *    joined, before the heap takes a new chunk, when the medium lists hold more than 32 KB, or when the heap has a
 *    free block of 64 KB or more (memory a program gave back: quick blocks would keep its chunks from going back).
 *  - A new chunk is at least the request with its headers; else 4 KB times 1.41 for every region the allocator
 *    holds (4, 6, 8, 12, 16 ... KB, plus 64 bytes so that two 4 KB blocks fit 8 KB's), and from 10 regions on at
 *    least the heap so far (so the last regions reach megabytes), at most 4 MB; smaller when memory is short.
 *  - A request of 8 KB or more gets a region of its own while the allocator holds fewer than 10 (6 stay for the
 *    heap and the program's own SYS_memalloc); free() gives it back at once.
 *  - A chunk whose blocks are all free again (after its quick blocks went back) goes back to the kernel, except
 *    the first one. (A free/malloc see-saw stays on the quick lists: it never empties a chunk.)
 * free() and realloc() check the tag: a pointer malloc did not give out (or one freed twice) is reported on
 * standard error and ignored. A header found damaged on the way (a program wrote past its block) stops the program
 * with status 134 and that header's address: the lists cannot be trusted any more. */
#define M_USE     0x5a4d5500u           /* "ZMU" + M_PF: a chunk's block in use */
#define M_QFREE   0x5a4d5100u           /* "ZMQ" + M_PF: on a quick list (in use, as far as the chunk is concerned) */
#define M_HFREE   0x5a4d4600u           /* "ZMF": free, on the free list (never M_PF: free neighbours are joined) */
#define M_END     0x5a4d4500u           /* "ZME" + M_PF: the end of a chunk */
#define M_BIG     0x5a4d4247u           /* "ZMBG": a block in its own region */
#define M_PF      1u                    /* the block before this one is free */
#define KIND(t)   ((t) & ~0xFFu)
#define SMALL_MAX 256u
#define OWN_MIN   (8u * 1024)
#define ZM_NREG   16u                   /* kernel/kernel.h's NREG (tools/zmalloc-test.sh checks it) */
#define OWN_REGS  (ZM_NREG * 5 / 8)     /* own regions only while the allocator holds fewer regions than this */
#define BIG_REGS  (ZM_NREG * 5 / 8)     /* from this many regions held, a new chunk is at least the heap so far */
#define CHUNK_MIN 4096u
#define CHUNK_MAX (4u << 20)
#define QUICK_MAX (32u * 1024)          /* more bytes than this on the medium lists: they all go back to their chunks */
#define MALLOC_MAX 0x3FFFFFF0u          /* a request above this is refused (no size arithmetic below can wrap) */
struct mhdr { u32 cap, tag; };
struct fblk { u32 cap, tag; struct fblk *next, *prev; };        /* a free block of a chunk */
struct chunk { struct chunk *next; u32 size; };                 /* a chunk's first 8 bytes: then its blocks */
#ifdef ZMALLOC_TRACE
/* A diagnostic build only (UDIAG=-DZMALLOC_TRACE, docs/d27-c-library.md "The allocator"; never shipped): the programs
 * named below append every call to /var/mt/NAME.PID as a line of hex ("m size block", "f block", "r old size new",
 * "a align size block"; the region heap's "h size block" and "g block"; "n" and "o" were netd's heap's, recorded
 * once with a call to zs_mtrace there), opened, written and closed each time (no descriptor kept, none taken from
 * the program). tools/zmalloc-host.c replays the files. */
extern char **_zargv;
static char mt_path[48]; static int mt_state;     /* 0 not decided, 1 traced, 2 not */
static int mt_depth;                    /* realloc and calloc call malloc: one line for the outer call */
void zs_mtrace(char op, u32 a, u32 b, u32 c, int n)
{
    if (!mt_state) {
        const char *nm = _zargv && _zargv[0] ? _zargv[0] : "", *t;
        for (t = nm; *t; t++) if (*t == '/') nm = t + 1;
        if (*nm == '-') nm++;
        static const char *const want[] = { "ahiru", "sumi", "maki", "markdown", "sh", "netd", "wasabid", "nfscd",
                                            "ctop", 0 };
        mt_state = 2;
        for (int i = 0; want[i]; i++) if (!strcmp(nm, want[i])) mt_state = 1;
        if (mt_state == 2) return;
        char *o = mt_path; u32 pid = (u32)sys_getpid();
        for (t = "/var/mt/"; *t; ) *o++ = *t++;
        for (t = nm; *t && o < mt_path + 36; ) *o++ = *t++;
        *o++ = '.';
        char d[8]; int k = 0;
        do d[k++] = (char)('0' + pid % 10); while ((pid /= 10) && k < 7);
        while (k) *o++ = d[--k];
        *o = 0;
    }
    if (mt_state != 1) return;
    char b2[40], *o = b2; u32 v[3] = { a, b, c };
    *o++ = op;
    for (int i = 0; i < n; i++) {
        *o++ = ' ';
        int k = 28; while (k > 0 && !((v[i] >> k) & 15)) k -= 4;
        for (; k >= 0; k -= 4) *o++ = "0123456789abcdef"[(v[i] >> k) & 15];
    }
    *o++ = '\n';
    int fd = sys_open(mt_path, O_WRONLY | O_CREAT | O_APPEND);
    if (fd >= 0) { sys_write(fd, b2, (u32)(o - b2)); sys_close(fd); }
}
#define ZM_API   static                /* the calls themselves: below, wrapped by the ones that write the lines */
static void *malloc1(zs_size n);
#define MT_IN()  (mt_depth++)
#define MT_OUT() (mt_depth--)
#define MT(op, a, b, c, n) do { if (!mt_depth) zs_mtrace(op, (u32)(a), (u32)(b), (u32)(c), n); } while (0)
#else
#define ZM_API                          /* the calls themselves are malloc, realloc, memalign */
#define malloc1 malloc
#define realloc1 realloc
#define memalign1 memalign
#define MT_IN()
#define MT_OUT()
#define MT(op, a, b, c, n)
#endif
#define NBIN 24                         /* free blocks by size: bin i holds capacities 16 << i up to twice that */
#define MED_MAX   8191u                 /* medium blocks (above SMALL_MAX): quick lists by bin, 256 .. 8191 */
#define MQ(cap)   (SMALL_MAX / 16 - 3 + binof(cap))     /* a medium block's quick list: 17 .. 21 */
#define NQUICK    (SMALL_MAX / 16 + 6)
#define BIN_LARGE 12                    /* bins from here on hold free blocks of 64 KB and more */
/* the allocator's state in one place: each function reaches it through one address register (zm), shorter code and
 * fewer relocations in every program than an absolute address for each */
static struct zmstate {
    struct mhdr *quick[NQUICK];         /* small: by capacity / 16; medium: by bin (MQ); the next pointer is the first
                                         * word after the header */
    u32 qbytes;                         /* the bytes on the medium quick lists */
    struct fblk *bin[NBIN];
    u32 binmap;                         /* bit i: bin i is not empty */
    struct chunk *chunks, *first;       /* the chunks held (newest first); the first one, never given back */
    u32 nchunks, nown, heapsz;          /* chunks held, own regions held, the chunks' bytes */
    struct fblk *fresh;                 /* the newest chunk's untouched rest (free, never used): not memory given
                                         * back (audit B3-11) */
} zms;
static inline struct zmstate *zmp(void)
{
    struct zmstate *z = &zms;
#ifdef __m68k__
    __asm__("" : "+a"(z));              /* (kept in an address register, not folded back into absolute addresses) */
#else
    __asm__("" : "+r"(z));              /* (the PC's test build, tools/zmalloc-test.sh) */
#endif
    return z;
}
#define ZM struct zmstate *zm = zmp()
#define quick   (zm->quick)
#define qbytes  (zm->qbytes)
#define bin     (zm->bin)
#define binmap  (zm->binmap)
#define chunks  (zm->chunks)
#define first   (zm->first)
#define nchunks (zm->nchunks)
#define nown    (zm->nown)
#define heapsz  (zm->heapsz)
#define fresh   (zm->fresh)

#define NEXTB(h)  ((struct mhdr *)((char *)(h) + 8 + (h)->cap))
/* the quick lists are the hot path (-O2, as the rest of libz); the chunks' code is small instead (-Os) */
#define COLD      __attribute__((optimize("Os"), noinline))
static u32 capof(zs_size n) { return n <= 16 ? 16 : ((u32)n + 7) & ~7u; }

/* a report on standard error: libz's formatter; the C library's without it (a program that only allocates links no
 * printf) */
COLD static void say(const char *who, const char *what, const void *p)
{
#ifndef ZENIX_LIBC
    dprintf(2, ZS_LIB ": %s%s%08x\n", who, what, (u32)p);
#else
    char b[120], *o = b;
    for (const char *t = ZS_LIB ": "; *t; ) *o++ = *t++;
    while (*who && o < b + 24) *o++ = *who++;
    while (*what) *o++ = *what++;
    for (int k = 28; k >= 0; k -= 4) *o++ = "0123456789abcdef"[((u32)p >> k) & 15];
    *o++ = '\n';
    sys_write(2, b, (u32)(o - b));
#endif
}
void zs_badptr(const char *who, const void *p)
{
    say(who, "() of a pointer malloc did not give out (or freed twice): 0x", p);
}
static void damaged(const void *h) __attribute__((noreturn));
COLD static void damaged(const void *h)
{
    say("malloc", ": a damaged block header (written over) at 0x", h);
    sys_exit(134);
    for (;;);
}

static inline u32 binof(u32 cap)
{
    u32 i = 27u - (u32)__builtin_clz(cap);  /* 16..31 -> 0, 32..63 -> 1 ... */
    return i < NBIN ? i : NBIN - 1;
}
COLD static void fl_del(struct fblk *b)
{
    ZM;
    if (b == fresh) fresh = 0;          /* (joined with a freed block, or used) */
    if (b->prev) b->prev->next = b->next;
    else {
        u32 i = binof(b->cap);
        if (!(bin[i] = b->next)) binmap &= ~(1u << i);
    }
    if (b->next) b->next->prev = b->prev;
}
/* h becomes a free block of capacity cap: on the list, its last word cap, the block after it told */
COLD static void mkfree(struct mhdr *h, u32 cap)
{
    ZM;
    struct fblk *b = (struct fblk *)h;
    b->cap = cap;
    b->tag = M_HFREE;
    ((u32 *)((char *)h + 8 + cap))[-1] = cap;
    NEXTB(h)->tag |= M_PF;
    u32 i = binof(cap);
    b->prev = 0;
    if ((b->next = bin[i])) b->next->prev = b;
    bin[i] = b;
    binmap |= 1u << i;
}
/* a block of the chunk given back: joined with free neighbours; a chunk wholly free goes back to the kernel */
COLD static void heap_put(struct mhdr *h)
{
    ZM;
    u32 cap = h->cap;
    struct mhdr *n = NEXTB(h);
    u32 k = KIND(n->tag);
    if (k != M_USE && k != M_QFREE && k != M_END && k != M_HFREE) damaged(n);
    if (k == M_HFREE) { fl_del((struct fblk *)n); cap += 8 + n->cap; }
    if (h->tag & M_PF) {
        u32 pc = ((u32 *)h)[-1];
        struct mhdr *pv = (struct mhdr *)((char *)h - 8 - pc);
        if (pc & 7 || pv->tag != M_HFREE || pv->cap != pc) damaged(h);
        fl_del((struct fblk *)pv);
        cap += 8 + pc;
        h = pv;
    }
    h->cap = cap;
    if (KIND(NEXTB(h)->tag) == M_END)
        for (struct chunk **cp = &chunks, *c; (c = *cp); cp = &c->next)
            if ((struct mhdr *)(c + 1) == h) {
                if (c == first) break;
                *cp = c->next;
                nchunks--;
                heapsz -= c->size;
                sys_memfree(c);
                return;
            }
    h->tag = M_USE;                     /* (no free block before it now) */
    mkfree(h, cap);
}
/* memory given back: a free block of 64 KB or more other than the newest chunk's untouched rest (a heap that has just
 * grown has that one; flushing the quick lists at every free for it kept them empty and every malloc on the slow path
 * until the chunk was used up, 7 times slower: audit B3-11) */
static inline int given_back(void)
{
    ZM;
    u32 m = binmap >> BIN_LARGE, i;
    struct fblk *f = fresh;
    if (!m) return 0;
    if (!f || (i = binof(f->cap)) < BIN_LARGE) return 1;
    return m != 1u << (i - BIN_LARGE) || bin[i] != f || f->next;
}
/* every quick block back into its chunk, joined (before the heap grows) */
static int qany(void)                   /* a block on a quick list */
{
    ZM;
    for (u32 i = 0; i < NQUICK; i++)
        if (quick[i]) return 1;
    return 0;
}
COLD static void quick_flush(void)
{
    ZM;
    for (u32 i = 0; i < NQUICK; i++)
        while (quick[i]) {
            struct mhdr *h = quick[i];
            quick[i] = *(struct mhdr **)(h + 1);
            if (KIND(h->tag) != M_QFREE) damaged(h);
            heap_put(h);
        }
    qbytes = 0;
}
/* a free block for cap bytes, 0 if none: the best of the first eight that fit in cap's own bin, else the first block
 * of the next bin that is not empty (every block there fits) */
COLD static struct fblk *fit(u32 cap)
{
    ZM;
    u32 i = binof(cap), m = binmap >> i << i, n = 8;
    struct fblk *b = 0;
    for (struct fblk *t = bin[i]; t && n; t = t->next)
        if (t->cap >= cap && (!b || t->cap < b->cap)) { b = t; n--; }
    if (!b && (m &= ~(1u << i))) b = bin[31u - (u32)__builtin_clz(m & -m)];
    if (b && b->tag != M_HFREE) damaged(b);
    return b;
}
/* take cap bytes from the free block b; the rest (if it can hold a free block) stays free */
COLD static struct mhdr *take(struct fblk *b, u32 cap)
{
    ZM;
    struct mhdr *h = (struct mhdr *)b;
    int wasfresh = b == fresh;
    fl_del(b);
    if (b->cap - cap >= 24) {
        u32 rest = b->cap - cap - 8;
        h->cap = cap;
        mkfree(NEXTB(h), rest);
        if (wasfresh) fresh = (struct fblk *)NEXTB(h);     /* the untouched rest is still untouched */
    } else
        NEXTB(h)->tag &= ~M_PF;
    h->tag = M_USE;
    return h;
}
COLD static struct mhdr *heap_get(u32 cap)
{
    ZM;
    struct mhdr *h;
    if (cap >= OWN_MIN && nown + nchunks < OWN_REGS && (h = sys_memalloc((cap + 8 + 15) & ~15u))) {
        h->cap = ((cap + 8 + 15) & ~15u) - 8;       /* a region of its own */
        h->tag = M_BIG;
        nown++;
        return h;
    }
    struct fblk *b = fit(cap);
    if (!b && qany()) { quick_flush(); b = fit(cap); }
    if (b) return take(b, cap);
    /* the chunk's own 8 bytes, the block's header, the end mark; 4 KB times 1.41 for each chunk held (4, 6, 8, 12,
     * 16, 24 KB ...), and from BIG_REGS regions on at least the heap so far, so the last regions reach megabytes */
    u32 least = cap + 24, k = nchunks + nown < 20 ? nchunks + nown : 20;
    u32 want = (CHUNK_MIN << k / 2) / 2 * (k & 1 ? 3 : 2) + 64;     /* (+64: two 4 KB blocks fit 8 KB's chunk) */
    if (nchunks + nown >= BIG_REGS && heapsz > want) want = heapsz;
    if (want > CHUNK_MAX) want = CHUNK_MAX;
    if (want < least) want = (least + 15) & ~15u;
    struct chunk *c;
    while (!(c = sys_memalloc(want)) && want / 2 >= least) want /= 2;      /* smaller if memory is short */
    if (!c && !(c = sys_memalloc(want = least))) return 0;
    c->size = want;
    c->next = chunks;
    chunks = c;
    if (!first) first = c;
    nchunks++;
    heapsz += want;
    h = (struct mhdr *)(c + 1);
    struct mhdr *end = (struct mhdr *)((char *)c + want - 8);
    end->cap = 0;
    end->tag = M_END;
    h->tag = M_USE;
    mkfree(h, want - 24);
    fresh = (struct fblk *)h;
    return take((struct fblk *)h, cap);
}
/* not a small block on its quick lists: a medium one from its bin's list (the head if it is big enough, else the
 * next bin's: every block there is), else the heap */
static struct mhdr *slow_get(u32 cap)
{
    ZM;
    struct mhdr *h;
    if (cap > SMALL_MAX && cap <= MED_MAX) {
        u32 i = MQ(cap);
        if (((h = quick[i]) && h->cap >= cap) || (i < NQUICK - 1 && (h = quick[++i]))) {
            quick[i] = *(struct mhdr **)(h + 1);
            qbytes -= h->cap;
            h->tag = M_USE | (h->tag & M_PF);
            return h;
        }
    }
    return heap_get(cap);
}
ZM_API void *malloc1(zs_size n)
{
    ZM;
    struct mhdr *h;
    if (n <= SMALL_MAX) {               /* a quick list: this size, or 16 bytes more */
        u32 i = n <= 16 ? 1 : ((u32)n + 15) >> 4;
        if ((h = quick[i]) || (i < SMALL_MAX / 16 && (h = quick[++i]))) {
            quick[i] = *(struct mhdr **)(h + 1);
            h->tag = M_USE | (h->tag & M_PF);
            return h + 1;
        }
        h = slow_get((n <= 16 ? 1 : ((u32)n + 15) >> 4) << 4);
    } else if (n > MALLOC_MAX)
        return 0;
    else
        h = slow_get(((u32)n + 7) & ~7u);
    return h ? h + 1 : 0;
}
#ifdef ZMALLOC_TRACE
void *malloc(zs_size n)
{
    void *p = malloc1(n);
    MT('m', n, p, 0, 2);
    return p;
}
#endif
/* the header of a block malloc gave out (in use), or 0 (reported) */
static struct mhdr *mhdr_of(void *p, const char *who)
{
    struct mhdr *h = (struct mhdr *)p - 1;
    if (((u32)p & 7) || !((h->tag & ~M_PF) == M_USE || h->tag == M_BIG)) {
        zs_badptr(who, p);
        return 0;
    }
    return h;
}
#ifdef ZENIX_LIBC
/* memalign's places (below, the C library's only: libz has no memalign, so its free stays as it was): an M_ALIGNED
 * header in front of the place says how far back its block starts */
#define M_ALIGNED 0x5a4d414cu           /* "ZMAL": an aligned place inside another block */
static struct mhdr *real_hdr(void *p)
{
    struct mhdr *h = (struct mhdr *)p - 1;
    return h->tag == M_ALIGNED && !((u32)p & 7) ? (struct mhdr *)((char *)p - h->cap) - 1 : h;
}
#else
#define real_hdr(p) ((struct mhdr *)(p) - 1)
#endif
COLD static void own_free(struct mhdr *h)
{
    ZM;
    h->tag = 0;
    nown--;
    sys_memfree(h);
}
COLD static void free_blk(struct mhdr *h)
{
    ZM;
    if (h->tag == M_BIG) { own_free(h); return; }
    if (h->cap > SMALL_MAX && h->cap <= MED_MAX) {      /* a medium block: its bin's quick list */
        u32 i = MQ(h->cap);
        h->tag = M_QFREE | (h->tag & M_PF);
        *(struct mhdr **)(h + 1) = quick[i];
        quick[i] = h;
        /* back to the chunks when the lists grow big, or when the heap has a large free block (memory a program
         * gave back: its quick blocks would keep chunks from going back to the kernel) */
        if ((qbytes += h->cap) > QUICK_MAX || given_back()) quick_flush();
        return;
    }
    heap_put(h);                        /* (a small one here is a memalign place's block: straight back) */
}
COLD static void free_slow(void *p)
{
    struct mhdr *h = mhdr_of(real_hdr(p) + 1, "free");
    if (!h) return;
    free_blk(h);
    if (given_back()) quick_flush();             /* a free block of 64 KB or more: the quick blocks back too */
}
/* a medium block onto its bin's quick list (free's, kept out of free itself: the hot path stays short, as the
 * 68020's 256-byte instruction cache likes it) */
__attribute__((noinline)) static void free_med(struct mhdr *h)
{
    ZM;
    u32 i = MQ(h->cap);
    h->tag = M_QFREE | (h->tag & M_PF);
    *(struct mhdr **)(h + 1) = quick[i];
    quick[i] = h;
    if ((qbytes += h->cap) > QUICK_MAX || given_back()) quick_flush();
}
void free(void *p)
{
    ZM;
    struct mhdr *h = (struct mhdr *)p - 1;
    if (!p) return;
    MT('f', p, 0, 0, 1);
    if (!((u32)p & 7) && (h->tag & ~M_PF) == M_USE) {
        u32 cap = h->cap;
        if (cap <= SMALL_MAX) {         /* a small block: its quick list (its capacity is at least 16 times i) */
            u32 i = cap >> 4;
            h->tag = M_QFREE | (h->tag & M_PF);
            *(struct mhdr **)(h + 1) = quick[i];
            quick[i] = h;
            if (given_back()) quick_flush();     /* (memory given back: see quick_flush's callers) */
            return;
        }
        if (cap <= MED_MAX) { free_med(h); return; }
    }
    free_slow(p);
}
/* zeroed n * size bytes; NULL when the product overflows or there is no memory (OpenSSH's bcrypt_pbkdf, D21) */
COLD void *calloc(zs_ulong n, zs_ulong size)
{
    if (size && n > 0xFFFFFFFFu / size) return 0;
    void *p = malloc1((u32)(n * size));
    MT('m', n * size, p, 0, 2);
    if (p) memset(p, 0, n * size);
    return p;
}
/* realloc: in place when the block is big enough or the free block after it makes it big enough; else a new block
 * with the old contents (only as many bytes as the old block holds) and the old one freed. On failure the old block
 * is left as it was. */
#ifdef ZMALLOC_TRACE
static void *realloc1(void *p, zs_ulong n);
void *realloc(void *p, zs_ulong n)
{
    MT_IN();
    void *q = realloc1(p, n);
    MT_OUT();
    MT('r', p, n, q, 3);
    return q;
}
#endif
COLD ZM_API void *realloc1(void *p, zs_ulong n)
{
    if (!p) return malloc((u32)n);
#ifdef ZENIX_LIBC
    if (real_hdr(p) + 1 != p) {         /* a memalign place: a new block, as much of the old as fits */
        struct mhdr *b = real_hdr(p);
        zs_ulong have = b->cap - (u32)((char *)p - (char *)(b + 1));
        void *q = malloc((u32)n);
        if (!q) return 0;
        memcpy(q, p, have < n ? have : n);
        free(p);
        return q;
    }
#endif
    struct mhdr *h = mhdr_of(p, "realloc");
    if (!h || n > MALLOC_MAX) return 0;
    u32 cap = capof(n);
    if (h->tag != M_BIG) {
        struct mhdr *nx = NEXTB(h);
        if (cap > h->cap && nx->tag == M_HFREE && h->cap + 8 + nx->cap >= cap) {   /* the free block after it */
            ZM;
            int wasfresh = (struct fblk *)nx == fresh;
            u32 all = h->cap + 8 + nx->cap;
            fl_del((struct fblk *)nx);
            if (all - cap >= 24) {      /* only what it needs: the rest stays free (taking all of it once swallowed
                                         * a new chunk's megabytes into a 16-byte block: DESIGN D52 9, Lua) */
                h->cap = cap;
                mkfree(NEXTB(h), all - cap - 8);
                if (wasfresh) fresh = (struct fblk *)NEXTB(h);     /* the untouched rest is still untouched */
            } else {
                h->cap = all;
                NEXTB(h)->tag &= ~M_PF;
            }
        }
    }
    if (cap <= h->cap) return p;
    void *q = malloc((u32)n);
    if (!q) return 0;
    memcpy(q, p, h->cap);
    free(p);
    return q;
}
#ifdef ZENIX_LIBC
/* What standard C and POSIX add (DESIGN D27 section 5.1). Every block is 8-aligned (its 8-byte header); a larger
 * alignment takes a block big enough to hold an aligned place with room for a header before it, and that header
 * (M_ALIGNED, its distance back to the block, defined above free) lets free() and realloc() find the block. */
#ifdef ZMALLOC_TRACE
static void *memalign1(zs_ulong align, zs_ulong n);
void *memalign(zs_ulong align, zs_ulong n)
{
    MT_IN();
    void *p = memalign1(align, n);
    MT_OUT();
    MT('a', align, n, p, 3);
    return p;
}
#endif
ZM_API void *memalign1(zs_ulong align, zs_ulong n)
{
    if (align <= 8) return malloc((zs_size)n);
    if (align > MALLOC_MAX / 2 || align & (align - 1) || n > MALLOC_MAX - align - 8) return 0;   /* (no wrap: B3-10) */
    char *b = malloc((zs_size)(n + align + 8));
    if (!b) return 0;
    char *p = (char *)(((u32)b + 8 + align - 1) & ~(u32)(align - 1));
    struct mhdr *h = (struct mhdr *)p - 1;
    h->cap = (u32)(p - b);
    h->tag = M_ALIGNED;
    return p;
}
void *aligned_alloc(zs_ulong align, zs_ulong n) { return memalign(align, n); }
int posix_memalign(void **out, zs_ulong align, zs_ulong n)
{
    if (align < sizeof(void *) || (align & (align - 1))) return EINVAL;
    void *p = memalign(align, n);
    if (!p) return ENOMEM;
    *out = p;
    return 0;
}
/* the bytes a block holds (at least what was asked for) */
zs_ulong malloc_usable_size(void *p)
{
    if (!p) return 0;
    struct mhdr *b = real_hdr(p), *h = mhdr_of(b + 1, "malloc_usable_size");
    return h ? h->cap - (u32)((char *)p - (char *)(b + 1)) : 0;
}
#endif
