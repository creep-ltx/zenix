/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* mount - an ADF disk image as a real drive (DA0:, DA1: ...), through
 * OS 3.2's trackfile.device: C:DAControl puts the image in and takes it
 * out, the device itself says what is mounted where. Before 3.2 there
 * is no such device: the image still opens as a folder (arcfs).
 *
 * The rules learned the hard way (Knowledge/amiga/disk-images.md): the
 * drive is chosen here, never learned back from DA_LASTDEVICE; an image
 * in a drive cannot be deleted or moved, so a delete takes it out first;
 * only images with a DOS boot block go in (others bring requesters on
 * every look); a DAn: path is never saved - it points at the folder that
 * holds the image instead. */
#ifndef CF_MOUNT_H
#define CF_MOUNT_H

#include "place.h"

int mnt_have(void);
void mnt_exit(void);              /* at the end */             /* trackfile.device and C:DAControl are there */
/* the drive ("DA0:") holding this image file; 0 when it is in none */
int mnt_find(const char *file, char *dev, int size);
/* the image whose drive (or volume) this path is on: its file, 0 none */
int mnt_imageof(const char *path, char *file, int size);
/* a delete or a move of the file: out of its drive first. 1 = it was
 * in one and is out now */
int mnt_release(const char *file);
/* a path on a mounted image becomes the folder that holds the image
 * (for saving: DAn: names nothing after the next start) */
void mnt_unmap(char *path, int size);

/* the key. On an .adf: into a drive, and go (go gets "DA0:"); on an
 * image already in one: take it out. Inside a mounted drive (the place
 * itself is on one): take that image out, go gets the image's folder.
 * other: the other pane's path, which must not be on the image ejected.
 * 1 = go somewhere (go set), 0 = stay */
int fs_mount(struct place *p, int cur, const char *other, char *go, int size);

#endif
