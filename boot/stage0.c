/* SPDX-License-Identifier: GPL-2.0-only */
/* stage0: read the Pi hardware addresses out of Emu68's device tree.
 *
 * The bootblock (1024 bytes) is too small for this, so it calls this routine, which lives in
 * the kernel image, while AmigaOS is still alive (devicetree.resource is AmigaOS memory and
 * its functions are 68k code). It is position independent (built with -mpcrel, no writable
 * data): it runs before the kernel has been relocated. The same file is compiled on the
 * host against a real device tree for testing (tools/stage0-test.c).
 *
 * What Emu68 does that matters here (read from Emu68's raspi/start_rpi64.c): it rewrites the
 * "ranges" of /soc and /scb so that the child bus (the Pi's 0x7Exxxxxx / 0x7Cxxxxxx / 0x40xxxxxx
 * addresses) appears at consecutive 2 MB aligned 68k addresses starting at 0xF2000000. So
 * the 68k address of a register is: find the entry of the node's ancestor bus "ranges" that
 * contains the register's bus address, and add the entry's (rewritten) parent address. The
 * addresses are NOT fixed: GENET's depends on how big /soc's windows are.
 */
#include "../kernel/hwinfo.h"

typedef unsigned int u32;
typedef unsigned char u8;

#ifdef HOST_TEST
#include "../tools/dtapi_host.h"
#else
#include "dtapi_amiga.h"
#endif

static u32 cell(const void *v, u32 i)
{
    const u8 *p = (const u8 *)v + 4 * i;
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}


static int prefix(const char *s, const char *p)
{
    while (*p && *s == *p) { s++; p++; }
    return *p == 0;
}

/* value of a one-cell property, or d when absent */
static u32 cellprop(struct ctx *c, void *key, const char *name, u32 d)
{
    void *p = key ? dt_find(c, key, name) : 0;
    return (p && dt_len(c, p) >= 4) ? cell(dt_val(c, p), 0) : d;
}

/* the 68k address of Pi bus address "bus", through the ranges of bus node "path" */
static u32 translate(struct ctx *c, const char *path, u32 bus)
{
    void *key = dt_open(c, path);
    if (!key)
        return 0;
    void *par = dt_parent(c, key);
    u32 ab = cellprop(c, key, "#address-cells", 2);
    u32 ac = cellprop(c, par, "#address-cells", 2);
    u32 sb = cellprop(c, key, "#size-cells", 1);
    void *p = dt_find(c, key, "ranges");
    if (!p || ab == 0 || ac == 0 || sb == 0)
        return 0;
    const void *v = dt_val(c, p);
    u32 n = dt_len(c, p) / 4, step = ab + ac + sb;
    for (u32 i = 0; i + step <= n; i += step) {
        if (ab > 1 && cell(v, i) != 0)
            continue;                           /* above 4 GB: not for us */
        u32 b = cell(v, i + ab - 1);
        u32 cpu = cell(v, i + ab + ac - 1);
        u32 size = cell(v, i + step - 1);
        if (bus >= b && bus - b < size)
            return bus - b + cpu;
    }
    return 0;
}

/* first "reg" address of a node, as a bus address (the node's parent says how many cells) */
static u32 regaddr(struct ctx *c, void *key)
{
    void *p = dt_find(c, key, "reg");
    u32 ac = cellprop(c, dt_parent(c, key), "#address-cells", 2);
    if (!p || ac == 0 || dt_len(c, p) < 4 * ac)
        return 0;
    return cell(dt_val(c, p), ac - 1);
}

static void *alias(struct ctx *c, const char *name)
{
    void *a = dt_open(c, "/aliases");
    void *p = a ? dt_find(c, a, name) : 0;
    return p ? dt_open(c, (const char *)dt_val(c, p)) : 0;
}

static void *child_named(struct ctx *c, void *key, const char *pre)
{
    for (void *k = dt_child(c, key, 0); k; k = dt_child(c, key, k))
        if (prefix(dt_name(c, k), pre))
            return k;
    return 0;
}

static void *child_compat(struct ctx *c, void *key, const char *compat)
{
    for (void *k = dt_child(c, key, 0); k; k = dt_child(c, key, k)) {
        void *p = dt_find(c, k, "compatible");
        if (p && prefix((const char *)dt_val(c, p), compat))
            return k;
    }
    return 0;
}

/* the node (below "key") whose phandle is h: depth first */
static void *by_phandle(struct ctx *c, void *key, u32 h, int depth)
{
    if (cellprop(c, key, "phandle", 0) == h)
        return key;
    if (depth > 6)
        return 0;
    for (void *k = dt_child(c, key, 0); k; k = dt_child(c, key, k)) {
        void *r = by_phandle(c, k, h, depth + 1);
        if (r)
            return r;
    }
    return 0;
}

/* the Zorro boards AmigaOS configured, from expansion.library's ConfigDev list. A ConfigDev is: node (14
 * bytes), cd_Flags +14, an ExpansionRom at +16 (er_Type +16, er_Product +17, er_Flags +18, er_Manufacturer
 * +20 (16 bit), er_SerialNumber +22), cd_BoardAddr +32, cd_BoardSize +36. */
static u32 b32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

void stage0_boards(struct ctx *c, struct hwinfo *hw)
{
    void *cd = 0;
    hw->nboards = 0;
    while (hw->nboards < HW_MAXBOARDS && (cd = os_next_board(c, cd))) {
        const u8 *p = (const u8 *)cd;
        struct hwboard *b = &hw->board[hw->nboards++];
        b->cd_flags = p[14];
        b->type = p[16];
        b->product = p[17];
        b->er_flags = p[18];
        b->manufacturer = (unsigned short)(p[20] << 8 | p[21]);
        b->serial = b32(p + 22);
        b->addr = b32(p + 32);
        b->size = b32(p + 36);
    }
}

void stage0_fill(struct ctx *c, struct hwinfo *hw)
{
    void *k;
    hw->magic = HWINFO_MAGIC;
    hw->phy_addr = 0xFF;
    hw->flags = HW_DT;
    if (dt_open(c, "/scb"))
        hw->flags |= HW_BCM2711;

    if ((k = dt_open(c, "/emu68"))) {
        void *p = dt_find(c, k, "dcache-range-ops");
        if (p && dt_len(c, p) == 4 && cell(dt_val(c, p), 0) == 1)
            hw->flags |= HW_RANGEOPS;
        p = dt_find(c, k, "vc4-mem");
        if (p && dt_len(c, p) >= 8) {
            hw->vc4_base = cell(dt_val(c, p), 0);
            hw->vc4_size = cell(dt_val(c, p), 1);
        }
    }

    hw->soc_base = translate(c, "/soc", 0x7E000000);

    if ((k = alias(c, "mailbox")))
        hw->mailbox = translate(c, "/soc", regaddr(c, k));
    if ((k = alias(c, "gpio")))
        hw->gpio = translate(c, "/soc", regaddr(c, k));
    if ((k = alias(c, "emmc2bus")) && (k = child_named(c, k, "mmc@")))
        hw->emmc = translate(c, "/soc", regaddr(c, k));

    if ((k = dt_open(c, "/soc")) && (k = child_compat(c, k, "arm,gic-400"))) {
        void *p = dt_find(c, k, "reg");
        if (p && dt_len(c, p) >= 16) {              /* <dist size cpu size ...>, one cell each */
            hw->gic_dist = translate(c, "/soc", cell(dt_val(c, p), 0));
            hw->gic_cpu = translate(c, "/soc", cell(dt_val(c, p), 2));
        }
    }

    if ((k = dt_open(c, "/soc")) && (k = child_compat(c, k, "brcm,bcm2711-rng200")))
        hw->rng200 = translate(c, "/soc", regaddr(c, k));

    if ((k = alias(c, "ethernet0"))) {
        void *p;
        hw->genet = translate(c, "/scb", regaddr(c, k));
        if ((p = dt_find(c, k, "interrupts")) && dt_len(c, p) >= 24) {   /* <type nr flags> x2 */
            hw->genet_irq0 = cell(dt_val(c, p), 1);
            hw->genet_irq1 = cell(dt_val(c, p), 4);
        }
        if ((p = dt_find(c, k, "local-mac-address")) && dt_len(c, p) >= 6)
            for (int i = 0; i < 6; i++)
                hw->mac[i] = ((const u8 *)dt_val(c, p))[i];
        u32 h = cellprop(c, k, "phy-handle", 0);
        void *ph = h ? by_phandle(c, k, h, 0) : 0;
        if (ph) {
            void *r = dt_find(c, ph, "reg");
            if (r && dt_len(c, r) >= 4)
                hw->phy_addr = cell(dt_val(c, r), 0);
        }
    }
}


#ifndef HOST_TEST
/* exec.library OpenResource (-498) and OpenLibrary (-552): name in a1, (version 0 in d0), result in d0.
 * a6 is the compiler's frame pointer: it is saved on the stack around the call, never pinned to a variable. */
static void *exec_open_resource(void *execbase, const char *name)
{
    register void *a1 __asm__("a1") = (void *)name;
    register void *d0 __asm__("d0");
    __asm__ volatile("move.l %%a6,-(%%sp)\n\tmove.l %2,%%a6\n\t"
                     "jsr -498(%%a6)\n\tmove.l (%%sp)+,%%a6"
        : "=r"(d0), "+r"(a1) : "a"(execbase) : "d1", "a0", "cc", "memory");
    return d0;
}

static void *exec_open_library(void *execbase, const char *name)
{
    register void *a1 __asm__("a1") = (void *)name;
    register void *d0 __asm__("d0") = 0;
    __asm__ volatile("move.l %%a6,-(%%sp)\n\tmove.l %2,%%a6\n\t"
                     "jsr -552(%%a6)\n\tmove.l (%%sp)+,%%a6"
        : "+r"(d0), "+r"(a1) : "a"(execbase) : "d1", "a0", "cc", "memory");
    return d0;
}

/* called by the bootblock: a6 = ExecBase, a1 = the hwinfo table to fill (the bootblock's own block while AmigaOS
 * is alive; it lands at HWINFO_ADDR when the kernel runs) */
void stage0_main(void *execbase, struct hwinfo *hw)
{
    u8 *z = (u8 *)hw;
    for (unsigned i = 0; i < sizeof *hw; i++)
        z[i] = 0;
    hw->magic = HWINFO_MAGIC;
    hw->phy_addr = 0xFF;
    struct ctx c = { 0, 0 };
    c.exp = exec_open_library(execbase, "expansion.library");
    if (c.exp)
        stage0_boards(&c, hw);
    c.db = exec_open_resource(execbase, "devicetree.resource");
    if (c.db)
        stage0_fill(&c, hw);                    /* not Emu68: the rest of the table stays empty */
}

/* entry: saves what the C code may not preserve, passes (ExecBase, table in a1) */
__asm__(
    ".section .text.entry,\"ax\"\n"
    ".globl stage0_entry\n"
    "stage0_entry:\n"
    "    movem.l %d2-%d7/%a2-%a6,-(%sp)\n"
    "    move.l %a1,-(%sp)\n"
    "    move.l %a6,-(%sp)\n"
    "    jsr stage0_main(%pc)\n"
    "    addq.l #8,%sp\n"
    "    movem.l (%sp)+,%d2-%d7/%a2-%a6\n"
    "    rts\n"
    ".text\n");
#endif
