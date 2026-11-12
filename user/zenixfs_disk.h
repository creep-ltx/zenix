/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* A ZenixFS block device on top of the disk server (one device per process). */
#include "../zenixfs/zenixfs.h"

/* open the named device: flags DISK_MOUNT (become its only writer) or DISK_RAW (tools; refused while mounted).
 * Returns 0, or -1. On success the device is ready for zdisk_dev(). */
int zdisk_open(const char *name, unsigned flags);
void zdisk_close(void);
unsigned zdisk_sectors(void);
int zdisk_can_flush(void);       /* 0: the disk cannot flush its write cache (DISKCAP_FLUSH unset): a commit is only "sent", not durable */                     /* 512-byte sectors on the device */
unsigned zdisk_dostype(void);
/* fill dev for filesystem blocks of 1 << bshift bytes (bshift 10..12) */
void zdisk_dev(struct zenixfs_dev *dev, unsigned bshift);
/* n neighbouring blocks in one disk request, for zenixfs_set_readn: 0 or -1 */
int zdisk_readn(void *ctx, uint32_t blk, uint32_t n, void *buf);
