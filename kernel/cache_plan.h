/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* DESIGN D37 (docs/d37-data-cache.md section 3): which 16 MB units of the address space the 68040/68060's data cache
 * may hold, from the units that hold fast RAM and those that hold a board that is not RAM (bit u of fast[]/io[]: unit
 * u, A31-A24; unit 0 is ignored). Pure code: kernel/cache.c and the PC's test (tools/cache-host-test.c) include it.
 *
 * A transparent translation register matches A31-A24 against a base, ignoring the bits of a mask: an aligned block of
 * 2^n units. DTT0 takes unit 0 (chip RAM and every chip and I/O of the classic Amiga) inhibited and wins where DTT1
 * overlaps it (MC68040UM 3.4, MC68060UM 4.4). DTT1 gets the smallest aligned block over every fast RAM unit. Returns
 * 0 with *base, *mask set, or why there is no safe block:
 *   CP_NOFAST  no fast RAM above the first 16 MB
 *   CP_SHARED  a board that is not RAM in the block (*bad: its unit)
 *   CP_OUTSIDE a 68040 with such a board outside the block: the 040 caches every address no TTR matches
 *              (write-through, MC68040UM 3.1.2), so it would be cached (*bad: its unit). The 060's TCR default makes
 *              everything outside inhibited, so there this is no reason. */
#define CP_NOFAST  1
#define CP_SHARED  2
#define CP_OUTSIDE 3

static inline int cp_bit(const unsigned *m, unsigned u) { return (m[u >> 5] >> (u & 31)) & 1; }

static inline int cache_plan(const unsigned *fast, const unsigned *io, int is060, unsigned *base, unsigned *mask,
                             unsigned *bad)
{
    int lo = -1, hi = 0;
    for (unsigned u = 1; u < 256; u++)
        if (cp_bit(fast, u)) {
            if (lo < 0)
                lo = (int)u;
            hi = (int)u;
        }
    if (lo < 0)
        return CP_NOFAST;
    unsigned m = 0;
    while (((unsigned)lo & ~m) != ((unsigned)hi & ~m))
        m = (m << 1) | 1;
    *base = (unsigned)lo & ~m;
    *mask = m;
    for (unsigned u = 1; u < 256; u++)
        if (cp_bit(io, u)) {
            *bad = u;
            if ((u & ~m) == *base)
                return CP_SHARED;
            if (!is060)
                return CP_OUTSIDE;
        }
    return 0;
}
