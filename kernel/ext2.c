#ifdef FSD
#include "../user/fsenv.h"      /* running as the filesystem server (user mode) */
#else
#include "kernel.h"
#endif

/* Read-only ext2 (revision 0/1, 128-byte or larger inodes, no extents),
 * written from the public format description. On-disk numbers are
 * little-endian; every field is swapped when read, file data never is. */

static inline u32 le32(u32 v)
{
    __asm__("rol.w #8,%0\n\tswap %0\n\trol.w #8,%0" : "+d"(v));
    return v;
}
static inline u16 le16(u16 v) { return (u16)((v >> 8) | (v << 8)); }

#define NBUF 48
static inline u32 strlen_(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static inline int strcmp_(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static inline int memcmp_(const void *a, const void *b, u32 n)
{
    const u8 *p = a, *q = b;
    while (n--) { if (*p != *q) return *p - *q; p++; q++; }
    return 0;
}
static struct { u32 blk, stamp; u8 *data; } buf[NBUF];
static u32 stamp;

static u32 part_lba, bsize, ninodes_per_group, inode_size;
static u32 first_data_block, blocks_per_group, ngroups, boot_time;
static u32 sectors_per_block, total_blocks, ninodes;
static u32 gd_blk;                      /* first block of the group descriptor table */
static int mounted;

/* returns the cache slot holding block blk (loading it), or -1.
 * Fast paths first: the last slot used, then a small hash of block
 * numbers; only a miss scans the whole cache for a victim. */
static int last_slot;
static u8 hashslot[64];                 /* blk & 63 -> slot that probably holds it */

static int bfind(u32 blk)
{
    int victim = 0;
    u32 oldest = 0xFFFFFFFF;
    stamp++;
    if (buf[last_slot].data && buf[last_slot].blk == blk) {
        buf[last_slot].stamp = stamp;
        return last_slot;
    }
    int h = hashslot[blk & 63];
    if (buf[h].data && buf[h].blk == blk) {
        buf[h].stamp = stamp;
        return last_slot = h;
    }
    for (int i = 0; i < NBUF; i++) {
        if (buf[i].data && buf[i].blk == blk) {
            buf[i].stamp = stamp;
            hashslot[blk & 63] = (u8)i;
            return last_slot = i;
        }
        if (!buf[i].data) {
            victim = i;
            oldest = 0;
        } else if (buf[i].stamp < oldest) {
            oldest = buf[i].stamp;
            victim = i;
        }
    }
    if (!buf[victim].data) {
        buf[victim].data = fs_alloc(bsize);
        if (!buf[victim].data)
            return -1;
    }
    if (fs_blk_read(part_lba + blk * sectors_per_block, sectors_per_block,
                    buf[victim].data))
        return -1;
    buf[victim].blk = blk;
    buf[victim].stamp = stamp;
    hashslot[blk & 63] = (u8)victim;
    return last_slot = victim;
}

/* pointer to the cached block: valid only until the next bfind/bread/bwrite */
static const u8 *bptr(u32 blk)
{
    int i = bfind(blk);
    return i < 0 ? 0 : buf[i].data;
}

static int bread_copy(u32 blk, u32 off, void *dst, u32 len)
{
    int i = bfind(blk);
    if (i < 0)
        return -1;
    memcpy(dst, buf[i].data + off, len);
    return 0;
}

/* write-through: the cache and the disk are updated together */
static int bwrite_copy(u32 blk, u32 off, const void *src, u32 len)
{
    int i = bfind(blk);
    if (i < 0)
        return -1;
    memcpy(buf[i].data + off, src, len);
    return fs_blk_write(part_lba + blk * sectors_per_block, sectors_per_block,
                     buf[i].data);
}

static u32 rd32(const u8 *p) { return le32(*(const u32 *)p); }

/* ---- the device is a partition (or a bare disk) opened through the disk
 * server: sector 0 of it is where the filesystem starts ---- */

int ext2_probe(void)
{
    u8 sec[512];
    if (fs_blk_read(2, 1, sec))         /* byte 1024: superblock */
        return 0;
    return le16(*(u16 *)(sec + 56)) == 0xEF53;
}

int ext2_mount(void)
{
    u8 sb[1024];
    part_lba = 0;
    if (fs_blk_read(part_lba + 2, 2, sb))
        return -1;
    u32 logbs = rd32(sb + 24);
    bsize = 1024u << logbs;
    if (logbs > 2) {
        fs_log("ext2: block size %u not supported\n", bsize);
        return -1;
    }
    sectors_per_block = bsize / 512;
    ninodes = rd32(sb + 0);
    total_blocks = rd32(sb + 4);
    ninodes_per_group = rd32(sb + 40);
    first_data_block = rd32(sb + 20);
    blocks_per_group = rd32(sb + 32);
    ngroups = (total_blocks - first_data_block + blocks_per_group - 1) / blocks_per_group;
    boot_time = rd32(sb + 48);          /* last write time: a clock of sorts */
    inode_size = rd32(sb + 76) == 0 ? 128 : le16(*(u16 *)(sb + 88));
    gd_blk = (bsize == 1024) ? 2 : 1;
    mounted = 1;
    fs_log("ext2: %u KB blocks, %u blocks, %u inodes\n",
            bsize / 1024, total_blocks, ninodes);
    return 0;
}

int ext2_iget(u32 ino, struct inode *out)
{
    if (!mounted || ino == 0 || ino > ninodes)
        return -1;
    u32 grp = (ino - 1) / ninodes_per_group;
    u32 idx = (ino - 1) % ninodes_per_group;
    u32 gd[2];
    u32 gdoff = grp * 32;
    if (bread_copy(gd_blk + gdoff / bsize, gdoff % bsize + 8, gd, 4))
        return -1;
    u32 itable = le32(gd[0]);
    u32 pos = idx * inode_size;
    u8 raw[128];
    if (bread_copy(itable + pos / bsize, pos % bsize, raw, 128))
        return -1;
    out->ino = ino;
    out->mode = le16(*(u16 *)(raw + 0));
    out->uid = le16(*(u16 *)(raw + 2));
    out->size = rd32(raw + 4);
    out->mtime = rd32(raw + 16);
    out->gid = le16(*(u16 *)(raw + 24));
    out->links = le16(*(u16 *)(raw + 26));
    out->nblocks = rd32(raw + 28);
    for (int i = 0; i < 15; i++)
        out->block[i] = rd32(raw + 40 + i * 4);
    return 0;
}

static u32 ind(u32 blk, u32 idx)
{
    u32 v;
    if (!blk || bread_copy(blk, idx * 4, &v, 4))
        return 0;
    return le32(v);
}

u32 ext2_bmap(const struct inode *in, u32 n)
{
    u32 per = bsize / 4;
    if (n < 12)
        return in->block[n];
    n -= 12;
    if (n < per)
        return ind(in->block[12], n);
    n -= per;
    if (n < per * per)
        return ind(ind(in->block[13], n / per), n % per);
    n -= per * per;
    return ind(ind(ind(in->block[14], n / (per * per)), (n / per) % per), n % per);
}

int ext2_read(const struct inode *in, u32 off, void *dst, u32 n)
{
    u8 *d = dst;
    if (off >= in->size)
        return 0;
    if (n > in->size - off)
        n = in->size - off;
    u32 done = 0;
    while (done < n) {
        u32 pos = off + done;
        u32 chunk = bsize - pos % bsize;
        if (chunk > n - done)
            chunk = n - done;
        u32 blk = ext2_bmap(in, pos / bsize);
        if (blk) {
            if (bread_copy(blk, pos % bsize, d + done, chunk))
                return -1;
        } else {
            memset(d + done, 0, chunk);     /* hole */
        }
        done += chunk;
    }
    return (int)n;
}

/* next live directory entry at/after byte offset *cookie, parsed in place in
 * the cache (one block lookup per call, not per field); returns 1, 0 at the
 * end, -1 on error */
int ext2_readdir(const struct inode *dir, u32 *cookie, struct udirent *out)
{
    u32 off = *cookie;
    while (off < dir->size) {
        u32 blk = ext2_bmap(dir, off / bsize);
        const u8 *p = blk ? bptr(blk) : 0;
        if (!p)
            return -1;
        u32 end = bsize - off % bsize;          /* bytes left in this block */
        const u8 *e = p + off % bsize;
        u32 rl = le16(*(const u16 *)(e + 4));
        if (rl < 8 || rl > end)
            return -1;
        off += rl;
        u32 ino = rd32(e);
        if (ino == 0)
            continue;
        u32 nl = e[6];
        if (nl > sizeof out->name - 1)
            nl = sizeof out->name - 1;
        memcpy(out->name, e + 8, nl);
        out->name[nl] = 0;
        out->ino = ino;
        out->type = e[7];
        *cookie = off;
        return 1;
    }
    *cookie = off;
    return 0;
}

int ext2_lookup(const struct inode *dir, const char *name, u32 len, u32 *ino)
{
    if (!S_ISDIR(dir->mode))
        return -1;
    for (u32 b = 0; b * bsize < dir->size; b++) {
        u32 blk = ext2_bmap(dir, b);
        const u8 *p = blk ? bptr(blk) : 0;
        if (!p)
            return -1;
        for (u32 o = 0; o + 8 <= bsize;) {
            u32 rl = le16(*(const u16 *)(p + o + 4));
            if (rl < 8 || o + rl > bsize)
                return -1;
            if (p[o + 6] == len && rd32(p + o) != 0) {
                u32 i = 0;
                while (i < len && p[o + 8 + i] == (u8)name[i])
                    i++;
                if (i == len) {
                    *ino = rd32(p + o);
                    return 0;
                }
            }
            o += rl;
        }
    }
    return -1;
}

/* resolve a path; cwd is the starting inode for relative paths */
int ext2_namei(const char *path, u32 cwd, struct inode *out, int depth)
{
    u32 cur = (path[0] == '/') ? 2 : cwd;
    struct inode in;
    if (depth > 8)
        return -1;
    if (ext2_iget(cur, &in))
        return -1;
    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        const char *e = path;
        while (*e && *e != '/')
            e++;
        u32 next;
        if (ext2_lookup(&in, path, (u32)(e - path), &next))
            return -1;
        struct inode nin;
        if (ext2_iget(next, &nin))
            return -1;
        if (S_ISLNK(nin.mode)) {
            char target[256];
            u32 n = nin.size < 255 ? nin.size : 255;
            if (nin.size < 60) {
                memcpy(target, nin.block, n);   /* fast symlink: in the inode */
            } else if (ext2_read(&nin, 0, target, n) != (int)n) {
                return -1;
            }
            target[n] = 0;
            struct inode t;
            if (ext2_namei(target, in.ino, &t, depth + 1))
                return -1;
            nin = t;
        }
        in = nin;
        path = e;
        if (*path && !S_ISDIR(in.mode))
            return -1;
    }
    *out = in;
    return 0;
}


/* ================= writing ================= */

static inline void wr32(u8 *p, u32 v) { *(u32 *)p = le32(v); }
static inline void wr16(u8 *p, u16 v) { *(u16 *)p = le16(v); }

static u32 fs_now(void)
{
    return boot_time + fs_ticks() / 100;
}

/* group descriptor field helpers: field at byte `f` of descriptor g */
static int gd_get(u32 g, u32 f, u32 size, u32 *v)
{
    u32 o = g * 32 + f;
    u8 b[4];
    if (bread_copy(gd_blk + o / bsize, o % bsize, b, size))
        return -1;
    *v = size == 2 ? le16(*(u16 *)b) : le32(*(u32 *)b);
    return 0;
}

static int gd_add(u32 g, u32 f, int delta)      /* 16-bit counters */
{
    u32 v;
    if (gd_get(g, f, 2, &v))
        return -1;
    u8 b[2];
    wr16(b, (u16)(v + (u32)delta));
    u32 o = g * 32 + f;
    return bwrite_copy(gd_blk + o / bsize, o % bsize, b, 2);
}

static int sb_add(u32 f, int delta)             /* 32-bit counters in the superblock */
{
    u32 blk = bsize == 1024 ? 1 : 0, base = bsize == 1024 ? 0 : 1024;
    u8 b[4];
    if (bread_copy(blk, base + f, b, 4))
        return -1;
    wr32(b, rd32(b) + (u32)delta);
    return bwrite_copy(blk, base + f, b, 4);
}

static int bitmap_alloc(u32 bm_blk, u32 nbits, u32 first_bit)
{
    static u8 bm[4096];
    if (bread_copy(bm_blk, 0, bm, bsize))
        return -1;
    for (u32 i = first_bit; i < nbits; i++)
        if (!(bm[i / 8] & (1u << (i % 8)))) {
            bm[i / 8] |= (u8)(1u << (i % 8));
            if (bwrite_copy(bm_blk, i / 8, &bm[i / 8], 1))
                return -1;
            return (int)i;
        }
    return -2;                          /* full */
}

static int bitmap_free(u32 bm_blk, u32 bit)
{
    u8 b;
    if (bread_copy(bm_blk, bit / 8, &b, 1))
        return -1;
    b &= (u8)~(1u << (bit % 8));
    return bwrite_copy(bm_blk, bit / 8, &b, 1);
}

static u32 alloc_block(u32 goal_group)
{
    for (u32 k = 0; k < ngroups; k++) {
        u32 g = (goal_group + k) % ngroups, bmb, nbits = blocks_per_group;
        if (gd_get(g, 0, 4, &bmb))
            return 0;
        if (g == ngroups - 1)
            nbits = total_blocks - first_data_block - g * blocks_per_group;
        int bit = bitmap_alloc(bmb, nbits, 0);
        if (bit == -1)
            return 0;
        if (bit < 0)
            continue;
        u32 blk = g * blocks_per_group + first_data_block + (u32)bit;
        static u8 zero[4096];
        memset(zero, 0, bsize);
        if (bwrite_copy(blk, 0, zero, bsize) || gd_add(g, 12, -1) || sb_add(12, -1))
            return 0;
        return blk;
    }
    return 0;
}

static void free_block(u32 blk)
{
    u32 g = (blk - first_data_block) / blocks_per_group, bit = (blk - first_data_block) % blocks_per_group, bmb;
    if (gd_get(g, 0, 4, &bmb))
        return;
    bitmap_free(bmb, bit);
    gd_add(g, 12, 1);
    sb_add(12, 1);
}

static u32 alloc_inode(int is_dir)
{
    for (u32 g = 0; g < ngroups; g++) {
        u32 bmb;
        if (gd_get(g, 4, 4, &bmb))
            return 0;
        int bit = bitmap_alloc(bmb, ninodes_per_group, g == 0 ? 10 : 0);
        if (bit == -1)
            return 0;
        if (bit < 0)
            continue;
        gd_add(g, 14, -1);
        if (is_dir)
            gd_add(g, 16, 1);
        sb_add(16, -1);
        return g * ninodes_per_group + (u32)bit + 1;
    }
    return 0;
}

static void free_inode(u32 ino, int is_dir)
{
    u32 g = (ino - 1) / ninodes_per_group, bmb;
    if (gd_get(g, 4, 4, &bmb))
        return;
    bitmap_free(bmb, (ino - 1) % ninodes_per_group);
    gd_add(g, 14, 1);
    if (is_dir)
        gd_add(g, 16, -1);
    sb_add(16, 1);
}

/* write the inode's fields back (other on-disk fields are kept) */
int ext2_iput(const struct inode *in)
{
    u32 grp = (in->ino - 1) / ninodes_per_group, idx = (in->ino - 1) % ninodes_per_group;
    u32 itable;
    if (gd_get(grp, 8, 4, &itable))
        return -1;
    u32 pos = idx * inode_size, blk = itable + pos / bsize, off = pos % bsize;
    u8 raw[128];
    if (bread_copy(blk, off, raw, 128))
        return -1;
    wr16(raw + 0, in->mode);
    wr16(raw + 2, in->uid);
    wr32(raw + 4, in->size);
    wr32(raw + 8, in->mtime);           /* atime */
    wr32(raw + 12, in->mtime);          /* ctime */
    wr32(raw + 16, in->mtime);
    wr32(raw + 20, in->links ? 0 : in->mtime);     /* dtime of a deleted inode */
    wr16(raw + 24, in->gid);
    wr16(raw + 26, in->links);
    wr32(raw + 28, in->nblocks);
    for (int i = 0; i < 15; i++)
        wr32(raw + 40 + i * 4, in->block[i]);
    return bwrite_copy(blk, off, raw, 128);
}

/* block number for file block n, allocating it (and indirect blocks) if missing */
static u32 bmap_alloc(struct inode *in, u32 n)
{
    u32 per = bsize / 4, spb = bsize / 512;
    u32 goal = (in->ino - 1) / ninodes_per_group;
    if (n < 12) {
        if (!in->block[n]) {
            in->block[n] = alloc_block(goal);
            if (in->block[n])
                in->nblocks += spb;
        }
        return in->block[n];
    }
    n -= 12;
    u32 *top, idx1 = 0, idx2 = 0;
    int levels;
    if (n < per) {
        top = &in->block[12]; idx1 = n; levels = 1;
    } else if (n - per < per * per) {
        n -= per;
        top = &in->block[13]; idx1 = n / per; idx2 = n % per; levels = 2;
    } else {
        return 0;                       /* triple-indirect files are not supported */
    }
    if (!*top) {
        *top = alloc_block(goal);
        if (!*top)
            return 0;
        in->nblocks += spb;
    }
    u32 tblk = *top;
    for (int lv = 0; lv < levels; lv++) {
        u32 idx = lv == 0 ? idx1 : idx2, ent;
        u8 b[4];
        if (bread_copy(tblk, idx * 4, b, 4))
            return 0;
        ent = rd32(b);
        int last = (lv == levels - 1);
        if (!ent) {
            ent = alloc_block(goal);
            if (!ent)
                return 0;
            in->nblocks += spb;
            wr32(b, ent);
            if (bwrite_copy(tblk, idx * 4, b, 4))
                return 0;
        }
        if (last)
            return ent;
        tblk = ent;
    }
    return 0;
}

int ext2_write(struct inode *in, u32 off, const void *src, u32 n)
{
    const u8 *s = src;
    u32 done = 0;
    while (done < n) {
        u32 pos = off + done, chunk = bsize - pos % bsize;
        if (chunk > n - done)
            chunk = n - done;
        u32 blk = bmap_alloc(in, pos / bsize);
        if (!blk)
            break;                      /* disk full or file too large */
        if (bwrite_copy(blk, pos % bsize, s + done, chunk))
            break;
        done += chunk;
    }
    if (off + done > in->size)
        in->size = off + done;
    in->mtime = fs_now();
    if (ext2_iput(in))
        return -1;
    return done || !n ? (int)done : -1;
}

static void free_tree(u32 blk, int level)
{
    if (!blk)
        return;
    if (level > 0) {
        u32 per = bsize / 4;
        for (u32 i = 0; i < per; i++) {
            u8 b[4];
            if (bread_copy(blk, i * 4, b, 4))
                break;
            u32 e = rd32(b);
            if (e)
                free_tree(e, level - 1);
        }
    }
    free_block(blk);
}

int ext2_truncate(struct inode *in)
{
    for (int i = 0; i < 12; i++)
        if (in->block[i])
            free_block(in->block[i]);
    free_tree(in->block[12], 1);
    free_tree(in->block[13], 2);
    free_tree(in->block[14], 3);
    for (int i = 0; i < 15; i++)
        in->block[i] = 0;
    in->size = 0;
    in->nblocks = 0;
    in->mtime = fs_now();
    return ext2_iput(in);
}

/* ---- directories ---- */

static u8 dbuf[4096];

static int dir_add(struct inode *dir, const char *name, u32 len, u32 ino)
{
    u32 need = 8 + ((len + 3) & ~3u);
    u32 nblk = dir->size / bsize;
    for (u32 i = 0; i < nblk; i++) {
        u32 blk = ext2_bmap(dir, i);
        if (!blk || bread_copy(blk, 0, dbuf, bsize))
            return -1;
        for (u32 o = 0; o + 8 <= bsize;) {
            u32 eino = rd32(dbuf + o), rl = le16(*(u16 *)(dbuf + o + 4));
            u32 elen = eino ? 8 + (((u32)dbuf[o + 6] + 3) & ~3u) : 0;
            if (rl < 8 || o + rl > bsize)
                return -1;
            if (rl - elen >= need) {
                u32 at = o + elen;
                if (elen) {                     /* split the live entry */
                    wr16(dbuf + o + 4, (u16)elen);
                    wr16(dbuf + at + 4, (u16)(rl - elen));
                } else {
                    wr16(dbuf + at + 4, (u16)rl);
                }
                wr32(dbuf + at, ino);
                dbuf[at + 6] = (u8)len;
                dbuf[at + 7] = 0;
                memcpy(dbuf + at + 8, name, len);
                return bwrite_copy(blk, 0, dbuf, bsize);
            }
            o += rl;
        }
    }
    u32 blk = bmap_alloc(dir, nblk);            /* no room: a new directory block */
    if (!blk)
        return -1;
    memset(dbuf, 0, bsize);
    wr32(dbuf, ino);
    wr16(dbuf + 4, (u16)bsize);
    dbuf[6] = (u8)len;
    memcpy(dbuf + 8, name, len);
    if (bwrite_copy(blk, 0, dbuf, bsize))
        return -1;
    dir->size += bsize;
    dir->mtime = fs_now();
    return ext2_iput(dir);
}

/* public: add an entry to a directory (used by fsck to attach orphans) */
int ext2_link(struct inode *dir, const char *name, u32 ino)
{
    return dir_add(dir, name, strlen_(name), ino);
}

static int dir_remove(struct inode *dir, const char *name, u32 len, u32 *ino_out)
{
    u32 nblk = dir->size / bsize;
    for (u32 i = 0; i < nblk; i++) {
        u32 blk = ext2_bmap(dir, i);
        if (!blk || bread_copy(blk, 0, dbuf, bsize))
            return -1;
        u32 prev = 0xFFFFFFFF;
        for (u32 o = 0; o + 8 <= bsize;) {
            u32 eino = rd32(dbuf + o), rl = le16(*(u16 *)(dbuf + o + 4)), nl = dbuf[o + 6];
            if (rl < 8 || o + rl > bsize)
                return -1;
            if (eino && nl == len && !memcmp_(dbuf + o + 8, name, len)) {
                *ino_out = eino;
                if (prev != 0xFFFFFFFF)
                    wr16(dbuf + prev + 4, (u16)(le16(*(u16 *)(dbuf + prev + 4)) + rl));
                else
                    wr32(dbuf + o, 0);
                return bwrite_copy(blk, 0, dbuf, bsize);
            }
            prev = o;
            o += rl;
        }
    }
    return -1;
}

static int dir_is_empty(const struct inode *dir)
{
    u32 cookie = 0;
    struct udirent e;
    while (ext2_readdir(dir, &cookie, &e) > 0)
        if (strcmp_(e.name, ".") && strcmp_(e.name, ".."))
            return 0;
    return 1;
}

/* split a path into parent directory and last component */
int ext2_namei_parent(const char *path, u32 cwd, struct inode *dir, char *name)
{
    char p[128];
    u32 n = 0;
    while (path[n] && n < sizeof p - 1) {
        p[n] = path[n];
        n++;
    }
    p[n] = 0;
    while (n > 1 && p[n - 1] == '/')
        p[--n] = 0;
    u32 slash = n;
    while (slash > 0 && p[slash - 1] != '/')
        slash--;
    u32 len = n - slash;
    if (len == 0 || len > 63 || (len == 1 && p[slash] == '.') ||
        (len == 2 && p[slash] == '.' && p[slash + 1] == '.'))
        return -1;
    memcpy(name, p + slash, len);
    name[len] = 0;
    if (slash == 0) {
        if (ext2_iget(cwd, dir))
            return -1;
    } else {
        p[slash] = 0;
        if (slash > 1 && p[slash - 1] == '/')
            p[slash - 1] = 0;
        if (ext2_namei(p[0] ? p : "/", cwd, dir, 0))
            return -1;
    }
    return S_ISDIR(dir->mode) ? 0 : -1;
}

static int new_inode(struct inode *out, u32 mode, int is_dir)
{
    u32 ino = alloc_inode(is_dir);
    if (!ino)
        return -1;
    memset(out, 0, sizeof *out);
    out->ino = ino;
    out->mode = (u16)mode;
    out->links = is_dir ? 2 : 1;
    out->mtime = fs_now();
    return 0;
}

/* create an empty regular file in dir */
int ext2_create(struct inode *dir, const char *name, struct inode *out)
{
    if (new_inode(out, S_IFREG | 0644, 0))
        return -1;
    if (ext2_iput(out) || dir_add(dir, name, strlen_(name), out->ino)) {
        free_inode(out->ino, 0);
        return -1;
    }
    return 0;
}

int ext2_mkdir(struct inode *dir, const char *name)
{
    struct inode d;
    u32 len = strlen_(name);
    u32 tmp;
    if (ext2_lookup(dir, name, len, &tmp) == 0)
        return -2;                      /* exists */
    if (new_inode(&d, S_IFDIR | 0755, 1))
        return -1;
    u32 blk = bmap_alloc(&d, 0);
    if (!blk) {
        free_inode(d.ino, 1);
        return -1;
    }
    d.size = bsize;
    memset(dbuf, 0, bsize);
    wr32(dbuf, d.ino);
    wr16(dbuf + 4, 12);
    dbuf[6] = 1;
    dbuf[8] = '.';
    wr32(dbuf + 12, dir->ino);
    wr16(dbuf + 16, (u16)(bsize - 12));
    dbuf[18] = 2;
    dbuf[20] = '.';
    dbuf[21] = '.';
    if (bwrite_copy(blk, 0, dbuf, bsize) || ext2_iput(&d) ||
        dir_add(dir, name, len, d.ino))
        return -1;
    dir->links++;
    dir->mtime = fs_now();
    return ext2_iput(dir);
}

int ext2_unlink(struct inode *dir, const char *name)
{
    u32 len = strlen_(name), ino;
    if (ext2_lookup(dir, name, len, &ino))
        return -1;
    struct inode in;
    if (ext2_iget(ino, &in) || S_ISDIR(in.mode))
        return -1;
    if (dir_remove(dir, name, len, &ino))
        return -1;
    if (in.links > 1) {
        in.links--;
        return ext2_iput(&in);
    }
    ext2_truncate(&in);
    in.links = 0;
    ext2_iput(&in);
    free_inode(in.ino, 0);
    return 0;
}

int ext2_rmdir(struct inode *dir, const char *name)
{
    u32 len = strlen_(name), ino;
    if (ext2_lookup(dir, name, len, &ino))
        return -1;
    struct inode in;
    if (ext2_iget(ino, &in) || !S_ISDIR(in.mode))
        return -1;
    if (!dir_is_empty(&in))
        return -2;
    if (dir_remove(dir, name, len, &ino))
        return -1;
    ext2_truncate(&in);
    in.links = 0;
    ext2_iput(&in);
    free_inode(in.ino, 1);
    dir->links--;
    dir->mtime = fs_now();
    return ext2_iput(dir);
}

/* rename within the filesystem: files anywhere, directories only inside one parent */
int ext2_rename(struct inode *od, const char *on, struct inode *nd, const char *nn)
{
    u32 ol = strlen_(on), nl = strlen_(nn), ino, tmp;
    if (ext2_lookup(od, on, ol, &ino))
        return -1;
    struct inode in;
    if (ext2_iget(ino, &in))
        return -1;
    if (S_ISDIR(in.mode) && od->ino != nd->ino)
        return -3;                      /* moving directories between parents: not supported */
    if (ext2_lookup(nd, nn, nl, &tmp) == 0) {
        if (tmp == ino)
            return 0;
        if (ext2_unlink(nd, nn))
            return -2;                  /* target is a directory or busy */
        if (ext2_iget(nd->ino, nd))
            return -1;
    }
    if (dir_add(nd, nn, nl, ino))
        return -1;
    if (od->ino == nd->ino && ext2_iget(od->ino, od))
        return -1;
    if (dir_remove(od, on, ol, &tmp))
        return -1;
    return 0;
}

/* usage numbers from the superblock (kept current by the allocator) */
int ext2_statfs(u32 *bs, u32 *blocks, u32 *bfree, u32 *files, u32 *ffree)
{
    u8 sbk[24];
    u32 blk = bsize == 1024 ? 1 : 0, base = bsize == 1024 ? 0 : 1024;
    if (bread_copy(blk, base, sbk, 24))
        return -1;
    *bs = bsize;
    *blocks = rd32(sbk + 4);
    *bfree = rd32(sbk + 12);
    *files = rd32(sbk + 0);
    *ffree = rd32(sbk + 16);
    return 0;
}
