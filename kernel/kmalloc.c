#include "kernel.h"

/* Small first-fit kernel heap over pmm pages. Allocations are capped at
 * PAGE_SIZE-16 for now (no multi-page contiguity in pmm yet); blocks are
 * 16-byte granular with an 8-byte header. Freed blocks go back on the
 * free list; no coalescing yet (TODO when fragmentation bites). */

struct blk {
    u32 size;                   /* payload bytes */
    struct blk *next;           /* free list link */
};

static struct blk *freelist;

void *kmalloc(u32 n)
{
    if (n == 0 || n > PAGE_SIZE - 16)
        panic("kmalloc: bad size");
    n = (n + 15) & ~15u;

    struct blk **pp = &freelist;
    for (struct blk *b = freelist; b; pp = &b->next, b = b->next) {
        if (b->size >= n) {
            if (b->size >= n + 32) {        /* split */
                struct blk *rest = (struct blk *)((u8 *)(b + 1) + n);
                rest->size = b->size - n - sizeof(struct blk);
                rest->next = b->next;
                *pp = rest;
                b->size = n;
            } else
                *pp = b->next;
            return b + 1;
        }
    }
    /* grow by one page and retry */
    struct blk *b = (struct blk *)pmm_alloc();  /* identity-mapped */
    b->size = PAGE_SIZE - sizeof(struct blk);
    b->next = freelist;
    freelist = b;
    return kmalloc(n);
}

void kfree(void *p)
{
    if (!p)
        return;
    struct blk *b = (struct blk *)p - 1;
    b->next = freelist;
    freelist = b;
}
