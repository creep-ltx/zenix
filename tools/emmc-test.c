/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of kernel/emmc.c against the SDHCI model in tools/emmc-fake.h. Build and run: tools/emmc-test.sh */
#include <stdarg.h>
#include <stdio.h>
#define EMMC_HOST_TEST 1
#include "../kernel/emmc.c"

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

static int bad;
#define EXPECT_TRUE(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); bad++; } else printf("ok:   %s\n", msg); } while (0)

int main(void)
{
    for (int i = 0; i < FAKE_SECTORS * 512; i++)
        fake_img[i] = (u8)(i * 31 + (i >> 9) * 7);
    memcpy(fake_img, "RDSK", 4);
    fake_hwinfo = (struct hwinfo){ .magic = HWINFO_MAGIC, .flags = HW_DT | HW_BCM2711, .soc_base = 0xF2000000,
                                   .mailbox = 0xF200B880, .emmc = 0xF2340000 };
    trace = getenv("TRACE") != NULL;

    EXPECT_TRUE(emmc_init() == 0, "emmc_init succeeds");
    EXPECT_TRUE(emmc_sectors() == FAKE_SECTORS, "card size read from the CSD");

    static u8 buf[512 * 300 + 16];
    u8 *b = buf + 1;                                    /* unaligned on purpose */
    EXPECT_TRUE(emmc_read(5, 1, b) == 0 && !memcmp(b, fake_img + 5 * 512, 512), "read one sector");
    EXPECT_TRUE(emmc_read(100, 3, b) == 0 && !memcmp(b, fake_img + 100 * 512, 3 * 512), "read 3 sectors (multi block, auto CMD12)");
    EXPECT_TRUE(emmc_read(0, 300, b) == 0 && !memcmp(b, fake_img, 300 * 512), "read 300 sectors");
    EXPECT_TRUE(emmc_read(FAKE_SECTORS - 1, 1, b) == 0 && !memcmp(b, fake_img + (FAKE_SECTORS - 1) * 512, 512), "read the last sector");
    EXPECT_TRUE(emmc_read(FAKE_SECTORS - 1, 2, b) != 0, "read past the end is refused");

    static u8 pat[512 * 9];
    for (int i = 0; i < (int)sizeof pat; i++)
        pat[i] = (u8)(i * 13 + 5);
    EXPECT_TRUE(emmc_write(2000, 1, pat) == 0 && !memcmp(fake_img + 2000 * 512, pat, 512), "write one sector");
    EXPECT_TRUE(emmc_write(2010, 9, pat) == 0 && !memcmp(fake_img + 2010 * 512, pat, 9 * 512), "write 9 sectors");
    EXPECT_TRUE(emmc_read(2010, 9, b) == 0 && !memcmp(b, pat, 9 * 512), "read back what was written");
    int stray = 0;                                      /* nothing outside the two written ranges changed */
    for (int i = 0; i < FAKE_SECTORS * 512; i++) {
        int sec = i / 512;
        if (sec == 2000 || (sec >= 2010 && sec < 2019))
            continue;
        if (fake_img[i] != (u8)(i * 31 + (i >> 9) * 7) && !(i < 4))
            stray++;
    }
    EXPECT_TRUE(stray == 0, "writes touched only their own sectors");

    {                                                   /* the boot line names what sector 0 holds */
        static u8 s0[512]; char w[48];
        memcpy(s0, "RDSK", 4); sector0_kind(s0, w);
        EXPECT_TRUE(!strcmp(w, "Amiga RDB"), "sector 0 with RDSK: Amiga RDB");
        memset(s0, 0, 512); s0[510] = 0x55; s0[511] = 0xAA; sector0_kind(s0, w);
        EXPECT_TRUE(!strcmp(w, "MBR partition table"), "sector 0 with 55 AA: MBR partition table");
        memset(s0, 0, 512); s0[0] = 0xfa; s0[3] = 0x0c; sector0_kind(s0, w);
        EXPECT_TRUE(!strcmp(w, "unknown (first bytes fa 00 00 0c)"), "other sector 0: its first bytes in hex");
    }
    EXPECT_TRUE(faults == 0, "no protocol faults from the model");
    printf(bad || faults ? "FAILED\n" : "all ok\n");
    return bad || faults;
}
