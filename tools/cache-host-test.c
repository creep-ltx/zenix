/* SPDX-License-Identifier: GPL-2.0-only */
/* DESIGN D37: kernel/cache_plan.h on the PC. Each machine's memory and boards, the block DTT1 gets (or why none), and
 * for every unit whether the data cache would hold it: the block's units except unit 0, never a board's. */
#include <stdio.h>
#include <string.h>
#include "../kernel/cache_plan.h"

static int fails;

struct area { unsigned lo, hi; };
static void mark(unsigned *m, struct area a)
{
    for (unsigned u = a.lo >> 24; u <= (a.hi - 1) >> 24; u++)
        m[u >> 5] |= 1u << (u & 31);
}

static void t(const char *name, int is060, const struct area *fast, const struct area *io, int want, unsigned wbase,
              unsigned wmask)
{
    unsigned f[8] = { 0 }, b[8] = { 0 }, base = 0, mask = 0, bad = 0;
    for (; fast && fast->hi; fast++)
        mark(f, *fast);
    for (; io && io->hi; io++)
        mark(b, *io);
    int k = cache_plan(f, b, is060, &base, &mask, &bad);
    int ok = k == want && (k || (base == wbase && mask == wmask));
    if (!k)                             /* the safety property: no board's unit is cached, unit 0 never */
        for (unsigned u = 0; u < 256; u++)
            if ((u & ~mask) == base && u && cp_bit(b, u))
                ok = 0;
    printf("%s %s: %s", ok ? "PASS" : "FAIL", name, k ? "no data cache" : "DTT1");
    if (!k)
        printf(" base %02x mask %02x (0x%08x-0x%08x)", base, mask, base << 24, ((base | mask) << 24) | 0xFFFFFF);
    else if (bad)
        printf(" (unit %02x)", bad);
    printf("\n");
    fails += !ok;
}

int main(void)
{
    static const struct area a1260[] = { { 0x01000000, 0x05000000 }, { 0 } };
    static const struct area uae_mb[] = { { 0x08000000, 0x0C000000 }, { 0 } };
    static const struct area uae_z3[] = { { 0x40000000, 0x44000000 }, { 0 } };
    static const struct area z2only[] = { { 0x00200000, 0x00A00000 }, { 0 } };
    static const struct area a4000[] = { { 0x07000000, 0x08000000 }, { 0x08000000, 0x10000000 }, { 0 } };
    static const struct area a4000z3[] = { { 0x07000000, 0x08000000 }, { 0x40000000, 0x41000000 }, { 0 } };
    static const struct area mixed[] = { { 0x00200000, 0x00A00000 }, { 0x08000000, 0x08800000 }, { 0 } };
    static const struct area gfx[] = { { 0x40000000, 0x42000000 }, { 0 } };       /* a Zorro III graphics card */
    static const struct area gfx2[] = { { 0x04000000, 0x04010000 }, { 0 } };      /* a board inside the 1260's block */
    static const struct area z2io[] = { { 0x00E90000, 0x00EA0000 }, { 0 } };     /* Zorro II I/O: unit 0, no matter */

    t("Apollo 1260, 64 MB (060)", 1, a1260, 0, 0, 0x00, 0x07);
    t("Apollo 1260, 64 MB (040 rules)", 0, a1260, 0, 0, 0x00, 0x07);
    t("FS-UAE processor-slot RAM", 1, uae_mb, 0, 0, 0x08, 0x03);
    t("FS-UAE Zorro III RAM", 0, uae_z3, 0, 0, 0x40, 0x03);
    t("Zorro II fast only", 1, z2only, 0, CP_NOFAST, 0, 0);
    t("no fast RAM", 1, 0, 0, CP_NOFAST, 0, 0);
    t("A4000 motherboard + CPU card RAM", 1, a4000, 0, 0, 0x00, 0x0F);
    t("A4000 RAM + Zorro III RAM", 1, a4000z3, 0, 0, 0x00, 0x7F);
    t("Zorro II and 32-bit fast", 1, mixed, z2io, 0, 0x08, 0x00);
    t("060 + Zorro III graphics card outside the block", 1, a1260, gfx, 0, 0x00, 0x07);
    t("040 + Zorro III graphics card outside the block", 0, a1260, gfx, CP_OUTSIDE, 0, 0);
    t("a board inside the block", 1, a1260, gfx2, CP_SHARED, 0, 0);
    t("A4000 RAM + Z3 RAM + Z3 graphics inside", 1, a4000z3, gfx, CP_SHARED, 0, 0);
    printf("%s\n", fails ? "cache-host-test: FAIL" : "cache-host-test: ok");
    return fails != 0;
}
