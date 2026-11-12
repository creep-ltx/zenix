/* SPDX-License-Identifier: GPL-2.0-only */
#pragma GCC optimize("Os")              /* embedded in the kernel image: size first */
#ifdef ZHOST
#include "../tools/host/zhost.h"     /* host build for the tests (tools/disk-host-test.sh) */
#else
#include "libz.h"
#endif
#include "../kernel/diskproto.h"
#include "rdb.h"
#include "say.h"
#ifndef ZHOST
typedef unsigned char u8;
#endif

/* The disk server: owns the raw disk (IDE or SD card, through the kernel's raw-sector calls,
 * which only this process may use), reads its partition table - an Amiga RDB, else a PC MBR -
 * and serves the whole disk and each partition as a named device.
 * Rules (docs/man/diskd.txt):
 *  - a handle is a device's slot, and a slot keeps its device across rescans (matched by name and place on the
 *    disk), so a filesystem server's handle never points at another partition after fdisk or a new floppy;
 *  - devices that share sectors overlap: the whole disk overlaps every partition, a PC partition of type 0x76
 *    overlaps the RDB partitions inside it, and two RDB partitions may overlap on a damaged table;
 *  - DISK_MOUNT and DISK_RAW are refused while an overlapping device is mounted; a write is refused when it
 *    touches a device another process has mounted. A plain open may read anything;
 *  - a mount whose server has died is released (checked when it would block someone);
 *  - restarted after a crash (DESIGN D11), it gets its slots and claims back from the table it left with the kernel
 *    (save/restore below), so old handles and claims keep meaning the same devices. */

#define MAXDEV 20
static struct diskinfo dev[MAXDEV];
static u8 used[MAXDEV];                 /* the slot holds a device */
static u8 phys[MAXDEV];                 /* which medium: 0 the disk, 1 the RAM disk, 2+N floppy dfN, PH_CD the CD drive */
#define PH_CD 6
static u8 order[MAXDEV];                /* DISK_LIST order: slots in the order the last scan found them */
static int norder;
static int fd_probe = 1;                /* floppy drives looked at: bit 0 = df0; df1-3 only when a name asks for them (probing an absent drive takes a third of a second) */
static int fdrive[MAXDEV];              /* floppy drive number of a device, -1 for the others */
static u32 disk_sectors, disk_caps;     /* disk_caps: DISKCAP_FLUSH if the driver's flush works (tried once at start) */
static u8 *ram_base;                    /* the boot ramdisk, if any */
static u32 ram_sectors;

static void save(void);
static int alive(u32 pid);
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u32 get32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* ---- the CD drive (DESIGN D44, docs/d44-cd.md): cd0, read-only, its 2048-byte blocks served as 512-byte sectors.
 * The kernel only sends packets (SYS_atapi); what they mean is here. A unit attention (a disc put in or taken out, a
 * reset) raises cd_gen; a mount made before it answers -ESTALE from then on (it was of another disc). ---- */
static int cd_there = 1;                /* until the kernel says it has no CD drive */
static u32 cd_sectors, cd_gen;          /* the disc's size in 512-byte sectors (0: no disc); the disc's generation */
static u32 sgen[MAXDEV];                /* a slot of the CD drive: the generation it was mounted on */
static struct uatapi aq;
/* one packet: OP, a block number (REQUEST SENSE: its allocation length << 8), a count of blocks; the drive's bytes
 * from SKIP, LEN of them into BUF -> the bytes it sent, or -errno (-EIO: the sense key is aq.err >> 4) */
static int cd_pkt(u8 op, u32 lba, u32 n, void *buf, u32 len, u32 skip)
{
    memset(&aq, 0, sizeof aq);
    aq.cdb[0] = op;
    put32(aq.cdb + 2, lba);
    aq.cdb[7] = (u8)(n >> 8); aq.cdb[8] = (u8)n;
    aq.buf = buf; aq.len = len; aq.skip = skip;
    int r = sys_atapi(&aq);
    if (r == -ENODEV) cd_there = 0;
    return r;
}
/* the disc in the drive: cd_sectors from READ CAPACITY, 0 without one. A UNIT ATTENTION or no medium (ASC 3Ah) raises
 * cd_gen (a mount was of a disc gone), nothing else does. A drive becoming ready (ASC 04h ASCQ 01h: a disc just put in)
 * is waited for, up to 10 s; one that wants START UNIT (04h 02h) gets it */
static void cd_look(void)
{
    static u8 b[20];
    cd_sectors = 0;
    for (int i = 0; i < 100 && cd_there; i++) {
        int r = cd_pkt(0x00, 0, 0, 0, 0, 0);                    /* TEST UNIT READY */
        if (r >= 0) {
            if (cd_pkt(0x25, 0, 0, b, 8, 0) == 8 && get32(b) < 0x3FFFFFFF) cd_sectors = (get32(b) + 1) * 4;
            break;
        }
        if (aq.err >> 4 == 6) { cd_gen++; continue; }          /* UNIT ATTENTION */
        if (aq.err >> 4 != 2 || cd_pkt(0x03, 18 << 8, 0, b, 18, 0) < 14) break;
        if (b[12] == 0x3A) { cd_gen++; break; }                 /* NOT READY, no medium */
        if (b[12] != 4 || b[13] > 2) break;
        if (b[13] == 2) cd_pkt(0x1B, 1 << 8, 0, 0, 0, 0);      /* START STOP UNIT: start */
        sys_sleep(10);                                          /* NOT READY, becoming ready */
    }
}

/* the scan's result before it is merged into the slots */
static struct diskinfo cand[MAXDEV];
static int cfd[MAXDEV], ncand, dropped;
static u8 cphys[MAXDEV];

static void add(const char *name, const char *label, u32 start, u32 sectors, u32 dostype, u32 flags)
{
    if (ncand == MAXDEV) { dropped++; return; }
    cfd[ncand] = -1;
    cphys[ncand] = 0;
    struct diskinfo *d = &cand[ncand++];
    memset(d, 0, sizeof *d);
    strcpy(d->name, name);
    strncpy(d->label, label, sizeof d->label - 1);
    d->start = start; d->sectors = sectors; d->dostype = dostype; d->flags = flags;
    d->maxsect = 256;
    d->caps = disk_caps;
    if (!strcmp(name, "ram0")) { d->caps = DISKCAP_RAM; cphys[ncand - 1] = 1; }
}

static u32 rd_base;                     /* first sector of the table being read (0, or an MBR partition) */
static int disk_rd(unsigned lba, void *buf) { return sys_blk_read(rd_base + lba, 1, buf); }

static void part_name(char *name, int n)
{
    strcpy(name, "ide0p");
    if (n >= 10) { name[5] = (char)('0' + n / 10); name[6] = (char)('0' + n % 10); name[7] = 0; }
    else { name[5] = (char)('0' + n); name[6] = 0; }
}

/* the partitions of an RDB that starts at sector "base" (inside "limit" sectors); returns 1 if there was one */
static int add_rdb(u32 base, u32 limit, int *count)
{
    static struct rdb r;
    rd_base = base;
    int rc = rdb_read(disk_rd, &r);
    rd_base = 0;
    if (rc != 1) return 0;
    for (int i = 0; i < r.nparts; i++) {
        char name[16];
        part_name(name, ++*count);
        const struct rdb_part *p = &r.part[i];
        if (!p->sectors || p->start + p->sectors > limit || p->start + p->sectors < p->start) {
            say("diskd: %s (%s) has a geometry or size that does not fit the disk: not served\n", name, p->label);
            continue;
        }
        add(name, p->label, base + p->start, p->sectors, p->dostype, p->bootable);
    }
    if (r.noedit) say("diskd: note: the partition table: %s\n", r.noedit);
    return 1;
}

#define DT_RDB_IN_MBR 0x52444200u       /* 'RDB\0': MBR type 0x76, an Amiga RDB inside (Emu68's convention) */
#define DT_MBR        0x4D425200u       /* 'MBR' + type byte: a partition of a kind we do not use */
static u32 le32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

/* A PC partition table (MBR) at sector 0: four primary entries. Type 0x83 (Linux) is ext2 for us (fsd checks the
 * filesystem's features before it uses one; ext3/ext4 are refused or read-only); type 0x76 holds an Amiga RDB of
 * its own (the partitions inside are listed too); the rest (FAT boot partitions, ...) are listed but never mounted.
 * Returns 1 if sector 0 really is a partition table: every entry has a plausible status and lies inside the disk
 * (a bare FAT volume also ends in 55 AA but has boot code where the table is). */
static int add_mbr(int *count)
{
    static u8 sec[512];
    if (disk_rd(0, sec) || sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    u32 st[4], len[4], ty[4], nused = 0;
    for (int i = 0; i < 4; i++) {
        const u8 *e = sec + 446 + 16 * i;
        if (e[0] != 0 && e[0] != 0x80) return 0;
        ty[i] = e[4]; st[i] = le32(e + 8); len[i] = le32(e + 12);
        if (ty[i] == 0 || len[i] == 0) continue;
        if (st[i] == 0 || st[i] + len[i] < st[i] || st[i] + len[i] > disk_sectors) return 0;
        nused++;
    }
    if (!nused) return 0;
    for (int i = 0; i < 4; i++) {
        if (ty[i] == 0 || len[i] == 0) continue;
        char name[16];
        part_name(name, ++*count);
        u32 dt = ty[i] == 0x83 ? DT_EXT2 : ty[i] == 0x76 ? DT_RDB_IN_MBR : DT_MBR | ty[i];
        add(name, "", st[i], len[i], dt, sec[446 + 16 * i] == 0x80);
        if (ty[i] == 0x76) add_rdb(st[i], len[i], count);
    }
    return 1;
}

/* is slot s the device the scan found as candidate c? The same sectors of the same medium: a partition keeps its
 * slot when a change before it in the table renames it (ide0p2 becomes ide0p1) */
static int same(int s, int c)
{
    if (phys[s] == PH_CD) return cphys[c] == PH_CD;     /* one slot for the drive, whatever disc is in it */
    return phys[s] == cphys[c] && dev[s].start == cand[c].start && dev[s].sectors == cand[c].sectors &&
           (!strcmp(dev[s].name, cand[c].name) || (!strncmp(dev[s].name, "ide0p", 5) && !strncmp(cand[c].name, "ide0p", 5)));
}

/* read the tables again and merge: a device found again keeps its slot (and its mount), a mounted device that
 * is gone keeps its slot until it is unmounted, the rest of the slots are freed and new devices take free ones */
static void scan(void)
{
    ncand = 0; dropped = 0;
    if (disk_sectors) add("ide0", "", 0, disk_sectors, 0, 0);
    if (ram_sectors) add("ram0", "", 0, ram_sectors, 0, 0);
    if (cd_there) {
        cd_look();
        if (cd_there) {
            add("cd0", "", 0, cd_sectors, 0, 0);
            cphys[ncand - 1] = PH_CD; cand[ncand - 1].caps = DISKCAP_RO; cand[ncand - 1].maxsect = 128;
        }
    }
    int count = 0;
    if (disk_sectors && !add_rdb(0, disk_sectors, &count)) add_mbr(&count);
    for (int f = 0; f < 4; f++) {                         /* floppy drives with a disk in them: df0 ... df3 */
        if (!(fd_probe & (1 << f))) continue;
        int r = sys_fd_info((u32)f);
        if ((r & 3) == 3) {
            char name[4] = { 'd', 'f', (char)('0' + f), 0 };
            int before = ncand;
            add(name, "", 0, (u32)r >> 8, 0, 0);
            if (ncand > before) {
                cfd[ncand - 1] = f;
                cphys[ncand - 1] = (u8)(2 + f);
                cand[ncand - 1].caps = DISKCAP_FLUSH;     /* writes are synchronous (verified): nothing is held back */
                cand[ncand - 1].maxsect = 22;
            }
        }
    }
    if (dropped) say("diskd: more than %d devices: %d not served\n", MAXDEV, dropped);
    u8 taken[MAXDEV];
    memset(taken, 0, sizeof taken);
    norder = 0;
    for (int s = 0; s < MAXDEV; s++) {                    /* old slots: found again, or kept while mounted */
        if (!used[s]) continue;
        int c;
        for (c = 0; c < ncand; c++) if (!taken[c] && same(s, c)) break;
        if (c < ncand) {
            u32 m = dev[s].mounted;
            dev[s] = cand[c]; dev[s].mounted = m; fdrive[s] = cfd[c]; phys[s] = cphys[c];
            taken[c] = 1;
        } else if (!dev[s].mounted) {
            used[s] = 0;
        }
    }
    for (int c = 0; c < ncand; c++) {                     /* new devices into free slots */
        if (taken[c]) continue;
        int s;
        for (s = 0; s < MAXDEV && used[s]; s++) ;
        if (s == MAXDEV) { say("diskd: no slot for %s: not served\n", cand[c].name); continue; }
        used[s] = 1; dev[s] = cand[c]; fdrive[s] = cfd[c]; phys[s] = cphys[c];
        taken[c] = 1;
    }
    for (int c = 0; c < ncand; c++)                       /* the listing order: as found */
        for (int s = 0; s < MAXDEV; s++)
            if (used[s] && same(s, c) && norder < MAXDEV) { order[norder++] = (u8)s; break; }
    for (int s = 0; s < MAXDEV; s++) {                    /* then mounted devices that are gone from the table */
        int in = 0;
        for (int k = 0; k < norder; k++) if (order[k] == s) in = 1;
        if (used[s] && !in && norder < MAXDEV) order[norder++] = (u8)s;
    }
    save();
}

/* ---- the saved table (D11 5.4) ----
 * For every slot: the medium (0xFF: unused), start, size, name (8 bytes, NUL-terminated: at most "ide0p19") and the
 * claiming pid; then the floppy drives looked at and a checksum. Left with the kernel (SVC_SAVE) whenever it changes,
 * before the reply to the request that changed it; read back (SVC_LOAD) by a restarted diskd before it opens its port
 * and checked like anything read from a disk. A damaged one is never guessed at: diskd stops, and the kernel with it. */
#define REC_SLOT 21
static u8 rec[2 + MAXDEV * REC_SLOT + 4];
static u32 rec_sum(int n)
{
    u32 s = 0x5A44534Bu;
    for (int i = 0; i < n; i++) s = (s << 1 | s >> 31) + rec[i];
    return s;
}
static void save(void)
{
    u8 *p = rec + 2;
    memset(rec, 0, sizeof rec);
    rec[0] = (u8)fd_probe; rec[1] = MAXDEV;
    for (int s = 0; s < MAXDEV; s++, p += REC_SLOT) {
        if (!used[s]) { p[0] = 0xFF; continue; }
        p[0] = phys[s]; put32(p + 1, dev[s].start); put32(p + 5, dev[s].sectors);
        strncpy((char *)p + 9, dev[s].name, 7); put32(p + 17, dev[s].mounted);
    }
    put32(p, rec_sum((int)(p - rec)));
    sys_svc_save(rec, sizeof rec);
}
/* 0: restored (or nothing saved), -1: damaged */
static int restore(void)
{
    int n = sys_svc_load(rec, sizeof rec);
    if (n <= 0) return 0;
    if (n != (int)sizeof rec || rec[1] != MAXDEV || rec[0] > 15 || get32(rec + n - 4) != rec_sum(n - 4)) return -1;
    for (int pass = 0; pass < 2; pass++) {                /* every slot checked before the first is used */
        const u8 *p = rec + 2;
        for (int s = 0; s < MAXDEV; s++, p += REC_SLOT) {
            if (p[0] == 0xFF) continue;
            u32 st = get32(p + 1), sz = get32(p + 5), lim = p[0] == 0 ? disk_sectors : p[0] == 1 ? ram_sectors : p[0] == PH_CD ? ~0u : 3520;
            if (p[0] > PH_CD || p[16] || !p[9] || st + sz < st || st + sz > lim) return -1;
            if (!pass) continue;
            used[s] = 1; phys[s] = p[0]; memset(&dev[s], 0, sizeof dev[s]);
            dev[s].start = st; dev[s].sectors = sz; strcpy(dev[s].name, (const char *)p + 9);
            dev[s].mounted = alive(get32(p + 17)) ? get32(p + 17) : 0;
            fdrive[s] = p[0] >= 2 && p[0] < PH_CD ? p[0] - 2 : -1;
        }
    }
    fd_probe = rec[0];
    return 0;
}

static int find(const char *name)
{
    for (int k = 0; k < norder; k++)
        if (!strcmp(dev[order[k]].name, name)) return order[k];
    return -1;
}

static int alive(u32 pid)               /* is the process that mounted a device still there? */
{
    struct uproc p;
    for (u32 i = 0; i < 64; i++) {
        if (sys_procinfo(i, &p) < 0) break;
        if (p.pid == pid && p.state != 0 && p.state != 5) return 1;     /* not free, not a zombie */
    }
    return 0;
}

/* the process that has device s mounted, if it still exists (a mount of a dead server is released here) */
static u32 holder(int s)
{
    if (dev[s].mounted && !alive(dev[s].mounted)) {
        say("diskd: %s: the filesystem server (pid %u) is gone: mount released\n", dev[s].name, dev[s].mounted);
        dev[s].mounted = 0;
        save();
    }
    return dev[s].mounted;
}

/* does device s (sectors [lba, lba+n) of it) share sectors with device t? */
static int overlaps(int s, u32 lba, u32 n, int t)
{
    if (phys[s] != phys[t]) return 0;
    u32 a = dev[s].start + lba, b = dev[t].start;
    return a < b + dev[t].sectors && b < a + n;
}

/* is there a device other than "self" overlapping [lba, lba+n) of s that someone other than pid has mounted? */
static int blocked(int s, u32 lba, u32 n, u32 pid)
{
    for (int t = 0; t < MAXDEV; t++)
        if (used[t] && dev[t].mounted && dev[t].mounted != pid && overlaps(s, lba, n, t) && holder(t))   /* (holder checks the pid only then: writes are hot) */
            return 1;
    return 0;
}

static int handle(struct umsg *m)
{
    if (m->uid != 0 && m->type != DISK_LIST && m->type != DISK_INFO && m->type != DISK_LISTSZ && m->type != DISK_INFOSZ)
        return -EACCES;                 /* raw sectors bypass every file permission: root only (the filesystem servers are root) */
    switch (m->type) {
    case DISK_OPEN: {
        const char *nm = (const char *)m->a[0];
        if (nm[0] == 'd' && nm[1] == 'f' && nm[2] >= '0' && nm[2] <= '3' && !nm[3]) {
            fd_probe |= 1 << (nm[2] - '0');
            scan();                                                  /* a disk may have been put in since */
        }
        if (!strcmp(nm, "cd0") && cd_there) scan();                 /* a disc may have been put in or changed */
        int h = find(nm);
        if (h < 0) return -ENODEV;
        if (m->a[1] & (DISK_MOUNT | DISK_RAW)) {
            /* a mount, or a tool that may write the device: nothing overlapping it may be mounted (not even
             * by the caller: a server mounts one device) - except a repeated mount by the holder of the claim (its
             * request was in service when a diskd died, and it asks again) */
            if ((m->a[1] & DISK_MOUNT) && dev[h].mounted == (u32)m->from) return h;
            if (blocked(h, 0, dev[h].sectors, 0)) return -EBUSY;
            if (m->a[1] & DISK_MOUNT) { dev[h].mounted = (u32)m->from; sgen[h] = cd_gen; save(); }
        }
        return h;
    }
    case DISK_UNMOUNT:
        if (m->a[0] < MAXDEV && used[m->a[0]] && dev[m->a[0]].mounted == (u32)m->from) { dev[m->a[0]].mounted = 0; save(); return 0; }
        return -EINVAL;
    case DISK_READ: case DISK_WRITE: {
        u32 h = m->a[0], lba = m->a[1], n = m->a[2];
        if (h >= MAXDEV || !used[h]) return -EBADF;
        if (phys[h] == PH_CD && dev[h].mounted == (u32)m->from && sgen[h] != cd_gen) return -ESTALE;  /* mounted on another disc */
        if (n == 0 || lba + n < lba || lba + n > dev[h].sectors) return -EINVAL;
        if (phys[h] == PH_CD) {                                     /* D44: 2048-byte blocks, the sectors asked kept */
            if (m->type == DISK_WRITE) return -EROFS;
            if (n > 128) return -EINVAL;                            /* maxsect */
            for (int t = 0; t < 2; t++) {
                if (t && dev[h].mounted == (u32)m->from) return -ESTALE;
                u32 nb = ((lba & 3) + n + 3) >> 2;
                if (cd_pkt(0x28, lba >> 2, nb, (void *)m->a[3], n * 512, (lba & 3) * 512) == (int)nb * 2048) return 0;
                if (aq.err >> 4 != 6) break;
                cd_gen++;                                           /* UNIT ATTENTION: the disc may have changed */
            }
            return -EIO;
        }
        if (m->type == DISK_WRITE && blocked((int)h, lba, n, (u32)m->from)) return -EBUSY;
        if (phys[h] == 1) {                                         /* the RAM disk: plain memory */
            if (m->type == DISK_READ) memcpy((void *)m->a[3], ram_base + lba * 512, n * 512);
            else memcpy(ram_base + lba * 512, (const void *)m->a[3], n * 512);
            return 0;
        }
        if (fdrive[h] >= 0)
            return sys_fd_rw((u32)fdrive[h], lba, n, (void *)m->a[3], m->type == DISK_WRITE);
        lba += dev[h].start;
        return m->type == DISK_READ ? sys_blk_read(lba, n, (void *)m->a[3])
                                    : sys_blk_write(lba, n, (const void *)m->a[3]);
    }
    case DISK_INFO: case DISK_INFOSZ: case DISK_LIST: case DISK_LISTSZ: {
        /* the struct goes into the caller's buffer: the SZ forms pass its size (a smaller buffer than the first 80
         * bytes is refused), the old forms get the first 80 bytes (frozen interfaces, CLAUDE.md) */
        u32 n = m->type == DISK_INFOSZ || m->type == DISK_LISTSZ ? m->a[2] : DISKINFO_FIRST, h;
        if (n < DISKINFO_FIRST) return -EINVAL;
        if (n > sizeof(struct diskinfo)) n = sizeof(struct diskinfo);
        if (m->type == DISK_INFO || m->type == DISK_INFOSZ) {
            if (m->a[0] >= MAXDEV || !used[m->a[0]]) return -EBADF;
            h = m->a[0];
        } else {
            if (m->a[0] >= (u32)norder) return -ENODEV;        /* past the end of the list */
            h = order[m->a[0]];
        }
        holder((int)h);
        memcpy((void *)m->a[1], &dev[h], n);
        return 0;
    }
    case DISK_FLUSH:
        if (m->a[0] >= MAXDEV || !used[m->a[0]]) return -EBADF;
        if (phys[m->a[0]] != 0) return 0;                           /* RAM disk, floppy: nothing to flush */
        if (!(dev[m->a[0]].caps & DISKCAP_FLUSH)) return -EOPNOTSUPP;   /* this disk cannot promise it */
        return sys_blk_flush();
    case DISK_RESCAN:
        scan();
        return norder;
    }
    return -ENOSYS;
}

int main(void)
{
    int n = sys_blk_size();
    disk_sectors = n > 0 ? (u32)n : 0;
    u32 ram[2];
    if (sys_blk_ram(ram) == 0 && ram[1]) { ram_base = (u8 *)ram[0]; ram_sectors = ram[1] / 512; }
    int any_fd = 0;
    if (sys_fd_info(0) & 1) any_fd = 1;
    cd_look();                                                      /* D44: is there a CD drive, a disc in it */
    if (!disk_sectors && !ram_sectors && !any_fd && !cd_there) { say("diskd: no disk\n"); return 1; }
    if (disk_sectors) {                 /* can the drive make writes durable? (ATA FLUSH CACHE, SCSI SYNCHRONIZE CACHE) */
        int f = sys_blk_flush();
        disk_caps = f == 0 ? DISKCAP_FLUSH : 0;
        if (f) say("diskd: ide0 cannot flush its write cache (%d): sync and ZenixFS commits are weaker there\n", f);
    }
    if (sys_svc_self() > 1 && restore()) {             /* started again after an end: the old slots and claims */
        say("diskd: the saved device table is damaged: stopping so no disk gets another's writes\n");
        return SVC_EXIT_DAMAGED;
    }
    scan();
    int port = sys_port_create("disk");
    if (port < 0) return 1;
    struct umsg *m = sys_recv(port, 1);
    while ((int)m != -1)
        m = sys_replyrecv(m, handle(m), port);
    return 1;
}
