/* SPDX-License-Identifier: GPL-2.0-only */
/* Host block devices for ZenFS: a memory image that can record every write and flush (for power-cut tests),
 * and a plain file image. */
#ifndef ZDEV_H
#define ZDEV_H
#include "../zenfs.h"

struct zlog { int is_flush; uint32_t blk; uint8_t *data; };

struct zmem {
    struct zfs_dev dev;
    uint8_t *img;
    int record;
    struct zlog *log; size_t nlog, caplog;
    unsigned reads, writes, flushes;
};
struct zmem *zmem_new(uint32_t bshift, uint32_t nblocks, int record);
struct zmem *zmem_clone(const struct zmem *src);          /* copy of the image, no log */
void zmem_free(struct zmem *m);
void zmem_apply(struct zmem *m, const struct zlog *e);    /* apply one logged write to the image */

struct zfile { struct zfs_dev dev; int fd; };
int zfile_open(struct zfile *f, const char *path, uint32_t bshift, int create_blocks);   /* create_blocks > 0: make a new image */
void zfile_close(struct zfile *f);
#endif
