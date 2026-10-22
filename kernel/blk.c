/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"

/* The raw disk behind the "ide0" name of the disk server: a Gayle IDE drive when there is one,
 * else the SD card of a Raspberry Pi 4 / CM4 under Emu68 (emmc.c). */
static int use_emmc;

int blk_init(void)
{
    if (ide_init() == 0) {
        use_emmc = 0;
        return 0;
    }
    if (emmc_init() == 0) {
        use_emmc = 1;
        return 0;
    }
    return -1;
}

u32 blk_sectors(void)
{
    return use_emmc ? emmc_sectors() : ide_sectors();
}

int blk_read(u32 lba, u32 count, void *buf)
{
    return use_emmc ? emmc_read(lba, count, buf) : ide_read(lba, count, buf);
}

int blk_write(u32 lba, u32 count, const void *buf)
{
    return use_emmc ? emmc_write(lba, count, buf) : ide_write(lba, count, buf);
}
