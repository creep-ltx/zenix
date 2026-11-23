/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the kernel's allocator (kernel/mem.c), built 32-bit with its pools at their real addresses (mmap): a
 * fast range that crosses 1 GB gives MEM_LOW1G blocks from its low part (audit A15: it was skipped whole), never one
 * that ends above 1 GB; a range wholly below 1 GB (the A1200's fast RAM) behaves as before; ordinary blocks still come
 * from anywhere, big ones from the high end. DESIGN D45: the boot's spans inside the "kernel" (their bounds set by the
 * link, tools/mem-test.sh) reserved as ranges of one used block, given back by mem_boot_release (filled with ILLEGAL,
 * the free count up by exactly the span), the CD's kept while a drive is there, memory protection's kept while it is on
 * and given back (the table disarmed first) when it is off (section 13), nothing reserved or freed when the range table
 * is full; MEM_LOW16M (d29 16.19) below 16 MB in a range that crosses it, with and without protection. Build and run:
 * tools/mem-test.sh */
#include <stdio.h>
#include <stdarg.h>
#include <sys/mman.h>
#include "../kernel/mem.c"                      /* (kernel.h declares memset and memcpy; no string.h: -m32 types) */

void exit(int) __attribute__((noreturn));
void kprintf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void panic(const char *why) { printf("panic: %s\n", why); exit(2); }
u32 klog_area_base(void) { return 0; }
u8 mmu_on;                                      /* D41: off but in the D45 section 13 case */
static int cd_there, flushes, disarms, freed_hooks, disarm_early;
static u32 *mmu_span_word;                      /* a word inside the MMU span: still unfilled when the disarm comes? */
static void t_v(void) {}
static void t_p(struct proc *p) { (void)p; }
static int t_b(u32 d, u32 l, u32 o) { (void)d; (void)l; (void)o; return 0; }
static void t_freed(u32 d, u32 l, u32 o) { (void)d; (void)l; (void)o; freed_hooks++; }
static void t_i(int b) { (void)b; }
static int t_f(const u8 *f, u32 pc) { (void)f; (void)pc; return 0; }
static const struct mmu_ops t_ops = { .sw = t_p, .forget = t_p, .redo = t_p, .block = t_b, .upd_gone = t_v,
                                      .freed = t_freed, .hand = t_i, .before_reset = t_v, .fault = t_f };
u8 aud_valid;                                   /* D60 6.2 (drv.c): no channel the kernel checked */
void drv_freed(u32 lo, u32 hi) { (void)lo; (void)hi; }
const struct mmu_ops *mmu_ops = &t_ops;
void mmu_disarm(void)                           /* the order: before the span is filled, flushed and freed */
{
    disarms++;
    disarm_early = mmu_span_word && *mmu_span_word != 0x4AFC4AFCu && flushes == 1 && span_blk[2] == 0;
}
int ide_cd_present(void) { return cd_there; }
void cache_flush_code(void) { flushes++; }

static int bad;
#define EXPECT_TRUE(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); bad++; } else printf("ok:   %s\n", msg); } while (0)

static void pool(u32 lo, u32 hi)                /* one fast range [lo, hi) of real memory at that address */
{
    void *m = mmap((void *)lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m != (void *)lo) { printf("FAIL: cannot map %x-%x for the test\n", lo, hi); exit(1); }
    struct bootinfo bi;
    memset(&bi, 0, sizeof bi);
    bi.nregions = 1;
    bi.r[0].attrs = MEMF_FAST; bi.r[0].lower = lo; bi.r[0].upper = hi;
    nranges = 0; freeb[0] = freeb[1] = 0;
    mem_init(&bi, 0x100, 0x100);                /* the "kernel" excluded far below */
}

int main(void)
{
    const u32 G = 0x40000000u;

    /* 1. a fast range crossing 1 GB (an Emu68 Pi with more memory): the mailbox block comes from its low part */
    pool(G - 0x100000, G + 0x100000);
    u8 *a = mem_alloc(128, MEM_FASTONLY | MEM_LOW1G, 0);
    EXPECT_TRUE(a && (u32)a + 128 <= G, "a range crossing 1 GB: a MEM_LOW1G block below 1 GB (it was skipped whole)");
    u8 *b = mem_alloc(64 * 1024, MEM_FASTONLY | MEM_LOW1G, 0);
    EXPECT_TRUE(b && (u32)b + 64 * 1024 <= G, "... a big MEM_LOW1G block too: from the low end, below 1 GB");
    u8 *c = mem_alloc(64 * 1024, MEM_FASTONLY, 0);
    EXPECT_TRUE(c && (u32)c >= G, "... an ordinary big block still comes from the high end (above 1 GB here)");
    EXPECT_TRUE(!mem_alloc(0x100000, MEM_FASTONLY | MEM_LOW1G, 0), "... a MEM_LOW1G block that does not fit below 1 GB: refused");
    u8 *d = mem_alloc(0x80000, MEM_FASTONLY, 0);
    EXPECT_TRUE(d != 0, "... while an ordinary one of that size still fits");
    mem_free(a); mem_free(b); mem_free(c); mem_free(d);
    u32 cf, ff; mem_stats(&cf, &ff);
    EXPECT_TRUE(ff == 0x200000, "... everything freed merges back (all 2 MB free)");
    munmap((void *)(G - 0x100000), 0x200000);

    /* 2. wholly above 1 GB: no MEM_LOW1G block at all */
    pool(G + 0x100000, G + 0x200000);
    EXPECT_TRUE(!mem_alloc(128, MEM_FASTONLY | MEM_LOW1G, 0), "a range wholly above 1 GB gives no MEM_LOW1G block");
    EXPECT_TRUE(mem_alloc(128, MEM_FASTONLY, 0) != 0, "... but ordinary ones");
    munmap((void *)(G + 0x100000), 0x100000);

    /* 3. wholly below 1 GB (as the A1200's fast RAM): as before - small blocks from the low end */
    pool(0x30000000, 0x30100000);
    u8 *e = mem_alloc(128, MEM_FASTONLY | MEM_LOW1G, 0), *f = mem_alloc(128, MEM_FASTONLY, 0);
    EXPECT_TRUE(e == (u8 *)0x30000000 + 16 && f == e + 144, "a range below 1 GB: MEM_LOW1G and ordinary small blocks as before, from the low end");
    u8 *g = mem_alloc(64 * 1024, MEM_FASTONLY, 0);
    EXPECT_TRUE(g && (u32)g + 64 * 1024 == 0x30100000, "... an ordinary big one from the high end");
    munmap((void *)0x30000000, 0x100000);

    /* 5. d29 16.19: MEM_LOW16M (SYS_memalloc's MEMA_LOW) in a fast range crossing 16 MB (Zorro II and III RAM side by
     * side would be two ranges; one crossing is the harder case): blocks that end below 16 MB, big ones too, from the
     * low end; with memory protection on (whole pages) as well */
    {
        const u32 M = 0x01000000u;
        pool(M - 0x100000, M + 0x100000);
        u8 *a = mem_alloc(128, MEM_FASTONLY | MEM_LOW16M, 0);
        EXPECT_TRUE(a && (u32)a + 128 <= M, "MEM_LOW16M: a small block below 16 MB");
        u8 *b = mem_alloc(0x40000, MEM_LOW16M, 0);
        EXPECT_TRUE(b && (u32)b + 0x40000 <= M && (u32)b < M - 0x80000, "... a big one too, from the low end");
        u8 *c = mem_alloc(0x40000, 0, 0);
        EXPECT_TRUE(c && (u32)c >= M, "... an ordinary big block still from the high end (above 16 MB here)");
        EXPECT_TRUE(!mem_alloc(0xC0000, MEM_LOW16M, 0), "... one that does not fit below 16 MB: refused (no chip RAM here)");
        mmu_on = 2;
        u8 *d = mem_alloc(0x20000, MEM_LOW16M | MEM_USER, 0);
        EXPECT_TRUE(d && (u32)d + 0x20000 <= M && !((u32)d & (MMU_PG - 1)), "... with protection on: whole pages, below 16 MB");
        u8 *e = mem_alloc(0xA0000, MEM_LOW16M | MEM_USER, 0);   /* 0x9F000 or so is left below */
        EXPECT_TRUE(!e, "... and refused when the pages would cross it");
        mmu_on = 0;
        mem_free(a); mem_free(b); mem_free(c); mem_free(d); mem_free(e);
        u32 cf, ff; mem_stats(&cf, &ff);
        EXPECT_TRUE(ff == 0x200000, "... everything freed merges back");
        munmap((void *)(M - 0x100000), 0x200000);
        pool(M, M + 0x100000);
        EXPECT_TRUE(!mem_alloc(128, MEM_LOW16M, 0), "a range wholly above 16 MB gives no MEM_LOW16M block");
        munmap((void *)M, 0x100000);
    }

    /* 4. DESIGN D45: a fast region 0x20000000-0x20100000 with the "kernel" at 0x20010000 (128 KB); its spans as
     * tools/mem-test.sh links them: the boot's 0x20010100-0x20018100 (32 KB), the CD's 0x20018100-0x20018400, memory
     * protection's 0x20018400-0x20019400 */
    {
        u32 R = 0x20000000u, K = 0x20010000u, B0 = 0x20010100u, B1 = 0x20018100u, C1 = 0x20018400u, M1 = 0x20019400u;
        void *m = mmap((void *)R, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (m != (void *)R) { printf("FAIL: cannot map the D45 region\n"); return 1; }
        struct bootinfo bi;
        memset(&bi, 0, sizeof bi);
        bi.nregions = 1;
        bi.r[0].attrs = MEMF_FAST; bi.r[0].lower = R; bi.r[0].upper = R + 0x100000;
        nranges = 0; freeb[0] = freeb[1] = 0;
        mem_init(&bi, K, 0x20000);
        u32 cf, f0, f1, f2, ct, ft;
        mem_stats(&cf, &f0);
        mem_totals(&ct, &ft);
        EXPECT_TRUE(nranges == 5 && span_blk[0] == (struct mblk *)B0 && span_blk[1] == (struct mblk *)B1 &&
                    span_blk[2] == (struct mblk *)C1, "D45: the three spans reserved as ranges of their own, after the real memory's");
        EXPECT_TRUE(f0 == 0x100000 - 0x20000, "... in use: the free count is the region less the kernel");
        EXPECT_TRUE(ft == f0 + (B1 - B0) + (C1 - B1) + (M1 - C1), "... counted in the pool's total");
        cd_there = 1;
        mmu_on = 2;
        mem_boot_release();
        mem_stats(&cf, &f1);
        EXPECT_TRUE(f1 == f0 + (B1 - B0) && flushes == 1, "the boot span given back: free up by exactly its size, the caches flushed");
        EXPECT_TRUE(((u32 *)B0)[4] == 0x4AFC4AFCu && ((u32 *)B1)[-1] == 0x4AFC4AFCu, "... filled with ILLEGAL first");
        EXPECT_TRUE(span_blk[1] && USED(span_blk[1]), "the CD span kept: a drive answered");
        EXPECT_TRUE(span_blk[2] && USED(span_blk[2]) && !disarms, "the memory protection span kept, its table armed: protection is on");
        int in = 0;
        for (int i = 0; i < 300 && !in; i++) {
            u8 *p = mem_alloc(8192, MEM_FASTONLY, 1);
            in = p && (u32)p >= B0 && (u32)p < B1;
        }
        EXPECT_TRUE(in, "... and the boot span's memory is handed out once the rest is taken");
        mem_free_owner(1);
        mmu_on = 0;
        freed_hooks = 0;
        mmu_span_word = (u32 *)C1 + 4;
        *mmu_span_word = 0;
        mem_boot_release();
        mem_stats(&cf, &f2);
        EXPECT_TRUE(f2 == f1 + (M1 - C1) && flushes == 2 && disarms == 1 && !span_blk[2] && !freed_hooks,
                    "protection off: the memory protection span given back, no hook run");
        EXPECT_TRUE(disarm_early, "... the table disarmed first: before the fill, the flush and the free");
        EXPECT_TRUE(((u32 *)C1)[4] == 0x4AFC4AFCu && ((u32 *)M1)[-1] == 0x4AFC4AFCu, "... filled with ILLEGAL first");
        mem_boot_release();
        mem_stats(&cf, &f1);
        EXPECT_TRUE(f1 == f2 && flushes == 2 && disarms == 1, "a further release does nothing");
        /* the range table full: the spans stay the kernel's, the release frees nothing */
        nranges = 0; freeb[0] = freeb[1] = 0;
        memset(ranges, 0, sizeof ranges);
        nranges = 14;                                   /* 14 empty slots taken: the region's two ranges fill it */
        span_blk[0] = span_blk[1] = span_blk[2] = 0;
        mem_init(&bi, K, 0x20000);
        mem_stats(&cf, &f0);
        cd_there = 0;
        mem_boot_release();
        mem_stats(&cf, &f1);
        EXPECT_TRUE(nranges == 16 && !span_blk[0] && !span_blk[1] && !span_blk[2] && f1 == f0 && flushes == 2 &&
                    disarms == 1,
                    "the range table full: the spans stay the kernel's, nothing freed, no panic");
        munmap((void *)R, 0x100000);
    }

    /* 6. (D60 stage 4) with protection on, a big block from the highest free block of fast RAM it fits in: a hole left at the top by a program that
     * ended takes the next big block, the biggest free block stays whole; with memory protection on the kernel's own
     * blocks never come from chip RAM while there is fast RAM (a DMA engine in a process's hands writes chip RAM) */
    {
        const u32 R = 0x30000000u, C = 0x31000000u;
        void *m1 = mmap((void *)R, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        void *m2 = mmap((void *)C, 0x40000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (m1 != (void *)R || m2 != (void *)C) { printf("FAIL: cannot map the test's pools\n"); return 1; }
        struct bootinfo bi;
        memset(&bi, 0, sizeof bi);
        bi.nregions = 2;
        bi.r[0].attrs = MEMF_FAST; bi.r[0].lower = R; bi.r[0].upper = R + 0x100000;
        bi.r[1].attrs = MEMF_CHIP; bi.r[1].lower = C; bi.r[1].upper = C + 0x40000;
        nranges = 0; freeb[0] = freeb[1] = 0; fast_pool = 0;
        mem_init(&bi, 0x100, 0x100);
        mmu_on = 2;                                 /* (with protection only) */
        u8 *top = mem_alloc(64 * 1024, 0, 7), *next = mem_alloc(64 * 1024, 0, 8);
        mem_free(top);                              /* a hole at the top, above next */
        u8 *c = mem_alloc(40 * 1024, 0, 9);
        mmu_on = 0;
        u32 cf, ff, big = 0;
        for (struct mblk *b = (struct mblk *)ranges[0].lo; b; b = next_of(&ranges[0], b))
            if (!USED(b) && SZ(b) > big)
                big = SZ(b);
        EXPECT_TRUE(top && next && c >= top && c + 40 * 1024 <= top + 64 * 1024 && next < top &&
                    big >= (u32)(next - (u8 *)R) - 64,
                    "a big block goes into the hole at the top, the biggest free block stays whole");
        mem_free(c);
        mem_free(next);
        u8 *fill = mem_alloc(0x100000 - 8192, 0, 7);  /* the fast RAM taken, to its last bytes */
        for (u32 n = 4096; n >= 16; n /= 2)
            while (mem_alloc(n, MEM_FASTONLY, 7))
                ;
        mmu_on = 2;
        void *k = mem_alloc(256, 0, 0), *ku = mem_alloc(4096, MEM_USER, 0), *u = mem_alloc(4096, MEM_USER, 7);
        mmu_on = 0;
        void *k0 = mem_alloc(256, 0, 0);
        mem_stats(&cf, &ff);
        EXPECT_TRUE(fill && !k && !ku && u && (u32)u >= C && k0 && (u32)k0 >= C,
                    "protection on, the fast RAM full: no kernel block from chip RAM, a process's block yes; off: chip RAM");
        munmap((void *)R, 0x100000);
        munmap((void *)C, 0x40000);
    }

    printf(bad ? "FAILED\n" : "all ok\n");
    return bad != 0;
}
