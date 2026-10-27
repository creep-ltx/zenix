/* SPDX-License-Identifier: GPL-2.0-only */
/* Stop the CM4's Ethernet (GENET v5) that a previous owner left running (DESIGN D19 14, step 9's open point).
 *
 * When Zenix is started from a running AmigaOS (bootzenix, a warm reset, zenixboot from the Workbench), AmigaOS's
 * genet.device may leave the receive DMA writing frames into its old rings, which are Zenix's memory after the
 * takeover, until the ring is full; the send DMA may still read. stage0 calls this while AmigaOS is alive, after the
 * kernel image is loaded and before the copier moves it, whenever the device tree names a GENET. The order is
 * Linux's teardown (bcmgenet.c): the chip's interrupts masked (Emu68 owns the GIC; a stale interrupt must never reach
 * Zenix), the MAC's receive off (no new frame enters), both DMA engines off (receive first: it is the one writing into
 * memory), a bounded wait until both say stopped (a frame in flight lands now, not later), the MAC's send off. The
 * PHY, the rings and the link are left alone: Zenix's driver resets the chip when it starts (net/drv/genet-core.c).
 *
 * Plain C, shared by stage0 (boot/stage0.c) and the PC test against the chip model (tools/genet-test.c). The includer
 * provides u32, gs_rd(base, off) and gs_wr(base, off, value) with the register's value as the chip means it (stage0
 * swaps the bytes: the registers are little-endian), and gs_clock(), a counter that moves about every 64 us (the
 * beam's line on an Amiga) for the wait. Answers the GSTOP_* bits that kernel/hwinfo.h stores in hwinfo.genet_stop. */
#include "../kernel/hwinfo.h"
#include "../net/drv/genet.h"

static u32 genet_stop(u32 base)
{
    u32 rev = gs_rd(base, SYS_REV_CTRL);
    if (((rev >> 24) & 15) != 6)                /* not GENET v5 (6 is v5 in Linux's numbering): write nothing */
        return GSTOP_NOTV5;
    u32 r = GSTOP_DONE;
    u32 cmd = gs_rd(base, UMAC_CMD);
    u32 rdma = gs_rd(base, RDMA_CTRL), tdma = gs_rd(base, TDMA_CTRL);
    if (rdma & DMA_EN) r |= GSTOP_RX;
    if (tdma & DMA_EN) r |= GSTOP_TX;
    if (cmd & (CMD_RX_EN | CMD_TX_EN)) r |= GSTOP_MAC;
    gs_wr(base, INTRL2_0 + INTRL2_CPU_MASK_SET, 0xffffffff);
    gs_wr(base, INTRL2_0 + INTRL2_CPU_CLEAR, 0xffffffff);
    gs_wr(base, INTRL2_1 + INTRL2_CPU_MASK_SET, 0xffffffff);
    gs_wr(base, INTRL2_1 + INTRL2_CPU_CLEAR, 0xffffffff);
    if (!(cmd & CMD_SW_RESET))
        gs_wr(base, UMAC_CMD, cmd & ~CMD_RX_EN);
    gs_wr(base, RDMA_CTRL, 0);
    gs_wr(base, TDMA_CTRL, 0);
    /* at most 100 ms (about 1,560 lines of 64 us), as the driver's own wait; the read count is only a net for a
     * clock that never moves (never a delay of its own) */
    u32 last = gs_clock(), lines = 0, reads = 0;
    while (!(gs_rd(base, RDMA_STATUS) & DMA_DISABLED) || !(gs_rd(base, TDMA_STATUS) & DMA_DISABLED)) {
        u32 now = gs_clock();
        if (now != last) { last = now; lines++; }
        if (lines > 1560 || ++reads > 4000000) { r |= GSTOP_SLOW; break; }
    }
    cmd = gs_rd(base, UMAC_CMD);
    if (!(cmd & CMD_SW_RESET))
        gs_wr(base, UMAC_CMD, cmd & ~(CMD_RX_EN | CMD_TX_EN));
    return r;
}
