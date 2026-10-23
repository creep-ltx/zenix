/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of kernel/a3000.c against the WD33C93A + DMAC model in tools/a3000-fake.h (tools/a3000-test.sh). */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/kernel.h"

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}
u16 irq_save2(void) { return 0; }
void irq_restore(u16 v) { (void)v; }
void *mem_alloc(u32 n, u32 flags, u32 owner)
{
    (void)flags; (void)owner;
    void *p = malloc(n);
    memset(p, 0xAA, n);                                  /* garbage: a read that skips the flush shows */
    return p;
}

#define A3000_HOST_TEST 1
#include "../kernel/a3000.c"

static int bad;
#define EXPECT_TRUE(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); bad++; } else printf("ok:   %s\n", msg); } while (0)

int main(void)
{
    for (int i = 0; i < FAKE_BLOCKS * 512; i++)
        fake_disk[i] = (u8)(i * 29 + (i >> 9) * 11);
    trace = getenv("TRACE") != NULL;
    EXPECT_TRUE(a3000_init() == 0, "a3000_init finds the disk on target 2 (targets 0 and 1 time out)");
    EXPECT_TRUE(a3000_sectors() == FAKE_BLOCKS, "capacity from READ CAPACITY");
    EXPECT_TRUE(resets >= 1, "the chip was reset");

    static u8 buf[512 * 300 + 8];
    u8 *b = buf + 1;                                     /* odd address on purpose: the DMA uses the bounce buffer */
    EXPECT_TRUE(a3000_read(7, 1, b) == 0 && !memcmp(b, fake_disk + 7 * 512, 512), "read one sector");
    EXPECT_TRUE(a3000_read(100, 5, b) == 0 && !memcmp(b, fake_disk + 100 * 512, 5 * 512), "read 5 sectors");
    EXPECT_TRUE(a3000_read(0, 300, b) == 0 && !memcmp(b, fake_disk, 300 * 512), "read 300 sectors (several bounce-buffer chunks)");
    EXPECT_TRUE(a3000_read(FAKE_BLOCKS - 1, 1, b) == 0 && !memcmp(b, fake_disk + (FAKE_BLOCKS - 1) * 512, 512), "read the last sector");
    EXPECT_TRUE(a3000_read(FAKE_BLOCKS - 1, 2, b) != 0, "read past the end is refused");

    static u8 pat[512 * 9 + 1];
    for (int i = 0; i < (int)sizeof pat; i++)
        pat[i] = (u8)(i * 13 + 5);
    static u8 before[FAKE_BLOCKS * 512];
    memcpy(before, fake_disk, sizeof before);
    EXPECT_TRUE(a3000_write(3000, 1, pat) == 0 && !memcmp(fake_disk + 3000 * 512, pat, 512), "write one sector");
    EXPECT_TRUE(a3000_write(3010, 9, pat + 1) == 0 && !memcmp(fake_disk + 3010 * 512, pat + 1, 9 * 512), "write 9 sectors from an odd address");
    EXPECT_TRUE(a3000_read(3010, 9, b) == 0 && !memcmp(b, pat + 1, 9 * 512), "read back what was written");
    int stray = 0;
    for (int i = 0; i < FAKE_BLOCKS * 512; i++) {
        int sec = i / 512;
        if (sec == 3000 || (sec >= 3010 && sec < 3019)) continue;
        if (fake_disk[i] != before[i]) stray++;
    }
    EXPECT_TRUE(stray == 0, "writes touched only their own sectors");
    EXPECT_TRUE(a3000_flush() == 0 && syncs == 1, "flush sends SYNCHRONIZE CACHE");
    fake_nosync = 1;
    EXPECT_TRUE(a3000_flush() == -95, "a drive without SYNCHRONIZE CACHE: flush says it cannot (-95)");
    EXPECT_TRUE(faults == 0, "no programming-model faults from the chip model");
    printf(bad || faults ? "FAILED\n" : "all ok\n");
    return bad || faults;
}
