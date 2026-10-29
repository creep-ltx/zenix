/* SPDX-License-Identifier: GPL-2.0-only */
/* utf8.library's calls Maki's upstream source makes (composing an accent written apart onto its letter), for its
 * Zenix build: the library never opens on Zenix (OpenLibrary is 0, amigados.h), so upstream leaves such accents out,
 * as it does on AmigaOS without the library; these are never reached. */
#include <amigados.h>
#define U8F_COMPOSE 1
static inline LONG Utf8Encode(ULONG c, STRPTR out) { (void)c; out[0] = 0; return 0; }
static inline LONG Utf8Decode(CONST_STRPTR s, LONG n, ULONG *c) { (void)s; (void)n; *c = 0; return -1; }
static inline LONG Utf8Normalize(CONST_STRPTR s, LONG n, STRPTR out, LONG cap, ULONG flags)
{
    (void)s; (void)n; (void)flags;
    if (cap > 0) out[0] = 0;
    return 0;
}
