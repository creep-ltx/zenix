/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* dms - see dms.h. A port of xDMS 1.3 (public domain) through BBX's Go
 * reader; the decoders keep xDMS's names where they help reading the
 * two side by side. Every array index is masked or checked, every loop
 * makes output or ends: bits read past a track's end come back as
 * zeros, and more than 8 such bytes marks the track bad. */
#include <string.h>
#include "dms.h"

#define HEADLEN 56
#define THLEN 20
#define MAXTRACK 32000          /* xDMS's buffers: no track is longer */
#define SLACK 512               /* a match may run up to 255 bytes past the end */

/* DEEP: LZHUF (Yoshizaki), adaptive Huffman, 16K window */
#define LAHEAD 60
#define THRESH 2
#define NCHAR (256 - THRESH + LAHEAD)   /* 314 symbols */
#define TSIZE (NCHAR * 2 - 1)           /* 627 tree nodes */
#define ROOT (TSIZE - 1)
#define MAXFREQ 0x8000
/* HEAVY: LZH as -lh5-, 4K (HEAVY1) or 8K (HEAVY2) window */
#define NC 510
#define NPT 20

/* ---- the bit reader (xDMS's GETBITS/DROPBITS) ------------------
 * The next bit is bit 31 of buf; cnt bits are valid, 16..31 after
 * every drop, so GET(n) for n <= 16 is a swap and one shift on the
 * 68000. The bytes come in pairs: an odd-length input gets a zero
 * byte after it (bitsinit), its buffer has room for that. */

struct bits {
    const unsigned char *p, *end;       /* end: even from the start */
    unsigned long buf;
    int cnt;
    int over;                   /* zero bytes fed past the end */
};

#define GET(b, n) ((unsigned)((b)->buf >> 16) >> (16 - (n)))

static void refill(struct bits *b)
{
    unsigned w = 0;
    if (b->p < b->end) {
        w = (unsigned)b->p[0] << 8 | b->p[1];
        b->p += 2;
    } else b->over += 2;
    b->buf |= (unsigned long)w << (16 - b->cnt);
    b->cnt += 16;
}

static void drop(struct bits *b, int n)
{
    if (n > 16) {               /* only from a corrupt code length */
        b->over = 99;
        n = 16;
    }
    b->buf = b->buf << n & 0xFFFFFFFFUL;      /* long may be 64 bits on the host */
    if ((b->cnt -= n) < 16) refill(b);
}

static void bitsinit(struct bits *b, unsigned char *in, long len)
{
    if (len & 1) in[len++] = 0;
    b->p = in;
    b->end = in + len;
    b->buf = 0;
    b->cnt = 0;
    b->over = 0;
    refill(b);
}

/* The decoders' loops keep the reader in locals (the 68000 has the
 * registers; through the struct -Os goes to memory for every bit):
 * LGET/LDROP do what GET/drop do, for n <= 16, the refill inline. */
#define BLOCALS unsigned long bb; int bc, over; const unsigned char *ip, *iend
#define BLOAD(b) (bb = (b)->buf, bc = (b)->cnt, ip = (b)->p, iend = (b)->end, over = (b)->over)
#define BSAVE(b) ((b)->buf = bb, (b)->cnt = bc, (b)->p = ip, (b)->over = over)
#define LGET(n) ((unsigned)(bb >> 16) >> (16 - (n)))
#define LDROP(n) do { bb = bb << (n) & 0xFFFFFFFFUL; if ((bc -= (n)) < 16) { \
        if (ip < iend) { bb |= (unsigned long)((unsigned)ip[0] << 8 | ip[1]) << (16 - bc); ip += 2; } \
        else over += 2; \
        bc += 16; } } while (0)

/* ---- the decoders' state: shared by all modes, carried across tracks */

struct dstate {
    unsigned char text[0x4000];         /* the one window every mode uses */
    unsigned short qloc, mloc, hloc, dloc;
    short initdeep;
    unsigned short lastlen, np;
    unsigned char dcode[256], dlen[256];        /* LZHUF's position tables */
    unsigned short freq[TSIZE + 1], prnt[TSIZE + NCHAR], son[TSIZE];
    unsigned short left[2 * NC - 1], right[2 * NC - 1 + 9];
    unsigned char clen[NC], ptlen[NPT];
    unsigned short ctable[4096], pttable[256];
    struct bits b;
};

/* xDMS's Init_Decrunchers: after a track whose flags bit 0 is clear.
 * The HEAVY tables are kept. */
static void reinit(struct dstate *d)
{
    d->qloc = 251;
    d->mloc = 0x3FBE;
    d->hloc = 0;
    d->dloc = 0x3FC4;
    d->initdeep = 1;
    memset(d->text, 0, 0x3FC8);
}

static struct dstate *newstate(void)
{
    struct dstate *d = arc_alloc(sizeof *d);
    int c;
    if (!d) return 0;
    /* d_code/d_len: runs of 32x0, 16x(1..3), 8x(4..11), 4x(12..23),
     * 2x(24..47), 1x(48..63); lengths 32x3 48x4 64x5 48x6 48x7 16x8 */
    for (c = 0; c < 256; c++) {
        if (c < 32) d->dcode[c] = 0, d->dlen[c] = 3;
        else if (c < 80) d->dcode[c] = 1 + (c - 32) / 16, d->dlen[c] = 4;
        else if (c < 144) d->dcode[c] = 4 + (c - 80) / 8, d->dlen[c] = 5;
        else if (c < 192) d->dcode[c] = 12 + (c - 144) / 4, d->dlen[c] = 6;
        else if (c < 240) d->dcode[c] = 24 + (c - 192) / 2, d->dlen[c] = 7;
        else d->dcode[c] = 48 + (c - 240), d->dlen[c] = 8;
    }
    reinit(d);
    return d;
}

/* ---- SIMPLE: run lengths, also the second stage of the others ---
 * 0x90 0 = a literal 0x90; 0x90 n c = n times c; 0x90 0xFF c hi lo =
 * (hi<<8|lo) times c. */

static int rle(const unsigned char *ip, long inlen, unsigned char *op, long size)
{
    const unsigned char *iend = ip + inlen;
    unsigned char *oend = op + size;
    unsigned a, b;
    long n;
    while (op < oend) {
        if (ip >= iend) return ARC_ECORRUPT;
        if ((a = *ip++) != 0x90) {
            *op++ = a;
            continue;
        }
        if (ip >= iend) return ARC_ECORRUPT;
        if (!(b = *ip++)) {
            *op++ = a;
            continue;
        }
        if (ip >= iend) return ARC_ECORRUPT;
        a = *ip++;
        if (b == 0xFF) {
            if (iend - ip < 2) return ARC_ECORRUPT;
            n = (long)ip[0] << 8 | ip[1];
            ip += 2;
        } else n = b;
        if (n > oend - op) return ARC_ECORRUPT;
        memset(op, (int)a, n);
        op += n;
    }
    return ARC_OK;
}

/* ---- QUICK: LZ77, 256-byte window --------------------------------
 * Each stage-1 decoder returns the bytes it wrote (a match may end up
 * to SLACK past size; those bytes are in the window too, as in xDMS),
 * or -1 for data that cannot be right. */

static long quick(struct dstate *d, unsigned char *out, long size)
{
    unsigned char *t = d->text, *op = out, *oend = out + size;
    unsigned loc = d->qloc, i, j, c;
    BLOCALS;
    BLOAD(&d->b);
    while (op < oend) {
        if (LGET(1)) {
            LDROP(1);
            c = LGET(8);
            LDROP(8);
            *op++ = t[loc++ & 0xFF] = c;
        } else {
            LDROP(1);
            j = LGET(2) + 2;
            LDROP(2);
            i = loc - LGET(8) - 1;
            LDROP(8);
            do *op++ = t[loc++ & 0xFF] = t[i++ & 0xFF]; while (--j);
        }
    }
    BSAVE(&d->b);
    d->qloc = (loc + 5) & 0xFF;
    return over > 8 ? -1 : op - out;
}

/* ---- MEDIUM: LZ77, 16K window, positions coded as in LZHUF ------ */

static long medium(struct dstate *d, unsigned char *out, long size)
{
    unsigned char *t = d->text, *op = out, *oend = out + size;
    const unsigned char *dcode = d->dcode, *dlen = d->dlen;
    unsigned loc = d->mloc, i, j, c, u;
    BLOCALS;
    BLOAD(&d->b);
    while (op < oend) {
        if (LGET(1)) {
            LDROP(1);
            c = LGET(8);
            LDROP(8);
            *op++ = t[loc++ & 0x3FFF] = c;
        } else {
            LDROP(1);
            c = LGET(8);
            LDROP(8);
            j = dcode[c] + 3;
            u = dlen[c];
            c = ((c << u) | LGET(u)) & 0xFF;
            LDROP(u);
            u = dlen[c];
            c = (unsigned)dcode[c] << 8 | (((c << u) | LGET(u)) & 0xFF);
            LDROP(u);
            i = loc - c - 1;
            do *op++ = t[loc++ & 0x3FFF] = t[i++ & 0x3FFF]; while (--j);
        }
    }
    BSAVE(&d->b);
    d->mloc = (loc + 66) & 0x3FFF;
    return over > 8 ? -1 : op - out;
}

/* ---- DEEP: LZHUF with adaptive Huffman codes, 16K window -------- */

static void deeptabs(struct dstate *d)
{
    unsigned i, j;
    for (i = 0; i < NCHAR; i++) {
        d->freq[i] = 1;
        d->son[i] = i + TSIZE;
        d->prnt[i + TSIZE] = i;
    }
    for (i = 0, j = NCHAR; j <= ROOT; i += 2, j++) {
        d->freq[j] = d->freq[i] + d->freq[i + 1];
        d->son[j] = i;
        d->prnt[i] = d->prnt[i + 1] = j;
    }
    d->freq[TSIZE] = 0xFFFF;
    d->prnt[ROOT] = 0;
    d->initdeep = 0;
}

/* the tree rebuilt with halved counts, when the root's count is full */
static void reconst(struct dstate *d)
{
    unsigned i, j, k, f;
    for (i = j = 0; i < TSIZE; i++)
        if (d->son[i] >= TSIZE) {
            d->freq[j] = (d->freq[i] + 1) / 2;
            d->son[j] = d->son[i];
            j++;
        }
    for (i = 0, j = NCHAR; j < TSIZE; i += 2, j++) {
        f = d->freq[j] = d->freq[i] + d->freq[i + 1];
        for (k = j - 1; f < d->freq[k]; k--) ;
        k++;
        memmove(&d->freq[k + 1], &d->freq[k], (j - k) * sizeof d->freq[0]);
        d->freq[k] = f;
        memmove(&d->son[k + 1], &d->son[k], (j - k) * sizeof d->son[0]);
        d->son[k] = i;
    }
    for (i = 0; i < TSIZE; i++) {
        k = d->son[i];
        if (k >= TSIZE) d->prnt[k] = i;
        else d->prnt[k] = d->prnt[k + 1] = i;
    }
}

/* one more of symbol c: move it up the tree past nodes it now outweighs */
static void update(struct dstate *d, unsigned c)
{
    unsigned i, j, k, l;
    if (d->freq[ROOT] == MAXFREQ) reconst(d);
    c = d->prnt[c + TSIZE];
    do {
        k = ++d->freq[c];
        if (k > d->freq[l = c + 1]) {
            while (k > d->freq[++l]) ;  /* freq[TSIZE] = 0xFFFF stops it */
            l--;
            d->freq[c] = d->freq[l];
            d->freq[l] = k;
            i = d->son[c];
            d->prnt[i] = l;
            if (i < TSIZE) d->prnt[i + 1] = l;
            j = d->son[l];
            d->son[l] = i;
            d->prnt[j] = c;
            if (j < TSIZE) d->prnt[j + 1] = c;
            d->son[c] = j;
            c = l;
        }
    } while ((c = d->prnt[c]) != 0);
}

static long deep(struct dstate *d, unsigned char *out, long size)
{
    unsigned char *t = d->text, *op = out, *oend = out + size;
    const unsigned short *son = d->son;
    unsigned loc = d->dloc, i, j, c, u;
    BLOCALS;
    if (d->initdeep) deeptabs(d);
    BLOAD(&d->b);
    while (op < oend) {
        /* DecodeChar: root to leaf, a bit per level */
        c = son[ROOT];
        while (c < TSIZE) {
            c = son[c + LGET(1)];
            LDROP(1);
        }
        c -= TSIZE;
        update(d, c);
        if (c < 256) {
            *op++ = t[loc++ & 0x3FFF] = c;
            continue;
        }
        j = c - 255 + THRESH;
        /* DecodePosition */
        i = LGET(8);
        LDROP(8);
        c = (unsigned)d->dcode[i] << 8;
        u = d->dlen[i];
        i = ((i << u) | LGET(u)) & 0xFF;
        LDROP(u);
        i = loc - (c | i) - 1;
        do *op++ = t[loc++ & 0x3FFF] = t[i++ & 0x3FFF]; while (--j);
    }
    BSAVE(&d->b);
    d->dloc = (loc + 60) & 0x3FFF;
    return over > 8 ? -1 : op - out;
}

/* ---- HEAVY: LZH (UNIX LHA's, by Masaru Oki) ----------------------
 * make_table: a lookup table for codes up to tablebits long, a tree in
 * left/right for the longer ones. Recursion is at most 33 deep. */

struct mk {
    struct dstate *d;
    const unsigned char *blen;
    unsigned short *tbl;
    int c;
    unsigned n, tblsiz, len, depth, maxdepth, avail, codeword, bit;
    int err;
};

static unsigned mktbl(struct mk *t)
{
    unsigned i = 0, l, r;
    if (t->err) return 0;
    if (t->len == t->depth) {
        while (++t->c < (int)t->n)
            if (t->blen[t->c] == t->len) {
                i = t->codeword;
                t->codeword += t->bit;
                if (t->codeword > t->tblsiz) {
                    t->err = 1;
                    return 0;
                }
                while (i < t->codeword) t->tbl[i++] = t->c;
                return t->c;
            }
        t->c = -1;
        t->len++;
        t->bit >>= 1;
    }
    t->depth++;
    if (t->depth < t->maxdepth) {
        mktbl(t);
        mktbl(t);
    } else if (t->depth > 32) {
        t->err = 1;
        return 0;
    } else {
        if ((i = t->avail++) >= 2 * t->n - 1) {
            t->err = 1;
            return 0;
        }
        l = mktbl(t);
        r = mktbl(t);
        t->d->left[i] = l;
        t->d->right[i] = r;
        if (t->codeword >= t->tblsiz) {
            t->err = 1;
            return 0;
        }
        if (t->depth == t->maxdepth) t->tbl[t->codeword++] = i;
    }
    t->depth--;
    return i;
}

static int maketable(struct dstate *d, unsigned nchar, const unsigned char *bitlen,
                     unsigned tablebits, unsigned short *table)
{
    struct mk t;
    t.d = d;
    t.n = t.avail = nchar;
    t.blen = bitlen;
    t.tbl = table;
    t.tblsiz = 1U << tablebits;
    t.bit = t.tblsiz / 2;
    t.maxdepth = tablebits + 1;
    t.depth = t.len = 1;
    t.c = -1;
    t.codeword = 0;
    t.err = 0;
    mktbl(&t);
    mktbl(&t);
    return !t.err && t.codeword == t.tblsiz;
}

static int treec(struct dstate *d)
{
    struct bits *b = &d->b;
    unsigned i, n = GET(b, 9) & 0x1FF;
    drop(b, 9);
    if (n > 0) {
        if (n > NC) return 0;
        for (i = 0; i < n; i++) {
            d->clen[i] = GET(b, 5) & 0x1F;
            drop(b, 5);
        }
        memset(d->clen + n, 0, NC - n);
        return maketable(d, NC, d->clen, 12, d->ctable);
    }
    n = GET(b, 9) & 0x1FF;
    drop(b, 9);
    memset(d->clen, 0, NC);
    for (i = 0; i < 4096; i++) d->ctable[i] = n;
    return 1;
}

static int treep(struct dstate *d)
{
    struct bits *b = &d->b;
    unsigned i, n = GET(b, 5) & 0x1F;
    drop(b, 5);
    if (n > 0) {
        if (n > d->np) return 0;
        for (i = 0; i < n; i++) {
            d->ptlen[i] = GET(b, 4) & 0xF;
            drop(b, 4);
        }
        memset(d->ptlen + n, 0, d->np - n);
        return maketable(d, d->np, d->ptlen, 8, d->pttable);
    }
    n = GET(b, 5) & 0x1F;
    drop(b, 5);
    memset(d->ptlen, 0, d->np);
    for (i = 0; i < 256; i++) d->pttable[i] = n;
    return 1;
}

/* a code longer than the table: walk the tree with the next 16 bits */
static unsigned walk(struct dstate *d, unsigned j, unsigned lim, unsigned bits)
{
    unsigned m = 0x8000;
    do {
        if (!m || j >= 2 * NC - 1) return 0xFFFF;
        j = bits & m ? d->right[j] : d->left[j];
        m >>= 1;
    } while (j >= lim);
    return j;
}

/* The hot loop of nearly every real DMS file, written for the 68000:
 * the bit reader in locals, the tables reached through pointers. Codes
 * from the tables are at most 12 (c) or 15 (p) bits, from the tree at
 * most 16 more: no drop is over 16. flags bit 3: HEAVY2's 8K window;
 * bit 1: new code tables come first, else the last track's are used. */
static long heavy(struct dstate *d, unsigned char *out, long size, int flags)
{
    struct bits *b = &d->b;
    unsigned char *t = d->text, *op = out, *oend = out + size;
    const unsigned short *ctab = d->ctable, *ptab = d->pttable;
    const unsigned char *clen = d->clen, *ptlen = d->ptlen;
    unsigned loc = d->hloc, mask, np, i, j, c, n;
    BLOCALS;
    if (flags & 8) {
        np = 15;
        mask = 0x1FFF;
    } else {
        np = 14;
        mask = 0x0FFF;
    }
    d->np = np;
    if (flags & 2)
        if (!treec(d) || !treep(d)) return -1;
    BLOAD(b);
    while (op < oend) {
        /* decode_c: a literal, or a match length + 253 */
        c = ctab[LGET(12)];
        if (c < NC) LDROP(clen[c]);
        else {
            LDROP(12);
            c = walk(d, c, NC, LGET(16));
            if (c == 0xFFFF || clen[c] < 12 || clen[c] > 28) return -1;
            LDROP(clen[c] - 12);
        }
        if (c < 256) {
            t[loc++ & mask] = c;
            *op++ = c;
            continue;
        }
        j = c - 253;            /* 3..256 */
        /* decode_p: the distance - 1; code np-1 = the last one again */
        c = ptab[LGET(8)];
        if (c < np) LDROP(ptlen[c]);
        else {
            LDROP(8);
            c = walk(d, c, np, LGET(16));
            if (c == 0xFFFF || ptlen[c] < 8) return -1;
            LDROP(ptlen[c] - 8);
        }
        if (c != np - 1) {
            if (c > 0) {
                n = c - 1;
                c = LGET(n) | 1U << n;
                LDROP(n);
            }
            d->lastlen = c;
        }
        i = (loc - d->lastlen - 1) & mask;
        c = loc & mask;
        loc += j;
        if (i + j <= mask + 1 && c + j <= mask + 1) {
            /* neither end wraps: plain pointers (forward, byte by
             * byte, so an overlapping match repeats as it should) */
            const unsigned char *sp = t + i;
            unsigned char *dp = t + c;
            do *dp++ = *op++ = *sp++; while (--j);
        } else
            do {
                *op++ = t[c++ & mask] = t[i++ & mask];
            } while (--j);
    }
    BSAVE(b);
    d->hloc = loc;
    return over > 8 ? -1 : op - out;
}

/* ---- the archive walk ------------------------------------------ */

struct dms {
    struct arcsrc *s;
    unsigned long pos;          /* file offset of the record in h */
    unsigned char h[THLEN];     /* the next record, hn bytes of it */
    int hn;
    unsigned char *a, *b;       /* track buffers: a gets the packed bytes */
    long acap, bcap;
    struct dstate *d;           /* made when a packed track needs it */
    /* the current record */
    unsigned num, pk1, pk2, unpk, usum, dcrc;
    int flags, mode;
};

static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }

static int need(unsigned char **p, long *cap, long n)
{
    if (n <= *cap) return 1;
    if (*p) arc_free(*p);
    n = (n + 1023) & ~1023L;
    *cap = (*p = arc_alloc(n)) ? n : 0;
    return *p != 0;
}

static void done(struct dms *z)
{
    if (z->a) arc_free(z->a);
    if (z->b) arc_free(z->b);
    if (z->d) arc_free(z->d);
}

/* the header, and the first record behind it in the same read */
static int start(struct dms *z, struct arcsrc *s, struct dmsinfo *i)
{
    unsigned char hb[HEADLEN + THLEN];
    static const unsigned char one = 1;
    long got;
    memset(z, 0, sizeof *z);
    z->s = s;
    if (arc_crc16(0, &one, 1) == 0) return ARC_ENOMEM;  /* no table */
    if ((got = s->read(s->ctx, 0, hb, sizeof hb)) < 0) return ARC_EREAD;
    if (got < HEADLEN || memcmp(hb, "DMS!", 4)) return ARC_ECORRUPT;
    if (be16(hb + HEADLEN - 2) != arc_crc16(0, hb + 4, HEADLEN - 6)) return ARC_ECRC;
    i->geninfo = be16(hb + 10);
    i->date = (unsigned long)be16(hb + 12) << 16 | be16(hb + 14);
    i->lowtrack = be16(hb + 16);
    i->hightrack = be16(hb + 18);
    i->packed = (unsigned long)hb[21] << 16 | be16(hb + 22);
    i->unpacked = (unsigned long)hb[25] << 16 | be16(hb + 26);
    i->version = be16(hb + 46);
    i->disktype = be16(hb + 50);
    i->mode = be16(hb + 52);
    i->diskhd = (i->geninfo & DMS_HD) != 0;
    i->encrypted = (i->geninfo & DMS_ENCRYPTED) != 0;
    i->fms = i->disktype == 7;
    z->pos = HEADLEN;
    z->hn = got - HEADLEN;
    memcpy(z->h, hb + HEADLEN, z->hn);
    return ARC_OK;
}

/* The next track record and, when data, its packed bytes in a (the
 * record after it read in the same go). 1 = a track, 0 = the end (no
 * more "TR"; xDMS too ignores what some files carry after the last
 * track), or an error. */
static int next(struct dms *z, int data)
{
    unsigned char *h = z->h;
    long got, n;
    if (z->hn < THLEN) {
        if (z->hn > 0 && h[0] == 'T' && (z->hn < 2 || h[1] == 'R')) return ARC_ECORRUPT;
        return 0;
    }
    if (h[0] != 'T' || h[1] != 'R') return 0;
    if (be16(h + THLEN - 2) != arc_crc16(0, h, THLEN - 2)) return ARC_ECRC;
    z->num = be16(h + 2);
    z->pk1 = be16(h + 6);
    z->pk2 = be16(h + 8);
    z->unpk = be16(h + 10);
    z->flags = h[12];
    z->mode = h[13];
    z->usum = be16(h + 14);
    z->dcrc = be16(h + 16);
    if (z->pk1 > MAXTRACK || z->pk2 > MAXTRACK || z->unpk > MAXTRACK) return ARC_ECORRUPT;
    n = (long)z->pk1 + THLEN;
    if (!data) {
        /* only the record after it: the caller skips this track */
        got = z->s->read(z->s->ctx, z->pos + n, h, THLEN);
        if (got < 0) return ARC_EREAD;
        if (z->pos + n > z->s->size) return ARC_ECORRUPT;
        z->pos += n;
        z->hn = got;
        return 1;
    }
    /* a also takes the RLE stage's output */
    if (!need(&z->a, &z->acap, n > (long)z->unpk ? n : (long)z->unpk)) return ARC_ENOMEM;
    if ((got = z->s->read(z->s->ctx, z->pos + THLEN, z->a, n)) < 0) return ARC_EREAD;
    if (got < (long)z->pk1) return ARC_ECORRUPT;       /* cut short */
    z->pos += n;
    z->hn = got - z->pk1;
    memcpy(h, z->a + z->pk1, z->hn);
    if (arc_crc16(0, z->a, z->pk1) != z->dcrc) return ARC_ECRC;
    return 1;
}

/* Unpacks the current track (xDMS's Unpack_Track): *res points at its
 * unpk bytes, in a or b. */
static int unpack(struct dms *z, unsigned char **res)
{
    struct dstate *d = z->d;
    long unpk = z->unpk, o;
    int mode = z->mode, flags = z->flags, r;
    if (mode > DMS_HEAVY2) return ARC_EUNSUP;
    if (mode == DMS_NOCOMP) {
        if (z->pk1 < z->unpk) return ARC_ECORRUPT;
        *res = z->a;
    } else if (mode == DMS_SIMPLE) {
        if (!need(&z->b, &z->bcap, unpk)) return ARC_ENOMEM;
        if ((r = rle(z->a, z->pk1, z->b, unpk))) return r;
        *res = z->b;
    } else {
        /* the first stage: a -> b; then RLE b -> a, as xDMS does */
        o = (long)z->pk2 > unpk ? (long)z->pk2 : unpk;
        if (!need(&z->b, &z->bcap, o + SLACK)) return ARC_ENOMEM;
        if (!d && !(d = z->d = newstate())) return ARC_ENOMEM;
        bitsinit(&d->b, z->a, z->pk1);
        if (mode == DMS_QUICK) o = quick(d, z->b, z->pk2);
        else if (mode == DMS_MEDIUM) o = medium(d, z->b, z->pk2);
        else if (mode == DMS_DEEP) o = deep(d, z->b, z->pk2);
        else o = heavy(d, z->b, z->pk2, mode == DMS_HEAVY1 ? flags & 7 : flags | 8);
        if (o < 0) return ARC_ECORRUPT;
        if (mode < DMS_HEAVY1 || flags & 4) {
            if ((r = rle(z->b, o, z->a, unpk))) return r;
            *res = z->a;
        } else {
            if (o < unpk) return ARC_ECORRUPT;
            *res = z->b;
        }
    }
    if (!(flags & 1) && d) reinit(d);
    return ARC_OK;
}

static unsigned sum16(const unsigned char *p, long n)
{
    unsigned s = 0;
    while (n-- > 0) s += *p++;
    return s & 0xFFFF;
}

int dms_info(struct arcsrc *s, struct dmsinfo *i)
{
    struct dms z;
    return start(&z, s, i);
}

int dms_unpack(struct arcsrc *s, struct arcsink *out,
               int (*track)(void *ctx, int tracknum), void *tctx)
{
    struct dms z;
    struct dmsinfo i;
    unsigned char *res;
    long tsize = 0;
    int r, nexttr = 0, any = 0;
    if ((r = start(&z, s, &i))) return r;
    if (i.encrypted || i.fms) return ARC_EUNSUP;
    for (;;) {
        if ((r = next(&z, 1)) <= 0) break;
        /* the disk: tracks 0..79 longer than 2048 bytes; track 80 (the
         * DIZ), 0xFFFF (the banner) and a 1024-byte track 0 (an advert)
         * are not, and xDMS leaves the decoders' state alone for them */
        if (z.num >= 80 || z.unpk <= 2048) continue;
        r = ARC_ECORRUPT;
        if ((int)z.num < nexttr) break;         /* out of order or twice */
        if ((r = unpack(&z, &res))) break;
        r = ARC_ECRC;
        if (sum16(res, z.unpk) != z.usum) break;
        if (!tsize) tsize = z.unpk == 22528 || (z.unpk != 11264 && i.diskhd) ? 22528 : 11264;
        any = 1;
        /* tracks the archive skipped: zeros, from the buffer this
         * track is not in */
        if (nexttr < (int)z.num) {
            unsigned char **zp = res == z.a ? &z.b : &z.a;
            long *zc = res == z.a ? &z.bcap : &z.acap;
            r = ARC_ENOMEM;
            if (!need(zp, zc, tsize)) break;
            memset(*zp, 0, tsize);
            r = ARC_OK;
            for (; nexttr < (int)z.num; nexttr++)
                if (!out->write(out->ctx, *zp, tsize) || (track && !track(tctx, nexttr))) {
                    r = ARC_ESTOP;
                    break;
                }
            if (r) break;
        }
        if (!out->write(out->ctx, res, z.unpk) || (track && !track(tctx, z.num))) {
            r = ARC_ESTOP;
            break;
        }
        nexttr = z.num + 1;
    }
    if (!r) {
        /* a file cut short between tracks: the header knew better */
        if (!any || (i.hightrack < 80 && nexttr <= i.hightrack)) r = ARC_ECORRUPT;
    }
    if (!r && nexttr < 80) {
        if (!tsize) tsize = i.diskhd ? 22528 : 11264;
        if (!need(&z.b, &z.bcap, tsize)) r = ARC_ENOMEM;
        else memset(z.b, 0, tsize);
        while (!r && nexttr < 80) {
            if (!out->write(out->ctx, z.b, tsize) || (track && !track(tctx, nexttr))) r = ARC_ESTOP;
            nexttr++;
        }
    }
    done(&z);
    return r;
}

int dms_text(struct arcsrc *s, unsigned int which, struct arcsink *out)
{
    struct dms z;
    struct dmsinfo i;
    unsigned char *res;
    int r, found = 0;
    if ((r = start(&z, s, &i))) return r;
    if (i.fms || (i.encrypted && which != DMS_DIZ)) return ARC_EUNSUP;
    while ((r = next(&z, 0)) > 0) {
        if (z.num != which) continue;
        /* read it again, with its data */
        z.pos -= THLEN + z.pk1;
        if ((r = s->read(s->ctx, z.pos, z.h, THLEN)) != THLEN) {
            r = r < 0 ? ARC_EREAD : ARC_ECORRUPT;
            break;
        }
        z.hn = THLEN;
        if ((r = next(&z, 1)) <= 0 || (r = unpack(&z, &res))) {
            if (!r) r = ARC_ECORRUPT;
            break;
        }
        if (sum16(res, z.unpk) != z.usum) {
            r = ARC_ECRC;
            break;
        }
        if (!out->write(out->ctx, res, z.unpk)) {
            r = ARC_ESTOP;
            break;
        }
        found = 1;
    }
    done(&z);
    return r < 0 ? r : found ? ARC_OK : 1;
}
