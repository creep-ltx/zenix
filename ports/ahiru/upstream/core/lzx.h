/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* lzx - Amiga LZX archives (LZX 1.21), read only: list and unpack.
 *
 * A port of unlzx.c 1.1 by way of BBX's Go reader (internal/diz/lzx).
 * LZX "merges" small files into one packed stream; a member inside such
 * a group is unpacked by running the group's stream from its start and
 * throwing away the bytes before the member - in a 64K window, so the
 * group is never held in memory whole. Methods: stored (0) and normal
 * (2); others give ARC_EUNSUP.
 *
 * What a listed member carries for lzx_extract (opaque to the program):
 *   dataoff  where the packed data of its group starts in the archive
 *   aux1     the group's packed length (0: no data was found for it)
 *   aux2     where the member starts in the group's unpacked stream
 *   aux3     the member's CRC-32
 *   method   the group's pack mode
 *
 * Dates: LZX 1.21 keeps the year in 6 bits from 1970 and gets 2000 and
 * later wrong. Read here as: 58-63 = 2000-2005, 0-7 = 2006-2013 (no
 * Amiga file is from before 1978), the rest 1970 + n. Files from 2014
 * on come out as 1978-1999, and from 2006 on LZX has also set the low
 * bit of the month (July is stored as August): as LZX itself shows them.
 */
#ifndef CF_LZX_H
#define CF_LZX_H

#include "arc.h"

/* one add() per member, in archive order. ARC_ECORRUPT after the
 * members before the damage when the archive is damaged or cut short;
 * ARC_ECORRUPT with no add() at all when it is not an LZX archive. */
int lzx_list(struct arcsrc *s, arc_addfn add, void *ctx);

/* unpack one listed member to out, its CRC-32 checked */
int lzx_extract(struct arcsrc *s, const struct arcent *e, struct arcsink *out);

/* frees the decoder lzx_extract keeps for the next member of a group;
   call when done with the archive (or before its source goes away) */
void lzx_end(void);

#endif
