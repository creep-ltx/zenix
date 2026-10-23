/* SPDX-License-Identifier: GPL-2.0-only */
#include "fsenv.h"
#include "../kernel/diskproto.h"

/* fsck.ext2 [-y] DEVICE   (default: report only)
 * Checks an unmounted ext2 filesystem the way e2fsck does and, with -y, repairs
 * what it safely can: block and inode bitmaps, free counts, link counts,
 * directory entries pointing at unused inodes, '..' entries, file type bytes, and
 * orphaned inodes (moved to /lost+found as #inode).
 * It first checks without writing. If it finds anything it cannot repair (duplicate
 * or out-of-range block pointers, an invalid mode, a damaged directory block, a limit
 * of its own) it repairs NOTHING, says so and exits with 4: a half-done repair could
 * make things worse. Only a volume it fully understands is checked: revision 0/1,
 * features filetype, sparse_super, large_file, ext_attr, resize_inode, dir_index
 * (an ext3 journal or any other feature: refused, use e2fsck).
 * Exit codes: 0 clean, 1 repaired, 4 errors remain, 8 operational error. */

static int port, h, fix;
static u32 bsize, nblocks, ninodes, ipg, bpg, first, ngroups, isize, first_ino;
static u32 compat, incompat, rocompat, gdt_blocks, gdt_start, rsv_gdt, sparse;
static u8 sb[1024];
static u8 *gdt;                          /* group descriptors, ngroups * 32 bytes */
static u8 *blockmap;                     /* blocks used, computed from what the inodes claim */
static u8 *aclmap;                       /* blocks claimed as extended-attribute blocks (may be shared) */
static u8 *usedi;                        /* inodes in use (computed) */
static u8 *maybe;                        /* inodes that look in use but are free in the bitmap: live only if named */
static u8 *isdir;                        /* inodes that are directories */
static u8 *itype;                        /* file type of each live inode (mode >> 12) */
static u8 *reach;                        /* directories reachable from the root */
static u8 *scanned;
static u16 *links;                       /* references found per inode (saturating at 65535) */
static u32 *dotdot;                      /* '..' of each directory as found */
static u32 *parent;                      /* real parent of each directory from the walk */
static u32 *dirlist; static u32 ndirs, dircap;
static u32 *orphan; static u32 norphans;
static u32 nfiles;
static int errors, fixed, unfixable;

#define MAXORPHANS 512

int fs_blk_read(u32 lba, u32 count, void *buf)
{
    struct umsg m;
    m.type = DISK_READ; m.a[0] = (u32)h; m.a[1] = lba; m.a[2] = count; m.a[3] = (u32)buf;
    return sys_call(port, &m);
}
int fs_blk_write(u32 lba, u32 count, const void *buf)
{
    struct umsg m;
    if (!fix) return -1;                 /* the check pass never writes */
    m.type = DISK_WRITE; m.a[0] = (u32)h; m.a[1] = lba; m.a[2] = count; m.a[3] = (u32)buf;
    return sys_call(port, &m);
}

static u32 L32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u32 L16(const u8 *p) { return p[0] | (u32)p[1] << 8; }
static void S32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void S16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static int tb(const u8 *m, u32 i) { return (m[i / 8] >> (i % 8)) & 1; }
static void sb_(u8 *m, u32 i) { m[i / 8] |= (u8)(1u << (i % 8)); }
static void cb_(u8 *m, u32 i) { m[i / 8] &= (u8)~(1u << (i % 8)); }

static int rblk(u32 n, void *buf) { return n >= nblocks ? -1 : fs_blk_read(n * (bsize / 512), bsize / 512, buf); }
static int wblk(u32 n, const void *buf) { return n >= nblocks ? -1 : fs_blk_write(n * (bsize / 512), bsize / 512, buf); }

#define problem(...) do { printf(__VA_ARGS__); printf("\n"); errors++; } while (0)

#define COMPAT_HAS_JOURNAL 0x0004
#define COMPAT_EXT_ATTR    0x0008
#define COMPAT_RESIZE      0x0010
#define COMPAT_DIR_INDEX   0x0020
#define INCOMPAT_FILETYPE  0x0002
#define RO_SPARSE          0x0001
#define RO_LARGE_FILE      0x0002
#define INDEX_FL           0x1000

/* ---- raw inode access (one cached block) ---- */
static u8 icache[4096];
static u32 icache_blk = 0xFFFFFFFF;

static u32 itable_of(u32 g) { return L32(gdt + g * 32 + 8); }

static u8 *inode_ptr(u32 ino, u32 *blk_out)
{
    u32 g = (ino - 1) / ipg, idx = (ino - 1) % ipg;
    u32 pos = idx * isize, blk = itable_of(g) + pos / bsize;
    if (blk != icache_blk) {
        if (rblk(blk, icache)) return 0;
        icache_blk = blk;
    }
    if (blk_out) *blk_out = blk;
    return icache + pos % bsize;
}
static int put_inode_dirty(void) { return wblk(icache_blk, icache); }

/* walk the block pointers of an inode; calls cb(block, is_data) for each block incl. indirect ones */
static int (*walk_cb)(u32 blk, int indirect, u32 ino);
static u32 walk_ino;

static void walk_tree(u32 blk, int level)
{
    if (!blk) return;
    if (walk_cb(blk, level > 0, walk_ino) && level > 0) {
        static u8 tmp[4][4096];
        u8 *t = tmp[level];
        if (rblk(blk, t)) return;
        for (u32 i = 0; i < bsize / 4; i++) {
            u32 b = L32(t + i * 4);
            if (b) walk_tree(b, level - 1);
        }
    }
}

static void walk_inode(const u8 *in, u32 ino, int (*cb)(u32, int, u32))
{
    walk_cb = cb; walk_ino = ino;
    for (int i = 0; i < 12; i++) walk_tree(L32(in + 40 + i * 4), 0);
    walk_tree(L32(in + 40 + 12 * 4), 1);
    walk_tree(L32(in + 40 + 13 * 4), 2);
    walk_tree(L32(in + 40 + 14 * 4), 3);
}

/* block n of a directory (copy of its inode): all levels of indirection. 0 = a hole, 0xFFFFFFFF = unreadable */
static u32 dir_bmap(const u8 *in, u32 n)
{
    static u8 ib[4096];
    u32 per = bsize / 4, blk, path[3], lv;
    if (n < 12) return L32(in + 40 + n * 4);
    n -= 12;
    if (n < per) { blk = L32(in + 40 + 12 * 4); lv = 1; path[0] = n; }
    else if ((n -= per) < per * per) { blk = L32(in + 40 + 13 * 4); lv = 2; path[0] = n / per; path[1] = n % per; }
    else { n -= per * per; blk = L32(in + 40 + 14 * 4); lv = 3; path[0] = n / (per * per); path[1] = (n / per) % per; path[2] = n % per; }
    for (u32 k = 0; k < lv; k++) {
        if (!blk) return 0;
        if (blk >= nblocks || rblk(blk, ib)) return 0xFFFFFFFF;
        blk = L32(ib + path[k] * 4);
    }
    return blk;
}

static int has_backup(u32 g)
{
    if (!sparse || g <= 1) return 1;
    for (u32 b = 3; b <= g; b *= 3) if (b == g) return 1;
    for (u32 b = 5; b <= g; b *= 5) if (b == g) return 1;
    for (u32 b = 7; b <= g; b *= 7) if (b == g) return 1;
    return 0;
}

/* ---- pass 1 callback: claim a block for an inode ---- */
static u32 cur_blocks;                   /* blocks (bsize units) the current inode owns */
static int claim(u32 blk, int indirect, u32 ino)
{
    (void)indirect;
    if (blk < first || blk >= nblocks) {
        problem("  inode %u: block %u is outside the filesystem", ino, blk);
        unfixable++;
        return 0;
    }
    if (tb(blockmap, blk)) {
        problem("  inode %u: block %u is also used elsewhere (duplicate or metadata)", ino, blk);
        unfixable++;
        return 0;
    }
    sb_(blockmap, blk);
    cur_blocks++;
    return 1;
}

static int add_dir(u32 ino)
{
    if (ndirs == dircap) {
        u32 nc = dircap ? dircap * 2 : 256;
        u32 *n = realloc(dirlist, nc * 4);
        if (!n) { printf("fsck.ext2: out of memory (directory list of %u)\n", ndirs); return -1; }
        dirlist = n; dircap = nc;
    }
    dirlist[ndirs++] = ino;
    sb_(isdir, ino);
    return 0;
}

static u8 dirent_type(u32 t)             /* directory entry file type of mode type t (mode >> 12) */
{
    static const u8 map[16] = { 0, 5, 3, 0, 2, 0, 4, 0, 1, 0, 7, 0, 6, 0, 0, 0 };
    return map[t & 15];
}

/* a live inode: check its mode, claim its blocks, check i_blocks; returns -1 on an operational error */
static int take_inode(u32 ino)
{
    u8 *in = inode_ptr(ino, 0);
    if (!in) { printf("fsck.ext2: read error in the inode table\n"); return -1; }
    u32 mode = L16(in), t = mode & 0170000;
    sb_(usedi, ino);
    nfiles++;
    if (t != 0040000 && t != 0100000 && t != 0120000 && t != 0020000 && t != 0060000 && t != 0010000 && t != 0140000) {
        problem("  inode %u has an invalid mode %o", ino, mode);
        unfixable++;
        return 0;
    }
    itype[ino] = (u8)(t >> 12);
    if (t == 0040000 && add_dir(ino)) return -1;
    if (L32(in + 20)) {
        problem("  inode %u is in use but has a deletion time", ino);
        if (fix) { S32(in + 20, 0); put_inode_dirty(); fixed++; }
    }
    /* fields ext2 without huge_file / 64bit / large_dir keeps at zero */
    if (L32(in + 112) || L32(in + 116) || (t == 0040000 && L32(in + 108))) {
        problem("  inode %u: fields that must be zero are not (fragment address, high halves)", ino);
        if (fix) { S32(in + 112, 0); S32(in + 116, 0); if (t == 0040000) S32(in + 108, 0); put_inode_dirty(); fixed++; }
    }
    u32 acl = L32(in + 104), spb = bsize / 512;
    cur_blocks = 0;
    if (acl) {
        if (!(compat & COMPAT_EXT_ATTR) || acl < first || acl >= nblocks) {
            problem("  inode %u: extended-attribute block %u is invalid", ino, acl);
            unfixable++;
        } else if (tb(blockmap, acl) && !tb(aclmap, acl)) {
            problem("  inode %u: extended-attribute block %u is also used elsewhere", ino, acl);
            unfixable++;
        } else {
            sb_(blockmap, acl); sb_(aclmap, acl);   /* one attribute block may be shared by several inodes */
            cur_blocks++;
        }
    }
    int fast_symlink = (t == 0120000 && L32(in + 28) <= (acl ? spb : 0) && L32(in + 4) < 60);
    if (t == 0020000 || t == 0060000 || t == 0010000 || t == 0140000 || fast_symlink) return 0;   /* no data blocks */
    u8 copy[128];
    memcpy(copy, in, 128);
    walk_inode(copy, ino, claim);
    icache_blk = 0xFFFFFFFF;
    in = inode_ptr(ino, 0);
    if (!in) return -1;
    u32 want = cur_blocks * spb;
    if (L32(in + 28) != want) {
        problem("  inode %u: i_blocks is %u, should be %u", ino, L32(in + 28), want);
        if (fix) { S32(in + 28, want); put_inode_dirty(); fixed++; }
    }
    return 0;
}

/* The resize inode (7): its double-indirect block is its own; it lists the reserved group-table blocks after the
 * table (already counted as metadata), and each of those lists its copies in the backup groups. Anything else in it
 * is damage fsck.ext2 does not repair (e2fsck rebuilds it). */
static int check_resize(u8 *in)
{
    static u8 dind[4096], rb[4096];
    u32 per = bsize / 4, d = L32(in + 40 + 13 * 4), bad = 0;
    for (int i = 0; i < 15; i++) if (i != 13 && L32(in + 40 + i * 4)) bad = 1;
    if (!rsv_gdt) { if (d) bad = 1; }
    else if (!d) bad = 1;
    else {
        walk_cb = claim; walk_ino = 7;
        if (!claim(d, 1, 7)) return 0;
        if (rblk(d, dind)) return -1;
        for (u32 i = 0; i < per && !bad; i++) {
            u32 v = L32(dind + i * 4), k = (i + per - gdt_blocks % per) % per;   /* entry i holds reserved block k */
            if (k >= rsv_gdt) { if (v) bad = 1; continue; }
            if (v != gdt_start + gdt_blocks + k) { bad = 1; break; }
            if (rblk(v, rb)) return -1;
            u32 j = 0;
            for (u32 g = 1; g < ngroups && !bad; g++) {
                if (!has_backup(g)) continue;
                if (j >= per || L32(rb + j * 4) != first + g * bpg + 1 + gdt_blocks + k) bad = 1;
                j++;
            }
            for (; j < per && !bad; j++) if (L32(rb + j * 4)) bad = 1;
        }
    }
    icache_blk = 0xFFFFFFFF;
    if (bad) {
        problem("  the resize inode (7) is damaged: fsck.ext2 does not rebuild it (e2fsck does)");
        unfixable++;
    }
    return 0;
}

/* one complete run over the volume; fix 0 writes nothing */
static int run(void)
{
    errors = fixed = unfixable = 0;
    ndirs = 0; norphans = 0; nfiles = 0;
    icache_blk = 0xFFFFFFFF;
    memset(blockmap, 0, (nblocks + 7) / 8); memset(aclmap, 0, (nblocks + 7) / 8);
    memset(usedi, 0, (ninodes + 8) / 8); memset(maybe, 0, (ninodes + 8) / 8); memset(isdir, 0, (ninodes + 8) / 8);
    memset(reach, 0, (ninodes + 8) / 8); memset(scanned, 0, (ninodes + 8) / 8); memset(itype, 0, ninodes + 1);
    memset(links, 0, (ninodes + 1) * 2); memset(dotdot, 0, (ninodes + 1) * 4); memset(parent, 0, (ninodes + 1) * 4);
    for (u32 i = 0; i < gdt_blocks; i++) if (rblk(gdt_start + i, gdt + i * bsize)) { printf("fsck.ext2: cannot read the group table\n"); return 8; }
    if (fs_blk_read(2, 2, sb)) { printf("fsck.ext2: cannot read the superblock\n"); return 8; }

    /* ---- metadata blocks ---- */
    sb_(blockmap, 0);
    for (u32 g = 0; g < ngroups; g++) {
        u32 gs = first + g * bpg, gb = nblocks - gs < bpg ? nblocks - gs : bpg;
        u32 bb = L32(gdt + g * 32 + 0), ib = L32(gdt + g * 32 + 4), it = L32(gdt + g * 32 + 8);
        u32 itb = (ipg * isize + bsize - 1) / bsize;
        if (has_backup(g))
            for (u32 k = 0; k < 1 + gdt_blocks + rsv_gdt; k++) if (gs + k < nblocks) sb_(blockmap, gs + k);
        if (bb < gs || bb >= gs + gb || ib < gs || ib >= gs + gb || it < gs || it >= gs + gb || itb > gs + gb - it) {
            printf("fsck.ext2: group %u has its tables outside the group: cannot repair\n", g);
            return 8;
        }
        sb_(blockmap, bb); sb_(blockmap, ib);
        for (u32 k = 0; k < itb; k++) sb_(blockmap, it + k);
    }

    /* ---- pass 1: inodes and their blocks ---- */
    printf("Pass 1: Checking inodes, blocks, and sizes\n");
    static u8 ibm[4096];
    for (u32 g = 0; g < ngroups; g++) {
        if (rblk(L32(gdt + g * 32 + 4), ibm)) { printf("fsck.ext2: cannot read an inode bitmap\n"); return 8; }
        for (u32 i = 0; i < ipg; i++) {
            u32 ino = g * ipg + i + 1;
            u8 *in = inode_ptr(ino, 0);
            if (!in) { printf("fsck.ext2: read error in the inode table\n"); return 8; }
            u32 mode = L16(in), lc = L16(in + 26);
            int reserved = ino < first_ino && ino != 2;
            int live = (mode != 0 && lc > 0) || ino == 2;     /* as e2fsck decides it: a link count; dtime is checked below */
            if (reserved) {                                      /* reserved inodes count as used */
                sb_(usedi, ino);
                if (ino == 7 && (compat & COMPAT_RESIZE) && live) {
                    if (check_resize(in)) return 8;
                } else if (live && ino == 1) {
                    u8 copy[128];                                /* the bad-blocks inode: its blocks are in use */
                    memcpy(copy, in, 128);
                    walk_inode(copy, ino, claim);
                    icache_blk = 0xFFFFFFFF;
                }
                continue;
            }
            if (!live) continue;                                 /* free: fixed by the bitmap pass if marked used */
            if (!tb(ibm, i)) { sb_(maybe, ino); continue; }      /* free in the bitmap: live only if a directory names it */
            if (take_inode(ino)) return 8;
        }
    }

    /* ---- pass 2: directories ---- */
    printf("Pass 2: Checking directory structure\n");
    static u8 db[4096];
    for (u32 d = 0; d < ndirs; d++) {
        u32 dino = dirlist[d];
        u8 *in = inode_ptr(dino, 0);
        if (!in) return 8;
        u8 copy[128];
        memcpy(copy, in, 128);
        u32 size = L32(copy + 4);
        int unindex = 0;
        for (u32 off = 0; off + bsize <= size; off += bsize) {
            u32 bi = off / bsize, blk = dir_bmap(copy, bi);
            if (!blk) { problem("  directory %u has a hole at block %u", dino, bi); unfixable++; continue; }
            if (blk == 0xFFFFFFFF || blk >= nblocks) { problem("  directory %u: block %u unreadable", dino, bi); unfixable++; continue; }
            if (rblk(blk, db)) { printf("fsck.ext2: read error in directory %u\n", dino); return 8; }
            int dirty = 0;
            u32 o = 0;
            while (o + 8 <= bsize) {
                u32 eino = L32(db + o), rl = L16(db + o + 4), nl = db[o + 6];
                if (rl < 8 || (rl & 3) || o + rl > bsize || nl + 8 > rl) {
                    problem("  directory %u: corrupt entry at offset %u", dino, off + o);
                    unfixable++;
                    break;
                }
                if (eino) {
                    if (eino <= ninodes && !tb(usedi, eino) && tb(maybe, eino)) {
                        problem("  inode %u is in use (named in directory %u) but free in the inode bitmap", eino, dino);
                        cb_(maybe, eino);
                        if (take_inode(eino)) return 8;
                        if (tb(isdir, eino)) {
                            problem("  inode %u is a directory: cannot repair a directory missing from the bitmap", eino);
                            unfixable++;
                        }
                        in = inode_ptr(dino, 0);
                    }
                    if (eino > ninodes || !tb(usedi, eino)) {
                        problem("  directory %u: entry points to unused inode %u", dino, eino);
                        if (fix) { S32(db + o, 0); dirty = 1; fixed++; }
                    } else {
                        if (links[eino] < 65535) links[eino]++;
                        if (nl == 1 && db[o + 8] == '.' && eino != dino) {
                            problem("  directory %u: '.' points to inode %u", dino, eino);
                            if (fix) { S32(db + o, dino); dirty = 1; fixed++; }
                        }
                        if (nl == 2 && db[o + 8] == '.' && db[o + 9] == '.') dotdot[dino] = eino;
                        if ((incompat & INCOMPAT_FILETYPE) && itype[eino] && db[o + 7] != dirent_type(itype[eino])) {
                            problem("  directory %u: entry for inode %u has file type %u, should be %u", dino, eino, db[o + 7], dirent_type(itype[eino]));
                            if (fix) { db[o + 7] = dirent_type(itype[eino]); dirty = 1; fixed++; }
                        }
                    }
                }
                o += rl;
            }
            if (dirty) { wblk(blk, db); unindex = 1; }
        }
        if (unindex && (L32(copy + 32) & INDEX_FL)) {        /* a changed htree directory is read as a plain list */
            in = inode_ptr(dino, 0);
            if (in) { S32(in + 32, L32(in + 32) & ~(u32)INDEX_FL); put_inode_dirty(); }
        }
    }

    /* ---- pass 3: connectivity ---- */
    printf("Pass 3: Checking directory connectivity\n");
    sb_(reach, 2);
    parent[2] = 2;
    for (int again = 1; again;) {
        again = 0;
        for (u32 d = 0; d < ndirs; d++) {
            u32 dino = dirlist[d];
            if (!tb(reach, dino) || tb(scanned, dino)) continue;
            sb_(scanned, dino);
            u8 *in = inode_ptr(dino, 0);
            if (!in) return 8;
            u8 copy[128];
            memcpy(copy, in, 128);
            for (u32 off = 0; off + bsize <= L32(copy + 4); off += bsize) {
                u32 blk = dir_bmap(copy, off / bsize);
                if (!blk || blk >= nblocks || rblk(blk, db)) continue;
                for (u32 o = 0; o + 8 <= bsize;) {
                    u32 eino = L32(db + o), rl = L16(db + o + 4), nl = db[o + 6];
                    if (rl < 8 || o + rl > bsize) break;
                    int dots = (nl == 1 && db[o + 8] == '.') || (nl == 2 && db[o + 8] == '.' && db[o + 9] == '.');
                    if (eino && !dots && eino <= ninodes && tb(usedi, eino) && tb(isdir, eino) && !tb(reach, eino)) {
                        sb_(reach, eino); parent[eino] = dino; again = 1;
                    }
                    o += rl;
                }
            }
        }
    }
    /* '..' must name the real parent */
    for (u32 d = 0; d < ndirs; d++) {
        u32 dino = dirlist[d];
        if (!tb(reach, dino) || dino == 2) continue;
        if (dotdot[dino] != parent[dino]) {
            problem("  directory %u: '..' is %u, should be %u", dino, dotdot[dino], parent[dino]);
            if (fix) {
                u8 *in = inode_ptr(dino, 0);
                u32 blk = in ? L32(in + 40) : 0;
                if (blk && !rblk(blk, db) && L32(db + 12) == dotdot[dino]) {
                    /* '..' is the second entry of the first block */
                    u32 o2 = L16(db + 4);
                    if (o2 + 12 <= bsize && L16(db + o2 + 4) >= 8 && db[o2 + 6] == 2) {
                        if (dotdot[dino] <= ninodes && links[dotdot[dino]] > 0) links[dotdot[dino]]--;
                        S32(db + o2, parent[dino]);
                        wblk(blk, db);
                        if (links[parent[dino]] < 65535) links[parent[dino]]++;
                        fixed++;
                    }
                }
            }
        }
    }

    /* orphans: inodes in use that no reachable directory names */
    for (u32 ino = first_ino; ino <= ninodes; ino++) {
        if (!tb(usedi, ino)) continue;
        if (tb(isdir, ino) ? !tb(reach, ino) : links[ino] == 0) {
            problem("  inode %u is in use but not reachable from any directory", ino);
            if (norphans < MAXORPHANS) orphan[norphans] = ino;
            norphans++;
        }
    }
    if (norphans > MAXORPHANS) {
        problem("  %u unreachable inodes: fsck.ext2 attaches at most %u to /lost+found", norphans, MAXORPHANS);
        unfixable++;
    }

    /* ---- pass 4: link counts ---- */
    printf("Pass 4: Checking reference counts\n");
    int relink = 0;
    for (u32 ino = 2; ino <= ninodes; ino++) {
        if (!tb(usedi, ino) || (ino < first_ino && ino != 2)) continue;
        u8 *in = inode_ptr(ino, 0);
        if (!in) return 8;
        u32 lc = L16(in + 26);
        if (links[ino] && lc != links[ino]) {
            problem("  inode %u: link count is %u, should be %u", ino, lc, (u32)links[ino]);
            if (fix) { S16(in + 26, links[ino]); put_inode_dirty(); fixed++; }
        }
    }
    if (fix && norphans && !unfixable) {
        /* attach orphans to /lost+found using the filesystem library */
        if (ext2_mount()) { printf("fsck.ext2: cannot attach orphans (mount failed)\n"); unfixable++; }
        else {
            struct inode lf, root;
            if (ext2_iget(2, &root) || ext2_lookup(&root, "lost+found", 10, &(u32){0}) ) {
                if (ext2_mkdir(&root, "lost+found")) printf("fsck.ext2: cannot create /lost+found\n");
            }
            if (!ext2_namei("/lost+found", 2, &lf, 0)) {
                for (u32 k = 0; k < norphans; k++) {
                    char name[16];
                    u32 v = orphan[k]; int n = 0; char t[12];
                    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
                    name[0] = '#';
                    for (int j = 0; j < n; j++) name[1 + j] = t[n - 1 - j];
                    name[1 + n] = 0;
                    if (!ext2_link(&lf, name, orphan[k])) { printf("  attached inode %u as /lost+found/%s\n", orphan[k], name); fixed++; relink = 1; }
                    if (ext2_iget(lf.ino, &lf)) break;
                }
            }
        }
    }
    if (relink) {
        printf("fsck.ext2: orphans attached: run fsck.ext2 -y again to settle the link counts\n");
        return 1;
    }

    /* ---- pass 5: bitmaps and counts ---- */
    printf("Pass 5: Checking group summary information\n");
    u32 total_free_b = 0, total_free_i = 0;
    static u8 bb[4096];
    for (u32 g = 0; g < ngroups; g++) {
        u32 gs = first + g * bpg, gb = nblocks - gs < bpg ? nblocks - gs : bpg;
        u32 bbn = L32(gdt + g * 32 + 0), ibn = L32(gdt + g * 32 + 4);
        u32 freeb = 0, freei = 0, dirs = 0, badb = 0, badi = 0, pad = 0;
        if (rblk(bbn, bb) || rblk(ibn, ibm)) { printf("fsck.ext2: cannot read the bitmaps of group %u\n", g); return 8; }
        for (u32 i = 0; i < gb; i++) {
            int used = tb(blockmap, gs + i);
            if (!used) freeb++;
            if (used != tb(bb, i)) { badb++; if (used) sb_(bb, i); else cb_(bb, i); }
        }
        for (u32 i = gb; i < bsize * 8; i++) if (!tb(bb, i)) { pad++; sb_(bb, i); }        /* padding is "used" */
        for (u32 i = 0; i < ipg; i++) {
            u32 ino = g * ipg + i + 1;
            int used = tb(usedi, ino);
            if (!used) freei++;
            else if (tb(isdir, ino)) dirs++;
            if (used != tb(ibm, i)) { badi++; if (used) sb_(ibm, i); else cb_(ibm, i); }
        }
        for (u32 i = ipg; i < bsize * 8; i++) if (!tb(ibm, i)) { pad++; sb_(ibm, i); }
        if (badb) problem("  group %u: %u block bitmap difference(s)", g, badb);
        if (badi) problem("  group %u: %u inode bitmap difference(s)", g, badi);
        if (pad) problem("  group %u: bitmap padding not set", g);
        u8 *e = gdt + g * 32;
        if (L16(e + 12) != freeb || L16(e + 14) != freei || L16(e + 16) != dirs) {
            problem("  group %u: free counts or directory count wrong", g);
            S16(e + 12, freeb); S16(e + 14, freei); S16(e + 16, dirs);
            if (fix) fixed++;
        }
        if (fix && (badb || badi || pad)) { wblk(bbn, bb); wblk(ibn, ibm); fixed++; }
        total_free_b += freeb; total_free_i += freei;
    }
    if (L32(sb + 12) != total_free_b || L32(sb + 16) != total_free_i) {
        problem("  superblock free counts wrong (blocks %u, inodes %u)", L32(sb + 12), L32(sb + 16));
        S32(sb + 12, total_free_b); S32(sb + 16, total_free_i);
        if (fix) fixed++;
    }
    if (fix) {
        for (u32 i = 0; i < gdt_blocks; i++) wblk(gdt_start + i, gdt + i * bsize);
        for (u32 g = 0; g < ngroups; g++) {                         /* keep the backups in step */
            if (g == 0 || !has_backup(g)) continue;
            u32 gs = first + g * bpg;
            for (u32 i = 0; i < gdt_blocks; i++) wblk(gs + 1 + i, gdt + i * bsize);
        }
        /* superblock: block 1 for 1K blocks, byte 1024 of block 0 otherwise */
        static u8 blk0[4096];
        if (bsize == 1024) { fs_blk_write(2, 2, sb); }
        else if (!rblk(0, blk0)) { memcpy(blk0 + 1024, sb, 1024); wblk(0, blk0); }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev = 0;
    int want_fix = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-y")) want_fix = 1;
        else if (!strcmp(argv[i], "-n")) want_fix = 0;
        else dev = argv[i];
    }
    if (!dev) { printf("usage: fsck.ext2 [-y] device\n"); return 2; }
    port = sys_port_find("disk");
    if (port < 0) { printf("fsck.ext2: no disk server\n"); return 8; }
    struct umsg m;
    m.type = DISK_OPEN; m.a[0] = (u32)dev; m.a[1] = DISK_RAW;
    h = sys_call(port, &m);
    if (h < 0) { printf("fsck.ext2: cannot open %s (mounted, or no such device)\n", dev); return 8; }

    if (fs_blk_read(2, 2, sb) || L16(sb + 56) != 0xEF53) { printf("fsck.ext2: %s has no ext2 filesystem\n", dev); return 8; }
    u32 logbs = L32(sb + 24);
    if (logbs > 2) { printf("fsck.ext2: block size above 4096 not supported\n"); return 8; }
    bsize = 1024u << logbs;
    ninodes = L32(sb + 0); nblocks = L32(sb + 4); first = L32(sb + 20);
    bpg = L32(sb + 32); ipg = L32(sb + 40);
    u32 rev = L32(sb + 76);
    if (rev > 1) { printf("fsck.ext2: unknown ext2 revision %u\n", rev); return 8; }
    isize = rev ? L16(sb + 88) : 128;
    first_ino = rev ? L32(sb + 84) : 11;
    compat = rev ? L32(sb + 92) : 0; incompat = rev ? L32(sb + 96) : 0; rocompat = rev ? L32(sb + 100) : 0;
    sparse = rocompat & RO_SPARSE;
    rsv_gdt = (compat & COMPAT_RESIZE) ? L16(sb + 206) : 0;
    if (incompat & ~(u32)INCOMPAT_FILETYPE) { printf("fsck.ext2: unsupported incompatible features %x (ext3/ext4?): use e2fsck\n", incompat); return 8; }
    if (compat & COMPAT_HAS_JOURNAL) { printf("fsck.ext2: %s has an ext3 journal, which fsck.ext2 cannot check: use e2fsck\n", dev); return 8; }
    if (rocompat & ~(u32)(RO_SPARSE | RO_LARGE_FILE)) { printf("fsck.ext2: unsupported read-only features %x: use e2fsck\n", rocompat); return 8; }
    if (!bpg || !ipg || bpg > 8 * bsize || ipg > 8 * bsize || first != (bsize == 1024 ? 1u : 0u) || nblocks <= first + 2 ||
        isize < 128 || isize > bsize || (isize & (isize - 1)) || first_ino < 11 || first_ino > ninodes) {
        printf("fsck.ext2: bad superblock\n"); return 8;
    }
    ngroups = (nblocks - first + bpg - 1) / bpg;
    if (ninodes / ipg != ngroups || ninodes % ipg) { printf("fsck.ext2: superblock inconsistent (groups x inodes-per-group != inodes)\n"); return 8; }
    gdt_blocks = (ngroups * 32 + bsize - 1) / bsize;
    gdt_start = first + 1;
    if (gdt_start + gdt_blocks + rsv_gdt > nblocks) { printf("fsck.ext2: bad superblock (group table)\n"); return 8; }

    gdt = malloc(gdt_blocks * bsize);
    blockmap = malloc((nblocks + 7) / 8);
    aclmap = malloc((nblocks + 7) / 8);
    usedi = malloc((ninodes + 8) / 8);
    maybe = malloc((ninodes + 8) / 8);
    isdir = malloc((ninodes + 8) / 8);
    reach = malloc((ninodes + 8) / 8);
    scanned = malloc((ninodes + 8) / 8);
    itype = malloc(ninodes + 1);
    links = malloc((ninodes + 1) * 2);
    dotdot = malloc((ninodes + 1) * 4);
    parent = malloc((ninodes + 1) * 4);
    orphan = malloc(MAXORPHANS * 4);
    if (!gdt || !blockmap || !aclmap || !usedi || !maybe || !isdir || !reach || !scanned || !itype || !links || !dotdot ||
        !parent || !orphan) {
        printf("fsck.ext2: out of memory (%u inodes, %u blocks)\n", ninodes, nblocks);
        return 8;
    }

    printf("fsck.ext2: %s: %u blocks of %u bytes, %u groups, %u inodes%s\n", dev, nblocks, bsize, ngroups, ninodes, want_fix ? " (repair mode)" : "");
    fix = 0;                                         /* first a check that writes nothing */
    int r = run();
    if (r) return r;
    if (want_fix && errors && unfixable) {
        printf("fsck.ext2: %u problem(s) cannot be repaired safely: nothing was changed (use e2fsck)\n", unfixable);
        return 4;
    }
    if (want_fix && errors) {
        printf("fsck.ext2: repairing\n");
        fix = 1;
        r = run();
        if (r) return r;
    }
    printf("fsck.ext2: %s: %u files, %u/%u blocks used\n", dev, nfiles, nblocks - (L32(sb + 12)), nblocks);
    if (unfixable) { printf("fsck.ext2: %u problem(s) could not be repaired\n", unfixable); return 4; }
    if (errors && !fix) { printf("fsck.ext2: %u problem(s) found: run with -y to repair\n", errors); return 4; }
    if (fixed) { printf("fsck.ext2: %u repair(s) made\n", fixed); return 1; }
    printf("fsck.ext2: clean\n");
    return 0;
}
