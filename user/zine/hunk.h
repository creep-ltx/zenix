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
 * Overlays (docs/d29-zine.md 16.6; the AmigaDOS Manual's "hunk_overlay", the Guru Book ch. 22.2.10): the root node,
 * then one HUNK_OVERLAY block (its length longword is the upper bound: length + 1 longwords follow, copied as they
 * are), then overlay nodes, each a HUNK_HEADER whose first hunk is after the root's and whose last is inside the
 * root's table size, its hunks, HUNK_BREAK. Bytes after the table that are not a node (data a program reads through
 * its overlay file handle) are left alone. A node's relocations may name the hunks before its first (its ancestors,
 * found in the hunk table).
 * Refused, each with its own message: an overlay table inside a node (nested overlays), a HUNK_BREAK without an
 * overlay table, external references (HUNK_EXT), HUNK_RELRELOC32, the 16- and 8-bit relocations, resident library
 * names, anything else. */
#include "zcore.h"

#define ZH_MAXHUNKS 1024u               /* hunks in one program */
#define ZH_MAXBYTES 0x10000000u         /* all hunks together, 256 MB: sums of sizes cannot wrap */
#define ZH_MAXTAB   16384u              /* hunk slots of an overlaid program's whole tree ((n + 1) * 4 cannot wrap) */

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

struct zh_info {                        /* one node: the root (zh_check) or an overlay node (zh_node) */
    zu32 nhunks;                        /* hunks to load */
    zu32 sizes;                         /* the byte offset of the header's size table in the file */
    zu32 bytes[3];                      /* memory the hunks take (their sizes + 8 each): any, chip, fast */
    zu32 start;                         /* the node's first byte (its HUNK_HEADER; the root: 0) */
    zu32 first, last, tabsize;          /* the header's first and last hunk slots and its table size */
    zu32 end;                           /* the byte after the node (the root: after its overlay table, if any) */
    zu32 ovl, ovl_words;                /* the root's overlay table: its first byte after the length, longwords; 0 */
    zu32 nodes, node_max, node_all;     /* zh_check of an overlaid root: the nodes after the table, the largest one's
                                         * bytes and all of them together (saturated at ZH_MAXBYTES) */
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

/* The same check and load with only the file's first hlen bytes in memory (f) and the rest read from the file as the
 * walk goes (d29 16.12: a program bigger than the memory beside its hunks is loaded by `zine` straight from its
 * file). The header's size table must lie within hlen (16 KB holds any header ZH_MAXHUNKS allows); win (wcap bytes,
 * 64 or more) is the window the walk reads blocks into; a hunk's data is read straight into its memory. read: 0, or
 * not 0 when the n bytes at off could not be read. An overlaid file is checked to its overlay table only (inf->ovl
 * set, its parts not walked): such a file stays in memory whole for the session, and is checked there again. */
struct zh_rd {
    int (*read)(void *u, zu32 off, zu8 *buf, zu32 n);
    void *u;
    zu8 *win;
    zu32 wcap;
};
int zh_check_rd(const zu8 *f, zu32 hlen, zu32 len, const struct zh_rd *rd, const char *name, struct zh_info *inf,
                char *msg, zu32 cap);
int zh_load_rd(const zu8 *f, zu32 hlen, zu32 len, const struct zh_rd *rd, const struct zh_info *inf,
               const struct zh_ops *ops, struct zh_seg *work, zu32 *seglist);

/* The size (bytes) and memory attributes of hunk i (i < inf->nhunks) as the header gives them */
zu32 zh_hunk_size(const zu8 *f, const struct zh_info *inf, zu32 i, zu32 *memf);

/* Loads a checked file: every hunk allocated (work: inf->nhunks entries from the caller), its data copied, the rest
 * zero, the relocations applied, the segment list built. 0 and *seglist the BPTR of the first segment, or a negative
 * errno with everything freed again (-ZE_NOMEM: an alloc failed; -ZE_NOEXEC: the file changed since zh_check). The
 * caller flushes the caches before it runs anything (CLAUDE.md). */
int zh_load(const zu8 *f, zu32 len, const struct zh_info *inf, const struct zh_ops *ops, struct zh_seg *work,
            zu32 *seglist);

/* Checks one overlay node at byte start (where the overlay supervisor's Seek put the file): 0 and *inf, or
 * -ZE_NOEXEC and the message. inf->end is the byte after its HUNK_BREAK. */
int zh_node(const zu8 *f, zu32 len, zu32 start, const char *name, struct zh_info *inf, char *msg, zu32 cap);

/* Loads a node zh_node checked, as zh_load does, its relocations to earlier hunks taken from tab, the hunk table
 * (tabn longwords, big-endian BPTRs as the program sees them; the last one is always 0). On success the node's
 * hunks are written into tab[first..last] and every entry after them is cleared (they were the hunks the overlay
 * supervisor freed before the load). -ZE_NOEXEC: the node lies outside the table or names a hunk not loaded. */
int zh_load_node(const zu8 *f, zu32 len, const struct zh_info *inf, const struct zh_ops *ops, struct zh_seg *work,
                 zu8 *tab, zu32 tabn, zu32 *seglist);

/* The hunk table entries of a loaded node (the root's after zh_load): tab[first + i] = segment i's BPTR, every
 * entry after them 0 */
void zh_table(const struct zh_info *inf, const struct zh_seg *work, zu8 *tab, zu32 tabn);

/* d29 16.18: 1 when `zine` should load the program from its file before the hand-over even though its file and hunks
 * fit together (path B, 16.12), so the file's copy in the volume takes no memory from the program: a plain program
 * (no overlays, at most 4 bytes after its last hunk: nothing it could read itself), its file at least 128 KB and a
 * sixteenth of the free memory (avail), its chip hunks 64 KB less than the chip RAM free now at most, and (16.19)
 * memory short: its file and hunks would leave less than 16 MB of avail (with more, the file stays, as before 16.18:
 * on the 68060 machine two demos crashed late on path B). zine also checks that the largest chip hunk fits in one
 * piece, and that the other ones find fast RAM. */
int zh_lean(const struct zh_info *inf, zu32 len, zu32 chip_free, zu32 avail);
