#include "kernel.h"

/* User address-space plumbing. Page tables and user pages are pmm pages;
 * the kernel reaches them through the supervisor identity map, so a
 * physical address doubles as a pointer. User virtual addresses are
 * resolved by walking the process's table tree in software (supervisor
 * accesses translate through SRP, so user pointers can never be
 * dereferenced directly). */

void umem_map(u32 *uroot, u32 va, u32 pa, u32 flags)
{
    u32 ai = va >> 25, bi = (va >> 18) & 0x7F, ci = (va >> 12) & 0x3F;
    u32 *bt, *ct;

    if ((uroot[ai] & 3) != 2) {
        bt = (u32 *)pmm_alloc();
        uroot[ai] = (u32)bt | 2;
    } else
        bt = (u32 *)(uroot[ai] & ~0xFu);

    if ((bt[bi] & 3) != 2) {
        ct = (u32 *)pmm_alloc();
        bt[bi] = (u32)ct | 2;
    } else
        ct = (u32 *)(bt[bi] & ~0xFu);

    ct[ci] = pa | flags | 1;
}

u32 uva_to_pa(const u32 *uroot, u32 va)
{
    u32 e = uroot[va >> 25];
    if ((e & 3) != 2)
        return 0;
    e = ((u32 *)(e & ~0xFu))[(va >> 18) & 0x7F];
    if ((e & 3) != 2)
        return 0;
    e = ((u32 *)(e & ~0xFu))[(va >> 12) & 0x3F];
    if ((e & 3) != 1)
        return 0;
    return (e & ~(PAGE_SIZE - 1)) | (va & (PAGE_SIZE - 1));
}

void umem_free_root(u32 *uroot)
{
    for (int ai = 0; ai < 128; ai++) {
        if ((uroot[ai] & 3) != 2)
            continue;
        u32 *bt = (u32 *)(uroot[ai] & ~0xFu);
        for (int bi = 0; bi < 128; bi++) {
            if ((bt[bi] & 3) != 2)
                continue;
            u32 *ct = (u32 *)(bt[bi] & ~0xFu);
            for (int ci = 0; ci < 64; ci++)
                if ((ct[ci] & 3) == 1)
                    pmm_free(ct[ci] & ~(PAGE_SIZE - 1));
            pmm_free((u32)ct);
        }
        pmm_free((u32)bt);
    }
    pmm_free((u32)uroot);
}

int copy_from_user(struct proc *p, void *dst, u32 uva, u32 n)
{
    u8 *d = dst;
    while (n) {
        u32 pa = uva_to_pa(p->uroot, uva);
        if (!pa)
            return -1;
        u32 chunk = PAGE_SIZE - (uva & (PAGE_SIZE - 1));
        if (chunk > n)
            chunk = n;
        memcpy(d, (void *)pa, chunk);
        d += chunk;
        uva += chunk;
        n -= chunk;
    }
    return 0;
}

int copy_to_user(struct proc *p, u32 uva, const void *src, u32 n)
{
    const u8 *s = src;
    while (n) {
        u32 pa = uva_to_pa(p->uroot, uva);
        if (!pa)
            return -1;
        u32 chunk = PAGE_SIZE - (uva & (PAGE_SIZE - 1));
        if (chunk > n)
            chunk = n;
        memcpy((void *)pa, s, chunk);
        s += chunk;
        uva += chunk;
        n -= chunk;
    }
    return 0;
}

int strncpy_from_user(struct proc *p, char *dst, u32 uva, u32 max)
{
    for (u32 i = 0; i < max; i++) {
        u32 pa = uva_to_pa(p->uroot, uva + i);
        if (!pa)
            return -1;
        dst[i] = *(char *)pa;
        if (!dst[i])
            return (int)i;
    }
    dst[max - 1] = 0;
    return (int)max - 1;
}
