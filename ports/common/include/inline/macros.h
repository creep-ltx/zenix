/* SPDX-License-Identifier: GPL-2.0-only */
/* The AmigaOS header of this name (calls into a library by its jump table), for the owner's programs on Zenix:
 * Markdown calls openurl.library's URL_OpenA with it, only once the library opened, which it never does on Zenix
 * (OpenLibrary is 0, amigados.h); the call is then 0 (no browser). */
#include <amigados.h>
#define LP2(offs, rt, name, t1, v1, r1, t2, v2, r2, bt, bn) ((void)(v1), (void)(v2), (void)(bn), (rt)0)
