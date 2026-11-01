// SPDX-License-Identifier: GPL-2.0-only
/* The LhA reader (DESIGN D33, docs/d33-archives.md): header levels 0, 1 and 2; -lh0-, -lz4- (stored), -lh1- (adaptive
 * Huffman), -lh4- to -lh7- (blocks of static Huffman tables), -lhd- (a directory). Written from the format's public
 * descriptions and checked against what the real tools write; the note says which fact came from where. Memory per
 * member: the window (4, 8, 32 or 64 KB) and 6.6 KB of tables and input; -lh1- 4 KB and 7.2 KB. */
#include "arcread.h"

#define INBUF 2048
#define NC 510
#define NT 19
#define NP 17
#define KC 10
#define KT 7
#define MAXEXT 64

/* the bytes of the stream, and what the -lh1- and -lh4-7 decoders share */
struct lzin {
    const struct arcsrc *src;
    unsigned long off, end;
    int pos, len, past;
    arc_u32 bb;             /* the next bits, first bit highest */
    int nb;
    int err;
    arc_u8 *win;
    unsigned wmask;
    unsigned wpos, wstart;  /* the next byte, the first not handed on */
    unsigned long total, size;
    unsigned crc;
    const struct arcsink *out;
    arc_u8 buf[INBUF];
};

static long src_read(const struct arcsrc *s, unsigned long off, void *buf, long n)
{
    if (off >= s->size) return 0;
    if ((unsigned long)n > s->size - off) n = (long)(s->size - off);
    return s->read(s->ctx, off, buf, n);
}

static int flushw(struct lzin *z)
{
    unsigned n = z->wpos - z->wstart;
    if (!n) return 0;
    z->crc = arc_crc16(z->crc, z->win + z->wstart, (long)n);
    if (z->out && !z->out->write(z->out->ctx, z->win + z->wstart, (long)n)) return ARC_ESTOP;
    z->wstart = z->wpos & z->wmask;
    z->wpos = z->wstart;
    return 0;
}

static unsigned nextbyte(struct lzin *z)
{
    if (z->pos >= z->len) {
        long want = INBUF, r = 0;
        if (z->off < z->end) {
            if ((unsigned long)want > z->end - z->off) want = (long)(z->end - z->off);
            r = src_read(z->src, z->off, z->buf, want);
            if (r < 0) { z->err = ARC_EREAD; r = 0; }
            z->off += (unsigned long)r;
        }
        z->pos = 0;
        z->len = (int)r;
        if (!r) { z->past++; return 0; }
    }
    return z->buf[z->pos++];
}

#define FILL(z) do { while ((z)->nb <= 24) { (z)->bb |= (arc_u32)nextbyte(z) << (24 - (z)->nb); (z)->nb += 8; } } while (0)
static inline unsigned peek16(struct lzin *z) { FILL(z); return z->bb >> 16; }
static inline void drop(struct lzin *z, int k) { z->bb <<= k; z->nb -= k; }
static inline unsigned getn(struct lzin *z, int k)      /* k <= 16 */
{
    unsigned v;
    if (!k) return 0;
    FILL(z);
    v = z->bb >> (32 - k);
    z->bb <<= k;
    z->nb -= k;
    return v;
}

/* one byte out of the decoder, or a copy from d back */
static inline int putbyte(struct lzin *z, unsigned c)
{
    z->win[z->wpos++] = (arc_u8)c;
    z->total++;
    if (z->wpos > z->wmask) return flushw(z);
    return 0;
}
static int copy(struct lzin *z, unsigned d, unsigned n)
{
    if ((unsigned long)n > z->size - z->total) return ARC_ECORRUPT;     /* more than the header promised */
    while (n--) {
        int r = putbyte(z, z->win[(z->wpos - d) & z->wmask]);
        if (r) return r;
    }
    return 0;
}
static void zinit(struct lzin *z, const struct arcsrc *src, const struct arcent *e, const struct arcsink *out,
                  arc_u8 *win, unsigned wsize)
{
    unsigned i;
    z->src = src;
    z->off = e->dataoff;
    z->end = e->dataoff + e->packed;
    z->win = win;
    z->wmask = wsize - 1;
    z->size = e->size;
    z->out = out;
    /* The window starts full of spaces: the old encoders could point at them. */
    for (i = 0; i < wsize; i++) win[i] = ' ';
}
/* the end of a member's stream: a read past its bytes by more than the look-ahead means it was cut short */
#define PASTMAX 4

/* ---- -lh4- to -lh7- ---- */
struct lh5 {
    struct lzin z;
    arc_u8 clen[NC], tlen[NT], plen[NP];
    arc_u16 ctab[1 << KC], csyms[NC];
    arc_u16 ttab[1 << KT], tsyms[NT];
    arc_u16 ptab[1 << KT], psyms[NP];
    struct hf hc, ht, hp;
};

static inline int hdec(struct lzin *z, const struct hf *h)
{
    unsigned v, e;
    int l, s;
    if (h->single >= 0) return h->single;
    v = peek16(z);
    e = h->tab[v >> (16 - h->kbits)];
    if (e) { drop(z, (int)(e >> 12)); return (int)(e & 0xFFF); }
    s = hf_long(h, v, &l);
    if (s < 0) return -1;
    drop(z, l);
    return s;
}

/* a table of lengths in the small form: 3 bits each, 7 and up continued in ones; `skip`: a 2-bit run of zeros after that
 * many (the first table only) */
static int rdsmall(struct lh5 *d, struct hf *h, arc_u8 *lens, int nmax, int nbits, int skip)
{
    struct lzin *z = &d->z;
    int n = (int)getn(z, nbits), i = 0, r;
    if (n == 0) {
        int c = (int)getn(z, nbits);
        if (c >= nmax) return ARC_ECORRUPT;
        for (i = 0; i < nmax; i++) lens[i] = 0;
        if ((r = hf_build(h, lens, nmax))) return r;
        h->single = c;
        return 0;
    }
    if (n > nmax) return ARC_ECORRUPT;
    while (i < n) {
        int c = (int)getn(z, 3);
        if (c == 7) {
            while (getn(z, 1)) {
                if (++c > 16) return ARC_ECORRUPT;
            }
        }
        lens[i++] = (arc_u8)c;
        if (i == skip) {
            int k = (int)getn(z, 2);
            while (k-- > 0 && i < nmax) lens[i++] = 0;
        }
    }
    while (i < nmax) lens[i++] = 0;
    return hf_build(h, lens, nmax);
}

static int rdc(struct lh5 *d)
{
    struct lzin *z = &d->z;
    int n = (int)getn(z, 9), i = 0, r;
    if (n == 0) {
        int c = (int)getn(z, 9);
        if (c >= NC) return ARC_ECORRUPT;
        for (i = 0; i < NC; i++) d->clen[i] = 0;
        if ((r = hf_build(&d->hc, d->clen, NC))) return r;
        d->hc.single = c;
        return 0;
    }
    if (n > NC) return ARC_ECORRUPT;
    while (i < n) {
        int c = hdec(z, &d->ht);
        if (c < 0 || z->past > PASTMAX) return ARC_ECORRUPT;
        if (c <= 2) {
            int run = c == 0 ? 1 : c == 1 ? (int)getn(z, 4) + 3 : (int)getn(z, 9) + 20;
            if (run > n - i) return ARC_ECORRUPT;
            while (run-- > 0) d->clen[i++] = 0;
        } else {
            d->clen[i++] = (arc_u8)(c - 2);
        }
    }
    while (i < NC) d->clen[i++] = 0;
    return hf_build(&d->hc, d->clen, NC);
}

static int lh5_run(struct lh5 *d, int np, int pbits, unsigned wsize)
{
    struct lzin *z = &d->z;
    unsigned long left = 0;
    d->hc.tab = d->ctab; d->hc.syms = d->csyms; d->hc.kbits = KC; d->hc.rev = 0;
    d->ht.tab = d->ttab; d->ht.syms = d->tsyms; d->ht.kbits = KT; d->ht.rev = 0;
    d->hp.tab = d->ptab; d->hp.syms = d->psyms; d->hp.kbits = KT; d->hp.rev = 0;
    while (z->total < z->size) {
        int c, r;
        if (!left) {
            left = getn(z, 16);
            if (!left || z->past > PASTMAX) return ARC_ECORRUPT;
            if ((r = rdsmall(d, &d->ht, d->tlen, NT, 5, 3))) return r;
            if ((r = rdc(d))) return r;
            if ((r = rdsmall(d, &d->hp, d->plen, np, pbits, -1))) return r;
        }
        left--;
        c = hdec(z, &d->hc);
        if (c < 0 || z->past > PASTMAX) return z->err ? z->err : ARC_ECORRUPT;
        if (c < 256) {
            if ((r = putbyte(z, (unsigned)c))) return r;
        } else {
            int p = hdec(z, &d->hp);
            unsigned dist;
            if (p < 0) return ARC_ECORRUPT;
            dist = p ? (1u << (p - 1)) + getn(z, p - 1) : 0;
            dist++;
            if (dist > wsize) return ARC_ECORRUPT;
            if ((r = copy(z, dist, (unsigned)c - 253))) return r;
        }
    }
    return 0;
}

/* ---- -lh1-: 314 codes (256 bytes, lengths 3-60) in an adaptive Huffman tree, the position's top 6 bits in a fixed code ---- */
#define N1 314
#define T1 (2 * N1 - 1)
#define ROOT (T1 - 1)
#define MAXF 0x8000
struct lh1 {
    struct lzin z;
    arc_u16 freq[T1 + 1], son[T1], up[T1 + N1];
    arc_u16 ftab[256], fsyms[64];
    arc_u8 flen[64];
    struct hf hf;
};

static void tree_init(struct lh1 *d)
{
    int i, j;
    for (i = 0; i < N1; i++) {
        d->freq[i] = 1;
        d->son[i] = (arc_u16)(i + T1);
        d->up[i + T1] = (arc_u16)i;
    }
    for (i = 0, j = N1; j <= ROOT; i += 2, j++) {
        d->freq[j] = (arc_u16)(d->freq[i] + d->freq[i + 1]);
        d->son[j] = (arc_u16)i;
        d->up[i] = d->up[i + 1] = (arc_u16)j;
    }
    d->freq[T1] = 0xFFFF;
    d->up[ROOT] = 0;
}

/* the counts have grown to the limit: halve those of the leaves and build the inner nodes again */
static void tree_rescale(struct lh1 *d)
{
    int i, j = 0, k, l;
    for (i = 0; i < T1; i++) {
        if (d->son[i] >= T1) {
            d->freq[j] = (arc_u16)((d->freq[i] + 1) / 2);
            d->son[j] = d->son[i];
            j++;
        }
    }
    for (i = 0, j = N1; j < T1; i += 2, j++) {
        unsigned f = (unsigned)d->freq[i] + d->freq[i + 1];
        for (k = j; f < d->freq[k - 1]; k--) ;
        for (l = j; l > k; l--) { d->freq[l] = d->freq[l - 1]; d->son[l] = d->son[l - 1]; }
        d->freq[k] = (arc_u16)f;
        d->son[k] = (arc_u16)i;
    }
    for (i = 0; i < T1; i++) {
        k = d->son[i];
        d->up[k] = (arc_u16)i;
        if (k < T1) d->up[k + 1] = (arc_u16)i;
    }
}

static void tree_update(struct lh1 *d, int c)
{
    int n;
    if (d->freq[ROOT] == MAXF) tree_rescale(d);
    n = d->up[c + T1];
    do {
        unsigned f = ++d->freq[n];
        int m = n + 1;
        if (f > d->freq[m]) {
            int a, b;
            while (f > d->freq[++m]) ;
            m--;
            d->freq[n] = d->freq[m];
            d->freq[m] = (arc_u16)f;
            a = d->son[n];
            d->up[a] = (arc_u16)m;
            if (a < T1) d->up[a + 1] = (arc_u16)m;
            b = d->son[m];
            d->son[m] = (arc_u16)a;
            d->up[b] = (arc_u16)n;
            if (b < T1) d->up[b + 1] = (arc_u16)n;
            d->son[n] = (arc_u16)b;
            n = m;
        }
    } while ((n = d->up[n]) != 0);
}

static int lh1_run(struct lh1 *d)
{
    static const arc_u8 counts[6] = { 1, 3, 8, 12, 24, 16 };    /* of lengths 3 to 8: the fixed code's 64 */
    struct lzin *z = &d->z;
    int i, k, n = 0;
    for (i = 0; i < 6; i++) for (k = 0; k < counts[i]; k++) d->flen[n++] = (arc_u8)(i + 3);
    d->hf.tab = d->ftab; d->hf.syms = d->fsyms; d->hf.kbits = 8; d->hf.rev = 0;
    if (hf_build(&d->hf, d->flen, 64)) return ARC_ECORRUPT;
    tree_init(d);
    while (z->total < z->size) {
        int c = d->son[ROOT], r;
        while (c < T1) c = d->son[c + (int)getn(z, 1)];
        c -= T1;
        tree_update(d, c);
        if (z->past > PASTMAX) return z->err ? z->err : ARC_ECORRUPT;
        if (c < 256) {
            if ((r = putbyte(z, (unsigned)c))) return r;
        } else {
            int top = hdec(z, &d->hf);
            unsigned dist;
            if (top < 0) return ARC_ECORRUPT;
            dist = (unsigned)top << 6 | getn(z, 6);
            if ((r = copy(z, dist + 1, (unsigned)c - 253))) return r;
        }
    }
    return 0;
}

/* ---- stored ---- */
static int lh0_run(struct lzin *z)
{
    while (z->total < z->size) {
        long n = INBUF, r;
        if ((unsigned long)n > z->size - z->total) n = (long)(z->size - z->total);
        r = src_read(z->src, z->off, z->buf, n);
        if (r < 0) return ARC_EREAD;
        if (r != n) return ARC_ECORRUPT;
        z->crc = arc_crc16(z->crc, z->buf, n);
        if (z->out && !z->out->write(z->out->ctx, z->buf, n)) return ARC_ESTOP;
        z->off += (unsigned long)n;
        z->total += (unsigned long)n;
    }
    return 0;
}

int lha_extract(const struct arcsrc *src, const struct arcent *e, const struct arcsink *out)
{
    int rc;
    arc_u8 *win = 0;
    struct lzin *z = 0;
    unsigned wsize = 0;
    unsigned crc = 0;
    if (e->method == AM_DIR) return 0;
    if (e->method == AM_REFUSED) return ARC_ECORRUPT;
    if (e->method == AM_OTHER) return ARC_EUNSUP;
    if (e->size == 0) return (e->aux1 & 0xFFFF) == 0 ? 0 : ARC_ECRC;    /* no window for nothing: 20000 empty members cost nothing */
    if (arc_crc16(0, "123456789", 9) != 0xBB3D) return ARC_ENOMEM;      /* no memory for its table: sums would all be 0 */
    switch (e->method) {
    case 0: {
        z = arc_alloc(sizeof *z);
        if (!z) return ARC_ENOMEM;
        z->src = src; z->off = e->dataoff; z->end = e->dataoff + e->packed; z->size = e->size; z->out = out;
        if (e->packed != e->size) { arc_free(z); return ARC_ECORRUPT; }
        rc = lh0_run(z);
        break; }
    case 1: {
        struct lh1 *d = arc_alloc(sizeof *d);
        if (!d) return ARC_ENOMEM;
        wsize = 4096;
        win = arc_alloc(wsize);
        if (!win) { arc_free(d); return ARC_ENOMEM; }
        z = &d->z;
        zinit(z, src, e, out, win, wsize);
        rc = lh1_run(d);
        if (!rc) rc = flushw(z);
        break; }
    case 4: case 5: case 6: case 7: {
        struct lh5 *d = arc_alloc(sizeof *d);
        int np = e->method == 7 ? 17 : e->method == 6 ? 16 : 14;
        if (!d) return ARC_ENOMEM;
        wsize = e->method == 4 ? 4096 : e->method == 5 ? 8192 : e->method == 6 ? 32768 : 65536;
        win = arc_alloc(wsize);
        if (!win) { arc_free(d); return ARC_ENOMEM; }
        z = &d->z;
        zinit(z, src, e, out, win, wsize);
        rc = lh5_run(d, np, e->method >= 6 ? 5 : 4, wsize);
        if (!rc) rc = flushw(z);
        break; }
    default:
        return ARC_EUNSUP;
    }
    if (!rc && z->err) rc = z->err;
    crc = z->crc;
    arc_free(z);                /* (the first member of lh1 and lh5: the same address) */
    if (win) arc_free(win);
    if (rc) return rc;
    return (crc & 0xFFFF) == (e->aux1 & 0xFFFF) ? 0 : ARC_ECRC;
}

/* ---- the list ---- */
static arc_u32 le32(const arc_u8 *p) { return p[0] | (arc_u32)p[1] << 8 | (arc_u32)p[2] << 16 | (arc_u32)p[3] << 24; }
static unsigned le16(const arc_u8 *p) { return p[0] | (unsigned)p[1] << 8; }

/* what a member's headers hold, in the heap (no large arrays on the stack) */
struct hdr {
    struct arcent e;
    arc_u8 b[1024];             /* the base header */
    arc_u8 x[512];              /* one extended header's start */
    arc_u8 dir[300], name[300];
    int dirn, namen, unixmode, islink, toolong;
    unsigned long unixt;
};

static void dos_date(arc_u32 v, struct arcent *e)
{
    int s = (int)(v & 31) * 2, mi = (int)((v >> 5) & 63), h = (int)((v >> 11) & 31);
    int d = (int)((v >> 16) & 31), mo = (int)((v >> 21) & 15), y = (int)((v >> 25) & 127) + 1980;
    if (d < 1 || mo < 1 || mo > 12 || h > 23 || mi > 59 || s > 59) return;
    arc_datestamp(y, mo, d, h, mi, s, &e->days, &e->mins, &e->ticks);
}
static void unix_date(arc_u32 t, struct arcent *e)
{
    if (t < 252460800u) return;             /* before 1978: AmigaDOS's dates do not reach it */
    t -= 252460800u;
    e->days = (long)(t / 86400);
    e->mins = (long)(t % 86400 / 60);
    e->ticks = (long)(t % 60 * 50);
}

/* the method's name to the number arcent.method holds */
static int method(const arc_u8 *m, struct arcent *e)
{
    e->aux3 = (unsigned long)m[1] << 16 | (unsigned long)m[2] << 8 | m[3];     /* for a list that names it */
    if (m[0] != '-' || m[4] != '-') return -1;
    if (m[1] == 'l' && m[2] == 'h') {
        if (m[3] == '0') return 0;
        if (m[3] == '1') return 1;
        if (m[3] >= '4' && m[3] <= '7') return m[3] - '0';
        if (m[3] == 'd') return AM_DIR;
    }
    if (m[1] == 'l' && m[2] == 'z' && m[3] == '4') return 0;
    return AM_OTHER;
}

/* one extended header: type, data[n] */
static void ext_use(struct hdr *h, unsigned type, const arc_u8 *d, int n)
{
    int i;
    switch (type) {
    case 1:
        if (n > (int)sizeof h->name) { n = (int)sizeof h->name; h->toolong = 1; }
        for (i = 0; i < n; i++) h->name[i] = d[i];
        h->namen = n;
        break;
    case 2:
        if (n > (int)sizeof h->dir) { n = (int)sizeof h->dir; h->toolong = 1; }
        for (i = 0; i < n; i++) h->dir[i] = d[i] == 0xFF ? '/' : d[i];
        h->dirn = n;
        break;
    case 0x3F: case 0x71: {                     /* a comment */
        int k;
        for (k = 0; k < n && k + 1 < (int)sizeof h->e.comment; k++) {
            unsigned c = d[k];
            h->e.comment[k] = c < 0x20 || c >= 0x7F ? '_' : (char)c;
        }
        h->e.comment[k] = 0;
        break; }
    case 0x50:
        if (n >= 2) h->unixmode = (int)le16(d) | 0x10000;
        break;
    case 0x54:
        if (n >= 4) h->unixt = le32(d);
        break;
    default:
        break;
    }
}

/* the chain of extended headers at off; the first's size is next. Returns the bytes of the chain or an error */
static long ext_chain(const struct arcsrc *src, struct hdr *h, unsigned long off, unsigned next, unsigned long limit)
{
    long total = 0;
    int count = 0;
    while (next) {
        long want, r;
        unsigned nx;
        if (next < 3 || ++count > MAXEXT) return ARC_ECORRUPT;
        if ((unsigned long)next > limit - (unsigned long)total) return ARC_ECORRUPT;
        want = next > sizeof h->x ? (long)sizeof h->x : (long)next;
        r = src_read(src, off + (unsigned long)total, h->x, want);
        if (r < 0) return ARC_EREAD;
        if (r != want) return ARC_ECORRUPT;
        if (next > sizeof h->x && (h->x[0] == 1 || h->x[0] == 2)) h->toolong = 1;     /* a name past what was read */
        ext_use(h, h->x[0], h->x + 1, (int)want - 3 > 0 ? (int)want - 3 : 0);
        if (next > sizeof h->x) {
            r = src_read(src, off + (unsigned long)total + next - 2, h->x, 2);
            if (r < 0) return ARC_EREAD;
            if (r != 2) return ARC_ECORRUPT;
            nx = le16(h->x);
        } else nx = le16(h->x + next - 2);
        total += (long)next;
        next = nx;
    }
    return total;
}

/* level 0 and 1 name field: path with '\' or '/', then a 0 and a filenote (the Amiga's LhA) */
static void base_name(struct hdr *h, const arc_u8 *p, int n)
{
    int i, k;
    for (i = 0; i < n && p[i]; i++) h->name[i] = p[i] == '\\' || p[i] == 0xFF ? '/' : p[i];
    h->namen = i;
    if (i < n) {
        i++;
        for (k = 0; i < n && k + 1 < (int)sizeof h->e.comment; i++, k++) {
            unsigned c = p[i];
            h->e.comment[k] = c < 0x20 || c >= 0x7F ? '_' : (char)c;
        }
        h->e.comment[k] = 0;
    }
}

/* the member at p in h. Returns its whole length, 0 at the end of the archive, < 0 an error */
static long rdmember(const struct arcsrc *src, unsigned long p, struct hdr *h)
{
    arc_u8 *b = h->b;
    long r;
    unsigned long hlen, data, packed;
    int level;
    {
        char *c = (char *)h;
        unsigned long k;
        for (k = 0; k < sizeof h->e; k++) c[k] = 0;
    }
    h->dirn = h->namen = h->unixmode = h->islink = h->toolong = 0;
    h->unixt = 0;
    r = src_read(src, p, b, sizeof h->b);
    if (r < 0) return ARC_EREAD;
    if (r == 0) return p ? 0 : ARC_ECORRUPT;    /* an empty file is no archive */
    if (b[0] == 0 && !(r >= 22 && b[2] == '-' && b[6] == '-' && b[20] == 2)) {
        /* The end mark. What follows may be anything (real archives carry a BBS's junk after it, and the 0x1A of old transfer
         * protocols), but a member right there means a byte was flipped, and that must not hide the rest and call it success. So
         * too a file of only zeros: no archive. */
        if (p == 0 && src->size > 1) return ARC_ECORRUPT;
        if (r >= 7 && b[2] == '-' && b[6] == '-') return ARC_ECORRUPT;      /* its own size byte zeroed */
        if (r >= 8 && b[3] == '-' && b[7] == '-') return ARC_ECORRUPT;      /* a member after the mark */
        return 0;
    }
    if (r < 22) return ARC_ECORRUPT;
    level = b[20];
    h->e.method = method(b + 2, &h->e);
    if (h->e.method < 0) return ARC_ECORRUPT;
    packed = le32(b + 7);
    h->e.size = le32(b + 11);
    if (level == 0 || level == 1) {
        int n = b[21], i;
        unsigned sum = 0;
        hlen = 2ul + b[0];
        if (hlen < 24 || (long)hlen > r || 22 + n > (long)hlen) return ARC_ECORRUPT;
        for (i = 2; i < (int)hlen; i++) sum += b[i];
        if ((sum & 0xFF) != b[1]) return ARC_ECORRUPT;
        dos_date(le32(b + 15), &h->e);
        base_name(h, b + 22, n);
        h->e.aux1 = 22 + n + 2 <= (long)hlen ? le16(b + 22 + n) : 0;
        /* The attribute byte is AmigaDOS's protection bits when the Amiga wrote it: the OS byte says 'A', or there is none
         * (level 0 from the Amiga's LhA). From anywhere else it means something else, and the default stands. */
        if (22 + n + 2 >= (long)hlen || b[22 + n + 2] == 'A') h->e.prot = b[19];
        data = p + hlen;
        if (level == 1) {
            long x;
            unsigned next;
            if (hlen < (unsigned long)22 + n + 5) return ARC_ECORRUPT;
            next = le16(b + hlen - 2);
            x = ext_chain(src, h, data, next, packed);
            if (x < 0) return x;
            data += (unsigned long)x;
            packed -= (unsigned long)x;
        }
    } else if (level == 2) {
        long x;
        hlen = le16(b);
        if (hlen < 26 || r < 26) return ARC_ECORRUPT;
        unix_date(le32(b + 15), &h->e);
        h->e.aux1 = le16(b + 21);
        if (b[23] == 'A') h->e.prot = b[19];
        x = ext_chain(src, h, p + 26, le16(b + 24), hlen - 24 >= 2 ? hlen - 26 : 0);
        if (x < 0) return x;
        data = p + hlen;
    } else {
        return ARC_EUNSUP;                      /* the later levels: the note says why */
    }
    if (h->unixt) unix_date((arc_u32)h->unixt, &h->e);
    if (data > src->size || packed > src->size - data) return ARC_ECORRUPT;
    h->e.dataoff = data;
    h->e.packed = packed;
    return (long)(data + packed - p);
}

/* the path out of the fields, the name checked. A link on Unix is `name|target` in a -lhd- member. */
static int finish(struct hdr *h)
{
    int i, n = 0, bad, isdir = h->e.method == AM_DIR;
    arc_u8 *t = h->x;
    if (h->dirn + h->namen + 1 > (int)sizeof h->x - 1) h->toolong = 1;      /* (a cut name must not land as another file) */
    for (i = 0; i < h->dirn && n < (int)sizeof h->x - 1; i++) t[n++] = h->dir[i] == '\\' ? '/' : h->dir[i];
    if (n && t[n - 1] != '/' && h->namen) t[n++] = '/';
    for (i = 0; i < h->namen && n < (int)sizeof h->x - 1; i++) t[n++] = h->name[i];
    if (n && t[n - 1] == '/' && h->e.size == 0) { isdir = 1; h->e.method = AM_DIR; }
    if (isdir) {
        h->e.isdir = 1;
        for (i = 0; i < n; i++) if (t[i] == '|') h->islink = 1;
    }
    if ((h->unixmode & 0170000) == 0120000) h->islink = 1;
    bad = arc_fixname(t, n, h->e.name, sizeof h->e.name);
    h->e.aux2 = (unsigned long)h->unixmode;
    if (bad && isdir && !h->e.name[0] && !h->islink && !h->toolong && (n == 0 || t[0] != '/')) return -1;   /* "./": nothing to do */
    return bad || h->islink || h->toolong;
}

int lha_list(const struct arcsrc *src, arc_addfn add, void *ctx)
{
    unsigned long p = 0;
    int rc = 0;
    struct hdr *h = arc_alloc(sizeof *h);
    if (!h) return ARC_ENOMEM;
    for (;;) {
        long n = rdmember(src, p, h);
        if (n == 0) break;
        if (n < 0) { rc = (int)n; break; }
        {
            int f = finish(h);
            p += (unsigned long)n;
            if (f < 0) continue;
            if (f) {
                h->e.method = AM_REFUSED;
                if (!arc_list_refused) { rc = ARC_ECORRUPT; break; }
            }
        }
        if (!add(ctx, &h->e)) break;
    }
    arc_free(h);
    return rc;
}
