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
#define LVO_Forbid      -132
#define LVO_Permit      -138
#define LVO_LoadView    -222
#define LVO_WaitTOF     -270
#define LVO_WaitBlit    -228
#define LVO_OwnBlitter  -456
#define LVO_DisownBlitter -462
#define gb_ActiView     34
#define gb_copinit      38
#define MEMF_CHIP       2
#define LVO_PutMsg      -366
#define LVO_AvailMem    -216
#define MEMF_FAST       4
