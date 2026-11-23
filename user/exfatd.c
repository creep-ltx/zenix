/* SPDX-License-Identifier: GPL-2.0-only */
#ifdef ZHOST
#include "../tools/host/zhost.h"           /* the host tests (tools/host/fsd-host.c built with -DEXFATD) */
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

/* exfatd DEVICE -id START,SIZE [-o OPTIONS]: reads exFAT volumes (USB sticks, SD cards), a file service started by mount(8)
 * as fs.DEVICE. The decisions and why: docs/d47-exfat.md; the format from Microsoft's exFAT specification (2019). In short:
 * - READ-ONLY: nothing but DISK_READ goes to the disk server, and every change answers EROFS.
 * - Small: nothing of the FAT or the allocation bitmap is kept whole; 16 sectors of FAT and directories are cached, 16 of
 *   a file's read ahead for small reads; whole sectors go straight to the caller, a contiguous run as one request.
 * - Every entry set's checksum is checked before it is used: a bad one is never followed. Everything read from the disk is
 *   untrusted: a damaged volume answers EIO, never crashes or hangs (tools/exfat-host-test.py, ASAN + UBSAN).
 * - Programs see 32-bit sizes: a file of 4 GB or more is listed with the size 4294967295 (as pfs3d's and NFS's) and its
 *   open answers EFBIG ("file too large"): never a size or data wrapped round. */

typedef unsigned long long u64;

#define SECT       512u                /* the disk server's sector */
#define NCACHE     16                  /* sectors of FAT and directories kept */
#define NWIN       16                  /* a file's sectors read ahead together for small reads */
#define NOPEN      64                  /* NFILE: every object the kernel holds open has a slot */
#define DIR_MAX    (256u << 20)        /* the format's largest directory */
#define ROOT       1u
#define NAMEMAX    255                 /* UTF-16 units */
#define UTF8MAX    (NAMEMAX * 3 + 1)   /* a name as UTF-8 (a surrogate pair's 4 bytes are 2 units: never more) */
#define FAT_EOC    0xFFFFFFFFu
#define FAT_BAD    0xFFFFFFF7u
#define T_EOD      0x00                /* entry types (spec 6.2.1, 7) */
#define T_BITMAP   0x81
#define T_UPCASE   0x82
#define T_LABEL    0x83
#define T_FILE     0x85
#define T_STREAM   0xC0
#define T_NAME     0xC1
#define A_RO       0x01                /* FileAttributes (spec 7.4.4) */
#define A_DIR      0x10

enum { K_FILE, K_DIR, K_ODD };         /* K_ODD: an entry set with a critical secondary entry this server does not know:
                                        * listed, not opened (spec 8.2) */

/* the volume, from the boot sector (all checked in check_boot); in the disk server's 512-byte sectors */
static u32 dsh;                        /* the volume's sector is 512 << dsh bytes */
static u32 cb;                         /* a cluster is 1 << cb bytes (9 .. 25) */
static u32 fat_lba, heap_lba, nclus, root_clus, nsect;
/* the mount */
static int dport = -1, dh = -1;
static u32 maxsect = 1;
static u32 m_uid, m_gid, m_fmode = 0644, m_dmode = 0755;
static char devname[16];
static unsigned cuid, cgid, cng;
static unsigned short cgs[NGROUPS];
static int quitting, stale;
static u32 free_cnt = 0xFFFFFFFFu;     /* counted from the bitmap at the first statfs */
static u32 bm_first, bm_len;           /* the active allocation bitmap */
static u32 vflags;                     /* the boot sector's VolumeFlags (spec 3.1.13) */

static u32 g16(const u8 *p) { return p[0] | (u32)p[1] << 8; }
static u32 g32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }
static u64 g64(const u8 *p) { return g32(p) | (u64)g32(p + 4) << 32; }

/* ---- the disk: reads only ---- */
static int dread(u32 lba, u32 n, void *buf)
{
    u8 *p = buf;
    if (stale) return -ESTALE;
    if (lba >= nsect || n > nsect - lba) return -EIO;
    while (n) {
        struct umsg m;
        u32 k = n < maxsect ? n : maxsect;
        m.type = DISK_READ; m.a[0] = (u32)dh; m.a[1] = lba; m.a[2] = k; m.a[3] = (u32)p;
        int r = disk_call(dport, &m);
        if (r < 0) {
            if (r == -ESTALE) stale = 1;   /* the stick was pulled or changed under the mount: nothing more is read */
            return r == -ESTALE ? r : -EIO;
        }
        lba += k; n -= k; p += k * SECT;
    }
    return 0;
}
static int err(void) { return stale ? -ESTALE : -EIO; }

/* FAT and directory sectors, in a small cache; a pointer lasts until the next call */
struct cblk { u32 lba, stamp; int valid; };
static struct cblk cache[NCACHE];
static u8 *cmem, *win;
static u32 cstamp, win_lba, win_n;
static const u8 *sect(u32 lba)
{
    struct cblk *c, *pick = 0;
    for (u32 i = 0; i < NCACHE; i++) {
        c = &cache[i];
        if (c->valid && c->lba == lba) { c->stamp = ++cstamp; return cmem + i * SECT; }
        if (!pick || (pick->valid && (!c->valid || c->stamp < pick->stamp))) pick = c;
    }
    pick->valid = 0;
    if (dread(lba, 1, cmem + (u32)(pick - cache) * SECT)) return 0;
    pick->lba = lba; pick->valid = 1; pick->stamp = ++cstamp;
    return cmem + (u32)(pick - cache) * SECT;
}

/* ---- clusters ---- */
static int clus_ok(u32 c) { return c >= 2 && c - 2 < nclus; }
static u32 clus_lba(u32 c) { return heap_lba + ((c - 2) << (cb - 9)); }
/* the cluster after C in its chain into *NX. 0, or -EIO: free, bad, out of range or the end (a chain shorter than its
 * length) */
static int fat_next(u32 c, u32 *nx)
{
    const u8 *s = sect(fat_lba + (c >> 7));
    if (!s) return err();
    u32 v = g32(s + (c & 127) * 4);
    if (!clus_ok(v)) return -EIO;      /* FAT_EOC, FAT_BAD, 0, 1, past the heap: none of them goes on */
    *nx = v;
    return 0;
}

/* a stream: a file's or a directory's clusters, contiguous (NoFatChain) or a FAT chain, with a cursor so that reading in
 * order costs one FAT entry per cluster */
struct stream {
    u32 first, nclu, size, vdl;        /* first cluster (0: none), clusters, bytes, valid data length (bytes) */
    u8 nofat, pad[3];
    u32 cidx, ccl;                     /* the cursor: the cluster of index cidx is ccl */
};
/* the cluster of index IDX into *CL. With RUN: how many clusters from it lie one after the other on the disk, at most
 * *RUN (and the cursor is left at the run's last). 0, or -EIO */
static int stream_clus(struct stream *s, u32 idx, u32 *cl, u32 *run)
{
    if (idx >= s->nclu || !s->first) return -EIO;
    if (s->nofat) {
        *cl = s->first + idx;
        if (run && *run > s->nclu - idx) *run = s->nclu - idx;
        return 0;
    }
    if (idx < s->cidx || !s->ccl) { s->cidx = 0; s->ccl = s->first; }
    while (s->cidx < idx) {
        u32 nx;
        int r = fat_next(s->ccl, &nx);
        if (r) { s->ccl = 0; return r; }
        s->ccl = nx; s->cidx++;
    }
    *cl = s->ccl;
    if (run) {
        u32 want = *run, n = 1;
        if (want > s->nclu - idx) want = s->nclu - idx;
        while (n < want) {
            u32 nx;
            int r = fat_next(s->ccl, &nx);
            if (r) { s->ccl = 0; return r; }
            if (nx != s->ccl + 1) break;
            s->ccl = nx; s->cidx++; n++;
        }
        *run = n;
    }
    return 0;
}
/* the 32 bytes at byte OFF of directory D (OFF a multiple of 32, below its size), their sector into *LBA; 0 with *E set
 * on an error: -ESTALE, -EIO (the sector), 1 (the chain broke before it: nothing after it can be reached either) */
static const u8 *dent(struct stream *d, u32 off, int *e, u32 *lba)
{
    u32 cl;
    int r = stream_clus(d, off >> cb, &cl, 0);
    if (r) { *e = r == -ESTALE ? r : 1; return 0; }
    *lba = clus_lba(cl) + ((off & ((1u << cb) - 1)) >> 9);
    const u8 *s = sect(*lba);
    if (!s) { *e = err(); return 0; }
    return s + (off & 511);
}

/* ---- names and times ---- */
static u16 q[NAMEMAX + 1];             /* the name asked for (UTF-16) */
static u32 q_len;
static char u8name[UTF8MAX];

/* an exFAT timestamp (spec 7.4.8), its 10 ms count and its UTC offset (7.4.10) as Unix time; 0 for one that cannot be */
static u32 ex_time(u32 ts, u32 ms10, u32 utc)
{
    u32 y = 1980 + (ts >> 25), mo = (ts >> 21) & 15, d = (ts >> 16) & 31, h = (ts >> 11) & 31, mi = (ts >> 5) & 63, s2 = ts & 31;
    static const u8 mdays[12] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mo < 1 || mo > 12 || !d || d > mdays[mo - 1] || (mo == 2 && d == 29 && (y & 3)) || h > 23 || mi > 59 || s2 > 29 ||
        ms10 > 199 || y > 2105) return 0;   /* (2100 is not a leap year: no stick says 2100-02-29; past 2105 u32 wraps) */
    u32 days = (u32)zdays((int)y, (int)mo, (int)d);
    u32 v = days * 86400 + h * 3600 + mi * 60 + s2 * 2 + ms10 / 100;
    if (utc & 0x80) {                  /* valid: the local time was UTC + offset x 15 minutes (a 7-bit signed number) */
        int off = (int)(utc & 0x7F) - (utc & 0x40 ? 128 : 0);
        v -= (u32)(off * 900);
    }
    return v;
}

/* ---- entry sets ---- */
typedef struct {
    u32 off, next;                     /* the File entry's place in its directory; the place after the set */
    u32 lba, idx;                      /* the File entry's sector and its index there (the inode number's source) */
    u8 kind, big, ro, pad;             /* big: a file of 4 GB or more */
    u32 mtime;
    struct stream s;
    u32 nlen;
    u16 name[NAMEMAX];
} ent_t;

/* the entry set at byte OFF of directory D into E, OFF's entry known to be a File entry; with NAMES its name too. NLEN, HASH:
 * when given (a lookup), a set whose stream entry's name length or hash differs is passed over without its checksum (1).
 * 0: a good set; 1: passed over; -EIO: damage (*NEXT then OFF + 32); -ESTALE */
static int set_read(struct stream *d, u32 off, ent_t *e, u32 want_len, int want_hash)
{
    int r = 0;
    u32 lba;
    const u8 *p = dent(d, off, &r, &lba);
    if (!p) return r == -ESTALE ? r : -EIO;
    u32 sc = p[1], stored = g16(p + 2), attr = g16(p + 4), sum = 0;
    u32 ts = g32(p + 12), ms = p[21], utc = p[23];
    e->off = off; e->next = off + 32;
    if (sc < 2 || (u64)off + 32 + sc * 32u > d->size) return -EIO;
    for (u32 i = 0; i < 32; i++) if (i != 2 && i != 3) sum = (((sum & 1) ? 0x8000 : 0) + (sum >> 1) + p[i]) & 0xFFFF;
    e->lba = lba; e->idx = (off & 511) >> 5;              /* (p is not used after the next dent) */
    u32 nn = 0, nl = 0, flags = 0, first = 0;
    u64 vdl = 0, len = 0;
    int odd = 0;
    for (u32 i = 1; i <= sc; i++) {
        const u8 *s = dent(d, off + i * 32, &r, &lba);
        if (!s) return r == -ESTALE ? r : -EIO;
        u32 t = s[0];
        if (i == 1) {
            if (t != T_STREAM) return -EIO;
            flags = s[1]; nl = s[3];
            if (want_len && (nl != want_len || (want_hash >= 0 && g16(s + 4) != (u32)want_hash))) {
                e->next = off + 32 + sc * 32;
                return 1;
            }
            vdl = g64(s + 8); first = g32(s + 20); len = g64(s + 24);
            if (!nl || (nl + 14) / 15 > sc - 1) return -EIO;
        } else if (i <= 1 + (nl + 14) / 15) {
            if (t != T_NAME) return -EIO;
            for (u32 k = 0; k < 15 && nn < nl; k++) e->name[nn++] = (u16)g16(s + 2 + k * 2);
        } else if (!(t & 0x80) || !(t & 0x40)) return -EIO;   /* not in use, or a primary: the set was cut */
        else if (!(t & 0x20)) odd = 1;                        /* an unknown critical secondary (spec 8.2) */
        /* benign secondaries (vendor extension and allocation, 7.8, 7.9): counted in the checksum, else ignored */
        for (u32 k = 0; k < 32; k++) sum = (((sum & 1) ? 0x8000 : 0) + (sum >> 1) + s[k]) & 0xFFFF;
    }
    if (sum != stored) return -EIO;              /* the set is not what was written: never followed */
    e->next = off + 32 + sc * 32;
    /* the stream (spec 6.3.5, 6.3.6, 7.6): every number checked before use */
    if (!(flags & 1) || vdl > len || (!first && len) || (first && !clus_ok(first))) return -EIO;
    e->kind = odd ? K_ODD : attr & A_DIR ? K_DIR : K_FILE;
    e->ro = (attr & A_RO) != 0;
    e->big = 0;
    if (e->kind == K_DIR && (len > DIR_MAX || (len & 31))) return -EIO;
    if (len > 0xFFFFFFFFull) { e->big = 1; len = 0xFFFFFFFFull; vdl = vdl < len ? vdl : len; }
    struct stream *st = &e->s;
    st->first = first; st->nofat = (flags & 2) && first; st->size = (u32)len; st->vdl = (u32)vdl;
    st->nclu = len ? (u32)((len + (1u << cb) - 1) >> cb) : 0;
    st->cidx = 0; st->ccl = 0;
    if (e->big) st->nclu = 0;                               /* never read: nothing past 4 GB can be asked for */
    if (st->nofat && st->nclu && (u64)first - 2 + st->nclu > nclus) return -EIO;   /* a contiguous run past the heap */
    e->mtime = ex_time(ts, ms, utc);
    e->nlen = nl;
    return 0;
}

/* the next entry set of directory D from byte *OFF (passing over unused entries and the others); *OFF moves past it.
 * 1: one in E, 0: the end, -EIO: damage met (*OFF then past the one entry), -ESTALE */
static int dir_next(struct stream *d, u32 *off, ent_t *e, int root, u32 want_len, int want_hash)
{
    for (;;) {
        if (*off >= d->size) return 0;
        int r = 0;
        u32 lba;
        const u8 *p = dent(d, *off, &r, &lba);
        if (!p) {
            if (r == -ESTALE) return r;
            *off = r == 1 ? d->size : (*off | 511) + 1;    /* a broken chain ends the directory; a bad sector is passed */
            return -EIO;
        }
        u32 t = p[0];
        if (t == T_EOD) return 0;
        if (t == T_FILE) {
            r = set_read(d, *off, e, want_len, want_hash);
            if (r == -ESTALE) return r;
            *off = r == -EIO ? *off + 32 : e->next;
            if (r == 1) continue;
            return r ? -EIO : 1;
        }
        if (t < 0x80) { *off += 32; continue; }              /* an unused entry (a deleted set's) */
        if (t >= 0xA0 && t < 0xC0) {                         /* a benign primary (the volume GUID, TexFAT's padding): passed
                                                              * over with its secondaries */
            *off += 32 + p[1] * 32u;
            continue;
        }
        *off += 32;
        if (root && (t == T_BITMAP || t == T_UPCASE || t == T_LABEL)) continue;
        return -EIO;                                         /* 0x80, a stray secondary, another critical primary */
    }
}

/* the root directory's stream (its length is its chain's, spec 3.1.10) */
static struct stream rootst;

/* ---- open objects ---- */
struct ofile {
    u32 ino, count;
    u32 lba, idx;                      /* its File entry's place */
    u8 kind, ro, pad[2];
    u32 mtime;
    struct stream s;
};
static struct ofile opens[NOPEN];
static struct ofile *slot_ino(u32 ino)
{
    for (u32 i = 0; i < NOPEN; i++) if (opens[i].count && opens[i].ino == ino) return &opens[i];
    return 0;
}
static struct ofile *slot_at(u32 lba, u32 idx)
{
    for (u32 i = 0; i < NOPEN; i++) if (opens[i].count && opens[i].lba == lba && opens[i].idx == idx) return &opens[i];
    return 0;
}
/* an object's number: an open one keeps what it has; else its place (the File entry's sector x 16 + its index: 30 bits,
 * wrapping on a volume over 32 GB), unless a slot holds that number for another object (as fatd's, d30): then the first
 * of a fixed sequence of numbers from 0x40000000 up that no slot holds, so the same object gets the same number for as
 * long as the open objects stay the same (a listing's ~ name opens it) */
static u32 ent_ino(const ent_t *e)
{
    struct ofile *o = slot_at(e->lba, e->idx);
    if (o) return o->ino;
    u32 v = ((e->lba << 4) | e->idx) & 0x3FFFFFFFu, w = v;
    if (v < 16) v += 16;
    for (u32 k = 1; slot_ino(v); k++) v = 0x40000000u | ((w + k * 0x9E3779Bu + (e->lba >> 26)) & 0x0FFFFFFF);
    return v;
}

/* ---- modes ---- */
static u32 mode_of(int kind, int ro)
{
    return kind == K_DIR ? S_IFDIR | m_dmode : S_IFREG | (ro ? m_fmode & ~0222u : m_fmode);
}
static int may(int kind, int ro, int want)
{
    return fsp_access(cuid, cgid, cng, cgs, mode_of(kind, ro), m_uid, m_gid, want);
}

/* ---- paths ---- */
/* a name as programs see it (UTF-8; a character a Unix name cannot hold is '_'); one over a udirent's 63 bytes is its
 * start, ~ and its inode number in hex, which opens it (as iso9660d, ffsd and pfs3d) */
static void list_name(const ent_t *e, u32 ino, char *out)
{
    int n = u16_to_utf8(e->name, e->nlen, u8name, sizeof u8name, '_');
    u32 k = n < 0 ? 0 : (u32)n, all = k;
    if (k > 63) {
        k = 52;
        while (k && ((u8)u8name[k] & 0xC0) == 0x80) k--;     /* not inside a character */
    }
    memcpy(out, u8name, k);
    if (k < all) {
        out[k++] = '~';
        int s = 28;
        while (s > 0 && !(ino >> s)) s -= 4;
        for (; s >= 0; s -= 4) out[k++] = "0123456789abcdef"[(ino >> s) & 15];
    }
    out[k] = 0;
}
/* is E's name the one in q (folded case; a character listed as '_' is found as '_')? */
static int same_name(const ent_t *e)
{
    if (e->nlen != q_len) return 0;
    for (u32 i = 0; i < q_len; i++) {
        u32 a = e->name[i], b = q[i];
        if (a != b && u16_fold(a) != u16_fold(b) && !(b == '_' && u16_bad(e->name, e->nlen, i))) return 0;
    }
    return 1;
}
/* the name asked for in q as the spec's NameHash would be (spec 7.6.4) when it is ASCII, whose up-casing the spec fixes;
 * -1 otherwise (then only the length is compared before the name) */
static int q_hash(void)
{
    u32 h = 0;
    for (u32 i = 0; i < q_len; i++) {
        u32 c = q[i];
        if (c >= 0x80 || c == '_') return -1;                /* '_' may stand for a character listed so */
        if (c >= 'a' && c <= 'z') c -= 32;
        h = (((h & 1) ? 0x8000 : 0) + (h >> 1) + c) & 0xFFFF;   /* the unit's low byte, then its high one (0) */
        h = ((h & 1) ? 0x8000 : 0) + (h >> 1);
    }
    return (int)h;
}
/* NAME (QL bytes) in directory D into E. 0, -ENOENT, -EIO (damage met on the way, and not found), -ESTALE, -ENAMETOOLONG */
static int lookup(struct stream *d, int root, const char *name, u32 ql, ent_t *e)
{
    char ln[64];
    int bad = 0, r, n = u16_from_utf8(name, ql, q, NAMEMAX + 1);
    if (n == -ENAMETOOLONG) return n;
    q_len = n < 0 ? 0 : (u32)n;
    if (n > 0) {                                         /* the name itself: only sets of its length (and ASCII's hash) */
        int h = q_hash();
        u32 off = 0;
        while ((r = dir_next(d, &off, e, root, q_len, h)) != 0) {
            if (r == -ESTALE) return r;
            if (r < 0) bad = 1;
            else if (same_name(e)) return 0;
        }
    }
    u32 tilde = 0;
    for (u32 i = 0; i < ql; i++) tilde |= name[i] == '~';
    if (ql <= 63 && tilde) {                             /* the short form of a long name, as a listing shows it */
        u32 off = 0;
        while ((r = dir_next(d, &off, e, root, 0, -1)) != 0) {
            if (r == -ESTALE) return r;
            if (r < 0) { bad = 1; continue; }
            if (e->nlen < 22) continue;                  /* (a name of fewer units fits 63 bytes) */
            list_name(e, ent_ino(e), ln);
            if (strlen(ln) == ql && !memcmp(ln, name, ql) && strchr(ln, '~')) return 0;
        }
    }
    return bad ? -EIO : -ENOENT;
}

/* the node a walk ends at: the root, or an entry set */
typedef struct { int root; ent_t e; } node_t;
static void node_stream(node_t *n, struct stream *s)
{
    *s = n->root ? rootst : n->e.s;
}
static int node_kind(const node_t *n) { return n->root ? K_DIR : n->e.kind; }

/* walk PATH's first LEN bytes from directory CWD (ROOT, or an open directory's number); the end in N. exFAT has no links
 * and no ".." entries, so "." and ".." are taken out of the text first, as the kernel does with every path it sends; a
 * relative ".." above an open directory answers -ENOENT (only a raw request can ask it: the kernel's paths are absolute) */
static int walk(u32 cwd, const char *path, u32 len, node_t *n)
{
    static char t[FS_PATHMAX];
    u32 tl = 0, i = 0;
    struct stream cur;
    if (len >= FS_PATHMAX) return -ENAMETOOLONG;
    while (i < len) {
        while (i < len && path[i] == '/') i++;
        if (i >= len) break;
        u32 s = i;
        while (i < len && path[i] != '/') i++;
        if (i - s == 1 && path[s] == '.') continue;
        if (i - s == 2 && path[s] == '.' && path[s + 1] == '.') {
            if (!tl) { if (path[0] != '/' && cwd != ROOT) return -ENOENT; continue; }
            while (tl && t[tl - 1] != '/') tl--;
            if (tl) tl--;
            continue;
        }
        if (tl) t[tl++] = '/';
        memcpy(t + tl, path + s, i - s);
        tl += i - s;
    }
    n->root = 1;
    cur = rootst;
    if (path[0] != '/' && cwd != ROOT) {
        struct ofile *o = slot_ino(cwd);
        if (!o || o->kind != K_DIR) return o ? -ENOTDIR : -ESTALE;
        cur = o->s; n->root = 0;
        n->e.kind = K_DIR; n->e.ro = 0; n->e.big = 0; n->e.s = o->s; n->e.lba = o->lba; n->e.idx = o->idx;
        n->e.mtime = o->mtime; n->e.nlen = 0;
    }
    for (i = 0; i < tl; ) {
        u32 s = i;
        while (i < tl && t[i] != '/') i++;
        if (node_kind(n) != K_DIR) return -ENOTDIR;
        if (!may(K_DIR, 0, FSP_X)) return -EACCES;
        int r = lookup(&cur, n->root, t + s, i - s, &n->e);
        if (r) return r;
        n->root = 0;
        cur = n->e.s;
        i++;
    }
    return 0;
}

/* the directory that holds PATH's last name, which must exist (WANT 1), must not (0), or either (-1): the answer a
 * writable volume would give, then -EROFS (as iso9660d) */
static int refuse_at(u32 cwd, const char *path, int want)
{
    static node_t d, t;
    u32 n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    u32 s = n;
    while (s > 0 && path[s - 1] != '/') s--;
    if (n == s || (n - s == 1 && path[s] == '.') || (n - s == 2 && path[s] == '.' && path[s + 1] == '.')) return -EINVAL;
    int r = walk(cwd, path, s, &d);
    if (r) return r;
    if (node_kind(&d) != K_DIR) return -ENOTDIR;
    if (!may(K_DIR, 0, FSP_X)) return -EACCES;
    if (want < 0) return -EROFS;
    struct stream ds;
    node_stream(&d, &ds);
    r = lookup(&ds, d.root, path + s, n - s, &t.e);
    if (r && r != -ENOENT) return r;
    if (want && r) return r;
    if (!want && !r) return -EEXIST;
    return -EROFS;
}

/* ---- file data ---- */
/* a chain's cursor moved back to the cluster of the last byte used (USED bytes from P, whose cluster is CL, inside the
 * run stream_clus measured): the next piece starts there, never from the chain's start */
static void at_used(struct stream *s, u32 p, u32 cl, u32 used)
{
    if (s->nofat) return;
    u32 j = (p + used - 1) >> cb;
    s->ccl = cl + (j - (p >> cb)); s->cidx = j;
}
/* N bytes of stream S from OFF into BUF: whole sectors straight into BUF, a contiguous run as one request (in pieces of
 * the device's largest); the rest through a window of up to NWIN sectors; past the valid data length zeros (spec 7.6.5) */
static int stream_read(struct stream *s, u32 off, u8 *buf, u32 n)
{
    if (off >= s->size) return 0;
    if (n > s->size - off) n = s->size - off;
    u32 done = 0, cmask = (1u << cb) - 1;
    while (done < n) {
        u32 p = off + done, k = n - done;
        if (p >= s->vdl) { memset(buf + done, 0, k); break; }
        if (k > s->vdl - p) k = s->vdl - p;
        u64 want = (((u64)k + (p & cmask) + cmask) >> cb);   /* the clusters K bytes from P touch */
        u32 cl, run = want < s->nclu - (p >> cb) ? (u32)want : s->nclu - (p >> cb);
        int e = stream_clus(s, p >> cb, &cl, &run);
        if (e) return e == -ESTALE ? e : err();
        u32 lba = clus_lba(cl) + ((p & cmask) >> 9);
        u32 avail = (u32)(((u64)run << (cb - 9)) - ((p & cmask) >> 9));   /* sectors from LBA in the run */
        if (!(p & 511) && k >= SECT) {
            u32 ns = k >> 9;
            if (ns > avail) ns = avail;
            if ((e = dread(lba, ns, buf + done))) return e == -ESTALE ? e : -EIO;
            done += ns * SECT;
            at_used(s, p, cl, ns * SECT);
            continue;
        }
        if (!win_n || lba < win_lba || lba >= win_lba + win_n) {
            u32 rem = s->vdl - (p & ~511u), left = (rem >> 9) + ((rem & 511) != 0);   /* (no wrap near 4 GB) */
            u32 m = avail < NWIN ? avail : NWIN;
            if (m > left) m = left;
            win_n = 0;
            if ((e = dread(lba, m, win))) return e == -ESTALE ? e : -EIO;
            win_lba = lba; win_n = m;
        }
        u32 o = p & 511, c = SECT - o < k ? SECT - o : k;
        memcpy(buf + done, win + (lba - win_lba) * SECT + o, c);
        done += c;
        at_used(s, p, cl, c);
    }
    return (int)n;
}

/* ---- requests ---- */
static void fill_ent(const ent_t *e, u32 ino, struct ustat *st)
{
    st->ino = ino;
    st->mode = mode_of(e->kind, e->ro);
    st->uid = m_uid; st->gid = m_gid;
    st->mtime = e->mtime;
    st->size = e->s.size;
    st->links = e->kind == K_DIR ? 2 : 1;
}
static void fill_root(struct ustat *st)
{
    st->ino = ROOT; st->mode = S_IFDIR | m_dmode; st->uid = m_uid; st->gid = m_gid; st->mtime = 0;
    st->size = rootst.size; st->links = 2;
}
static int do_open(struct umsg *m)
{
    static node_t n;
    const char *path = (const char *)m->a[0];
    u32 flags = m->a[2];
    struct ustat *st = (struct ustat *)m->a[3];
    int r = walk(m->a[1], path, strlen(path), &n);
    if (r == -ENOENT && (flags & O_CREAT)) return (r = refuse_at(m->a[1], path, 0)) == -EEXIST ? -EROFS : r;
    if (r) return r;
    if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) return -EEXIST;
    int kind = node_kind(&n);
    if (kind == K_DIR && (flags & (O_WRONLY | O_RDWR))) return -EISDIR;
    if (!may(kind, n.root ? 0 : n.e.ro, fsp_open_want(flags))) return -EACCES;
    if (flags & (O_WRONLY | O_RDWR | O_TRUNC)) return -EROFS;
    if (n.root) { fill_root(st); return ROOT; }
    if (kind == K_ODD) return -EOPNOTSUPP;
    if (n.e.big) return -EFBIG;
    struct ofile *o = slot_at(n.e.lba, n.e.idx);
    if (!o) {
        u32 ino = ent_ino(&n.e);
        for (u32 i = 0; i < NOPEN && !o; i++) if (!opens[i].count) o = &opens[i];
        if (!o) return -ENFILE;
        o->ino = ino; o->lba = n.e.lba; o->idx = n.e.idx; o->kind = (u8)kind; o->ro = n.e.ro; o->mtime = n.e.mtime;
        o->s = n.e.s;
    }
    o->count++;
    fill_ent(&n.e, o->ino, st);
    return (int)o->ino;
}
static void fill_slot(const struct ofile *o, struct ustat *st)
{
    st->ino = o->ino; st->mode = mode_of(o->kind, o->ro); st->uid = m_uid; st->gid = m_gid; st->mtime = o->mtime;
    st->size = o->s.size; st->links = o->kind == K_DIR ? 2 : 1;
}
/* a listing's place: the byte in the directory where the next entry starts; bit 31: damage was met (answered -EIO at the
 * end, so what can be read is listed) */
static int do_readdir(struct umsg *m)
{
    static ent_t e;
    struct udirent *ud = (struct udirent *)m->a[2];
    u32 bad = m->a[1] >> 31, off = m->a[1] & 0x7FFFFFFF;
    if (off & 31) return -EINVAL;                    /* a place no listing gave (lseek on a directory): never inside an entry */
    u32 *newc = (u32 *)m->a[3];
    struct stream *d;
    int root = m->a[0] == ROOT;
    if (root) d = &rootst;
    else {
        struct ofile *o = slot_ino(m->a[0]);
        if (!o) return -ESTALE;
        if (o->kind != K_DIR) return -ENOTDIR;
        d = &o->s;
    }
    for (;;) {
        int r = dir_next(d, &off, &e, root, 0, -1);
        if (r == 0) { *newc = bad << 31 | off; return bad ? -EIO : 0; }
        if (r == -ESTALE) return r;
        if (r < 0) { bad = 1; continue; }
        u32 ino = ent_ino(&e);
        list_name(&e, ino, ud->name);
        ud->type = e.kind == K_DIR ? 2 : 1;
        ud->ino = ino;
        *newc = bad << 31 | off;
        return 1;
    }
}
/* DESIGN D49: FS_READDIRS, the same records in a batch, each with FS_LSTAT's answer (fill_ent of the entry set) when the
 * caller may search directories; the damage rule of do_readdir (go on, -EIO at the end) kept in the position's bit 31 */
static int do_readdirs(struct umsg *m)
{
    static ent_t e;
    static char nm[64];
    struct fsrdx *q = (struct fsrdx *)m->a[3];
    u32 bad = m->a[1] >> 31, off = m->a[1] & 0x7FFFFFFF, at;
    unsigned used = 0;
    if (off & 31) return -EINVAL;
    struct stream *d;
    int root = m->a[0] == ROOT, r;
    if (root) d = &rootst;
    else {
        struct ofile *o = slot_ino(m->a[0]);
        if (!o) return -ESTALE;
        if (o->kind != K_DIR) return -ENOTDIR;
        d = &o->s;
    }
    int x = may(K_DIR, 0, FSP_X);
    for (;;) {
        at = off;
        r = dir_next(d, &off, &e, root, 0, -1);
        if (r == 0) { r = bad ? -EIO : 0; break; }
        if (r == -ESTALE) break;
        if (r < 0) { bad = 1; continue; }
        u32 ino = ent_ino(&e);
        list_name(&e, ino, nm);
        if (!nm[0]) continue;                       /* an empty name (damage) is never listed */
        if (used && (m->kflags & UMSG_CANCEL)) { off = at; break; }   /* a killed client: what is written */
        struct udirx *x1 = udirx_put((char *)m->a[2], q->size, &used, nm, ino, e.kind == K_DIR ? 2 : 1, bad << 31 | off);
        if (!x1) { off = at; break; }               /* full: this record comes first next time */
        if (x) { fill_ent(&e, ino, (struct ustat *)&x1->st); x1->flags = UDX_STAT; }
    }
    q->next = bad << 31 | off;
    return used ? (int)used : r;
}
/* the bitmap's zero bits for clusters 2 .. nclus + 1 (spec 7.1.5), read through the window; 0, or -errno */
static int count_free(void)
{
    static const u8 zeros[16] = { 4, 3, 3, 2, 3, 2, 2, 1, 3, 2, 2, 1, 2, 1, 1, 0 };
    struct stream s;
    u32 left = nclus, cnt = 0, spc = 1u << (cb - 9), idx = 0, in = 0;
    s.first = bm_first; s.nofat = 0; s.size = s.vdl = bm_len; s.nclu = (u32)(((u64)bm_len + (1u << cb) - 1) >> cb);
    s.cidx = 0; s.ccl = 0;
    win_n = 0;
    while (left) {
        u32 cl, k = spc - in, need = left / 4096 + 1;
        int r = stream_clus(&s, idx, &cl, 0);
        if (r) return r == -ESTALE ? r : -EIO;
        if (k > NWIN) k = NWIN;
        if (k > need) k = need;
        if ((r = dread(clus_lba(cl) + in, k, win))) return r;
        for (u32 i = 0; i < k * SECT && left; i++) {
            u32 b = win[i];
            if (left >= 8) { cnt += zeros[b & 15] + zeros[b >> 4]; left -= 8; }
            else { for (u32 j = 0; j < left; j++) cnt += !(b >> j & 1); left = 0; }
        }
        if ((in += k) == spc) { in = 0; idx++; }
    }
    free_cnt = cnt;
    return 0;
}
static int handle1(struct umsg *m)
{
    static node_t n;
    int r;
    struct ofile *o;
    const char *p = (const char *)m->a[0];
    switch (m->type) {
    case FS_OPEN: return do_open(m);
    case FS_CLOSE:
        if ((o = slot_ino(m->a[0]))) o->count--;
        return 0;
    case FS_READ:
        if (!(o = slot_ino(m->a[0]))) return m->a[0] == ROOT ? -EISDIR : -ESTALE;
        if (o->kind == K_DIR) return -EISDIR;
        return stream_read(&o->s, m->a[1], (u8 *)m->a[2], m->a[3]);
    case FS_STAT: case FS_LSTAT:
        if ((r = walk(m->a[1], p, strlen(p), &n))) return r;
        if (n.root) { fill_root((struct ustat *)m->a[2]); return 0; }
        fill_ent(&n.e, ent_ino(&n.e), (struct ustat *)m->a[2]);
        return 0;
    case FS_FSTAT:
        if (m->a[0] == ROOT) { fill_root((struct ustat *)m->a[1]); return 0; }
        if (!(o = slot_ino(m->a[0]))) return -ESTALE;
        fill_slot(o, (struct ustat *)m->a[1]);
        return 0;
    case FS_READDIR: return do_readdir(m);
    case FS_READDIRS: return do_readdirs(m);
    case FS_CHDIR:
        if ((r = walk(m->a[1], p, strlen(p), &n))) return r;
        if (node_kind(&n) != K_DIR) return -ENOTDIR;
        if (!may(K_DIR, 0, FSP_X)) return -EACCES;
        return n.root ? (int)ROOT : (int)ent_ino(&n.e);
    /* changes: none; the answer a writable volume would give comes first (ENOENT, EEXIST, EACCES), then EROFS */
    case FS_WRITE:
        return -EROFS;
    case FS_MKDIR:
        return refuse_at(m->a[1], p, 0);
    case FS_UNLINK: case FS_RMDIR:
        r = refuse_at(m->a[1], p, 1);
        return r == -EINVAL && m->type == FS_UNLINK ? -EISDIR : r;
    case FS_RENAME:
        if ((r = refuse_at(m->a[2], p, 1)) != -EROFS) return r;
        return refuse_at(m->a[2], (const char *)m->a[1], -1);
    case FS_LINK:
        if ((r = walk(m->a[2], p, strlen(p), &n))) return r;
        return refuse_at(m->a[2], (const char *)m->a[1], 0);
    case FS_SYMLINK:
        return refuse_at(m->a[2], (const char *)m->a[1], 0);
    case FS_CHMOD: case FS_CHOWN: case FS_UTIMES:
        return (r = walk(m->a[1], p, strlen(p), &n)) ? r : -EROFS;
    case FS_READLINK:
        return (r = walk(m->a[1], p, strlen(p), &n)) ? r : -EINVAL;
    case FS_STATFS: {
        struct ustatfs *f = (struct ustatfs *)m->a[0];
        if (free_cnt == 0xFFFFFFFFu && (r = count_free())) return r;
        f->bsize = 1u << cb; f->blocks = nclus; f->bfree = free_cnt; f->files = 0; f->ffree = 0;
        return 0;
    }
    case FS_SYNC:
        return 0;
    case FS_ROOT: {
        struct fsinfo *fi = (struct fsinfo *)m->a[0];
        strncpy(fi->dev, devname, sizeof fi->dev - 1);
        strcpy(fi->type, "exfat");
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

/* ---- the mount ---- */
static int why(const char *t)
{
    dprintf(1, "exfatd: %s: %s\n", devname, t);
    return -1;
}
/* the boot region from the volume's sector BASE (0 the main one, 12 its backup, whose place depends on the sector size:
 * SS, which its boot sector must then say): the boot sector checked (spec 3.1) and the region's checksum over its first
 * 11 sectors (3.4). 0, or the reason */
static const char *check_boot(u32 base, u32 try_ss)
{
    u8 *b = win;                                       /* the boot sector (512 bytes of it are all that is read there) */
    if (dread(base << (try_ss - 9), 1, b)) return "cannot read its boot sector";
    if (memcmp(b + 3, "EXFAT   ", 8)) return "not an exFAT volume (no EXFAT in its boot sector)";
    if (b[510] != 0x55 || b[511] != 0xAA || b[0] != 0xEB || b[1] != 0x76 || b[2] != 0x90) return "its boot sector's signature is wrong";
    for (u32 i = 11; i < 64; i++) if (b[i]) return "its boot sector's must-be-zero bytes are not";
    u32 ss = b[108], cs = b[109], nf = b[110], flags = g16(b + 106);
    if (ss < 9 || ss > 12 || cs > 25 - ss || (base && ss != try_ss)) return "its sector or cluster size is out of range";
    if (b[105] != 1) return "an exFAT revision other than 1.x: refused";
    if (nf != 1 && nf != 2) return "its number of FATs is out of range";
    u64 vlen = g64(b + 72);
    u32 fo = g32(b + 80), fl = g32(b + 84), ho = g32(b + 88), cc = g32(b + 92), rc = g32(b + 96);
    dsh = ss - 9; cb = ss + cs;
    if (vlen < (1u << (20 - ss)) || (vlen << dsh) >> dsh != vlen || vlen << dsh > nsect) return "its length is out of range or past the device";
    if (fo < 24 || !cc || cc > 0xFFFFFFF5u) return "its FAT offset or cluster count is out of range";
    if ((u64)fl << ss < ((u64)cc + 2) * 4 || (u64)fo + (nf == 2 ? (u64)fl << 1 : fl) > ho) return "its FAT is too short or overlaps the clusters";
    if ((u64)ho + ((u64)cc << cs) > vlen) return "its clusters run past its end";
    if (rc < 2 || rc - 2 >= cc) return "its root directory cluster is out of range";
    if ((flags & 1) && nf == 1) return "its active FAT is the second of one";
    /* the checksum of sectors base .. base + 10, all but VolumeFlags and PercentInUse, against sector base + 11 */
    u32 sum = 0, bps = 1u << ss;
    for (u32 sec = 0; sec < 11; sec++)
        for (u32 piece = 0; piece < (bps >> 9); piece++) {
            if (dread(((base + sec) << dsh) + piece, 1, b)) return "cannot read its boot region";
            for (u32 i = 0; i < SECT; i++) {
                u32 at = sec * bps + piece * SECT + i;
                if (at == 106 || at == 107 || at == 112) continue;
                sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + b[i];
            }
        }
    for (u32 piece = 0; piece < (bps >> 9); piece++) {
        if (dread(((base + 11) << dsh) + piece, 1, b)) return "cannot read its boot checksum";
        for (u32 i = 0; i < SECT; i += 4) if (g32(b + i) != sum) return "its boot region's checksum is wrong";
    }
    vflags = flags;
    fat_lba = (fo + ((flags & 1) ? fl : 0)) << dsh;
    heap_lba = ho << dsh;
    nclus = cc; root_clus = rc;
    return 0;
}
static int try_mount(const char *name, int restart)
{
    struct umsg m;
    struct diskinfo di;
    (void)restart;
    strncpy(devname, name, sizeof devname - 1);
    if (dport < 0 && (dport = sys_port_find("disk")) < 0) return -1;
    m.type = DISK_OPEN; m.a[0] = (u32)name; m.a[1] = DISK_MOUNT;
    if ((dh = disk_call(dport, &m)) < 0) return why("in use, or no such device");
    m.type = DISK_INFOSZ; m.a[0] = (u32)dh; m.a[1] = (u32)&di; m.a[2] = sizeof di;
    if (disk_call(dport, &m) < 0) { why("cannot read the device"); goto no; }
    maxsect = di.maxsect ? di.maxsect : 1;
    nsect = di.sectors;
    if (!cmem) { cmem = malloc(NCACHE * SECT); win = malloc(NWIN * SECT); }
    if (!cmem || !win) { why("out of memory"); goto no; }
    if (nsect < 2048) { why("too small for an exFAT volume"); goto no; }
    const char *e = check_boot(0, 9);
    if (e) {                                           /* the main boot region is damaged: its backup (spec 3), at sector
                                                        * 12 of whichever sector size it says */
        u32 ss = 9;
        while (ss <= 12 && check_boot(12, ss)) ss++;
        if (ss > 12) { why(e); goto no; }
        dprintf(1, "exfatd: %s: %s: its backup boot region is used\n", name, e);
    }
    {
        u32 flags = vflags;
        if (flags & 2) dprintf(1, "exfatd: %s: marked dirty (not removed cleanly?): read as it is\n", name);
        if (flags & 4) dprintf(1, "exfatd: %s: marked as having had media failures: read as it is\n", name);
    }
    /* the root directory: its chain's length (spec 3.1.10), at most 256 MB */
    rootst.first = root_clus; rootst.nofat = 0; rootst.cidx = 0; rootst.ccl = 0;
    {
        u32 c = root_clus, n = 1, maxn = (DIR_MAX >> cb) ? DIR_MAX >> cb : 1;
        for (;;) {
            const u8 *s = sect(fat_lba + (c >> 7));
            if (!s) { why("cannot read its FAT"); goto no; }
            u32 v = g32(s + (c & 127) * 4);
            if (v == FAT_EOC) break;
            if (!clus_ok(v) || n >= maxn) { why("its root directory's chain is damaged"); goto no; }
            c = v; n++;
        }
        rootst.nclu = n;
        rootst.size = rootst.vdl = (u32)(((u64)n << cb) > DIR_MAX ? DIR_MAX : (u64)n << cb);
    }
    /* the root's critical entries (spec 7.1-7.3, 8.2): the bitmap and the up-case table must be there, nothing unknown */
    {
        u32 off = 0, nup = 0, lba;
        int r = 0;
        while (off < rootst.size) {
            const u8 *p = dent(&rootst, off, &r, &lba);
            if (!p) { why("cannot read its root directory"); goto no; }
            u32 t = p[0];
            if (t == T_EOD) break;
            if (t == T_BITMAP) {
                u32 first = g32(p + 20);
                u64 len = g64(p + 24);
                if ((p[1] & 1) == (vflags & 1)) {                    /* the active FAT's bitmap */
                    if (!clus_ok(first) || len < (nclus + 7) / 8 || len > 0x20000000u) { why("its allocation bitmap's entry is damaged"); goto no; }
                    bm_first = first; bm_len = (u32)len;
                }
            } else if (t == T_UPCASE) {
                if (!clus_ok(g32(p + 20)) || !g64(p + 24) || g64(p + 24) > 0x20000) { why("its up-case table's entry is damaged"); goto no; }
                nup++;
            } else if (t > 0x80 && t < 0xA0 && t != T_LABEL && t != T_FILE) { why("an entry of a kind unknown to exFAT 1.00 in its root: refused"); goto no; }
            off += 32;
        }
        if (!bm_first || nup != 1) { why(!bm_first ? "no allocation bitmap" : "no up-case table, or two"); goto no; }
    }
    return 0;
no:
    m.type = DISK_UNMOUNT; m.a[0] = (u32)dh;
    disk_call(dport, &m);
    dh = -1;
    return -1;
}

/* -o uid=N,gid=N,fmode=OCT,dmode=OCT,ro,defaults,noauto */
static u32 optnum(const char *s, u32 base, int *bad)
{
    u32 v = 0;
    if (!*s || *s == ',') *bad = 1;
    for (; *s && *s != ','; s++) {
        if (*s < '0' || *s >= (char)('0' + base)) { *bad = 1; return 0; }
        v = v * base + (u32)(*s - '0');
    }
    return v;
}
static int word_is(const char *o, const char *w)
{
    u32 l = strlen(w);
    return !strncmp(o, w, l) && (o[l] == ',' || !o[l]);
}
static int options(const char *o)
{
    while (*o) {
        int bad = 0;
        if (!strncmp(o, "uid=", 4)) m_uid = optnum(o + 4, 10, &bad);
        else if (!strncmp(o, "gid=", 4)) m_gid = optnum(o + 4, 10, &bad);
        else if (!strncmp(o, "fmode=", 6)) m_fmode = optnum(o + 6, 8, &bad) & 0777;
        else if (!strncmp(o, "dmode=", 6)) m_dmode = optnum(o + 6, 8, &bad) & 0777;
        else if (word_is(o, "rw")) { dprintf(1, "exfatd: exFAT is read-only on Zenix: -o rw is refused\n"); return -1; }
        else if (!(word_is(o, "ro") || word_is(o, "defaults") || word_is(o, "noauto"))) bad = 1;
        if (bad) { dprintf(1, "exfatd: bad option at: %s (uid=, gid=, fmode=, dmode=, ro)\n", o); return -1; }
        while (*o && *o != ',') o++;
        if (*o) o++;
    }
    return 0;
}

#ifdef ZHOST
static int dirty;                      /* what the host tests' commands look at: nothing ever waits to be written here */
static int sync_all(void) { return 0; }
static int go_clean(void) { return 0; }
static int readonly = 1;
#else
int main(int argc, char **argv)
{
    if (argc < 2) { dprintf(1, "usage: exfatd DEVICE [-id START,SIZE] [-o OPTIONS]\n"); return 2; }
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
    if (port < 0) { dprintf(1, "exfatd: cannot create the port %s\n", pname); return 1; }
    struct umsg *m = sys_recv(port, 1);
    for (;;) {
        if ((int)m == -1) return 1;
        int r = handle(m);
        if (quitting) {
            sys_reply(m, r);
            struct umsg u;
            u.type = DISK_UNMOUNT; u.a[0] = (u32)dh;
            disk_call(dport, &u);
            return 0;
        }
        m = sys_replyrecv(m, r, port);
    }
}
#endif
