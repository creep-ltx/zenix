#include "kernel.h"

/* Memory manager. No MMU, so every byte is physical and nothing can be
 * moved once handed out: fragmentation is the enemy. Rules:
 *   - chip and fast RAM are separate pools; fast is preferred
 *   - big blocks are cut from the HIGH end of a free block, small ones
 *     from the low end, so the two kinds do not interleave
 *   - every block records its owner (a process id), so a dying or killed
 *     process gets everything back with mem_free_owner()
 * Each range is a run of contiguous blocks; a 16-byte header gives the
 * size (used flag in bit 0), the size of the block before it, the owner
 * and a magic value that catches overruns and double frees. */

#define MAGIC   0x5a4d454du         /* "ZMEM" */
#define HDR     16u
#define BIG     (16u * 1024)

struct mblk {
    u32 size;                       /* whole block incl. header; bit 0 = used */
    u32 prev;                       /* size of previous block, 0 for the first */
    u32 owner;
    u32 magic;
};

struct range { u32 lo, hi; int chip; };
static struct range ranges[16];
static int nranges;
u32 boot_ram_pa, boot_ram_size;
static u32 freeb[2];                    /* free block bytes: [0] fast, [1] chip */

#define SZ(b)   ((b)->size & ~1u)
#define USED(b) ((b)->size & 1u)

static struct mblk *next_of(struct range *r, struct mblk *b)
{
    u32 n = (u32)b + SZ(b);
    return n < r->hi ? (struct mblk *)n : 0;
}

static void add_range(u32 lo, u32 hi, int chip)
{
    lo = (lo + 15) & ~15u;
    hi &= ~15u;
    if (hi <= lo + 2 * HDR || nranges == 16)
        return;
    struct mblk *b = (struct mblk *)lo;
    b->size = hi - lo;
    b->prev = 0;
    b->owner = 0;
    b->magic = MAGIC;
    ranges[nranges++] = (struct range){ lo, hi, chip };
    freeb[chip] += hi - lo;
}

/* Add [lo,hi) minus up to two excluded spans */
static void add_minus(u32 lo, u32 hi, int chip, const u32 ex[][2], int nex)
{
    for (int i = 0; i < nex; i++) {
        u32 a = ex[i][0], b = ex[i][1];
        if (b <= lo || a >= hi)
            continue;
        if (a > lo)
            add_minus(lo, a, chip, ex + i + 1, nex - i - 1);
        if (b < hi)
            add_minus(b, hi, chip, ex + i + 1, nex - i - 1);
        return;
    }
    add_range(lo, hi, chip);
}

void mem_init(const struct bootinfo *bi, u32 kernel_pa, u32 kernel_mem)
{
    /* low chip RAM (vectors, bootinfo, bootblock scratch) and the kernel */
    u32 ex[3][2] = { { 0, 0x2000 }, { kernel_pa, kernel_pa + kernel_mem },
                     { bi->ram_pa, bi->ram_pa + bi->ram_size } };
    int nex = bi->ram_size ? 3 : 2;
    boot_ram_pa = bi->ram_pa;
    boot_ram_size = bi->ram_size;
    for (int i = 0; i < bi->nregions; i++) {
        const struct bootregion *r = &bi->r[i];
        if (r->attrs & (MEMF_CHIP | MEMF_FAST))
            add_minus(r->lower, r->upper, (r->attrs & MEMF_CHIP) != 0, ex, nex);
    }
}

/* A header that is not ours, or a size that does not lead to the next block inside the range, means something
 * wrote over the heap: stop with the address on the serial port instead of walking garbage (a size of 0 would loop
 * forever in the kernel, which nothing can interrupt). */
static void check_blk(const struct range *r, const struct mblk *b)
{
    if (b->magic != MAGIC || SZ(b) < HDR || (SZ(b) & 15) || SZ(b) > r->hi - (u32)b) {
        kprintf("mem: corrupt block header at %p (size %x, magic %x)\n", (u32)b, b->size, b->magic);
        panic("mem: heap corrupt");
    }
}

static void *alloc_in(struct range *r, u32 need, u32 owner)
{
    for (struct mblk *b = (struct mblk *)r->lo; b; b = next_of(r, b)) {
        check_blk(r, b);
        if (USED(b) || SZ(b) < need)
            continue;
        u32 left = SZ(b) - need;
        struct mblk *use = b, *nx;
        if (left < 2 * HDR) {                   /* too small to split */
            freeb[r->chip] -= SZ(b);
            b->size |= 1;
            b->owner = owner;
            return b + 1;
        }
        freeb[r->chip] -= need;
        if (need >= BIG) {                      /* big: cut from the high end */
            use = (struct mblk *)((u32)b + left);
            b->size = left;
            use->size = need | 1;
            use->prev = left;
            use->owner = owner;
            use->magic = MAGIC;
            nx = next_of(r, use);
            if (nx)
                nx->prev = need;
        } else {                                /* small: from the low end */
            struct mblk *rest = (struct mblk *)((u32)b + need);
            rest->size = left;
            rest->prev = need;
            rest->owner = 0;
            rest->magic = MAGIC;
            b->size = need | 1;
            b->owner = owner;
            nx = next_of(r, rest);
            if (nx)
                nx->prev = left;
        }
        return use + 1;
    }
    return 0;
}

void *mem_alloc(u32 n, u32 flags, u32 owner)
{
    if (n == 0 || n > MEM_ALLOC_MAX)            /* the size arithmetic below cannot wrap */
        return 0;
    u32 need = ((n + 15) & ~15u) + HDR;
    for (int pass = 0; pass < 2; pass++) {
        /* pass 0: the wanted pool; pass 1: the other, unless chip-only */
        int want_chip = (flags & MEM_CHIP) ? 1 : 0;
        if (pass == 1) {
            if (flags & (MEM_CHIP | MEM_FASTONLY))
                break;
            want_chip = 1;
        }
        for (int i = 0; i < nranges; i++) {
            if (ranges[i].chip != want_chip)
                continue;
            void *p = alloc_in(&ranges[i], need, owner);
            if (p)
                return p;
        }
    }
    return 0;
}

static struct range *range_of(u32 a)
{
    for (int i = 0; i < nranges; i++)
        if (a >= ranges[i].lo && a < ranges[i].hi)
            return &ranges[i];
    return 0;
}

/* free a block, merge with free neighbours; returns the merged block */
static struct mblk *free_blk(struct range *r, struct mblk *b)
{
    freeb[r->chip] += SZ(b);
    b->size &= ~1u;
    b->owner = 0;
    struct mblk *nx = next_of(r, b);
    if (nx && !USED(nx)) {                      /* merge forward */
        b->size += SZ(nx);
        nx = next_of(r, b);
        if (nx)
            nx->prev = SZ(b);
    }
    if (b->prev) {                              /* merge backward */
        struct mblk *pv = (struct mblk *)((u32)b - b->prev);
        if (!USED(pv)) {
            pv->size += SZ(b);
            nx = next_of(r, pv);
            if (nx)
                nx->prev = SZ(pv);
            b = pv;
        }
    }
    return b;
}

void mem_free(void *p)
{
    if (!p)
        return;
    struct mblk *b = (struct mblk *)p - 1;
    struct range *r = range_of((u32)b);
    if (!r || b->magic != MAGIC || !USED(b)) {
        kprintf("mem_free: bad or double free of %p\n", (u32)p);
        panic("mem_free: bad or double free");
    }
    check_blk(r, b);
    free_blk(r, b);
}

u32 mem_free_owner(u32 owner)
{
    u32 got = 0;
    for (int i = 0; i < nranges; i++) {
        struct range *r = &ranges[i];
        struct mblk *b = (struct mblk *)r->lo;
        while (b) {
            check_blk(r, b);
            if (USED(b) && b->owner == owner) {
                got += SZ(b) - HDR;
                b = free_blk(r, b);
            }
            b = next_of(r, b);
        }
    }
    return got;
}

void mem_stats(u32 *chip_free, u32 *fast_free)
{
    *chip_free = freeb[1];
    *fast_free = freeb[0];
}

void *kmalloc(u32 n) { return mem_alloc(n, 0, 0); }
void kfree(void *p) { mem_free(p); }
