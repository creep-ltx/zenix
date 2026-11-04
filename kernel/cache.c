/* SPDX-License-Identifier: GPL-2.0-only */
/* DESIGN D37 (docs/d37-data-cache.md): the CPU's caches at boot. 68020/68030 and Emu68: the instruction cache only, as
 * before. A real 68040/68060: the data cache on for fast RAM above the first 16 MB, through the transparent
 * translation registers (and the 060's TCR default) with the MMU off; the first 16 MB (chip RAM, the chips, every I/O
 * of the classic Amiga) stays inhibited and serialized. Copyback unless the boot option says otherwise (bootinfo
 * kopts, zenixboot DCACHE=), checked by a self-test. Cold code, run once: -Os, and small (the kernel's floppy room). */
#pragma GCC optimize("Os")
#include "kernel.h"
#include "hw.h"
#include "hwinfo.h"
#include "cache_plan.h"

/* start.S */
long cpu_pcr(void);
int cache_apply(const u32 *r);
u32 cache_probe(u32 *p, u32 on, u32 off);
void cache_reset_state(void);
void cache_push_lines(u32 lo, u32 hi);

u8 cache_dmode;                         /* CACHE_D_*: what the data cache does now */

#define TTR(base, mask, cm) ((base) << 24 | (mask) << 16 | 0xC000u | (cm) << 5)  /* E, S = 1x (all accesses) */
#define CM_WT  0u
#define CM_CB  1u
#define CM_INH 2u                       /* 040: inhibited, serialized; 060: inhibited, precise */
#define CACR_DE 0x80000000u             /* 040 DE, 060 EDC */

static void mark(unsigned *map, u32 lo, u32 hi)  /* the 16 MB units [lo, hi) touches */
{
    for (u32 u = lo >> 24; hi > lo && u <= (hi - 1) >> 24; u++)
        map[u >> 5] |= 1u << (u & 31);
}

static void off(const char *why, int is040plus)
{
    cache_enable(is040plus);
    cache_dmode = CACHE_D_OFF;
    kprintf("cache: instruction cache on, data cache off: %s\n", why);
}

void cache_setup(const struct bootinfo *bi)
{
    static const char *const why[] = { 0, "no fast RAM above 16 MB", "a board in fast RAM's block",
                                       "68040, a board above 16 MB" };
    const struct hwinfo *hw = (const struct hwinfo *)HWINFO_ADDR;
    int hwok = hw->magic == HWINFO_MAGIC;
    if (!(bi->attn & 0x88))
        return off("68020/68030", 0);
    if (!hwok)                          /* an old loader: Emu68 and the boards cannot be known (review D37) */
        return off("no hardware table", 1);
    if (hw->flags & HW_DT)
        return off("Emu68 (its own)", 1);       /* exactly as before: Emu68 keeps memory coherent with the chips */
    if (bi->kopts & KOPT_DCACHE_OFF)
        return off("DCACHE=OFF", 1);
    if (!(CIAA(0) & 0x40))              /* CIA-A PRA bit 6 low: the left mouse button is held */
        return off("mouse button held", 1);
    unsigned fast[8] = { 0 }, io[8] = { 0 }, base, mask, bad;
    for (int i = 0; i < bi->nregions && i < 8; i++)
        if (bi->r[i].attrs & MEMF_FAST)
            mark(fast, bi->r[i].lower, bi->r[i].upper);
    for (u32 i = 0; i < hw->nboards && i < HW_MAXBOARDS; i++) {
        const struct hwboard *b = &hw->board[i];
        if (!(b->type & 0x20) && b->addr && !(b->cd_flags & 1))      /* not RAM, configured */
            mark(io, b->addr, b->addr + b->size);
    }
    long pcr = cpu_pcr();               /* -1: no PCR, a 68040 (AttnFlags say 040 on a 060 without 68060.library) */
    int is060 = pcr != -1, wt = (bi->kopts & KOPT_DCACHE_WT) != 0;
    int k = cache_plan(fast, io, is060, &base, &mask, &bad);
    if (k)
        return off(why[k], 1);
    u32 r[5] = { TTR(0u, 0u, CM_INH), TTR(base, mask, wt ? CM_WT : CM_CB), is060 ? 0x0200u : 0,  /* DCO inhibited */
                 is060 ? 0xA0C08000u : 0x80008000u,     /* EDC ESB EBC CABC EIC; 040 DE IE */
                 is060 ? (u32)((pcr & 2) | 1) : ~0u };   /* PCR: ESS on, DFP kept, EDEBUG off */
    if (cache_apply(r))
        return off("a register missing", 1);
    cache_dmode = wt ? CACHE_D_WT : CACHE_D_CB;
    kprintf("cache: %s, data cache %s for %p-%p, %s uncached\n",
            is060 ? "68060 (superscalar, branch cache, store buffer)" : "68040", wt ? "write-through" : "copyback",
            base ? base << 24 : 0x01000000u, (base | mask) << 24 | 0xFFFFFFu, is060 ? "the rest" : "the first 16 MB");

    /* the self-test (D37 5.5): chip RAM must come out uncached, else the data cache goes off again */
    u32 on = r[3] & ~0x00400000u;       /* CABC reads as 0 */
    static u32 fline[4] __attribute__((aligned(16)));    /* the kernel's own memory: the largest fast region */
    u8 *cb = mem_alloc(32, MEM_CHIP, 0);
    u32 c = (u32)cb, fu = (u32)fline >> 24;
    u32 cr = cb ? cache_probe((u32 *)((c + 15) & ~15u), on, on & ~CACR_DE) : 0;
    u32 fr = fu && (fu & ~mask) == base ? cache_probe(fline, on, on & ~CACR_DE) : 4;
    mem_free(cb);
    if (cr) {
        cache_reset_state();
        return off("the self-test found chip RAM cached", 1);
    }
    static const char *const seen[] = { "not cached", "write-through", "?", "copyback", "not checked" };
    kprintf("cache: self-test: chip RAM %s, fast RAM %s\n", cb ? "uncached" : "not checked", seen[fr]);
}

/* before a software reset (kbd.c machine_reset): nothing dirty lost, the registers as a hardware reset leaves them */
void cache_before_reset(void)
{
    if (cache_dmode != CACHE_D_OFF)
        cache_reset_state();
}

/* DRV_CACHE (drv.c) on a real 68040/68060 with the data cache on: the lines of [lo, hi) pushed and invalidated */
void cache_push_range(u32 lo, u32 hi)
{
    if (cache_dmode != CACHE_D_OFF)
        cache_push_lines(lo & ~15u, (hi + 15) & ~15u);
}
