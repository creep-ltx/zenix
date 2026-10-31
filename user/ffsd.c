/* SPDX-License-Identifier: GPL-2.0-only */
#ifdef ZHOST
#include "../tools/host/zhost.h"           /* the host tests (tools/host/fsd-host.c built with -DFFSD) */
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
#include "rdb.h"

/* ffsd DEVICE -id START,SIZE [-o OPTIONS]: the AmigaDOS filesystem server (OFS and FFS, DOS\0 .. DOS\7), a file service
 * started by mount(8) as fs.DEVICE. The decisions and why: docs/d31-ffs.md. In short:
 * - Read-only unless -o rw; only DOS\0 .. DOS\3 are ever written, and only after a check of the whole volume.
 * - Everything read from the disk is untrusted: a damaged volume answers an error, never crashes or hangs.
 * - The order of writes: new blocks, the bitmap, changed extension blocks, changed headers and directories; a block goes
 *   free in the bitmap only after nothing on the disk names it. The root's "bitmap valid" flag is never cleared. So a
 *   power cut leaves a valid volume with at worst lost blocks. */

#define NOPEN       64                 /* NFILE: an open file has a slot here */
#define NPEND       16                 /* removed entries waiting for the commit */
#define COMMIT_TICKS 300               /* 3 s after the first change */
#define CHECK_EXT   256                /* extension blocks walked at a file's first open (9 MB of file at 512 bytes) */
#define MAX_SIZE    0x7FFFFFFFu        /* the format's file sizes are signed */
#define T_HEADER    2u
#define T_DATA      8u
#define T_LIST      16u
#define ST_ROOT     1u
#define ST_DIR      2u
#define ST_SOFT     3u
#define ST_LDIR     4u
#define ST_FILE     0xFFFFFFFDu
#define ST_LFILE    0xFFFFFFFCu
#define P_DELETE    1u                 /* the protection bits, active low: set means refused */
#define P_EXEC      2u
#define P_WRITE     4u
#define P_READ      8u
#define P_ARCHIVE   16u
#define AMIGA_EPOCH 252460800u         /* 1978-01-01 in Unix time */
/* the longs of a header counted from the end of the block */
#define O_HIGHSEQ   2u
#define O_HSIZE     3u
#define O_FIRST     4u
#define O_SUM       5u
#define O_TABLE     6u
#define O_BMFLAG    (LW - 50)
#define O_BMPAGES   (LW - 49)
#define O_PROT      (LW - 48)
#define O_SIZE      (LW - 47)
#define O_BMEXT     (LW - 24)
#define O_DAYS      (LW - 23)
#define O_DAYS_LN   (LW - 15)
#define O_REAL      (LW - 11)
#define O_NEXTLINK  (LW - 10)
#define O_VOLDATE   (LW - 10)
#define O_CHAIN     (LW - 4)
#define O_PARENT    (LW - 3)
#define O_EXT       (LW - 2)
#define O_SEC       (LW - 1)

/* the volume (all checked at mount) */
static u32 BS, LW, HT, SPB, nblocks, reserved, root_blk, nbm, PB;      /* PB: blocks a bitmap block covers */
static u32 dostype;
static int is_ofs, is_intl, is_ln;
static u8 volname[31];
static u32 volname_len;
static int bm_valid;
/* the mount */
static int dport = -1, dh = -1;
static u32 maxsect = 1, dev_sectors;
static u32 m_uid, m_gid, m_fmode = 0755, m_dmode = 0755, o_bs, o_res;
static int readonly = 1, nocheck;
static char devname[16];
static unsigned cuid, cgid, cng;
static unsigned short cgs[NGROUPS];
static int stoplinks, quitting;
/* state */
static int dirty, vol_changed;
static u32 dirty_since, free_cnt = 0xFFFFFFFFu, next_free;
static u32 pend[NPEND], npend;
static u8 *sb;                         /* one block of scratch: what a free walks */

#ifdef ZHOST
static u32 get32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
#else
static u32 get32(const u8 *p) { return *(const u32 *)p; }
static void put32(u8 *p, u32 v) { *(u32 *)p = v; }
#endif
#define GL(d, i) get32((d) + 4 * (i))
#define SL(d, i, v) put32((d) + 4 * (i), (v))

static u32 sum_of(const u8 *d)
{
    u32 s = 0;
    for (u32 i = 0; i < LW; i++) s += GL(d, i);
    return s;
}

/* ---- the disk ---- */
static int need_flush;                 /* something was written since the last disk flush */
static int dflush(void)
{
    struct umsg m;
    need_flush = 0;
    m.type = DISK_FLUSH; m.a[0] = (u32)dh;
    int r = disk_call(dport, &m);
    return r < 0 && r != -95 ? -EIO : 0;              /* -95: the disk cannot flush */
}
/* N blocks at BLOCK, in pieces the device takes. A write never reaches the reserved blocks */
static int dio(u32 type, u32 block, u32 n, void *buf)
{
    u8 *p = buf;
    if (block >= nblocks || n > nblocks - block) return -EIO;
    if (type == DISK_WRITE) {
        if (block < reserved) return -EIO;
        need_flush = 1;
    }
    u32 lba = block * SPB, left = n * SPB;
    while (left) {
        struct umsg m;
        u32 k = left < maxsect ? left : maxsect;
        m.type = type; m.a[0] = (u32)dh; m.a[1] = lba; m.a[2] = k; m.a[3] = (u32)p;
        if (disk_call(dport, &m) < 0) return -EIO;
        lba += k; left -= k; p += k * 512;
    }
    return 0;
}

/* ---- the block cache: one list of blocks, written in the order of the kinds ---- */
enum { K_NEW, K_BM, K_EXT, K_REF, K_KINDS };
enum { SUM_NONE, SUM_HDR, SUM_BM };
struct cbuf { u32 blk, stamp; u8 kind, dirty, valid, ok, sum, pad[3]; u8 *d; };
static struct cbuf *cache;
static u32 ncache, cstamp;

static int cache_alloc(void)
{
    ncache = 12288 / BS;
    if (ncache < 8) ncache = 8;
    cache = malloc(ncache * sizeof *cache);
    u8 *mem = malloc(ncache * BS);
    sb = malloc(BS);
    if (!cache || !mem || !sb) return -1;
    memset(cache, 0, ncache * sizeof *cache);
    for (u32 i = 0; i < ncache; i++) cache[i].d = mem + i * BS;
    return 0;
}
static void seal(struct cbuf *c)
{
    if (c->sum == SUM_NONE) return;
    u32 at = c->sum == SUM_HDR ? O_SUM : 0;
    SL(c->d, at, 0);
    SL(c->d, at, 0 - sum_of(c->d));
}
static int flush_kind(int kind)
{
    int r;
    for (u32 i = 0; i < ncache; i++) {
        struct cbuf *c = &cache[i];
        if (!c->valid || !c->dirty || c->kind != kind) continue;
        seal(c);
        if ((r = dio(DISK_WRITE, c->blk, 1, c->d))) return r;
        c->dirty = 0;
    }
    return need_flush ? dflush() : 0;                 /* direct writes count: what follows must not overtake them */
}
static int flush_upto(int kind)
{
    int r;
    for (int k = 0; k <= kind; k++) if ((r = flush_kind(k))) return r;
    return 0;
}
static int flush_all(void) { return flush_upto(K_REF); }

/* BLOCK in the cache; NOREAD: the caller overwrites all of it. 0 on an I/O error. The last two blocks asked for are
 * never the ones thrown out, so a caller may hold two at a time */
static struct cbuf *cget(u32 blk, int kind, int noread)
{
    struct cbuf *c, *pick = 0;
    if (blk >= nblocks) return 0;
    for (u32 i = 0; i < ncache; i++) {
        c = &cache[i];
        if (c->valid && c->blk == blk) {
            c->stamp = ++cstamp;
            if (!c->dirty) c->kind = (u8)kind;
            return c;
        }
    }
    for (u32 i = 0; i < ncache; i++) {
        c = &cache[i];
        if (!c->valid) { pick = c; break; }
        if (cstamp - c->stamp < 2) continue;
        if (!pick || (pick->dirty && !c->dirty) || (pick->dirty == c->dirty && c->stamp < pick->stamp)) pick = c;
    }
    if (pick->valid && pick->dirty && flush_upto(pick->kind)) return 0;
    pick->valid = 0;
    if (!noread && dio(DISK_READ, blk, 1, pick->d)) return 0;
    pick->blk = blk; pick->kind = (u8)kind; pick->dirty = 0; pick->valid = 1; pick->ok = 0; pick->sum = SUM_NONE;
    pick->stamp = ++cstamp;
    return pick;
}
/* changed: it goes to the disk with its kind. A block dirty already keeps the earlier kind: a header not yet linked stays
 * new, whatever is done to it */
static void mark(struct cbuf *c, int kind, int sum)
{
    c->sum = (u8)sum;
    if (!c->dirty) { c->kind = (u8)kind; c->dirty = 1; }
}
/* before a transfer past the cache: WRITING drops the blocks it overwrites, reading saves the changed ones first */
static int cache_range(u32 blk, u32 n, int writing)
{
    for (u32 i = 0; i < ncache; i++) {
        struct cbuf *c = &cache[i];
        if (!c->valid || c->blk < blk || c->blk - blk >= n) continue;
        if (writing) c->valid = 0;
        else if (c->dirty) {
            seal(c);
            if (dio(DISK_WRITE, c->blk, 1, c->d)) return -EIO;
            c->dirty = 0;
        }
    }
    return 0;
}

/* ---- checked blocks ---- */
static int in_range(u32 b) { return b >= reserved && b < nblocks; }
/* a checksum is compared once for each time a block is read */
static int sum_good(struct cbuf *c)
{
    if (c->ok) return 1;
    if (sum_of(c->d)) return 0;
    c->ok = 1;
    return 1;
}
/* a header block: type, own number (the root's is 0), checksum. 0 on damage */
static struct cbuf *hdr(u32 key)
{
    if (!in_range(key)) return 0;
    struct cbuf *c = cget(key, K_REF, 0);
    if (!c || GL(c->d, 0) != T_HEADER || !sum_good(c)) return 0;
    if (GL(c->d, 1) != key && !(key == root_blk && !GL(c->d, 1))) return 0;
    return c;
}
/* an extension block of file header OWNER */
static struct cbuf *ext(u32 key, u32 owner)
{
    if (!in_range(key)) return 0;
    struct cbuf *c = cget(key, K_EXT, 0);
    if (!c || GL(c->d, 0) != T_LIST || !sum_good(c) || GL(c->d, 1) != key || GL(c->d, O_PARENT) != owner ||
        GL(c->d, O_SEC) != ST_FILE || GL(c->d, O_HIGHSEQ) > HT) return 0;
    return c;
}

/* ---- names ---- */
static u8 q[108];                      /* the name asked for: Latin-1 */
static u32 q_len;

static u32 up(u32 c)
{
    if (c >= 'a' && c <= 'z') return c - 32;
    if (is_intl && c >= 224 && c <= 254 && c != 247) return c - 32;
    return c;
}
static u32 name_hash(const u8 *n, u32 len)
{
    u32 h = len;
    for (u32 i = 0; i < len; i++) h = (h * 13 + up(n[i])) & 0x7FF;
    return h % HT;
}
/* an entry's name on the disk. The root keeps its place on a long-name volume. 0, or -EIO */
static int name_of(const u8 *d, u32 key, const u8 **n, u32 *len)
{
    const u8 *p = is_ln && key != root_blk ? d + BS - 184 : d + BS - 80;
    u32 l = *p, cap = is_ln && key != root_blk ? 107 : 30;
    if (!l || l > cap) return -EIO;
    *n = p + 1; *len = l;
    return 0;
}
/* NAME (LEN bytes of UTF-8) into q as Latin-1. 0, -EILSEQ (not Latin-1, or not UTF-8), -ENAMETOOLONG */
static int name_in(const char *name, u32 len)
{
    u32 n = 0, cap = is_ln ? 107 : 30;
    const u8 *p = (const u8 *)name, *e = p + len;
    while (p < e) {
        u32 c = *p++;
        if (c >= 0x80) {
            if ((c != 0xC2 && c != 0xC3) || p >= e || (*p & 0xC0) != 0x80) return -EILSEQ;
            c = (c & 3) << 6 | (*p++ & 0x3F);
        }
        if (n == cap) return -ENAMETOOLONG;
        q[n++] = (u8)c;
    }
    q_len = n;
    return 0;
}
static int name_bad(void)
{
    if (!q_len) return -EINVAL;
    for (u32 i = 0; i < q_len; i++) {
        u32 c = q[i];
        if (c < 0x20 || c == ':' || c == '/' || (c >= 0x7F && c < 0xA0)) return -EINVAL;
    }
    return 0;
}
/* Latin-1 as UTF-8 in OUT, what can be typed: damage (a control character, a /) shows as _. Length, or -1 past CAP */
static int utf8_of(const u8 *n, u32 len, char *out, u32 cap, int raw)
{
    u32 k = 0;
    for (u32 i = 0; i < len; i++) {
        u32 c = n[i];
        if (!raw && (c < 0x20 || c == '/')) c = '_';
        if (k + 2 >= cap) return -1;
        if (c < 0x80) out[k++] = (char)c;
        else { out[k++] = (char)(0xC0 | c >> 6); out[k++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[k] = 0;
    return (int)k;
}
/* a name for a listing: on a long-name volume one over 63 bytes of UTF-8 is its start, ~ and the block number */
static void name_list(const u8 *n, u32 len, u32 key, char *out, u32 cap)
{
    if (utf8_of(n, len, out, cap, 0) >= 0) return;
    u32 k = len;
    while (k && utf8_of(n, k, out, cap - 10, 0) < 0) k--;
    char *p = out + strlen(out);
    *p++ = '~';
    int s = 28;
    while (s > 0 && !(key >> s)) s -= 4;
    for (; s >= 0; s -= 4) *p++ = "0123456789abcdef"[(key >> s) & 15];
    *p = 0;
}

/* ---- dates ---- */
static u32 amiga_to_unix(u32 days, u32 mins, u32 ticks)
{
    if (days > 46000) return 0xFFFFFFFFu;             /* past the Unix time here; damage */
    if (mins > 1439) mins = 1439;
    if (ticks > 2999) ticks = 2999;
    return AMIGA_EPOCH + days * 86400 + mins * 60 + ticks / 50;
}
static void put_date(u8 *d, u32 at, u32 t)
{
    if (t < AMIGA_EPOCH) t = AMIGA_EPOCH;
    t -= AMIGA_EPOCH;
    SL(d, at, t / 86400);
    SL(d, at + 1, (t % 86400) / 60);
    SL(d, at + 2, (t % 60) * 50);
}

/* ---- an entry of a directory ---- */
typedef struct {
    u32 key;                           /* the entry's own block */
    u32 real;                          /* the file or directory it is: key, or a hard link's target */
    u32 sec;                           /* ST_FILE, ST_DIR, ST_SOFT or ST_ROOT: what real is */
    u32 parent;                        /* the directory the entry is in */
    u32 chain;                         /* the next entry of its hash chain */
    u32 prot, size, mtime, links;
    u8 islink;
} node_t;

static int is_dir(const node_t *n) { return n->sec == ST_DIR || n->sec == ST_ROOT; }
/* the fields of the entry at KEY, a hard link's from its file */
static int node_load(u32 key, node_t *n)
{
    struct cbuf *c = hdr(key);
    if (!c) return -EIO;
    u32 sec = GL(c->d, O_SEC);
    n->key = n->real = key; n->sec = sec; n->islink = 0;
    n->parent = GL(c->d, O_PARENT); n->chain = GL(c->d, O_CHAIN);
    if (key == root_blk ? sec != ST_ROOT : sec == ST_ROOT) return -EIO;
    if (sec == ST_LFILE || sec == ST_LDIR) {
        u32 t = GL(c->d, O_REAL);
        if (t == key || !(c = hdr(t)) || GL(c->d, O_SEC) != (sec == ST_LFILE ? ST_FILE : ST_DIR)) return -EIO;
        n->real = t; n->sec = GL(c->d, O_SEC); n->islink = 1;
    } else if (sec != ST_FILE && sec != ST_DIR && sec != ST_SOFT && sec != ST_ROOT) return -EIO;
    u32 at = is_ln && n->real != root_blk ? O_DAYS_LN : O_DAYS;
    n->mtime = amiga_to_unix(GL(c->d, at), GL(c->d, at + 1), GL(c->d, at + 2));
    n->prot = GL(c->d, O_PROT);
    n->size = GL(c->d, O_SIZE);
    if (n->sec != ST_ROOT && n->sec != ST_DIR && n->sec != ST_SOFT && n->size > MAX_SIZE) return -EIO;
    /* a file's links: itself and its list of hard links */
    u32 nl = n->sec == ST_FILE ? GL(c->d, O_NEXTLINK) : 0;
    n->links = 1;
    for (u32 i = 0; nl && i < 256; i++) {
        struct cbuf *l = hdr(nl);
        if (!l) break;
        n->links++;
        nl = GL(l->d, O_NEXTLINK);
    }
    return 0;
}
/* a hash chain walked with Brent's mark: a loop answers -EIO after at most a few times its length, with no memory */
struct cwalk { u32 mark, left, span; };
static void cw_init(struct cwalk *w) { w->mark = 0; w->left = 1; w->span = 1; }
static int cw_step(struct cwalk *w, u32 next)
{
    if (next && next == w->mark) return -EIO;
    if (!--w->left) { w->span *= 2; w->left = w->span; w->mark = next; }
    return 0;
}
/* the entry of DIR named q (a long-name volume's listed form too). 0, -ENOENT, -EIO */
static int lookup(u32 dir, node_t *n)
{
    struct cbuf *c = hdr(dir);
    struct cwalk w;
    if (!c) return -EIO;
    if (GL(c->d, O_SEC) != ST_DIR && GL(c->d, O_SEC) != ST_ROOT) return -ENOTDIR;
    u32 e = GL(c->d, O_TABLE + name_hash(q, q_len));
    cw_init(&w);
    while (e) {
        const u8 *nm;
        u32 len;
        if (!(c = hdr(e)) || GL(c->d, O_PARENT) != dir || name_of(c->d, e, &nm, &len)) return -EIO;
        if (len == q_len) {
            u32 i = 0;
            while (i < len && up(nm[i]) == up(q[i])) i++;
            if (i == len) return node_load(e, n);
        }
        u32 nx = GL(c->d, O_CHAIN);
        if (cw_step(&w, nx)) return -EIO;
        e = nx;
    }
    if (is_ln) {                                      /* START~KEY, as a listing shows a name too long to fit */
        u32 t = q_len, key = 0, sh = 0;
        while (t && q[t - 1] != '~' && sh < 32) {
            u32 ch = q[--t], v = ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= '0' && ch <= '9' ? ch - '0' : 99;
            if (v > 15) return -ENOENT;
            key |= v << sh; sh += 4;
        }
        if (t < 2 || !sh || !in_range(key) || !(c = hdr(key)) || GL(c->d, O_PARENT) != dir) return -ENOENT;
        const u8 *nm;
        u32 len;
        char alias[64];
        if (name_of(c->d, key, &nm, &len) || utf8_of(nm, len, alias, 64, 0) >= 0) return -ENOENT;
        name_list(nm, len, key, alias, 64);
        char want[64];
        if (utf8_of(q, q_len, want, 64, 0) < 0 || strcmp(want, alias)) return -ENOENT;
        return node_load(key, n);
    }
    return -ENOENT;
}

/* INO as a directory: stateless. 0, -ESTALE (no header there any more), -ENOTDIR */
static int ino_dir(u32 ino)
{
    struct cbuf *c = hdr(ino);
    if (!c) return -ESTALE;
    u32 s = GL(c->d, O_SEC);
    return s == ST_DIR || s == ST_ROOT ? 0 : -ENOTDIR;
}

/* ---- soft links ---- */
/* the text of the soft link KEY as a Zenix path: a leading / is ../, a//b is a/../b, this volume's name is the way up to
 * its root (the server does not know the mount point), another volume's stays as it is and leads nowhere */
static int link_text(u32 key, char *out, u32 cap)
{
    struct cbuf *c = hdr(key);
    if (!c || GL(c->d, O_SEC) != ST_SOFT) return -EIO;
    const u8 *t = c->d + 24;
    u32 n = 0, max = BS - 224, k = 0, parent = GL(c->d, O_PARENT);
    while (n < max && t[n]) n++;
    if (n == max) return -EIO;
    if (!n) return -ENOENT;
    u32 start = 0, colon = n;
    for (u32 i = 0; i < n; i++) if (t[i] == ':') { colon = i; break; }
    if (colon < n) {
        u32 i = 0;
        if (colon == volname_len) while (i < colon && up(t[i]) == up(volname[i])) i++;
        if (!colon || i == colon) {                   /* ":x" or "ThisVolume:x": from the root */
            if (n > 255) return -ENAMETOOLONG;
            static u8 copy[256];
            memcpy(copy, t, n);
            t = copy;
            for (u32 up_n = 0; parent != root_blk; up_n++) {
                struct cbuf *p = hdr(parent);
                if (!p || up_n > 512) return -EIO;
                if (k + 3 >= cap) return -ENAMETOOLONG;
                out[k++] = '.'; out[k++] = '.'; out[k++] = '/';
                parent = GL(p->d, O_PARENT);
            }
            start = colon + 1;
        }
    }
    int seg = 1;
    for (u32 i = start; i < n; i++) {
        u32 ch = t[i];
        if (ch == '/' && seg) {
            if (k + 3 >= cap) return -ENAMETOOLONG;
            out[k++] = '.'; out[k++] = '.'; out[k++] = '/';
            continue;
        }
        seg = ch == '/';
        if (k + 2 >= cap) return -ENAMETOOLONG;
        if (ch < 0x80) out[k++] = (char)ch;
        else { out[k++] = (char)(0xC0 | ch >> 6); out[k++] = (char)(0x80 | (ch & 0x3F)); }
    }
    if (!k) { out[k++] = '.'; }                       /* "ThisVolume:" at the root */
    out[k] = 0;
    return (int)k;
}
/* a Zenix relative path as a soft link's text on the disk. Length, or -errno */
static int link_store(const char *t, u8 *out, u32 cap)
{
    u32 k = 0;
    const u8 *p = (const u8 *)t;
    if (*p == '/') return -EINVAL;                    /* an absolute path means something else on the Amiga */
    while (*p) {
        const u8 *e = p;
        while (*e && *e != '/') e++;
        u32 n = (u32)(e - p);
        if (n == 1 && p[0] == '.') ;                  /* "." says nothing */
        else if (n == 2 && p[0] == '.' && p[1] == '.') {
            if (k + 1 > cap) return -ENAMETOOLONG;
            out[k++] = '/';
        } else if (n) {
            for (u32 i = 0; i < n; i++) {
                u32 c = p[i];
                if (c >= 0x80) {
                    if ((c != 0xC2 && c != 0xC3) || i + 1 >= n || (p[i + 1] & 0xC0) != 0x80) return -EILSEQ;
                    c = (c & 3) << 6 | (p[++i] & 0x3F);
                }
                if (c < 0x20 || c == ':') return -EINVAL;
                if (k + 1 > cap) return -ENAMETOOLONG;
                out[k++] = (u8)c;
            }
            if (*e && e[1]) {
                if (k + 1 > cap) return -ENAMETOOLONG;
                out[k++] = '/';
            }
        }
        p = *e ? e + 1 : e;
    }
    if (!k) return -EINVAL;
    return (int)k;
}

/* ---- paths ---- */
static const char *lk_path;            /* where a walk stopped at a link (D26): the path, the name, the link */
static u32 lk_s, lk_e, lk_key;

static int dmode_ok(int want) { return fsp_access(cuid, cgid, cng, cgs, m_dmode, m_uid, m_gid, want); }
static int dir_may(void) { return fsp_dir_modify(cuid, cgid, cng, cgs, m_dmode, m_uid, m_gid); }

/* walk PATH's first LEN bytes from directory CWD; the end in N. Links are followed inside the volume (not the last name
 * unless FOLLOW), or with stoplinks the walk ends at the first one with -FS_LINKED */
static int walk(u32 cwd, const char *path, u32 len, int follow, int depth, node_t *n)
{
    u32 cur = path[0] == '/' ? root_blk : cwd, i = 0;
    int r;
    if (depth > 8) return -ELOOP;
    if ((r = ino_dir(cur))) return r;
    for (;;) {
        while (i < len && path[i] == '/') i++;
        if (i >= len) return node_load(cur, n);
        u32 s = i, k;
        while (i < len && path[i] != '/') i++;
        k = i;
        while (k < len && path[k] == '/') k++;
        int last = k >= len;
        if (!dmode_ok(FSP_X)) return -EACCES;
        if (i - s == 1 && path[s] == '.') continue;
        if (i - s == 2 && path[s] == '.' && path[s + 1] == '.') {
            struct cbuf *c = hdr(cur);
            if (!c) return -EIO;
            if (cur != root_blk) {
                cur = GL(c->d, O_PARENT);
                if ((r = ino_dir(cur))) return r == -ESTALE || r == -ENOTDIR ? -EIO : r;
            }
            continue;
        }
        if ((r = name_in(path + s, i - s))) return r == -EILSEQ ? -ENOENT : r;
        if ((r = lookup(cur, n))) return r;
        if (n->sec == ST_SOFT && (!last || follow)) {
            if (stoplinks && path[0] == '/') {
                lk_path = path; lk_s = s; lk_e = i; lk_key = n->key;
                return -FS_LINKED;
            }
            char t[256];
            int m = link_text(n->key, t, sizeof t);
            if (m < 0) return m;
            if ((r = walk(cur, t, (u32)m, 1, depth + 1, n))) return r;
        }
        if (last) return 0;
        if (!is_dir(n)) return -ENOTDIR;
        cur = n->real;
    }
}
/* the directory that holds PATH's last name, and that name in q */
static int parent_of(u32 cwd, const char *path, u32 *dir)
{
    static node_t pn;
    u32 n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    u32 s = n;
    while (s > 0 && path[s - 1] != '/') s--;
    if (n == s || (n - s == 1 && path[s] == '.') || (n - s == 2 && path[s] == '.' && path[s + 1] == '.')) return -EINVAL;
    int r = walk(cwd, path, s, 1, 0, &pn);
    if (r) return r;
    if (!is_dir(&pn)) return -ENOTDIR;
    *dir = pn.real;
    return name_in(path + s, n - s);
}

/* ---- the bitmap ---- */
static u32 bx_index = 0xFFFFFFFFu, bx_block;   /* the bitmap extension block last asked for */
/* the block that holds bitmap page P. 0, or -EIO */
static int bm_where(u32 p, u32 *out)
{
    struct cbuf *c = hdr(root_blk);
    if (!c || p >= nbm) return -EIO;
    u32 b;
    if (p < 25) b = GL(c->d, O_BMPAGES + p);
    else {
        u32 x = (p - 25) / (LW - 1), i = 0, e = GL(c->d, O_BMEXT);
        if (bx_index <= x) { i = bx_index; e = bx_block; }
        while (1) {
            if (!in_range(e)) return -EIO;
            if (i == x) break;
            if (!(c = cget(e, K_REF, 0))) return -EIO;
            e = GL(c->d, LW - 1);
            i++;
        }
        bx_index = x; bx_block = e;
        if (!(c = cget(e, K_REF, 0))) return -EIO;
        b = GL(c->d, (p - 25) % (LW - 1));
    }
    if (!in_range(b)) return -EIO;
    *out = b;
    return 0;
}
static struct cbuf *bm_page(u32 p)
{
    u32 b;
    if (bm_where(p, &b)) return 0;
    struct cbuf *c = cget(b, K_BM, 0);
    return c && sum_good(c) ? c : 0;
}
static int count_free(void)
{
    if (free_cnt != 0xFFFFFFFFu) return 0;
    u32 k = 0, left = nblocks - reserved;
    for (u32 p = 0; p < nbm; p++) {
        struct cbuf *c = bm_page(p);
        if (!c) return -EIO;
        for (u32 w = 1; w < LW && left; w++) {
            u32 v = GL(c->d, w), bits = left < 32 ? left : 32;
            if (bits < 32) v &= (1u << bits) - 1;
            while (v) { k++; v &= v - 1; }
            left -= bits;
        }
    }
    free_cnt = k;
    return 0;
}
static int sync_all(void);
/* a free block, marked in use. -ENOSPC when there is none */
static int alloc_block(u32 *out)
{
    u32 total = nblocks - reserved;
    for (int again = 0; again < 2; again++) {
        u32 b = next_free < reserved || next_free >= nblocks ? root_blk : next_free;
        for (u32 scanned = 0; scanned < total; ) {
            u32 k = b - reserved, p = k / PB, wi = (k % PB) / 32, bit = k % 32;
            struct cbuf *c = bm_page(p);
            if (!c) return -EIO;
            u32 v = GL(c->d, 1 + wi) >> bit;
            if (!v) {                                 /* nothing free in the rest of this word */
                u32 skip = 32 - bit;
                if (skip > nblocks - b) skip = nblocks - b;   /* the bits past the end count for nothing */
                b += skip; scanned += skip;
                if (b >= nblocks) b = reserved;
                continue;
            }
            if (v & 1 && b != root_blk) {
                SL(c->d, 1 + wi, GL(c->d, 1 + wi) & ~(1u << bit));
                mark(c, K_BM, SUM_BM);
                next_free = b + 1;
                if (free_cnt != 0xFFFFFFFFu && free_cnt) free_cnt--;
                *out = b;
                return 0;
            }
            b++; scanned++;
            if (b >= nblocks) b = reserved;
        }
        if (!npend) break;                            /* what waits to be freed comes free now */
        int r = sync_all();
        if (r) return r;
        dirty = 1;
    }
    return -ENOSPC;
}
static void free_block(u32 b)
{
    if (!in_range(b) || b == root_blk) return;
    u32 k = b - reserved, p = k / PB, wi = (k % PB) / 32, bit = k % 32;
    struct cbuf *c = bm_page(p);
    if (!c) return;                                   /* a lost block: never a crash */
    u32 v = GL(c->d, 1 + wi);
    if (v >> bit & 1) return;
    SL(c->d, 1 + wi, v | 1u << bit);
    mark(c, K_BM, SUM_BM);
    if (free_cnt != 0xFFFFFFFFu) free_cnt++;
    if (b >= root_blk && b < next_free) next_free = b;
}
/* the blocks named by the table in sb, and on through its extension blocks. OWNER: the file header's number */
static void free_list(u32 owner)
{
    u32 guard = nblocks;
    for (;;) {
        u32 hs = GL(sb, O_HIGHSEQ);
        if (hs > HT) return;
        for (u32 i = 0; i < hs; i++) free_block(GL(sb, O_TABLE + HT - 1 - i));
        u32 e = GL(sb, O_EXT);
        if (!e || !guard--) return;
        struct cbuf *c = ext(e, owner);
        if (!c) return;
        memcpy(sb, c->d, BS);
        free_block(e);
    }
}
/* the blocks of an entry nothing names any more, and the entry */
static void free_entry(u32 key)
{
    struct cbuf *c = hdr(key);
    if (!c) return;
    if (GL(c->d, O_SEC) == ST_FILE) {
        memcpy(sb, c->d, BS);
        free_list(key);
    }
    free_block(key);
}

/* ---- commits ---- */
static int sync_all(void)
{
    int r;
    if (vol_changed) {
        struct cbuf *c = hdr(root_blk);
        if (!c) return -EIO;
        u32 now = (u32)sys_time();
        put_date(c->d, O_VOLDATE, now);
        mark(c, K_REF, SUM_HDR);
        vol_changed = 0;
    }
    if ((r = flush_all())) return r;
    if (npend) {
        for (u32 i = 0; i < npend; i++) free_entry(pend[i]);
        npend = 0;
        if ((r = flush_kind(K_BM))) return r;
    }
    dirty = 0;
    return 0;
}
static int touch(void)
{
    if (readonly) return -EROFS;
    if (!dirty) { dirty = 1; dirty_since = sys_ticks(); }
    vol_changed = 1;
    return 0;
}
static int go_clean(void)
{
    int r = sync_all();
    return r ? r : dflush();
}
static int inner_commit(void)
{
    int r = sync_all();
    if (!r) { dirty = 1; vol_changed = 1; }
    return r;
}
static int entry_release(u32 key)
{
    if (npend == NPEND) { int r = inner_commit(); if (r) return r; }
    pend[npend++] = key;
    return 0;
}

/* ---- hash chains ---- */
/* the place of entry KEY's chain in directory DIR: PREV the entry before KEY (0: the directory's own slot), in *SLOT the
 * long of the block PREV-or-DIR that holds the link. FIND set: find KEY there, or -EIO. Else: where KEY goes in block order,
 * *NEXT what follows it */
static int chain_place(u32 dir, u32 key, u32 h, int find, u32 *prev, u32 *at, u32 *next)
{
    struct cbuf *c = hdr(dir);
    struct cwalk w;
    if (!c) return -EIO;
    u32 p = 0, e = GL(c->d, O_TABLE + h);
    cw_init(&w);
    while (e) {
        if (find ? e == key : e > key) break;
        if (!(c = hdr(e))) return -EIO;
        u32 nx = GL(c->d, O_CHAIN);
        if (cw_step(&w, nx)) return -EIO;
        p = e; e = nx;
    }
    if (find && !e) return -EIO;
    *prev = p; *at = p ? O_CHAIN : O_TABLE + h; *next = e;
    return 0;
}
static int entry_hash(u32 key, u32 *h)
{
    struct cbuf *c = hdr(key);
    const u8 *n;
    u32 len;
    if (!c || name_of(c->d, key, &n, &len)) return -EIO;
    *h = name_hash(n, len);
    return 0;
}
/* the entry KEY leaves its chain: one write */
static int chain_remove(u32 dir, u32 key)
{
    u32 h, prev, at, next;
    int r = entry_hash(key, &h);
    if (r || (r = chain_place(dir, key, h, 1, &prev, &at, &next))) return r;
    struct cbuf *e = hdr(key);
    if (!e) return -EIO;
    u32 after = GL(e->d, O_CHAIN);
    struct cbuf *c = hdr(prev ? prev : dir);
    if (!c) return -EIO;
    SL(c->d, at, after);
    mark(c, K_REF, SUM_HDR);
    return 0;
}
/* the entry KEY, whose name is already in it, joins DIR's chain at its place in block order. FLUSH: it reaches the disk
 * before the one write that links it (an entry that was on the disk already, so not a new block) */
static int chain_insert(u32 dir, u32 key, int flush)
{
    u32 h, prev, at, next;
    int r = entry_hash(key, &h);
    if (r || (r = chain_place(dir, key, h, 0, &prev, &at, &next))) return r;
    struct cbuf *e = hdr(key);
    if (!e) return -EIO;
    SL(e->d, O_CHAIN, next);
    SL(e->d, O_PARENT, dir);
    mark(e, K_EXT, SUM_HDR);                          /* before any link to it, also if the cache threw it out meanwhile */
    if (flush && (r = flush_all())) return r;
    struct cbuf *c = hdr(prev ? prev : dir);
    if (!c) return -EIO;
    SL(c->d, at, key);
    mark(c, K_REF, SUM_HDR);
    return 0;
}
static int dir_touch(u32 dir)
{
    struct cbuf *c = hdr(dir);
    if (!c) return -EIO;
    put_date(c->d, O_DAYS, (u32)sys_time());
    mark(c, K_REF, SUM_HDR);
    return 0;
}
static void set_name(u8 *d, const u8 *n, u32 len)
{
    u8 *p = d + BS - 80;
    memset(p, 0, 31);
    p[0] = (u8)len;
    memcpy(p + 1, n, len);
}

/* ---- open files ---- */
struct ofile {
    u32 ino, count, size, mtime, prot;
    u32 eidx, eblk;                    /* the extension block last used: reading in order is one step per block */
    u8 orphan, stale, pad[2];          /* orphan: its name is gone, its blocks go at the last close;
                                        * stale: opened before this server's start */
};
static struct ofile opens[NOPEN];

static struct ofile *slot_ino(u32 ino)
{
    for (u32 i = 0; i < NOPEN; i++) if (opens[i].count && opens[i].ino == ino) return &opens[i];
    return 0;
}
/* the block holding the table of group G of the file (0: the header). *OUT 0: the chain ends before it */
static int group_block(struct ofile *o, u32 g, u32 *out)
{
    u32 i = 0, b = o->ino;
    if (o->eidx && g >= o->eidx) { i = o->eidx; b = o->eblk; }
    while (i < g) {
        struct cbuf *c = i ? ext(b, o->ino) : hdr(b);
        if (!c) return -EIO;
        u32 nx = GL(c->d, O_EXT);
        if (!nx) { *out = 0; return 0; }
        b = nx; i++;
    }
    if (g) {
        if (!ext(b, o->ino)) return -EIO;
        o->eidx = g; o->eblk = b;
    }
    *out = b;
    return 0;
}
static struct cbuf *group_get(struct ofile *o, u32 b) { return b == o->ino ? hdr(b) : ext(b, o->ino); }
/* data block IDX of the file. *OUT 0: not allocated */
static int data_block(struct ofile *o, u32 idx, u32 *out)
{
    u32 g = idx / HT, pos = idx % HT, b;
    int r = group_block(o, g, &b);
    if (r) return r;
    if (!b) { *out = 0; return 0; }
    struct cbuf *c = group_get(o, b);
    if (!c) return -EIO;
    u32 hs = GL(c->d, O_HIGHSEQ);
    if (hs > HT) return -EIO;
    if (pos >= hs) { *out = 0; return 0; }
    u32 d = GL(c->d, O_TABLE + HT - 1 - pos);
    if (!in_range(d)) return -EIO;
    *out = d;
    return 0;
}
/* an OFS data block of the file, checked */
static struct cbuf *ofs_block(struct ofile *o, u32 d, u32 idx)
{
    struct cbuf *c = cget(d, K_NEW, 0);
    if (!c || GL(c->d, 0) != T_DATA || !sum_good(c) || GL(c->d, 1) != o->ino || GL(c->d, 2) != idx + 1 ||
        GL(c->d, 3) > BS - 24) return 0;
    return c;
}
static int file_read(struct ofile *o, u32 off, u8 *buf, u32 n)
{
    u32 done = 0, pay = is_ofs ? BS - 24 : BS;
    if (off >= o->size) return 0;
    if (n > o->size - off) n = o->size - off;
    while (done < n) {
        u32 idx = off / pay, in = off % pay, left = n - done, d;
        int r = data_block(o, idx, &d);
        if (!r && !d) r = -EIO;                       /* the list is shorter than the file's size */
        if (r) return done ? (int)done : r;
        if (is_ofs) {
            struct cbuf *c = ofs_block(o, d, idx);
            if (!c || in >= GL(c->d, 3)) return done ? (int)done : -EIO;
            u32 k = GL(c->d, 3) - in;
            if (k > left) k = left;
            memcpy(buf + done, c->d + 24 + in, k);
            done += k; off += k;
            continue;
        }
        if (!in && left >= BS && !((u32)(buf + done) & 1)) {   /* whole blocks go straight to the caller */
            u32 run = 1, g = idx / HT, max = left / BS, b;
            if (!group_block(o, g, &b) && b) {        /* contiguous blocks of this table are one transfer */
                struct cbuf *c = group_get(o, b);
                u32 pos = idx % HT;
                if (c) {
                    u32 hs = GL(c->d, O_HIGHSEQ);
                    while (run < max && pos + run < hs && pos + run < HT && GL(c->d, O_TABLE + HT - 1 - (pos + run)) == d + run) run++;
                }
            }
            if ((r = cache_range(d, run, 0)) || (r = dio(DISK_READ, d, run, buf + done))) return done ? (int)done : r;
            done += run * BS; off += run * BS;
            continue;
        }
        struct cbuf *c = cget(d, K_NEW, 0);
        if (!c) return done ? (int)done : -EIO;
        u32 k = BS - in < left ? BS - in : left;
        memcpy(buf + done, c->d + in, k);
        done += k; off += k;
    }
    return (int)done;
}
/* a block that was free: all zero in the cache */
static struct cbuf *new_block(u32 b)
{
    struct cbuf *c = cget(b, K_NEW, 1);
    if (!c) return 0;
    memset(c->d, 0, BS);
    c->ok = 1;
    return c;
}
/* data block IDX of the file, the list grows to reach it. *FRESH: it is new */
static int data_block_grow(struct ofile *o, u32 idx, u32 *out, int *fresh)
{
    u32 g = idx / HT, pos = idx % HT, b, d;
    int r = group_block(o, g, &b);
    *fresh = 0;
    if (r) return r;
    if (!b) {                                         /* a new extension block, after the last one */
        u32 pb;
        if (!g || (r = group_block(o, g - 1, &pb))) return r ? r : -EIO;
        if (!pb) return -EIO;                         /* the list has a gap: never made here */
        struct cbuf *pc = group_get(o, pb);
        if (!pc) return -EIO;
        if (GL(pc->d, O_HIGHSEQ) != HT) return -EIO;
        if ((r = alloc_block(&b))) return r;
        struct cbuf *c = new_block(b);
        if (!c) return -EIO;
        SL(c->d, 0, T_LIST); SL(c->d, 1, b);
        SL(c->d, O_PARENT, o->ino); SL(c->d, O_SEC, ST_FILE);
        mark(c, K_NEW, SUM_HDR);
        if (!(pc = group_get(o, pb))) return -EIO;
        SL(pc->d, O_EXT, b);
        mark(pc, pb == o->ino ? K_REF : K_EXT, SUM_HDR);
        o->eidx = g; o->eblk = b;
    }
    struct cbuf *c = group_get(o, b);
    if (!c) return -EIO;
    u32 hs = GL(c->d, O_HIGHSEQ);
    if (hs > HT) return -EIO;
    if (pos < hs) {
        d = GL(c->d, O_TABLE + HT - 1 - pos);
        if (!in_range(d)) return -EIO;
        *out = d;
        return 0;
    }
    if (pos != hs) return -EIO;
    if ((r = alloc_block(&d))) return r;
    if (!(c = group_get(o, b))) return -EIO;
    SL(c->d, O_TABLE + HT - 1 - pos, d);
    SL(c->d, O_HIGHSEQ, pos + 1);
    if (!idx) SL(c->d, O_FIRST, d);
    mark(c, b == o->ino ? K_REF : K_EXT, SUM_HDR);
    if (is_ofs && idx) {                              /* the block before it points at it: after the bitmap */
        u32 pd;
        if ((r = data_block(o, idx - 1, &pd))) return r;
        struct cbuf *pc = pd ? ofs_block(o, pd, idx - 1) : 0;
        if (!pc) return -EIO;
        SL(pc->d, 4, d);
        pc->sum = SUM_HDR;
        pc->kind = pc->dirty && pc->kind > K_EXT ? pc->kind : K_EXT;
        pc->dirty = 1;
    }
    *out = d;
    *fresh = 1;
    return 0;
}
/* BUF 0: zeros */
static int file_put(struct ofile *o, u32 off, const u8 *buf, u32 n)
{
    u32 done = 0, pay = is_ofs ? BS - 24 : BS;
    while (done < n) {
        u32 idx = off / pay, in = off % pay, left = n - done, d;
        int fresh, r = data_block_grow(o, idx, &d, &fresh);
        if (r) return done ? (int)done : r;
        if (idx * pay >= o->size && off >= o->size) fresh = 1;   /* named past the size (a cut, d31 7): it holds nothing */
        if (is_ofs) {
            struct cbuf *c;
            if (fresh) {
                if (!(c = new_block(d))) return done ? (int)done : -EIO;
                SL(c->d, 0, T_DATA); SL(c->d, 1, o->ino); SL(c->d, 2, idx + 1);
            } else if (!(c = ofs_block(o, d, idx))) return done ? (int)done : -EIO;
            u32 k = pay - in < left ? pay - in : left;
            if (buf) memcpy(c->d + 24 + in, buf + done, k);
            else memset(c->d + 24 + in, 0, k);
            if (in + k > GL(c->d, 3)) SL(c->d, 3, in + k);
            mark(c, K_NEW, SUM_HDR);
            done += k; off += k;
            continue;
        }
        if (buf && !in && left >= BS && !((u32)(buf + done) & 1)) {
            if ((r = cache_range(d, 1, 1)) || (r = dio(DISK_WRITE, d, 1, (void *)(buf + done)))) return done ? (int)done : r;
            done += BS; off += BS;
            continue;
        }
        u32 k = BS - in < left ? BS - in : left;
        struct cbuf *c = fresh ? new_block(d) : cget(d, K_NEW, k == BS);
        if (!c) return done ? (int)done : -EIO;
        if (buf) memcpy(c->d + in, buf + done, k);
        else memset(c->d + in, 0, k);
        mark(c, K_NEW, SUM_NONE);
        done += k; off += k;
    }
    return (int)done;
}
/* the header's size, date and archive bit: one write */
static int file_stamp(struct ofile *o)
{
    struct cbuf *c = hdr(o->ino);
    if (!c) return -EIO;
    o->mtime = (u32)sys_time();
    o->prot &= ~P_ARCHIVE;
    SL(c->d, O_SIZE, o->size);
    SL(c->d, O_PROT, o->prot);
    put_date(c->d, O_DAYS, o->mtime);
    mark(c, K_REF, SUM_HDR);
    return 0;
}
static int file_write(struct ofile *o, u32 off, const u8 *buf, u32 n)
{
    int r;
    if (!n) return 0;
    if (off > MAX_SIZE || n > MAX_SIZE - off) return -EFBIG;
    if ((r = touch())) return r;
    if (off > o->size) {                              /* no holes here: the gap is written as zeros */
        r = file_put(o, o->size, 0, off - o->size);
        if (r > 0) o->size += (u32)r;
        if (r < 0 || o->size != off) { file_stamp(o); return r < 0 ? r : -ENOSPC; }
    }
    r = file_put(o, off, buf, n);
    if (r > 0 && off + (u32)r > o->size) o->size = off + (u32)r;
    int e = file_stamp(o);
    return e ? e : r;
}
/* O_TRUNC: the header says empty on the disk first, then its blocks go free */
static int file_trunc(struct ofile *o)
{
    int r = touch();
    if (r || (r = inner_commit())) return r;
    struct cbuf *c = hdr(o->ino);
    if (!c) return -EIO;
    memcpy(sb, c->d, BS);
    memset(c->d + 4 * O_TABLE, 0, HT * 4);
    SL(c->d, O_HIGHSEQ, 0); SL(c->d, O_FIRST, 0); SL(c->d, O_EXT, 0);
    o->size = 0; o->eidx = 0;
    if ((r = file_stamp(o)) || (r = flush_all())) return r;
    free_list(o->ino);
    return 0;
}

/* ---- requests ---- */
static unsigned file_mode(u32 prot)
{
    unsigned m = 0;
    if (!(prot & P_READ)) m |= 0444;
    if (!(prot & P_WRITE)) m |= 0222;
    if (!(prot & P_EXEC)) m |= 0111;
    return m & m_fmode;
}
static void fill_node(const node_t *n, struct ustat *st)
{
    struct ofile *o = n->sec == ST_FILE ? slot_ino(n->real) : 0;
    st->ino = n->real;
    st->uid = m_uid; st->gid = m_gid;
    st->mtime = n->mtime;
    if (is_dir(n)) { st->mode = S_IFDIR | m_dmode; st->size = BS; st->links = 2; return; }
    if (n->sec == ST_SOFT) {
        struct cbuf *c = hdr(n->key);
        u32 l = 0;
        if (c) while (l < BS - 224 && c->d[24 + l]) l++;
        st->mode = 0120000 | 0777; st->size = l; st->links = 1;
        return;
    }
    st->mode = S_IFREG | file_mode(o ? o->prot : n->prot);
    st->size = o ? o->size : n->size;
    st->links = n->links;
    if (o) st->mtime = o->mtime;
}
/* a file's extension chain at its first open: each block a good one, no loop. A walk bounded by the size would not
 * notice one. Over CHECK_EXT the walk stops here: reads stay bounded by the size */
static int ext_check(u32 key)
{
    struct cbuf *c = hdr(key);
    struct cwalk w;
    if (!c) return -EIO;
    cw_init(&w);
    u32 e = GL(c->d, O_EXT);
    for (u32 i = 0; e && i < CHECK_EXT; i++) {
        if (!(c = ext(e, key))) return -EIO;
        u32 nx = GL(c->d, O_EXT);
        if (cw_step(&w, nx)) return -EIO;
        e = nx;
    }
    return 0;
}
static int slot_new(const node_t *n, struct ofile **out)
{
    struct ofile *o = slot_ino(n->real);
    if (o) {
        if (o->stale) return -ESTALE;
        o->count++; *out = o;
        return 0;
    }
    for (u32 i = 0; i < NOPEN; i++) {
        o = &opens[i];
        if (o->count) continue;
        memset(o, 0, sizeof *o);
        o->ino = n->real; o->count = 1; o->size = n->size; o->mtime = n->mtime; o->prot = n->prot;
        *out = o;
        return 0;
    }
    return -ENFILE;
}
static int slot_get(u32 ino, struct ofile **out)
{
    struct ofile *o = slot_ino(ino);
    if (!o || o->stale) return ino_dir(ino) ? -ESTALE : -EISDIR;
    *out = o;
    return 0;
}
/* a new entry of kind SEC named q in DIR. Its block is new and full: nothing on the disk names it yet; the one link
 * write comes last. Returns its number through *KEY */
static int entry_new(u32 dir, u32 sec, const u8 *text, u32 tlen, u32 *key)
{
    u32 b;
    int r = alloc_block(&b);
    if (r) return r;
    struct cbuf *c = new_block(b);
    if (!c) return -EIO;
    SL(c->d, 0, T_HEADER); SL(c->d, 1, b); SL(c->d, O_SEC, sec); SL(c->d, O_PARENT, dir);
    set_name(c->d, q, q_len);
    put_date(c->d, O_DAYS, (u32)sys_time());
    if (text) memcpy(c->d + 24, text, tlen);
    mark(c, K_NEW, SUM_HDR);
    if ((r = chain_insert(dir, b, 0)) || (r = dir_touch(dir))) return r;   /* (a lost block at worst) */
    *key = b;
    return 0;
}
static int do_open(struct umsg *m)
{
    static node_t n;
    const char *path = (const char *)m->a[0];
    u32 flags = m->a[2], dir;
    int writing = (flags & (O_WRONLY | O_RDWR)) != 0, excl = (flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL);
    int r = walk(m->a[1], path, strlen(path), !excl, 0, &n);
    struct ofile *o;
    struct ustat *st = (struct ustat *)m->a[3];
    if (r == 0 && excl) return -EEXIST;
    if (r) {
        if (r != -ENOENT || !(flags & O_CREAT)) return r;
        if ((r = parent_of(m->a[1], path, &dir))) return r;
        if (!dir_may()) return -EACCES;
        if ((r = name_bad())) return r;
        static node_t t;
        r = lookup(dir, &t);                          /* a dangling link has the name, and the walk followed it */
        if (!r) return -EEXIST;
        if (r != -ENOENT) return r;
        if ((r = touch())) return r;
        u32 key;
        if ((r = entry_new(dir, ST_FILE, 0, 0, &key)) || (r = node_load(key, &n))) return r;
    } else if (is_dir(&n)) {
        if (writing) return -EISDIR;
        if (!fsp_access(cuid, cgid, cng, cgs, S_IFDIR | m_dmode, m_uid, m_gid, fsp_open_want(flags))) return -EACCES;
        fill_node(&n, st);
        return (int)n.real;
    } else if (n.sec == ST_SOFT) {
        return -ELOOP;                                /* (a link not followed: O_EXCL answered above) */
    } else {
        if (!fsp_access(cuid, cgid, cng, cgs, S_IFREG | file_mode(n.prot), m_uid, m_gid, fsp_open_want(flags))) return -EACCES;
        if (writing && readonly) return -EROFS;
        if (!slot_ino(n.real) && !(writing && (flags & O_TRUNC)) && (r = ext_check(n.real))) return r;
    }
    if ((r = slot_new(&n, &o))) return r;
    if (writing && (flags & O_TRUNC) && o->size) {
        if ((r = file_trunc(o))) { o->count--; return r; }
    } else if (writing && (flags & O_TRUNC)) {
        struct cbuf *c = hdr(o->ino);                 /* no size, but perhaps blocks: a cut left them */
        if (c && GL(c->d, O_HIGHSEQ) && (r = file_trunc(o))) { o->count--; return r; }
    }
    fill_node(&n, st);
    st->ino = o->ino; st->size = o->size; st->mode = S_IFREG | file_mode(o->prot); st->mtime = o->mtime;
    return (int)o->ino;
}

/* a listing's place is the last entry it gave: its header says the rest (its hash, its next), also when it was removed
 * meanwhile, so a program that removes what it reads misses nothing. The cookie: bit 31 damage was met, below that the
 * last entry; RD_END after the last slot. A listing goes on past a damaged chain and answers -EIO at the end */
#define RD_END 0x7FFFFFFFu
static u32 rd_dir;
static struct cwalk rd_w;
static int do_readdir(struct umsg *m)
{
    struct udirent *ud = (struct udirent *)m->a[2];
    u32 dir = m->a[0], cookie = m->a[1], bad = cookie >> 31, last = cookie & RD_END, s = 0, e = 0;
    u32 *newc = (u32 *)m->a[3];
    const u8 *nm;
    u32 len;
    struct cbuf *c;
    int r = ino_dir(dir);
    if (r) return r;
    if (last == RD_END) { *newc = cookie; return bad ? -EIO : 0; }
    if (rd_dir != dir || !last) { rd_dir = dir; cw_init(&rd_w); }
    if (last) {
        if (!(c = hdr(last)) || GL(c->d, O_PARENT) != dir || name_of(c->d, last, &nm, &len)) return -EIO;   /* its block was used again */
        s = name_hash(nm, len) + 1;
        e = GL(c->d, O_CHAIN);
        if (cw_step(&rd_w, e)) { bad = 1; e = 0; }
    }
    for (;;) {
        if (!e) {
            if (s >= HT) {
                *newc = bad << 31 | RD_END;
                return bad ? -EIO : 0;
            }
            if (!(c = hdr(dir))) return -EIO;
            e = GL(c->d, O_TABLE + s);
            s++;
            cw_init(&rd_w);
            continue;
        }
        c = hdr(e);
        if (!c || GL(c->d, O_PARENT) != dir || name_of(c->d, e, &nm, &len) || name_hash(nm, len) != s - 1) { bad = 1; e = 0; continue; }
        u32 sec = GL(c->d, O_SEC);
        if (sec != ST_FILE && sec != ST_DIR && sec != ST_SOFT && sec != ST_LFILE && sec != ST_LDIR) { bad = 1; e = 0; continue; }
        name_list(nm, len, e, ud->name, sizeof ud->name);
        ud->type = sec == ST_DIR || sec == ST_LDIR ? 2 : sec == ST_SOFT ? 7 : 1;
        ud->ino = sec == ST_LFILE || sec == ST_LDIR ? GL(c->d, O_REAL) : e;
        *newc = bad << 31 | e;
        return 1;
    }
}

/* is directory DIR inside directory SRC, or SRC itself? Up through the parents */
static int inside(u32 dir, u32 src)
{
    for (u32 i = 0; i < 4096; i++) {
        if (dir == src) return 1;
        if (dir == root_blk) return 0;
        struct cbuf *c = hdr(dir);
        if (!c) return -EIO;
        dir = GL(c->d, O_PARENT);
    }
    return -EIO;
}
static int dir_empty(u32 d)
{
    struct cbuf *c = hdr(d);
    if (!c) return -EIO;
    for (u32 i = 0; i < HT; i++) if (GL(c->d, O_TABLE + i)) return -ENOTEMPTY;
    return 0;
}
/* the hard link L leaves its file's list, as the list's last step */
static int list_splice(u32 real, u32 l)
{
    u32 b = real;
    struct cbuf *c, *t = hdr(l);
    if (!t) return -EIO;
    u32 after = GL(t->d, O_NEXTLINK);
    for (u32 i = 0; i < 4096; i++) {
        if (!(c = hdr(b))) return -EIO;
        u32 nx = GL(c->d, O_NEXTLINK);
        if (nx == l) {
            SL(c->d, O_NEXTLINK, after);
            mark(c, K_REF, SUM_HDR);
            return 0;
        }
        if (!nx) return 0;                            /* not listed: nothing to do */
        b = nx;
    }
    return -EIO;
}
static int unlink_link(u32 l, u32 dir, u32 real)
{
    int r;
    if ((r = chain_remove(dir, l)) || (r = flush_all()) || (r = list_splice(real, l))) return r;
    return entry_release(l);
}
/* a new hard link to REAL, named NM in DIR: its block, then the file's list, then the directory's chain, each on the disk
 * before the next. After this the file has both names (d31 8) */
static int link_new(u32 real, u32 dir, const u8 *nm, u32 len, u32 sec, u32 *out)
{
    u32 b, nl;
    int r = alloc_block(&b);
    if (r) return r;
    struct cbuf *c = hdr(real);
    if (!c) return -EIO;
    nl = GL(c->d, O_NEXTLINK);
    if (!(c = new_block(b))) return -EIO;
    SL(c->d, 0, T_HEADER); SL(c->d, 1, b); SL(c->d, O_SEC, sec); SL(c->d, O_PARENT, dir);
    SL(c->d, O_REAL, real); SL(c->d, O_NEXTLINK, nl);
    set_name(c->d, nm, len);
    put_date(c->d, O_DAYS, (u32)sys_time());
    mark(c, K_NEW, SUM_HDR);
    if (!(c = hdr(real))) return -EIO;
    SL(c->d, O_NEXTLINK, b);
    mark(c, K_REF, SUM_HDR);
    if ((r = flush_all()) || (r = chain_insert(dir, b, 0)) || (r = flush_all())) return r;
    *out = b;
    return 0;
}
/* the header H, in no chain now, takes the name and place of its hard link L, and L goes */
static int take_place(u32 h, u32 l, u32 ldir)
{
    u8 nm[31];
    const u8 *p;
    u32 len;
    int r;
    struct cbuf *c = hdr(l);
    if (!c || name_of(c->d, l, &p, &len) || len > 30) return -EIO;
    memcpy(nm, p, len);
    if (!(c = hdr(h))) return -EIO;
    set_name(c->d, nm, len);
    mark(c, K_REF, SUM_HDR);
    if ((r = chain_insert(ldir, h, 1)) || (r = flush_all())) return r;
    return unlink_link(l, ldir, h);
}
/* the header H (in chain of directory HDIR) takes the name of its hard link L. Through all of this the file has a name:
 * L's, which leads to H. The same chain: one write */
static int promote(u32 h, u32 hdir, u32 l, u32 ldir)
{
    u32 h1, h2;
    int r;
    if ((r = entry_hash(h, &h1)) || (r = entry_hash(l, &h2))) return r;
    if (hdir == ldir && h1 == h2) {
        const u8 *p;
        u8 nm[31];
        u32 len;
        struct cbuf *c = hdr(l);
        if (!c || name_of(c->d, l, &p, &len) || len > 30) return -EIO;
        memcpy(nm, p, len);
        if (!(c = hdr(h))) return -EIO;
        set_name(c->d, nm, len);
        mark(c, K_REF, SUM_HDR);
        if ((r = flush_all())) return r;
        return unlink_link(l, ldir, h);
    }
    if ((r = chain_remove(hdir, h)) || (r = flush_all())) return r;
    return take_place(h, l, ldir);
}
/* the name N goes, and what it names if that was the last name. The original of a file with hard links takes over the
 * first link's name and the link goes: nothing that points at the header has to change */
static int unlink_entry(const node_t *n)
{
    int r;
    if (n->islink) {
        if ((r = unlink_link(n->key, n->parent, n->real)) || (r = dir_touch(n->parent))) return r;
        return 0;
    }
    struct cbuf *c = hdr(n->key);
    if (!c) return -EIO;
    u32 l = n->sec == ST_SOFT ? 0 : GL(c->d, O_NEXTLINK);
    if (l) {
        if (!(c = hdr(l))) return -EIO;
        u32 ld = GL(c->d, O_PARENT);
        if ((r = promote(n->key, n->parent, l, ld))) return r;
        return dir_touch(n->parent);
    }
    if ((r = chain_remove(n->parent, n->key)) || (r = dir_touch(n->parent))) return r;
    struct ofile *o = slot_ino(n->key);
    if (o) { o->orphan = 1; return 0; }               /* readable through its handle; the blocks at the last close */
    return entry_release(n->key);
}

static int do_rename(struct umsg *m)
{
    static node_t a, t;
    u32 db;
    u8 nm[31];
    const char *from = (const char *)m->a[0], *to = (const char *)m->a[1];
    int r = walk(m->a[2], from, strlen(from), 0, 0, &a);
    if (r) return r;
    if (a.key == root_blk) return -EBUSY;
    if (from[0] != '/' && a.key == m->a[2]) return -EBUSY;
    if ((r = parent_of(m->a[2], to, &db))) return r == -EINVAL ? -EBUSY : r;
    if (!dir_may()) return -EACCES;
    if ((r = name_bad())) return r;
    u32 len = q_len;
    if (len > 30) return readonly ? -EROFS : -ENAMETOOLONG;    /* (only a long-name volume, never written, gets here) */
    memcpy(nm, q, len);
    if (is_dir(&a) && !a.islink) {
        if ((r = inside(db, a.key)) < 0) return r;
        if (r) return -EINVAL;
    }
    r = lookup(db, &t);
    if (r && r != -ENOENT) return r;
    int exists = !r;
    if (exists && t.key == a.key) {                   /* the same entry */
        const u8 *p;
        u32 l;
        struct cbuf *c = hdr(a.key);
        if (!c || name_of(c->d, a.key, &p, &l)) return -EIO;
        if (l == len && !memcmp(p, nm, len)) return 0;
        exists = 0;                                   /* only the case changes */
    }
    if (exists && t.real == a.real && t.sec != ST_SOFT) return 0;     /* two names of one file: Unix leaves both */
    if (exists) {
        if (is_dir(&t) && !is_dir(&a)) return -EISDIR;
        if (!is_dir(&t) && is_dir(&a)) return -ENOTDIR;
        if (is_dir(&t) && !t.islink && (r = dir_empty(t.real))) return r;
        if (t.prot & P_DELETE) return -EPERM;
    }
    if ((r = touch())) return r;
    u32 h1, l = 0;
    if ((r = entry_hash(a.key, &h1))) return r;
    if (a.parent == db && h1 == name_hash(nm, len)) {  /* the same chain: one write */
        struct cbuf *c = hdr(a.key);
        if (!c) return -EIO;
        set_name(c->d, nm, len);
        mark(c, K_REF, SUM_HDR);
        if ((r = flush_all())) return r;
        if (exists && ((r = node_load(t.key, &t)) || (r = unlink_entry(&t)))) return r;
    } else if (a.sec == ST_SOFT) {                    /* nothing points at a soft link: a new one, the old one goes */
        struct cbuf *c = hdr(a.key);
        if (!c) return -EIO;
        memcpy(sb, c->d, BS);
        if ((r = alloc_block(&l)) || !(c = new_block(l))) return r ? r : -EIO;
        memcpy(c->d, sb, BS);
        SL(c->d, 1, l);
        set_name(c->d, nm, len);
        mark(c, K_NEW, SUM_HDR);
        if ((r = chain_insert(db, l, 0)) || (r = flush_all())) return r;
        if (exists && ((r = node_load(t.key, &t)) || (r = unlink_entry(&t)) || (r = flush_all()))) return r;
        if ((r = chain_remove(a.parent, a.key)) || (r = entry_release(a.key))) return r;
    } else {                                          /* d31 8: the new name first, as a hard link */
        if ((r = link_new(a.real, db, nm, len, is_dir(&a) ? ST_LDIR : ST_LFILE, &l))) return r;
        if (exists && ((r = node_load(t.key, &t)) || (r = unlink_entry(&t)) || (r = flush_all()))) return r;
        r = a.islink ? unlink_link(a.key, a.parent, a.real) : promote(a.key, a.parent, l, db);
        if (r) return r;
    }
    if ((r = dir_touch(a.parent)) || (db != a.parent && (r = dir_touch(db)))) return r;
    return flush_all();
}

static int do_mkdir(struct umsg *m)
{
    static node_t n;
    u32 dir, key;
    int r = parent_of(m->a[1], (const char *)m->a[0], &dir);
    if (r) return r == -EINVAL ? -EEXIST : r;
    if (!dir_may()) return -EACCES;
    r = lookup(dir, &n);
    if (!r) return -EEXIST;
    if (r != -ENOENT) return r;
    if ((r = touch()) || (r = name_bad())) return r;
    return entry_new(dir, ST_DIR, 0, 0, &key);
}

static int do_symlink(struct umsg *m)
{
    static node_t n;
    static u8 text[512];
    u32 dir, key;
    const char *target = (const char *)m->a[0];
    int r = parent_of(m->a[2], (const char *)m->a[1], &dir);
    if (r) return r == -EINVAL ? -EEXIST : r;
    if (!dir_may()) return -EACCES;
    if (!target[0]) return -ENOENT;
    r = lookup(dir, &n);
    if (!r) return -EEXIST;
    if (r != -ENOENT) return r;
    if ((r = touch()) || (r = name_bad())) return r;
    u32 cap = BS - 225 < sizeof text ? BS - 225 : sizeof text;
    int tl = link_store(target, text, cap);
    if (tl < 0) return tl;
    return entry_new(dir, ST_SOFT, text, (u32)tl, &key);
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
        if (o && !--o->count && o->orphan && !o->stale && !readonly && !touch()) return entry_release(o->ino);
        return 0;
    case FS_READ:
        if ((r = slot_get(m->a[0], &o))) return r;
        return file_read(o, m->a[1], (u8 *)m->a[2], m->a[3]);
    case FS_WRITE:
        if ((r = slot_get(m->a[0], &o))) return r;
        return file_write(o, m->a[1], (const u8 *)m->a[2], m->a[3]);
    case FS_STAT: case FS_LSTAT: {
        const char *p = (const char *)m->a[0];
        if ((r = walk(m->a[1], p, strlen(p), m->type == FS_STAT, 0, &n))) return r;
        fill_node(&n, (struct ustat *)m->a[2]);
        return 0;
    }
    case FS_FSTAT: {
        struct ustat *st = (struct ustat *)m->a[1];
        o = slot_ino(m->a[0]);
        if (o) {
            if (o->stale) return -ESTALE;
            if ((r = node_load(o->ino, &n))) return r;
            st->ino = o->ino; st->mode = S_IFREG | file_mode(o->prot); st->size = o->size;
            st->links = o->orphan ? 0 : n.links;
            st->mtime = o->mtime; st->uid = m_uid; st->gid = m_gid;
            return 0;
        }
        if ((r = ino_dir(m->a[0]))) return r == -ENOTDIR ? -ESTALE : r;
        if ((r = node_load(m->a[0], &n))) return r;
        fill_node(&n, st);
        return 0;
    }
    case FS_READDIR: return do_readdir(m);
    case FS_CHDIR: {
        const char *p = (const char *)m->a[0];
        if ((r = walk(m->a[1], p, strlen(p), 1, 0, &n))) return r;
        if (!is_dir(&n)) return -ENOTDIR;
        if (!dmode_ok(FSP_X)) return -EACCES;
        return (int)n.real;
    }
    case FS_MKDIR: return do_mkdir(m);
    case FS_UNLINK:
        if ((r = parent_of(m->a[1], (const char *)m->a[0], &dir))) return r == -EINVAL ? -EISDIR : r;
        if (!dir_may()) return -EACCES;
        if ((r = lookup(dir, &n))) return r;
        if (is_dir(&n) && !n.islink) return -EISDIR;
        if (n.prot & P_DELETE) return -EPERM;
        if ((r = touch())) return r;
        return unlink_entry(&n);
    case FS_RMDIR:
        if ((r = parent_of(m->a[1], (const char *)m->a[0], &dir))) return r;
        if (!dir_may()) return -EACCES;
        if ((r = lookup(dir, &n))) return r;
        if (!is_dir(&n)) return -ENOTDIR;
        if (!n.islink && (r = dir_empty(n.real))) return r;
        if (n.prot & P_DELETE) return -EPERM;
        if ((r = touch())) return r;
        return unlink_entry(&n);
    case FS_RENAME: return do_rename(m);
    case FS_STATFS: {
        struct ustatfs *f = (struct ustatfs *)m->a[0];
        if (npend && !readonly && (r = sync_all())) return r;
        if ((r = bm_valid ? count_free() : (free_cnt = 0, 0))) return r;
        u32 sh = BS == 512 ? 1 : 0;                   /* bsize at least 1 KB: programs divide by 1024 */
        f->bsize = BS << sh;
        f->blocks = (nblocks - reserved) >> sh; f->bfree = free_cnt >> sh; f->files = 0; f->ffree = 0;
        return 0;
    }
    case FS_SYNC:
        if (readonly) return dflush();
        return go_clean();
    case FS_CHMOD: {
        const char *p = (const char *)m->a[0];
        if ((r = walk(m->a[1], p, strlen(p), 1, 0, &n))) return r;
        if (!fsp_owner_or_root(cuid, m_uid)) return -EPERM;
        u32 w = m->a[2] & 07777;
        if (is_dir(&n)) return w == m_dmode ? 0 : -EPERM;
        /* one bit each for r, w, e: a mode the bits cannot show is refused; a class the mount's mask hides stays as it is */
        u32 prot = n.prot;
        if (m_fmode & 0444) prot = w & 0444 ? prot & ~P_READ : prot | P_READ;
        if (m_fmode & 0222) prot = w & 0222 ? prot & ~(P_WRITE | P_DELETE) : prot | P_WRITE | P_DELETE;
        if (m_fmode & 0111) prot = w & 0111 ? prot & ~P_EXEC : prot | P_EXEC;
        if (file_mode(prot) != w) return -EPERM;
        if (prot == n.prot) return 0;
        if ((r = touch())) return r;
        struct cbuf *c = hdr(n.real);
        if (!c) return -EIO;
        SL(c->d, O_PROT, prot);
        mark(c, K_REF, SUM_HDR);
        if ((o = slot_ino(n.real))) o->prot = prot;
        return 0;
    }
    case FS_CHOWN: {
        const char *p = (const char *)m->a[0];
        if ((r = walk(m->a[1], p, strlen(p), 1, 0, &n))) return r;
        if (!fsp_is_root(cuid)) return -EACCES;
        if ((m->a[2] != 0xFFFFFFFFu && m->a[2] != m_uid) || (m->a[3] != 0xFFFFFFFFu && m->a[3] != m_gid)) return -EPERM;
        return 0;
    }
    case FS_UTIMES: {
        const char *p = (const char *)m->a[0];
        if ((r = walk(m->a[1], p, strlen(p), 1, 0, &n))) return r;
        int given = m->a[2] != FS_UTIME_NOW;
        unsigned md = is_dir(&n) ? S_IFDIR | m_dmode : S_IFREG | file_mode(n.prot);
        if (!fsp_utimes_ok(cuid, m_uid, given, fsp_access(cuid, cgid, cng, cgs, md, m_uid, m_gid, FSP_W))) return -EACCES;
        if ((r = touch())) return r;
        struct cbuf *c = hdr(n.real);
        if (!c) return -EIO;
        u32 t = given ? m->a[2] : (u32)sys_time();
        put_date(c->d, O_DAYS, t);
        mark(c, K_REF, SUM_HDR);
        if ((o = slot_ino(n.real))) o->mtime = t;
        return 0;
    }
    case FS_LINK: {
        const char *p = (const char *)m->a[0];
        u32 l;
        if ((r = walk(m->a[2], p, strlen(p), 1, 0, &n))) return r;
        if (is_dir(&n)) return -EPERM;                /* no hard links to directories */
        u32 real = n.real;
        if ((r = parent_of(m->a[2], (const char *)m->a[1], &dir))) return r;
        if (!dir_may()) return -EACCES;
        if ((r = name_bad())) return r;
        r = lookup(dir, &n);
        if (!r) return -EEXIST;
        if (r != -ENOENT) return r;
        if ((r = touch())) return r;
        if ((r = link_new(real, dir, q, q_len, ST_LFILE, &l))) return r;
        return dir_touch(dir);
    }
    case FS_SYMLINK: return do_symlink(m);
    case FS_READLINK: {
        const char *p = (const char *)m->a[0];
        char t[256];
        if ((r = walk(m->a[1], p, strlen(p), 0, 0, &n))) return r;
        if (n.sec != ST_SOFT) return -EINVAL;
        int l = link_text(n.key, t, sizeof t);
        if (l < 0) return l;
        if ((u32)l > m->a[3]) l = (int)m->a[3];
        memcpy((char *)m->a[2], t, (u32)l);
        return l;
    }
    case FS_ROOT: {
        struct fsinfo *fi = (struct fsinfo *)m->a[0];
        strncpy(fi->dev, devname, sizeof fi->dev - 1);
        strcpy(fi->type, "ffs");
        return (int)root_blk;
    }
    case FS_QUIT:
        quitting = 1;
        return 0;
    }
    return -ENOSYS;
}

/* the FS_LINKED answer (kernel/fsproto.h): the continuation written over the path argument the walk stopped in. Every
 * request walks before it changes anything, so a bounced one changed nothing */
static int linked(struct umsg *m)
{
    int a1 = (m->type == FS_RENAME || m->type == FS_LINK || m->type == FS_SYMLINK) && lk_path == (const char *)m->a[1];
    char *p = (char *)m->a[a1];
    char t[256], out[FS_PATHMAX];
    int n = link_text(lk_key, t, sizeof t), r;
    if (n < 0) return n;
    if ((r = fsl_text(out, p, lk_s, lk_e, t, (unsigned)n))) return r;
    u32 e = lk_e;
    while (p[e] == '/') e++;
    int last = m->type == FS_OPEN && !p[e];           /* the link was the last name of the original path */
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
    int r = handle1(m);
    return r == -FS_LINKED ? linked(m) : r;
}

/* ---- the check before a writable mount (d31 2): the whole volume, no block named twice ---- */
static int why(const char *t)
{
    dprintf(1, "ffsd: %s: %s\n", devname, t);
    return -1;
}
static u32 *owned;                     /* one bit per block from the first after the reserved ones */
static int own(u32 b)
{
    if (!in_range(b)) return 0;
    u32 k = b - reserved;
    if (owned[k >> 5] >> (k & 31) & 1) return 0;
    owned[k >> 5] |= 1u << (k & 31);
    return 1;
}
/* is the entry KEY in the chain of its directory? 1, 0, -1 on damage or a read error */
static int in_chain(u32 key)
{
    struct cbuf *c = hdr(key);
    const u8 *n;
    u32 len;
    struct cwalk w;
    if (!c || name_of(c->d, key, &n, &len)) return -1;
    u32 h = name_hash(n, len), dir = GL(c->d, O_PARENT);
    if (!(c = hdr(dir))) return -1;
    if (GL(c->d, O_SEC) != ST_DIR && GL(c->d, O_SEC) != ST_ROOT) return -1;
    u32 e = GL(c->d, O_TABLE + h);
    cw_init(&w);
    while (e) {
        if (e == key) return 1;
        if (!(c = hdr(e))) return -1;
        e = GL(c->d, O_CHAIN);
        if (cw_step(&w, e)) return -1;
    }
    return 0;
}
/* what a power cut left half done, to finish when the whole volume has proved good (d31 8): at most one */
enum { FIX_NONE, FIX_SPLICE, FIX_TAKE, FIX_DROP };
static int fix_kind;
static u32 fix_h, fix_l, fix_dir;
static const char *check_file(u32 key)
{
    struct cbuf *c = hdr(key);
    if (!c) return "a damaged file header";
    u32 size = GL(c->d, O_SIZE), pay = is_ofs ? BS - 24 : BS, want = size / pay + (size % pay != 0), got = 0;
    if (size > MAX_SIZE || want > nblocks) return "a file larger than the volume";
    for (;;) {
        u32 hs = GL(c->d, O_HIGHSEQ), e = GL(c->d, O_EXT);
        if (hs > HT || (e && hs != HT)) return "a file's list of blocks is damaged";
        for (u32 i = 0; i < hs; i++) {
            if (!own(GL(c->d, O_TABLE + HT - 1 - i))) return "a data block outside the volume, or named twice";
            got++;
        }
        if (!e) break;
        if (!own(e)) return "an extension block outside the volume, or named twice";
        if (!(c = ext(e, key))) return "a damaged extension block";
    }
    if (got < want) return "a file with fewer blocks than its size";    /* more: a cut while it grew (d31 7) */
    if (is_ofs && want) {                             /* the first data block says so; the rest cost a read each */
        c = hdr(key);
        if (!c) return "a damaged file header";
        u32 d = GL(c->d, O_TABLE + HT - 1);
        struct cbuf *dc = cget(d, K_NEW, 0);
        if (!dc || GL(dc->d, 0) != T_DATA || !sum_good(dc) || GL(dc->d, 1) != key) return "a damaged data block";
    }
    return 0;
}
/* the file's list of hard links: each member a link to it. One not in its directory's chain: a cut around its removal */
static const char *check_list(u32 key)
{
    struct cbuf *c = hdr(key);
    if (!c) return "a damaged file header";
    u32 l = GL(c->d, O_NEXTLINK);
    for (u32 i = 0; l; i++) {
        if (i > 4096 || !(c = hdr(l))) return "a damaged list of hard links";
        u32 nx = GL(c->d, O_NEXTLINK);
        if ((GL(c->d, O_SEC) != ST_LFILE && GL(c->d, O_SEC) != ST_LDIR) || GL(c->d, O_REAL) != key) return "a damaged list of hard links";
        int in = in_chain(l);
        if (in < 0) return "a damaged list of hard links";
        if (!in) {
            if (fix_kind || !own(l)) return "more than one change left half done";
            fix_kind = FIX_SPLICE; fix_h = key; fix_l = l;
        }
        l = nx;
    }
    return 0;
}
static const char *check_tree(u32 start);
static u32 fix_walk;                   /* a directory found only through its hard link: walked after the tree */
static const char *check_link(u32 key)
{
    struct cbuf *c = hdr(key);
    if (!c) return "a damaged link";
    u32 t = GL(c->d, O_REAL), s = GL(c->d, O_SEC), dir = GL(c->d, O_PARENT), l;
    if (t == key || !(c = hdr(t)) || GL(c->d, O_SEC) != (s == ST_LFILE ? ST_FILE : ST_DIR)) return "a hard link to nothing";
    l = GL(c->d, O_NEXTLINK);
    for (u32 i = 0; l != key; i++) {
        if (!l || i > 4096 || !(c = hdr(l))) return "a hard link its file does not know";
        l = GL(c->d, O_NEXTLINK);
    }
    int in = in_chain(t);
    if (in < 0) return "a hard link to a damaged file";
    if (!in) {                                        /* a cut in a rename: the header has left its chain */
        const char *w;
        if (fix_kind == FIX_TAKE && fix_h == t) return 0;         /* its other links lead here too */
        if (fix_kind || !own(t)) return "more than one change left half done";
        /* the newest link, the head of its list, is the one it was taking the place of */
        if (!(c = hdr(t)) || !(fix_l = GL(c->d, O_NEXTLINK)) || !(c = hdr(fix_l))) return "a damaged list of hard links";
        fix_dir = GL(c->d, O_PARENT);
        fix_kind = FIX_TAKE; fix_h = t;
        if ((w = check_list(t))) return w;
        if (s == ST_LFILE) return check_file(t);
        fix_walk = t;
        return 0;
    }
    if (!(c = hdr(t))) return "a damaged file header";
    if (GL(c->d, O_PARENT) == dir) {                  /* a cut at the end of a rename: both entries have one name */
        const u8 *a, *b;
        u32 la, lb;
        u8 copy[31];
        if (name_of(c->d, t, &a, &la) || la > 30) return "a damaged file header";
        memcpy(copy, a, la);
        if (!(c = hdr(key)) || name_of(c->d, key, &b, &lb)) return "a damaged link";
        if (la == lb && !memcmp(copy, b, la)) {
            if (fix_kind) return "more than one change left half done";
            fix_kind = FIX_DROP; fix_h = t; fix_l = key; fix_dir = dir;
        }
    }
    return 0;
}
/* the walk: no stack, no recursion, whatever the depth: after a directory its header says where to go on */
static const char *check_tree(u32 start)
{
    u32 d = start, h = 0, e = 0;
    const char *w;
    for (;;) {
        struct cbuf *c;
        if (!e) {
            if (!(c = hdr(d))) return "a damaged directory";
            while (h < HT && !(e = GL(c->d, O_TABLE + h))) h++;
            if (h == HT) {
                if (d == start) return 0;
                if (entry_hash(d, &h) || !(c = hdr(d))) return "a damaged directory";
                e = GL(c->d, O_CHAIN);
                d = GL(c->d, O_PARENT);
                h++;
                continue;
            }
            h++;
        }
        const u8 *nm;
        u32 len;
        if (!own(e)) return "a header block outside the volume, or named twice (a loop?)";
        if (!(c = hdr(e))) return "a damaged header block";
        if (GL(c->d, O_PARENT) != d) return "an entry whose parent is another directory";
        if (name_of(c->d, e, &nm, &len) || name_hash(nm, len) != h - 1) return "an entry in another name's hash chain";
        u32 sec = GL(c->d, O_SEC), nx = GL(c->d, O_CHAIN);
        if (sec == ST_DIR) {
            if ((w = check_list(e))) return w;
            d = e; h = 0; e = 0;
            continue;
        }
        if (sec == ST_FILE) { if ((w = check_file(e)) || (w = check_list(e))) return w; }
        else if (sec == ST_LFILE || sec == ST_LDIR) { if ((w = check_link(e))) return w; }
        else if (sec != ST_SOFT) return "an entry of an unknown kind";
        e = nx;
    }
}
/* the bitmap against the walk: a named block marked free is damage. Nothing changes until all of it has been looked at */
static const char *check_bitmap(u32 *leaks)
{
    u32 total = nblocks - reserved;
    *leaks = 0;
    free_cnt = 0;
    for (u32 p = 0; p < nbm; p++) {                   /* first all of them: one page says what another page's block is */
        u32 b;
        if (bm_where(p, &b)) return "a bitmap pointer outside the volume";
        if (!own(b)) return "a bitmap block named twice";
    }
    for (int give = 0; give < 2; give++) {
        u32 k = 0;
        for (u32 p = 0; p < nbm; p++) {
            struct cbuf *c = bm_page(p);
            if (!c) return "a bitmap block with a bad checksum";
            for (u32 w = 1; w < LW && k < total; w++, k += 32) {
                u32 bits = total - k < 32 ? total - k : 32, in = bits < 32 ? (1u << bits) - 1 : 0xFFFFFFFFu;
                u32 fr = GL(c->d, w) & in, used = owned[k >> 5], leak = ~(fr | used) & in;
                if (!give) {
                    if (fr & used) return "a block in use is marked free";
                    for (u32 v = fr | leak; v; v &= v - 1) free_cnt++;
                    for (u32 v = leak; v; v &= v - 1) (*leaks)++;
                } else if (leak) {                    /* a block nothing names goes free: the walk proved it */
                    SL(c->d, w, GL(c->d, w) | leak);
                    mark(c, K_BM, SUM_BM);
                }
            }
        }
        if (!*leaks) break;
    }
    return 0;
}
static int check_volume(void)
{
    u32 words = ((nblocks - reserved) >> 5) + 1, leaks = 0;
    const char *w = 0;
    owned = malloc(words * 4);
    if (!owned) { why("not enough memory to check the volume: mounted read-only"); readonly = 1; return 0; }
    memset(owned, 0, words * 4);
    own(root_blk);
    /* the bitmap's extension blocks: walked in order; the pages themselves in check_bitmap */
    struct cbuf *c = hdr(root_blk);
    u32 e = c ? GL(c->d, O_BMEXT) : 0;
    while (e && !w) {
        if (!own(e) || !(c = cget(e, K_REF, 0))) w = "a damaged bitmap extension block";
        else e = GL(c->d, LW - 1);
    }
    if (!w) w = check_tree(root_blk);
    if (!w && fix_walk) w = check_tree(fix_walk);
    if (!w) w = check_bitmap(&leaks);
    free(owned);
    owned = 0;
    if (w) {
        dprintf(1, "ffsd: %s: damaged: %s: mounted read-only\n", devname, w);
        readonly = 1;
        free_cnt = 0xFFFFFFFFu;
        for (u32 i = 0; i < ncache; i++) cache[i].dirty = 0;     /* (nothing was changed; the bits stay as they were) */
        return 0;
    }
    if (leaks) {
        dprintf(1, "ffsd: %s: %u lost blocks given back\n", devname, leaks);
        dirty = 1; dirty_since = sys_ticks();
    }
    if (fix_kind) {                                   /* the volume is good but for this: finish it */
        int r = fix_kind == FIX_SPLICE ? list_splice(fix_h, fix_l) : fix_kind == FIX_DROP ? unlink_link(fix_l, fix_dir, fix_h) : take_place(fix_h, fix_l, fix_dir);
        if (!r) r = flush_all();
        if (r) { why("could not finish a change a power cut left half done: mounted read-only"); readonly = 1; return 0; }
        if (fix_kind == FIX_SPLICE) free_block(fix_l);
        dprintf(1, "ffsd: %s: finished a change a power cut left half done\n", devname);
        dirty = 1; dirty_since = sys_ticks();
    }
    return 0;
}

/* ---- the volume ---- */
/* the block size and reserved blocks from the RDB, the one on the disk or inside an MBR type 0x76 partition (rdb.h) */
static int rdb_h;
static u32 rdb_base;
static int rdb_rd(unsigned lba, void *buf)
{
    struct umsg m;
    m.type = DISK_READ; m.a[0] = (u32)rdb_h; m.a[1] = rdb_base + lba; m.a[2] = 1; m.a[3] = (u32)buf;
    if (disk_call(dport, &m) < 0) return -1;
#ifdef ZHOST
    u32 *w = buf;                                     /* rdb.h reads native longs: the Amiga's */
    for (u32 i = 0; i < 128; i++) w[i] = __builtin_bswap32(w[i]);
#endif
    return 0;
}
static void rdb_geometry(const struct diskinfo *di)
{
    static const char whole[] = "ide0";
    struct umsg m;
    struct diskinfo d;
    struct rdb *r;
    if (strncmp(di->name, "ide0p", 5) || !(r = malloc(sizeof *r))) return;
    m.type = DISK_OPEN; m.a[0] = (u32)whole; m.a[1] = 0;
    if ((rdb_h = disk_call(dport, &m)) < 0) { free(r); return; }
    for (u32 i = 0; i < 17 && !o_bs; i++) {
        rdb_base = 0;
        if (i) {
            m.type = DISK_LISTSZ; m.a[0] = i - 1; m.a[1] = (u32)&d; m.a[2] = sizeof d;
            if (disk_call(dport, &m) < 0) break;
            if (d.dostype != 0x52444200u) continue;  /* 'RDB\0': diskd's name for an MBR type 0x76 */
            rdb_base = d.start;
        }
        if (rdb_read(rdb_rd, r) != 1) continue;
        for (int k = 0; k < r->nparts; k++) {
            const struct rdb_part *p = &r->part[k];
            if (p->sectors == di->sectors && rdb_base + p->start == di->start) { o_bs = p->blksize; o_res = p->reserved; break; }
        }
    }
    free(r);
}
/* the root of a volume with block size S (a power of 2): 0 when it is one */
static int root_try(u32 s, u32 res, u32 sectors, u8 *b)
{
    u32 spb = s / 512, n = sectors / spb;
    if (n < 8 || res >= n / 2) return -1;
    u32 rt = (res + n - 1) / 2;
    BS = s; LW = s / 4; HT = LW - 56; SPB = spb; nblocks = n; reserved = res; root_blk = rt;
    if (dio(DISK_READ, rt, 1, b)) return -1;
    return GL(b, 0) == T_HEADER && GL(b, O_SEC) == ST_ROOT && GL(b, O_HSIZE) == HT && !sum_of(b) ? 0 : -1;
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
    dev_sectors = di.sectors;
    if (!o_bs) rdb_geometry(&di);
    if (o_bs && (o_bs < 512 || o_bs > 32768 || (o_bs & (o_bs - 1)))) { why("only blocks of 512 to 32768 bytes, a power of 2"); goto no; }
    u32 res = o_res ? o_res : 2;
    static u8 first[512];
    /* the boot block's type: a partition's own says the same */
    BS = 512; LW = 128; SPB = 1; nblocks = di.sectors; reserved = 0;
    if (dio(DISK_READ, 0, 1, first)) { why("cannot read the boot block"); goto no; }
    u32 t0 = GL(first, 0), dt = di.dostype;
    if ((t0 & 0xFFFFFF00u) == 0x444F5300u && (t0 & 255) < 8) dostype = t0;
    else if ((dt & 0xFFFFFF00u) == 0x444F5300u && (dt & 255) < 8) dostype = dt;
    else { why("not an AmigaDOS volume (no DOS\\0 .. DOS\\7 in the boot block)"); goto no; }
    u32 v = dostype & 255;
    is_ofs = !(v & 1); is_intl = v >= 2; is_ln = v >= 6;
    /* the root, at the size the table says, or the first size at which it fits */
    int found = 0;
    for (u32 s = o_bs ? o_bs : 512; s <= (o_bs ? o_bs : 32768) && !found; s *= 2) {
        u8 *b = malloc(s);
        if (!b) { why("out of memory"); goto no; }
        found = !root_try(s, res, di.sectors, b);
        if (found) {
            bm_valid = GL(b, O_BMFLAG) == 0xFFFFFFFFu;
            volname_len = b[s - 80] <= 30 ? b[s - 80] : 0;
            memcpy(volname, b + s - 79, volname_len);
        }
        free(b);
    }
    if (!found) { why("no root block where the format has it: not a volume, or damaged"); goto no; }
    PB = 32 * (LW - 1);
    nbm = (nblocks - reserved + PB - 1) / PB;
    next_free = root_blk;
    if (cache_alloc()) { why("out of memory"); goto no; }
    if (!readonly) {
        if (!(di.caps & (DISKCAP_FLUSH | DISKCAP_RAM)))
            why("the disk cannot flush its write cache: a power cut may damage the volume if the disk reorders writes");
        if (v > 3) { why(v < 6 ? "a directory cache volume is never written: mounted read-only" : "a long-name volume is never written: mounted read-only"); readonly = 1; }
        else if (BS != 512) { why("blocks over 512 bytes are never written (one block is not one write to the disk): mounted read-only"); readonly = 1; }
        else if (!bm_valid) { why("its bitmap is marked invalid (AmigaOS validates it at its next mount): mounted read-only"); readonly = 1; }
        else if (t0 != dostype || ((dt & 0xFFFFFF00u) == 0x444F5300u && dt != dostype)) { why("the boot block and the partition table name different types: mounted read-only"); readonly = 1; }
        else if (!nocheck && check_volume()) goto no;
    }
    return 0;
no:
    m.type = DISK_UNMOUNT; m.a[0] = (u32)dh;
    disk_call(dport, &m);
    dh = -1;
    return -1;
}

/* -o uid=N,gid=N,fmode=OCT,dmode=OCT,ro,rw,blocksize=N,reserved=N,defaults,noauto */
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
        else if (!strncmp(o, "blocksize=", 10)) o_bs = optnum(o + 10, 10, &bad);
        else if (!strncmp(o, "reserved=", 9)) o_res = optnum(o + 9, 10, &bad);
        else if (word_is(o, "ro")) readonly = 1;
        else if (word_is(o, "rw")) readonly = 0;
#ifdef ZHOST
        else if (word_is(o, "nocheck")) nocheck = 1;  /* the fuzz run: write code against damage the check would have refused */
#endif
        else if (!(word_is(o, "defaults") || word_is(o, "noauto"))) bad = 1;
        if (bad) { dprintf(1, "ffsd: bad option at: %s (uid=, gid=, fmode=, dmode=, ro, rw, blocksize=, reserved=)\n", o); return -1; }
        while (*o && *o != ',') o++;
        if (*o) o++;
    }
    return 0;
}

#ifndef ZHOST
int main(int argc, char **argv)
{
    if (argc < 2) { dprintf(1, "usage: ffsd DEVICE [-id START,SIZE] [-o OPTIONS]\n"); return 2; }
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
    if (port < 0) { dprintf(1, "ffsd: cannot create the port %s\n", pname); return 1; }
    if (restart) {                                /* the kernel's open files from before: stale until closed (d31) */
        static u32 ino[NOPEN];
        int n = sys_svc_opens(ino, sizeof ino);
        for (int i = 0, j = 0; i < n && i < NOPEN; i++)
            if (ino_dir(ino[i])) { opens[j].ino = ino[i]; opens[j].count = 1; opens[j].stale = 1; j++; }
    }
    struct umsg *m = 0;
    int warned = 0;
    for (;;) {
        if (!m) {
            if (dirty) {                          /* changes wait: the next request, or the commit time */
                u32 age = sys_ticks() - dirty_since;
                if (age < COMMIT_TICKS) m = (struct umsg *)sys3(32, (u32)port, 1 + COMMIT_TICKS - age, 0);
                if (!m) {                         /* on time, also under a steady stream of requests */
                    if (sync_all() && !warned++) dprintf(1, "ffsd: %s: a write failed: changes are not saved yet\n", devname);
                    dirty_since = sys_ticks();
                    continue;
                }
            } else m = sys_recv(port, 1);
            if ((int)m == -1) break;
        }
        int r = handle(m);
        if (quitting) {
            int e = readonly ? 0 : go_clean();
            if (e) dprintf(1, "ffsd: %s: could not save everything at umount\n", devname);
            sys_reply(m, r);
            struct umsg u;
            u.type = DISK_UNMOUNT; u.a[0] = (u32)dh;
            disk_call(dport, &u);
            return 0;
        }
        if (!dirty) {
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
