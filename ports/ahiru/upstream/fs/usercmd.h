/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* usercmd - your own commands on keys: "run Echo {file}" (see usercmd.c) */
#ifndef CF_USERCMD_H
#define CF_USERCMD_H

#include "place.h"

/* the template with its {words} filled in, each a quoted path or name:
 *   {file}   the entry under the cursor, its whole path
 *   {name}   its name            {dir}    the folder
 *   {marked} the marked ones' paths, a space between ({file} when none)
 *   {other}  the other folder (CFile's other pane, Ahiru's next tab)
 * other may be NULL. 0 = too long, {file} etc. with nothing there, or a
 * name holding a '"' or a control character (it cannot be quoted) */
int uc_expand(const char *tpl, struct place *p, int cur, const char *other, char *out, int size);

#endif
