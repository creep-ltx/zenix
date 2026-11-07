/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* ZINE's memory planner (docs/d29-zine.md section 4): from the walk of Zenix's pools (the kernel's mem_walk, step 2)
 * it works out what is saved before the hand-over and put back after (every used chip block, each free chip block's
 * header, low memory, each free fast block's header), the program's free memory (chip: the chip pools and
 * 0x1000-0x1FFF, whole; fast: exactly Zenix's free blocks), the numbers `zine` prints, and the ZINE block's size.
 * Everything outside the pools (the kernel, the RAM disk, the klog area) is never given away. Plain C (zcore.h):
 * the kernel and the host tests build it. */
#include "zcore.h"

#define ZP_LOW      0x2000u             /* low memory, saved whole: 0-0x3FF the vectors, 0x400-0xFFF ZINE's low page */
#define ZP_PROGLOW  0x1000u             /* the program's chip memory starts here */
#define ZP_HDR      16u                 /* a Zenix block's header (kernel/mem.c) */
#define ZP_SUMENT   12u                 /* a checksum table entry: address, length, CRC32 (4.4) */
#define ZP_FIXSUMS  8u                  /* the sums besides the used blocks: kernel, BSS, RAM disk, klog, ... */
/* the ZINE block beyond the saved memory and the sums (4.2, 4.3; *proposed*, measured at step 3) */
#define ZP_CRCTAB   4096u               /* CRC32 slicing-by-4's table */
#define ZP_STACK    (16u << 10)         /* the supervisor stack */
#define ZP_RING     (16u << 10)         /* the output ring */
#define ZP_MARGIN   (16u << 10)

struct zp_region {                      /* a memory region as bootinfo gives it (kernel.h struct bootregion) */
    zu32 attrs, lower, upper;
};
struct zp_blk {                         /* one Zenix block, header included */
    zu32 addr, size, used;              /* used: 0 free, 1 Zenix's, ZP_KEEP the program's already (d29 16.12: its
                                         * hunks `zine` loaded; chip ones are neither saved nor given away, fast ones
                                         * count as the program's for the half-the-fast-RAM rule) */
};
#define ZP_KEEP 2
struct zp_range {                       /* one pool range [lo, hi) and its blocks, blk[first] .. blk[first + n - 1] */
    zu32 lo, hi, chip, first, n;
};
struct zp_in {
    const struct zp_region *reg;
    zu32 nreg;
    const struct zp_range *rng;
    zu32 nrng;
    const struct zp_blk *blk;
    zu32 nblk;
    zu32 blob_runtime;                  /* the blob's data and BSS (its header) */
};
struct zp_piece {                       /* memory saved before the hand-over and put back after */
    zu32 addr, len;
};
struct zp_chunk {                       /* free memory the program gets (a MemChunk), in region reg */
    zu32 addr, len, reg;
};
struct zp_out {
    struct zp_piece *piece;             /* the caller's arrays: nblk + 1 pieces and nblk + nrng + 1 chunks are enough */
    zu32 piece_cap, npiece;
    struct zp_chunk *chunk;
    zu32 chunk_cap, nchunk;
    zu32 save;                          /* bytes saved (all pieces) */
    zu32 chip_free, fast_free;          /* what the program gets */
    zu32 fast_total, fast_largest;
    zu32 fast_prog;                     /* fast bytes in ZP_KEEP blocks */
    zu32 nsum;                          /* checksum table entries */
    zu32 block;                         /* the ZINE block's bytes */
    int why;                            /* ZP_OK, or why ZINE refuses on this machine now */
};
#define ZP_OK      0
#define ZP_NOFAST  1                    /* no fast RAM: the saved memory has nowhere to go */
#define ZP_HOLDS   2                    /* Zenix holds more than half the fast RAM */

/* 0 and *out filled (out->why says whether ZINE may go on), -ZE_INVAL if the walk is not one consistent picture
 * (blocks that do not tile their range, ranges outside a region or over each other, a chip range in low memory),
 * -ZE_NOMEM if out's arrays are too small. */
int zp_plan(const struct zp_in *in, struct zp_out *out);
