/* SPDX-License-Identifier: GPL-2.0-only */
#ifdef ZHOST
#include "../tools/host/zhost.h"           /* the host tests (tools/host/fsd-host.c built with -DFATD) */
#else
#include "libz.h"
typedef unsigned char u8;
typedef unsigned short u16;
#endif
#include "../kernel/fsproto.h"
#include "../kernel/diskproto.h"
#include "../kernel/fsperm.h"
#include "diskcall.h"
#include "volid.h"
#include "utf16name.h"

/* fatd DEVICE -id START,SIZE [-o OPTIONS]: the FAT filesystem server (FAT12, FAT16, FAT32, long names), a file service
 * started by mount(8) as fs.DEVICE. The decisions and why: docs/d30-fat.md. In short:
 * - Everything read from the disk is untrusted: a damaged volume answers an error, never crashes or hangs.
 * - The order of writes: data, then FAT, then directories; a freed chain is zeroed in the FAT only after the directory
 *   that named it is on the disk. So a power cut can lose clusters, never another file.
 * - The boot sector is never written. */

#define SECT       512
#define NCACHE     24                  /* sectors of FAT, directory and partial data */
#define NOPEN      64                  /* NFILE: an open file has a slot here */
#define NPEND      16                  /* chains waiting to be freed at the commit */
#define COMMIT_TICKS 300               /* 3 s after the first change */
#define QUIET_TICKS  500               /* the volume goes clean 5 s after the last commit */
#define DIR_MAX    (65536u * 32)       /* the format's largest directory */
#define EOC        0x0FFFFFFFu         /* fat_get's answer for any end-of-chain value */
#define ROOT       1u
#define DINO(c)    (0x40000000u | (c))
#define A_RO 0x01
#define A_VOL 0x08
#define A_DIR 0x10
#define A_ARC 0x20
#define A_LFN 0x0F

typedef struct { u8 e[32]; u32 dir, off, first, lba; u16 byte; u8 haslfn, isroot; } node_t;

static int dport = -1, dh = -1;
static u32 maxsect = 1;
/* the volume, from the boot sector (all checked in check_bpb) */
static u32 fat_start, fat_secs, nfats, active, root_start, root_secs, data_start, spc, clus_shift, nclus, root_clus,
           fsinfo_sec, total_secs, fat_bits;
static int mirror = 1;
/* options */
static u32 m_uid, m_gid, m_fmode = 0644, m_dmode = 0755;
static int readonly, force;
/* state */
static u32 free_cnt = 0xFFFFFFFFu, next_free = 2;
static int marked, dirty, info_dirty;
static u32 dirty_since;
static u32 pend[NPEND], npend;
static char devname[16];
static unsigned cuid, cgid, cng;
static unsigned short cgs[NGROUPS];
static int quitting;

/* ---- bytes on the disk are little-endian ---- */
static u32 get16(const u8 *p) { return p[0] | (u32)p[1] << 8; }
static u32 get32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void put32(u8 *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }

/* ---- the disk ---- */
static int dcall(u32 type, u32 lba, u32 n, void *buf)
{
    struct umsg m;
    m.type = type; m.a[0] = (u32)dh; m.a[1] = lba; m.a[2] = n; m.a[3] = (u32)buf;
    return disk_call(dport, &m) < 0 ? -EIO : 0;
}
static int need_flush;                 /* something was written since the last disk flush */
static int dflush(void)
{
    struct umsg m;
    need_flush = 0;
    m.type = DISK_FLUSH; m.a[0] = (u32)dh;
    int r = disk_call(dport, &m);
    return r < 0 && r != -95 ? -EIO : 0;              /* -95: the disk cannot flush */
}
/* N sectors at LBA, in pieces the device takes */
static int dio(u32 type, u32 lba, u32 n, void *buf)
{
    u8 *p = buf;
    if (lba >= total_secs || n > total_secs - lba) return -EIO;
    if (type == DISK_WRITE) {
        if (!lba) return -EIO;                    /* the boot sector is never written (d30) */
        need_flush = 1;
    }
    while (n) {
        u32 k = n < maxsect ? n : maxsect;
        int r = dcall(type, lba, k, p);
        if (r) return r;
        lba += k; n -= k; p += k * SECT;
    }
    return 0;
}

/* ---- the sector cache ---- */
enum { K_DATA, K_FAT, K_DIR };
struct cbuf { u32 lba, stamp; u8 kind, dirty, valid, pad; u8 d[SECT]; };
static struct cbuf cache[NCACHE];
static struct cbuf *clast;
static u32 cstamp;

static int fat_write_one(struct cbuf *c, u32 copy)
{
    return dio(DISK_WRITE, c->lba - (fat_start + active * fat_secs) + fat_start + copy * fat_secs, 1, c->d);
}
/* every dirty sector of a kind on the disk; the FAT copy by copy, a disk flush between them */
static int flush_kind(int kind)
{
    int any = 0, r;
    if (kind == K_FAT) {
        u32 ncopies = mirror ? nfats : 1;
        for (u32 i = 0; i < NCACHE; i++) if (cache[i].valid && cache[i].dirty && cache[i].kind == K_FAT) any = 1;
        if (!any) return 0;
        for (u32 cp = 0; cp < ncopies; cp++) {
            for (u32 i = 0; i < NCACHE; i++)
                if (cache[i].valid && cache[i].dirty && cache[i].kind == K_FAT &&
                    (r = fat_write_one(&cache[i], mirror ? cp : active))) return r;
            if ((r = dflush())) return r;
        }
        for (u32 i = 0; i < NCACHE; i++) if (cache[i].valid && cache[i].kind == K_FAT) cache[i].dirty = 0;
        return 0;
    }
    for (u32 i = 0; i < NCACHE; i++)
        if (cache[i].valid && cache[i].dirty && cache[i].kind == kind) {
            if ((r = dio(DISK_WRITE, cache[i].lba, 1, cache[i].d))) return r;
            cache[i].dirty = 0;
        }
    return need_flush ? dflush() : 0;              /* direct writes count: what follows must not overtake them */
}
/* everything but the waiting frees, in the order the crash rules ask */
static int flush_all(void)
{
    int r;
    if ((r = flush_kind(K_DATA)) || (r = flush_kind(K_FAT)) || (r = flush_kind(K_DIR))) return r;
    return 0;
}

/* LBA in the cache; NOREAD: the caller overwrites all of it. 0 on an I/O error */
static struct cbuf *cget(u32 lba, int kind, int noread)
{
    struct cbuf *c = clast, *pick = 0;
    if (c && c->valid && c->lba == lba) { c->stamp = ++cstamp; if (!c->dirty) c->kind = (u8)kind; return c; }
    for (u32 i = 0; i < NCACHE; i++) {
        c = &cache[i];
        if (c->valid && c->lba == lba) {
            c->stamp = ++cstamp;
            if (!c->dirty) c->kind = (u8)kind;
            return clast = c;
        }
    }
    /* free or the oldest clean one, else the oldest dirty one */
    for (u32 i = 0; i < NCACHE; i++) {
        c = &cache[i];
        if (!c->valid) { pick = c; break; }
        if (!pick || (pick->dirty && !c->dirty) || (pick->dirty == c->dirty && c->stamp < pick->stamp)) pick = c;
    }
    if (pick->valid && pick->dirty) {             /* its turn in the order: what must come before it goes first */
        int r = 0;
        if (pick->kind == K_FAT) r = flush_kind(K_DATA);
        else if (pick->kind == K_DIR && !(r = flush_kind(K_DATA))) r = flush_kind(K_FAT);
        if (r || (r = flush_kind(pick->kind))) return 0;
    }
    pick->valid = 0;
    if (!noread && dio(DISK_READ, lba, 1, pick->d)) return 0;
    pick->lba = lba; pick->kind = (u8)kind; pick->dirty = 0; pick->valid = 1; pick->stamp = ++cstamp;
    return clast = pick;
}
/* before a transfer past the cache: WRITING drops the sectors it overwrites, reading saves the changed ones first */
static int cache_range(u32 lba, u32 n, int writing)
{
    for (u32 i = 0; i < NCACHE; i++) {
        struct cbuf *c = &cache[i];
        if (!c->valid || c->lba < lba || c->lba - lba >= n) continue;
        if (writing) c->valid = 0;
        else if (c->dirty) {
            if (dio(DISK_WRITE, c->lba, 1, c->d)) return -EIO;
            c->dirty = 0;
        }
    }
    return 0;
}

/* ---- the FAT ---- */
static u32 fat_base(void) { return fat_start + active * fat_secs; }

/* the raw value of entry CL (CL below nclus, or 1 for the mark). 0 or -EIO */
static int fat_raw(u32 cl, u32 *out)
{
    u32 off = fat_bits == 12 ? cl + cl / 2 : fat_bits == 16 ? cl * 2 : cl * 4;
    u32 s = fat_base() + (off >> 9), i = off & 511;
    struct cbuf *c = cget(s, K_FAT, 0);
    if (!c) return -EIO;
    if (fat_bits == 12) {
        u32 v = c->d[i];
        if (i == 511) { if (!(c = cget(s + 1, K_FAT, 0))) return -EIO; v |= (u32)c->d[0] << 8; }
        else v |= (u32)c->d[i + 1] << 8;
        *out = cl & 1 ? v >> 4 : v & 0xFFF;
    } else *out = fat_bits == 16 ? get16(c->d + i) : get32(c->d + i) & 0x0FFFFFFF;
    return 0;
}
static int fat_rawset(u32 cl, u32 v)
{
    u32 off = fat_bits == 12 ? cl + cl / 2 : fat_bits == 16 ? cl * 2 : cl * 4;
    u32 s = fat_base() + (off >> 9), i = off & 511;
    if (fat_bits == 12 && i == 511 && !cget(s + 1, K_FAT, 0)) return -EIO;   /* no half-changed entry if it cannot be read */
    struct cbuf *c = cget(s, K_FAT, 0);
    if (!c) return -EIO;
    if (fat_bits == 12) {
        u8 lo = cl & 1 ? (u8)((c->d[i] & 0x0F) | (v << 4)) : (u8)v;
        c->d[i] = lo; c->kind = K_FAT; c->dirty = 1;
        if (i == 511) { if (!(c = cget(s + 1, K_FAT, 0))) return -EIO; i = (u32)-1; }
        c->d[i + 1] = cl & 1 ? (u8)(v >> 4) : (u8)((c->d[i + 1] & 0xF0) | ((v >> 8) & 0x0F));
    } else if (fat_bits == 16) put16(c->d + i, v);
    else put32(c->d + i, (get32(c->d + i) & 0xF0000000u) | (v & 0x0FFFFFFF));   /* the top 4 bits are not ours */
    c->kind = K_FAT; c->dirty = 1;
    return 0;
}
/* the value of CL's entry: 0 free, EOC any end mark; bad clusters and reserved values come back as they are */
static int fat_get(u32 cl, u32 *out)
{
    u32 v;
    int r = fat_raw(cl, &v);
    if (r) return r;
    u32 eoc = fat_bits == 12 ? 0xFF8 : fat_bits == 16 ? 0xFFF8 : 0x0FFFFFF8;
    *out = v >= eoc ? EOC : v;
    return 0;
}
/* the cluster after CL in a chain: 0 at its end, -EIO if the chain is damaged */
static int chain_next(u32 cl, u32 *nx)
{
    u32 v;
    int r = fat_get(cl, &v);
    if (r) return r;
    if (v == EOC) { *nx = 0; return 0; }
    if (v < 2 || v >= nclus) return -EIO;
    u32 t;                                         /* a link to a free cluster: damage, or a flush a power cut tore */
    if ((r = fat_get(v, &t))) return r;
    if (!t) return -EIO;
    *nx = v;
    return 0;
}
static int fat_set(u32 cl, u32 v)
{
    if (v == EOC) v = fat_bits == 12 ? 0xFFF : fat_bits == 16 ? 0xFFFF : 0x0FFFFFFF;
    return fat_rawset(cl, v);
}

static u32 clus_lba(u32 cl) { return data_start + ((cl - 2) << (clus_shift - 9)); }

static int sync_all(void);
static void dm_forget(void);
/* a commit in the middle of a request: what the request changes after it still waits for the next one */
static int inner_commit(void)
{
    int r = sync_all();
    if (!r) dirty = 1;
    return r;
}
/* a free cluster, marked as the end of a chain; -ENOSPC when there is none */
static int alloc_clus(u32 *out)
{
    u32 total = nclus - 2;
    for (int again = 0; again < 2; again++) {
        u32 c = next_free < 2 || next_free >= nclus ? 2 : next_free;
        for (u32 i = 0; i < total; i++) {
            u32 v;
            int r = fat_raw(c, &v);
            if (r) return r;
            if (v == 0) {
                if ((r = fat_set(c, EOC))) return r;
                next_free = c + 1;
                if (free_cnt != 0xFFFFFFFFu && free_cnt) free_cnt--;
                info_dirty = 1;
                *out = c;
                return 0;
            }
            if (++c >= nclus) c = 2;
        }
        if (!npend) break;                        /* what is waiting to be freed comes free now */
        int r = inner_commit();
        if (r) return r;
    }
    return -ENOSPC;
}
/* a chain whose name is gone (or going, in the same commit) waits for the end of the commit */
static int chain_release(u32 first)
{
    if (first < 2 || first >= nclus || first == root_clus) return 0;   /* an entry that names the root: damage */
    if (npend == NPEND) { int r = inner_commit(); if (r) return r; }
    pend[npend++] = first;
    return 0;
}
static int apply_frees(void)
{
    dm_forget();
    for (u32 i = 0; i < npend; i++) {
        u32 c = pend[i];
        while (c >= 2 && c < nclus && c != root_clus) {   /* each step zeroes one: a loop in the chain ends at its own start */
            u32 v;
            int r = fat_get(c, &v);
            if (r) return r;
            if (v == 0) break;
            if ((r = fat_rawset(c, 0))) return r;
            if (free_cnt != 0xFFFFFFFFu) free_cnt++;
            info_dirty = 1;
            if (c < next_free) next_free = c;
            c = v;
        }
        pend[i] = 0;
    }
    npend = 0;
    return flush_kind(K_FAT);
}
static int sync_all(void)
{
    int r = flush_all();
    if (r) return r;
    if (npend && (r = apply_frees())) return r;
    dirty = 0;
    return 0;
}

/* The clean-shutdown bit of FAT[1] (FAT16, FAT32; FAT12 has none). Set (bit 1) is clean */
static u32 clean_bit(void) { return fat_bits == 16 ? 0x8000u : fat_bits == 32 ? 0x08000000u : 0; }
static int mark_set(int unclean)
{
    u32 v, b = clean_bit();
    int r;
    if (!b) { marked = unclean; return 0; }
    if ((r = fat_raw(1, &v))) return r;
    v = unclean ? v & ~b : v | b;
    if ((r = fat_rawset(1, v)) || (r = flush_kind(K_FAT))) return r;
    marked = unclean;
    return 0;
}
static int write_info(void)
{
    if (!info_dirty || fat_bits != 32 || !fsinfo_sec) { info_dirty = 0; return 0; }
    static u8 s[SECT];
    int r = dio(DISK_READ, fsinfo_sec, 1, s);
    if (r) return r;
    info_dirty = 0;
    if (get32(s) != 0x41615252u || get32(s + 484) != 0x61417272u || get32(s + 508) != 0xAA550000u) return 0;
    put32(s + 488, free_cnt);
    put32(s + 492, next_free < nclus ? next_free : 2);
    return dio(DISK_WRITE, fsinfo_sec, 1, s);
}
/* the volume is whole on the disk: say so */
static int go_clean(void)
{
    int r = sync_all();
    if (r) return r;
    if (!marked) return dflush();
    if ((r = write_info()) || (r = dflush())) return r;
    if ((r = mark_set(0))) return r;
    return dflush();
}
/* about to change something */
static int touch(void)
{
    if (readonly) return -EROFS;
    if (!marked) {
        int r = mark_set(1);
        if (r) return r;
        if ((r = dflush())) return r;
        marked = 1;
    }
    if (!dirty) { dirty = 1; dirty_since = sys_ticks(); }
    return 0;
}

/* ---- directories ---- */
/* the sector that holds byte OFF of directory DIR (0: the FAT12/16 root). 1, 0 past its end, or -errno. The walk
 * starts from the last place: reading a directory in order costs one step per cluster */
static u32 dm_dir = 0xFFFFFFFFu, dm_idx, dm_clu;
static int dir_lba(u32 dir, u32 off, u32 *lba)
{
    if (off >= DIR_MAX) return dir ? -EIO : 0;     /* longer than any directory: a loop in the chain */
    if (!dir) {
        if (off >= root_secs * SECT) return 0;
        *lba = root_start + (off >> 9);
        return 1;
    }
    u32 idx = off >> clus_shift, c = dir, i = 0;
    if (dm_dir == dir && idx >= dm_idx) { c = dm_clu; i = dm_idx; }
    while (i < idx) {
        u32 nx;
        int r = chain_next(c, &nx);
        if (r) return r;
        if (!nx) return 0;
        c = nx; i++;
    }
    dm_dir = dir; dm_idx = idx; dm_clu = c;
    *lba = clus_lba(c) + ((off >> 9) & (spc - 1));
    return 1;
}
static void dm_forget(void) { dm_dir = 0xFFFFFFFFu; }

/* VFAT's places of the 13 characters in a long-name entry */
static const u8 lfn_at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
static u16 lfn[262], q[262];                     /* the long name just read; the name asked for (UCS-2) */
static u32 lfn_len, q_len;

static u32 lfn_sum(const u8 *sh)
{
    u32 s = 0;
    for (int i = 0; i < 11; i++) s = (((s & 1) << 7) + (s >> 1) + sh[i]) & 0xFF;
    return s;
}

/* the next real entry of DIR at or after *COOKIE into N (its long name, if a good one, in lfn). 1, 0 at the end, -errno */
static int dir_next(u32 dir, u32 *cookie, node_t *n)
{
    u32 off = *cookie & ~31u, exp = 0, sum = 0, first = 0, lba;
    int ok = 0;
    for (;; off += 32) {
        int r = dir_lba(dir, off, &lba);
        if (r <= 0) return r;
        struct cbuf *c = cget(lba, K_DIR, 0);
        if (!c) return -EIO;
        const u8 *p = c->d + (off & 511);
        if (!p[0]) { *cookie = off; return 0; }
        if (p[0] == 0xE5) { ok = 0; continue; }
        if ((p[11] & 0x3F) == A_LFN) {
            u32 ord = p[0] & 0x3F;
            if (p[0] & 0x40) {
                if (!ord || ord > 20) { ok = 0; continue; }
                exp = ord; sum = p[13]; first = off; ok = 1;
                lfn_len = 0;
                for (u32 i = 0; i < 262; i++) lfn[i] = 0;
                lfn_len = ord * 13;
            } else if (!ok || !ord || ord != exp || p[13] != sum) { ok = 0; continue; }
            for (u32 i = 0; i < 13; i++) lfn[(ord - 1) * 13 + i] = (u16)get16(p + lfn_at[i]);
            exp = ord - 1;
            continue;
        }
        if (p[11] & A_VOL) { ok = 0; continue; }
        memcpy(n->e, p, 32);
        n->dir = dir; n->off = off; n->lba = lba; n->byte = (u16)(off & 511); n->isroot = 0;
        n->haslfn = ok && exp == 0 && lfn_sum(p) == sum;
        n->first = n->haslfn ? first : off;
        if (n->haslfn) {
            u32 k = 0;
            while (k < lfn_len && lfn[k] && lfn[k] != 0xFFFF) k++;
            lfn_len = k;
            if (!k || k > 255) n->haslfn = 0, n->first = off;   /* an empty or overlong name: as if none */
        }
        *cookie = off + 32;
        return 1;
    }
}

/* ---- names ---- */
/* the old names' codepage, CP437's upper half, as Unicode */
static const u16 cp437[128] = {
    0xC7,0xFC,0xE9,0xE2,0xE4,0xE0,0xE5,0xE7,0xEA,0xEB,0xE8,0xEF,0xEE,0xEC,0xC4,0xC5,
    0xC9,0xE6,0xC6,0xF4,0xF6,0xF2,0xFB,0xF9,0xFF,0xD6,0xDC,0xA2,0xA3,0xA5,0x20A7,0x192,
    0xE1,0xED,0xF3,0xFA,0xF1,0xD1,0xAA,0xBA,0xBF,0x2310,0xAC,0xBD,0xBC,0xA1,0xAB,0xBB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x3B1,0xDF,0x393,0x3C0,0x3A3,0x3C3,0xB5,0x3C4,0x3A6,0x398,0x3A9,0x3B4,0x221E,0x3C6,0x3B5,0x2229,
    0x2261,0xB1,0x2265,0x2264,0x2320,0x2321,0xF7,0x2248,0xB0,0x2219,0xB7,0x221A,0x207F,0xB2,0x25A0,0xA0 };

static u32 fold(u32 c) { return u16_fold(c); }
/* NAME (LEN bytes of UTF-8) into q. 0, -EILSEQ, -ENAMETOOLONG */
static int name_in(const char *name, u32 len)
{
    int n = u16_from_utf8(name, len, q, 255);
    if (n < 0) return n;
    q_len = (u32)n;
    return 0;
}
/* LEN units of UCS-2/UTF-16 as UTF-8 in OUT (CAP bytes with the NUL). The length, or -1: not a good name (a control
 * character, '/', a broken surrogate pair) or too long */
static int name_out(const u16 *u, u32 len, char *out, u32 cap) { return u16_to_utf8(u, len, out, cap, 0); }
/* the 8.3 name of a short entry as UCS-2 (at most 12 units); LOW: with its lower-case bits */
static u32 short_units(const u8 *e, u16 *out, int low)
{
    u32 n = 0, b = 8, x = 3;
    while (b && e[b - 1] == ' ') b--;
    while (x && e[8 + x - 1] == ' ') x--;
    for (u32 i = 0; i < b + x; i++) {
        if (i == b) out[n++] = '.';
        u32 c = i < b ? e[i] : e[8 + i - b];
        if (i == 0 && c == 0x05) c = 0xE5;           /* the format's stand-in for a real 0xE5 */
        if (c >= 0x80) c = cp437[c - 0x80];
        else if (c < 0x20 || c == '/') c = '_';      /* damage: a name that can still be typed */
        else if (low && c >= 'A' && c <= 'Z' && (e[12] & (i < b ? 0x08 : 0x10))) c += 32;
        out[n++] = (u16)c;
    }
    if (!n) out[n++] = '_';                          /* damage: all spaces */
    return n;
}
static int same_name(const u16 *a, u32 n)
{
    if (n != q_len) return 0;
    for (u32 i = 0; i < n; i++) if (a[i] != q[i] && fold(a[i]) != fold(q[i])) return 0;
    return 1;
}

/* the entry named q in DIR. 0, -ENOENT, -errno */
static int dir_find(u32 dir, node_t *n)
{
    u32 cookie = 0;
    u16 s[13];
    for (;;) {
        int r = dir_next(dir, &cookie, n);
        if (r < 0) return r;
        if (!r) return -ENOENT;
        if (n->haslfn && same_name(lfn, lfn_len)) return 0;
        if (same_name(s, short_units(n->e, s, 0))) return 0;
    }
}

static u32 first_of(const u8 *e)
{
    return get16(e + 26) | (fat_bits == 32 ? get16(e + 20) << 16 : 0);
}
static void set_first(u8 *e, u32 c)
{
    put16(e + 26, c);
    put16(e + 20, fat_bits == 32 ? c >> 16 : 0);
}
static u32 root_dir(void) { return fat_bits == 32 ? root_clus : 0; }

/* the entry at N's place in the cache, marked changed; 0 on an I/O error */
static u8 *entry_w(u32 lba, u32 byte)
{
    struct cbuf *c = cget(lba, K_DIR, 0);
    if (!c) return 0;
    c->kind = K_DIR; c->dirty = 1;
    return c->d + byte;
}

/* ---- time: FAT's fields, read as UTC ---- */
static u32 fat_to_unix(u32 d, u32 t)
{
    u32 y = 1980 + (d >> 9), m = (d >> 5) & 15, day = d & 31;
    if (m < 1 || m > 12 || !day) return 0;
    u32 yy = m <= 2 ? y - 1 : y, era = yy / 400, yoe = yy - era * 400;
    u32 doy = (153 * (m + (m > 2 ? (u32)-3 : 9)) + 2) / 5 + day - 1;
    u32 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    u32 days = era * 146097 + doe - 719468;
    return days * 86400 + (t >> 11) * 3600 + ((t >> 5) & 63) * 60 + (t & 31) * 2;
}
static void unix_to_fat(u32 now, u32 *d, u32 *t)
{
    if (now < 315532800u) { *d = 0x21; *t = 0; return; }   /* before 1980: the format's first day */
    u32 z = now / 86400 + 719468, era = z / 146097, doe = z - era * 146097;
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, y = yoe + era * 400;
    u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    u32 day = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    if (y > 2107) { *d = 0xFF9F; *t = 0xBF7D; return; }
    *d = (y - 1980) << 9 | m << 5 | day;
    u32 s = now % 86400;
    *t = (s / 3600) << 11 | ((s / 60) % 60) << 5 | (s % 60) / 2;
}
static void stamp(u8 *e, u32 now)
{
    u32 d, t;
    unix_to_fat(now, &d, &t);
    put16(e + 22, t); put16(e + 24, d);
}

/* ---- open files ---- */
struct ofile {
    u32 ino, count;
    u32 first, size, mtime;            /* first cluster 0: empty */
    u32 lba;                           /* its entry */
    u16 byte;
    u8 attr, orphan;                   /* orphan: its name is gone; its chain goes at the last close */
    u8 stale, pad;                     /* stale: opened before this server's start */
    u32 pidx, pclu;                    /* the place of the last access: reading in order is one step per cluster */
};
static struct ofile opens[NOPEN];
static u32 synth;

static struct ofile *slot_ino(u32 ino)
{
    for (u32 i = 0; i < NOPEN; i++) if (opens[i].count && opens[i].ino == ino) return &opens[i];
    return 0;
}
static struct ofile *slot_at(u32 lba, u32 byte)
{
    for (u32 i = 0; i < NOPEN; i++)
        if (opens[i].count && !opens[i].stale && !opens[i].orphan && opens[i].lba == lba && opens[i].byte == byte) return &opens[i];
    return 0;
}
/* a file's number: an open one keeps what it has; else its place, unless a slot holds that number for another file */
static u32 file_ino(const node_t *n)
{
    struct ofile *o = slot_at(n->lba, n->byte);
    if (o) return o->ino;
    u32 v = ((n->lba << 4) | (n->byte >> 5)) & 0x3FFFFFFFu;
    if (v < 16) v += 16;                          /* wrapped on a volume over 32 GB: never the root's number */
    while (slot_ino(v)) v = 0x60000000u | (++synth & 0x0FFFFFFF);
    return v;
}
static int is_dir(const node_t *n) { return n->isroot || (n->e[11] & A_DIR); }
/* the directory N is: its first cluster as a DIR value */
static int dir_of(const node_t *n, u32 *dir)
{
    if (n->isroot) { *dir = root_dir(); return 0; }
    u32 c = first_of(n->e);
    if (!c && n->e[0] == '.' && n->e[1] == '.') { *dir = root_dir(); return 0; }   /* ".." of a directory in the root */
    if (c < 2 || c >= nclus) return -EIO;
    *dir = c;
    return 0;
}
static u32 dir_ino(u32 dir) { return !dir || dir == root_clus ? ROOT : DINO(dir); }
/* INO as a directory: stateless. A cluster that does not start with its own "." is stale. A handle to a removed
 * directory may still list it, or the directory that took its cluster: reading only, paths on a mount are text */
static int ino_dir(u32 ino, u32 *dir)
{
    if (ino == ROOT) { *dir = root_dir(); return 0; }
    if ((ino & 0xF0000000u) != 0x40000000u) return (ino & 0xF0000000u) == 0x60000000u || ino < 0x40000000u ? -ENOTDIR : -ESTALE;
    u32 c = ino & 0x0FFFFFFF;
    if (c < 2 || c >= nclus) return -ESTALE;
    struct cbuf *b = cget(clus_lba(c), K_DIR, 0);
    if (!b) return -EIO;
    if (b->d[0] != '.' || b->d[1] != ' ' || !(b->d[11] & A_DIR) || first_of(b->d) != c) return -ESTALE;
    *dir = c;
    return 0;
}

static unsigned file_mode(u32 attr) { return S_IFREG | (attr & A_RO ? m_fmode & ~0222u : m_fmode); }
static void fill_node(const node_t *n, struct ustat *st)
{
    u32 d;
    if (is_dir(n)) {
        st->ino = dir_of(n, &d) ? 0 : dir_ino(d);
        st->mode = S_IFDIR | m_dmode; st->size = 1u << clus_shift; st->links = 2;
    } else {
        struct ofile *o = slot_at(n->lba, n->byte);
        st->ino = file_ino(n); st->mode = file_mode(o ? o->attr : n->e[11]); st->links = 1;
        st->size = o ? o->size : get32(n->e + 28);
    }
    st->mtime = n->isroot ? 0 : fat_to_unix(get16(n->e + 24), get16(n->e + 22));
    st->uid = m_uid; st->gid = m_gid;
}

/* ---- paths ---- */
static int dmode_ok(int want) { return fsp_access(cuid, cgid, cng, cgs, m_dmode, m_uid, m_gid, want); }
/* walk PATH's first LEN bytes from directory CWD; the end in N. No links on FAT */
static int walk(u32 cwd, const char *path, u32 len, node_t *n)
{
    u32 i = 0;
    memset(n, 0, sizeof *n);
    n->isroot = 1;
    if (path[0] != '/') {
        u32 d;
        int r = ino_dir(cwd, &d);
        if (r) return r;
        if (d != root_dir()) { n->isroot = 0; n->e[11] = A_DIR; set_first(n->e, d); }
    }
    while (i < len) {
        while (i < len && path[i] == '/') i++;
        if (i >= len) break;
        u32 s = i, d;
        while (i < len && path[i] != '/') i++;
        int r;
        if (!is_dir(n)) return -ENOTDIR;
        if ((r = dir_of(n, &d))) return r;
        if (!dmode_ok(FSP_X)) return -EACCES;
        if (i - s == 1 && path[s] == '.') continue;
        if (!d && i - s == 2 && path[s] == '.' && path[s + 1] == '.') continue;   /* the root's has none */
        if (d == root_clus && fat_bits == 32 && i - s == 2 && path[s] == '.' && path[s + 1] == '.') continue;
        if ((r = name_in(path + s, i - s))) return r == -EILSEQ ? -ENOENT : r;
        if ((r = dir_find(d, n))) return r;
        n->isroot = 0;
    }
    return 0;
}
static int namei(u32 cwd, const char *path, node_t *n) { return walk(cwd, path, strlen(path), n); }

/* the directory that holds PATH's last name, and that name in q */
static int parent(u32 cwd, const char *path, u32 *dir)
{
    u32 n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    u32 s = n;
    while (s > 0 && path[s - 1] != '/') s--;
    if (n == s || (n - s == 1 && path[s] == '.') || (n - s == 2 && path[s] == '.' && path[s + 1] == '.')) return -EINVAL;
    static node_t pn;
    int r = walk(cwd, path, s, &pn);
    if (r) return r;
    if (!is_dir(&pn)) return -ENOTDIR;
    if ((r = dir_of(&pn, dir))) return r;
    return name_in(path + s, n - s);
}
static int dir_may(void) { return fsp_dir_modify(cuid, cgid, cng, cgs, m_dmode, m_uid, m_gid); }

/* ---- making and removing entries ---- */
/* what FAT cannot name (q): Windows could not open it */
static int name_bad(void)
{
    if (!q_len) return -EINVAL;
    for (u32 i = 0; i < q_len; i++) {
        u32 c = q[i];
        if (c < 0x20 || c == 0x7F || c == 0xFFFF || (c < 0x80 && strchr("\"*/:<>?\\|", (int)c))) return -EINVAL;
    }
    if (q[q_len - 1] == '.' || q[q_len - 1] == ' ') return -EINVAL;
    return 0;
}
/* does DIR have an entry with the 11 bytes SH? 1, 0, -errno */
static int short_exists(u32 dir, const u8 *sh)
{
    u32 cookie = 0;
    static node_t t;
    for (;;) {
        int r = dir_next(dir, &cookie, &t);
        if (r <= 0) return r;
        if (!memcmp(t.e, sh, 11)) return 1;
    }
}
/* the 8.3 alias of q in SH; 1 if q can stand without a long name (all capitals and it fits). -errno */
static int make_short(u32 dir, u8 *sh)
{
    u8 base[8], ext[3];
    u32 nb = 0, nx = 0, dot = q_len, lossy = 0, upper = 1;
    for (u32 i = 0; i < q_len; i++) if (q[i] == '.') dot = i;
    for (u32 i = 0; i < q_len; i++) {
        u32 c = q[i];
        if (i == dot) continue;
        if (c >= 'a' && c <= 'z') { upper = 0; c -= 32; }
        else if (c == ' ' || c == '.') { lossy = 1; continue; }
        else if (c >= 0x80 || strchr("+,;=[]", (int)c)) { lossy = 1; c = '_'; }
        if (i < dot) { if (nb < 8) base[nb++] = (u8)c; else lossy = 1; }
        else { if (nx < 3) ext[nx++] = (u8)c; else lossy = 1; }
    }
    if (!nb) { base[nb++] = '_'; lossy = 1; }
    if (q[0] == '.') lossy = 1;
    memset(sh, ' ', 11);
    memcpy(sh, base, nb);
    memcpy(sh + 8, ext, nx);
    int r;
    if (!lossy) {
        if ((r = short_exists(dir, sh)) < 0) return r;
        if (!r) return upper;
    }
    /* NAME~1 .. NAME~9, then two letters, a number and ~N, as other systems do */
    for (u32 k = 1; k < 512; k++) {
        char t[8];
        u32 len = 0, pre;
        if (k < 10) { t[0] = '~'; t[1] = (char)('0' + k); len = 2; pre = nb < 6 ? nb : 6; }
        else {
            pre = nb < 2 ? nb : 2;
            for (u32 s = 12, v = k * 2729; ; s -= 4) { t[len++] = "0123456789ABCDEF"[(v >> s) & 15]; if (!s) break; }
            t[len++] = '~'; t[len++] = '1';
        }
        memset(sh, ' ', 8);
        memcpy(sh, base, pre);
        memcpy(sh + pre, t, len);
        if ((r = short_exists(dir, sh)) < 0) return r;
        if (!r) return 0;
    }
    return -ENOSPC;
}
/* a new cluster, zeroed on the disk, at the end of the chain whose last cluster is TAIL */
static int dir_grow(u32 tail)
{
    u32 c;
    static u8 zero[SECT];
    int r = alloc_clus(&c);
    if (r) return r;
    u32 lba = clus_lba(c);
    if ((r = cache_range(lba, spc, 1))) return r;
    for (u32 i = 0; i < spc; i++) if ((r = dio(DISK_WRITE, lba + i, 1, zero))) return r;
    return fat_set(tail, c);
}
/* room for COUNT entries in a row in DIR: *OFF their place. The directory grows if it must. */
static int dir_room(u32 dir, u32 count, u32 *where)
{
    u32 off = 0, run = 0, lba, tail = dir;
    int end = 0;                                  /* past the end marker everything is free, whatever is there */
    for (;; off += 32) {
        int r = dir_lba(dir, off, &lba);
        if (r < 0) return r == -EIO && off >= DIR_MAX ? -ENOSPC : r;
        if (!r) {                                 /* past the end: a new cluster, or a full FAT12/16 root */
            if (!dir) return -ENOSPC;
            u32 c = dir, nx;
            for (u32 i = 0; i < (DIR_MAX >> 9); i++) {
                if ((r = chain_next(c, &nx))) return r;
                if (!nx) break;
                c = nx;
            }
            tail = c;
            if ((r = dir_grow(tail))) return r;
            dm_forget();
            off -= 32;
            continue;
        }
        struct cbuf *c = cget(lba, K_DIR, 0);
        if (!c) return -EIO;
        u8 b = c->d[off & 511];
        if (!b) end = 1;
        if (b == 0xE5 || end) {
            if (!run) *where = off;
            if (++run == count) return 0;
        } else run = 0;
    }
}
/* the entries for the name in q, with the body BODY (attr, times, first cluster, size). The place into N. */
static int entry_add(u32 dir, const u8 *body, node_t *n)
{
    u8 sh[11];
    int r = name_bad();
    if (r) return r;
    int plain = make_short(dir, sh);
    if (plain < 0) return plain;
    u32 parts = plain ? 0 : (q_len + 12) / 13, off;
    if ((r = dir_room(dir, parts + 1, &off))) return r;
    u32 lba, sum = lfn_sum(sh), o = off;
    int was_end = 0;
    n->first = off;
    for (u32 p = parts; p; p--, o += 32) {
        if ((r = dir_lba(dir, o, &lba)) <= 0) return r ? r : -EIO;
        u8 *e = entry_w(lba, o & 511);
        if (!e) return -EIO;
        if (!e[0]) was_end = 1;
        memset(e, 0, 32);
        e[0] = (u8)(p | (p == parts ? 0x40 : 0));
        e[11] = A_LFN; e[13] = (u8)sum;
        u32 base = (p - 1) * 13;
        for (u32 i = 0; i < 13; i++) {
            u32 at = base + i;
            put16(e + lfn_at[i], at < q_len ? q[at] : at == q_len ? 0 : 0xFFFF);
        }
    }
    if ((r = dir_lba(dir, o, &lba)) <= 0) return r ? r : -EIO;
    struct cbuf *cb = cget(lba, K_DIR, 0);
    if (!cb) return -EIO;
    if (was_end || !cb->d[o & 511]) {             /* what follows was "free to the end": keep that true */
        u32 l2;
        r = dir_lba(dir, o + 32, &l2);
        if (r < 0 && o + 32 < DIR_MAX) return r;
        if (r > 0) {
            u8 *z = entry_w(l2, (o + 32) & 511);
            if (!z) return -EIO;
            z[0] = 0;
        }
    }
    /* the last step, which cannot fail after it: a caller that fails frees what the entry would name */
    u8 *e = entry_w(lba, o & 511);
    if (!e) return -EIO;
    memcpy(e, body, 32);
    memcpy(e, sh, 11);
    e[12] = 0;                                    /* the lower-case bits belonged to the old short name */
    memcpy(n->e, e, 32);
    n->dir = dir; n->off = o; n->lba = lba; n->byte = (u16)(o & 511); n->isroot = 0; n->haslfn = !plain;
    return 0;
}
/* the entries of N go: the long-name parts and the short entry */
static int entry_del(const node_t *n)
{
    for (u32 o = n->first; o <= n->off; o += 32) {
        u32 lba;
        int r = dir_lba(n->dir, o, &lba);
        if (r <= 0) return r ? r : -EIO;
        u8 *e = entry_w(lba, o & 511);
        if (!e) return -EIO;
        e[0] = 0xE5;
    }
    return 0;
}
static void new_body(u8 *b, u32 attr, u32 first)
{
    memset(b, 0, 32);
    b[11] = (u8)attr;
    set_first(b, first);
    u32 now = (u32)sys_time(), d, t;
    unix_to_fat(now, &d, &t);
    put16(b + 14, t); put16(b + 16, d); put16(b + 18, d); put16(b + 22, t); put16(b + 24, d);
}
/* an open file's entry from its slot */
static int entry_update(struct ofile *o)
{
    if (o->orphan) return 0;
    u8 *e = entry_w(o->lba, o->byte);
    if (!e) return -EIO;
    if (!e[0] || e[0] == 0xE5 || (e[11] & (A_DIR | A_VOL))) return -EIO;
    set_first(e, o->first);
    put32(e + 28, o->size);
    e[11] = (u8)o->attr;
    stamp(e, o->mtime);
    return 0;
}

/* ---- file data ---- */
/* the cluster holding byte OFF of O. GROW: the chain grows to reach it. *CL 0: past the end */
static int file_clus(struct ofile *o, u32 off, int grow, u32 *cl)
{
    u32 idx = off >> clus_shift, c = o->first, i = 0;
    int r;
    if (!c) {
        if (!grow) { *cl = 0; return 0; }
        if ((r = alloc_clus(&c))) return r;
        o->first = c;
    } else if (c < 2 || c >= nclus) return -EIO;
    if (o->pclu && idx >= o->pidx) { c = o->pclu; i = o->pidx; }
    while (i < idx) {
        u32 nx;
        if ((r = chain_next(c, &nx))) return r;
        if (!nx) {
            if (!grow) { *cl = 0; return 0; }
            if ((r = alloc_clus(&nx)) || (r = fat_set(c, nx))) return r;
        }
        c = nx; i++;
    }
    o->pidx = idx; o->pclu = c;
    *cl = c;
    return 0;
}
static int file_read(struct ofile *o, u32 off, u8 *buf, u32 n)
{
    u32 done = 0, cbytes = 1u << clus_shift;
    if (off >= o->size) return 0;
    if (n > o->size - off) n = o->size - off;
    while (done < n) {
        u32 c;
        int r = file_clus(o, off, 0, &c);
        if (r) return done ? (int)done : r;
        if (!c) return done ? (int)done : -EIO;       /* the chain is shorter than the file's size */
        u32 inclu = off & (cbytes - 1), lba = clus_lba(c) + (inclu >> 9), byte = off & 511;
        u32 left = n - done;
        if (!byte && left >= SECT && !((u32)(buf + done) & 1)) {       /* whole sectors go straight to the caller */
            u32 secs = (cbytes - inclu) >> 9, last = c;
            while (secs < left >> 9) {                /* contiguous clusters are one transfer */
                u32 nx;
                if (chain_next(last, &nx) || nx != last + 1) break;
                last = nx;
                secs += spc;
            }
            if (secs > left >> 9) secs = left >> 9;
            if ((r = cache_range(lba, secs, 0)) || (r = dio(DISK_READ, lba, secs, buf + done))) return done ? (int)done : r;
            done += secs << 9; off += secs << 9;
            continue;
        }
        struct cbuf *b = cget(lba, K_DATA, 0);
        if (!b) return done ? (int)done : -EIO;
        u32 k = SECT - byte < left ? SECT - byte : left;
        memcpy(buf + done, b->d + byte, k);
        done += k; off += k;
    }
    return (int)done;
}
/* BUF 0: zeros */
static int file_put(struct ofile *o, u32 off, const u8 *buf, u32 n)
{
    u32 done = 0, cbytes = 1u << clus_shift;
    while (done < n) {
        u32 c;
        int r = file_clus(o, off, 1, &c);
        if (r) return done ? (int)done : r;
        u32 inclu = off & (cbytes - 1), lba = clus_lba(c) + (inclu >> 9), byte = off & 511, left = n - done;
        if (buf && !byte && left >= SECT && !((u32)(buf + done) & 1)) {
            u32 secs = (cbytes - inclu) >> 9;
            if (secs > left >> 9) secs = left >> 9;
            if ((r = cache_range(lba, secs, 1)) || (r = dio(DISK_WRITE, lba, secs, (void *)(buf + done)))) return done ? (int)done : r;
            done += secs << 9; off += secs << 9;
        } else {
            u32 k = SECT - byte < left ? SECT - byte : left;
            /* a sector past the file's old end has nothing to keep */
            struct cbuf *b = cget(lba, K_DATA, !byte && k == SECT);
            if (!b) return done ? (int)done : -EIO;
            if (buf) memcpy(b->d + byte, buf + done, k);
            else memset(b->d + byte, 0, k);
            b->kind = K_DATA; b->dirty = 1;
            done += k; off += k;
        }
    }
    return (int)done;
}
static int file_write(struct ofile *o, u32 off, const u8 *buf, u32 n)
{
    int r;
    if (!n) return 0;
    if (n > 0xFFFFFFFFu - off) return -EFBIG;
    if ((r = touch())) return r;
    if (off > o->size) {                          /* no holes in FAT: the gap is written as zeros */
        if (off - o->size >= 0x40000000u) return -EFBIG;
        r = file_put(o, o->size, 0, off - o->size);
        if (r > 0) { o->size += (u32)r; }
        if (r < 0 || o->size != off) { o->mtime = (u32)sys_time(); entry_update(o); return r < 0 ? r : -ENOSPC; }
    }
    r = file_put(o, off, buf, n);
    if (r > 0 && off + (u32)r > o->size) o->size = off + (u32)r;
    o->mtime = (u32)sys_time();
    o->attr |= A_ARC;
    int e = entry_update(o);
    return e ? e : r;
}
/* O_TRUNC: the entry first, its chain after that entry is on the disk */
static int file_trunc(struct ofile *o)
{
    int r = touch();
    if (r) return r;
    u32 first = o->first;
    o->first = 0; o->size = 0; o->pclu = 0; o->pidx = 0;
    o->mtime = (u32)sys_time();
    if ((r = entry_update(o))) return r;
    return chain_release(first);
}

/* ---- requests ---- */
/* a file's chain at its first open: in range, and no loop (a mark moved to every 2nd, 4th, 8th ... step; a walk bounded
 * by the file's size would not notice one). A chain over CHECK_MAX clusters is walked no further here: its reads and
 * writes stay bounded by the size. Keeps a normal open cheap: a 16 MB file at 4 KB clusters is 8 sectors of FAT32 */
#define CHECK_MAX 16384
static int chain_check(u32 c)
{
    u32 mark = 0, left = 1, span = 1;
    if (!c) return 0;
    if (c < 2 || c >= nclus) return -EIO;
    for (u32 i = 0; i < CHECK_MAX; i++) {
        u32 nx;
        int r = chain_next(c, &nx);
        if (r) return r;
        if (!nx) return 0;
        if (nx == mark) return -EIO;
        if (!--left) { span *= 2; left = span; mark = nx; }
        c = nx;
    }
    return 0;
}
static int slot_new(const node_t *n, struct ofile **out)
{
    struct ofile *o = slot_at(n->lba, n->byte);
    if (o) { o->count++; *out = o; return 0; }
    u32 ino = file_ino(n);
    for (u32 i = 0; i < NOPEN; i++) {
        o = &opens[i];
        if (o->count) continue;
        memset(o, 0, sizeof *o);
        o->ino = ino; o->count = 1;
        o->first = first_of(n->e); o->size = get32(n->e + 28);
        o->mtime = fat_to_unix(get16(n->e + 24), get16(n->e + 22));
        o->lba = n->lba; o->byte = n->byte; o->attr = n->e[11];
        *out = o;
        return 0;
    }
    return -ENFILE;
}
static int slot_get(u32 ino, struct ofile **out)
{
    struct ofile *o = slot_ino(ino);
    if (!o || o->stale) return -ESTALE;
    *out = o;
    return 0;
}

static int do_open(struct umsg *m)
{
    static node_t n;
    const char *path = (const char *)m->a[0];
    u32 flags = m->a[2], dir;
    int writing = (flags & (O_WRONLY | O_RDWR)) != 0, r = namei(m->a[1], path, &n);
    struct ofile *o;
    if (r == 0 && (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) return -EEXIST;
    if (r) {
        if (r != -ENOENT || !(flags & O_CREAT)) return r;
        if ((r = parent(m->a[1], path, &dir))) return r;
        if (!dir_may()) return -EACCES;
        if ((r = touch())) return r;
        u8 b[32];
        new_body(b, A_ARC, 0);
        if ((r = entry_add(dir, b, &n))) return r;
    } else if (is_dir(&n)) {
        if (writing) return -EISDIR;
        if (!fsp_access(cuid, cgid, cng, cgs, m_dmode, m_uid, m_gid, fsp_open_want(flags))) return -EACCES;
        if ((r = dir_of(&n, &dir))) return r;
        fill_node(&n, (struct ustat *)m->a[3]);
        return (int)((struct ustat *)m->a[3])->ino;
    } else {
        if (!fsp_access(cuid, cgid, cng, cgs, file_mode(n.e[11]), m_uid, m_gid, fsp_open_want(flags))) return -EACCES;
        if (writing && readonly) return -EROFS;
        if (!slot_at(n.lba, n.byte) && !(writing && (flags & O_TRUNC)) && (r = chain_check(first_of(n.e)))) return r;
    }
    if ((r = slot_new(&n, &o))) return r;
    if (writing && (flags & O_TRUNC) && (o->size || o->first)) {
        if ((r = file_trunc(o))) { o->count--; return r; }
    }
    struct ustat *st = (struct ustat *)m->a[3];
    fill_node(&n, st);
    st->ino = o->ino; st->size = o->size; st->mode = file_mode(o->attr); st->mtime = o->mtime;
    return (int)o->ino;
}

static int do_readdir(struct umsg *m)
{
    static node_t n;
    struct udirent *ud = (struct udirent *)m->a[2];
    u32 cookie = m->a[1], d;
    u16 s[13];
    int r = ino_dir(m->a[0], &d);
    if (r) return r;
    r = dir_next(d, &cookie, &n);
    if (r > 0) {
        int k = n.haslfn ? name_out(lfn, lfn_len, ud->name, sizeof ud->name) : -1;
        if (k < 0) name_out(s, short_units(n.e, s, 1), ud->name, sizeof ud->name);
        ud->type = is_dir(&n) ? 2 : 1;
        if (!is_dir(&n)) ud->ino = file_ino(&n);
        else { u32 x; ud->ino = dir_of(&n, &x) ? 0 : dir_ino(x); }
    }
    *(u32 *)m->a[3] = cookie;
    return r;
}

/* is DIR (a cluster) inside the directory SRC, or SRC itself? Walks up through ".." */
static int inside(u32 dir, u32 src)
{
    static node_t t;
    for (u32 i = 0; i < 4096; i++) {
        if (dir == src) return 1;
        if (!dir || dir == root_clus) return 0;
        u32 c = 32;                               /* ".." is the second entry */
        struct cbuf *b;
        u32 lba;
        int r = dir_lba(dir, c, &lba);
        if (r <= 0) return r ? r : -EIO;
        if (!(b = cget(lba, K_DIR, 0))) return -EIO;
        memcpy(t.e, b->d + 32, 32);
        if (t.e[0] != '.' || t.e[1] != '.') return -EIO;
        u32 up = first_of(t.e);
        if (up && (up < 2 || up >= nclus)) return -EIO;
        dir = up ? up : root_dir();
    }
    return -EIO;
}
/* a moved directory's ".." */
static int set_dotdot(u32 cl, u32 newdir)
{
    u32 lba;
    int r = dir_lba(cl, 32, &lba);
    if (r <= 0) return r ? r : -EIO;
    u8 *e = entry_w(lba, 32);
    if (!e) return -EIO;
    if (e[0] != '.' || e[1] != '.') return -EIO;
    set_first(e, newdir == root_dir() ? 0 : newdir);
    return 0;
}
static int dir_empty(u32 d)
{
    static node_t t;
    u32 cookie = 0;
    for (;;) {
        int r = dir_next(d, &cookie, &t);
        if (r <= 0) return r < 0 ? r : 0;
        if (t.e[0] == '.' && (t.e[1] == ' ' || (t.e[1] == '.' && t.e[2] == ' '))) continue;
        return -ENOTEMPTY;
    }
}

/* a new name first, then the old one goes: a power cut leaves the file under one of them, or both */
static int do_rename(struct umsg *m)
{
    static node_t a, b, nw;
    u32 da, db, sc = 0;
    int r;
    if ((r = namei(m->a[2], (const char *)m->a[0], &a))) return r;
    if (a.isroot || !a.lba) return -EBUSY;        /* the root, or the working directory given as "." */
    if (a.e[0] == '.' && (a.e[1] == ' ' || a.e[1] == '.')) return -EINVAL;
    if ((r = parent(m->a[2], (const char *)m->a[1], &db))) return r == -EINVAL ? -EBUSY : r;
    if (!dir_may()) return -EACCES;
    int src_dir = is_dir(&a);
    if (src_dir) {
        if ((r = dir_of(&a, &sc))) return r;
        if ((r = inside(db, sc)) < 0) return r;
        if (r) return -EINVAL;
    }
    da = a.dir;
    r = dir_find(db, &b);
    if (r && r != -ENOENT) return r;
    int exists = r == 0;
    if ((r = touch())) return r;
    struct ofile *so = src_dir ? 0 : slot_at(a.lba, a.byte);
    if (exists && b.lba == a.lba && b.byte == a.byte) {     /* the same file */
        u16 s[13];
        if ((b.haslfn && lfn_len == q_len && !memcmp(lfn, q, q_len * 2)) ||
            (!b.haslfn && short_units(b.e, s, 1) == q_len && !memcmp(s, q, q_len * 2))) return 0;
        exists = 0;                               /* only the case changes: a new name, the old one goes */
    }
    if (exists) {                                 /* in place: one 32-byte write replaces the file */
        if (is_dir(&b) && !src_dir) return -EISDIR;
        if (!is_dir(&b) && src_dir) return -ENOTDIR;
        u32 old = first_of(b.e), tdir;
        if (src_dir) {
            if ((r = dir_of(&b, &tdir))) return r;
            if ((r = dir_empty(tdir))) return r;
            old = tdir;
        }
        struct ofile *to = src_dir ? 0 : slot_at(b.lba, b.byte);
        u8 *e = entry_w(b.lba, b.byte);
        if (!e) return -EIO;
        u32 srcfirst = src_dir ? sc : so ? so->first : first_of(a.e);
        e[11] = a.e[11];                          /* e[12]: the lower-case bits stay with this short name */
        memcpy(e + 13, a.e + 13, 19);
        if (so) { set_first(e, so->first); put32(e + 28, so->size); }
        if (to) to->orphan = 1;                   /* the old file stays readable through its handle */
        /* two names on one chain, as a power cut in an earlier rename leaves: the chain is not going anywhere */
        else if (old != srcfirst && (r = chain_release(old))) return r;
        if (to && old == srcfirst) to->first = 0;
        nw = b;
    } else {
        u8 body[32];
        memcpy(body, a.e, 32);
        if (so) { set_first(body, so->first); put32(body + 28, so->size); }
        if ((r = entry_add(db, body, &nw))) return r;
    }
    if (src_dir && da != db && (r = set_dotdot(sc, db))) return r;
    if ((r = flush_all())) return r;              /* the new name is on the disk */
    if ((r = entry_del(&a))) return r;
    if (so) { so->lba = nw.lba; so->byte = nw.byte; }
    return flush_kind(K_DIR);                     /* and the old one gone: the time with two names is short */
}

static int do_mkdir(struct umsg *m)
{
    static node_t n;
    u32 dir, c;
    int r = parent(m->a[1], (const char *)m->a[0], &dir);
    if (r) return r == -EINVAL ? -EEXIST : r;
    if (!dir_may()) return -EACCES;
    /* name_in's q must survive dir_find here: it is the name asked for */
    r = dir_find(dir, &n);
    if (!r) return -EEXIST;
    if (r != -ENOENT) return r;
    if ((r = touch()) || (r = name_bad()) || (r = alloc_clus(&c))) return r;
    u32 lba = clus_lba(c);
    static u8 s[SECT];
    if ((r = cache_range(lba, spc, 1))) return r;
    u8 b[32];
    new_body(b, A_DIR, 0);
    memset(s, 0, sizeof s);
    for (u32 i = 1; i < spc; i++) if ((r = dio(DISK_WRITE, lba + i, 1, s))) goto bad;
    memcpy(s, b, 32);
    memset(s, ' ', 11); s[0] = '.';
    set_first(s, c);
    memcpy(s + 32, b, 32);
    memset(s + 32, ' ', 11); s[32] = s[33] = '.';
    set_first(s + 32, dir == root_dir() ? 0 : dir);
    if ((r = dio(DISK_WRITE, lba, 1, s))) goto bad;
    set_first(b, c);
    if ((r = entry_add(dir, b, &n))) goto bad;
    return 0;
bad:
    chain_release(c);
    return r;
}

static int count_free(void)
{
    if (free_cnt != 0xFFFFFFFFu) return 0;
    u32 k = 0;
    for (u32 c = 2; c < nclus; c++) {
        u32 v;
        int r = fat_raw(c, &v);
        if (r) return r;
        if (!v) k++;
    }
    free_cnt = k;
    info_dirty = 1;
    return 0;
}

static int handle1(struct umsg *m)
{
    static node_t n;
    struct ofile *o;
    u32 dir;
    int r;
    switch (m->type) {
    case FS_OPEN: return do_open(m);
    case FS_CLOSE:
        o = slot_ino(m->a[0]);
        if (o && !--o->count && o->orphan && !o->stale) {
            if (!readonly && !touch()) chain_release(o->first);
        }
        return 0;
    case FS_READ:
        if ((m->a[0] & 0xF0000000u) == 0x40000000u || m->a[0] == ROOT) return -EISDIR;
        if ((r = slot_get(m->a[0], &o))) return r;
        return file_read(o, m->a[1], (u8 *)m->a[2], m->a[3]);
    case FS_WRITE:
        if ((m->a[0] & 0xF0000000u) == 0x40000000u || m->a[0] == ROOT) return -EISDIR;
        if ((r = slot_get(m->a[0], &o))) return r;
        return file_write(o, m->a[1], (const u8 *)m->a[2], m->a[3]);
    case FS_STAT: case FS_LSTAT:
        if ((r = namei(m->a[1], (const char *)m->a[0], &n))) return r;
        fill_node(&n, (struct ustat *)m->a[2]);
        return 0;
    case FS_FSTAT: {
        struct ustat *st = (struct ustat *)m->a[1];
        if (m->a[0] == ROOT || (m->a[0] & 0xF0000000u) == 0x40000000u) {
            if ((r = ino_dir(m->a[0], &dir))) return r;
            st->ino = m->a[0]; st->mode = S_IFDIR | m_dmode; st->size = 1u << clus_shift; st->links = 2;
            st->uid = m_uid; st->gid = m_gid; st->mtime = 0;
            return 0;
        }
        if ((r = slot_get(m->a[0], &o))) return r;
        st->ino = o->ino; st->mode = file_mode(o->attr); st->size = o->size; st->links = o->orphan ? 0 : 1;
        st->mtime = o->mtime; st->uid = m_uid; st->gid = m_gid;
        return 0;
    }
    case FS_READDIR: return do_readdir(m);
    case FS_CHDIR:
        if ((r = namei(m->a[1], (const char *)m->a[0], &n))) return r;
        if (!is_dir(&n)) return -ENOTDIR;
        if (!dmode_ok(FSP_X)) return -EACCES;
        if ((r = dir_of(&n, &dir))) return r;
        return (int)dir_ino(dir);
    case FS_MKDIR: return do_mkdir(m);
    case FS_UNLINK:
        if ((r = parent(m->a[1], (const char *)m->a[0], &dir))) return r == -EINVAL ? -EISDIR : r;
        if (!dir_may()) return -EACCES;
        if ((r = dir_find(dir, &n))) return r;
        if (is_dir(&n)) return -EISDIR;
        if ((r = touch()) || (r = entry_del(&n))) return r;
        o = slot_at(n.lba, n.byte);
        if (o) { o->orphan = 1; return 0; }       /* readable through its handle; the chain at the last close */
        return chain_release(first_of(n.e));
    case FS_RMDIR: {
        if ((r = parent(m->a[1], (const char *)m->a[0], &dir))) return r;
        if (!dir_may()) return -EACCES;
        if ((r = dir_find(dir, &n))) return r;
        if (!is_dir(&n)) return -ENOTDIR;
        u32 d;
        if ((r = dir_of(&n, &d))) return r;
        if ((r = dir_empty(d))) return r;
        if ((r = touch()) || (r = entry_del(&n))) return r;
        return chain_release(d);
    }
    case FS_RENAME: return do_rename(m);
    case FS_STATFS: {
        struct ustatfs *f = (struct ustatfs *)m->a[0];
        if (npend && !readonly && (r = sync_all())) return r;
        if ((r = count_free())) return r;
        u32 sh = clus_shift == 9 ? 1 : 0;         /* bsize at least 1 KB: programs divide by 1024 */
        f->bsize = (1u << clus_shift) << sh;
        f->blocks = (nclus - 2) >> sh; f->bfree = free_cnt >> sh; f->files = 0; f->ffree = 0;
        return 0;
    }
    case FS_SYNC:
        if (readonly) return dflush();
        return go_clean();
    case FS_CHMOD: {
        if ((r = namei(m->a[1], (const char *)m->a[0], &n))) return r;
        if (!fsp_owner_or_root(cuid, m_uid)) return -EPERM;
        u32 w = m->a[2] & 07777;
        if (is_dir(&n)) return w == m_dmode ? 0 : -EPERM;
        if (w != m_fmode && w != (m_fmode & ~0222u)) return -EPERM;
        u32 ro = w == m_fmode ? 0 : A_RO;
        if (m_fmode == (m_fmode & ~0222u)) return 0;   /* the mount gives no write bits: nothing on the disk to change */
        if ((n.e[11] & A_RO) == ro) return 0;
        if ((r = touch())) return r;
        u8 *e = entry_w(n.lba, n.byte);
        if (!e) return -EIO;
        e[11] = (u8)((e[11] & ~A_RO) | ro);
        o = slot_at(n.lba, n.byte);
        if (o) o->attr = e[11];
        return 0;
    }
    case FS_CHOWN:
        if ((r = namei(m->a[1], (const char *)m->a[0], &n))) return r;
        if (!fsp_is_root(cuid)) return -EACCES;
        if ((m->a[2] != 0xFFFFFFFFu && m->a[2] != m_uid) || (m->a[3] != 0xFFFFFFFFu && m->a[3] != m_gid)) return -EPERM;
        return 0;
    case FS_UTIMES: {
        if ((r = namei(m->a[1], (const char *)m->a[0], &n))) return r;
        int given = m->a[2] != FS_UTIME_NOW;
        unsigned md = is_dir(&n) ? m_dmode : file_mode(n.e[11]);
        if (!fsp_utimes_ok(cuid, m_uid, given, fsp_access(cuid, cgid, cng, cgs, md, m_uid, m_gid, FSP_W))) return -EACCES;
        if (n.isroot || !n.lba) return 0;         /* the root, or the working directory given as ".": no entry here */
        if ((r = touch())) return r;
        u8 *e = entry_w(n.lba, n.byte);
        if (!e) return -EIO;
        u32 t = given ? m->a[2] : (u32)sys_time();
        stamp(e, t);
        o = slot_at(n.lba, n.byte);
        if (o) o->mtime = t;
        return 0;
    }
    case FS_LINK: case FS_SYMLINK:
        return -EPERM;                            /* FAT has neither */
    case FS_READLINK:
        if ((r = namei(m->a[1], (const char *)m->a[0], &n))) return r;
        return -EINVAL;
    case FS_ROOT: {
        struct fsinfo *fi = (struct fsinfo *)m->a[0];
        strncpy(fi->dev, devname, sizeof fi->dev - 1);
        strcpy(fi->type, "fat");
        return (int)ROOT;
    }
    case FS_QUIT:
        quitting = 1;
        return 0;
    }
    return -ENOSYS;
}

static int handle(struct umsg *m)
{
    if (!(m->kflags & UMSG_KERNEL) && m->uid != 0) return -EACCES;   /* raw IPC: inode numbers would bypass every check */
    cuid = m->uid; cgid = m->gid;
    cng = 0;
    if (m->kflags & UMSG_GROUPS) {
        const struct umsg_groups *g = UMSG_GROUPS_AT(m);
        cng = g->n < NGROUPS ? g->n : NGROUPS;
        for (unsigned i = 0; i < cng; i++) cgs[i] = g->g[i];
    }
    return handle1(m);
}

/* ---- the volume ---- */
static int why(const char *t)
{
    dprintf(1, "fatd: %s: %s\n", devname, t);
    return -1;
}
static int check_bpb(const u8 *b, u32 dev_secs)
{
    u32 bps = get16(b + 11);
    spc = b[13];
    if (b[510] != 0x55 || b[511] != 0xAA) return why("not a FAT volume (no boot signature). A partition table? Mount a partition");
    if (bps != SECT) return why(bps && !(bps & (bps - 1)) && bps >= 512 && bps <= 4096 ? "only 512-byte sectors are supported" : "not a FAT volume (bad sector size)");
    if (!spc || (spc & (spc - 1))) return why("not a FAT volume (bad cluster size)");
    clus_shift = 9;
    while ((1u << (clus_shift - 9)) < spc) clus_shift++;
    u32 res = get16(b + 14), rootents = get16(b + 17);
    nfats = b[16];
    total_secs = get16(b + 19);
    if (!total_secs) total_secs = get32(b + 32);
    fat_secs = get16(b + 22);
    int fat32_layout = !fat_secs;
    if (fat32_layout) fat_secs = get32(b + 36);
    if (!res || !nfats || nfats > 4 || !fat_secs || !total_secs) return why("not a FAT volume (bad layout)");
    if (b[21] != 0xF0 && b[21] < 0xF8) return why("not a FAT volume (bad media byte)");
    if (total_secs > dev_secs) return why("damaged: the volume is larger than its device");
    if (fat_secs > total_secs / nfats) return why("damaged: bad FAT size");
    u32 fats_end = res + nfats * fat_secs;
    root_secs = (rootents * 32 + SECT - 1) / SECT;
    fat_start = res;
    root_start = fats_end;
    data_start = fats_end + root_secs;
    if (fats_end >= total_secs || data_start >= total_secs) return why("damaged: no room for data");
    u32 clusters = (total_secs - data_start) >> (clus_shift - 9);
    /* the specification: the number of clusters says 12 or 16. FAT32 by its layout: real boot partitions are made
     * "FAT32" with fewer clusters than the specification's minimum (mkfs.fat -F 32) */
    if (!clusters) return why("damaged: no clusters");
    if (clusters > 0x0FFFFFF5u) return why("damaged: too many clusters");
    fat_bits = fat32_layout ? 32 : clusters < 4085 ? 12 : clusters < 65525 ? 16 : 0;
    if (!fat_bits) return why("damaged: too many clusters for a FAT16 layout");
    nclus = clusters + 2;
    u32 fat_bytes_needed = fat_bits == 12 ? nclus + nclus / 2 + 1 : fat_bits == 16 ? nclus * 2 : nclus * 4;
    if (fat_secs < (fat_bytes_needed + SECT - 1) / SECT) return why("damaged: the FAT is too short for the volume");
    if (fat_bits == 32) {
        if (rootents) return why("damaged: a root directory area on a FAT32 layout");
        if (get16(b + 42)) return why("an unknown FAT32 version");
        u32 ext = get16(b + 40);
        root_clus = get32(b + 44);
        if (root_clus < 2 || root_clus >= nclus) return why("damaged: bad root directory");
        fsinfo_sec = get16(b + 48);
        if (fsinfo_sec >= res) fsinfo_sec = 0;
        if (ext & 0x80) { mirror = 0; active = ext & 15; if (active >= nfats) return why("damaged: bad active FAT"); }
    } else {
        if (!rootents) return why("damaged: no root directory");
        root_clus = 0;
    }
    return 0;
}

static int try_mount(const char *name, int restart)
{
    struct umsg m;
    struct diskinfo di;
    static u8 b[SECT];
    (void)restart;
    strncpy(devname, name, sizeof devname - 1);
    if (dport < 0 && (dport = sys_port_find("disk")) < 0) return -1;
    m.type = DISK_OPEN; m.a[0] = (u32)name; m.a[1] = DISK_MOUNT;
    if ((dh = disk_call(dport, &m)) < 0) return why("in use, or no such device");
    m.type = DISK_INFOSZ; m.a[0] = (u32)dh; m.a[1] = (u32)&di; m.a[2] = sizeof di;
    if (disk_call(dport, &m) < 0) return why("cannot read the device");
    maxsect = di.maxsect ? di.maxsect : 1;
    if (!(di.caps & (DISKCAP_FLUSH | DISKCAP_RAM)))
        why("the disk cannot flush its write cache: a power cut may damage the volume if the disk reorders writes");
    total_secs = di.sectors;
    if (dio(DISK_READ, 0, 1, b)) return why("cannot read the boot sector");
    if (check_bpb(b, di.sectors)) goto no;
    /* the dirty marks: the boot sector's (other systems'), FAT[1]'s */
    u32 v = 0;
    int sig = fat_bits == 32 ? b[66] : b[38], bs_dirty = (sig == 0x28 || sig == 0x29) && ((fat_bits == 32 ? b[65] : b[37]) & 1);
    if (clean_bit() && fat_raw(1, &v)) { why("cannot read the FAT"); goto no; }
    int unclean = bs_dirty || (clean_bit() && !(v & clean_bit()));
    if (unclean && !force && !readonly) {
        why("was not unmounted cleanly: mounted read-only. Check it with fsck.fat on another computer, or mount with -o force");
        readonly = 1;
    }
    if (unclean && !readonly) {
        why("was not unmounted cleanly: mounted writable (-o force)");
        if (clean_bit() && !(v & clean_bit())) marked = 1;      /* ours to clear at the end */
        info_dirty = 1;                                         /* FSInfo is not trusted below: the next write says unknown or the count */
        /* the boot sector's byte stays as it is: that sector is never written here */
    }
    if (fat_bits == 32 && fsinfo_sec && !dio(DISK_READ, fsinfo_sec, 1, b) && !unclean &&
        get32(b) == 0x41615252u && get32(b + 484) == 0x61417272u && get32(b + 508) == 0xAA550000u) {
        u32 fc = get32(b + 488), nf = get32(b + 492);
        if (fc <= nclus - 2) free_cnt = fc;
        if (nf >= 2 && nf < nclus) next_free = nf;
    }
    return 0;
no:
    m.type = DISK_UNMOUNT; m.a[0] = (u32)dh;
    disk_call(dport, &m);
    dh = -1;
    return -1;
}

/* -o uid=N,gid=N,fmode=OCT,dmode=OCT,ro,rw,force,defaults,noauto */
static u32 optnum(const char *s, u32 base, int *bad)
{
    u32 v = 0;
    if (!*s) *bad = 1;
    for (; *s && *s != ','; s++) {
        if (*s < '0' || *s >= (char)('0' + base)) { *bad = 1; return 0; }
        v = v * base + (u32)(*s - '0');
    }
    return v;
}
static int options(const char *o)
{
    while (*o) {
        int bad = 0;
        if (!strncmp(o, "uid=", 4)) m_uid = optnum(o + 4, 10, &bad);
        else if (!strncmp(o, "gid=", 4)) m_gid = optnum(o + 4, 10, &bad);
        else if (!strncmp(o, "fmode=", 6)) m_fmode = optnum(o + 6, 8, &bad) & 0777;
        else if (!strncmp(o, "dmode=", 6)) m_dmode = optnum(o + 6, 8, &bad) & 0777;
        else if (!strncmp(o, "ro", 2) && (o[2] == ',' || !o[2])) readonly = 1;
        else if (!strncmp(o, "force", 5) && (o[5] == ',' || !o[5])) force = 1;
        else if (!((!strncmp(o, "rw", 2) && (o[2] == ',' || !o[2])) || (!strncmp(o, "defaults", 8) && (o[8] == ',' || !o[8])) ||
                   (!strncmp(o, "noauto", 6) && (o[6] == ',' || !o[6])))) bad = 1;
        if (bad) { dprintf(1, "fatd: bad option at: %s (uid=, gid=, fmode=, dmode=, ro, rw, force)\n", o); return -1; }
        while (*o && *o != ',') o++;
        if (*o) o++;
    }
    return 0;
}

#ifndef ZHOST
int main(int argc, char **argv)
{
    if (argc < 2) { dprintf(1, "usage: fatd DEVICE [-id START,SIZE] [-o OPTIONS]\n"); return 2; }
    const char *dev = argv[1], *id = 0;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-id")) id = argv[i + 1];
        else if (!strcmp(argv[i], "-o")) { if (options(argv[i + 1])) return 1; }
    }
    char pname[16], who[16], name[16], full[64];
    strcpy(pname, "fs."); strncpy(pname + 3, dev, 12); pname[15] = 0;
    strcpy(who, pname);
    int restart = (sys_svc_self() & 0xFFFF) > 1;
    if (id) {                                     /* the place only (volid.h): an all-zero uuid confirms nothing */
        u32 l = strlen(id);
        if (l > 17) l = 17;
        memcpy(full, id, l);
        full[l] = ',';
        memset(full + l + 1, '0', 32);
        full[l + 33] = 0;
        if (vol_find(sys_port_find("disk"), dev, full, name, who, restart)) return 1;
        dev = name;
    }
    if (try_mount(dev, restart)) return 1;
    int port = sys_port_create(pname);
    if (port < 0) { dprintf(1, "fatd: cannot create the port %s\n", pname); return 1; }
    if (restart) {                                /* the kernel's open files from before: stale until closed (d30) */
        static u32 ino[NOPEN];
        int n = sys_svc_opens(ino, sizeof ino);
        for (int i = 0, j = 0; i < n && i < NOPEN; i++)
            if ((ino[i] & 0xF0000000u) != 0x40000000u && ino[i] != ROOT) { opens[j].ino = ino[i]; opens[j].count = 1; opens[j].stale = 1; j++; }
    }
    struct umsg *m = 0;
    u32 saved_at = sys_ticks();                   /* the last commit: the volume goes clean QUIET_TICKS after it */
    int warned = 0;
    for (;;) {
        if (!m) {
            u32 age, now = sys_ticks();
            if (dirty) {                          /* changes wait: the next request, or the commit time */
                age = now - dirty_since;
                if (age < COMMIT_TICKS) m = (struct umsg *)sys3(32, (u32)port, 1 + COMMIT_TICKS - age, 0);
                if (!m) {                         /* on time, also under a steady stream of requests */
                    if (sync_all()) {
                        if (!warned++) dprintf(1, "fatd: %s: a write failed: changes are not saved yet\n", devname);
                        dirty_since = sys_ticks();
                    } else saved_at = sys_ticks();
                    continue;
                }
            } else if (marked) {                  /* all saved: counted from the commit, not from the last request */
                age = now - saved_at;
                if (age < QUIET_TICKS) m = (struct umsg *)sys3(32, (u32)port, 1 + QUIET_TICKS - age, 0);
                if (!m) {
                    if (go_clean() && !warned++) dprintf(1, "fatd: %s: a write failed: the volume stays marked\n", devname);
                    saved_at = sys_ticks();
                    continue;
                }
            } else m = sys_recv(port, 1);
            if ((int)m == -1) break;
        }
        int r = handle(m);
        if (quitting) {
            int e = readonly ? 0 : go_clean();
            if (e) dprintf(1, "fatd: %s: could not save everything at umount\n", devname);
            sys_reply(m, r);
            struct umsg u;
            u.type = DISK_UNMOUNT; u.a[0] = (u32)dh;
            disk_call(dport, &u);
            return 0;
        }
        if (!dirty && !marked) {
            m = sys_replyrecv(m, r, port);
            if ((int)m == -1) break;
            continue;
        }
        sys_reply(m, r);
        m = 0;
    }
    return 1;
}
#endif
