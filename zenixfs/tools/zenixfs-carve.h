/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ZENIXFS_CARVE_H
#define ZENIXFS_CARVE_H
struct carve_stats {
    int bshift, live_filter;                   /* block size found; 1 = only blocks the free-space map says are in use were read */
    unsigned leaves, leaves_stale, items;      /* valid leaf blocks seen, of those not in use (skipped), distinct items used */
    unsigned files, dirs, links, lost_dirs, lost_files, skipped_existing, no_inode, partial, failed, data_blocks_lost;
};
/* image: a ZenixFS image (any damage); outdir: where the files go (created; existing names are left alone).
 * include_stale: also read leaf blocks that the free-space map says are free (copies the filesystem had replaced but not yet
 * reused): they may hold what a damaged current block held, but also files that were deleted shortly before. A file whose data
 * is not whole gets the suffix .partial, a file whose inode is lost .nosize.
 * Returns 0, or negative: -1 cannot open, -2 not recognisable as ZenixFS, -3 no usable leaf found. */
int zenixfs_carve(const char *image, const char *outdir, struct carve_stats *st, int verbose, int include_stale);
/* block size (10..12) the image seems to use, judged from the blocks themselves; -1 if none fits */
int zenixfs_guess_bshift(const char *image);
#endif
