/* SPDX-License-Identifier: GPL-2.0-only */
/* the server is size-critical (it is embedded in the kernel image): optimise for size */
#pragma GCC optimize("Os")
#ifdef FSD
#include "../user/fsenv.h"      /* running as the filesystem server (user mode) */
#else
#include "kernel.h"
#endif
#include "errno.h"
#include "fsproto.h"            /* FS_PATHMAX */

/* ext2 (revision 0/1), written from the public format description. On-disk numbers are little-endian; every field
 * is swapped when read, file data never is.
 * What is mounted (ext2_mount_as, the rules are in docs/architecture.md "ext2: what is mounted"):
 *   - refused: a revision above 1, block size above 4 KB, a superblock or group table that does not add up, and
 *     every INCOMPAT feature except filetype (extents, 64bit, flex_bg, meta_bg, journal recovery, ...);
 *   - read only: unknown RO_COMPAT features (only sparse_super and large_file are known), an ext3 journal
 *     (has_journal: Zenix does not write journals), or a volume marked "has errors";
 *   - read and write otherwise. dir_index (htree) directories are read as plain lists, and a directory Zenix changes
 *     loses its index flag, as e2fsck would; the filetype byte is written when that feature is on; extended
 *     attribute blocks are released with their file.
 * Everything read from the disk is checked before use (block numbers against the volume, inode numbers, directory
 * entry lengths): a damaged disk fails the request with -5 (EIO) and logs "ext2: damaged: ...", it never crashes
 * the server or makes it write outside the volume. */

#ifdef ZHOST                            /* host test build (little-endian PC): disk order is CPU order */
static inline u32 le32(u32 v) { return v; }
static inline u16 le16(u16 v) { return v; }
#else
static inline u32 le32(u32 v)
{
    __asm__("rol.w #8,%0\n\tswap %0\n\trol.w #8,%0" : "+d"(v));
    return v;
}
static inline u16 le16(u16 v) { return (u16)((v >> 8) | (v << 8)); }
#endif

#define EIO_ (-5)
#define EROFS_ (-30)

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
static int last_slot;
static u8 hashslot[64];                 /* blk & 63 -> slot that probably holds it */

static u32 part_lba, bsize, ninodes_per_group, inode_size;
static u32 first_data_block, blocks_per_group, ngroups, boot_time;
static u32 sectors_per_block, total_blocks, ninodes;
static u32 gd_blk;                      /* first block of the group descriptor table */
static u32 rev, f_compat, f_incompat, f_rocompat, first_ino, extra_isize;
static int mounted;
int ext2_readonly;                      /* mounted read-only: every write is refused (EROFS) */
const char *ext2_why = "";              /* why the last mount was refused or made read-only */
int (*ext2_busy)(u32 ino);              /* set by the server: is this inode open? (its deletion then waits for the close) */

#define COMPAT_HAS_JOURNAL  0x0004
#define COMPAT_EXT_ATTR     0x0008
#define COMPAT_DIR_INDEX    0x0020
#define INCOMPAT_FILETYPE   0x0002
#define RO_SPARSE_SUPER     0x0001
#define RO_LARGE_FILE       0x0002
#define EXT2_INDEX_FL       0x1000      /* i_flags: an htree directory */
#define XATTR_MAGIC         0xEA020000u

/* a damaged disk: say what, at most 20 times (a badly damaged disk must not flood the screen) */
static u32 ndamage;
static int damaged(const char *what, u32 v)
{
    if (ndamage++ < 20)
        fs_log("ext2: damaged: %s %u\n", what, v);
    return EIO_;
}

static void cache_drop(void)            /* forget every cached block (a new device, maybe another block size) */
{
    for (int i = 0; i < NBUF; i++) {
        if (buf[i].data)
            fs_free(buf[i].data);
        buf[i].data = 0;
        buf[i].blk = 0xFFFFFFFF;
    }
    last_slot = 0;
}

/* returns the cache slot holding block blk (loading it), or -1.
 * Fast paths first: the last slot used, then a small hash of block
 * numbers; only a miss scans the whole cache for a victim. */

static int bfind(u32 blk)
{
    int victim = 0;
    u32 oldest = 0xFFFFFFFF;
    if (blk >= total_blocks) {          /* every block number from the disk ends here: none outside the volume */
        damaged("block number out of range:", blk);
        return -1;
    }
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
    buf[victim].blk = 0xFFFFFFFF;
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
        return -EIO;
    memcpy(dst, buf[i].data + off, len);
    return 0;
}

/* write-through: the cache and the disk are updated together */
static int bwrite_copy(u32 blk, u32 off, const void *src, u32 len)
{
    if (ext2_readonly)
        return EROFS_;
    int i = bfind(blk);
    if (i < 0)
        return -EIO;
    memcpy(buf[i].data + off, src, len);
    return fs_blk_write(part_lba + blk * sectors_per_block, sectors_per_block,
                     buf[i].data);
}

static u32 rd32(const u8 *p) { return le32(*(const u32 *)p); }
static u32 rd16(const u8 *p) { return le16(*(const u16 *)p); }

/* ---- the device is a partition (or a bare disk) opened through the disk
 * server: sector 0 of it is where the filesystem starts ---- */

int ext2_probe(void)
{
    u8 sec[512];
    if (fs_blk_read(2, 1, sec))         /* byte 1024: superblock */
        return 0;
    return le16(*(u16 *)(sec + 56)) == 0xEF53;
}

static void feature_names(char *out, u32 bits, const char *const *names)
{
    u32 n = 0;
    out[0] = 0;
    for (int i = 0; i < 32 && n < 90; i++) {
        if (!(bits & (1u << i)))
            continue;
        const char *s = names[i] ? names[i] : "?";
        if (n) out[n++] = ' ';
        while (*s && n < 98) out[n++] = *s++;
    }
    out[n] = 0;
}
static const char *const incompat_names[32] = { "compression", "filetype", "needs_recovery", "journal_dev", "meta_bg",
    0, "extent", "64bit", "mmp", "flex_bg", "ea_inode", 0, "dirdata", "metadata_csum_seed", "large_dir", "inline_data",
    "encrypt", "casefold" };
static const char *const rocompat_names[32] = { "sparse_super", "large_file", "btree_dir", "huge_file", "uninit_bg",
    "dir_nlink", "extra_isize", "has_snapshot", "quota", "bigalloc", "metadata_csum", "replica", "read-only", "project",
    "shared_blocks", "verity", "orphan_present" };
static char why_buf[160];

static int refuse(const char *why)
{
    ext2_why = why;
    fs_log("ext2: not mounted: %s\n", why);
    return -1;
}
static void readonly(const char *why)
{
    if (!ext2_readonly)
        ext2_why = why;
    ext2_readonly = 1;
}

/* mode EXT2_RW: the volume must be writable (the root, fsck), else it is refused; EXT2_ANY: read-only is allowed */
int ext2_mount_as(int mode)
{
    u8 sb[1024];
    part_lba = 0;
    mounted = 0;
    ext2_readonly = 0;
    ext2_why = "";
    ndamage = 0;
    cache_drop();
    total_blocks = 0;
    if (fs_blk_read(part_lba + 2, 2, sb))
        return refuse("cannot read the superblock");
    if (rd16(sb + 56) != 0xEF53)
        return refuse("no ext2 superblock");
    rev = rd32(sb + 76);
    u32 logbs = rd32(sb + 24);
    if (rev > 1)
        return refuse("unknown ext2 revision");
    if (logbs > 2)
        return refuse("block size above 4 KB");
    bsize = 1024u << logbs;
    sectors_per_block = bsize / 512;
    ninodes = rd32(sb + 0);
    total_blocks = rd32(sb + 4);
    first_data_block = rd32(sb + 20);
    blocks_per_group = rd32(sb + 32);
    ninodes_per_group = rd32(sb + 40);
    boot_time = rd32(sb + 48);          /* last write time: a clock of sorts */
    if (first_data_block != (bsize == 1024 ? 1u : 0u) || total_blocks <= first_data_block + 2 ||
        rd32(sb + 28) != logbs || blocks_per_group == 0 || blocks_per_group > 8 * bsize ||
        ninodes_per_group == 0 || ninodes_per_group > 8 * bsize) {
        total_blocks = 0;
        return refuse("damaged superblock (sizes)");
    }
    ngroups = (total_blocks - first_data_block + blocks_per_group - 1) / blocks_per_group;
    if (ninodes / ninodes_per_group != ngroups || ninodes % ninodes_per_group) {
        total_blocks = 0;
        return refuse("damaged superblock (inode count)");
    }
    inode_size = 128;
    first_ino = 11;
    f_compat = f_incompat = f_rocompat = 0;
    extra_isize = 0;
    if (rev == 1) {
        inode_size = rd16(sb + 88);
        first_ino = rd32(sb + 84);
        f_compat = rd32(sb + 92);
        f_incompat = rd32(sb + 96);
        f_rocompat = rd32(sb + 100);
        extra_isize = rd16(sb + 350);                   /* s_want_extra_isize */
        if (inode_size < 128 || inode_size > bsize || (inode_size & (inode_size - 1)) ||
            first_ino < 11 || first_ino > ninodes || extra_isize > inode_size - 128 || (extra_isize & 3)) {
            total_blocks = 0;
            return refuse("damaged superblock (inode size or first inode)");
        }
    }
    if (f_incompat & ~(u32)INCOMPAT_FILETYPE) {
        char names[100];
        feature_names(names, f_incompat & ~(u32)INCOMPAT_FILETYPE, incompat_names);
        snprintf(why_buf, sizeof why_buf, "features Zenix cannot read: %s (ext3/ext4?)", names);
        total_blocks = 0;
        return refuse(why_buf);
    }
    /* the group table and every group's bitmaps and inode table must lie inside their group */
    gd_blk = first_data_block + 1;
    u32 gd_blocks = (ngroups * 32 + bsize - 1) / bsize;
    u32 itb = (ninodes_per_group * inode_size + bsize - 1) / bsize;
    if (gd_blk + gd_blocks > total_blocks) {
        total_blocks = 0;
        return refuse("damaged superblock (group table)");
    }
    for (u32 g = 0; g < ngroups; g++) {
        u8 d[12];
        u32 o = g * 32;
        if (bread_copy(gd_blk + o / bsize, o % bsize, d, 12)) {
            total_blocks = 0;
            return refuse("cannot read the group table");
        }
        u32 gs = first_data_block + g * blocks_per_group;
        u32 ge = total_blocks - gs < blocks_per_group ? total_blocks : gs + blocks_per_group;
        u32 bb = rd32(d), ib = rd32(d + 4), it = rd32(d + 8);
        if (bb < gs || bb >= ge || ib < gs || ib >= ge || it < gs || it >= ge || itb > ge - it) {
            snprintf(why_buf, sizeof why_buf, "damaged group table (group %u)", g);
            total_blocks = 0;
            return refuse(why_buf);
        }
    }
    if (f_rocompat & ~(u32)(RO_SPARSE_SUPER | RO_LARGE_FILE)) {
        char names[100];
        feature_names(names, f_rocompat & ~(u32)(RO_SPARSE_SUPER | RO_LARGE_FILE), rocompat_names);
        snprintf(why_buf, sizeof why_buf, "read-only: features Zenix cannot write: %s", names);
        readonly(why_buf);
    }
    if (f_compat & COMPAT_HAS_JOURNAL)
        readonly("read-only: ext3 journal (Zenix does not write journals)");
    if (rd16(sb + 58) & 2)
        readonly("read-only: the filesystem has errors (run fsck.ext2 on it)");
    if (ext2_readonly && mode == EXT2_RW) {
        ext2_readonly = 0;
        total_blocks = 0;
        return refuse(ext2_why + 11);                   /* without the "read-only: " */
    }
    mounted = 1;
    fs_log("ext2: %u KB blocks, %u blocks, %u inodes%s\n",
            bsize / 1024, total_blocks, ninodes, ext2_readonly ? ", read-only" : "");
    if (ext2_readonly)
        fs_log("ext2: %s\n", ext2_why);
    return 0;
}

int ext2_mount(void)
{
    return ext2_mount_as(EXT2_RW);
}

/* Errors are the negative errno numbers of kernel/errno.h (audit E): -ENOENT, -EEXIST, -ENOTDIR, -EISDIR, -ENOTEMPTY,
 * -ENOSPC, -EIO (a damaged volume or the disk), -EROFS ... */
int ext2_iget(u32 ino, struct inode *out)
{
    if (!mounted || ino == 0 || ino > ninodes)
        return -EIO;
    u32 grp = (ino - 1) / ninodes_per_group;
    u32 idx = (ino - 1) % ninodes_per_group;
    u32 gd[2];
    u32 gdoff = grp * 32;
    if (bread_copy(gd_blk + gdoff / bsize, gdoff % bsize + 8, gd, 4))
        return -EIO;
    u32 itable = le32(gd[0]);
    u32 pos = idx * inode_size;
    u8 raw[128];
    if (bread_copy(itable + pos / bsize, pos % bsize, raw, 128))
        return -EIO;
    out->ino = ino;
    out->mode = le16(*(u16 *)(raw + 0));
    out->uid = le16(*(u16 *)(raw + 2));
    out->size = rd32(raw + 4);
    out->mtime = rd32(raw + 16);
    out->gid = le16(*(u16 *)(raw + 24));
    out->links = le16(*(u16 *)(raw + 26));
    out->nblocks = rd32(raw + 28);
    out->flags = rd32(raw + 32);
    out->acl = rd32(raw + 104);
    out->size_high = S_ISREG(out->mode) ? rd32(raw + 108) : 0;
    for (int i = 0; i < 15; i++)
        out->block[i] = rd32(raw + 40 + i * 4);
    return 0;
}

/* is this a fast symbolic link (the target text in the inode's block array, no data block)? As Linux decides it:
 * a symlink whose block count, without its extended-attribute block, is 0. */
static int fast_symlink(const struct inode *in)
{
    u32 ea = in->acl ? bsize / 512 : 0;
    return S_ISLNK(in->mode) && in->nblocks <= ea && in->size < 60;
}
/* does block[] hold block numbers? Not for fast symbolic links (text) or device inodes (device numbers). */
static int has_block_map(const struct inode *in)
{
    return S_ISREG(in->mode) || S_ISDIR(in->mode) || (S_ISLNK(in->mode) && !fast_symlink(in));
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
    if (!has_block_map(in))
        return 0;
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
                return EIO_;
        } else {
            memset(d + done, 0, chunk);     /* hole */
        }
        done += chunk;
    }
    return (int)n;
}

/* is the directory entry at p (rl bytes left in the block from it) well formed? */
static int dirent_bad(const u8 *e, u32 left)
{
    u32 rl = rd16(e + 4), nl = e[6];
    return left < 8 || rl < 8 || (rl & 3) || rl > left || nl + 8 > rl || rd32(e) > ninodes;
}

/* next live directory entry at/after byte offset *cookie, parsed in place in
 * the cache (one block lookup per call, not per field); returns 1, 0 at the
 * end, -5 on a damaged directory */
int ext2_readdir(const struct inode *dir, u32 *cookie, struct udirent *out)
{
    u32 off = *cookie;
    if (off & 3)
        return EIO_;
    while (off < dir->size) {
        u32 blk = ext2_bmap(dir, off / bsize);
        const u8 *p = blk ? bptr(blk) : 0;
        if (!p)
            return damaged("directory block missing in inode", dir->ino);
        u32 end = bsize - off % bsize;          /* bytes left in this block */
        const u8 *e = p + off % bsize;
        if (dirent_bad(e, end))
            return damaged("directory entry in inode", dir->ino);
        u32 rl = rd16(e + 4);
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
        out->type = (f_incompat & INCOMPAT_FILETYPE) ? e[7] : 0;
        *cookie = off;
        return 1;
    }
    *cookie = off;
    return 0;
}

int ext2_lookup(const struct inode *dir, const char *name, u32 len, u32 *ino)
{
    if (!S_ISDIR(dir->mode))
        return -ENOTDIR;
    for (u32 b = 0; b < dir->size / bsize; b++) {
        u32 blk = ext2_bmap(dir, b);
        const u8 *p = blk ? bptr(blk) : 0;
        if (!p)
            return damaged("directory block missing in inode", dir->ino);
        for (u32 o = 0; o + 8 <= bsize;) {
            if (dirent_bad(p + o, bsize - o))
                return damaged("directory entry in inode", dir->ino);
            u32 rl = rd16(p + o + 4);
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
    return -ENOENT;
}

/* the bytes of a fast symbolic link (up to 59 characters stored in the inode's block array) */
static void fast_link_bytes(const struct inode *in, char *out, u32 n)
{
    for (u32 i = 0; i < n; i++)
        out[i] = (char)(in->block[i / 4] >> (8 * (i % 4)));    /* the on-disk bytes are little-endian words */
}

/* the target of a symbolic link inode into buf (max bytes, no NUL); returns its length */
int ext2_readlink(const struct inode *in, char *buf, u32 max)
{
    u32 n = in->size < max ? in->size : max;
    if (fast_symlink(in))
        fast_link_bytes(in, buf, n);
    else if (ext2_read(in, 0, buf, n) != (int)n)
        return EIO_;
    return (int)n;
}

/* resolve a path; cwd is the starting inode; follow_last: a final symbolic link is followed */
/* Who is asking. The filesystem server sets these for each request and turns enforcement on; mkfs and fsck leave
 * it off. Root passes every check except "execute" on a file that has no x bit at all. */
u32 ext2_uid, ext2_gid, ext2_umask = 022;
int ext2_enforce, ext2_denied;          /* ext2_denied: the last failure was a permission failure */

int ext2_access(const struct inode *in, int want)       /* want: FSP_R, FSP_W, FSP_X; the rules are in fsperm.h */
{
    if (!ext2_enforce)
        return 1;
    int ok = fsp_access(ext2_uid, ext2_gid, in->mode, in->uid, in->gid, want);
    if (!ok)
        ext2_denied = 1;
    return ok;
}

int ext2_namei_ex(const char *path, u32 cwd, struct inode *out, int depth, int follow_last)
{
    u32 cur = (path[0] == '/') ? 2 : cwd;
    struct inode in;
    int r;
    if (depth > 8)
        return -ELOOP;
    if ((r = ext2_iget(cur, &in)))
        return r;
    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        const char *e = path;
        while (*e && *e != '/')
            e++;
        u32 next;
        if (!S_ISDIR(in.mode))
            return -ENOTDIR;
        if (!ext2_access(&in, 1))               /* searching a directory needs x */
            return -EACCES;
        if (e - path > 255)
            return -ENAMETOOLONG;
        if ((r = ext2_lookup(&in, path, (u32)(e - path), &next)))
            return r;
        struct inode nin;
        if ((r = ext2_iget(next, &nin)))
            return r;
        const char *rest = e;
        while (*rest == '/')
            rest++;
        if (S_ISLNK(nin.mode) && (*rest || follow_last)) {
            char target[256];
            int n = ext2_readlink(&nin, target, 255);
            if (n < 0)
                return n;
            target[n] = 0;
            struct inode t;
            if ((r = ext2_namei_ex(target, in.ino, &t, depth + 1, 1)))
                return r;
            nin = t;
        }
        in = nin;
        path = e;
        if (*rest && !S_ISDIR(in.mode))
            return -ENOTDIR;
    }
    *out = in;
    return 0;
}

int ext2_namei(const char *path, u32 cwd, struct inode *out, int depth)
{
    return ext2_namei_ex(path, cwd, out, depth, 1);
}

/* ================= writing ================= */

static inline void wr32(u8 *p, u32 v) { *(u32 *)p = le32(v); }
static inline void wr16(u8 *p, u16 v) { *(u16 *)p = le16(v); }

static u32 fs_now(void)
{
    return fs_time();
}

/* group descriptor field helpers: field at byte `f` of descriptor g */
static int gd_get(u32 g, u32 f, u32 size, u32 *v)
{
    if (g >= ngroups)
        return -1;
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

/* clear the superblock's "cleanly unmounted" state (s_state bit 0, not bit 1 "has errors", which makes a read-only
 * mount): Linux and e2fsck then check the volume. A restarted fsd does it when its predecessor ended in the middle of a
 * request (DESIGN D11 5.4: ext2 has no journal, the change may be half done). */
int ext2_mark_unclean(void)                     /* 0, -EROFS, or -EIO (audit A14: was a bare -1) */
{
    int r = ext2_set_clean(0);
    return r < 0 ? r : 0;
}

/* the clean bit itself (FS_MARK, DESIGN D15: zup marks the root while it installs): the bit before (1 clean, 0 not),
 * or -EROFS / -EIO */
int ext2_set_clean(int clean)
{
    u32 blk = bsize == 1024 ? 1 : 0, base = bsize == 1024 ? 0 : 1024;
    u8 b[2];
    if (ext2_readonly)
        return -EROFS;
    if (bread_copy(blk, base + 58, b, 2))
        return -EIO;
    u16 st = rd16(b);
    wr16(b, (u16)(clean ? st | 1u : st & ~1u));
    return bwrite_copy(blk, base + 58, b, 2) ? -EIO : (int)(st & 1u);
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

/* clear a bit: 0 done, 1 it was clear already (the disk is damaged: nothing is changed), -1 error */
static int bitmap_free(u32 bm_blk, u32 bit)
{
    u8 b;
    if (bit >= 8 * bsize || bread_copy(bm_blk, bit / 8, &b, 1))
        return -1;
    if (!(b & (1u << (bit % 8))))
        return 1;
    b &= (u8)~(1u << (bit % 8));
    return bwrite_copy(bm_blk, bit / 8, &b, 1) ? -1 : 0;
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

/* give a block back. Never trusts the number: outside the data area, or already free, it is reported and left alone */
static void free_block(u32 blk)
{
    if (blk < first_data_block || blk >= total_blocks) {
        damaged("freeing a block number out of range:", blk);
        return;
    }
    u32 g = (blk - first_data_block) / blocks_per_group, bit = (blk - first_data_block) % blocks_per_group, bmb;
    if (gd_get(g, 0, 4, &bmb))
        return;
    int r = bitmap_free(bmb, bit);
    if (r == 1)
        damaged("freeing a block that is already free:", blk);
    if (r)
        return;
    gd_add(g, 12, 1);
    sb_add(12, 1);
}

static u32 alloc_inode(int is_dir)
{
    for (u32 g = 0; g < ngroups; g++) {
        u32 bmb;
        if (gd_get(g, 4, 4, &bmb))
            return 0;
        u32 skip = 0;                                   /* the reserved inodes 1 .. first_ino-1 */
        if (g * ninodes_per_group < first_ino - 1)
            skip = first_ino - 1 - g * ninodes_per_group;
        if (skip >= ninodes_per_group)
            continue;
        int bit = bitmap_alloc(bmb, ninodes_per_group, skip);
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
    if (ino < first_ino || ino > ninodes) {
        damaged("freeing an inode number out of range:", ino);
        return;
    }
    u32 g = (ino - 1) / ninodes_per_group, bmb;
    if (gd_get(g, 4, 4, &bmb))
        return;
    int r = bitmap_free(bmb, (ino - 1) % ninodes_per_group);
    if (r == 1)
        damaged("freeing an inode that is already free:", ino);
    if (r)
        return;
    gd_add(g, 14, 1);
    if (is_dir)
        gd_add(g, 16, -1);
    sb_add(16, 1);
}

/* write the inode's fields back (other on-disk fields are kept) */
int ext2_iput(const struct inode *in)
{
    if (in->ino == 0 || in->ino > ninodes)
        return -EIO;
    u32 grp = (in->ino - 1) / ninodes_per_group, idx = (in->ino - 1) % ninodes_per_group;
    u32 itable;
    if (gd_get(grp, 8, 4, &itable))
        return -EIO;
    u32 pos = idx * inode_size, blk = itable + pos / bsize, off = pos % bsize;
    u8 raw[128];
    if (bread_copy(blk, off, raw, 128))
        return -EIO;
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
    wr32(raw + 32, S_ISDIR(in->mode) ? in->flags & ~(u32)EXT2_INDEX_FL : in->flags);  /* a changed directory is a plain list */
    for (int i = 0; i < 15; i++)
        wr32(raw + 40 + i * 4, in->block[i]);
    wr32(raw + 104, in->acl);
    if (S_ISREG(in->mode))
        wr32(raw + 108, in->size_high);
    return bwrite_copy(blk, off, raw, 128);
}

/* a directory Zenix changes is no longer indexed: drop the htree flag (the index blocks read as empty entries) */
static int dir_unindex(struct inode *dir)
{
    if (!(dir->flags & EXT2_INDEX_FL))
        return 0;
    dir->flags &= ~(u32)EXT2_INDEX_FL;
    return ext2_iput(dir);
}

/* block number for file block n, allocating it (and indirect blocks) if missing */
static u32 bmap_alloc(struct inode *in, u32 n)
{
    u32 per = bsize / 4, spb = bsize / 512;
    u32 goal = (in->ino - 1) / ninodes_per_group;
    if (!has_block_map(in))
        return 0;
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
    if (ext2_readonly)
        return EROFS_;
    if (in->size_high)
        return -27;                     /* EFBIG: a file over 4 GB (large_file) is only read */
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
        return -EIO;
    return done || !n ? (int)done : -ENOSPC;    /* nothing written: no free block (or the file's map is full) */
}

/* free a block tree; *budget limits the blocks a (damaged) inode can make us visit */
static void free_tree(u32 blk, int level, u32 *budget)
{
    if (!blk || !*budget)
        return;
    if (blk < first_data_block || blk >= total_blocks) {
        damaged("block number out of range in a file:", blk);
        return;
    }
    (*budget)--;
    if (level > 0) {
        u32 per = bsize / 4;
        for (u32 i = 0; i < per && *budget; i++) {
            u8 b[4];
            if (bread_copy(blk, i * 4, b, 4))
                break;
            u32 e = rd32(b);
            if (e)
                free_tree(e, level - 1, budget);
        }
    }
    free_block(blk);
}

/* The blocks of an inode are given back in two steps, in the order a power cut can survive (audit 2026-10-26 F5):
 * first the inode is written without them (forget_data and release_acl, then ext2_iput), then the blocks
 * are freed in the bitmaps from a copy of the old map (free_data, drop_acl). A cut in between leaves blocks marked in
 * use that no inode holds (a leak fsck gives back), never a block marked free that an inode still points at (the next
 * file would get it too). */

/* free the data blocks of a regular file, directory or slow symbolic link (never block[] of a fast symlink or a
 * device: that is text / device numbers), as OLD (a copy of the inode before forget_data) holds them */
static void free_data(const struct inode *old)
{
    if (has_block_map(old)) {
        u32 budget = old->nblocks / (bsize / 512) + 1;  /* no more blocks than the inode says it has */
        for (int i = 0; i < 12; i++)
            free_tree(old->block[i], 0, &budget);
        free_tree(old->block[12], 1, &budget);
        free_tree(old->block[13], 2, &budget);
        free_tree(old->block[14], 3, &budget);
    }
}

/* the inode without its data blocks (in memory; the caller writes it). The extended-attribute block, if any, stays:
 * see release_acl */
static void forget_data(struct inode *in)
{
    if (has_block_map(in)) {
        for (int i = 0; i < 15; i++)
            in->block[i] = 0;
        in->nblocks = in->acl ? bsize / 512 : 0;
        in->size = 0;
        in->size_high = 0;
    }
}

/* the inode without its extended-attribute block (in memory); the block itself goes in drop_acl */
static u32 release_acl(struct inode *in)
{
    u32 b = in->acl;
    if (!b)
        return 0;
    in->acl = 0;
    in->nblocks = in->nblocks >= bsize / 512 ? in->nblocks - bsize / 512 : 0;
    return b;
}

/* one reference less on an extended-attribute block, freed with the last one */
static void drop_acl(u32 b)
{
    if (!b)
        return;
    u8 h[8];
    if (b < first_data_block || b >= total_blocks || bread_copy(b, 0, h, 8) || rd32(h) != XATTR_MAGIC) {
        damaged("extended attribute block", b);
        return;
    }
    u32 refs = rd32(h + 4);
    if (refs > 1) {
        wr32(h + 4, refs - 1);
        bwrite_copy(b, 4, h + 4, 4);
    } else {
        free_block(b);
    }
}

int ext2_truncate(struct inode *in)
{
    if (ext2_readonly)
        return EROFS_;
    if (!S_ISREG(in->mode))
        return -EISDIR;
    struct inode old = *in;
    forget_data(in);
    in->mtime = fs_now();
    int r = ext2_iput(in);
    if (r)
        return r;                       /* the inode still holds its blocks: none is freed */
    free_data(&old);
    return 0;
}

/* an inode with no names left: give its blocks and number back (the inode is written empty first, see above; its
 * number is freed last, so a cut leaves at worst a leaked inode and leaked blocks) */
static void release(struct inode *in)
{
    int is_dir = S_ISDIR(in->mode);
    struct inode old = *in;
    forget_data(in);
    u32 acl = release_acl(in);
    in->links = 0;
    in->mtime = fs_now();
    if (ext2_iput(in))
        return;                         /* not written: everything stays allocated (a leak, never a block in two files) */
    free_data(&old);
    drop_acl(acl);
    free_inode(in->ino, is_dir);
}

/* the last close of an inode whose last name went while it was open: delete it now. Only an inode with no links */
int ext2_release(u32 ino)
{
    struct inode in;
    if (ext2_readonly)
        return EROFS_;
    if (ext2_iget(ino, &in) || in.links != 0 || in.mode == 0)
        return -EINVAL;
    release(&in);
    return 0;
}

/* the last name of an inode went: delete it now, or (open) at the last close */
static void unlinked(struct inode *in)
{
    if (ext2_busy && ext2_busy(in->ino)) {
        in->links = 0;
        in->mtime = fs_now();
        ext2_iput(in);                  /* kept, with its blocks, until ext2_release */
        return;
    }
    release(in);
}

/* ---- directories ---- */

static u8 dbuf[4096];

static u8 file_type(u32 mode)            /* the directory entry's file type byte (filetype feature) */
{
    if (!(f_incompat & INCOMPAT_FILETYPE))
        return 0;                       /* revision 0: that byte is the high byte of the name length */
    switch (mode & S_IFMT) {
    case S_IFREG: return 1;
    case S_IFDIR: return 2;
    case 0020000: return 3;
    case 0060000: return 4;
    case 0010000: return 5;
    case 0140000: return 6;
    case S_IFLNK: return 7;
    }
    return 0;
}

static int dir_add(struct inode *dir, const char *name, u32 len, u32 ino, u32 mode)
{
    u32 need = 8 + ((len + 3) & ~3u);
    u32 nblk = dir->size / bsize;
    if (len == 0)
        return -EINVAL;
    if (len > 255)
        return -ENAMETOOLONG;
    for (u32 i = 0; i < nblk; i++) {
        u32 blk = ext2_bmap(dir, i);
        if (!blk || bread_copy(blk, 0, dbuf, bsize))
            return damaged("directory block missing in inode", dir->ino);
        for (u32 o = 0; o + 8 <= bsize;) {
            if (dirent_bad(dbuf + o, bsize - o))
                return damaged("directory entry in inode", dir->ino);
            u32 eino = rd32(dbuf + o), rl = rd16(dbuf + o + 4);
            u32 elen = eino ? 8 + (((u32)dbuf[o + 6] + 3) & ~3u) : 0;
            if (rl >= elen && rl - elen >= need) {
                u32 at = o + elen;
                if (elen) {                     /* split the live entry */
                    wr16(dbuf + o + 4, (u16)elen);
                    wr16(dbuf + at + 4, (u16)(rl - elen));
                } else {
                    wr16(dbuf + at + 4, (u16)rl);
                }
                wr32(dbuf + at, ino);
                dbuf[at + 6] = (u8)len;
                dbuf[at + 7] = file_type(mode);
                memcpy(dbuf + at + 8, name, len);
                if (bwrite_copy(blk, 0, dbuf, bsize))
                    return -EIO;
                return dir_unindex(dir);
            }
            o += rl;
        }
    }
    u32 blk = bmap_alloc(dir, nblk);            /* no room: a new directory block */
    if (!blk)
        return -ENOSPC;
    memset(dbuf, 0, bsize);
    wr32(dbuf, ino);
    wr16(dbuf + 4, (u16)bsize);
    dbuf[6] = (u8)len;
    dbuf[7] = file_type(mode);
    memcpy(dbuf + 8, name, len);
    if (bwrite_copy(blk, 0, dbuf, bsize))
        return -EIO;
    dir->size += bsize;
    dir->mtime = fs_now();
    dir->flags &= ~(u32)EXT2_INDEX_FL;
    return ext2_iput(dir);
}

/* public: add an entry to a directory (used by fsck to attach orphans) */
int ext2_link(struct inode *dir, const char *name, u32 ino)
{
    struct inode in;
    int r = ext2_iget(ino, &in);
    if (r)
        return r;
    return dir_add(dir, name, strlen_(name), ino, in.mode);
}

/* remove the entry NAME (newino 0), or point it at newino with mode's file-type byte instead: one block write either
 * way. *ino_out gets the inode number it had */
static int dir_change(struct inode *dir, const char *name, u32 len, u32 *ino_out, u32 newino, u32 mode)
{
    u32 nblk = dir->size / bsize;
    for (u32 i = 0; i < nblk; i++) {
        u32 blk = ext2_bmap(dir, i);
        if (!blk || bread_copy(blk, 0, dbuf, bsize))
            return damaged("directory block missing in inode", dir->ino);
        u32 prev = 0xFFFFFFFF;
        for (u32 o = 0; o + 8 <= bsize;) {
            if (dirent_bad(dbuf + o, bsize - o))
                return damaged("directory entry in inode", dir->ino);
            u32 eino = rd32(dbuf + o), rl = rd16(dbuf + o + 4), nl = dbuf[o + 6];
            if (eino && nl == len && !memcmp_(dbuf + o + 8, name, len)) {
                *ino_out = eino;
                if (newino) {
                    wr32(dbuf + o, newino);
                    dbuf[o + 7] = file_type(mode);
                } else if (prev != 0xFFFFFFFF)
                    wr16(dbuf + prev + 4, (u16)(rd16(dbuf + prev + 4) + rl));
                else
                    wr32(dbuf + o, 0);
                if (bwrite_copy(blk, 0, dbuf, bsize))
                    return -EIO;
                return dir_unindex(dir);
            }
            prev = o;
            o += rl;
        }
    }
    return -ENOENT;
}

static int dir_remove(struct inode *dir, const char *name, u32 len, u32 *ino_out)
{
    return dir_change(dir, name, len, ino_out, 0, 0);
}

static int dir_is_empty(const struct inode *dir)
{
    u32 cookie = 0;
    struct udirent e;
    int r;
    while ((r = ext2_readdir(dir, &cookie, &e)) > 0)
        if (strcmp_(e.name, ".") && strcmp_(e.name, ".."))
            return 0;
    return r == 0;                      /* a damaged directory is not "empty" */
}

/* split a path into parent directory and last component */
int ext2_namei_parent(const char *path, u32 cwd, struct inode *dir, char *name)
{
    char p[FS_PATHMAX];                 /* the one path limit (fsproto.h): longer is refused, never cut */
    u32 n = 0;
    while (path[n]) {
        if (n == sizeof p - 1)
            return -ENAMETOOLONG;
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
    if (len > 63)
        return -ENAMETOOLONG;
    if (len == 0 || (len == 1 && p[slash] == '.') ||
        (len == 2 && p[slash] == '.' && p[slash + 1] == '.'))
        return -EINVAL;                 /* "", "." or ".." as the last part: not something to create or remove */
    memcpy(name, p + slash, len);
    name[len] = 0;
    int r;
    if (slash == 0) {
        if ((r = ext2_iget(cwd, dir)))
            return r;
    } else {
        p[slash] = 0;
        if (slash > 1 && p[slash - 1] == '/')
            p[slash - 1] = 0;
        if ((r = ext2_namei(p[0] ? p : "/", cwd, dir, 0)))
            return r;
    }
    return S_ISDIR(dir->mode) ? 0 : -ENOTDIR;
}

/* a new inode: the whole on-disk inode is cleared first (a reused inode keeps nothing of its old life:
 * flags, extended-attribute block, size_high), then the fields are filled in */
static int new_inode(struct inode *out, u32 mode, int is_dir)
{
    if (ext2_readonly)
        return EROFS_;
    u32 ino = alloc_inode(is_dir);
    if (!ino)
        return -ENOSPC;                 /* no free inode */
    u32 grp = (ino - 1) / ninodes_per_group, idx = (ino - 1) % ninodes_per_group, itable;
    static u8 zero[4096];
    if (gd_get(grp, 8, 4, &itable)) {
        free_inode(ino, is_dir);
        return -EIO;
    }
    u32 pos = idx * inode_size;
    memset(zero, 0, inode_size);
    if (inode_size > 128)
        wr16(zero + 128, (u16)extra_isize);     /* i_extra_isize as the superblock wants it */
    if (bwrite_copy(itable + pos / bsize, pos % bsize, zero, inode_size)) {
        free_inode(ino, is_dir);
        return -EIO;
    }
    memset(out, 0, sizeof *out);
    out->ino = ino;
    out->mode = (u16)mode;
    out->links = is_dir ? 2 : 1;
    out->mtime = fs_now();
    out->uid = (u16)ext2_uid;                   /* the creator owns it (0 outside the server) */
    out->gid = (u16)ext2_gid;
    return 0;
}

/* a new entry in a set-group-id directory gets the directory's group, a new directory also the bit (fsperm.h rule 7) */
static void inherit_group(struct inode *in, const struct inode *dir)
{
    in->gid = (u16)fsp_new_gid(ext2_gid, dir->mode, dir->gid);
    if (S_ISDIR(in->mode))
        in->mode = (u16)(in->mode | fsp_new_dir_bits(dir->mode));
}

/* create an empty regular file in dir */
int ext2_create(struct inode *dir, const char *name, struct inode *out)
{
    int r = new_inode(out, S_IFREG | fsp_new_file_mode(ext2_umask), 0);
    if (r)
        return r;
    inherit_group(out, dir);
    if ((r = ext2_iput(out)) || (r = dir_add(dir, name, strlen_(name), out->ino, out->mode))) {
        out->links = 0;
        ext2_iput(out);
        free_inode(out->ino, 0);
        return r < 0 ? r : -EIO;
    }
    return 0;
}

int ext2_mkdir(struct inode *dir, const char *name)
{
    struct inode d;
    u32 len = strlen_(name);
    u32 tmp;
    if (ext2_lookup(dir, name, len, &tmp) == 0)
        return -EEXIST;
    if (dir->links >= 65000)
        return -EMLINK;
    int r = new_inode(&d, S_IFDIR | fsp_new_dir_mode(ext2_umask), 1);
    if (r)
        return r;
    inherit_group(&d, dir);
    u32 blk = bmap_alloc(&d, 0);
    if (!blk) {
        d.links = 0;
        ext2_iput(&d);
        free_inode(d.ino, 1);
        return -ENOSPC;
    }
    d.size = bsize;
    memset(dbuf, 0, bsize);
    wr32(dbuf, d.ino);
    wr16(dbuf + 4, 12);
    dbuf[6] = 1;
    dbuf[7] = file_type(S_IFDIR);
    dbuf[8] = '.';
    wr32(dbuf + 12, dir->ino);
    wr16(dbuf + 16, (u16)(bsize - 12));
    dbuf[18] = 2;
    dbuf[19] = file_type(S_IFDIR);
    dbuf[20] = '.';
    dbuf[21] = '.';
    if (bwrite_copy(blk, 0, dbuf, bsize) || ext2_iput(&d))
        return -EIO;
    /* the parent's count first (the new ".." counts there), then the entry: a cut in between leaves the parent's
     * count one too high and an unnamed directory (leaks fsck finds), never a count below the names (F5) */
    dir->links++;
    dir->mtime = fs_now();
    if ((r = ext2_iput(dir)))
        return r;
    if ((r = dir_add(dir, name, len, d.ino, d.mode))) {
        if (ext2_iget(dir->ino, dir) == 0) {    /* dir_add may have rewritten it (index flag, a new block) */
            dir->links--;
            ext2_iput(dir);
        }
        release(&d);                            /* the new directory has no name: its block and number go back */
        return r;
    }
    return 0;
}

int ext2_unlink(struct inode *dir, const char *name)
{
    u32 len = strlen_(name), ino;
    if (ext2_readonly)
        return EROFS_;
    int r;
    if ((r = ext2_lookup(dir, name, len, &ino)))
        return r;
    struct inode in;
    if ((r = ext2_iget(ino, &in)))
        return r;
    if (S_ISDIR(in.mode))
        return -EISDIR;
    if ((r = dir_remove(dir, name, len, &ino)))
        return r;
    if (in.links > 1) {
        in.links--;
        return ext2_iput(&in);
    }
    unlinked(&in);
    return 0;
}

int ext2_rmdir(struct inode *dir, const char *name)
{
    u32 len = strlen_(name), ino;
    if (ext2_readonly)
        return EROFS_;
    int r;
    if ((r = ext2_lookup(dir, name, len, &ino)))
        return r;
    struct inode in;
    if ((r = ext2_iget(ino, &in)))
        return r;
    if (!S_ISDIR(in.mode))
        return -ENOTDIR;
    if (ino == 2)
        return -EBUSY;                  /* the root */
    if (!dir_is_empty(&in))
        return -ENOTEMPTY;
    if (in.links > 2)                   /* an unnamed subdirectory (a power cut in its mkdir or rmdir) still has its ".."
                                         * here: removing this one would leave that ".." naming a free inode (F5) */
        return -ENOTEMPTY;
    if ((r = dir_remove(dir, name, len, &ino)))
        return r;
    if ((r = ext2_iget(dir->ino, dir)))       /* dir_remove may have rewritten it (index flag) */
        return r;
    unlinked(&in);
    if (dir->links > 2)
        dir->links--;
    dir->mtime = fs_now();
    return ext2_iput(dir);
}

int ext2_chmod(u32 ino, u32 mode)
{
    struct inode in;
    int r = ext2_iget(ino, &in);
    if (r)
        return r;
    in.mode = (u16)((in.mode & S_IFMT) | (mode & 07777));
    return ext2_iput(&in);
}

int ext2_chown(u32 ino, u32 uid, u32 gid)
{
    struct inode in;
    int r = ext2_iget(ino, &in);
    if (r)
        return r;
    if (uid != 0xFFFFFFFFu) in.uid = (u16)uid;
    if (gid != 0xFFFFFFFFu) in.gid = (u16)gid;
    in.mode &= (u16)~06000;                     /* changing the owner clears set-user-id / set-group-id */
    return ext2_iput(&in);
}

int ext2_utimes(u32 ino, u32 t)
{
    struct inode in;
    int r = ext2_iget(ino, &in);
    if (r)
        return r;
    in.mtime = t;
    return ext2_iput(&in);
}

/* a hard link: another name for an existing (non-directory) inode */
int ext2_hardlink(struct inode *dir, const char *name, u32 ino)
{
    struct inode in;
    u32 tmp;
    if (ext2_lookup(dir, name, strlen_(name), &tmp) == 0)
        return -EEXIST;
    int r = ext2_iget(ino, &in);
    if (r)
        return r;
    if (S_ISDIR(in.mode))
        return -EPERM;                  /* no hard links to directories */
    if (in.links == 0)
        return -ENOENT;
    if (in.links >= 65000)
        return -EMLINK;
    in.links++;
    if (ext2_iput(&in))
        return -EIO;
    if ((r = dir_add(dir, name, strlen_(name), ino, in.mode))) {
        in.links--;
        ext2_iput(&in);
        return r;
    }
    return 0;
}

int ext2_symlink(struct inode *dir, const char *name, const char *target)
{
    struct inode in;
    u32 tmp, len = strlen_(target);
    if (ext2_lookup(dir, name, strlen_(name), &tmp) == 0)
        return -EEXIST;
    if (len == 0)
        return -ENOENT;                 /* as Linux: an empty target */
    if (len > bsize - 1)
        return -ENAMETOOLONG;
    int r = new_inode(&in, S_IFLNK | 0777, 0);
    if (r)
        return r;
    inherit_group(&in, dir);
    in.size = len;
    if (len < 60) {                                    /* fast symlink: the text lives in the inode */
        for (u32 i = 0; i < len; i++)
            in.block[i / 4] |= (u32)(u8)target[i] << (8 * (i % 4));
    } else {
        u32 blk = bmap_alloc(&in, 0);
        if (!blk || bwrite_copy(blk, 0, target, len)) {
            release(&in);
            return blk ? -EIO : -ENOSPC;
        }
    }
    if ((r = ext2_iput(&in)) || (r = dir_add(dir, name, strlen_(name), in.ino, in.mode))) {
        release(&in);
        return r < 0 ? r : -EIO;
    }
    return 0;
}

/* Crash-safe in the Linux order, every step one write-through block write, so a power cut between any two leaves at
 * worst a link count one too high or an old target with no name (a leaked inode e2fsck reattaches), never a missing
 * name or two names on a count of 1: raise the moving inode's count; point the existing target entry at it in place
 * (inode number and file-type byte) or add the new entry; remove the old entry; lower the count; then drop the old
 * target's inode. Files move anywhere in the filesystem; directories only inside one parent, so no ".." and no parent
 * count changes */
int ext2_rename(struct inode *od, const char *on, struct inode *nd, const char *nn)
{
    u32 ol = strlen_(on), nl = strlen_(nn), ino, old = 0, tmp;
    if (ext2_readonly)
        return EROFS_;
    int r;
    if ((r = ext2_lookup(od, on, ol, &ino)))
        return r;
    struct inode in, tg;
    if ((r = ext2_iget(ino, &in)))
        return r;
    if (S_ISDIR(in.mode) && od->ino != nd->ino)
        return -EXDEV;                  /* moving directories between parents: not supported here; mv copies it */
    if (ext2_lookup(nd, nn, nl, &old) == 0) {
        if (old == ino)
            return 0;
        if ((r = ext2_iget(old, &tg)))
            return r;
        if (S_ISDIR(tg.mode))
            return -EISDIR;             /* a directory is never replaced */
    } else
        old = 0;
    if (in.links >= 65000)
        return -EMLINK;
    in.links++;
    if ((r = ext2_iput(&in)))
        return r;
    r = old ? dir_change(nd, nn, nl, &tmp, ino, in.mode) : dir_add(nd, nn, nl, ino, in.mode);
    if (r) {
        in.links--;
        ext2_iput(&in);
        return r;
    }
    if ((r = ext2_iget(od->ino, od)) || (r = dir_remove(od, on, ol, &tmp)))
        return r;
    in.links--;
    if ((r = ext2_iput(&in)) || !old)
        return r;
    if ((r = ext2_iget(old, &tg)))
        return r;
    if (tg.links > 1) {
        tg.links--;
        return ext2_iput(&tg);
    }
    unlinked(&tg);
    return 0;
}

/* usage numbers from the superblock (kept current by the allocator) */
int ext2_statfs(u32 *bs, u32 *blocks, u32 *bfree, u32 *files, u32 *ffree)
{
    u8 sbk[24];
    u32 blk = bsize == 1024 ? 1 : 0, base = bsize == 1024 ? 0 : 1024;
    if (bread_copy(blk, base, sbk, 24))
        return -EIO;
    *bs = bsize;
    *blocks = rd32(sbk + 4);
    *bfree = rd32(sbk + 12);
    *files = rd32(sbk + 0);
    *ffree = rd32(sbk + 16);
    return 0;
}
