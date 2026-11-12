/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of boot/stage0.c against a real Raspberry Pi device tree (.dtb) after the
 * same "ranges" rewriting Emu68 does (raspi/start_rpi64.c map_peripheral_ranges), plus the
 * /emu68 properties and a MAC address. Expected values were worked out by hand from the
 * CM4 tree: /soc windows 0x1800000 + 0x2000000 + 0x800000 from 0xF2000000, then /scb.
 * Build and run: tools/stage0-test.sh <bcm2711-rpi-cm4.dtb> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define HOST_TEST 1
typedef unsigned int u32;
/* a GENET's register window as a previous owner left it (the receive and send DMA on, the MAC on), and the beam */
static u32 genet_regs[0x10000 / 4], genet_at, genet_writes;
/* an xHCI's first registers (DESIGN D46): CAPLENGTH 0x20, HCIVERSION 1.0; USBCMD at 0x20, USBSTS at 0x24 */
static u32 xhci_regs[0x40 / 4], xhci_at, xhci_writes, xhci_slow;
static u32 gs_rd(u32 base, u32 off)
{
    if (base == xhci_at && off < sizeof xhci_regs)
        return xhci_regs[off / 4];
    return base == genet_at ? genet_regs[off / 4] : 0xdeadbeef;
}
static void gs_wr(u32 base, u32 off, u32 v)
{
    if (base == xhci_at && off < sizeof xhci_regs) {
        xhci_regs[off / 4] = v;
        xhci_writes++;
        if (off == 0x20 && !(v & 1) && !xhci_slow) xhci_regs[0x24 / 4] |= 1;    /* Run/Stop off: halted */
        return;
    }
    if (base != genet_at) { printf("a write outside the GENET: %x+%x\n", base, off); genet_writes += 1000; return; }
    genet_regs[off / 4] = v;
    genet_writes++;
    if (off == 0x3044 && !(v & 1)) genet_regs[0x3048 / 4] |= 1;   /* the DMA says stopped */
    if (off == 0x5044 && !(v & 1)) genet_regs[0x5048 / 4] |= 1;
}
static u32 beam;
static u32 gs_clock(void) { return ++beam; }
#include "../boot/stage0.c"

static u32 start_map = 0xf20 / 2;

static void map_ranges(void *fdt, const char *path)
{
    int n = fdt_path_offset(fdt, path);
    if (n < 0) return;
    int len;
    u32 *r = (u32 *)fdt_getprop_w(fdt, n, "ranges", &len);
    if (!r) return;
    int acpu = fdt32_to_cpu(*(u32 *)fdt_getprop(fdt, fdt_parent_offset(fdt, n), "#address-cells", &len));
    const u32 *p = fdt_getprop(fdt, n, "#address-cells", &len); int abus = p ? fdt32_to_cpu(*p) : 2;
    p = fdt_getprop(fdt, n, "#size-cells", &len); int sbus = p ? fdt32_to_cpu(*p) : 1;
    int is_scb = !strcmp(path, "/scb");
    int plen; fdt_getprop(fdt, n, "ranges", &plen);
    int step = abus + acpu + sbus;
    for (int i = 0; i * 4 < plen; i += step) {
        u32 b = fdt32_to_cpu(r[i + abus - 1]);
        u32 cpu = fdt32_to_cpu(r[i + abus + acpu - 1]);
        u32 size = fdt32_to_cpu(r[i + step - 1]);
        if (is_scb && b == cpu) continue;               /* "ignore large identity mappings" */
        r[i + abus + acpu - 1] = cpu_to_fdt32(start_map << 21);
        start_map += (size + (1 << 21) - 1) >> 21;
    }
}

static int fails;
#define CHECK(name, got, want) do { printf("%-12s %08x %s\n", name, (u32)(got), (u32)(got) == (u32)(want) ? "ok" : "WRONG"); \
    if ((u32)(got) != (u32)(want)) { printf("  expected %08x\n", (u32)(want)); fails++; } } while (0)

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s file.dtb\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror(argv[1]); return 2; }
    static char in[1 << 18], buf[1 << 19];
    size_t n = fread(in, 1, sizeof in, f); fclose(f);
    if (fdt_open_into(in, buf, sizeof buf) < 0) { fprintf(stderr, "bad dtb (%zu bytes)\n", n); return 2; }
    map_ranges(buf, "/soc");
    map_ranges(buf, "/scb");
    int root = 0, e = fdt_add_subnode(buf, root, "emu68");
    fdt_setprop_u32(buf, e, "dcache-range-ops", 1);
    u32 vc[2] = { cpu_to_fdt32(0x30000000), cpu_to_fdt32(16 << 20) };
    fdt_setprop(buf, e, "vc4-mem", vc, 8);
    int xh = fdt_path_offset(buf, "/scb/xhci@7e9c0000");      /* what the firmware does for otg_mode=1 */
    if (xh >= 0) fdt_setprop_string(buf, xh, "status", "okay");
    int eth = fdt_path_offset(buf, "/scb/ethernet@7d580000");
    if (eth >= 0) { u8 mac[6] = { 0xdc, 0xa6, 0x32, 1, 2, 3 }; fdt_setprop(buf, eth, "local-mac-address", mac, 6); }

    /* two fake ConfigDevs: a Zorro II RAM board and an Emu68-style Zorro III ROM board */
    static u8 cd0[56], cd1[56];
    static void *boards[3] = { cd0, cd1, 0 };
    cd0[14] = 0x02; cd0[16] = 0xE0; cd0[17] = 0x0A; cd0[18] = 0x40; cd0[20] = 0x08; cd0[21] = 0x2C;
    cd0[22] = 0x12; cd0[23] = 0x34; cd0[24] = 0x56; cd0[25] = 0x78;
    cd0[32] = 0x00; cd0[33] = 0x20; cd0[34] = 0x00; cd0[35] = 0x00; cd0[37] = 0x40;          /* 0x00200000, 0x00400000 */
    cd1[14] = 0x00; cd1[16] = 0x90; cd1[17] = 0x21; cd1[20] = 0x6D; cd1[21] = 0x73;
    cd1[32] = 0x40; cd1[33] = 0x00; cd1[34] = 0x00; cd1[35] = 0x00; cd1[37] = 0x01;          /* 0x40000000, 0x00010000 */

    struct ctx c = { buf, boards };
    struct hwinfo hw; memset(&hw, 0, sizeof hw);
    stage0_fill(&c, &hw);
    stage0_boards(&c, &hw);

    CHECK("magic", hw.magic, HWINFO_MAGIC);
    CHECK("flags", hw.flags, HW_DT | HW_BCM2711 | HW_RANGEOPS);
    CHECK("soc_base", hw.soc_base, 0xF2000000);
    CHECK("mailbox", hw.mailbox, 0xF200B880);
    CHECK("gpio", hw.gpio, 0xF2200000);
    CHECK("emmc", hw.emmc, 0xF2340000);
    CHECK("gic_dist", hw.gic_dist, 0xF5841000);
    CHECK("gic_cpu", hw.gic_cpu, 0xF5842000);
    CHECK("rng200", hw.rng200, 0xF2104000);         /* /soc/rng@7e104000 (DESIGN D19 9.3) */
    CHECK("genet", hw.genet, 0xF7580000);
    CHECK("genet_irq0", hw.genet_irq0, 0x9d);
    CHECK("genet_irq1", hw.genet_irq1, 0x9e);
    CHECK("phy_addr", hw.phy_addr, 0);
    CHECK("mac01", hw.mac[0] << 8 | hw.mac[1], 0xdca6);
    CHECK("vc4_base", hw.vc4_base, 0x30000000);
    CHECK("vc4_size", hw.vc4_size, 16 << 20);
    CHECK("nboards", hw.nboards, 2);
    CHECK("b0 mfr", hw.board[0].manufacturer, 0x082C);
    CHECK("b0 product", hw.board[0].product, 0x0A);
    CHECK("b0 type", hw.board[0].type, 0xE0);
    CHECK("b0 serial", hw.board[0].serial, 0x12345678);
    CHECK("b0 addr", hw.board[0].addr, 0x00200000);
    CHECK("b0 size", hw.board[0].size, 0x00400000);
    CHECK("b0 cdflags", hw.board[0].cd_flags, 2);
    CHECK("b0 erflags", hw.board[0].er_flags, 0x40);
    CHECK("b1 mfr", hw.board[1].manufacturer, 0x6D73);
    CHECK("b1 product", hw.board[1].product, 0x21);
    CHECK("b1 addr", hw.board[1].addr, 0x40000000);
    CHECK("b1 size", hw.board[1].size, 0x00010000);

    /* the GENET a previous owner left running: stopped, unless the caller is a dry run (zenixboot CHECK) */
    genet_at = hw.genet;
    genet_regs[0] = 0x06010000;                                 /* SYS_REV_CTRL: v5.1 */
    genet_regs[0x3044 / 4] = genet_regs[0x5044 / 4] = 0x20001;  /* RDMA_CTRL, TDMA_CTRL: ring 16, on */
    genet_regs[0x808 / 4] = 0xb;                                /* UMAC_CMD: 1000 Mbit/s, receive and send on */
    stage0_stop(&hw, 1);
    CHECK("dry run", genet_writes << 8 | hw.genet_stop, 0);
    stage0_stop(&hw, 0);
    CHECK("genet_stop", hw.genet_stop, GSTOP_DONE | GSTOP_RX | GSTOP_TX | GSTOP_MAC);
    CHECK("RDMA_CTRL", genet_regs[0x3044 / 4], 0);
    CHECK("TDMA_CTRL", genet_regs[0x5044 / 4], 0);
    CHECK("UMAC_CMD", genet_regs[0x808 / 4], 0x8);
    CHECK("INTRL2 mask", genet_regs[0x210 / 4] & genet_regs[0x250 / 4], 0xffffffff);
    CHECK("writes", genet_writes, 8);

    /* DESIGN D46: the xHCI found through /__symbols__ (0x7e9c0000 in /scb's first range, 0x7c000000 -> 0xf6000000) */
    CHECK("xhci", hw.xhci, 0xF89C0000);
    CHECK("xhci_size", hw.xhci_size, 0x100000);
    CHECK("xhci_stop (an address without an xHCI)", hw.xhci_stop, XSTOP_NOTXHCI);     /* the GENET checks' stops */
    hw.xhci_stop = 0;
    xhci_at = hw.xhci;
    xhci_regs[0] = 0x01000020;                                  /* HCIVERSION 1.0, CAPLENGTH 0x20 */
    xhci_regs[0x20 / 4] = 0x5;                                  /* USBCMD: Run/Stop, INTE */
    xhci_regs[0x24 / 4] = 0;                                    /* USBSTS: running */
    genet_writes = 0;
    stage0_stop(&hw, 1);
    CHECK("xhci dry run", xhci_writes << 8 | hw.xhci_stop, 0);
    stage0_stop(&hw, 0);
    CHECK("xhci_stop running", hw.xhci_stop, XSTOP_DONE | XSTOP_RAN);
    CHECK("USBCMD", xhci_regs[0x20 / 4], 0);
    CHECK("xhci writes", xhci_writes, 1);
    xhci_writes = 0;
    stage0_stop(&hw, 0);
    CHECK("xhci_stop halted already", hw.xhci_stop << 8 | xhci_writes, XSTOP_DONE << 8);
    xhci_regs[0x20 / 4] = 1; xhci_regs[0x24 / 4] = 0; xhci_slow = 1;
    stage0_stop(&hw, 0);
    CHECK("xhci_stop never halts", hw.xhci_stop, XSTOP_DONE | XSTOP_RAN | XSTOP_SLOW);
    xhci_regs[0] = 0x12345678;                                  /* not an xHCI */
    xhci_writes = 0;
    stage0_stop(&hw, 0);
    CHECK("not an xhci", hw.xhci_stop << 8 | xhci_writes, XSTOP_NOTXHCI << 8);
    xh = fdt_path_offset(buf, "/scb/xhci@7e9c0000");          /* offsets moved with the properties added since */
    if (xh >= 0) fdt_setprop_string(buf, xh, "status", "disabled");
    struct hwinfo h2; memset(&h2, 0, sizeof h2);
    stage0_fill(&c, &h2);
    CHECK("xhci disabled (no otg_mode=1)", h2.xhci | h2.xhci_size, 0);
    hw.xhci = 0; hw.xhci_stop = 0;

    hw.genet = 0; hw.genet_stop = 0; genet_writes = 0;
    stage0_stop(&hw, 0);
    CHECK("no genet", genet_writes << 8 | hw.genet_stop, 0);
    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails != 0;
}
