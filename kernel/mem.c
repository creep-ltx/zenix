/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#include "zine.h"
#include "mmu.h"

/* Memory manager. No MMU, so every byte is physical and nothing can be
 * moved once handed out: fragmentation is the enemy. Rules:
 *   - chip and fast RAM are separate pools; fast is preferred
 *   - big blocks are cut from the HIGH end of a free block, small ones
 *     from the low end, so the two kinds do not interleave (a block that
 *     must end below a limit, MEM_LOW1G or MEM_LOW16M: always the low end)
 *   - every block records its owner (a process id), so a dying or killed
 *     process gets everything back with mem_free_owner()
 * Each range is a run of contiguous blocks; a 16-byte header gives the
 * size (used flag in bit 0), the size of the block before it, the owner
 * and a magic value that catches overruns and double frees.
 * With memory protection on (DESIGN D41, kernel/mmu.c), a MEM_USER block (a process's own memory) is whole pages:
 * its data starts on a page, its header is the last 16 bytes of the page below, so no two owners share a page. */

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

KINIT static void add_range(u32 lo, u32 hi, int chip)
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

/* Add [lo,hi) minus the excluded spans */
KINIT static void add_minus(u32 lo, u32 hi, int chip, const u32 ex[][2], int nex)
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

/* DESIGN D15: the bundle's place, checked (it comes from zenixboot, but a damaged table must not make the kernel give
 * away memory it uses): one region holds it and the 16 bytes before it (its block header), 16-byte aligned, clear of
 * low memory, the kernel, the klog area and the RAM disk. Then it is a block in use inside the pool, given back by
 * upd_release with the normal mem_free (it merges with its neighbours; no range slot is used). */
u32 upd_pa, upd_size, upd_flags;
static void *upd_blk;

static int inside(u32 a, u32 b, u32 lo, u32 hi) { return a < hi && lo < b; }

KINIT static void upd_reserve(const struct bootinfo *bi, const u32 ex[][2], int nex)
{
    u32 a = bi->upd_pa, n = bi->upd_size, lo = a - HDR, hi = (a + n + 15) & ~15u;
    const char *why = 0;
    int in = 0;
    upd_flags = bi->upd_flags;
    if (!a && !n)
        return;
    for (int i = 0; i < bi->nregions && i < 8; i++)
        if ((bi->r[i].attrs & (MEMF_CHIP | MEMF_FAST)) && bi->r[i].lower <= lo && hi <= bi->r[i].upper && lo < hi)
            in = 1;
    if ((a & 15) || !n || n > MEM_ALLOC_MAX || a < 0x2000 + HDR || hi < a)
        why = "a bad address or size";
    else if (!in)
        why = "not inside one memory region";
    for (int i = 0; !why && i < nex; i++)
        if (inside(lo, hi, ex[i][0], ex[i][1]))
            why = "it overlaps the kernel, the klog area, low memory or the RAM disk";
    struct range *r = 0;
    struct mblk *b = 0;
    for (int i = 0; !why && i < nranges; i++)
        if (lo >= ranges[i].lo && hi <= ranges[i].hi)
            r = &ranges[i];
    if (!why && r)                              /* the free block that holds it (at mem_init: the range's only one) */
        for (b = (struct mblk *)r->lo; b && !((u32)b <= lo && hi <= (u32)b + SZ(b)); b = next_of(r, b))
            ;
    if (!why && (!b || USED(b)))
        why = "not in free memory";
    if (why) {
        kprintf("update: the bundle at %p (%u bytes) is ignored: %s\n", a, n, why);
        return;
    }
    u32 end = (u32)b + SZ(b);
    if (lo - (u32)b < 2 * HDR)                  /* no room for a free block before it: the block starts at b */
        lo = (u32)b;
    if (end - hi < 2 * HDR)
        hi = end;
    struct mblk *u = (struct mblk *)lo, *nx;
    if (lo > (u32)b) {
        b->size = lo - (u32)b;
        u->prev = b->size;
    }
    u->size = (hi - lo) | 1;
    u->owner = 0;
    u->magic = MAGIC;
    if (hi < end) {
        struct mblk *t = (struct mblk *)hi;
        t->size = end - hi;
        t->prev = hi - lo;
        t->owner = 0;
        t->magic = MAGIC;
        if ((nx = next_of(r, t)))
            nx->prev = SZ(t);
    } else if ((nx = next_of(r, u)))
        nx->prev = hi - lo;
    freeb[r->chip] -= hi - lo;
    upd_blk = u + 1;
    upd_pa = a;
    upd_size = n;
    kprintf("update: a bundle of %u bytes at %p, kept for zup%s\n", n, a, upd_flags & UPD_F_ROLLBACK ? "; previous version asked for" : "");
}

__attribute__((cold, optimize("Os"))) void upd_release(void)
{
    if (upd_blk) {
        mmu_upd_gone();                 /* D41: its pages leave root's views (they were read only for zup) */
        mem_free(upd_blk);
        kprintf("update: the bundle's %u bytes are free again\n", upd_size);
    }
    upd_blk = 0;
    upd_pa = upd_size = 0;
    upd_flags &= UPD_F_TRIAL;           /* D22 4.3: zpkg asks after boot gave the bundle back */
}

/* DESIGN D45 (docs/d45-boot-memory.md): the spans link/kernel.ld puts at the end of the kernel's image - the boot's
 * code and data, the CD drive's code and memory protection's (section 13) - reserved here, before mmu_setup builds its tables over the ranges, each as
 * a range of its own holding one block in use by the kernel (its header in the span's first 32 bytes, as upd_reserve
 * makes the bundle a block), appended after the real memory's ranges; mem_boot_release gives them back. Chip or fast as
 * the region that holds the kernel. A span not reserved (no range slot) stays resident: its pointer stays 0. */
extern char __zinit_start[], __zinit_end[], __zhwcd_start[], __zhwcd_end[], __zhwmmu_start[], __zhwmmu_end[];
/* [0] the boot's, [1] the CD drive's, [2] memory protection's (D45 section 13); 0: not reserved (or given back) */
static struct mblk *span_blk[3];

KINIT void mem_init(const struct bootinfo *bi, u32 kernel_pa, u32 kernel_mem)
{
    /* low chip RAM (vectors, bootinfo, bootblock scratch) and the kernel */
    /* ... and the kernel log's area at the top of chip RAM (audit A12; 0 when the log could not be kept there) */
    u32 kla = klog_area_base();
    u32 ex[4][2] = { { 0, 0x2000 }, { kernel_pa, kernel_pa + kernel_mem },
                     { kla, kla ? kla + KLOG_AREA_SIZE : 0 }, { bi->ram_pa, bi->ram_pa + bi->ram_size } };
    int nex = bi->ram_size ? 4 : 3;
    boot_ram_pa = bi->ram_pa;
    boot_ram_size = bi->ram_size;
    for (int i = 0; i < bi->nregions; i++) {
        const struct bootregion *r = &bi->r[i];
        if (r->attrs & (MEMF_CHIP | MEMF_FAST))
            add_minus(r->lower, r->upper, (r->attrs & MEMF_CHIP) != 0, ex, nex);
    }
    upd_reserve(bi, ex, nex);
    for (int k = 0; k < 3; k++) {       /* the bounds in code: a table of them would be resident data (D45 section 5) */
        u32 lo = ((u32)(k == 0 ? __zinit_start : k == 1 ? __zhwcd_start : __zhwmmu_start) + 15) & ~15u;
        u32 hi = (u32)(k == 0 ? __zinit_end : k == 1 ? __zhwcd_end : __zhwmmu_end) & ~15u;
        int chip = -1, n = nranges;
        for (int i = 0; i < bi->nregions && i < 8; i++)
            if ((bi->r[i].attrs & (MEMF_CHIP | MEMF_FAST)) && bi->r[i].lower <= lo && hi <= bi->r[i].upper)
                chip = (bi->r[i].attrs & MEMF_CHIP) != 0;
        if (chip >= 0)
            add_range(lo, hi, chip);    /* (adds nothing if it is too small or the table is full) */
        if (nranges == n) {
            kprintf("mem: span %d at %p stays the kernel's\n", k, lo);
            continue;
        }
        span_blk[k] = (struct mblk *)lo;
        span_blk[k]->size |= 1;
        freeb[chip] -= hi - lo;
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

/* cut the used block [h, u) out of the free block b: h is b or leaves a free block of at least 2 headers before it, u
 * is b's end or leaves one of at least 2 headers after it */
__attribute__((noinline)) static void *carve(struct range *r, struct mblk *b, u32 h, u32 u, u32 owner)
{
    u32 e = (u32)b + SZ(b);
    struct mblk *use = (struct mblk *)h, *nx;
    if (h > (u32)b) {
        b->size = h - (u32)b;
        use->prev = b->size;
    }
    use->size = (u - h) | 1;
    use->owner = owner;
    use->magic = MAGIC;
    if (u < e) {
        struct mblk *t = (struct mblk *)u;
        t->size = e - u;
        t->prev = u - h;
        t->owner = 0;
        t->magic = MAGIC;
        if ((nx = next_of(r, t)))
            nx->prev = e - u;
    } else if ((nx = next_of(r, use)))
        nx->prev = u - h;
    freeb[r->chip] -= u - h;
    return use + 1;
}

/* NEED: the block's bytes with its header. LIMIT (0: none): the block must end at or below it, cut from the low end of
 * a free block (MEM_LOW1G, MEM_LOW16M). PG (DESIGN D41, a MEM_USER block with protection on): NEED is the data's
 * bytes, whole pages, the data starts on a page; its header is the last 16 bytes of the page below, which holds no
 * other such block's data (that one ends on a page at or below it). Big blocks from the high end, small ones from the
 * low end. */
__attribute__((noinline)) static void *alloc_in(struct range *r, u32 need, u32 owner, u32 limit, int pg)
{
    int top = need >= BIG && !limit;           /* from the high end (a limit: always the low end) */
    for (struct mblk *b = (struct mblk *)r->lo; b; b = next_of(r, b)) {
        check_blk(r, b);
        u32 lo = (u32)b, e = lo + SZ(b), h = lo, u;
        if (USED(b) || SZ(b) < need + (pg ? HDR : 0))
            continue;
        if (pg) {
            u32 a = top ? (e - need) & ~(MMU_PG - 1) : (lo + HDR + MMU_PG - 1) & ~(MMU_PG - 1);
            if (a - HDR > lo && a - HDR - lo < 2 * HDR)
                a = top ? a - MMU_PG : a + MMU_PG;  /* no room for a free block in front: a page further */
            if (a < lo + HDR || a + need > e || a + need < a)
                continue;
            h = a - HDR;
            u = a + need;
        } else if (top) {
            h = e - need;
            u = e;
        } else
            u = lo + need;
        if (e - u < 2 * HDR)
            u = e;                              /* too small to stay free: taken into the block */
        if (limit && u > limit)
            break;                              /* past the limit; the blocks only get higher from here */
        if (!pg && h - lo < 2 * HDR)
            h = lo;
        return carve(r, b, h, u, owner);
    }
    return 0;
}

void *mem_alloc(u32 n, u32 flags, u32 owner)
{
    if (n == 0 || n > MEM_ALLOC_MAX)            /* the size arithmetic below cannot wrap */
        return 0;
    u32 need = ((n + 15) & ~15u) + HDR;
    u32 limit = (flags & MEM_LOW16M) ? 0x01000000u : (flags & MEM_LOW1G) ? 0x40000000u : 0;
    int pg = (flags & MEM_USER) && mmu_on;
    for (int pass = 0; pass < 2; pass++) {
        /* pass 0: the wanted pool; pass 1: the other, unless chip-only */
        int want_chip = (flags & MEM_CHIP) ? 1 : 0;
        if (pass == 1) {
            if (flags & (MEM_CHIP | MEM_FASTONLY | MEM_LOW1G))
                break;
            want_chip = 1;
        }
        for (int i = 0; i < nranges; i++) {
            if (ranges[i].chip != want_chip)
                continue;
            if (limit && ranges[i].lo >= limit)
                continue;                       /* wholly above the limit; one crossing it gives from its low part (A15) */
            void *p = alloc_in(&ranges[i], pg ? (n + MMU_PG - 1) & ~(MMU_PG - 1) : need, owner, limit, pg);
            if (p) {
                if (__builtin_expect(pg, 0))    /* pg holds mmu_on: the table directly, not the gate (D45 13.5) */
                    mmu_ops->block((u32)p, n, owner);    /* valid at once for the process flipped in (D41 9) */
                return p;
            }
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
    mmu_freed((u32)(b + 1), SZ(b) - HDR);   /* D41: its whole pages invalid for every program */
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

void mem_set_owner(void *p, u32 owner)
{
    struct mblk *b = (struct mblk *)p - 1;
    struct range *r = range_of((u32)b);
    if (!r || b->magic != MAGIC || !USED(b))
        panic("mem_set_owner: not a block in use");
    b->owner = owner;
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

/* D41: whose block holds a (a fault's line): its owner, *used 0 for a free block; MEM_NOT_POOL outside the pools */
KCOLD u32 mem_owner_of(u32 a, int *used)
{
    struct range *r = range_of(a);
    if (!r)
        return MEM_NOT_POOL;
    for (struct mblk *b = (struct mblk *)r->lo; b; b = next_of(r, b))
        if (a < (u32)b + SZ(b)) {
            *used = USED(b) && a >= (u32)(b + 1);
            return a >= (u32)(b + 1) ? b->owner : 0;
        }
    return MEM_NOT_POOL;
}

void mem_stats(u32 *chip_free, u32 *fast_free)
{
    *chip_free = freeb[1];
    *fast_free = freeb[0];
}

/* DESIGN D29 (ZINE): the pools as they are (the blob walks their blocks with interrupts masked), and their sizes */
_Static_assert(sizeof(struct range) == sizeof(struct zine_range), "ZINE reads the range table");
u32 mem_ranges(const struct zine_range **r)
{
    *r = (const struct zine_range *)ranges;
    return (u32)nranges;
}

void mem_totals(u32 *chip, u32 *fast)
{
    *chip = *fast = 0;
    for (int i = 0; i < nranges; i++)
        *(ranges[i].chip ? chip : fast) += ranges[i].hi - ranges[i].lo;
}

void *kmalloc(u32 n) { return mem_alloc(n, 0, 0); }
void kfree(void *p) { mem_free(p); }

/* DESIGN D45: called once, by the idle loop when kmain has handed over to it (nothing has been scheduled yet; no
 * interrupt allocates). The block is filled with ILLEGAL first and the caches pushed and the instruction cache (and
 * Emu68's translations) dropped, so a path into it the build's check missed stops at once with the kernel's exception
 * line, never runs stale code; then it is an ordinary free (D41: its pages invalid for every program, as they were). */
KCOLD void mem_boot_release(void)
{
    for (int k = 0; k < 3; k++) {
        struct mblk *b = span_blk[k];
        if (!b || (k == 1 && ide_cd_present()) || (k == 2 && mmu_on))
            continue;
        span_blk[k] = 0;
        if (k == 2)
            mmu_disarm();               /* D45 13: the table back to its stubs: nothing points into the span now */
        u32 n = SZ(b), chip = range_of((u32)b)->chip, f0 = freeb[chip];
        for (u32 *p = (u32 *)(b + 1), *e = (u32 *)((u32)b + n); p < e; p++)
            *p = 0x4AFC4AFCu;
        cache_flush_code();
        mem_free(b + 1);
        kprintf("mem: %u bytes of %s code given back (%s RAM free %u -> %u)\n", n,
                k == 0 ? "boot" : k == 1 ? "CD drive" : "memory protection",
                chip ? "chip" : "fast", f0, freeb[chip]);
    }
}
