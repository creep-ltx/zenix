/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* lha - LhA / LZH archives: list the members and unpack one.
 *
 * Header levels 0 to 3; methods -lh0- (stored), -lh1- (LHarc's
 * adaptive Huffman), -lh4- to -lh7- (static Huffman, 4K to 64K
 * windows), the old LArc -lzs-/-lz5-/-lz4-, and -lhd- directories.
 * The shape is arc.h's; arcent.method holds an LHA_* code, aux1 the
 * member's CRC-16 and aux2 its header level.
 */
#ifndef CF_LHA_H
#define CF_LHA_H

#include "arc.h"

enum {
    LHA_UNKNOWN = -1,           /* listed, but extract says ARC_EUNSUP */
    LHA_LH0, LHA_LH1, LHA_LH4, LHA_LH5, LHA_LH6, LHA_LH7,
    LHA_LZS, LHA_LZ5, LHA_LZ4, LHA_LHD
};

int lha_list(struct arcsrc *s, arc_addfn add, void *ctx);
int lha_extract(struct arcsrc *s, const struct arcent *e, struct arcsink *out);

#endif
