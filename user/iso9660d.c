/* SPDX-License-Identifier: GPL-2.0-only */
#ifdef ZHOST
#include "../tools/host/zhost.h"           /* the host tests (tools/host/fsd-host.c built with -DISO9660D) */
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

/* iso9660d DEVICE -id START,SIZE [-o OPTIONS]: reads ISO 9660 volumes (CD-ROMs, and their images on any device), a file
 * service started by mount(8) as fs.DEVICE. The decisions and why: docs/d44-cd.md. In short:
 * - READ-ONLY, as the medium: nothing but DISK_READ goes to the disk server, and every change answers EROFS.
 * - Rock Ridge (Unix names, modes, symbolic links, deep directories moved) when the root says so; else Joliet's long names
 *   when there are any; else the plain names (";1" cut, case ignored when looked up).
 * - An inode number is half the byte position of the object's directory record on the volume (a directory's: its "."
 *   record, at the start of its extent), so it names the record again after a restart, with no table of open files.
 * - Everything read from the disc is untrusted: every extent, length and record is checked before use; a damaged disc
 *   answers EIO, never crashes or hangs (tools/iso9660-host-test.py, ASAN + UBSAN). */

#define BLK         2048u
#define NCACHE      6                  /* directory blocks kept */
#define NWIN        8                  /* a file's blocks read ahead together for small reads */
#define MAXVOL      0x200000u          /* blocks: 4 GB (an inode number is half a byte position) */
#define NAMEMAX     255
#define MAXDIR      0x1000000u         /* a directory's largest size: 16 MB, about 100,000 names */
#define MAX_FOLLOW  8                  /* symbolic links inside one walk */
#define MAX_CE      4                  /* Rock Ridge continuation areas for one record (fewer than NCACHE - 1: the
                                        * record's own block stays in the cache while they are read) */
/* a directory record */
#define D_LEN       0
#define D_XAR       1
#define D_EXT       6                  /* the big-endian halves of the both-endian numbers */
#define D_SIZE      14
#define D_DATE      18
#define D_FLAGS     25
#define D_UNIT      26                 /* interleaving: file unit size, gap */
#define D_ILEN      32
#define D_ID        33
#define F_DIR       2u
#define F_ASSOC     4u
#define F_MULTI     0x80u

enum { K_FILE, K_DIR, K_LINK, K_BIG };    /* K_BIG: a file in several extents or interleaved: listed, not read */

/* the volume */
static u32 nblocks, root_ext, rr, rr_skip, joliet;
/* the mount */
static int dport = -1, dh = -1;
static u32 maxsect = 4;
static u32 m_uid, m_gid, m_fmode = 0555, m_dmode = 0555, o_norock, o_nojoliet;
static char devname[16];
static unsigned cuid, cgid, cng;
static unsigned short cgs[NGROUPS];
static int stoplinks, quitting, links_left;

static u32 g16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static u32 g32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* ---- the disc: reads only, whole blocks ---- */
static int dread(u32 b, u32 n, void *buf)
{
    u8 *p = buf;
    if (b >= nblocks || n > nblocks - b) return -EIO;
    u32 lba = b * 4, left = n * 4;
    while (left) {
        struct umsg m;
        u32 k = left < maxsect ? left : maxsect;
        m.type = DISK_READ; m.a[0] = (u32)dh; m.a[1] = lba; m.a[2] = k; m.a[3] = (u32)p;
        int r = disk_call(dport, &m);
        if (r < 0) return r == -ESTALE ? r : -EIO;
        lba += k; left -= k; p += k * 512;
    }
    return 0;
}

/* directory and descriptor blocks, in a small cache; a pointer lasts until the next call */
struct cblk { u32 b, stamp; int valid; };
static struct cblk cache[NCACHE];
static u8 *cmem, *win;
static u32 cstamp, win_b, win_n;
static int stale;                      /* the disc was changed under the mount (diskd's -ESTALE): nothing more is read */
static u8 *blk(u32 b)
{
    struct cblk *c, *pick = 0;
    if (stale) return 0;
    for (u32 i = 0; i < NCACHE; i++) {
        c = &cache[i];
        if (c->valid && c->b == b) { c->stamp = ++cstamp; return cmem + i * BLK; }
        if (!pick || (pick->valid && (!c->valid || c->stamp < pick->stamp))) pick = c;
    }
    pick->valid = 0;
    int r = dread(b, 1, cmem + (u32)(pick - cache) * BLK);
    if (r) { if (r == -ESTALE) stale = 1; return 0; }
    pick->b = b; pick->valid = 1; pick->stamp = ++cstamp;
    return cmem + (u32)(pick - cache) * BLK;
}
static int err(void) { return stale ? -ESTALE : -EIO; }

/* ---- dates: the record's seven bytes (years since 1900, month, day, hour, minute, second, offset from GMT in 15
 * minutes) as Unix time; 0 for a date that cannot be ---- */
static u32 iso_time(const u8 *t)
{
    int y = 1900 + t[0], mo = t[1], d = t[2];
    if (y < 1970 || y > 2105 || mo < 1 || mo > 12 || d < 1 || d > 31 || t[3] > 23 || t[4] > 59 || t[5] > 60) return 0;
    int days = (int)zdays(y, mo, d);
    u32 v = (u32)days * 86400u + t[3] * 3600u + t[4] * 60u + t[5], z = (u32)((signed char)t[6] * 900);
    return (signed char)t[6] > 0 && v < z ? 0 : v - z;
}

/* ---- directory records ---- */
typedef struct {
    u32 pos;                           /* the record's byte position on the volume (its inode number * 2) */
    u32 ext, size, mtime, mode;        /* mode: Rock Ridge's PX, 0 without */
    u8 kind, dot, hide;                /* dot: "." or ".."; hide: an associated file, or a directory Rock Ridge moved (RE) */
    u32 nlen;                          /* the name as programs see it (UTF-8, no ';' version, no '/'), NUL-terminated */
    char name[NAMEMAX + 1];
    u32 pl;                            /* Rock Ridge PL: the real parent of a moved directory ("..") */
} rec_t;
#define W_NAME 1
#define W_LINK 2
static char ltext[NAMEMAX + 1];        /* the last W_LINK parse's Rock Ridge SL text */
static u32 llen;
static int lbad;

/* one character into the name: C as UTF-8 (up to U+FFFF), or with RAW the byte C as it is (Rock Ridge names are bytes:
 * UTF-8 from today's tools); too long: nlen past NAMEMAX */
static void name_put(rec_t *r, u32 c, int raw)
{
    if (c < 0x20 || c == '/') c = '_';
    u8 t[3];
    u32 n = c < 0x80 || raw ? (t[0] = (u8)c, 1) : c < 0x800 ? (t[0] = (u8)(0xC0 | c >> 6), t[1] = (u8)(0x80 | (c & 0x3F)), 2)
                                                    : (t[0] = (u8)(0xE0 | c >> 12), t[1] = (u8)(0x80 | (c >> 6 & 0x3F)), t[2] = (u8)(0x80 | (c & 0x3F)), 3);
    for (u32 i = 0; i < n; i++) {
        if (r->nlen < NAMEMAX) r->name[r->nlen] = (char)t[i];
        r->nlen++;
    }
}
static void link_put(const char *s, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        if (!s[i]) lbad = 1;
        if (llen < NAMEMAX) ltext[llen] = s[i]; else lbad = 1;
        llen++;
    }
}

/* Rock Ridge (SUSP) entries of a record's system use area A, N bytes: NM, PX, SL, CL, PL, RE; a CE continues the area in
 * another block. 0, or -EIO */
static int susp(const u8 *a, u32 n, rec_t *r, int want, int *nm)
{
    for (int hops = 0; ; hops++) {
        u32 ce_b = 0, ce_off = 0, ce_len = 0, i = 0;
        while (i + 4 <= n) {
            u32 l = a[i + 2], s = g16(a + i);
            if (l < 4 || l > n - i) return -EIO;
            const u8 *e = a + i;
            if (s == 0x4E4D && l >= 5 && (want & W_NAME) && !(e[4] & 6)) {          /* NM */
                if (!*nm) { *nm = 1; r->nlen = 0; }
                for (u32 k = 5; k < l; k++) name_put(r, e[k], 1);
            } else if (s == 0x5058 && l >= 36) r->mode = g32(e + 8);              /* PX */
            else if (s == 0x534C && l >= 5 && (want & W_LINK)) {                  /* SL */
                if (r->kind != K_LINK) { r->kind = K_LINK; llen = 0; lbad = 0; }
                for (u32 k = 5; k + 2 <= l; ) {
                    u32 cf = e[k], cl = e[k + 1];
                    if (cl > l - k - 2) return -EIO;
                    if (cf & 8) { llen = 0; link_put("/", 1); }
                    else if (cf & 2) link_put(".", 1);
                    else if (cf & 4) link_put("..", 2);
                    else link_put((const char *)e + k + 2, cl);
                    if (!(cf & 9)) link_put("/", 1);
                    k += 2 + cl;
                }
            } else if (s == 0x534C) r->kind = K_LINK;
            else if (s == 0x434C && l >= 12) { r->ext = g32(e + 8); r->kind = K_DIR; r->size = 0; }   /* CL: the moved directory */
            else if (s == 0x504C && l >= 12) r->pl = g32(e + 8);                  /* PL */
            else if (s == 0x5245) r->hide = 1;                                    /* RE */
            else if (s == 0x4345 && l >= 28) { ce_b = g32(e + 8); ce_off = g32(e + 16); ce_len = g32(e + 24); }
            else if (s == 0x5354) break;                                         /* ST */
            i += l;
        }
        if (!ce_len) return 0;
        if (hops >= MAX_CE || ce_off >= BLK || ce_len > BLK - ce_off) return -EIO;
        if (!(a = blk(ce_b))) return -EIO;
        a += ce_off; n = ce_len;
    }
}

/* the record at volume byte POS (inside one block, at D): checked, then what WANT asks for. 0, or -EIO */
static int rec_parse(u32 pos, const u8 *d, rec_t *r, int want)
{
    u32 len = d[D_LEN], il;
    if (len < 34 || (len & 1) || (pos & 2047) + len > BLK) return -EIO;
    il = d[D_ILEN];                                     /* (only now: the record is inside its block) */
    if (D_ID + il > len || !il) return -EIO;
    r->pos = pos;
    r->ext = g32(d + D_EXT) + d[D_XAR]; r->size = g32(d + D_SIZE);
    r->mtime = iso_time(d + D_DATE);
    r->mode = 0; r->pl = 0; r->hide = (d[D_FLAGS] & F_ASSOC) != 0;
    r->kind = d[D_FLAGS] & F_DIR ? K_DIR : (d[D_FLAGS] & F_MULTI) || d[D_UNIT] || d[D_UNIT + 1] ? K_BIG : K_FILE;
    r->dot = il == 1 && d[D_ID] <= 1;
    r->nlen = 0;
    if (r->kind == K_DIR && d[D_XAR]) return -EIO;     /* never made: a directory's data starts at its "." record */
    int nm = 0;
    if (rr) {
        u32 s = D_ID + il + !(il & 1) + rr_skip;       /* the system use area, after the name's pad byte */
        if (s < len && susp(d + s, len - s, r, want, &nm)) return -EIO;
        if (r->kind == K_LINK && (want & W_LINK)) {
            if (lbad || !llen) return -EIO;
            if (llen > 1 && ltext[llen - 1] == '/') llen--;
            ltext[llen] = 0;
        }
    }
    if ((want & W_NAME) && !nm && !r->dot) {
        const u8 *id = d + D_ID;
        if (joliet) {                                   /* UCS-2, big-endian */
            for (u32 k = 0; k + 1 < il && g16(id + k) != ';'; k += 2) name_put(r, g16(id + k), 0);
        } else {
            u32 n = il;
            if (r->kind != K_DIR) for (n = 0; n < il && id[n] != ';'; n++) ;
            for (u32 k = 0; k < n; k++) name_put(r, id[k], 0);       /* Latin-1 */
        }
        if (r->nlen > 1 && r->nlen <= NAMEMAX && r->name[r->nlen - 1] == '.') r->nlen--;   /* "README." is README */
    }
    if (r->nlen > NAMEMAX) return -EIO;
    r->name[r->nlen] = 0;
    if (r->kind == K_DIR) {                             /* a directory: its extent all on the volume, its "." there; at
                                                         * most 16 MB (a listing's place is 31 bits; no disc has more) */
        if (r->ext >= nblocks || r->size > MAXDIR || (r->size && (r->size - 1) / BLK >= nblocks - r->ext)) return -EIO;
    } else if (r->size && (r->ext >= nblocks || (r->size - 1) / BLK >= nblocks - r->ext)) return -EIO;
    return 0;                                           /* (an empty file's extent is not looked at: tools write 0 there) */
}

/* the record at volume byte POS. 0, or -EIO */
static int rec_at(u32 pos, rec_t *r, int want)
{
    const u8 *d = blk(pos >> 11);
    if (!d) return err();
    return rec_parse(pos, d + (pos & 2047), r, want) ? -EIO : 0;
}

/* a directory, by its inode number: its "." record, at the start of its extent and naming that extent. 0, -ENOTDIR (a
 * record of something else), -EIO */
static int dir_node(u32 ino, rec_t *r)
{
    u32 pos = ino << 1;
    if (!ino || ino >= MAXVOL << 10 || (pos & 2047)) return -ENOTDIR;
    int e = rec_at(pos, r, 0);
    if (e) return e;
    if (r->kind != K_DIR || !r->dot || r->ext != pos >> 11 || r->size < 68) return -ENOTDIR;
    return 0;
}
static u32 ino_of(const rec_t *r) { return r->kind == K_DIR ? r->ext << 10 : r->pos >> 1; }

/* the next record of directory D from byte OFF of it (skipping a block's padding); its place after it into *NEXT.
 * 1: one, 0: the end, -EIO: damage (*NEXT is then the next block's start) */
static int dir_next(const rec_t *dir, u32 off, u32 *next, rec_t *r, int want)
{
    for (;;) {
        if (off >= dir->size) return 0;
        const u8 *d = blk(dir->ext + (off >> 11));
        u32 o = off & 2047;
        *next = (off | 2047) + 1;
        if (!d) return err();
        u32 len = d[o];
        if (!len) { off = *next; continue; }           /* a record never crosses a block: the rest is padding */
        if (off + len > dir->size || rec_parse(dir->ext * BLK + off, d + o, r, want)) return -EIO;
        *next = off + len;
        return 1;
    }
}

/* the parent of directory DIR (its ".." record, or Rock Ridge's PL for a moved one) */
static int dir_parent(const rec_t *dir, u32 *out)
{
    rec_t p;
    u32 off = 0, nx;
    if (dir->ext == root_ext) { *out = root_ext << 10; return 0; }
    for (int i = 0; i < 2; i++, off = nx)
        if (dir_next(dir, off, &nx, &p, 0) != 1) return -EIO;
    if (!p.dot || p.kind != K_DIR) return -EIO;
    *out = (p.pl ? p.pl : p.ext) << 10;
    return 0;
}

/* a name for a listing: one longer than a udirent's 63 bytes is its start, ~ and the inode number in hex, which opens it */
static void list_name(const rec_t *r, char *out)
{
    u32 k = r->nlen <= 63 ? r->nlen : 52, ino = ino_of(r);
    while (k < r->nlen && k && ((u8)r->name[k] & 0xC0) == 0x80) k--;     /* not inside a character */
    memcpy(out, r->name, k);
    if (k < r->nlen) {
        out[k++] = '~';
        int s = 28;
        while (s > 0 && !(ino >> s)) s -= 4;
        for (; s >= 0; s -= 4) out[k++] = "0123456789abcdef"[(ino >> s) & 15];
    }
    out[k] = 0;
}
static int same_name(const rec_t *r, const char *q, u32 ql)
{
    if (r->nlen != ql) return 0;
    for (u32 i = 0; i < ql; i++) {
        u32 a = (u8)r->name[i], b = (u8)q[i];
        if (a != b && (rr || (a | 0x20) != (b | 0x20) || (a | 0x20) < 'a' || (a | 0x20) > 'z')) return 0;
    }
    return 1;                           /* Rock Ridge names are exact; ISO and Joliet names ignore ASCII case */
}

/* NAME (QL bytes) in directory DIR into R. 0, -ENOENT, -EIO (damage on the way, and not found) */
static int lookup(const rec_t *dir, const char *q, u32 ql, rec_t *r)
{
    u32 off = 0, nx;
    int bad = 0, k;
    char ln[64];
    while ((k = dir_next(dir, off, &nx, r, W_NAME)) != 0) {
        off = nx;
        if (k < 0) { if (stale) return -ESTALE; bad = 1; continue; }
        if (r->dot || r->hide) continue;
        if (!same_name(r, q, ql)) {
            if (r->nlen <= 63) continue;
            list_name(r, ln);
            if (strlen(ln) != ql || memcmp(ln, q, ql)) continue;
        }
        return rec_at(r->pos, r, W_NAME | W_LINK);
    }
    return bad ? -EIO : -ENOENT;
}

/* ---- modes ---- */
static u32 mode_of(const rec_t *r)
{
    u32 perm = r->kind == K_DIR ? m_dmode : m_fmode;
    if (r->mode && !o_norock) perm = r->mode & 07777 & ~06000u;      /* Rock Ridge's, without set-id: a disc is not trusted */
    return (r->kind == K_DIR ? S_IFDIR : r->kind == K_LINK ? 0120000 : S_IFREG) | (r->kind == K_LINK ? 0777 : perm);
}
static int may(const rec_t *r, int want)
{
    return fsp_access(cuid, cgid, cng, cgs, mode_of(r), m_uid, m_gid, want);
}

/* ---- paths ---- */
static const char *lk_path;            /* where a walk stopped at a link (D26): the path, the name's bytes, the text */
static u32 lk_s, lk_e;

/* walk PATH's first LEN bytes from directory CWD (an inode number); the end in N. Links are followed inside the volume
 * (an absolute one from its root; not the last name unless FOLLOW), or with stoplinks the walk ends at the first one with
 * -FS_LINKED (its text in ltext) */
static int walk(u32 cwd, const char *path, u32 len, int follow, int depth, rec_t *n)
{
    static rec_t cur;
    u32 i = 0;
    int r;
    if (depth > MAX_FOLLOW) return -ELOOP;
    if ((r = dir_node(path[0] == '/' ? root_ext << 10 : cwd, &cur))) return r == -ENOTDIR && !stale ? -ESTALE : r;
    for (;;) {
        while (i < len && path[i] == '/') i++;
        if (i >= len) { *n = cur; return 0; }
        u32 s = i, k;
        while (i < len && path[i] != '/') i++;
        k = i;
        while (k < len && path[k] == '/') k++;
        int last = k >= len;
        if (!may(&cur, FSP_X)) return -EACCES;
        if (i - s == 1 && path[s] == '.') continue;
        if (i - s == 2 && path[s] == '.' && path[s + 1] == '.') {
            u32 p;
            if ((r = dir_parent(&cur, &p)) || (r = dir_node(p, &cur))) return r == -ENOTDIR ? -EIO : r;
            continue;
        }
        if ((r = lookup(&cur, path + s, i - s, n))) return r;
        if (n->kind == K_LINK && (!last || follow)) {
            if (stoplinks && path[0] == '/') {
                lk_path = path; lk_s = s; lk_e = i;
                return -FS_LINKED;
            }
            char t[NAMEMAX + 1];
            if (--links_left < 0) return -ELOOP;
            memcpy(t, ltext, llen + 1);
            /* the text is relative to the directory that holds the link; an absolute one starts at this volume's root */
            if ((r = walk(ino_of(&cur), t, llen, 1, depth + 1, n))) return r;
        }
        if (last) return 0;
        if (n->kind != K_DIR) return -ENOTDIR;
        if ((r = dir_node(ino_of(n), &cur))) return r == -ENOTDIR ? -EIO : r;
    }
}

/* the directory that holds PATH's last name, into D; that name must exist (WANT 1), must not (0), or either (-1) */
static int refuse_at(u32 cwd, const char *path, int want)
{
    static rec_t d, t;
    u32 n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    u32 s = n;
    while (s > 0 && path[s - 1] != '/') s--;
    if (n == s || (n - s == 1 && path[s] == '.') || (n - s == 2 && path[s] == '.' && path[s + 1] == '.')) return -EINVAL;
    int r = walk(cwd, path, s, 1, 0, &d);
    if (r) return r;
    if (d.kind != K_DIR) return -ENOTDIR;
    if (!may(&d, FSP_X)) return -EACCES;
    if (want < 0) return -EROFS;
    r = lookup(&d, path + s, n - s, &t);
    if (r && r != -ENOENT) return r;
    if (want && r) return r;
    if (!want && !r) return -EEXIST;
    return -EROFS;
}

/* ---- file data ---- */
/* N bytes of the file R from OFF into BUF: whole blocks straight into BUF, the rest through a window of up to NWIN of the
 * file's blocks */
static int file_read(const rec_t *r, u32 off, u8 *buf, u32 n)
{
    if (r->kind != K_FILE) return r->kind == K_DIR ? -EISDIR : r->kind == K_BIG ? -EOPNOTSUPP : -EINVAL;
    if (off >= r->size) return 0;
    if (n > r->size - off) n = r->size - off;
    u32 fb = (r->size + BLK - 1) / BLK, done = 0;
    while (done < n) {
        u32 p = off + done, b = p / BLK, o = p % BLK, k = n - done;
        if (!o && k >= BLK) {
            u32 nb = k / BLK;
            if (nb > 32) nb = 32;
            int e = dread(r->ext + b, nb, buf + done);
            if (e) { if (e == -ESTALE) stale = 1; return err(); }
            done += nb * BLK;
            continue;
        }
        if (!win_n || r->ext + b < win_b || r->ext + b >= win_b + win_n) {
            u32 m = fb - b < NWIN ? fb - b : NWIN;
            win_n = 0;
            int e = dread(r->ext + b, m, win);
            if (e) { if (e == -ESTALE) stale = 1; return err(); }
            win_b = r->ext + b; win_n = m;
        }
        u32 c = BLK - o < k ? BLK - o : k;
        memcpy(buf + done, win + (r->ext + b - win_b) * BLK + o, c);
        done += c;
    }
    return (int)n;
}

/* ---- requests ---- */
static void fill(const rec_t *r, struct ustat *st)
{
    st->ino = ino_of(r);
    st->mode = mode_of(r);
    st->uid = m_uid; st->gid = m_gid;
    st->mtime = r->mtime;
    st->size = r->kind == K_LINK ? llen : r->size;
    st->links = r->kind == K_DIR ? 2 : 1;
}
/* the object an inode number names: a directory, or the record of anything else */
static int node_of(u32 ino, rec_t *r)
{
    int e = dir_node(ino, r);
    if (e != -ENOTDIR) return e;
    if (!ino || ino >= MAXVOL << 10) return -ESTALE;
    if ((e = rec_at(ino << 1, r, W_LINK))) return e;
    return r->kind == K_DIR || r->dot ? -ESTALE : 0;
}
static int do_open(struct umsg *m)
{
    static rec_t n;
    const char *path = (const char *)m->a[0];
    u32 flags = m->a[2];
    int r = walk(m->a[1], path, strlen(path), !((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)), 0, &n);
    if (r == -ENOENT && (flags & O_CREAT)) return (r = refuse_at(m->a[1], path, 0)) == -EEXIST ? -EROFS : r;
    if (r) return r;
    if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) return -EEXIST;
    if (n.kind == K_LINK) return -ELOOP;
    if (n.kind == K_DIR && (flags & (O_WRONLY | O_RDWR))) return -EISDIR;
    if (!may(&n, fsp_open_want(flags))) return -EACCES;
    if (flags & (O_WRONLY | O_RDWR | O_TRUNC)) return -EROFS;
    if (n.kind == K_BIG) return -EOPNOTSUPP;
    if (n.kind == K_DIR && (r = dir_node(ino_of(&n), &n))) return r == -ENOTDIR ? -EIO : r;   /* its own "." agrees */
    fill(&n, (struct ustat *)m->a[3]);
    return (int)ino_of(&n);
}
/* a listing's place: the byte in the directory where the next record starts; bit 31: damage was met (answered -EIO at
 * the end, so what can be read is listed) */
static int do_readdir(struct umsg *m)
{
    static rec_t d, e;
    struct udirent *ud = (struct udirent *)m->a[2];
    u32 cookie = m->a[1], bad = cookie >> 31, off = cookie & 0x7FFFFFFF, nx;
    u32 *newc = (u32 *)m->a[3];
    int r = dir_node(m->a[0], &d);
    if (r) return r == -ENOTDIR ? -ENOTDIR : r;
    for (;;) {
        r = dir_next(&d, off, &nx, &e, W_NAME);
        if (r == 0) { *newc = cookie; return bad ? -EIO : 0; }
        off = nx;
        if (r < 0) { if (stale) return -ESTALE; bad = 1; continue; }
        if (e.dot || e.hide) continue;
        list_name(&e, ud->name);
        ud->type = e.kind == K_DIR ? 2 : e.kind == K_LINK ? 7 : 1;
        ud->ino = ino_of(&e);
        *newc = bad << 31 | off;
        return 1;
    }
}
/* DESIGN D49: FS_READDIRS, do_readdir's records in a batch, each with FS_LSTAT's answer when the caller may search this
 * directory: a file's from its record, a directory's from its own "." (as FS_LSTAT), none for a Rock Ridge link (its
 * size is the link text's, read only by a walk) */
static int do_readdirs(struct umsg *m)
{
    static rec_t d, e, t;
    static char nm[64];
    struct fsrdx *q = (struct fsrdx *)m->a[3];
    u32 cookie = m->a[1], bad = cookie >> 31, off = cookie & 0x7FFFFFFF, nx, at;
    unsigned used = 0;
    int r = dir_node(m->a[0], &d);
    if (r) return r;
    int x = may(&d, FSP_X);
    for (;;) {
        at = off;
        r = dir_next(&d, off, &nx, &e, W_NAME);
        if (r == 0) { r = bad ? -EIO : 0; break; }
        off = nx;
        if (r < 0) { if (stale) { off = at; r = -ESTALE; break; } bad = 1; continue; }
        if (e.dot || e.hide) continue;
        list_name(&e, nm);
        if (!nm[0]) continue;                       /* an empty name (damage) is never listed */
        if (used && (m->kflags & UMSG_CANCEL)) { off = at; break; }   /* a killed client: what is written */
        struct udirx *y = udirx_put((char *)m->a[2], q->size, &used, nm, ino_of(&e),
                                    e.kind == K_DIR ? 2 : e.kind == K_LINK ? 7 : 1, bad << 31 | off);
        if (!y) { off = at; break; }                /* full: this record comes first next time */
        if (x && e.kind != K_LINK) {
            if (e.kind != K_DIR) { fill(&e, (struct ustat *)&y->st); y->flags = UDX_STAT; }
            else if (!dir_node(ino_of(&e), &t)) { fill(&t, (struct ustat *)&y->st); y->flags = UDX_STAT; }
        }
    }
    q->next = bad << 31 | off;
    return used ? (int)used : r;
}
static int handle1(struct umsg *m)
{
    static rec_t n;
    int r;
    const char *p = (const char *)m->a[0];
    switch (m->type) {
    case FS_OPEN: return do_open(m);
    case FS_CLOSE: return 0;
    case FS_READ:
        if ((r = node_of(m->a[0], &n))) return r;
        return file_read(&n, m->a[1], (u8 *)m->a[2], m->a[3]);
    case FS_STAT: case FS_LSTAT:
        if ((r = walk(m->a[1], p, strlen(p), m->type == FS_STAT, 0, &n))) return r;
        if (n.kind == K_DIR && (r = dir_node(ino_of(&n), &n))) return r == -ENOTDIR ? -EIO : r;  /* as fstat: its "." */
        fill(&n, (struct ustat *)m->a[2]);
        return 0;
    case FS_FSTAT:
        if ((r = node_of(m->a[0], &n))) return r;
        fill(&n, (struct ustat *)m->a[1]);
        return 0;
    case FS_READDIR: return do_readdir(m);
    case FS_READDIRS: return do_readdirs(m);
    case FS_CHDIR:
        if ((r = walk(m->a[1], p, strlen(p), 1, 0, &n))) return r;
        if (n.kind != K_DIR) return -ENOTDIR;
        if (!may(&n, FSP_X)) return -EACCES;
        return (r = dir_node(ino_of(&n), &n)) ? (r == -ENOTDIR ? -EIO : r) : (int)ino_of(&n);
    /* changes: none, on a read-only medium; the kernel still needs to know where a path leads (D26) */
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
        if ((r = walk(m->a[2], p, strlen(p), 1, 0, &n))) return r;
        return refuse_at(m->a[2], (const char *)m->a[1], 0);
    case FS_SYMLINK:
        return refuse_at(m->a[2], (const char *)m->a[1], 0);
    case FS_CHMOD: case FS_CHOWN: case FS_UTIMES:
        return (r = walk(m->a[1], p, strlen(p), 1, 0, &n)) ? r : -EROFS;
    case FS_STATFS: {
        struct ustatfs *f = (struct ustatfs *)m->a[0];
        f->bsize = BLK; f->blocks = nblocks; f->bfree = 0; f->files = 0; f->ffree = 0;
        return 0;
    }
    case FS_SYNC:
        return 0;
    case FS_READLINK:
        if ((r = walk(m->a[1], p, strlen(p), 0, 0, &n))) return r;
        if (n.kind != K_LINK) return -EINVAL;
        r = (int)(llen < m->a[3] ? llen : m->a[3]);
        memcpy((char *)m->a[2], ltext, (u32)r);
        return r;
    case FS_ROOT: {
        struct fsinfo *fi = (struct fsinfo *)m->a[0];
        strncpy(fi->dev, devname, sizeof fi->dev - 1);
        strcpy(fi->type, "iso9660");
        return (int)(root_ext << 10);
    }
    case FS_QUIT:
        quitting = 1;
        return 0;
    }
    return -ENOSYS;
}
/* the FS_LINKED answer (kernel/fsproto.h): the continuation written over the path argument the walk stopped in */
static int linked(struct umsg *m)
{
    int a1 = (m->type == FS_RENAME || m->type == FS_LINK || m->type == FS_SYMLINK) && lk_path == (const char *)m->a[1];
    char *p = (char *)m->a[a1];
    char out[FS_PATHMAX];
    int r;
    if ((r = fsl_text(out, p, lk_s, lk_e, ltext, llen))) return r;
    u32 e = lk_e;
    while (p[e] == '/') e++;
    int last = m->type == FS_OPEN && !p[e];
    strcpy(p, out);
    return -(FS_LINKED + (a1 ? FSL_A1 : 0) + (last ? FSL_LAST : 0));
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
    stoplinks = (m->kflags & UMSG_LINKS) != 0;
    links_left = 40;
    int r = handle1(m);
    return r == -FS_LINKED ? linked(m) : r;
}

/* ---- the mount ---- */
static int why(const char *t)
{
    dprintf(1, "iso9660d: %s: %s\n", devname, t);
    return -1;
}
static int try_mount(const char *name, int restart)
{
    struct umsg m;
    struct diskinfo di;
    static rec_t r;
    (void)restart;
    strncpy(devname, name, sizeof devname - 1);
    if (dport < 0 && (dport = sys_port_find("disk")) < 0) return -1;
    m.type = DISK_OPEN; m.a[0] = (u32)name; m.a[1] = DISK_MOUNT;
    if ((dh = disk_call(dport, &m)) < 0) return why("in use, or no such device");
    m.type = DISK_INFOSZ; m.a[0] = (u32)dh; m.a[1] = (u32)&di; m.a[2] = sizeof di;
    if (disk_call(dport, &m) < 0) { why("cannot read the device"); goto no; }
    maxsect = di.maxsect >= 4 ? di.maxsect & ~3u : 4;
    nblocks = di.sectors / 4;
    if (!cmem) { cmem = malloc(NCACHE * BLK); win = malloc(NWIN * BLK); }
    if (!cmem || !win) { why("out of memory"); goto no; }
    if (nblocks < 18) { why(di.sectors ? "too small for an ISO 9660 volume" : "no disc in the drive"); goto no; }
    /* the volume descriptors from block 16: the primary one, a Joliet one, up to the set's end */
    u32 pvd = 0, svd = 0, b;
    for (b = 16; b < 16 + 64 && b < nblocks; b++) {
        const u8 *d = blk(b);
        if (!d || memcmp(d + 1, "CD001", 5) || d[6] != 1) break;
        if (d[0] == 255) break;
        if (d[0] == 1 && !pvd) pvd = b;
        if (d[0] == 2 && !svd && d[88] == '%' && d[89] == '/' && (d[90] == '@' || d[90] == 'C' || d[90] == 'E')) svd = b;
    }
    if (!pvd) { why(b == 16 ? "not an ISO 9660 volume (no CD001 at block 16)" : "no primary volume descriptor: damaged"); goto no; }
    const u8 *d = blk(pvd);
    if (!d || g16(d + 130) != BLK) { why("logical blocks other than 2048 bytes: refused"); goto no; }
    u32 vs = g32(d + 84);
    if (vs < nblocks) nblocks = vs;                     /* the volume, not what the drive says lies after it */
    if (nblocks > MAXVOL) { why("larger than 4 GB: refused"); goto no; }
    if (rec_parse(pvd * BLK + 156, d + 156, &r, 0) || r.kind != K_DIR) { why("its root directory record is damaged"); goto no; }
    root_ext = r.ext;
    if (dir_node(root_ext << 10, &r)) { why("its root directory is damaged"); goto no; }
    /* Rock Ridge: the root's "." record begins its system use area with SP (BE EF, then the bytes to skip in every
     * record's area) */
    d = blk(root_ext);
    if (!o_norock && d && d[0] >= 41 && d[32] == 1 && !memcmp(d + 34, "SP\7\1\xBE\xEF", 6)) { rr = 1; rr_skip = d[40]; }
    else if (!o_nojoliet && svd) {
        d = blk(svd);
        if (d && g16(d + 130) == BLK && !rec_parse(svd * BLK + 156, d + 156, &r, 0) && r.kind == K_DIR && !dir_node(r.ext << 10, &r)) {
            joliet = 1; root_ext = r.ext;
        }
    }
    if (rr_skip > 200) { why("its Rock Ridge marker is damaged"); goto no; }
    return 0;
no:
    m.type = DISK_UNMOUNT; m.a[0] = (u32)dh;
    disk_call(dport, &m);
    dh = -1;
    return -1;
}

/* -o uid=N,gid=N,fmode=OCT,dmode=OCT,norock,nojoliet,ro,defaults,noauto */
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
        else if (word_is(o, "norock")) o_norock = 1;
        else if (word_is(o, "nojoliet")) o_nojoliet = 1;
        else if (word_is(o, "rw")) { dprintf(1, "iso9660d: an ISO 9660 volume is read-only: -o rw is refused\n"); return -1; }
        else if (!(word_is(o, "ro") || word_is(o, "defaults") || word_is(o, "noauto"))) bad = 1;
        if (bad) { dprintf(1, "iso9660d: bad option at: %s (uid=, gid=, fmode=, dmode=, norock, nojoliet, ro)\n", o); return -1; }
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
    if (argc < 2) { dprintf(1, "usage: iso9660d DEVICE [-id START,SIZE] [-o OPTIONS]\n"); return 2; }
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
    if (port < 0) { dprintf(1, "iso9660d: cannot create the port %s\n", pname); return 1; }
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
