/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* A ZenixFS block device on top of the disk server (one device per process). */
#include "../zenixfs/zenixfs.h"

/* open the named device: flags DISK_MOUNT (become its only writer) or DISK_RAW (tools; refused while mounted).
 * Returns 0, or -1. On success the device is ready for zdisk_dev(). */
int zdisk_open(const char *name, unsigned flags);
void zdisk_close(void);
/* 1 if the device's first sector holds a ZenixFS superblock (reads only; does not claim the device) */
int zdisk_probe(const char *name);
unsigned zdisk_sectors(void);                     /* 512-byte sectors on the device */
unsigned zdisk_dostype(void);
/* fill dev for filesystem blocks of 1 << bshift bytes (bshift 10..12) */
void zdisk_dev(struct zenixfs_dev *dev, unsigned bshift);
