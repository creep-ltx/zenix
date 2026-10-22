#include "kernel.h"

/* Minimal FAT16 (whole-disk, no partition table): mount, root directory
 * list, file read, file create/replace. On-disk format is little-endian;
 * everything goes through rd16/rd32/wr16/wr32. Single static sector
 * buffer — strictly sequential use, no cache yet (M2 TODO). */

static struct {
    u32 fat_start, fat_sectors, nfats;
    u32 root_start, root_sectors, root_entries;
    u32 data_start, spc, nclusters;
} fs;

static u8 sec[512];
static u32 sec_lba = ~0u;       /* what's currently in sec[] */

static int memcmp_4(const u8 *a, const char *b)
{
    return a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3];
}

static u16 rd16(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 rd32(const u8 *p) { return p[0] | (p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static void wr16(u8 *p, u16 v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void wr32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static u32 part_start;          /* LBA of the FAT filesystem's sector 0 */

static int load(u32 lba)
{
    lba += part_start;
    if (sec_lba == lba)
        return 0;
    if (ide_read(lba, 1, sec))
        return -1;
    sec_lba = lba;
    return 0;
}

static int store(u32 lba)
{
    lba += part_start;
    sec_lba = lba;
    return ide_write(lba, 1, sec) ? -1 : 0;
}

static u32 be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Amiga disks carry a Rigid Disk Block in the first 16 sectors; the
 * filesystem lives in its first partition. Raw images without an RDB
 * are treated as superfloppy (filesystem at LBA 0). RDB fields are
 * big-endian; partition start = LowCyl * Surfaces * BlocksPerTrack. */
static int find_partition(void)
{
    part_start = 0;
    for (u32 s = 0; s < 16; s++) {
        if (ide_read(s, 1, sec))
            return -1;
        if (memcmp_4(sec, "RDSK"))
            continue;
        u32 pl = be32(sec + 28);        /* rdb_PartitionList */
        if (pl == 0xFFFFFFFFu)
            break;
        if (ide_read(pl, 1, sec))
            return -1;
        if (memcmp_4(sec, "PART"))
            break;
        u32 heads = be32(sec + 140);    /* de_Surfaces */
        u32 bpt = be32(sec + 148);      /* de_BlocksPerTrack */
        u32 low = be32(sec + 164);      /* de_LowCyl */
        part_start = low * heads * bpt;
        kprintf("fat: RDB partition, filesystem at lba %u\n", part_start);
        return 0;
    }
    kprintf("fat: no RDB, superfloppy layout\n");
    return 0;
}

int fat_mount(void)
{
    sec_lba = ~0u;
    if (find_partition())
        return -1;
    if (load(0))
        return -1;
    if (rd16(sec + 11) != 512) {
        kprintf("fat: unsupported sector size %u; sector 0 starts:", rd16(sec + 11));
        for (int i = 0; i < 16; i++)
            kprintf(" %x", sec[i]);
        kprintf("\n");
        return -1;
    }
    fs.spc = sec[13];
    fs.fat_start = rd16(sec + 14);
    fs.nfats = sec[16];
    fs.root_entries = rd16(sec + 17);
    fs.fat_sectors = rd16(sec + 22);
    fs.root_start = fs.fat_start + fs.nfats * fs.fat_sectors;
    fs.root_sectors = (fs.root_entries * 32 + 511) / 512;
    fs.data_start = fs.root_start + fs.root_sectors;
    u32 total = rd16(sec + 19) ? rd16(sec + 19) : rd32(sec + 32);
    fs.nclusters = (total - fs.data_start) / fs.spc;
    if (!fs.spc || !fs.fat_sectors || fs.nclusters < 16 ||
        fs.nclusters >= 65525) {
        kprintf("fat: bad/unsupported BPB (%u clusters)\n", fs.nclusters);
        return -1;
    }
    kprintf("fat: FAT16, %u sectors/cluster, %u clusters, root %u entries\n",
            fs.spc, fs.nclusters, fs.root_entries);
    return 0;
}

static u32 cluster_lba(u32 cl)
{
    return fs.data_start + (cl - 2) * fs.spc;
}

static u32 fat_get(u32 cl)
{
    if (load(fs.fat_start + (cl * 2) / 512))
        return 0xFFFF;
    return rd16(sec + (cl * 2) % 512);
}

static int fat_set(u32 cl, u16 val)
{
    for (u32 f = 0; f < fs.nfats; f++) {
        u32 lba = fs.fat_start + f * fs.fat_sectors + (cl * 2) / 512;
        if (load(lba))
            return -1;
        wr16(sec + (cl * 2) % 512, val);
        if (store(lba))
            return -1;
    }
    return 0;
}

static u32 fat_alloc(void)
{
    for (u32 cl = 2; cl < fs.nclusters + 2; cl++)
        if (fat_get(cl) == 0)
            return cl;
    return 0;
}

/* "HELLO.TXT" -> "HELLO   TXT" (8+3, space-padded, upper-cased) */
static void to83(const char *name, char out[11])
{
    memset(out, ' ', 11);
    int i = 0;
    for (; *name && *name != '.' && i < 8; name++, i++)
        out[i] = (*name >= 'a' && *name <= 'z') ? *name - 32 : *name;
    if (*name == '.')
        name++;
    for (i = 8; *name && i < 11; name++, i++)
        out[i] = (*name >= 'a' && *name <= 'z') ? *name - 32 : *name;
}

static int memcmp_11(const u8 *a, const char *b)
{
    for (int i = 0; i < 11; i++)
        if (a[i] != (u8)b[i])
            return 1;
    return 0;
}

/* find a root entry by 8.3 name (or, if name==0, a free slot);
 * returns 0 and fills lba/off on success */
static int root_find(const char name83[11], u32 *lba, u32 *off)
{
    for (u32 s = 0; s < fs.root_sectors; s++) {
        if (load(fs.root_start + s))
            return -1;
        for (u32 o = 0; o < 512; o += 32) {
            u8 c = sec[o];
            if (name83) {
                if (c == 0)
                    return -1;              /* end of directory */
                if (c == 0xE5 || (sec[o + 11] & 0x08))
                    continue;               /* deleted / volume label */
                if (memcmp_11(sec + o, name83))
                    continue;
            } else {
                if (c != 0 && c != 0xE5)
                    continue;               /* occupied */
            }
            *lba = fs.root_start + s;
            *off = o;
            return 0;
        }
    }
    return -1;
}

void fat_ls(void)
{
    kprintf("fat: root directory:\n");
    for (u32 s = 0; s < fs.root_sectors; s++) {
        if (load(fs.root_start + s))
            return;
        for (u32 o = 0; o < 512; o += 32) {
            u8 c = sec[o];
            if (c == 0)
                return;
            if (c == 0xE5 || (sec[o + 11] & 0x08))
                continue;
            char nm[13];
            int n = 0;
            for (int i = 0; i < 8 && sec[o + i] != ' '; i++)
                nm[n++] = sec[o + i];
            if (sec[o + 8] != ' ') {
                nm[n++] = '.';
                for (int i = 8; i < 11 && sec[o + i] != ' '; i++)
                    nm[n++] = sec[o + i];
            }
            nm[n] = 0;
            kprintf("  %s (%u bytes)\n", nm, rd32(sec + o + 28));
        }
    }
}

/* locate a file: first cluster + size (for streaming reads) */
int fat_stat(const char *name, u32 *first, u32 *size)
{
    char n83[11];
    to83(name, n83);
    u32 lba, off;
    if (root_find(n83, &lba, &off))
        return -1;
    if (load(lba))
        return -1;
    *first = rd16(sec + off + 26);
    *size = rd32(sec + off + 28);
    return 0;
}

/* read n bytes at byte offset pos from a chain; returns bytes read.
 * Rewalk from the first cluster each call — O(n^2) for big files, fine
 * until the block cache lands. */
int fat_read_at(u32 first, u32 size, u32 pos, void *buf, u32 n)
{
    if (pos >= size)
        return 0;
    if (n > size - pos)
        n = size - pos;
    u32 cbytes = fs.spc * 512;
    u32 cl = first;
    for (u32 skip = pos / cbytes; skip; skip--) {
        cl = fat_get(cl);
        if (cl < 2 || cl >= 0xFFF8)
            return -1;
    }
    u32 coff = pos % cbytes;
    u8 *p = buf;
    u32 left = n;
    while (left) {
        if (load(cluster_lba(cl) + coff / 512))
            return -1;
        u32 soff = coff % 512;
        u32 take = 512 - soff;
        if (take > left)
            take = left;
        memcpy(p, sec + soff, take);
        p += take;
        left -= take;
        coff += take;
        if (coff == cbytes) {
            coff = 0;
            cl = fat_get(cl);
            if ((cl < 2 || cl >= 0xFFF8) && left)
                break;
        }
    }
    return (int)(n - left);
}

/* nth visible root entry (for the readdir syscall); -1 past the end */
int fat_readdir(u32 index, char name[16], u32 *size)
{
    u32 seen = 0;
    for (u32 s = 0; s < fs.root_sectors; s++) {
        if (load(fs.root_start + s))
            return -1;
        for (u32 o = 0; o < 512; o += 32) {
            u8 c = sec[o];
            if (c == 0)
                return -1;
            if (c == 0xE5 || (sec[o + 11] & 0x08))
                continue;
            if (seen++ != index)
                continue;
            int n = 0;
            for (int i = 0; i < 8 && sec[o + i] != ' '; i++)
                name[n++] = (char)sec[o + i];
            if (sec[o + 8] != ' ') {
                name[n++] = '.';
                for (int i = 8; i < 11 && sec[o + i] != ' '; i++)
                    name[n++] = (char)sec[o + i];
            }
            name[n] = 0;
            *size = rd32(sec + o + 28);
            return 0;
        }
    }
    return -1;
}

/* read whole file into buf (up to max); returns size or -1 */
int fat_read_file(const char *name, void *buf, u32 max)
{
    char n83[11];
    to83(name, n83);
    u32 lba, off;
    if (root_find(n83, &lba, &off))
        return -1;
    if (load(lba))
        return -1;
    u32 size = rd32(sec + off + 28);
    u32 cl = rd16(sec + off + 26);
    u32 left = size < max ? size : max;
    u8 *p = buf;
    while (left && cl >= 2 && cl < 0xFFF8) {
        for (u32 s = 0; s < fs.spc && left; s++) {
            if (load(cluster_lba(cl) + s))
                return -1;
            u32 n = left < 512 ? left : 512;
            memcpy(p, sec, n);
            p += n;
            left -= n;
        }
        cl = fat_get(cl);
    }
    return (int)(size < max ? size : max);
}

/* create or replace a file in the root directory */
int fat_write_file(const char *name, const void *data, u32 len)
{
    char n83[11];
    to83(name, n83);

    /* free the old chain if the file exists */
    u32 lba, off;
    if (root_find(n83, &lba, &off) == 0) {
        if (load(lba))
            return -1;
        u32 cl = rd16(sec + off + 26);
        while (cl >= 2 && cl < 0xFFF8) {
            u32 next = fat_get(cl);
            if (fat_set(cl, 0))
                return -1;
            cl = next;
        }
    } else if (root_find(0, &lba, &off)) {
        kprintf("fat: root directory full\n");
        return -1;
    }

    /* allocate and write the new chain */
    u32 first = 0, prev = 0;
    const u8 *p = data;
    u32 left = len;
    while (left) {
        u32 cl = fat_alloc();
        if (!cl) {
            kprintf("fat: disk full\n");
            return -1;
        }
        if (fat_set(cl, 0xFFFF))
            return -1;
        if (prev && fat_set(prev, (u16)cl))
            return -1;
        if (!first)
            first = cl;
        prev = cl;
        for (u32 s = 0; s < fs.spc; s++) {
            memset(sec, 0, 512);
            u32 n = left < 512 ? left : 512;
            if (n) {
                memcpy(sec, p, n);
                p += n;
                left -= n;
            }
            if (store(cluster_lba(cl) + s))
                return -1;
        }
    }

    /* write the directory entry */
    if (load(lba))
        return -1;
    memset(sec + off, 0, 32);
    memcpy(sec + off, n83, 11);
    sec[off + 11] = 0x20;           /* archive */
    wr16(sec + off + 26, (u16)first);
    wr32(sec + off + 28, len);
    return store(lba);
}
