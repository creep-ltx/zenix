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
    int eth = fdt_path_offset(buf, "/scb/ethernet@7d580000");
    if (eth >= 0) { u8 mac[6] = { 0xdc, 0xa6, 0x32, 1, 2, 3 }; fdt_setprop(buf, eth, "local-mac-address", mac, 6); }

    struct ctx c = { buf };
    struct hwinfo hw; memset(&hw, 0, sizeof hw);
    stage0_fill(&c, &hw);

    CHECK("magic", hw.magic, HWINFO_MAGIC);
    CHECK("flags", hw.flags, HW_DT | HW_BCM2711 | HW_RANGEOPS);
    CHECK("soc_base", hw.soc_base, 0xF2000000);
    CHECK("mailbox", hw.mailbox, 0xF200B880);
    CHECK("gpio", hw.gpio, 0xF2200000);
    CHECK("emmc", hw.emmc, 0xF2340000);
    CHECK("gic_dist", hw.gic_dist, 0xF5841000);
    CHECK("gic_cpu", hw.gic_cpu, 0xF5842000);
    CHECK("genet", hw.genet, 0xF7580000);
    CHECK("genet_irq0", hw.genet_irq0, 0x9d);
    CHECK("genet_irq1", hw.genet_irq1, 0x9e);
    CHECK("phy_addr", hw.phy_addr, 0);
    CHECK("mac01", hw.mac[0] << 8 | hw.mac[1], 0xdca6);
    CHECK("vc4_base", hw.vc4_base, 0x30000000);
    CHECK("vc4_size", hw.vc4_size, 16 << 20);
    printf(fails ? "FAILED (%d)\n" : "all ok\n", fails);
    return fails != 0;
}
