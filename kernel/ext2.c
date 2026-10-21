#include "kernel.h"

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
static struct { u32 blk, stamp; u8 *data; } buf[NBUF];
static u32 stamp;

static u32 part_lba, bsize, ninodes_per_group, inode_size;
static u32 sectors_per_block, total_blocks, ninodes;
static u32 gd_blk;                      /* first block of the group descriptor table */
static int mounted;

static int bread_copy(u32 blk, u32 off, void *dst, u32 len)
{
    int victim = 0;
    u32 oldest = 0xFFFFFFFF;
    stamp++;
    for (int i = 0; i < NBUF; i++) {
        if (buf[i].data && buf[i].blk == blk) {
            buf[i].stamp = stamp;
            memcpy(dst, buf[i].data + off, len);
            return 0;
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
        buf[victim].data = mem_alloc(bsize, MEM_FASTONLY, 0);
        if (!buf[victim].data)
            buf[victim].data = mem_alloc(bsize, 0, 0);
        if (!buf[victim].data)
            return -1;
    }
    if (ide_read(part_lba + blk * sectors_per_block, sectors_per_block,
                 buf[victim].data))
        return -1;
    buf[victim].blk = blk;
    buf[victim].stamp = stamp;
    memcpy(dst, buf[victim].data + off, len);
    return 0;
}

static u32 rd32(const u8 *p) { return le32(*(const u32 *)p); }

/* ---- partition table: find the ext2 partition (RDB), else sector 0 ---- */

static int ext2_at(u32 lba)
{
    u8 sec[512];
    if (ide_read(lba + 2, 1, sec))      /* byte 1024: superblock */
        return 0;
    return le16(*(u16 *)(sec + 56)) == 0xEF53;
}

static u32 find_partition(void)
{
    u32 sec[128];
    for (u32 i = 0; i < 16; i++) {
        if (ide_read(i, 1, sec))
            return 0xFFFFFFFF;
        if (sec[0] != 0x5244534b)       /* 'RDSK' */
            continue;
        u32 next = sec[7];              /* rdb_PartitionList */
        for (int guard = 0; next != 0xFFFFFFFF && guard < 16; guard++) {
            if (ide_read(next, 1, sec))
                break;
            if (sec[0] != 0x50415254)   /* 'PART' */
                break;
            u32 *env = &sec[32];        /* DosEnvec at byte 128 */
            u32 surf = env[3], bpt = env[5], low = env[9];
            u32 lba = low * surf * bpt;
            if (ext2_at(lba))
                return lba;
            next = sec[4];              /* pb_Next */
        }
    }
    return ext2_at(0) ? 0 : 0xFFFFFFFF; /* no RDB: bare filesystem */
}

int ext2_mount(void)
{
    u8 sb[1024];
    if (ide_init())
        return -1;
    part_lba = find_partition();
    if (part_lba == 0xFFFFFFFF) {
        kprintf("ext2: no ext2 partition found\n");
        return -1;
    }
    if (ide_read(part_lba + 2, 2, sb))
        return -1;
    u32 logbs = rd32(sb + 24);
    bsize = 1024u << logbs;
    if (logbs > 2) {
        kprintf("ext2: block size %u not supported\n", bsize);
        return -1;
    }
    sectors_per_block = bsize / 512;
    ninodes = rd32(sb + 0);
    total_blocks = rd32(sb + 4);
    ninodes_per_group = rd32(sb + 40);
    inode_size = rd32(sb + 76) == 0 ? 128 : le16(*(u16 *)(sb + 88));
    gd_blk = (bsize == 1024) ? 2 : 1;
    mounted = 1;
    kprintf("ext2: partition at sector %u, %u KB blocks, %u blocks, %u inodes\n",
            part_lba, bsize / 1024, total_blocks, ninodes);
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

/* directory entry number `idx` (counting from 0); returns 1, 0 at the end, -1 on error */
int ext2_readdir(const struct inode *dir, u32 *cookie, struct udirent *out)
{
    u32 off = *cookie;
    while (off + 8 <= dir->size) {
        u8 h[8];
        if (ext2_read(dir, off, h, 8) != 8)
            return -1;
        u32 ino = rd32(h);
        u32 reclen = le16(*(u16 *)(h + 4));
        u32 nlen = h[6];
        if (reclen < 8)
            return -1;
        off += reclen;
        if (ino == 0)
            continue;
        if (nlen > sizeof out->name - 1)
            nlen = sizeof out->name - 1;
        if (ext2_read(dir, off - reclen + 8, out->name, nlen) != (int)nlen)
            return -1;
        out->name[nlen] = 0;
        out->ino = ino;
        out->type = h[7];
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
    u32 cookie = 0;
    struct udirent e;
    int r;
    while ((r = ext2_readdir(dir, &cookie, &e)) > 0) {
        u32 i = 0;
        while (i < len && e.name[i] == name[i])
            i++;
        if (i == len && e.name[len] == 0) {
            *ino = e.ino;
            return 0;
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
