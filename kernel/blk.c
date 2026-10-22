/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"

/* The raw disk behind the "ide0" name of the disk server: a Gayle IDE drive when there is one, else the
 * SD card of a Raspberry Pi 4 / CM4 under Emu68 (emmc.c), else the Amiga 3000's SCSI disk (a3000.c). */
static enum { B_IDE, B_EMMC, B_A3000 } which;

int blk_init(void)
{
    if (ide_init() == 0) {
        which = B_IDE;
        return 0;
    }
    if (emmc_init() == 0) {
        which = B_EMMC;
        return 0;
    }
    if (a3000_init() == 0) {
        which = B_A3000;
        return 0;
    }
    return -1;
}

u32 blk_sectors(void)
{
    return which == B_EMMC ? emmc_sectors() : which == B_A3000 ? a3000_sectors() : ide_sectors();
}

int blk_read(u32 lba, u32 count, void *buf)
{
    return which == B_EMMC ? emmc_read(lba, count, buf) : which == B_A3000 ? a3000_read(lba, count, buf) : ide_read(lba, count, buf);
}

int blk_write(u32 lba, u32 count, const void *buf)
{
    return which == B_EMMC ? emmc_write(lba, count, buf) : which == B_A3000 ? a3000_write(lba, count, buf) : ide_write(lba, count, buf);
}
