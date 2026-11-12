/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the kernel's allocator (kernel/mem.c), built 32-bit with its pools at their real addresses (mmap): a
 * fast range that crosses 1 GB gives MEM_LOW1G blocks from its low part (audit A15: it was skipped whole), never one
 * that ends above 1 GB; a range wholly below 1 GB (the A1200's fast RAM) behaves as before; ordinary blocks still come
 * from anywhere, big ones from the high end. DESIGN D45: the boot's spans inside the "kernel" (their bounds set by the
 * link, tools/mem-test.sh) reserved as ranges of one used block, given back by mem_boot_release (filled with ILLEGAL,
 * the free count up by exactly the span), the CD's kept while a drive is there, memory protection's kept while it is on
 * and given back (the table disarmed first) when it is off (section 13), nothing reserved or freed when the range table
 * is full. Build and run: tools/mem-test.sh */
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
static void t_3(u32 d, u32 l, u32 o) { (void)d; (void)l; (void)o; }
static void t_freed(u32 d, u32 l) { (void)d; (void)l; freed_hooks++; }
static void t_i(int b) { (void)b; }
static int t_f(const u8 *f, u32 pc) { (void)f; (void)pc; return 0; }
static const struct mmu_ops t_ops = { t_p, t_v, t_p, t_p, t_3, t_v, t_freed, t_i, t_v, t_f };
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

    printf(bad ? "FAILED\n" : "all ok\n");
    return bad != 0;
}
