/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the kernel's allocator (kernel/mem.c), built 32-bit with its pools at their real addresses (mmap): a
 * fast range that crosses 1 GB gives MEM_LOW1G blocks from its low part (audit A15: it was skipped whole), never one
 * that ends above 1 GB; a range wholly below 1 GB (the A1200's fast RAM) behaves as before; ordinary blocks still come
 * from anywhere, big ones from the high end. Build and run: tools/mem-test.sh */
#include <stdio.h>
#include <stdarg.h>
#include <sys/mman.h>
#include "../kernel/mem.c"                      /* (kernel.h declares memset and memcpy; no string.h: -m32 types) */

void exit(int) __attribute__((noreturn));
void kprintf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void panic(const char *why) { printf("panic: %s\n", why); exit(2); }
u32 klog_area_base(void) { return 0; }

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

    printf(bad ? "FAILED\n" : "all ok\n");
    return bad != 0;
}
