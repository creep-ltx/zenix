/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* zip - the built-in zip reader: list and extract, stored and deflate.
 *
 * The central directory is the list (found from the end record, which
 * sits in the last 64K+22 bytes); each member's data is found through
 * its local header at extract time. Deflate is our own inflate
 * (RFC 1951) with a 32K window. Plain C, no Amiga calls (see arc.h);
 * proven by tests/zip.sh against 7z and unzip, on the host and under
 * vamos.
 *
 * What a listed member carries for zip_extract:
 *   dataoff  the local header's offset (already moved by any bytes in
 *            front of the archive, as in a self-extractor)
 *   method   0 stored, 8 deflate, anything else is listed but gives
 *            ARC_EUNSUP; -1 for a ZIP64 member too big for 32 bits
 *   aux1     the CRC-32
 *   aux2     the general purpose flags (bit 0: encrypted)
 */
#ifndef CF_ZIP_H
#define CF_ZIP_H

#include "arc.h"

/* one add() per member, in the central directory's order. ARC_OK,
 * ARC_ESTOP when add() said stop, or an error code. */
int zip_list(struct arcsrc *s, arc_addfn add, void *ctx);

/* one member's bytes to out, a window (32K) at a time; the size and
 * the CRC are checked at the end (ARC_ECRC). */
int zip_extract(struct arcsrc *s, const struct arcent *e, struct arcsink *out);

#endif
