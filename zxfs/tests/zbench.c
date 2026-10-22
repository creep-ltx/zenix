/* SPDX-License-Identifier: GPL-2.0-only */
/* Library-only benchmark on a RAM-backed device: the cost of the ZXFS algorithms themselves, with no real disk,
 * no kernel and no FUSE. Host figures; they say nothing about an Amiga. Usage: zbench [files] [megabytes] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../zxfs.h"
#include "../host/zdev.h"

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char **argv)
{
    int nfiles = argc > 1 ? atoi(argv[1]) : 20000;
    int mb = argc > 2 ? atoi(argv[2]) : 256;
    uint32_t nblocks = (uint32_t)((mb + 64) * 256 + nfiles / 4);   /* 4 KB blocks */
    struct zmem *m = zmem_new(12, nblocks, 0);
    struct zxfs *fs; uint32_t d, f;
    if (zxfs_mkfs(&m->dev, NULL, 0, 1)) return 1;
    if (zxfs_mount(&fs, &m->dev, 4096)) return 1;
    struct zxfs_stats st0, st1;
    char nm[32]; uint8_t small[100]; memset(small, 'x', sizeof small);
    zxfs_mkdir(fs, ZXFS_ROOT_INO, "d", 1, 0755, 0, 0, &d);

    double t = now(); zxfs_get_stats(fs, &st0);
    for (int i = 0; i < nfiles; i++) { int l = snprintf(nm, sizeof nm, "file%06d", i); zxfs_create(fs, d, nm, l, 0644, 0, 0, &f); zxfs_write(fs, f, 0, small, sizeof small); }
    zxfs_sync(fs); double t1 = now(); zxfs_get_stats(fs, &st1);
    printf("create+write %d small files: %.0f files/s   (device: %u writes, %u flushes, %u commits)\n", nfiles, nfiles / (t1 - t), st1.writes - st0.writes, st1.flushes - st0.flushes, st1.commits - st0.commits);

    t = now();
    for (int i = 0; i < nfiles; i++) { int l = snprintf(nm, sizeof nm, "file%06d", i); if (zxfs_lookup(fs, d, nm, l, &f)) return 2; }
    t1 = now(); printf("lookup: %.0f lookups/s\n", nfiles / (t1 - t));

    t = now(); uint64_t cookie = 0; uint32_t ino; uint8_t type; int n = 0;
    while (zxfs_readdir(fs, d, &cookie, nm, &ino, &type) > 0) n++;
    t1 = now(); printf("readdir: %d entries, %.0f entries/s\n", n, n / (t1 - t));

    zxfs_create(fs, ZXFS_ROOT_INO, "big", 3, 0644, 0, 0, &f);
    size_t chunk = 65536; uint8_t *buf = malloc(chunk); memset(buf, 0xA5, chunk);
    zxfs_get_stats(fs, &st0); t = now();
    for (uint64_t off = 0; off < (uint64_t)mb << 20; off += chunk) zxfs_write(fs, f, off, buf, chunk);
    zxfs_sync(fs); t1 = now(); zxfs_get_stats(fs, &st1);
    printf("sequential write %d MB: %.0f MB/s   (device: %u writes, %u flushes, %u commits)\n", mb, mb / (t1 - t), st1.writes - st0.writes, st1.flushes - st0.flushes, st1.commits - st0.commits);

    zxfs_unmount(fs); zxfs_mount(&fs, &m->dev, 4096);       /* cold cache */
    zxfs_lookup(fs, ZXFS_ROOT_INO, "big", 3, &f);
    zxfs_get_stats(fs, &st0); t = now();
    for (uint64_t off = 0; off < (uint64_t)mb << 20; off += chunk) if (zxfs_read(fs, f, off, buf, chunk) != (int)chunk) return 3;
    t1 = now(); zxfs_get_stats(fs, &st1);
    printf("sequential read %d MB (cold): %.0f MB/s   (device reads: %u)\n", mb, mb / (t1 - t), st1.reads - st0.reads);

    t = now();
    for (int i = 0; i < nfiles; i++) { int l = snprintf(nm, sizeof nm, "file%06d", i); zxfs_unlink(fs, d, nm, l); }
    zxfs_unlink(fs, ZXFS_ROOT_INO, "big", 3); zxfs_sync(fs); t1 = now();
    printf("delete %d files + the big one: %.0f deletes/s\n", nfiles, nfiles / (t1 - t));
    struct zxfs_stats s; zxfs_get_stats(fs, &s);
    printf("fsck after: %d problems\n", zxfs_fsck(fs, NULL, NULL));
    zxfs_unmount(fs); zmem_free(m);
    return 0;
}
