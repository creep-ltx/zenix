/* SPDX-License-Identifier: GPL-2.0-only */
/* malloc, free, calloc, realloc: libz's allocator, one source for libz and the C library (user/zshare.h) */
#include "zshare.h"

/* ---------------------------------------------------------------- memory
 * malloc over the kernel's SYS_memalloc, which gives a process at most NREG (8) extra regions, so small blocks must
 * not use one each. Every block has an 8-byte header: its capacity (the bytes after the header) and a tag saying
 * whether it is a small block in use, a big block in use or a free block.
 *  - Small blocks hold 16, 32, 64 ... 32768 bytes (size classes; the header comes on top, so a request of exactly a
 *    power of two, as buffers and doubling arrays usually are, fills its class). They are cut from chunks the kernel
 *    gives: the first is 4 KB (a program that needs little, such as login waiting for its shell, holds little), each
 *    next one four times the last, up to 512 KB (smaller if memory is short), so a program that needs a lot still
 *    uses few regions (4 + 16 + 64 + 256 + 512 KB in five). A freed small block goes on its class's free list and is used again;
 *    it is never given back to the kernel (that happens when the program ends). O(1), no coalescing; a class that is
 *    empty carves a block of a larger class before taking new chunk space.
 *  - Bigger blocks get a region of their own, and free() gives exactly those back with SYS_memfree.
 * free() and realloc() check the tag: a pointer malloc did not give out (or one freed twice) is reported on
 * standard error and ignored instead of corrupting the lists. */
#define M_SMALL   0x5a4d5300u           /* "ZMS" + class: a small block in use */
#define M_BIG     0x5a4d4247u           /* "ZMBG": a block in its own region */
#define M_FREE    0x5a4d4652u           /* "ZMFR": on a free list */
#define NCLASS    12                    /* capacities 16 << 0 .. 16 << 11 = 32 KB */
#define CAP(k)    (16u << (k))
#define BLK(k)    (CAP(k) + 8u)         /* a class's block: capacity + header */
#define CHUNK_MIN 4096u
#define CHUNK_MAX (512u * 1024)
#define MALLOC_MAX 0x3FFFFFF0u          /* a request above this is refused (no size arithmetic below can wrap) */
struct mhdr { u32 cap, tag; };
static struct mhdr *freel[NCLASS];      /* free blocks; the next pointer is the first word after the header */
static char *chunk; static u32 left;    /* the unused end of the newest chunk */
static u32 next_chunk = CHUNK_MIN;

static void mfree_put(struct mhdr *h, int k)
{
    h->cap = CAP(k);
    h->tag = M_FREE;
    *(struct mhdr **)(h + 1) = freel[k];
    freel[k] = h;
}
/* memory that is not needed now (the end of a chunk, the rest of a carved block): cut it into free blocks of the
 * largest classes that fit, so none of it is lost */
static void carve(char *p, u32 n)
{
    while (n >= BLK(0)) {
        int k = NCLASS - 1;
        while (BLK(k) > n) k--;
        mfree_put((struct mhdr *)p, k);
        p += BLK(k); n -= BLK(k);
    }
}
static struct mhdr *small_get(int k)
{
    u32 sz = BLK(k);
    struct mhdr *h = freel[k];
    if (h) { freel[k] = *(struct mhdr **)(h + 1); return h; }
    if (left < sz) {
        for (int j = k + 1; j < NCLASS; j++)            /* a free block of a larger class: take ours from its start */
            if (freel[j]) {
                h = freel[j]; freel[j] = *(struct mhdr **)(h + 1);
                carve((char *)h + sz, BLK(j) - sz);
                return h;
            }
        u32 want = next_chunk;
        while (want < sz) want *= 2;
        char *c;
        while (!(c = sys_memalloc(want)) && want / 2 >= sz) want /= 2;  /* smaller if memory is short */
        if (!c && !(c = sys_memalloc(want = sz))) return 0;             /* last try: just this block */
        carve(chunk, left);                             /* the old chunk's end, before it is forgotten */
        chunk = c; left = want;
        if (want >= next_chunk) next_chunk = want < CHUNK_MAX / 4 ? want * 4 : CHUNK_MAX;   /* not after a short one */
    }
    h = (struct mhdr *)chunk; chunk += sz; left -= sz;
    return h;
}
void *malloc(zs_size n)
{
    if (n > MALLOC_MAX) return 0;
    struct mhdr *h;
    if (n <= CAP(NCLASS - 1)) {
        int k = 0;
        while (CAP(k) < n) k++;
        if (!(h = small_get(k))) return 0;
        h->cap = CAP(k);
        h->tag = M_SMALL | (u32)k;
    } else {
        u32 need = (n + sizeof *h + 15) & ~15u;
        if (!(h = sys_memalloc(need))) return 0;
        h->cap = need - sizeof *h;
        h->tag = M_BIG;
    }
    return h + 1;
}
void zs_badptr(const char *who, const void *p)
{
#ifdef ZENIX_LIBC
    char b[96], *o = b;
    for (const char *t = ZS_LIB ": "; *t; ) *o++ = *t++;
    while (*who && o < b + 24) *o++ = *who++;
    for (const char *t = "() of a pointer malloc did not give out (or freed twice): 0x"; *t; ) *o++ = *t++;
    for (int k = 28; k >= 0; k -= 4) *o++ = "0123456789abcdef"[((u32)p >> k) & 15];
    *o++ = '\n';
    sys_write(2, b, (u32)(o - b));
#else
    fprintf(2, ZS_LIB ": %s() of a pointer malloc did not give out (or freed twice): %p\n", who, p);
#endif
}
/* the header of a block malloc gave out, or 0 (reported) */
static struct mhdr *mhdr_of(void *p, const char *who)
{
    struct mhdr *h = (struct mhdr *)p - 1;
    if (((u32)p & 7) || !(h->tag == M_BIG || (h->tag & ~0xFFu) == M_SMALL)) {
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
void free(void *p)
{
    if (!p) return;
    struct mhdr *h = mhdr_of(real_hdr(p) + 1, "free");
    if (!h) return;
    if (h->tag == M_BIG) { h->tag = M_FREE; sys_memfree(h); }
    else mfree_put(h, (int)(h->tag & 0xFF));
}
/* realloc: in place when the block is already big enough, else a new block with the old contents (only as many
 * bytes as the old block holds) and the old one freed. On failure the old block is left as it was. */
/* zeroed n * size bytes; NULL when the product overflows or there is no memory (OpenSSH's bcrypt_pbkdf, D21) */
void *calloc(zs_ulong n, zs_ulong size)
{
    if (size && n > 0xFFFFFFFFu / size) return 0;
    void *p = malloc((u32)(n * size));
    if (p) memset(p, 0, n * size);
    return p;
}
void *realloc(void *p, zs_ulong n)
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
    if (!h) return 0;
    if (n <= h->cap) return p;
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
void *memalign(zs_ulong align, zs_ulong n)
{
    if (align <= 8) return malloc((zs_size)n);
    if (align & (align - 1) || n > MALLOC_MAX - align - 8) return 0;
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
