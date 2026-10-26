/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* The volume id (DESIGN D11 4.3, docs/d11-service-manager.md): where a filesystem is - its device's start and size on
 * its medium (ide0, ram0, dfN: the device name's prefix) - and its superblock's 16-byte uuid. boot reads it for the
 * root and registers the server with "-id START,SIZE,UUID" (hex); the server finds its volume by it at every start:
 * the place first (a partition renamed by fdisk is found under its new name), the uuid confirms it (an all-zero one,
 * from a tool that wrote none, confirms nothing); only if nothing is at that place, the one device that carries the
 * uuid. A copy made with dd carries the same uuid: two carriers are refused. Needs libz.h and diskproto.h. */

static int vol_call(int port, u32 type, u32 a0, u32 a1, u32 a2, void *a3)
{
    struct umsg m;
    m.type = type; m.a[0] = a0; m.a[1] = a1; m.a[2] = a2; m.a[3] = (u32)a3;
    int r = sys_call(port, &m);
    return r == -ECONNRESET ? sys_call(port, &m) : r;     /* a diskd that ended while serving it (diskcall.h) */
}

static __attribute__((unused)) unsigned char vol_state;    /* ext2: the superblock's state (s_state) vol_look read */
/* the filesystem on DEV: 1 ext2 (magic at byte 1080, uuid at 1128), 2 ZenixFS ("ZNFS" at byte 32, uuid at 68), 0
 * neither; *di its disk server entry, uuid its id (zeros if none or neither) */
static __attribute__((unused)) int vol_look(int port, const char *dev, struct diskinfo *di, unsigned char *uuid)
{
    static unsigned char sec[512];
    int type = 0, h = vol_call(port, DISK_OPEN, (u32)dev, 0, 0, 0);
    memset(uuid, 0, 16);
    if (h < 0 || vol_call(port, DISK_INFOSZ, (u32)h, (u32)di, sizeof *di, 0) < 0)
        return -1;
    if (vol_call(port, DISK_READ, (u32)h, 2, 1, sec) == 0 && sec[56] == 0x53 && sec[57] == 0xEF) {
        type = 1;
        memcpy(uuid, sec + 104, 16);
        vol_state = sec[58];                        /* s_state's low byte: bit 0 clean, bit 1 errors (DESIGN D17) */
    } else if (vol_call(port, DISK_READ, (u32)h, 0, 1, sec) == 0 && !memcmp(sec + 32, "ZNFS", 4)) {
        type = 2;
        memcpy(uuid, sec + 68, 16);
    }
    return type;
}

static __attribute__((unused)) char *vol_hex8(char *p, const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++) {
        *p++ = "0123456789abcdef"[b[i] >> 4];
        *p++ = "0123456789abcdef"[b[i] & 15];
    }
    return p;
}
/* the volume id of DEV, "START,SIZE,UUID" in hex, into ID (at least 50 bytes; empty if DEV cannot be read): what boot
 * and mount register a filesystem server with. 1 ext2, 2 ZenixFS, 0 neither */
static __attribute__((unused)) int vol_id(int port, const char *dev, char *id)
{
    struct diskinfo di;
    unsigned char uuid[16], w[8];
    int type = vol_look(port, dev, &di, uuid);
    *id = 0;
    if (type < 0)
        return 0;
    for (int i = 0; i < 4; i++) {
        w[i] = (unsigned char)(di.start >> (24 - 8 * i));
        w[4 + i] = (unsigned char)(di.sectors >> (24 - 8 * i));
    }
    char *p = vol_hex8(id, w, 4);
    *p++ = ',';
    p = vol_hex8(p, w + 4, 4);
    *p++ = ',';
    *vol_hex8(p, uuid, 16) = 0;
    return type;
}

static __attribute__((unused)) int vol_medium(const char *a, const char *b)       /* the same medium: ide0..., ram0, the same dfN */
{
    return !strncmp(a, b, a[0] == 'd' ? 3 : 4);
}

static __attribute__((unused)) u32 vol_hex(const char **p, int n)                 /* n hex digits (0: up to a ',') */
{
    u32 v = 0;
    for (int i = 0; n ? i < n : **p && **p != ','; i++, (*p)++) {
        char c = **p;
        v = v << 4 | (u32)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
    }
    return v;
}

/* the device that holds the volume ID; its name into NAME. 0, or 1 after printing why not (WHO: "fsd", "fs.ide0p2"...)
 * RESTART: this is not the server's first start (a floppy without a uuid is then refused: it may have been swapped) */
static __attribute__((unused)) int vol_find(int port, const char *dev, const char *id, char *name, const char *who, int restart)
{
    unsigned char want[16], got[16];
    struct diskinfo di;
    const char *p = id;
    u32 start = vol_hex(&p, 0);
    if (*p) p++;
    u32 size = vol_hex(&p, 0);
    if (*p) p++;
    int zero = 1;
    for (int i = 0; i < 16; i++) {
        want[i] = (unsigned char)vol_hex(&p, 2);
        if (want[i]) zero = 0;
    }
    if (restart && zero && dev[0] == 'd') {
        printf("%s: a floppy without a volume id is not served again after a restart: mount it again\n", who);
        return 1;
    }
    int at_place = 0, carriers = 0;
    for (u32 i = 0; i < 32; i++) {
        if (vol_call(port, DISK_LISTSZ, i, (u32)&di, sizeof di, 0) < 0) break;
        if (!vol_medium(di.name, dev) || di.start != start || di.sectors != size) continue;
        at_place = 1;
        if (vol_look(port, di.name, &di, got) >= 0 && (zero || !memcmp(got, want, 16))) {
            strcpy(name, di.name);
            return 0;
        }
    }
    if (!at_place && !zero)                                /* the partition was moved: the uuid alone, if unique */
        for (u32 i = 0; i < 32; i++) {
            char n[16];
            if (vol_call(port, DISK_LISTSZ, i, (u32)&di, sizeof di, 0) < 0) break;
            strcpy(n, di.name);
            if (vol_look(port, n, &di, got) > 0 && !memcmp(got, want, 16) && carriers++ == 0)
                strcpy(name, n);
        }
    if (carriers == 1)
        return 0;
    printf(carriers ? "%s: two devices carry the volume's id\n" : "%s: the volume is on no device\n", who);
    return 1;
}
