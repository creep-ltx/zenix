/* SPDX-License-Identifier: GPL-2.0-only */
/* Memory for the boot programs while AmigaOS lives (zenixboot, boot/zenixboot.c, and the demo floppy's stage 2,
 * boot/demoload.c): where the kernel will run, and buffers that keep out of it. Included once by each; the includer
 * defines AllocMem, FreeMem, EXEC_L, MEMF_* and the types first, and fills zone_lo/zone_hi before alloc_safe. */
#pragma once
/* Where nothing of ours may lie: low memory (vectors, the copier at 0x500, the table's home at 0x1000, the kernel's
 * first stack below 0x10000), the klog area at the top of chip RAM (the kernel writes its log there at once), and
 * the kernel's home with its BSS (the copy and the BSS clear would overwrite it). */
static u32 zone_lo[3], zone_hi[3];

static int overlaps(const u8 *p, u32 n)
{
    u32 a = (u32)p, b = a + n;
    for (int i = 0; i < 3; i++)
        if (a < zone_hi[i] && zone_lo[i] < b) return 1;
    return 0;
}

/* AllocMem outside the zones: a block that lands in one is kept (so the next try gets another) and given back at
 * the end. Chip RAM first (as the floppy loads), then any memory; the bundle (fast) fast RAM first, then any; the demo floppy's RAM disk (fast top) fast RAM from the top, then any. */
#define ALLOC_CHIP 0                     /* chip RAM first, cleared, from the top */
#define ALLOC_FAST 1                     /* fast RAM first */
#define ALLOC_FAST_TOP 2                 /* fast RAM first, from the top (the demo floppy's RAM disk, D25) */
static u8 *alloc_safe(u32 n, int fast)
{
    u8 *keep[16];
    u32 nkeep = 0;
    u8 *p = 0;
    for (int pass = 0; pass < 2 && !p; pass++) {
        long flags = pass ? 0 : fast ? MEMF_FAST : MEMF_CHIP;
        flags |= fast == ALLOC_FAST ? 0 : fast == ALLOC_FAST_TOP ? MEMF_REVERSE : MEMF_CLEAR | MEMF_REVERSE;
        while (nkeep < 16) {
            u8 *q = AllocMem(n, flags);
            if (!q) break;
            if (!overlaps(q, n)) { p = q; break; }
            keep[nkeep++] = q;
        }
    }
    while (nkeep) FreeMem(keep[--nkeep], n);
    return p;
}

/* The kernel's home, chosen the same way as handoff_tables does (largest fast region of the first 8 in exec's list,
 * 256 KB aligned, else chip 0x40000): needed before the buffers are allocated, so that they keep out of it.
 * zb_tables chooses again; the two must agree (checked). */
static u32 pick_home(void)
{
    u32 best = 0, home = 0x40000;
    u8 *mh = (u8 *)EXEC_L(322);         /* MemList lh_Head */
    for (int n = 0; n < 8 && *(u32 *)mh; n++, mh = (u8 *)*(u32 *)mh) {
        u16 attrs = *(u16 *)(mh + 14);
        u32 lo = (*(u32 *)(mh + 20) + 0x3FFFF) & 0xFFFC0000, hi = *(u32 *)(mh + 24);
        if (!(attrs & 4) || hi <= lo) continue;
        if (hi - lo > best) { best = hi - lo; home = lo; }
    }
    return home;
}

