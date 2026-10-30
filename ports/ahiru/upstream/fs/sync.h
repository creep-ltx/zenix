/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* sync - compare two folders, make one like the other (see sync.c) */
#ifndef CF_SYNC_H
#define CF_SYNC_H

#include "place.h"

/* marks in a and b what the other lacks, or holds older or different
 * (a folder: anything below differs); *na, *nb the counts. 0 = stopped */
int fs_compare(struct place *a, struct place *b, int *na, int *nb);
/* makes b like a: asks copy-only or mirror, plans, says how much, asks,
 * then copies (never an older file over a newer one). 1 = done */
int fs_sync(struct place *a, struct place *b);

#endif
