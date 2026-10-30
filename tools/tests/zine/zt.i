/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's test programs (tools/tests/zine/*.S): AmigaOS library offsets they call, from the NDK's .fd files (names
 * and numbers only; user/zine/lvo.c has the full tables). Position-independent code only (tools/mkhunktest.py). */
#define LVO_AllocMem    -198
#define LVO_FreeMem     -210
#define LVO_OpenLibrary -552
#define LVO_CloseLibrary -414
#define LVO_Output      -60
#define LVO_Write       -48
#define LVO_Exit        -144
