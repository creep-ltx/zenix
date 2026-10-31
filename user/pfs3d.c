/* SPDX-License-Identifier: GPL-2.0-only */
#ifdef ZHOST
#include "../tools/host/zhost.h"           /* the host tests (tools/host/fsd-host.c built with -DPFS3D) */
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
#include "amigatext.h"

/* pfs3d DEVICE -id START,SIZE [-o OPTIONS]: reads PFS3 volumes, a file service started by mount(8) as fs.DEVICE. The
 * decisions and why, and where the format's facts came from: docs/d32-pfs3.md. In short:
 * - READ-ONLY, always: there is no code here that writes to the disk. Nothing but DISK_READ goes to the disk server.
 * - What the root block names is the volume: PFS3 commits by writing a new root, so that is a whole, consistent state.
 * - Everything read from the disk is untrusted: a damaged volume answers an error, never crashes or hangs.
 * - Written from the on-disk format's facts; no code of PFS3's own is here (its licence does not mix with Zenix's). */

#define NOPEN       64                 /* NFILE: an open file has a slot here */
#define CACHE_BYTES 16384              /* reserved blocks kept: at least 8 of them */
#define MAX_RB      4096u              /* the largest reserved block */
#define MAX_FOLLOW  8                  /* soft links in a walk, hard links on the way to their object */
/* the root block, in bytes (only its first 512 bytes hold anything read here) */
#define R_TYPE      0
#define R_OPTIONS   4
#define R_DAY       12
#define R_NAME      20
#define R_LASTRES   52
#define R_FIRSTRES  56
#define R_RESSIZE   64
#define R_CLUSTER   66
#define R_FREE      68
#define R_DISKSIZE  84
#define R_EXT       88
#define R_INDEX     (96 + 5 * 4)       /* the 99 index blocks of a volume without super blocks */
/* the options word of the root */
#define M_HARDDISK  1u
#define M_SPLIT     2u
#define M_DIREXT    4u
#define M_SIZEFIELD 16u
#define M_EXTENSION 32u
#define M_SUPER     128u
#define M_LARGEFILE 2048u
#define M_KNOWN     0x1FFFu
/* the rootblock extension */
#define X_DONE      28                 /* a postponed operation: its number */
#define X_ROOTDATE  16
#define X_SUPER     64                 /* 16 super blocks */
/* the blocks */
#define ID_DB       0x4442u
#define ID_AB       0x4142u
#define ID_IB       0x4942u
#define ID_SB       0x5342u
#define ID_EX       0x4558u
#define ROOT_ANODE  5u
#define NO_ANODE    0xFFFFFFFFu
/* a directory's entries: fixed part */
#define E_TYPE      1
#define E_ANODE     2
#define E_SIZE      6
#define E_DAY       10
#define E_PROT      16
#define E_NLEN      17
#define E_NAME      18
#define DIR_FIRST   20                 /* the first entry of a directory block */
#define P_EXEC      2u                 /* the protection bits, active low: set means refused */
#define P_WRITE     4u
#define P_READ      8u

/* what an object is */
enum { K_FILE, K_DIR, K_SOFT, K_LFILE, K_LDIR, K_ROLL, K_BAD };

/* the volume (all checked at mount) */
static u32 BS, SPB, nblocks, RB, RC, res_first, res_last, apb, ipb, m_opts, root_date;
static u32 blocks_free, disk_size;
static u32 super[16];
static u8 root[512];
static u8 volname[32];
static u32 volname_len;
/* the mount */
static int dport = -1, dh = -1;
static u32 maxsect = 1;
static u32 m_uid, m_gid, m_fmode = 0755, m_dmode = 0755, o_bs;
static char devname[16];
static unsigned cuid, cgid, cng;
static unsigned short cgs[NGROUPS];
static int stoplinks, quitting;

static u32 g16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static u32 g32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* ---- the disk: reads only ---- */
static int dread(u32 block, u32 n, void *buf)
{
    u8 *p = buf;
    if (block >= nblocks || n > nblocks - block) return -EIO;
    u32 lba = block * SPB, left = n * SPB;
    while (left) {
        struct umsg m;
        u32 k = left < maxsect ? left : maxsect;
        m.type = DISK_READ; m.a[0] = (u32)dh; m.a[1] = lba; m.a[2] = k; m.a[3] = (u32)p;
        if (disk_call(dport, &m) < 0) return -EIO;
        lba += k; left -= k; p += k * 512;
    }
    return 0;
}

/* ---- reserved blocks, in a small cache; a pointer lasts until the next call ---- */
struct cblk { u32 blk, stamp; u8 valid; u8 *d; };
static struct cblk *cache;
static u32 ncache, cstamp;
static u8 *dbuf;                       /* one data block: partial reads, soft links */
static u32 dbuf_blk = 0xFFFFFFFFu;

static int cache_alloc(void)
{
    ncache = CACHE_BYTES / RB;
    if (ncache < 8) ncache = 8;
    cache = malloc(ncache * sizeof *cache);
    u8 *mem = malloc(ncache * RB);
    dbuf = malloc(BS);
    if (!cache || !mem || !dbuf) return -1;
    memset(cache, 0, ncache * sizeof *cache);
    for (u32 i = 0; i < ncache; i++) cache[i].d = mem + i * RB;
    return 0;
}
/* a reserved block lies on the grid of the reserved area */
static u8 *res_block(u32 blk)
{
    struct cblk *c, *pick = 0;
    if (blk < res_first || blk > res_last || RC > res_last - blk + 1 || (blk - res_first) % RC) return 0;
    for (u32 i = 0; i < ncache; i++) {
        c = &cache[i];
        if (c->valid && c->blk == blk) { c->stamp = ++cstamp; return c->d; }
        if (!pick || (pick->valid && (!c->valid || c->stamp < pick->stamp))) pick = c;
    }
    pick->valid = 0;
    if (dread(blk, RC, pick->d)) return 0;
    pick->blk = blk; pick->valid = 1; pick->stamp = ++cstamp;
    return pick->d;
}
/* the block with this id and sequence number */
static u8 *seq_block(u32 blk, u32 id, u32 seq)
{
    u8 *d = res_block(blk);
    return d && g16(d) == id && g32(d + 8) == seq ? d : 0;
}

/* ---- anodes. An anode number on the disk is two 16-bit parts or, on old volumes, one number; here (and as inode number)
 * it is always one small positive number, so it cannot look like an error in a reply ---- */
static u32 an_of(u32 raw)
{
    if (!(m_opts & M_SPLIT)) return raw / apb > 0xFFFF ? NO_ANODE : raw;
    if ((raw & 0xFFFF) >= apb) return NO_ANODE;
    return (raw >> 16) * apb + (raw & 0xFFFF);
}
struct anode { u32 cluster, block, next; };
static int anode_get(u32 an, struct anode *a)
{
    u32 seq = an / apb, inblock = an % apb, ix = seq / ipb, b;
    u8 *d;
    if (!an || seq > 0xFFFF) return -EIO;
    if (m_opts & M_SUPER) {
        u32 sp = ix / ipb;
        if (sp > 15 || !(d = seq_block(super[sp], ID_SB, sp))) return -EIO;
        b = g32(d + 12 + 4 * (ix % ipb));
    } else {
        if (ix > 98) return -EIO;
        b = g32(root + R_INDEX + 4 * ix);
    }
    if (!(d = seq_block(b, ID_IB, ix))) return -EIO;
    b = g32(d + 12 + 4 * (seq % ipb));
    if (!(d = seq_block(b, ID_AB, seq))) return -EIO;
    d += 16 + 12 * inblock;
    a->cluster = g32(d); a->block = g32(d + 4); a->next = g32(d + 8);
    return 0;
}
/* a chain walked with Brent's mark: a loop answers -EIO after at most a few times its length, with no memory */
struct cwalk { u32 mark, left, span; };
static void cw_init(struct cwalk *w) { w->mark = 0; w->left = 1; w->span = 1; }
static int cw_step(struct cwalk *w, u32 next)
{
    if (next && next == w->mark) return -EIO;
    if (!--w->left) { w->span *= 2; w->left = w->span; w->mark = next; }
    return 0;
}

/* ---- directories: a chain of anodes of one block each, each block full of entries of their own length ---- */
struct dwalk { u32 dir, an, ord, block, next; struct cwalk w; };
/* the block the walk stands on: 0 if it is not a block of this directory */
static u8 *dw_block(struct dwalk *w)
{
    u8 *d = res_block(w->block);
    return d && g16(d) == ID_DB && an_of(g32(d + 12)) == w->dir ? d : 0;
}
static int dw_load(struct dwalk *w)
{
    struct anode a;
    if (anode_get(w->an, &a) || a.cluster != 1) return -EIO;
    w->block = a.block; w->next = a.next;
    return 0;
}
static int dw_start(struct dwalk *w, u32 dir)
{
    w->dir = w->an = dir; w->ord = 0;
    cw_init(&w->w);
    return dw_load(w);
}
/* the next block: 0, 1 at the end, -EIO */
static int dw_next(struct dwalk *w)
{
    w->ord++;
    if (!w->next) return 1;
    u32 nx = an_of(w->next);
    if (cw_step(&w->w, nx)) return -EIO;
    w->an = nx;
    return dw_load(w);
}

typedef struct {
    u32 an;                            /* the entry's own anode */
    u32 kind, size, sizehi, mtime, link, vsize, roll;
    u32 prot;
    const u8 *name;                    /* in the cached block: only until the next read */
    u32 nlen;
} ent_t;

/* the entry at OFF of block D. The offset after it, 0 at the end of the block, -EIO on damage */
static int ent_read(const u8 *d, u32 off, ent_t *e)
{
    if (off + 2 > RB) return 0;                       /* the block is full */
    u32 sz = d[off];
    if (!sz) return 0;
    if ((sz & 1) || sz < E_NAME + 2 || off + sz > RB) return -EIO;
    const u8 *p = d + off;
    u32 nlen = p[E_NLEN];
    if (E_NAME + nlen + 1 > sz) return -EIO;
    u32 clen = p[E_NAME + nlen], used = E_NAME + nlen + 1 + clen;  /* the comment's length follows the name */
    if (used > sz) return -EIO;
    switch (p[E_TYPE]) {
    case 2: e->kind = K_DIR; break;
    case 3: e->kind = K_SOFT; break;
    case 4: e->kind = K_LDIR; break;
    case 0xFD: e->kind = K_FILE; break;
    case 0xFC: e->kind = K_LFILE; break;
    case 0xF0: e->kind = K_ROLL; break;
    default: e->kind = K_BAD; break;
    }
    e->an = an_of(g32(p + E_ANODE));
    e->size = g32(p + E_SIZE);
    e->mtime = am_to_unix(g16(p + E_DAY), g16(p + E_DAY + 2), g16(p + E_DAY + 4));
    e->prot = p[E_PROT];
    e->name = p + E_NAME; e->nlen = nlen;
    e->link = e->vsize = e->roll = e->sizehi = 0;
    if (m_opts & M_DIREXT) {
        /* the extra fields, from the end of the entry down: a word of flags, then one word for each flag set. A flag says
         * whether a 16-bit part is stored: link, link, uid, gid, prot, prot, vsize, vsize, roll, roll, high size */
        if (used + 2 > sz) return -EIO;
        u32 fl = g16(p + sz - 2), at = sz - 2;
        u32 w[11];
        for (u32 i = 0; i < 11; i++) {
            w[i] = 0;
            if (fl >> i & 1) {
                if (at < used + 2) return -EIO;
                at -= 2;
                w[i] = g16(p + at);
            }
        }
        if (w[0] || w[1]) e->link = an_of(w[0] << 16 | w[1]);
        e->vsize = w[6] << 16 | w[7];
        e->roll = w[8] << 16 | w[9];
        if (m_opts & M_LARGEFILE) e->sizehi = w[10];
    }
    return (int)(off + sz);
}

/* an entry of DIR by its anode. 0, -ENOENT, -EIO */
static int ent_find(u32 dir, u32 an, ent_t *e)
{
    struct dwalk w;
    int r, bad = 0;
    if ((r = dw_start(&w, dir))) return r;
    for (;;) {
        const u8 *d = dw_block(&w);
        if (!d) bad = 1;
        else for (int off = DIR_FIRST; ; ) {
            off = ent_read(d, (u32)off, e);
            if (off < 0) bad = 1;
            if (off <= 0) break;
            if (e->an == an) return 0;
        }
        if ((r = dw_next(&w))) return r < 0 || bad ? -EIO : -ENOENT;
    }
}

/* ---- what a name stands for ---- */
typedef struct {
    u32 own;                           /* the entry's own anode: what a long name's listed form says */
    u32 real;                          /* the object it is: its anode, the inode number */
    u32 at;                            /* the directory the name was found in */
    u32 kind;                          /* K_FILE, K_DIR, K_SOFT or K_ROLL */
    u32 size;                          /* what a program sees */
    u32 fsize, roll;                   /* a rollover file: the circle's size, where it starts */
    u32 prot, mtime, links;
    u8 big;                            /* 4 GB or more: seen, cannot be opened */
} node_t;

static int is_dir(const node_t *n) { return n->kind == K_DIR; }
/* entry E of DIR as a node: a hard link becomes its object */
static int node_of(ent_t *e, u32 dir, node_t *n)
{
    u32 kind = e->kind;
    int r;
    n->own = e->an; n->at = dir;
    for (int i = 0; kind == K_LFILE || kind == K_LDIR; i++) {
        struct anode a;                               /* a link's anode: the object's directory; then the object's anode */
        u32 want = e->link;
        if (i > MAX_FOLLOW || !want || anode_get(e->an, &a)) return -EIO;
        dir = an_of(a.cluster);
        if ((r = ent_find(dir, want, e))) return r == -ENOENT ? -EIO : r;
        if (kind == K_LDIR ? e->kind != K_DIR && e->kind != K_LDIR : e->kind == K_DIR || e->kind == K_LDIR) return -EIO;
        kind = e->kind;
    }
    if (kind == K_BAD || !e->an || e->an == NO_ANODE) return -EIO;
    n->real = e->an; n->kind = kind;
    n->prot = e->prot; n->mtime = e->mtime;
    n->size = n->fsize = e->size; n->roll = 0; n->big = e->sizehi != 0;
    if (kind == K_ROLL) {
        if (e->vsize > e->size || (e->size ? e->roll >= e->size : e->roll)) return -EIO;
        n->size = e->vsize; n->roll = e->roll; n->big = 0;
        n->kind = K_FILE;
    }
    if (n->big) n->size = 0xFFFFFFFFu;
    /* an object's links: itself and the list of them that starts in its own entry */
    n->links = 1;
    u32 l = kind == K_DIR ? 0 : e->link;
    for (u32 i = 0; l && i < 256; i++) {
        struct anode a;
        if (anode_get(l, &a)) break;
        n->links++;
        l = a.next ? an_of(a.next) : 0;
    }
    return 0;
}
/* the root's own node */
static void node_root(node_t *n)
{
    memset(n, 0, sizeof *n);
    n->own = n->real = n->at = ROOT_ANODE; n->kind = K_DIR; n->mtime = root_date; n->links = 2;
}
/* AN as a directory: stateless. 0, -ESTALE (no anode there any more), -ENOTDIR */
static int ino_dir(u32 an)
{
    struct dwalk w;
    if (dw_start(&w, an)) return -ESTALE;
    return dw_block(&w) ? 0 : -ENOTDIR;
}
/* the parent of directory DIR */
static int dir_parent(u32 dir, u32 *out)
{
    struct dwalk w;
    if (dw_start(&w, dir)) return -EIO;
    const u8 *d = dw_block(&w);
    if (!d) return -EIO;
    *out = an_of(g32(d + 16));
    return 0;
}
/* directory DIR as a node: its entry is in its parent */
static int dir_node(u32 dir, node_t *n)
{
    ent_t e;
    u32 p;
    int r;
    if (dir == ROOT_ANODE) { node_root(n); return 0; }
    if ((r = dir_parent(dir, &p))) return r;
    if ((r = ent_find(p, dir, &e))) return r == -ENOENT ? -EIO : r;
    if (e.kind != K_DIR) return -EIO;
    return node_of(&e, p, n);
}

/* ---- names ---- */
static u8 q[108];                      /* the name asked for: Latin-1 */
static u32 q_len;
static u32 up(u32 c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 224 && c <= 254 && c != 247)) return c - 32;
    return c;
}
/* a long name's listed form: its anode in hex after the last ~. 0 if q has no such end */
static u32 alias_of(void)
{
    u32 t = q_len, key = 0, sh = 0;
    while (t && q[t - 1] != '~' && sh < 32) {
        u32 ch = q[--t], v = ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= '0' && ch <= '9' ? ch - '0' : 99;
        if (v > 15) return 0;
        key |= v << sh; sh += 4;
    }
    return t >= 2 && sh ? key : 0;
}
/* the entry of DIR named q (or a long name's listed form). 0, -ENOENT, -EIO. Damage in a block: the rest of that block
 * cannot be read, the search goes on in the next, and a name not found is -EIO then: it may be in what could not be read */
static int lookup(u32 dir, node_t *n)
{
    struct dwalk w;
    ent_t e;
    int r, bad = 0;
    if ((r = dw_start(&w, dir))) return -EIO;
    for (;;) {
        const u8 *d = dw_block(&w);
        if (!d) bad = 1;
        else for (int off = DIR_FIRST; ; ) {
            off = ent_read(d, (u32)off, &e);
            if (off < 0) bad = 1;
            if (off <= 0) break;
            if (e.nlen != q_len) continue;
            u32 i = 0;
            while (i < q_len && up(e.name[i]) == up(q[i])) i++;
            if (i == q_len) return node_of(&e, dir, n);
        }
        if ((r = dw_next(&w))) { if (r < 0) bad = 1; break; }
    }
    if (bad) return -EIO;
    u32 key = alias_of();
    if (!key || ent_find(dir, key, &e)) return -ENOENT;
    char alias[64], want[64];
    if (am_utf8_of(e.name, e.nlen, alias, 64, 0) >= 0) return -ENOENT;
    am_name_list(e.name, e.nlen, key, alias, 64);
    if (am_utf8_of(q, q_len, want, 64, 0) < 0 || strcmp(want, alias)) return -ENOENT;
    return node_of(&e, dir, n);
}

/* ---- file data ---- */
static int data_ok(u32 b, u32 n) { return b > res_last && b < nblocks && n <= nblocks - b; }
struct ofile {
    u32 ino, count, stale;
    u32 size, fsize, roll, prot, mtime, links;
    u32 an, start, cluster, block;     /* where the last read was: anode, its first block in the file, what it says */
    u32 next;
    struct cwalk w;                    /* a chain that loops is met however many reads it takes to get there */
};
static struct ofile opens[NOPEN];
static struct ofile *slot_ino(u32 ino)
{
    for (u32 i = 0; i < NOPEN; i++) if (opens[i].count && opens[i].ino == ino) return &opens[i];
    return 0;
}
static int slot_load(struct ofile *o, u32 an)
{
    struct anode a;
    if (anode_get(an, &a) || a.cluster > nblocks) return -EIO;
    o->an = an; o->cluster = a.cluster; o->block = a.block; o->next = a.next;
    return 0;
}
/* the disk block of the file's block IDX and how many follow it on the disk */
static int block_at(struct ofile *o, u32 idx, u32 *blk, u32 *run)
{
    int r;
    if (!o->an || idx < o->start) {
        o->start = 0;
        cw_init(&o->w);
        if ((r = slot_load(o, o->ino))) { o->an = 0; return r; }
    }
    while (idx - o->start >= o->cluster) {
        u32 nx = an_of(o->next);
        if (!o->next || cw_step(&o->w, nx)) return -EIO;  /* the chain is shorter than the file's size, or loops */
        o->start += o->cluster;
        if ((r = slot_load(o, nx))) { o->an = 0; return r; }
    }
    u32 in = idx - o->start;
    *blk = o->block + in; *run = o->cluster - in;
    if (*blk < in || !data_ok(*blk, *run)) return -EIO;
    return 0;
}
static const u8 *data_block(u32 b)
{
    if (b != dbuf_blk) {
        dbuf_blk = 0xFFFFFFFFu;
        if (dread(b, 1, dbuf)) return 0;
        dbuf_blk = b;
    }
    return dbuf;
}
/* N bytes at byte P of the file as the disk has it. The bytes read, or -EIO */
static int phys_read(struct ofile *o, u32 p, u8 *buf, u32 n)
{
    u32 done = 0;
    while (done < n) {
        u32 idx = p / BS, in = p % BS, left = n - done, b, run;
        int r = block_at(o, idx, &b, &run);
        if (r) return done ? (int)done : r;
        if (!in && left >= BS && !((u32)(buf + done) & 1)) {   /* whole blocks go straight to the caller */
            u32 cnt = left / BS;
            if (cnt > run) cnt = run;
            if (dread(b, cnt, buf + done)) return done ? (int)done : -EIO;
            done += cnt * BS; p += cnt * BS;
            continue;
        }
        const u8 *d = data_block(b);
        if (!d) return done ? (int)done : -EIO;
        u32 k = BS - in;
        if (k > left) k = left;
        memcpy(buf + done, d + in, k);
        done += k; p += k;
    }
    return (int)done;
}
/* a rollover file is a circle: its first byte is at ROLL, and it goes round */
static int file_read(struct ofile *o, u32 off, u8 *buf, u32 n)
{
    if (off >= o->size) return 0;
    if (n > o->size - off) n = o->size - off;
    if (n > 0x7FFFFFFFu) n = 0x7FFFFFFFu;
    if (!o->roll) return phys_read(o, off, buf, n);
    u32 tail = o->fsize - o->roll;                    /* the bytes before it goes round */
    if (off >= tail) return phys_read(o, off - tail, buf, n);
    if (n <= tail - off) return phys_read(o, o->roll + off, buf, n);
    u32 first = tail - off;
    int r = phys_read(o, o->roll + off, buf, first);
    if (r != (int)first) return r;
    r = phys_read(o, 0, buf + first, n - first);
    return r < 0 ? (int)first : (int)first + r;
}

/* ---- soft links ---- */
/* the text of soft link AN as a Zenix path. A text from the volume's root goes up UPS names, what the path that reached
 * the link is deep; UPS < 0: as deep as directory DIR, which is the name's own */
static int link_text(u32 an, u32 size, u32 dir, int ups, char *out, u32 cap)
{
    struct anode a;
    static u8 copy[256];
    if (!size) return -ENOENT;
    if (size > BS || anode_get(an, &a) || !a.cluster || !data_ok(a.block, 1)) return -EIO;
    if (size > 255) return -ENAMETOOLONG;
    const u8 *d = data_block(a.block);
    if (!d) return -EIO;
    u32 n = 0, k = 0;
    while (n < size && d[n]) n++;
    if (!n) return -ENOENT;
    memcpy(copy, d, n);
    int st = am_link_root(copy, n, volname, volname_len, up);
    if (st >= 0) {
        for (u32 i = 0; ups < 0 ? dir != ROOT_ANODE : i < (u32)ups; i++) {
            u32 p;
            if (ups < 0 && (i > 512 || dir_parent(dir, &p))) return -EIO;
            if (k + 3 >= cap) return -ENAMETOOLONG;
            out[k++] = '.'; out[k++] = '.'; out[k++] = '/';
            if (ups < 0) dir = p;
        }
    }
    return am_link_rest(copy, st < 0 ? 0 : (u32)st, n, out, k, cap);
}

/* ---- paths ---- */
static const char *lk_path;            /* where a walk stopped at a link (D26): the path, the name, the link */
static u32 lk_s, lk_e;
static node_t lk_node;

static int links_left;                 /* in one request, as the kernel's limit: a few links that name each other many times end */
static int dmode_ok(int want) { return fsp_access(cuid, cgid, cng, cgs, S_IFDIR | m_dmode, m_uid, m_gid, want); }

/* walk PATH's first LEN bytes from directory CWD; the end in N. Links are followed inside the volume (not the last name
 * unless FOLLOW), or with stoplinks the walk ends at the first one with -FS_LINKED */
static int walk(u32 cwd, const char *path, u32 len, int follow, int depth, node_t *n)
{
    u32 cur = path[0] == '/' ? ROOT_ANODE : cwd, i = 0;
    int r;
    if (depth > MAX_FOLLOW) return -ELOOP;
    if ((r = ino_dir(cur))) return r;
    for (;;) {
        while (i < len && path[i] == '/') i++;
        if (i >= len) return dir_node(cur, n);
        u32 s = i, k;
        while (i < len && path[i] != '/') i++;
        k = i;
        while (k < len && path[k] == '/') k++;
        int last = k >= len;
        if (!dmode_ok(FSP_X)) return -EACCES;
        if (i - s == 1 && path[s] == '.') continue;
        if (i - s == 2 && path[s] == '.' && path[s + 1] == '.') {
            if (cur != ROOT_ANODE) {
                u32 p;
                if ((r = dir_parent(cur, &p))) return r;
                if ((r = ino_dir(p))) return r == -ESTALE || r == -ENOTDIR ? -EIO : r;
                cur = p;
            }
            continue;
        }
        if ((r = am_name_in(path + s, i - s, q, 107, &q_len))) return r == -EILSEQ ? -ENOENT : r;
        if ((r = lookup(cur, n))) return r;
        if (n->kind == K_SOFT && (!last || follow)) {
            if (stoplinks && path[0] == '/') {
                lk_path = path; lk_s = s; lk_e = i; lk_node = *n;
                return -FS_LINKED;
            }
            char t[256];
            if (--links_left < 0) return -ELOOP;
            int m = link_text(n->real, n->size, cur, -1, t, sizeof t);
            if (m < 0) return m;
            /* the link's text is relative to the directory that holds its name */
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
    return am_name_in(path + s, n - s, q, 107, &q_len);
}
/* What a change answers: the same as on a writable filesystem up to the point of changing anything (a link leading out of
 * the volume bounces, FS_LINKED, and the change happens where it leads), then read-only. WANT 1: the name must be there,
 * 0: it must not, -1: only its directory matters */
static int refuse_at(u32 cwd, const char *path, int want)
{
    static node_t t;
    u32 dir;
    int r = parent_of(cwd, path, &dir);
    if (r) return r;
    if (!dmode_ok(FSP_X)) return -EACCES;
    if (want < 0) return -EROFS;
    r = lookup(dir, &t);
    if (r && r != -ENOENT) return r;
    if (want && r) return r;
    if (!want && !r) return -EEXIST;
    return -EROFS;
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
    st->ino = n->real;
    st->uid = m_uid; st->gid = m_gid;
    st->mtime = n->mtime;
    st->size = n->size; st->links = n->links;
    if (is_dir(n)) { st->mode = S_IFDIR | m_dmode; st->size = RB; st->links = 2; }
    else if (n->kind == K_SOFT) st->mode = 0120000 | 0777;
    else st->mode = S_IFREG | file_mode(n->prot);
}
static int slot_new(const node_t *n)
{
    struct ofile *o = slot_ino(n->real);
    if (o) {
        if (o->stale) return -ESTALE;
        o->count++;
        return 0;
    }
    for (u32 i = 0; i < NOPEN; i++) {
        o = &opens[i];
        if (o->count) continue;
        memset(o, 0, sizeof *o);
        o->ino = n->real; o->count = 1; o->size = n->size; o->fsize = n->fsize; o->roll = n->roll;
        o->prot = n->prot; o->mtime = n->mtime; o->links = n->links;
        return 0;
    }
    return -ENFILE;
}
static int do_open(struct umsg *m)
{
    static node_t n;
    const char *path = (const char *)m->a[0];
    u32 flags = m->a[2];
    int r = walk(m->a[1], path, strlen(path), !((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)), 0, &n);
    struct ustat *st = (struct ustat *)m->a[3];
    if (r == -ENOENT && (flags & O_CREAT)) {          /* in a directory that exists it would be made: read-only */
        u32 dir;
        return (r = parent_of(m->a[1], path, &dir)) ? r : -EROFS;
    }
    if (r) return r;
    if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) return -EEXIST;
    if (is_dir(&n)) {
        if (flags & (O_WRONLY | O_RDWR)) return -EISDIR;
        if (!fsp_access(cuid, cgid, cng, cgs, S_IFDIR | m_dmode, m_uid, m_gid, fsp_open_want(flags))) return -EACCES;
        if (ino_dir(n.real)) return -EIO;             /* an entry says directory; its first block says otherwise */
        fill_node(&n, st);
        return (int)n.real;
    }
    if (n.kind == K_SOFT) return -ELOOP;              /* (a link not followed: O_EXCL answered above) */
    if (!fsp_access(cuid, cgid, cng, cgs, S_IFREG | file_mode(n.prot), m_uid, m_gid, fsp_open_want(flags))) return -EACCES;
    if (flags & (O_WRONLY | O_RDWR | O_TRUNC)) return -EROFS;
    if (n.big) return -EFBIG;
    if ((r = slot_new(&n))) return r;
    fill_node(&n, st);
    return (int)n.real;
}
/* A listing's place: bits 30..11 which block of the directory, bits 10..0 half the offset in it; bit 31 damage was met.
 * A damaged block is left for the next one, and the listing answers -EIO at its end, so what can be read is seen.
 * The walk is kept for the next request: no walk from the start at each block of a big directory */
#define RD_ORD(c)  ((c) >> 11 & 0xFFFFF)
#define RD_OFF(c)  (((c) & 0x7FF) * 2)
static struct dwalk rd;
static int rd_live;
static int do_readdir(struct umsg *m)
{
    struct udirent *ud = (struct udirent *)m->a[2];
    u32 dir = m->a[0], cookie = m->a[1], bad = cookie >> 31, ord = RD_ORD(cookie), off = RD_OFF(cookie);
    u32 *newc = (u32 *)m->a[3];
    ent_t e;
    int r = ino_dir(dir);
    if (r) return slot_ino(dir) ? -ENOTDIR : -EIO;
    if (!rd_live || rd.dir != dir || rd.ord > ord) {
        if ((r = dw_start(&rd, dir))) { rd_live = 0; return bad ? -EIO : r; }
        rd_live = 1;
    }
    while (rd.ord < ord) {
        if ((r = dw_next(&rd))) { rd_live = 0; return r < 0 || bad ? -EIO : 0; }
    }
    if (!off) off = DIR_FIRST;
    for (;;) {
        const u8 *d = dw_block(&rd);
        int nx;
        if (!d) { bad = 1; nx = 0; }
        else if (off < DIR_FIRST || off & 1) { bad = 1; nx = 0; }
        else {
            nx = ent_read(d, off, &e);
            if (nx < 0) { bad = 1; nx = 0; }
        }
        if (nx) {
            if (e.kind == K_BAD || !e.nlen) { bad = 1; off = (u32)nx; continue; }
            am_name_list(e.name, e.nlen, e.an, ud->name, sizeof ud->name);
            ud->type = e.kind == K_DIR || e.kind == K_LDIR ? 2 : e.kind == K_SOFT ? 7 : 1;
            ud->ino = e.kind == K_LFILE || e.kind == K_LDIR ? e.link : e.an;
            if ((u32)nx >= RB) {                      /* the block is full: the next call starts on the next one */
                *newc = bad << 31 | (rd.ord + 1) << 11;
                if (rd.ord + 1 > 0xFFFFF) return -EIO;
            } else *newc = bad << 31 | rd.ord << 11 | (u32)nx / 2;
            return 1;
        }
        r = dw_next(&rd);
        if (r) {
            rd_live = 0;
            *newc = cookie;
            return r < 0 || bad ? -EIO : 0;
        }
        if (rd.ord > 0xFFFFF) { rd_live = 0; return -EIO; }
        off = DIR_FIRST;
    }
}
static int handle1(struct umsg *m)
{
    static node_t n;
    struct ofile *o;
    int r;
    switch (m->type) {
    case FS_OPEN: return do_open(m);
    case FS_CLOSE:
        o = slot_ino(m->a[0]);
        if (o) o->count--;
        return 0;
    case FS_READ:
        o = slot_ino(m->a[0]);
        if (!o || o->stale) return ino_dir(m->a[0]) ? -ESTALE : -EISDIR;
        return file_read(o, m->a[1], (u8 *)m->a[2], m->a[3]);
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
            st->ino = o->ino; st->mode = S_IFREG | file_mode(o->prot); st->size = o->size; st->links = o->links;
            st->mtime = o->mtime; st->uid = m_uid; st->gid = m_gid;
            return 0;
        }
        if ((r = ino_dir(m->a[0]))) return r == -ENOTDIR ? -ESTALE : r;
        if ((r = dir_node(m->a[0], &n))) return r;
        fill_node(&n, st);
        return 0;
    }
    case FS_READDIR: return do_readdir(m);
    case FS_CHDIR: {
        const char *p = (const char *)m->a[0];
        if ((r = walk(m->a[1], p, strlen(p), 1, 0, &n))) return r;
        if (!is_dir(&n)) return -ENOTDIR;
        if (!dmode_ok(FSP_X)) return -EACCES;
        if (ino_dir(n.real)) return -EIO;
        return (int)n.real;
    }
    /* changes: there is no code for them, on purpose (docs/d32-pfs3.md); the kernel still needs to know where a path leads */
    case FS_WRITE:
        return -EROFS;
    case FS_MKDIR:
        return refuse_at(m->a[1], (const char *)m->a[0], 0);
    case FS_UNLINK: case FS_RMDIR:
        r = refuse_at(m->a[1], (const char *)m->a[0], 1);
        return r == -EINVAL && m->type == FS_UNLINK ? -EISDIR : r;
    case FS_RENAME: {
        if ((r = refuse_at(m->a[2], (const char *)m->a[0], 1)) != -EROFS) return r;
        return refuse_at(m->a[2], (const char *)m->a[1], -1);
    }
    case FS_LINK: {
        const char *p = (const char *)m->a[0];
        if ((r = walk(m->a[2], p, strlen(p), 1, 0, &n))) return r;
        return refuse_at(m->a[2], (const char *)m->a[1], 0);
    }
    case FS_SYMLINK:
        return refuse_at(m->a[2], (const char *)m->a[1], 0);
    case FS_CHMOD: case FS_CHOWN: case FS_UTIMES: {
        const char *p = (const char *)m->a[0];
        return (r = walk(m->a[1], p, strlen(p), 1, 0, &n)) ? r : -EROFS;
    }
    case FS_STATFS: {
        struct ustatfs *f = (struct ustatfs *)m->a[0];
        u32 sh = BS == 512 ? 1 : 0;                   /* bsize at least 1 KB: programs divide by 1024 */
        f->bsize = BS << sh;
        f->blocks = disk_size >> sh; f->bfree = blocks_free >> sh; f->files = 0; f->ffree = 0;
        return 0;
    }
    case FS_SYNC:
        return 0;
    case FS_READLINK: {
        const char *p = (const char *)m->a[0];
        char t[256];
        if ((r = walk(m->a[1], p, strlen(p), 0, 0, &n))) return r;
        if (n.kind != K_SOFT) return -EINVAL;
        u32 pl = strlen(p);
        while (pl && p[pl - 1] == '/') pl--;
        while (pl && p[pl - 1] != '/') pl--;
        int l = link_text(n.real, n.size, n.at, p[0] == '/' ? (int)am_path_depth(p, pl) : -1, t, sizeof t);
        if (l < 0) return l;
        if ((u32)l > m->a[3]) l = (int)m->a[3];
        memcpy((char *)m->a[2], t, (u32)l);
        return l;
    }
    case FS_ROOT: {
        struct fsinfo *fi = (struct fsinfo *)m->a[0];
        strncpy(fi->dev, devname, sizeof fi->dev - 1);
        strcpy(fi->type, "pfs3");
        return (int)ROOT_ANODE;
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
    char t[256], out[FS_PATHMAX];
    int n = link_text(lk_node.real, lk_node.size, lk_node.at, (int)am_path_depth(p, lk_s), t, sizeof t), r;
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
    links_left = 40;
    int r = handle1(m);
    return r == -FS_LINKED ? linked(m) : r;
}

/* ---- the mount ---- */
static int why(const char *t)
{
    dprintf(1, "pfs3d: %s: %s\n", devname, t);
    return -1;
}
/* a type a PFS root or boot block has: PFS\\1, PFS\\2, muPF, AFS\\1, muAF */
static int pfs_id(u32 t)
{
    return t == 0x50465301u || t == 0x50465302u || t == 0x6D755046u || t == 0x41465301u || t == 0x6D754146u;
}
/* the root at block size S (a power of 2). 0, or -1 when it is not there. The checks: whatever a later sum relies on */
static int root_try(u32 s, u32 sectors)
{
    u32 spb = s / 512;
    static u8 first[512];
    if (sectors / spb < 8) return -1;
    /* the first sector of each is all that is needed here */
    SPB = 1; nblocks = sectors;
    int bad = dread(2 * spb, 1, root) || dread(0, 1, first);
    SPB = spb; nblocks = sectors / spb; BS = s;
    if (bad || !pfs_id(g32(root + R_TYPE)) || !pfs_id(g32(first))) return -1;
    if (!g32(root + R_OPTIONS) || !g16(root + R_RESSIZE)) return -1;
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
    if (o_bs && (o_bs < 512 || o_bs > MAX_RB || (o_bs & (o_bs - 1)))) { why("only blocks of 512 to 4096 bytes, a power of 2"); goto no; }
    /* where the root is, smallest block first: a format writes blocks 0 and 1 whole, so at a larger block size every smaller
     * size's root place is gone; at a smaller one an older, larger volume's root may still lie further on and must not be
     * found. An original PFS\1 volume keeps 512-byte blocks even when the partition table says more */
    int found = 0;
    for (u32 s = o_bs ? o_bs : 512; s <= (o_bs ? o_bs : MAX_RB) && !found; s *= 2)
        if (!root_try(s, di.sectors)) found = 1;
    if (!found) { why("not a PFS volume (no PFS root block in its place)"); goto no; }
    m_opts = g32(root + R_OPTIONS);
    RB = g16(root + R_RESSIZE);
    res_first = g32(root + R_FIRSTRES); res_last = g32(root + R_LASTRES);
    u32 type = g32(root + R_TYPE), rcl = g16(root + R_CLUSTER);
    if (m_opts & ~M_KNOWN) { why("options this server does not know (a newer format): refused"); goto no; }
    if (!(m_opts & M_HARDDISK)) { why("not in hard disk mode (an old floppy format): refused"); goto no; }
    if ((type == 0x50465301u || type == 0x6D755046u) && (BS != 512 || RB > 1024 || (m_opts & M_LARGEFILE))) { why("an original PFS type with the newer format's features: damaged"); goto no; }
    if (RB < BS || RB > MAX_RB || (RB & (RB - 1)) || rcl < 1 || rcl > 521) { why("impossible reserved block sizes: damaged"); goto no; }
    RC = RB / BS;
    apb = (RB - 16) / 12; ipb = (RB - 12) / 4;
    if (res_first < 2 || res_last < res_first || res_last >= nblocks || res_last - res_first < RC) { why("the reserved area is not inside the partition: damaged"); goto no; }
    /* the root's disk size counts 512-byte sectors, whatever the block size (verified on a PFS\\2 volume with 1 KB blocks) */
    disk_size = (m_opts & M_SIZEFIELD) && g32(root + R_DISKSIZE) ? g32(root + R_DISKSIZE) / SPB : nblocks;
    if (disk_size > nblocks) { why("the volume is larger than its partition: refused"); goto no; }
    blocks_free = g32(root + R_FREE);
    if (blocks_free > disk_size) blocks_free = 0;
    volname_len = root[R_NAME] < 32 ? root[R_NAME] : 0;
    memcpy(volname, root + R_NAME + 1, volname_len);
    root_date = am_to_unix(g16(root + R_DAY), g16(root + R_DAY + 2), g16(root + R_DAY + 4));
    if (cache_alloc()) { why("out of memory"); goto no; }
    if (m_opts & M_EXTENSION) {
        const u8 *x = res_block(g32(root + R_EXT));
        if (!x || g16(x) != ID_EX) { why("its rootblock extension is damaged"); goto no; }
        root_date = am_to_unix(g16(x + X_ROOTDATE), g16(x + X_ROOTDATE + 2), g16(x + X_ROOTDATE + 4));
        for (u32 i = 0; i < 16; i++) super[i] = g32(x + X_SUPER + 4 * i);
        if (g32(x + X_DONE)) why("AmigaOS was cut off in the middle of a delete; it finishes that at its next mount. What is read here is the state on the disk");
    } else if (m_opts & M_SUPER) { why("super blocks but no rootblock extension: damaged"); goto no; }
    if (ino_dir(ROOT_ANODE)) { why("its root directory cannot be found: damaged"); goto no; }
    return 0;
no:
    m.type = DISK_UNMOUNT; m.a[0] = (u32)dh;
    disk_call(dport, &m);
    dh = -1;
    return -1;
}

/* -o uid=N,gid=N,fmode=OCT,dmode=OCT,ro,blocksize=N,defaults,noauto */
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
        else if (word_is(o, "rw")) { dprintf(1, "pfs3d: a PFS3 volume is only ever read on Zenix: -o rw is refused\n"); return -1; }
        else if (!(word_is(o, "ro") || word_is(o, "defaults") || word_is(o, "noauto"))) bad = 1;
        if (bad) { dprintf(1, "pfs3d: bad option at: %s (uid=, gid=, fmode=, dmode=, ro, blocksize=)\n", o); return -1; }
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
    if (argc < 2) { dprintf(1, "usage: pfs3d DEVICE [-id START,SIZE] [-o OPTIONS]\n"); return 2; }
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
    if (port < 0) { dprintf(1, "pfs3d: cannot create the port %s\n", pname); return 1; }
    if (restart) {                                /* the kernel's open files from before: stale until closed, as ffsd */
        static u32 ino[NOPEN];
        int n = sys_svc_opens(ino, sizeof ino);
        for (int i = 0, j = 0; i < n && i < NOPEN; i++)
            if (ino_dir(ino[i])) { opens[j].ino = ino[i]; opens[j].count = 1; opens[j].stale = 1; j++; }
    }
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
