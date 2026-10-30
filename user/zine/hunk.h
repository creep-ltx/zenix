/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* The AmigaOS hunk file: ZINE's check and loader in one walk (docs/d29-zine.md section 6), so a file the loader would
 * refuse is refused by `zine` while Zenix still runs, with the same message. Plain C (zcore.h): /bin/zine, the blob
 * and the host tests build this one file. Everything in the file is untrusted: every count and offset is checked
 * with arithmetic that cannot wrap, and no byte outside the file or a hunk is read or written.
 *
 * Accepted: HUNK_HEADER without resident library names; HUNK_CODE, HUNK_DATA, HUNK_BSS; HUNK_RELOC32;
 * HUNK_RELOC32SHORT as 0x3FC and as 0x3F7 (the Guru Book, ch. 22.2.6: the ROM's loader takes 0x3F7 for it in
 * executables; the AmigaDOS Manual's HUNK_DRELOC32 of the same number is an object-module block the linker resolves);
 * its padding word is optional (both forms are read); HUNK_SYMBOL, HUNK_DEBUG, HUNK_NAME skipped; HUNK_END (a hunk
 * whose HUNK_END is missing before the next hunk or the end of the file ends there).
 * Refused, each with its own message: overlays (HUNK_OVERLAY, HUNK_BREAK), external references (HUNK_EXT),
 * HUNK_RELRELOC32, the 16- and 8-bit relocations, resident library names, anything else. */
#include "zcore.h"

#define ZH_MAXHUNKS 1024u               /* hunks in one program */
#define ZH_MAXBYTES 0x10000000u         /* all hunks together, 256 MB: sums of sizes cannot wrap */

#define HUNK_CODE         0x3E9u
#define HUNK_DATA         0x3EAu
#define HUNK_BSS          0x3EBu
#define HUNK_RELOC32      0x3ECu
#define HUNK_RELOC16      0x3EDu
#define HUNK_RELOC8       0x3EEu
#define HUNK_EXT          0x3EFu
#define HUNK_SYMBOL       0x3F0u
#define HUNK_DEBUG        0x3F1u
#define HUNK_END          0x3F2u
#define HUNK_HEADER       0x3F3u
#define HUNK_OVERLAY      0x3F5u
#define HUNK_BREAK        0x3F6u
#define HUNK_DRELOC32     0x3F7u        /* in an executable: HUNK_RELOC32SHORT (above) */
#define HUNK_RELOC32SHORT 0x3FCu
#define HUNK_RELRELOC32   0x3FDu
#define HUNK_NAME         0x3E8u

struct zh_info {
    zu32 nhunks;                        /* hunks to load */
    zu32 sizes;                         /* the byte offset of the header's size table in the file */
    zu32 bytes[3];                      /* memory the hunks take (their sizes + 8 each): any, chip, fast */
};

/* One hunk's memory: what alloc gave. p: where the loader writes (on the PC a buffer, on the Amiga the memory itself);
 * addr: its address as the program sees it. The hunk's segment is at addr: the size longword, the next-segment BPTR,
 * then the data (addr + 8). */
struct zh_seg {
    zu8 *p;
    zu32 addr, size;
};
struct zh_ops {
    /* size bytes (8-aligned, zero-filled or not: the loader clears it), memory attributes (ZMEMF_CHIP, ZMEMF_FAST,
     * 0 for any, or the attribute longword the header gave); 0 or a negative errno */
    int (*alloc)(void *u, zu32 size, zu32 memf, struct zh_seg *out);
    void (*free)(void *u, const struct zh_seg *s);
    void *u;
};

/* Checks the whole file: 0 and *inf filled, or -ZE_NOEXEC and a message in msg ("NAME uses overlays: not supported
 * yet"; "NAME is not an AmigaOS program (unknown hunk 0x3F9 at byte 1234)"); cap is msg's size (64 or more). */
int zh_check(const zu8 *f, zu32 len, const char *name, struct zh_info *inf, char *msg, zu32 cap);

/* The size (bytes) and memory attributes of hunk i (i < inf->nhunks) as the header gives them */
zu32 zh_hunk_size(const zu8 *f, const struct zh_info *inf, zu32 i, zu32 *memf);

/* Loads a checked file: every hunk allocated (work: inf->nhunks entries from the caller), its data copied, the rest
 * zero, the relocations applied, the segment list built. 0 and *seglist the BPTR of the first segment, or a negative
 * errno with everything freed again (-ZE_NOMEM: an alloc failed; -ZE_NOEXEC: the file changed since zh_check). The
 * caller flushes the caches before it runs anything (CLAUDE.md). */
int zh_load(const zu8 *f, zu32 len, const struct zh_info *inf, const struct zh_ops *ops, struct zh_seg *work,
            zu32 *seglist);
