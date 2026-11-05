/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* A small writer of a fresh AmigaDOS FFS volume (DOS\1, 512-byte blocks, 2 reserved blocks): the boot partition
 * zinstall makes (DESIGN D35). The layout is tools/mkffs.py's and the ADF FAQ's. No system calls: blocks go through
 * write(), the data through read(). The volume is complete only when ffsw_end() returned 0. */
struct ffsw_io {
    void *ctx;
    int (*wr)(void *ctx, unsigned lba, unsigned n, const void *buf);      /* in blocks of this partition */
    int (*rd)(void *ctx, unsigned lba, unsigned n, void *buf);
};
typedef int (*ffsw_src)(void *ctx, unsigned char *buf, unsigned n);       /* exactly n bytes: 0, else an error */

#define FFSW_MAXFILES 12
#define FFSW_MAXBLOCKS ((25u + 2u * 127u) * 4064u)  /* 25 bitmap blocks in the root, two extension blocks: about 550 MB (as user/zpkg/bootfs.c) */

int ffsw_begin(const struct ffsw_io *io, unsigned blocks, const char *name, unsigned days, unsigned mins);
int ffsw_mkdir(const char *name);       /* a directory in the root: 0, or -1 */
/* a file; dir "" or a directory made above. *sha32: the SHA-256 of the data as read from src (for the check after) */
int ffsw_file(const char *dir, const char *name, unsigned size, ffsw_src src, void *srcctx, unsigned char sha32[32]);
int ffsw_end(void);
/* after ffsw_end: the SHA-256 of a file from the blocks on the disk, found as any reader finds them */
int ffsw_read_sha(const char *dir, const char *name, unsigned size, unsigned char sha32[32]);
const char *ffsw_error(void);
