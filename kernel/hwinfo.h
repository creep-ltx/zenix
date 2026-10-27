/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* Hardware table for the Raspberry Pi side of an Emu68 machine (PiStorm).
 *
 * Written by stage0 (boot/stage0.c) while AmigaOS is still alive, from the device tree
 * that Emu68 publishes as devicetree.resource, and read by the drivers. All addresses
 * are the ones the 68k must use: Emu68 rewrites the "ranges" of /soc and /scb so that
 * the Pi's peripherals appear from 0xF2000000 upwards, in the order of the ranges.
 * On a machine without Emu68 (no devicetree.resource) the flags are 0 and every
 * address is 0; boards[] is filled on every Amiga. Notes: Knowledge/amiga/emu68-sd-controller-registers.md and
 * emu68-network-usb-rtg-hardware.md.
 */
#define HWINFO_ADDR     0x1100          /* in the low chip RAM the kernel never hands out; the bootblock copies the table here after the takeover */
#define HWINFO_MAGIC    0x5A485731      /* "ZHW1" */
#define HWINFO_DRYRUN   0x5A485744      /* "ZHWD": put in the magic word by a caller that may still go back to AmigaOS
                                         * (zenixboot CHECK): stage0 then fills the table but writes no hardware */

#define HW_DT           0x01            /* devicetree.resource found: running under Emu68 */
#define HW_BCM2711      0x02            /* /scb present: Pi 4 / CM4 */
#define HW_RANGEOPS     0x04            /* /emu68 dcache-range-ops == 1: LINE-F cache range opcodes exist */

/* hwinfo.genet_stop (boot/genet-stop.h, DESIGN D19 14: the warm start from AmigaOS) */
#define GSTOP_DONE      0x01            /* a GENET v5 was found and stopped: interrupts masked, the MAC and both DMAs off */
#define GSTOP_RX        0x02            /* its receive DMA was running (AmigaOS's driver left it on) */
#define GSTOP_TX        0x04            /* its send DMA was running */
#define GSTOP_MAC       0x08            /* the MAC's receive or send was on */
#define GSTOP_SLOW      0x10            /* the DMA did not say "stopped" within 100 ms */
#define GSTOP_NOTV5     0x20            /* the address held no GENET v5: nothing written */

#define HW_MAXBOARDS 16

/* one configured Zorro expansion board, copied from expansion.library's ConfigDev list */
struct hwboard {
    unsigned short manufacturer;    /* er_Manufacturer: 0x6D73 'ms' is Emu68's own */
    unsigned char product;          /* er_Product */
    unsigned char type;             /* er_Type: bits 7-6 = 11 Zorro II / 10 Zorro III, bit 5 = RAM to add to the memory list, bit 4 = has a diag area (ROM), bit 3 = chained to the next */
    unsigned int serial;            /* er_SerialNumber */
    unsigned int addr;              /* cd_BoardAddr */
    unsigned int size;              /* cd_BoardSize */
    unsigned char er_flags;         /* er_Flags */
    unsigned char cd_flags;         /* cd_Flags (CDF_CONFIGME 2, CDF_SHUTUP 1, CDF_BADMEMORY 8, ...) */
    unsigned short pad;
};

struct hwinfo {
    unsigned int magic;
    unsigned int flags;
    unsigned int soc_base;      /* where the Pi bus address 0x7E000000 appears for the 68k */
    unsigned int mailbox;       /* VideoCore mailbox registers */
    unsigned int emmc;          /* EMMC2 (the SD slot on a Pi 4 / CM4) registers */
    unsigned int gpio;
    unsigned int genet;         /* GENET Ethernet registers (/scb) */
    unsigned int genet_irq0;    /* GIC SPI numbers from the device tree (add 32 for the GIC interrupt id) */
    unsigned int genet_irq1;
    unsigned int phy_addr;      /* MDIO address of the PHY, 0xFF = unknown */
    unsigned char mac[6];       /* local-mac-address from the firmware */
    unsigned short pad;
    unsigned int gic_dist;      /* GIC-400 distributor and CPU interface */
    unsigned int gic_cpu;
    unsigned int vc4_base;      /* /emu68 vc4-mem: RAM reserved as the graphics framebuffer */
    unsigned int vc4_size;
    unsigned int nboards;       /* Zorro boards AmigaOS configured (expansion.library), valid entries in board[] */
    unsigned int rng200;        /* the BCM2711's random number generator (RNG200, "brcm,bcm2711-rng200" in /soc), 0 if
                                 * none: credited fully by the entropy pool (DESIGN D19 9.3) */
    unsigned int genet_stop;    /* what stage0 did to a GENET a previous owner may have left running: GSTOP_* bits, 0 if
                                 * there was none (or a dry run); the driver and the boot log say it */
    unsigned int reserved[3];
    struct hwboard board[HW_MAXBOARDS];
};
/* User-visible since DESIGN D19 step 7 (SYS_drv DRV_HWINFO gives drivers a copy, at most the caller's size): it grows
 * only at the end from here, new fields first from reserved[], then after board[]; its first size is frozen */
#define HWINFO_FIRST 408
