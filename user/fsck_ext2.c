#include "fsenv.h"
#include "../kernel/diskproto.h"

/* fsck.ext2 [-y] DEVICE   (default: report only)
 * Checks an unmounted ext2 filesystem the way e2fsck does and, with -y, repairs
 * what it safely can: block and inode bitmaps, free counts, link counts,
 * directory entries pointing at unused inodes, '..' entries, and orphaned
 * inodes (moved to /lost+found as #inode). It cannot repair duplicate or
 * out-of-range block pointers: it reports them and exits with 4.
 * Exit codes: 0 clean, 1 repaired, 4 errors remain. */

static int port, h, fix;
static u32 bsize, nblocks, ninodes, ipg, bpg, first, ngroups, isize, first_ino;
static u8 *gdt;                          /* group descriptors, ngroups * 32 bytes */
static u8 *blockmap;                     /* blocks used, computed from what the inodes claim */
static u8 *usedi;                        /* inodes in use (computed) */
static u8 *reach;                        /* directories reachable from the root */
static u8 *scanned;
static u8 *links;                        /* references found per inode (saturating) */
static u32 *dotdot;                      /* '..' of each directory as found */
static u32 *parent;                      /* real parent of each directory from the walk */
static u32 *dirlist; static u32 ndirs;
static int errors, fixed, unfixable;

int fs_blk_read(u32 lba, u32 count, void *buf)
{
    struct umsg m;
    m.type = DISK_READ; m.a[0] = (u32)h; m.a[1] = lba; m.a[2] = count; m.a[3] = (u32)buf;
    return sys_call(port, &m);
}
int fs_blk_write(u32 lba, u32 count, const void *buf)
{
    struct umsg m;
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

static int rblk(u32 n, void *buf) { return fs_blk_read(n * (bsize / 512), bsize / 512, buf); }
static int wblk(u32 n, const void *buf) { return fs_blk_write(n * (bsize / 512), bsize / 512, buf); }

#define problem(...) do { printf(__VA_ARGS__); printf("\n"); errors++; } while (0)

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

static int has_backup(u32 g, u32 sparse)
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

static int mounted_check(void) { return 0; }

int main(int argc, char **argv)
{
    const char *dev = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-y")) fix = 1;
        else if (!strcmp(argv[i], "-n")) fix = 0;
        else dev = argv[i];
    }
    (void)mounted_check;
    if (!dev) { printf("usage: fsck.ext2 [-y] device\n"); return 2; }
    port = sys_port_find("disk");
    if (port < 0) { printf("fsck.ext2: no disk server\n"); return 8; }
    struct umsg m;
    m.type = DISK_OPEN; m.a[0] = (u32)dev; m.a[1] = DISK_RAW;
    h = sys_call(port, &m);
    if (h < 0) { printf("fsck.ext2: cannot open %s (mounted, or no such device)\n", dev); return 8; }

    u8 sb[1024];
    if (fs_blk_read(2, 2, sb) || L16(sb + 56) != 0xEF53) { printf("fsck.ext2: %s has no ext2 filesystem\n", dev); return 8; }
    u32 logbs = L32(sb + 24);
    bsize = 1024u << logbs;
    if (logbs > 2) { printf("fsck.ext2: block size %u not supported\n", bsize); return 8; }
    ninodes = L32(sb + 0); nblocks = L32(sb + 4); first = L32(sb + 20);
    bpg = L32(sb + 32); ipg = L32(sb + 40);
    u32 rev = L32(sb + 76);
    isize = rev ? L16(sb + 88) : 128;
    first_ino = rev ? L32(sb + 84) : 11;
    u32 compat = rev ? L32(sb + 92) : 0, incompat = rev ? L32(sb + 96) : 0, rocompat = rev ? L32(sb + 100) : 0;
    u32 sparse = rocompat & 1, rsv_gdt = (compat & 0x10) ? L16(sb + 206) : 0;
    if (incompat & ~2u) { printf("fsck.ext2: unsupported incompatible features %x\n", incompat); return 8; }
    if (!bpg || !ipg || isize < 128) { printf("fsck.ext2: bad superblock\n"); return 8; }
    ngroups = (nblocks - first + bpg - 1) / bpg;
    if (ngroups * ipg != ninodes) { printf("fsck.ext2: superblock inconsistent (groups x inodes-per-group != inodes)\n"); return 8; }
    u32 gdt_blocks = (ngroups * 32 + bsize - 1) / bsize;
    u32 gdt_start = first + 1;

    gdt = malloc(gdt_blocks * bsize);
    blockmap = malloc((nblocks + 7) / 8);
    usedi = malloc((ninodes + 8) / 8);
    reach = malloc((ninodes + 8) / 8);
    scanned = malloc((ninodes + 8) / 8);
    links = malloc(ninodes + 1);
    dotdot = malloc((ninodes + 1) * 4);
    parent = malloc((ninodes + 1) * 4);
    if (!gdt || !blockmap || !usedi || !reach || !scanned || !links || !dotdot || !parent) { printf("fsck.ext2: out of memory\n"); return 8; }
    memset(blockmap, 0, (nblocks + 7) / 8); memset(usedi, 0, (ninodes + 8) / 8);
    memset(reach, 0, (ninodes + 8) / 8); memset(scanned, 0, (ninodes + 8) / 8);
    memset(links, 0, ninodes + 1); memset(dotdot, 0, (ninodes + 1) * 4); memset(parent, 0, (ninodes + 1) * 4);
    dirlist = malloc(4096 * 4);
    for (u32 i = 0; i < gdt_blocks; i++) if (rblk(gdt_start + i, gdt + i * bsize)) { printf("fsck.ext2: cannot read the group table\n"); return 8; }

    printf("fsck.ext2: %s: %u blocks of %u bytes, %u groups, %u inodes%s\n", dev, nblocks, bsize, ngroups, ninodes, fix ? " (repair mode)" : "");

    /* ---- metadata blocks ---- */
    sb_(blockmap, 0);
    for (u32 g = 0; g < ngroups; g++) {
        u32 gs = first + g * bpg, gb = nblocks - gs < bpg ? nblocks - gs : bpg;
        u32 bb = L32(gdt + g * 32 + 0), ib = L32(gdt + g * 32 + 4), it = L32(gdt + g * 32 + 8);
        u32 itb = (ipg * isize + bsize - 1) / bsize;
        if (has_backup(g, sparse))
            for (u32 k = 0; k < 1 + gdt_blocks + rsv_gdt; k++) if (gs + k < nblocks) sb_(blockmap, gs + k);
        if (bb < gs || bb >= gs + gb || ib < gs || ib >= gs + gb || it < gs || it + itb > gs + gb) {
            printf("fsck.ext2: group %u has its tables outside the group: cannot repair\n", g);
            return 8;
        }
        sb_(blockmap, bb); sb_(blockmap, ib);
        for (u32 k = 0; k < itb; k++) sb_(blockmap, it + k);
    }

    /* ---- pass 1: inodes and their blocks ---- */
    printf("Pass 1: Checking inodes, blocks, and sizes\n");
    static u8 ibm[4096];
    u32 nfiles = 0;
    for (u32 g = 0; g < ngroups; g++) {
        if (rblk(L32(gdt + g * 32 + 4), ibm)) { printf("fsck.ext2: cannot read an inode bitmap\n"); return 8; }
        for (u32 i = 0; i < ipg; i++) {
            u32 ino = g * ipg + i + 1;
            u8 *in = inode_ptr(ino, 0);
            if (!in) { printf("fsck.ext2: read error in the inode table\n"); return 8; }
            u32 mode = L16(in), lc = L16(in + 26), dtime = L32(in + 20);
            int reserved = ino < first_ino && ino != 2;
            int live = (mode != 0 && lc > 0 && dtime == 0) || ino == 2;
            if (reserved && !live) { sb_(usedi, ino); continue; }     /* reserved inodes count as used */
            if (!live) {
                continue;                                        /* marked used but empty: fixed by the bitmap pass */
            }
            sb_(usedi, ino);
            nfiles++;
            u32 t = mode & 0170000;
            if (t != 0040000 && t != 0100000 && t != 0120000 && t != 0020000 && t != 0060000 && t != 0010000 && t != 0140000) {
                problem("  inode %u has an invalid mode %o", ino, mode);
                unfixable++;
                continue;
            }
            if (t == 0040000 && ndirs < 4096) dirlist[ndirs++] = ino;
            int fast_symlink = (t == 0120000 && L32(in + 28) == 0);
            if (t == 0020000 || t == 0060000 || t == 0010000 || t == 0140000 || fast_symlink) continue;   /* no blocks */
            cur_blocks = 0;
            u8 copy[128];
            memcpy(copy, in, 128);
            walk_inode(copy, ino, claim);
            icache_blk = 0xFFFFFFFF;
            in = inode_ptr(ino, 0);
            u32 want = cur_blocks * (bsize / 512);
            if (L32(in + 28) != want) {
                problem("  inode %u: i_blocks is %u, should be %u", ino, L32(in + 28), want);
                if (fix) { S32(in + 28, want); put_inode_dirty(); fixed++; }
            }
        }
    }

    /* ---- pass 2: directories ---- */
    printf("Pass 2: Checking directory structure\n");
    static u8 db[4096];
    for (u32 d = 0; d < ndirs; d++) {
        u32 dino = dirlist[d];
        u8 *in = inode_ptr(dino, 0);
        u8 copy[128];
        memcpy(copy, in, 128);
        u32 size = L32(copy + 4);
        for (u32 off = 0; off < size; off += bsize) {
            u32 bi = off / bsize, blk;
            if (bi < 12) blk = L32(copy + 40 + bi * 4);
            else {                                                 /* indirect directory blocks: single level */
                static u8 ind[4096];
                u32 ib = L32(copy + 40 + 12 * 4);
                blk = 0;
                if (ib && bi - 12 < bsize / 4 && !rblk(ib, ind)) blk = L32(ind + (bi - 12) * 4);
            }
            if (!blk) { problem("  directory %u has a hole at block %u", dino, bi); unfixable++; continue; }
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
                    if (eino > ninodes || !tb(usedi, eino)) {
                        problem("  directory %u: entry points to unused inode %u", dino, eino);
                        if (fix) { S32(db + o, 0); dirty = 1; fixed++; }
                    } else {
                        if (links[eino] < 255) links[eino]++;
                        if (nl == 1 && db[o + 8] == '.' && eino != dino) {
                            problem("  directory %u: '.' points to inode %u", dino, eino);
                            if (fix) { S32(db + o, dino); dirty = 1; fixed++; }
                        }
                        if (nl == 2 && db[o + 8] == '.' && db[o + 9] == '.') dotdot[dino] = eino;
                    }
                }
                o += rl;
            }
            if (dirty) wblk(blk, db);
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
            u8 copy[128];
            memcpy(copy, in, 128);
            for (u32 off = 0; off < L32(copy + 4); off += bsize) {
                u32 bi = off / bsize, blk = bi < 12 ? L32(copy + 40 + bi * 4) : 0;
                if (!blk || rblk(blk, db)) continue;
                for (u32 o = 0; o + 8 <= bsize;) {
                    u32 eino = L32(db + o), rl = L16(db + o + 4), nl = db[o + 6];
                    if (rl < 8 || o + rl > bsize) break;
                    int dots = (nl == 1 && db[o + 8] == '.') || (nl == 2 && db[o + 8] == '.' && db[o + 9] == '.');
                    if (eino && !dots && eino <= ninodes && tb(usedi, eino)) {
                        for (u32 k = 0; k < ndirs; k++)
                            if (dirlist[k] == eino && !tb(reach, eino)) { sb_(reach, eino); parent[eino] = dino; again = 1; }
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
                u32 blk = L32(in + 40);
                if (blk && !rblk(blk, db) && L32(db + 12) == dotdot[dino]) {
                    /* '..' is the second entry of the first block */
                    u32 o2 = L16(db + 4);
                    if (L16(db + o2 + 4) >= 8 && db[o2 + 6] == 2) {
                        links[dotdot[dino]] = links[dotdot[dino]] > 0 ? links[dotdot[dino]] - 1 : 0;
                        S32(db + o2, parent[dino]);
                        wblk(blk, db);
                        if (links[parent[dino]] < 255) links[parent[dino]]++;
                        fixed++;
                    }
                }
            }
        }
    }

    /* orphans: inodes in use that no reachable directory names */
    u32 norphans = 0;
    static u32 orphan[512];
    for (u32 ino = first_ino; ino <= ninodes; ino++) {
        if (!tb(usedi, ino)) continue;
        int isdir = 0;
        for (u32 k = 0; k < ndirs; k++) if (dirlist[k] == ino) isdir = 1;
        if (isdir ? !tb(reach, ino) : links[ino] == 0) {
            problem("  inode %u is in use but not reachable from any directory", ino, 0);
            if (norphans < 512) orphan[norphans++] = ino;
        }
    }

    /* ---- pass 4: link counts ---- */
    printf("Pass 4: Checking reference counts\n");
    int relink = 0;
    for (u32 ino = 2; ino <= ninodes; ino++) {
        if (!tb(usedi, ino) || (ino < first_ino && ino != 2)) continue;
        u8 *in = inode_ptr(ino, 0);
        u32 lc = L16(in + 26);
        if (links[ino] && lc != links[ino]) {
            problem("  inode %u: link count is %u, should be %u", ino, lc, (u32)links[ino]);
            if (fix) { S16(in + 26, links[ino]); put_inode_dirty(); fixed++; }
        }
    }
    if (fix && norphans) {
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
        u32 freeb = 0, freei = 0, dirs = 0, badb = 0, badi = 0;
        if (rblk(bbn, bb) || rblk(ibn, ibm)) { printf("fsck.ext2: cannot read the bitmaps of group %u\n", g); return 8; }
        for (u32 i = 0; i < gb; i++) {
            int used = tb(blockmap, gs + i);
            if (!used) freeb++;
            if (used != tb(bb, i)) { badb++; if (used) sb_(bb, i); else cb_(bb, i); }
        }
        for (u32 i = 0; i < ipg; i++) {
            u32 ino = g * ipg + i + 1;
            int used = tb(usedi, ino);
            if (!used) freei++;
            if (used != tb(ibm, i)) { badi++; if (used) sb_(ibm, i); else cb_(ibm, i); }
        }
        for (u32 k = 0; k < ndirs; k++) if ((dirlist[k] - 1) / ipg == g) dirs++;
        if (badb) problem("  group %u: %u block bitmap difference(s)", g, badb);
        if (badi) problem("  group %u: %u inode bitmap difference(s)", g, badi);
        u8 *e = gdt + g * 32;
        if (L16(e + 12) != freeb || L16(e + 14) != freei || L16(e + 16) != dirs) {
            problem("  group %u: free counts or directory count wrong", g, 0);
            S16(e + 12, freeb); S16(e + 14, freei); S16(e + 16, dirs);
            if (fix) fixed++;
        }
        if (fix && (badb || badi)) { wblk(bbn, bb); wblk(ibn, ibm); fixed++; }
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
            if (g == 0 || !has_backup(g, sparse)) continue;
            u32 gs = first + g * bpg;
            for (u32 i = 0; i < gdt_blocks; i++) wblk(gs + 1 + i, gdt + i * bsize);
        }
        /* superblock: block 1 for 1K blocks, byte 1024 of block 0 otherwise */
        u8 blk0[4096];
        if (bsize == 1024) { fs_blk_write(2, 2, sb); }
        else if (!rblk(0, blk0)) { memcpy(blk0 + 1024, sb, 1024); wblk(0, blk0); }
    }

    printf("fsck.ext2: %s: %u files, %u/%u blocks used\n", dev, nfiles, nblocks - total_free_b, nblocks);
    if (unfixable) { printf("fsck.ext2: %u problem(s) could not be repaired\n", unfixable); return 4; }
    if (errors && !fix) { printf("fsck.ext2: %u problem(s) found: run with -y to repair\n", errors); return 4; }
    if (fixed) { printf("fsck.ext2: %u repair(s) made\n", fixed); return 1; }
    printf("fsck.ext2: clean\n");
    return 0;
}
