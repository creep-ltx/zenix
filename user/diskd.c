#include "libz.h"
#include "../kernel/diskproto.h"
typedef unsigned char u8;

/* The disk server: owns the IDE disk (through the kernel's raw-sector calls,
 * which only this process may use), reads the Amiga partition table (RDB)
 * and serves the whole disk and each partition as a named device. */

#define MAXDEV 12
static struct diskinfo dev[MAXDEV];
static int ndev;
static u32 disk_sectors;

static void add(const char *name, const char *label, u32 start, u32 sectors, u32 dostype, u32 flags)
{
    if (ndev == MAXDEV) return;
    struct diskinfo *d = &dev[ndev++];
    memset(d, 0, sizeof *d);
    strcpy(d->name, name);
    strcpy(d->label, label);
    d->start = start; d->sectors = sectors; d->dostype = dostype; d->flags = flags;
}

static void scan(void)
{
    static u32 sec[128];
    ndev = 0;
    add("ide0", "", 0, disk_sectors, 0, 0);
    for (u32 i = 0; i < 16; i++) {
        if (sys_blk_read(i, 1, sec)) return;
        if (sec[0] != 0x5244534b) continue;                 /* 'RDSK' */
        u32 next = sec[7];                                  /* partition list */
        for (int guard = 0; next != 0xFFFFFFFF && guard < 16; guard++) {
            if (sys_blk_read(next, 1, sec) || sec[0] != 0x50415254) break;   /* 'PART' */
            u32 *env = &sec[32];                            /* DosEnvec */
            u32 surf = env[3], bpt = env[5], low = env[9], high = env[10];
            u32 start = low * surf * bpt, size = (high - low + 1) * surf * bpt;
            char label[32];
            u8 *nm = (u8 *)&sec[9];                         /* BSTR drive name */
            u32 l = nm[0] < 31 ? nm[0] : 31;
            memcpy(label, nm + 1, l); label[l] = 0;
            char name[16] = "ide0p";
            name[5] = (char)('1' + (ndev - 1)); name[6] = 0;
            add(name, label, start, size, env[16], sec[5] & 1);
            next = sec[4];
        }
        return;
    }
}

static int find(const char *name)
{
    for (int i = 0; i < ndev; i++)
        if (!strcmp(dev[i].name, name)) return i;
    return -1;
}

static int handle(struct umsg *m)
{
    switch (m->type) {
    case DISK_OPEN: return find((const char *)m->a[0]);
    case DISK_READ: case DISK_WRITE: {
        u32 h = m->a[0], lba = m->a[1], n = m->a[2];
        if (h >= (u32)ndev || n == 0 || lba + n < lba || lba + n > dev[h].sectors) return -1;
        lba += dev[h].start;
        return m->type == DISK_READ ? sys_blk_read(lba, n, (void *)m->a[3])
                                    : sys_blk_write(lba, n, (const void *)m->a[3]);
    }
    case DISK_INFO:
        if (m->a[0] >= (u32)ndev) return -1;
        memcpy((void *)m->a[1], &dev[m->a[0]], sizeof(struct diskinfo));
        return 0;
    case DISK_LIST:
        if (m->a[0] >= (u32)ndev) return -1;
        memcpy((void *)m->a[1], &dev[m->a[0]], sizeof(struct diskinfo));
        return 0;
    case DISK_RESCAN:
        scan();
        return ndev;
    }
    return -1;
}

int main(void)
{
    int n = sys_blk_size();
    if (n <= 0) { printf("diskd: no disk\n"); return 1; }
    disk_sectors = (u32)n;
    scan();
    int port = sys_port_create("disk");
    if (port < 0) return 1;
    struct umsg *m = sys_recv(port, 1);
    while ((int)m != -1)
        m = sys_replyrecv(m, handle(m), port);
    return 1;
}
