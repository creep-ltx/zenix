/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* renames - many renames at once (see renames.c) */
#ifndef CF_RENAMES_H
#define CF_RENAMES_H

#include "place.h"
#include "ops.h"

/* entries idx[] to the names newn[] (checked first, two steps); 1 = done */
int fs_renameset(struct place *p, const int *idx, char (*newn)[NAMEMAX + 4], int n);
/* asks a from-pattern and a to-name (core/rename.h), shows the list of
 * old -> new through asklist(), renames on yes. The marked, else all */
int fs_patrename(struct place *p, int cur);

#endif
