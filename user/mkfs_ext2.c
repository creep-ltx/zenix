/* SPDX-License-Identifier: GPL-2.0-only */
#ifdef ZHOST
#include "../tools/host/zhost.h"     /* host build for the tests (tools/ext2-host-test.sh) */
#else
#include "libz.h"
typedef unsigned char u8;
typedef unsigned short u16;
#endif
#include "../kernel/diskproto.h"

/* mkfs.ext2 [-b 1024|2048|4096] [-i BYTES_PER_INODE] DEVICE
 * Makes a revision-0 ext2 filesystem (128-byte inodes, no optional features,
 * a superblock and group table copy in every group): readable by Linux and
 * by the Zenix filesystem server. Every group's inode table is zeroed: e2fsck
 * and fsck.ext2 read every inode, and old data there would look like files. */

static int port, h;
static u32 bsize = 1024, bpi = 4096;
static u8 blk[4096], bm[4096], imap[4096];

static int wblk(u32 n, const void *buf)
{
    struct umsg m;
    m.type = DISK_WRITE; m.a[0] = (u32)h; m.a[1] = n * (bsize / 512); m.a[2] = bsize / 512; m.a[3] = (u32)buf;
    return sys_call(port, &m);
}

/* n zero blocks from block b, in pieces of up to 32 KB */
static u32 maxsect = 64;                     /* the device's largest transfer (DISK_INFO), at most 32 KB */
static int wzero(u32 b, u32 n)
{
    static u8 *z;
    u32 per = (maxsect < 64 ? maxsect : 64) * 512 / bsize;
    if (per == 0) per = 1;
    if (!z) { z = (u8 *)xmalloc(32768); memset(z, 0, 32768); }
    while (n) {
        u32 k = n < per ? n : per;
        struct umsg m;
        m.type = DISK_WRITE; m.a[0] = (u32)h; m.a[1] = b * (bsize / 512); m.a[2] = k * (bsize / 512); m.a[3] = (u32)z;
        if (sys_call(port, &m)) return -1;
        b += k; n -= k;
    }
    return 0;
}

static void w32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void w16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void setbit(u8 *b, u32 i) { b[i / 8] |= (u8)(1u << (i % 8)); }

static u32 cdiv(u32 a, u32 b) { return (a + b - 1) / b; }

int main(int argc, char **argv)
{
    const char *dev = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) bsize = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) bpi = (u32)atoi(argv[++i]);
        else dev = argv[i];
    }
    if (!dev || (bsize != 1024 && bsize != 2048 && bsize != 4096) || bpi < 1024) {
        printf("usage: mkfs.ext2 [-b 1024|2048|4096] [-i bytes-per-inode] device\n");
        return 2;
    }
    port = sys_port_find("disk");
    if (port < 0) { printf("mkfs.ext2: no disk server\n"); return 1; }
    struct umsg m;
    m.type = DISK_OPEN; m.a[0] = (u32)dev; m.a[1] = DISK_RAW;
    h = sys_call(port, &m);
    struct diskinfo di;
    m.type = DISK_INFOSZ; m.a[0] = (u32)h; m.a[1] = (u32)&di; m.a[2] = sizeof di;
    if (h < 0 || sys_call(port, &m) < 0) { printf("mkfs.ext2: cannot open %s (mounted, or no such device)\n", dev); return 1; }
    if (di.maxsect) maxsect = di.maxsect;
    if (!strcmp(dev, "ide0") && di.start == 0 && di.dostype == 0)
        printf("mkfs.ext2: warning: formatting the whole disk, not a partition\n");

    u32 first = bsize == 1024 ? 1 : 0;
    u32 nblocks = (di.sectors / (bsize / 512));
    u32 bpg = 8 * bsize;
    u32 ngroups, gdt, ipg, itb, overhead;
    for (;;) {
        if (nblocks < 64) { printf("mkfs.ext2: device too small\n"); return 1; }
        ngroups = cdiv(nblocks - first, bpg);
        gdt = cdiv(ngroups * 32, bsize);
        u32 ninodes = (u32)(((unsigned long long)nblocks * bsize) / bpi);
        ipg = (cdiv(ninodes, ngroups) + 7) & ~7u;
        if (ipg < 16) ipg = 16;
        if (ipg > bsize * 8) ipg = bsize * 8;
        itb = cdiv(ipg * 128, bsize);
        overhead = 1 + gdt + 2 + itb;
        u32 lastblocks = (nblocks - first) - (ngroups - 1) * bpg;
        if (ngroups > 1 && lastblocks < overhead + 50) { nblocks -= lastblocks; continue; }   /* drop a tiny last group */
        break;
    }
    u32 ninodes = ipg * ngroups;
    u32 lf_blocks = 12288 / bsize;                  /* lost+found: 12 KB */
    if (lf_blocks < 3) lf_blocks = 3;
    u32 root_blk = first + 1 + gdt + 2 + itb;       /* first data blocks of group 0 */
    u32 lf_first = root_blk + 1;
    u32 used0 = overhead + 1 + lf_blocks;
    u32 *gfree = (u32 *)xmalloc(ngroups * 4 * 4);    /* free blocks, free inodes, dirs */

    /* group descriptor table (same in every group) */
    u8 *gd = (u8 *)xmalloc(gdt * bsize);
    memset(gd, 0, gdt * bsize);
    u32 total_free = 0;
    for (u32 g = 0; g < ngroups; g++) {
        u32 gs = first + g * bpg, gb = (nblocks - gs < bpg) ? nblocks - gs : bpg;
        u32 used = overhead + (g == 0 ? 1 + lf_blocks : 0);
        u32 freeb = gb - used, freei = ipg - (g == 0 ? 11 : 0);
        u8 *e = gd + g * 32;
        w32(e + 0, gs + 1 + gdt);
        w32(e + 4, gs + 1 + gdt + 1);
        w32(e + 8, gs + 1 + gdt + 2);
        w16(e + 12, freeb); w16(e + 14, freei); w16(e + 16, g == 0 ? 2 : 0);
        total_free += freeb;
        gfree[g] = freeb;
    }

    /* superblock */
    static u8 sb[4096];                 /* not on the 16 KB stack (audit A12) */
    memset(sb, 0, sizeof sb);
    w32(sb + 0, ninodes); w32(sb + 4, nblocks); w32(sb + 8, 0);
    w32(sb + 12, total_free); w32(sb + 16, ninodes - 11); w32(sb + 20, first);
    w32(sb + 24, bsize == 1024 ? 0 : bsize == 2048 ? 1 : 2); w32(sb + 28, bsize == 1024 ? 0 : bsize == 2048 ? 1 : 2);
    w32(sb + 32, bpg); w32(sb + 36, bpg); w32(sb + 40, ipg);
    w16(sb + 52, 0); w16(sb + 54, 0xFFFF); w16(sb + 56, 0xEF53); w16(sb + 58, 1); w16(sb + 60, 1);
    w32(sb + 76, 0);                                /* revision 0 */
    {   /* a volume id (DESIGN D11 4.3: the root's server finds its volume by place and uuid): the clock, the tick and
         * (on Zenix) the E-clock, mixed */
        u32 x = (u32)sys_time() * 2654435761u ^ sys_ticks();
        for (int i = 0; i < 16; i++) {
#ifndef ZHOST
            x += sys_hrtime();
#endif
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            sb[104 + i] = (u8)(x >> 24);
        }
    }

    printf("mkfs.ext2: %s: %u blocks of %u bytes, %u groups, %u inodes\n", dev, nblocks, bsize, ngroups, ninodes);

    for (u32 g = 0; g < ngroups; g++) {
        u32 gs = first + g * bpg, gb = (nblocks - gs < bpg) ? nblocks - gs : bpg;
        /* superblock copy + group table */
        if (bsize == 1024) {
            memcpy(blk, sb, 1024);
            if (wblk(gs, blk)) goto fail;
        } else {
            static u8 tmp[4096];
            memset(tmp, 0, bsize);
            if (g == 0) memcpy(tmp + 1024, sb, 1024); else memcpy(tmp, sb, 1024);
            if (wblk(gs, tmp)) goto fail;
        }
        for (u32 k = 0; k < gdt; k++) {
            if (wblk(gs + 1 + k, gd + k * bsize)) goto fail;
        }
        /* block bitmap: metadata (and group 0's directories) used, padding past the end used */
        memset(bm, 0, bsize);
        u32 used = overhead + (g == 0 ? 1 + lf_blocks : 0);
        for (u32 i = 0; i < used; i++) setbit(bm, i);
        for (u32 i = gb; i < bsize * 8; i++) setbit(bm, i);
        if (wblk(gs + 1 + gdt, bm)) goto fail;
        /* inode bitmap */
        memset(imap, 0, bsize);
        if (g == 0) for (u32 i = 0; i < 11; i++) setbit(imap, i);
        for (u32 i = ipg; i < bsize * 8; i++) setbit(imap, i);
        if (wblk(gs + 1 + gdt + 1, imap)) goto fail;
        if (g && wzero(gs + 1 + gdt + 2, itb)) goto fail;     /* the inode table (group 0's is written below) */
    }

    /* inodes 2 (root) and 11 (lost+found) live in group 0's table */
    u32 itable = first + 1 + gdt + 2;
    u8 *it = (u8 *)xmalloc(itb * bsize);
    memset(it, 0, itb * bsize);
    u8 *root = it + (2 - 1) * 128, *lf = it + (11 - 1) * 128;
    w16(root + 0, 0040755); w32(root + 4, bsize); w16(root + 26, 3); w32(root + 28, bsize / 512); w32(root + 40, root_blk);
    w16(lf + 0, 0040700); w32(lf + 4, lf_blocks * bsize); w16(lf + 26, 2); w32(lf + 28, lf_blocks * bsize / 512);
    for (u32 i = 0; i < lf_blocks; i++) w32(lf + 40 + i * 4, lf_first + i);
    for (u32 k = 0; k < itb; k++)
        if (wblk(itable + k, it + k * bsize)) goto fail;

    /* root directory: ., .., lost+found */
    memset(blk, 0, bsize);
    w32(blk, 2); w16(blk + 4, 12); blk[6] = 1; blk[8] = '.';
    w32(blk + 12, 2); w16(blk + 16, 12); blk[18] = 2; blk[20] = '.'; blk[21] = '.';
    w32(blk + 24, 11); w16(blk + 28, bsize - 24); blk[30] = 10; memcpy(blk + 32, "lost+found", 10);
    if (wblk(root_blk, blk)) goto fail;
    /* lost+found: . and .. then empty blocks */
    for (u32 i = 0; i < lf_blocks; i++) {
        memset(blk, 0, bsize);
        if (i == 0) {
            w32(blk, 11); w16(blk + 4, 12); blk[6] = 1; blk[8] = '.';
            w32(blk + 12, 2); w16(blk + 16, bsize - 12); blk[18] = 2; blk[20] = '.'; blk[21] = '.';
        } else {
            w32(blk, 0); w16(blk + 4, bsize);
        }
        if (wblk(lf_first + i, blk)) goto fail;
    }
    (void)used0;
    printf("mkfs.ext2: done\n");
    return 0;
fail:
    printf("mkfs.ext2: write error\n");
    return 1;
}
