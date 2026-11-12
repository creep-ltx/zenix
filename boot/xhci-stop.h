/* SPDX-License-Identifier: GPL-2.0-only */
/* Halt the BCM2711's xHCI USB controller that a previous owner left running (DESIGN D46 3.6, the warm start).
 *
 * When Zenix is started from a running AmigaOS (zenixboot from the Workbench), Poseidon/Neptune's xhci.device has the
 * controller running: its event ring and the keyboard's interrupt transfers are written by DMA into AmigaOS memory,
 * which is Zenix's after the takeover. stage0 calls this while AmigaOS is alive, as the last hardware step before the
 * takeover, whenever the hardware table names an xHCI. Run/Stop and the interrupt enable are cleared in USBCMD and the
 * controller is given up to 100 ms to say HCHalted (xHCI 1.2 5.4.1: within 16 ms). No reset: the USB server resets the
 * controller when it starts. The registers are little-endian; the includer's gs_rd/gs_wr swap (as for genet-stop.h),
 * gs_clock() moves about every 64 us; gs_wait() is genet-stop.h's. Answers the XSTOP_* bits kernel/hwinfo.h stores in hwinfo.xhci_stop. */
#include "../kernel/hwinfo.h"

static u32 xhci_stop(u32 base)
{
    u32 cap = gs_rd(base, 0), len = cap & 0xFF, ver = cap >> 16;
    if (len < 0x20 || len > 0x80 || ver < 0x0090 || ver > 0x0120)
        return XSTOP_NOTXHCI;                   /* CAPLENGTH or HCIVERSION make no sense (as usbd's xc_probe): nothing */
    u32 r = XSTOP_DONE;
    if (gs_rd(base, len + 4) & 1)               /* USBSTS.HCHalted: already halted */
        return r;
    r |= XSTOP_RAN;
    gs_wr(base, len, gs_rd(base, len) & ~5u);   /* USBCMD: Run/Stop (bit 0) and INTE (bit 2) off */
    if (gs_wait(base, len + 4, 1))              /* genet-stop.h's wait: at most 100 ms */
        r |= XSTOP_SLOW;
    return r;
}
