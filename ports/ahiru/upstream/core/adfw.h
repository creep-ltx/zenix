/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* adfw - changing an AmigaDOS volume inside an image file: the writer
 * beside core/adf.c's reader.
 *
 * Plain C like the reader: blocks go out through a write callback,
 * memory comes from arc_alloc. What it writes: the OFS and FFS file
 * systems, plain and international (DOS\0..DOS\3), on floppy images
 * (DD and HD) and on a hard disk image's partition (the one adf_open
 * opened) whose bitmap fits in 64 KB (about 250 MB of 512-byte blocks).
 * Refused with ARC_EUNSUP: DOS\4/5 (the directory cache would have to
 * be kept too) and DOS\6/7 (long names) - read them, do not change them;
 * a volume whose bitmap is not valid ("needs validating") or whose
 * bitmap blocks or root are damaged.
 *
 * The rule for every change: a crash or power cut between any two block
 * writes leaves a valid volume - at worst blocks marked used that
 * nothing points at (lost space, never lost structure). adfw.c's top
 * comment has the order of writes for each call.
 *
 * Paths are like adf_list's: "dir/sub/name", no volume part, matched
 * without case; a hard link to a directory is followed on the way.
 */
#ifndef CF_ADFW_H
#define CF_ADFW_H

#include "adf.h"

/* bytes to off in the image: returns n when all were written */
typedef long (*adf_wrfn)(void *ctx, unsigned long off, const void *buf, long n);

/* the writer's own results, beside arc.h's */
enum {
    ADF_EWRITE = -20,       /* the image could not be written */
    ADF_EFULL = -21,        /* not enough free blocks (nothing was changed) */
    ADF_EEXIST = -22,       /* the name is taken (by a directory, or rename's target) */
    ADF_ENOTFOUND = -23,    /* no such entry */
    ADF_ENOTEMPTY = -24,    /* a directory with something in it */
    ADF_ENAME = -25,        /* not a name AmigaDOS takes (empty, longer than 30,
                               '/', ':', control characters) or a comment over 79 */
    ADF_ELOOP = -26,        /* a directory moved into itself */
    ADF_ELINKED = -27,      /* has hard links pointing at it: not deleted/replaced */
    ADF_ESHORT = -28,       /* putfile's get() gave less than size (nothing left behind) */
    ADF_EVALIDATE = -29     /* the bitmap is not valid: the disk needs validating */
};

/* plain words for an ADF_ code (arc_errstr's for the rest) */
const char *adf_errstr(int e);

/* after adf_open: the volume becomes writable through wr. 0, or
   ARC_EUNSUP (a variant written wrongly if at all), ADF_EVALIDATE,
   ARC_ECORRUPT (bitmap or root damaged), ARC_ENOMEM, ARC_EREAD.
   adf_close frees what this takes (4 blocks + the bitmap). */
int adf_writable(struct adfvol *v, adf_wrfn wr, void *wctx);

/* the time changes are stamped with (the parents' dates, the volume's
   "altered" date, mkdir's new directory) - set it before each change;
   0/0/0 until set */
void adf_now(struct adfvol *v, long days, long mins, long ticks);

int adf_mkdir(struct adfvol *v, const char *path);

/* a file of size bytes, read from get (returns how many it put in buf;
   less than asked before size is reached = stop: ADF_ESHORT, the volume
   as it was). An existing file or link of that name is REPLACED safely:
   the new file is written and linked under a scratch name in the same
   hash chain, then the old entry unlinked, then the new one renamed by
   one block write - at every moment the old or the new file is there.
   Replacing needs room for both (else ADF_EFULL). A directory of that
   name is ADF_EEXIST, a file with hard links ADF_ELINKED. */
int adf_putfile(struct adfvol *v, const char *path, unsigned long size,
                long (*get)(void *ctx, void *buf, long n), void *gctx,
                unsigned long prot, long days, long mins, long ticks,
                const char *comment);

/* a file, a link or an empty directory (else ADF_ENOTEMPTY: the caller
   empties it first); something with hard links to it is ADF_ELINKED */
int adf_delete(struct adfvol *v, const char *path);

/* rename and/or move into another directory; to = the full new path.
   A taken name is ADF_EEXIST (unless it is the same entry: a change of
   case), a directory into itself ADF_ELOOP */
int adf_rename(struct adfvol *v, const char *from, const char *to);

int adf_setprot(struct adfvol *v, const char *path, unsigned long prot);
int adf_setcomment(struct adfvol *v, const char *path, const char *comment);
int adf_setdate(struct adfvol *v, const char *path, long days, long mins, long ticks);

/* free blocks as bytes a file could hold (OFS 488 a block, FFS 512) */
unsigned long adf_freebytes(struct adfvol *v);

/* a blank volume into an image: 880K (hd 0) or 1760K (hd 1), every
   block written. dostype 0..3 (DOS\0..DOS\3); | ADF_BOOT makes it
   bootable (the 1.x boot block every Kickstart boots). Dates: the
   volume's creation date. 0 or ARC_EUNSUP, ADF_ENAME, ADF_EWRITE,
   ARC_ENOMEM. */
#define ADF_BOOT 0x100
int adf_format(adf_wrfn wr, void *wctx, int hd, int dostype, const char *name,
               long days, long mins, long ticks);

/* the test checker (core/adfchk.c, not in CFile's build): walks the
   whole volume; every block used once, the bitmap agrees, checksums,
   hash chains (right slot, ascending, no names twice), file structure.
   msg gets one line per problem. Returns the number of errors; blocks
   marked used that nothing references are counted in *leaked (not
   errors). */
int adf_check(struct adfvol *v, void (*msg)(void *ctx, const char *s), void *mctx,
              unsigned long *leaked);

#endif
