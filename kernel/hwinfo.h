/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* Hardware table for the Raspberry Pi side of an Emu68 machine (PiStorm).
 *
 * Written by stage0 (boot/stage0.c) while AmigaOS is still alive, from the device tree
 * that Emu68 publishes as devicetree.resource, and read by the drivers. All addresses
 * are the ones the 68k must use: Emu68 rewrites the "ranges" of /soc and /scb so that
 * the Pi's peripherals appear from 0xF2000000 upwards, in the order of the ranges.
 * On a machine without Emu68 (no devicetree.resource) the flags are 0 and every
 * address is 0. Notes: Knowledge/amiga/emu68-sd-controller-registers.md and
 * emu68-network-usb-rtg-hardware.md.
 */
#define HWINFO_ADDR     0x1100          /* in the low chip RAM the kernel never hands out */
#define HWINFO_MAGIC    0x5A485731      /* "ZHW1" */

#define HW_DT           0x01            /* devicetree.resource found: running under Emu68 */
#define HW_BCM2711      0x02            /* /scb present: Pi 4 / CM4 */
#define HW_RANGEOPS     0x04            /* /emu68 dcache-range-ops == 1: LINE-F cache range opcodes exist */

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
    unsigned int reserved[6];
};
