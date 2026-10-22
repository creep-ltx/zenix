#include "kernel.h"

/* Trivial bitmap page-frame allocator over one contiguous region
 * (the largest fast RAM region; falls back to chip above the kernel). */

#define MAXPAGES 32768u         /* covers up to 128MB */

static u32 base;
static u32 npages;
static u8 bmap[MAXPAGES / 8];

void pmm_init(u32 lo, u32 hi)
{
    base = (lo + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    npages = (hi > base) ? (hi - base) / PAGE_SIZE : 0;
    if (npages > MAXPAGES)
        npages = MAXPAGES;
    kprintf("pmm: %u pages (%uKB) at %p\n", npages, npages * 4, base);
}

void pmm_reserve(u32 pa, u32 len)
{
    u32 lo = pa & ~(PAGE_SIZE - 1);
    u32 hi = (pa + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    u32 n = 0;
    for (u32 a = lo; a < hi; a += PAGE_SIZE) {
        if (a < base)
            continue;
        u32 i = (a - base) / PAGE_SIZE;
        if (i >= npages)
            break;
        bmap[i >> 3] |= 1 << (i & 7);
        n++;
    }
    kprintf("pmm: reserved %u pages at %p\n", n, lo);
}

u32 pmm_alloc(void)
{
    for (u32 i = 0; i < npages; i++) {
        if (!(bmap[i >> 3] & (1 << (i & 7)))) {
            bmap[i >> 3] |= 1 << (i & 7);
            u32 pa = base + i * PAGE_SIZE;
            memset((void *)pa, 0, PAGE_SIZE);
            return pa;
        }
    }
    panic("pmm: out of pages");
}

void pmm_free(u32 pa)
{
    if (pa < base)
        panic("pmm_free: bad address");
    u32 i = (pa - base) / PAGE_SIZE;
    if (i >= npages || !(bmap[i >> 3] & (1 << (i & 7))))
        panic("pmm_free: not allocated");
    bmap[i >> 3] &= (u8)~(1 << (i & 7));
}

u32 pmm_free_pages(void)
{
    u32 n = 0;
    for (u32 i = 0; i < npages; i++)
        if (!(bmap[i >> 3] & (1 << (i & 7))))
            n++;
    return n;
}
