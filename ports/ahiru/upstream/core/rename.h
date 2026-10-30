/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* rename - new names from a pattern (plain C, tested on the host).
 *
 * from matches the old name: * any run of characters (the shortest that
 * works), ? one, the rest as it is, without case ("" = *). to is the new
 * name: its first * takes what the first * of from matched, the second
 * the second, and so on; ? likewise, numbered apart from the *s; # is a
 * running number, ## two digits and so on.
 *   *.JPG  -> *.jpg        Holiday.JPG -> Holiday.jpg
 *   *      -> pic-##-*     x -> pic-07-x
 *   ??*    -> *            drops the first two characters
 *   mod.*  -> *.mod        mod.space debris -> space debris.mod
 * Returns 1 and out when name matches; 0 when not (or out too small). */
#ifndef CF_RENAME_H
#define CF_RENAME_H

int ren_make(const char *from, const char *to, const char *name, long num, char *out, int size);

#endif
