/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* sums - checksums of files (fs side; MD5 and the line formats are in
 * core/sums.c). One command, two ways:
 *   on a .md5 or .sfv file (nothing marked): every file it names, in its
 *     folder, is read and checked - a list of ok / BAD / missing
 *   else: MD5 and CRC-32 of the marked files (or the one under the
 *     cursor), shown, and on Return saved as a .md5 file (md5sum's
 *     shape: md5sum -c reads it, and so does this command) */
#ifndef CF_FSSUMS_H
#define CF_FSSUMS_H

#include "place.h"

int fs_checksums(struct place *p, int cur);

#endif
