/* SPDX-License-Identifier: GPL-2.0-only */
/* utf8.library's two calls the text-UI library's upstream source makes in its TUI_LIB build, which Zenix compiles
 * only with the widths switch on (ZENIX_TUI_UTF8, ports/tui/README): a character's width is the C library's
 * wcwidth (the same tables as the console's, DESIGN D28 section 9); composing a letter and its accents is not there
 * (picolibc has no normalisation), so the text is copied as it is and the accents are left out, as upstream does
 * without utf8.library. */
#include <amiga.h>
#include <wchar.h>
int wcwidth(wchar_t c);                 /* picolibc declares it for X/Open programs only */
#define U8F_COMPOSE 1
extern struct Library *Utf8Base;
static inline LONG Utf8CharWidth(ULONG c) { return wcwidth((wchar_t)c); }
static inline LONG Utf8Normalize(CONST_STRPTR s, LONG n, STRPTR out, LONG cap, ULONG flags)
{
    LONG k = n < cap - 1 ? n : cap - 1;
    (void)flags;
    if (cap <= 0) return 0;
    memcpy(out, s, (size_t)k);
    out[k] = 0;
    return k;
}
