/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* ZINE's in-memory volume (docs/d29-zine.md section 5.6): the program's directory as one block that `zine` fills
 * before the hand-over and ZINE's dos reads during the session (names found by ZINE itself, no handler). Plain C
 * (zcore.h). The block is internal to one build (zine and the blob come from the same tree): native byte order.
 *
 * Layout: struct zv_hdr, then cap entries (struct zv_ent; entry 0 is the root, the volume itself; a directory before
 * its entries), then the files' data, each 4-aligned. Names follow AmigaDOS's rules: Latin-1, compared without case
 * (ASCII and the Latin-1 letters, not x and the division sign), no ':' or '/', at most ZV_NAMEMAX bytes. Two names that
 * differ only in case (a Unix filesystem has them) are both kept: a lookup takes the exact name first, else the first
 * added (d29 16.10). */
#include "zcore.h"

#define ZV_MAGIC    0x5a564f4cu         /* "ZVOL" */
#define ZV_VERSION  5u
#define ZV_NAMEMAX  107                 /* bytes of a name (fib_FileName holds 108 with its end) */
#define ZV_VOLMAX   30                  /* bytes of the volume's name */
#define ZV_MAXFILES 2000u               /* section 5.6's limits (16.15: was 500): more, and only the program goes in */
#define ZV_MAXBYTES (48u << 20)         /* the files' bytes (d29 16.12; was 16 MB): the block stays one SYS_memalloc */
#define ZV_NKEEP    8u                  /* regions of a program `zine` loaded itself (16.12) */

/* AmigaDOS protection bits (dos/dos.h): the low four are set to forbid */
#define ZV_FIBF_DELETE  1u
#define ZV_FIBF_EXECUTE 2u
#define ZV_FIBF_WRITE   4u
#define ZV_FIBF_READ    8u

struct zv_hdr {
    zu32 magic, version;
    zu32 nent, cap;                     /* entries used, entries room */
    zu32 size;                          /* the whole block's bytes */
    zu32 data, used;                    /* where the data starts (from the block's start), bytes of it used */
    char volname[ZV_VOLMAX + 2];        /* Latin-1, 0-ended */
    /* d29 16.12: a program `zine` loaded from its file itself (too big to be kept beside its hunks): its segment list
     * (a BPTR; 0: the program is the volume's file, loaded by the blob) and the SYS_memalloc regions its hunks are in
     * (the program's own memory: the blob neither saves nor gives them; zine frees them afterwards) */
    zu32 pre_seg, nkeep;
    struct { zu32 addr, size; } keep[ZV_NKEEP];
    zu32 now;                           /* d29 16.13: Zenix's clock at the hand-over (Unix seconds; 0 unknown) */
    /* d29 16.14: the CPU support packages zine loaded and relocated in memory of its own (Zenix's, not the
     * program's), 0 none: [0] the 68060's (user/zine/zsp060.S), [1] the 68040's (zsp040.S); their bytes */
    zu32 sp[2], sp_size[2];
    zu32 mem;                           /* d29 16.23: the program's memory (ZV_MEM_ALL, zine -c ZV_MEM_CHIP, -l ZV_MEM_LOW) */
};
#define ZV_MEM_ALL  0u                  /* every free chip and fast block */
#define ZV_MEM_CHIP 1u                  /* chip RAM only, as AmigaOS's NoFastMem leaves a program */
#define ZV_MEM_LOW  2u                  /* chip RAM and the fast RAM below 16 MB (the 24-bit addresses) */
#define ZV_LOW16    0x1000000u          /* 16 MB: the end of the 24-bit addresses */
struct zv_ent {
    zu32 parent;                        /* its directory's entry (the root: 0) */
    zu32 size;                          /* bytes (a directory: 0) */
    zu32 prot;                          /* fib_Protection */
    zu32 days, mins, ticks;             /* the date (a DateStamp: days since 1978-01-01, minutes, 1/50 s) */
    zu32 off;                           /* the data, from the block's start */
    zu16 dir;                           /* 1: a directory */
    zu16 namelen;
    char name[ZV_NAMEMAX + 1];          /* Latin-1, 0-ended */
    zu32 nx;                            /* d29 16.23: 0, or the file is read from the disk during the session: off
                                         * holds nx runs of its sectors on the IDE drive (struct zv_run) */
};
struct zv_run { zu32 lba, count; };     /* sectors of the whole drive; lba ZV_HOLE: count sectors of zeros */
#define ZV_HOLE     0xFFFFFFFFu
#define ZV_MAXRUNS  0x10000u            /* runs of one file */

/* the bytes a block needs for n entries (the root included) and the files' bytes */
zu32 zv_need(zu32 n, zu32 bytes, zu32 nfiles);
/* 1 when nfiles files of bytes bytes may go in with budget bytes for them (16.12: at most 500 files, 48 MB and the
 * budget `zine` works out: half the fast RAM the program's hunks leave), else 0 */
int zv_fits(zu32 nfiles, zu32 bytes, zu32 budget);

/* Builder (in `zine`). zv_init: an empty volume in blk (size bytes, room for cap entries, the root included); the
 * volume's name from name (UTF-8; "ZINE" if it is no AmigaDOS name). 0 or a negative errno. */
int zv_init(void *blk, zu32 size, zu32 cap, const char *name);
/* Adds name (UTF-8, as the directory gave it) under the directory entry parent: dir 1 for a directory, mode the Unix
 * mode, mtime Unix seconds, size the bytes. The entry's index, with *data where its size bytes go (the caller reads
 * the file into it); or a ZV_E_ code (negative) and nothing added: say zv_reason() and go on. */
int zv_add(void *blk, zu32 parent, const char *name, int dir, zu32 mode, zu32 mtime, zu32 size, zu8 **data);
/* d29 16.23: a file of size bytes read from the disk: its nx runs (1..ZV_MAXRUNS) are already at zv_tail's place */
int zv_add_runs(void *blk, zu32 parent, const char *name, zu32 mode, zu32 mtime, zu32 size, zu32 nx);
/* where the next entry's data goes, and the bytes of room there */
zu8 *zv_tail(void *blk, zu32 *room);
/* (-3 was ZV_E_CLASH, a name the same as another but for case: since 16.10 both are added; zv_twin finds it) */
#define ZV_E_NAME   -1                  /* not a Latin-1 name, or holds ':' or '/', or is empty, "." or ".." */
#define ZV_E_LONG   -2                  /* longer than 107 bytes */
#define ZV_E_FULL   -4                  /* no entry left */
#define ZV_E_SPACE  -5                  /* no room for the data */
#define ZV_E_PARENT -6                  /* parent is no directory of the volume */
const char *zv_reason(int e);

/* Reader (the blob's dos). zv_check: the block is whole and consistent (0, or -ZE_INVAL). */
int zv_check(const void *blk, zu32 len);
const struct zv_ent *zv_ent(const void *blk, zu32 i);
/* Finds path (Latin-1, AmigaDOS form) from the directory entry cur, each name the exact one first, else without case: "NAME:", "SYS:", "PROGDIR:" and ":" are the
 * root, "C:", "S:", "L:", "LIBS:", "DEVS:", "FONTS:" the root's directories of those names; '/' at the start or
 * twice goes up one. 0 and *out, or -ZE_NOENT, -ZE_NOTDIR, -ZE_NODEV (a device the volume is not: "NIL:", "CON:"). */
int zv_find(const void *blk, zu32 cur, const char *path, zu32 *out);
/* an entry before i in i's directory whose name is the same as i's but for case (the one a lookup that is not exact
 * finds), or 0 */
zu32 zv_twin(const void *blk, zu32 i);
/* the first entry after `after` in directory dir (ExNext's order), or 0 at the end; zv_next(blk, dir, 0) is the first */
zu32 zv_next(const void *blk, zu32 dir, zu32 after);
/* AmigaDOS's comparison: 1 when a and b (Latin-1, n bytes of a, b 0-ended) are the same name but for case */
int zv_same(const char *a, zu32 n, const char *b);
