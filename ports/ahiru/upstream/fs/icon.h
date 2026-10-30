/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* icon - an icon's tooltypes as text and back, byte by byte: the .info
 * is rewritten as prefix + a fresh tooltype block + the untouched
 * suffix, so images, position and any colour appendix survive exactly.
 * No icon.library needed. opmsg says why on failure. */
#ifndef CF_ICON_H
#define CF_ICON_H

int tt_export(const char *icon, const char *file);  /* one tooltype a line */
int tt_import(const char *icon, const char *file);  /* the lines back; 1 = written */

#endif
