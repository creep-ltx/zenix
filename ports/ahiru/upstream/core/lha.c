/* Zenix's copy: derived from other authors' code (lha: LHa for UNIX; lzx: unlzx 1.1; dms: xDMS 1.3, public
 * domain), through the owner's BBX Go readers. Its licence is NOT settled: not for a public release of Zenix
 * until it is (docs/audit-2026-10-29-block2.md, "Licences to settle"). */
/* lha - see lha.h. Plain C, no Amiga headers, no stdio, no malloc:
 * memory comes from arc_alloc, bytes from the arcsrc, output goes to
 * the arcsink a window at a time.
 *
 * The decoders follow LHa for UNIX (slide.c, huf.c, dhuf.c, larc.c)
 * and BBX's Go port, with every table build and loop bounded so that
 * damaged input ends in ARC_ECORRUPT or ARC_ECRC, never a wild write
 * or a hang. Integers are assembled from bytes: the 68000 is
 * big-endian and faults on odd word access.
 */
#include <string.h>
#include "lha.h"

#define le16(p) ((unsigned int)(p)[0] | (unsigned int)(p)[1] << 8)
#define le32(p) ((unsigned long)(p)[0] | (unsigned long)(p)[1] << 8 | \
                 (unsigned long)(p)[2] << 16 | (unsigned long)(p)[3] << 24)

/* ---- headers ----------------------------------------------------- */

#define SCAN 4096               /* the self-extractor scan reads this much at a time */
#define HMAX 300                /* bytes of an extended header we look at */
#define MAXEXT 64               /* more extended headers than this is damage */
#define SFXMAX 65536UL          /* how far in a self-extractor's archive may start */

struct lst {
    struct arcent e;
    unsigned char nm[256], dr[256];
    int nml, drl;
    unsigned char h[SCAN];
};

static long rd(struct arcsrc *s, unsigned long off, unsigned char *buf, long n)
{
    if (off >= s->size) return 0;
    if ((unsigned long)n > s->size - off) n = (long)(s->size - off);
    return s->read(s->ctx, off, buf, n);
}

static int methodof(const unsigned char *m)
{
    if (m[0] != '-' || m[1] != 'l' || m[4] != '-') return LHA_UNKNOWN;
    if (m[2] == 'h') {
        switch (m[3]) {
        case '0': return LHA_LH0;
        case '1': return LHA_LH1;
        case '4': return LHA_LH4;
        case '5': return LHA_LH5;
        case '6': return LHA_LH6;
        case '7': return LHA_LH7;
        case 'd': return LHA_LHD;
        }
    } else if (m[2] == 'z') {
        switch (m[3]) {
        case 's': return LHA_LZS;
        case '5': return LHA_LZ5;
        case '4': return LHA_LZ4;
        }
    }
    return LHA_UNKNOWN;
}

/* does a header plausibly start here? (n >= 22) */
static int ishdr(const unsigned char *h)
{
    if (h[2] != '-' || h[3] != 'l' || h[6] != '-') return 0;
    if (h[4] != 'h' && h[4] != 'z') return 0;
    switch (h[20]) {
    case 0: case 1: return h[0] >= 22 + h[21];
    case 2: return le16(h) >= 26;
    case 3: return le16(h) == 4;
    }
    return 0;
}

/* the level 0/1 header sum: the scan must not stop on stray "-lh" text */
static int sumok(const unsigned char *h)
{
    unsigned int k, t = 0;
    for (k = 2; k < (unsigned int)h[0] + 2; k++) t += h[k];
    return (t & 0xFF) == h[1];
}

/* where the first header is: at 0, or inside the first 64K of a
 * self-extractor (there only a checked level 0/1 or a level 2/3) */
static int findstart(struct arcsrc *s, unsigned char *b, unsigned long *start)
{
    unsigned long pos = 0;
    long n, i;
    n = rd(s, 0, b, SCAN);
    if (n < 0) return ARC_EREAD;
    if (n >= 22 && ishdr(b)) {
        *start = 0;
        return ARC_OK;
    }
    while (n >= 22 && pos < SFXMAX) {
        for (i = 0; i + 22 <= n; i++) {
            unsigned char *h = b + i;
            if (h[2] != '-' || !ishdr(h)) continue;
            if (h[20] < 2 && (i + h[0] + 2 > n || !sumok(h))) continue;
            *start = pos + (unsigned long)i;
            return ARC_OK;
        }
        if (n < SCAN) break;
        pos += SCAN - 260;      /* overlap: a whole level 0/1 header fits */
        n = rd(s, pos, b, SCAN);
        if (n < 0) return ARC_EREAD;
    }
    return ARC_ECORRUPT;
}

static void dosdate(unsigned long t, struct arcent *e)
{
    int sec = (int)(t & 31) * 2, mi = (int)(t >> 5) & 63, hr = (int)(t >> 11) & 31;
    if (sec > 59 || mi > 59 || hr > 23) return;
    arc_datestamp((int)(t >> 25) + 1980, (int)(t >> 21) & 15, (int)(t >> 16) & 31,
                  hr, mi, sec, &e->days, &e->mins, &e->ticks);
}

/* Unix seconds; 252460800 is 1.1.1978, where AmigaDOS days start */
static void unixdate(unsigned long t, struct arcent *e)
{
    if (t < 252460800UL) {
        e->days = e->mins = e->ticks = 0;
        return;
    }
    t -= 252460800UL;
    e->days = (long)(t / 86400UL);
    t %= 86400UL;
    e->mins = (long)(t / 60);
    e->ticks = (long)(t % 60) * 50;
}

static int copyto(unsigned char *d, int max, const unsigned char *s, int n)
{
    if (n > max) n = max;
    memcpy(d, s, n);
    return n;
}

/* one extended header: type, then dl bytes of data (perhaps cut short) */
static void ext(struct lst *L, const unsigned char *x, int dl, int *attr)
{
    struct arcent *e = &L->e;
    switch (x[0]) {
    case 0x01: L->nml = copyto(L->nm, 255, x + 1, dl); break;
    case 0x02: L->drl = copyto(L->dr, 255, x + 1, dl); break;
    case 0x3F: e->comment[copyto((unsigned char *)e->comment, 79, x + 1, dl)] = 0; break;
    case 0x40: if (dl >= 2) *attr = x[1]; break;
    case 0x54: if (dl >= 4) unixdate(le32(x + 1), e); break;
    }
}

/* add n path bytes: '\' (MS-DOS) and 0xFF (level 2) are '/', no "//",
 * no leading '/'; a NUL ends it */
static int putpath(char *d, int k, const unsigned char *s, int n)
{
    int i;
    for (i = 0; i < n && s[i]; i++) {
        int c = s[i];
        if (c == '\\' || c == 0xFF) c = '/';
        if (c == '/' && (k == 0 || d[k - 1] == '/')) continue;
        if (k < 255) d[k++] = (char)c;
    }
    return k;
}

/* the header at off: 1 = a member, 2 = a member whose data is cut
 * off, 0 = no header here (the end), < 0 = damage */
static int header(struct arcsrc *s, struct lst *L, unsigned long off, unsigned long *next)
{
    struct arcent *e = &L->e;
    unsigned char *h = L->h;
    unsigned long hsz, es, p, end;
    long n;
    int level, os = 0, attr, w = 2, k;

    memset(e, 0, sizeof *e);
    L->nml = L->drl = 0;
    n = rd(s, off, h, 32);
    if (n < 0) return ARC_EREAD;
    if (n < 22 || !ishdr(h)) return 0;  /* end marker, padding or an advert */
    level = h[20];
    e->method = methodof(h + 2);
    e->isdir = e->method == LHA_LHD;
    e->packed = le32(h + 7);
    e->size = le32(h + 11);
    e->aux2 = level;
    attr = h[19];

    if (level < 2) {
        int nl, i;
        hsz = (unsigned long)h[0] + 2;
        n = rd(s, off, h, (long)hsz);
        if (n < 0) return ARC_EREAD;
        if ((unsigned long)n < hsz) return ARC_ECORRUPT;
        nl = h[21];
        if (22UL + nl + 2 + (level ? 3 : 0) > hsz) return ARC_ECORRUPT;
        /* Amiga LhA keeps the filenote after a NUL in the name */
        for (i = 0; i < nl && h[22 + i]; i++) ;
        L->nml = copyto(L->nm, 255, h + 22, i);
        if (i + 1 < nl) e->comment[copyto((unsigned char *)e->comment, 79, h + 23 + i, nl - i - 1)] = 0;
        e->aux1 = le16(h + 22 + nl);
        if (hsz > 24UL + nl) os = h[24 + nl];
        dosdate(le32(h + 15), e);
        p = off + hsz;
        end = s->size;
        es = level ? le16(h + hsz - 2) : 0;
    } else if (level == 2) {
        hsz = le16(h);
        e->aux1 = le16(h + 21);
        os = h[23];
        unixdate(le32(h + 15), e);
        es = le16(h + 24);
        p = off + 26;
        end = off + hsz;
    } else {
        if (n < 32) return ARC_ECORRUPT;
        hsz = le32(h + 24);
        if (hsz < 32) return ARC_ECORRUPT;
        e->aux1 = le16(h + 21);
        os = h[23];
        unixdate(le32(h + 15), e);
        es = le32(h + 28);
        p = off + 32;
        end = off + hsz;
        w = 4;
    }

    if (level >= 2 && hsz > s->size - off) return ARC_ECORRUPT;

    /* the extended headers: [type][data][size of the next one] */
    for (k = 0; es; k++) {
        unsigned long cur = es;
        long want = cur > HMAX ? HMAX : (long)cur;
        if (k >= MAXEXT || cur < (unsigned long)w + 1 || p > end || cur > end - p) return ARC_ECORRUPT;
        n = rd(s, p, h, want);
        if (n < 0) return ARC_EREAD;
        if (n < want) return ARC_ECORRUPT;
        ext(L, h, (int)(cur - w - 1 > HMAX - 1 ? HMAX - 1 : cur - w - 1), &attr);
        if (cur > HMAX) {
            n = rd(s, p + cur - w, h, w);
            if (n < 0) return ARC_EREAD;
            if (n < w) return ARC_ECORRUPT;
            es = w == 4 ? le32(h) : le16(h);
        } else
            es = w == 4 ? le32(h + cur - w) : le16(h + cur - w);
        if (level == 1) {
            /* level 1 counts its extended headers in the packed size */
            if (e->packed < cur) return ARC_ECORRUPT;
            e->packed -= cur;
        }
        p += cur;
    }
    e->dataoff = level < 2 ? p : off + hsz;

    /* Amiga LhA keeps the protection bits in the MS-DOS attribute
     * byte; level 0 has no OS id, and 0x20 there is MS-DOS "archive" */
    if (os == 'A' || (level == 0 && os == 0 && attr != 0x20)) e->prot = attr & 0xFF;

    /* Amiga LhA writes a drawer as -lh0- with no name, only a path */
    if (e->size == 0 && L->drl && (!L->nml || L->nm[L->nml - 1] == '/' || L->nm[L->nml - 1] == '\\'))
        e->isdir = 1;
    k = putpath(e->name, 0, L->dr, L->drl);
    if (k && L->nml) k = putpath(e->name, k, (const unsigned char *)"/", 1);
    k = putpath(e->name, k, L->nm, L->nml);
    while (k && e->name[k - 1] == '/') k--;
    e->name[k] = 0;
    if (e->isdir) e->size = 0;

    *next = e->dataoff + e->packed;
    if (*next < e->dataoff || *next > s->size) return 2;
    return 1;
}

int lha_list(struct arcsrc *s, arc_addfn add, void *ctx)
{
    struct lst *L;
    unsigned long off, next;
    int r, k;
    if (!(L = arc_alloc(sizeof *L))) return ARC_ENOMEM;
    r = findstart(s, L->h, &off);
    /* a member's header must start after the last one's: the
     * offsets only grow, so the loop ends */
    while (r == ARC_OK) {
        k = header(s, L, off, &next);
        if (k <= 0) {
            r = k;
            break;
        }
        if (!add(ctx, &L->e)) break;
        if (k == 2) {
            r = ARC_ECORRUPT;
            break;
        }
        if (next <= off) break;
        off = next;
    }
    arc_free(L);
    return r;
}

/* ---- the decoders ------------------------------------------------ */

#define IBUF 4096               /* packed bytes read at a time */
#define PASTMAX 16              /* zero bytes fed past the data before it is damage */
#define NC 510                  /* literals and match lengths 3..256 */
#define NT 19                   /* the code-length code */
#define NPT 20                  /* >= NT and every position count */
#define CBITS 12                /* the literal table's index bits */
#define LH1N 314                /* lh1: literals and match lengths 3..60 */
#define LH1T (LH1N * 2 - 1)     /* lh1: nodes in its tree */
#define LH1R (LH1T - 1)         /* lh1: the root */

struct dec {
    struct arcsrc *s;
    struct arcsink *out;
    unsigned long roff, rend;   /* the next packed byte to read, the end of the data */
    const unsigned char *ip, *ie;
    unsigned long bb;           /* bits, left-aligned in 32 */
    int bc;                     /* how many of them are real */
    int past, err;
    unsigned int crc;
    unsigned char *win;
    unsigned int wsize;
    union {
        struct {                /* lh4-lh7: ar002's tables */
            unsigned short ct[1 << CBITS], pt[256];
            unsigned short left[2 * NC], right[2 * NC];
            unsigned char cl[NC], pl[NPT];
        } st;
        struct {                /* lh1: the adaptive tree, the fixed position code */
            unsigned short freq[LH1T + 1], prnt[LH1T + LH1N], son[LH1T];
            unsigned char pt[256], pl[64];
        } dy;
    } u;
    unsigned char ibuf[IBUF];
};

/* refill the input; past the end of the data it gives zeros, as LhA
 * does, and counts them so damage cannot run on */
static void more(struct dec *z)
{
    long n = 0, want;
    if (z->roff < z->rend) {
        want = z->rend - z->roff > IBUF ? IBUF : (long)(z->rend - z->roff);
        n = z->s->read(z->s->ctx, z->roff, z->ibuf, want);
        if (n < 0) z->err = ARC_EREAD;
        if (n <= 0) {
            n = 0;
            z->rend = z->roff;
        }
        z->roff += n;
    }
    if (n == 0) {
        memset(z->ibuf, 0, 16);
        n = 16;
        z->past += 16;
    }
    z->ip = z->ibuf;
    z->ie = z->ibuf + n;
}

/* the bit reader on locals in the hot loops: FILL leaves >= 25 bits */
#define FILL() while (bc <= 24) { \
        if (ip == ie) { more(z); ip = z->ip; ie = z->ie; } \
        bb |= (unsigned long)*ip++ << (24 - bc); bc += 8; }
#define DROP(n) (bb = (bb << (n)) & 0xFFFFFFFFUL, bc -= (n))
#define LOAD() (bb = z->bb, bc = z->bc, ip = z->ip, ie = z->ie)
#define SAVE() (z->bb = bb, z->bc = bc, z->ip = ip, z->ie = ie)

/* the same on the struct, for the rare block headers; n is 1..16 */
static unsigned int getb(struct dec *z, int n)
{
    unsigned long bb = z->bb;
    int bc = z->bc;
    const unsigned char *ip = z->ip, *ie = z->ie;
    unsigned int v;
    FILL();
    v = (unsigned int)(bb >> (32 - n));
    DROP(n);
    SAVE();
    return v;
}

/* out with the window's first n bytes */
static int flush(struct dec *z, unsigned int n)
{
    if (!n) return 1;
    z->crc = arc_crc16(z->crc, z->win, n);
    if (!z->out->write(z->out->ctx, z->win, n)) {
        z->err = ARC_ESTOP;
        return 0;
    }
    return 1;
}

/* a code's lookup table and overflow tree (ar002's make_table). Only
 * a complete code is taken, which bounds the tree to nchar-1 nodes. */
static int mktable(struct dec *z, int nchar, const unsigned char *len, int tbits, unsigned short *table)
{
    unsigned long start[18], weight[17], total, i, k, nx, mask;
    unsigned int count[17], avail;
    unsigned short *left = z->u.st.left, *right = z->u.st.right, *p;
    int ch, l, jut = 16 - tbits, n;

    for (l = 1; l <= 16; l++) count[l] = 0;
    for (ch = 0; ch < nchar; ch++) count[len[ch]]++;
    total = 0;
    for (l = 1; l <= 16; l++) {
        start[l] = total;
        total += (unsigned long)count[l] << (16 - l);
    }
    if (total != 0x10000UL) return ARC_ECORRUPT;
    start[17] = total;
    for (l = 1; l <= tbits; l++) {
        start[l] >>= jut;
        weight[l] = 1UL << (tbits - l);
    }
    for (; l <= 16; l++) weight[l] = 1UL << (16 - l);
    for (i = start[tbits + 1] >> jut, k = 1UL << tbits; i < k; i++) table[i] = 0;

    avail = nchar;
    mask = 1UL << (15 - tbits);
    for (ch = 0; ch < nchar; ch++) {
        if (!(l = len[ch])) continue;
        nx = start[l] + weight[l];
        if (l <= tbits) {
            for (i = start[l]; i < nx; i++) table[i] = (unsigned short)ch;
        } else {
            k = start[l];
            p = &table[k >> jut];
            for (n = l - tbits; n > 0; n--) {
                if (!*p) {
                    if (avail >= 2 * NC) return ARC_ECORRUPT;
                    left[avail] = right[avail] = 0;
                    *p = (unsigned short)avail++;
                }
                p = k & mask ? &right[*p] : &left[*p];
                k <<= 1;
            }
            *p = (unsigned short)ch;
        }
        start[l] = nx;
    }
    return ARC_OK;
}

/* a symbol through an 8-bit table; bits 23..16 follow the tree */
static int ptsym(struct dec *z, const unsigned short *pt, int nn, unsigned long bb)
{
    unsigned long m = 1UL << 23;
    int j = pt[bb >> 24];
    while (j >= nn && m >= 0x10000UL) {
        j = bb & m ? z->u.st.right[j] : z->u.st.left[j];
        m >>= 1;
    }
    return j < nn ? j : -1;
}

/* the code-length or position code lengths (huf.c's read_pt_len) */
static int readpt(struct dec *z, int nn, int nbit, int special)
{
    unsigned char *pl = z->u.st.pl;
    unsigned short *pt = z->u.st.pt;
    int i, n, c;
    n = getb(z, nbit);
    if (n == 0) {
        /* one symbol, no bits */
        c = getb(z, nbit);
        if (c >= nn) return ARC_ECORRUPT;
        for (i = 0; i < 256; i++) pt[i] = (unsigned short)c;
        for (i = 0; i < nn; i++) pl[i] = 0;
        return ARC_OK;
    }
    if (n > nn) return ARC_ECORRUPT;
    i = 0;
    while (i < n) {
        c = getb(z, 3);
        if (c == 7) {
            /* 7 and up: one more for each 1 bit, ended by a 0 */
            while (getb(z, 1))
                if (++c > 16) return ARC_ECORRUPT;
        }
        pl[i++] = (unsigned char)c;
        if (i == special) {
            c = getb(z, 2);
            while (c-- > 0 && i < nn) pl[i++] = 0;
        }
    }
    while (i < nn) pl[i++] = 0;
    return mktable(z, nn, pl, 8, pt);
}

/* the literal/length code lengths, sent in the code-length code */
static int readc(struct dec *z)
{
    unsigned char *cl = z->u.st.cl;
    int i, n, c;
    n = getb(z, 9);
    if (n == 0) {
        c = getb(z, 9);
        if (c >= NC) return ARC_ECORRUPT;
        for (i = 0; i < (1 << CBITS); i++) z->u.st.ct[i] = (unsigned short)c;
        for (i = 0; i < NC; i++) cl[i] = 0;
        return ARC_OK;
    }
    if (n > NC) return ARC_ECORRUPT;
    i = 0;
    while (i < n) {
        unsigned long bb;
        int bc;
        const unsigned char *ip, *ie;
        LOAD();
        FILL();
        c = ptsym(z, z->u.st.pt, NT, bb);
        if (c < 0) return ARC_ECORRUPT;
        DROP(z->u.st.pl[c]);
        SAVE();
        if (c <= 2) {
            /* runs of unused symbols */
            if (c == 0) c = 1;
            else if (c == 1) c = getb(z, 4) + 3;
            else c = getb(z, 9) + 20;
            while (c-- > 0 && i < NC) cl[i++] = 0;
        } else
            cl[i++] = (unsigned char)(c - 2);
    }
    while (i < NC) cl[i++] = 0;
    return mktable(z, NC, cl, CBITS, z->u.st.ct);
}

/* -lh4- to -lh7-: blocks of three Huffman codes, then LZ77 */
static int unstatic(struct dec *z, unsigned long left, int np, int pbit)
{
    unsigned long bb, blk = 0, m;
    int bc, r;
    const unsigned char *ip, *ie;
    unsigned char *win = z->win, *cl = z->u.st.cl, *pl = z->u.st.pl;
    unsigned short *ct = z->u.st.ct, *pt = z->u.st.pt;
    unsigned short *lft = z->u.st.left, *rgt = z->u.st.right;
    unsigned int wpos = 0, wsize = z->wsize, wmask = wsize - 1, from, c, len;
    unsigned long dist;

    LOAD();
    while (left) {
        if (z->past > PASTMAX) return z->err ? z->err : ARC_ECORRUPT;
        if (!blk) {
            SAVE();
            blk = getb(z, 16);
            if (!blk) blk = 0x10000UL;      /* LhA's 16-bit counter wraps */
            if ((r = readpt(z, NT, 5, 3)) || (r = readc(z)) || (r = readpt(z, np, pbit, -1)))
                return r;
            LOAD();
        }
        blk--;
        FILL();
        c = ct[bb >> (32 - CBITS)];
        if (c >= NC) {
            m = 1UL << (31 - CBITS);
            while (c >= NC && m >= 0x10000UL) {
                c = bb & m ? rgt[c] : lft[c];
                m >>= 1;
            }
            if (c >= NC) return ARC_ECORRUPT;
        }
        DROP(cl[c]);
        if (c < 256) {
            win[wpos++] = (unsigned char)c;
            left--;
            if (wpos == wsize) {
                if (!flush(z, wsize)) return z->err;
                wpos = 0;
            }
            continue;
        }
        len = c - 253;
        FILL();
        c = pt[bb >> 24];
        if ((int)c >= np) {
            m = 1UL << 23;
            while ((int)c >= np && m >= 0x10000UL) {
                c = bb & m ? rgt[c] : lft[c];
                m >>= 1;
            }
            if ((int)c >= np) return ARC_ECORRUPT;
        }
        DROP(pl[c]);
        dist = c;
        if (c > 1) {
            c--;
            FILL();
            dist = (1UL << c) + (bb >> (32 - c));
            DROP(c);
        }
        if (len > left) len = (unsigned int)left;
        left -= len;
        from = (wpos - (unsigned int)dist - 1) & wmask;
        while (len--) {
            win[wpos++] = win[from];
            from = (from + 1) & wmask;
            if (wpos == wsize) {
                if (!flush(z, wsize)) return z->err;
                wpos = 0;
            }
        }
    }
    return flush(z, wpos) ? ARC_OK : z->err;
}

/* ---- lh1: Okumura's LZHUF, an adaptive Huffman tree --------------- */

static void dyninit(struct dec *z)
{
    unsigned short *freq = z->u.dy.freq, *prnt = z->u.dy.prnt, *son = z->u.dy.son;
    unsigned char *pt = z->u.dy.pt, *pl = z->u.dy.pl;
    int i, j, l, code;
    for (i = 0; i < LH1N; i++) {
        freq[i] = 1;
        son[i] = (unsigned short)(i + LH1T);
        prnt[i + LH1T] = (unsigned short)i;
    }
    for (i = 0, j = LH1N; j <= LH1R; i += 2, j++) {
        freq[j] = freq[i] + freq[i + 1];
        son[j] = (unsigned short)i;
        prnt[i] = prnt[i + 1] = (unsigned short)j;
    }
    freq[LH1T] = 0xFFFF;        /* the sentinel that stops update's search */
    prnt[LH1R] = 0;

    /* the fixed code for a position's top 6 bits: 1 code of 3 bits,
     * 3 of 4, 8 of 5, 12 of 6, 24 of 7, 16 of 8 */
    for (i = 0; i < 64; i++) pl[i] = i < 1 ? 3 : i < 4 ? 4 : i < 12 ? 5 : i < 24 ? 6 : i < 48 ? 7 : 8;
    code = 0;
    for (l = 3, i = 0; l <= 8; l++, code <<= 1)
        for (; i < 64 && pl[i] == l; i++, code++)
            for (j = code << (8 - l); j < (code + 1) << (8 - l); j++) pt[j] = (unsigned char)i;
}

/* halve the counts and rebuild the tree */
static void reconst(struct dec *z)
{
    unsigned short *freq = z->u.dy.freq, *prnt = z->u.dy.prnt, *son = z->u.dy.son;
    unsigned int f;
    int i, j, k;
    for (i = j = 0; i < LH1T; i++)
        if (son[i] >= LH1T) {
            freq[j] = (unsigned short)((freq[i] + 1) / 2);
            son[j] = son[i];
            j++;
        }
    for (i = 0, j = LH1N; j < LH1T; i += 2, j++) {
        f = freq[i] + freq[i + 1];
        for (k = j - 1; f < freq[k]; k--) ;
        k++;
        memmove(freq + k + 1, freq + k, (j - k) * sizeof *freq);
        freq[k] = (unsigned short)f;
        memmove(son + k + 1, son + k, (j - k) * sizeof *son);
        son[k] = (unsigned short)i;
    }
    for (i = 0; i < LH1T; i++) {
        k = son[i];
        prnt[k] = (unsigned short)i;
        if (k < LH1T) prnt[k + 1] = (unsigned short)i;
    }
}

/* one more use of symbol c: count it and keep the tree ordered */
static void update(struct dec *z, int c)
{
    unsigned short *freq = z->u.dy.freq, *prnt = z->u.dy.prnt, *son = z->u.dy.son;
    unsigned int k;
    int i, j, l;
    if (freq[LH1R] == 0x8000) reconst(z);
    c = prnt[c + LH1T];
    do {
        k = ++freq[c];
        l = c + 1;
        if (k > freq[l]) {
            while (k > freq[++l]) ;
            l--;
            freq[c] = freq[l];
            freq[l] = (unsigned short)k;
            i = son[c];
            prnt[i] = (unsigned short)l;
            if (i < LH1T) prnt[i + 1] = (unsigned short)l;
            j = son[l];
            son[l] = (unsigned short)i;
            prnt[j] = (unsigned short)c;
            if (j < LH1T) prnt[j + 1] = (unsigned short)c;
            son[c] = (unsigned short)j;
            c = l;
        }
    } while ((c = prnt[c]) != 0);
}

static int unlh1(struct dec *z, unsigned long left)
{
    unsigned long bb;
    int bc;
    const unsigned char *ip, *ie;
    unsigned char *win = z->win, *pt = z->u.dy.pt, *pl = z->u.dy.pl;
    unsigned short *son = z->u.dy.son;
    unsigned int wpos = 0, wsize = z->wsize, wmask = wsize - 1, from, c, len, dist;

    dyninit(z);
    LOAD();
    while (left) {
        if (z->past > PASTMAX) return z->err ? z->err : ARC_ECORRUPT;
        /* the tree is never damaged (only the choice of leaf comes
         * from the data), so this walk always ends */
        c = son[LH1R];
        while (c < LH1T) {
            FILL();
            c = son[c + (unsigned int)(bb >> 31)];
            DROP(1);
        }
        c -= LH1T;
        update(z, (int)c);
        if (c < 256) {
            win[wpos++] = (unsigned char)c;
            left--;
            if (wpos == wsize) {
                if (!flush(z, wsize)) return z->err;
                wpos = 0;
            }
            continue;
        }
        len = c - 253;
        FILL();
        c = pt[bb >> 24];
        DROP(pl[c]);
        FILL();
        dist = c << 6 | (unsigned int)(bb >> 26);
        DROP(6);
        if (len > left) len = (unsigned int)left;
        left -= len;
        from = (wpos - dist - 1) & wmask;
        while (len--) {
            win[wpos++] = win[from];
            from = (from + 1) & wmask;
            if (wpos == wsize) {
                if (!flush(z, wsize)) return z->err;
                wpos = 0;
            }
        }
    }
    return flush(z, wpos) ? ARC_OK : z->err;
}

/* ---- LArc: -lzs- (2K window) and -lz5- (4K, a preset window) ------ */

static void lz5init(unsigned char *t)
{
    int i;
    for (i = 0; i < 256; i++) memset(t + i * 13 + 18, i, 13);
    for (i = 0; i < 256; i++) t[256 * 13 + 18 + i] = (unsigned char)i;
    for (i = 0; i < 256; i++) t[256 * 13 + 256 + 18 + i] = (unsigned char)(255 - i);
    memset(t + 256 * 13 + 512 + 18, 0, 128);
    memset(t + 256 * 13 + 512 + 128 + 18, ' ', 128 - 18);
}

static int unlarc(struct dec *z, unsigned long left, int lz5)
{
    unsigned long bb;
    int bc, flags = 0, fcnt = 0;
    const unsigned char *ip, *ie;
    unsigned char *win = z->win;
    unsigned int wpos = 0, wsize = z->wsize, wmask = wsize - 1, from, c, len;

    LOAD();
    while (left) {
        if (z->past > PASTMAX) return z->err ? z->err : ARC_ECORRUPT;
        FILL();
        if (lz5) {
            /* a flag byte, low bit first: 1 = a literal byte, 0 = a
             * 12-bit position and a 4-bit length in two bytes */
            if (!fcnt) {
                flags = (int)(bb >> 24);
                DROP(8);
                fcnt = 8;
                FILL();
            }
            fcnt--;
            c = flags & 1;
            flags >>= 1;
            if (!c) {
                from = (unsigned int)(bb >> 24) | (unsigned int)(bb >> 12 & 0xF00);
                len = (unsigned int)(bb >> 16 & 0xF) + 3;
                DROP(16);
                from += 18;     /* the ring started 18 bytes before ours */
            } else {
                c = (unsigned int)(bb >> 24);
                DROP(8);
                len = 0;
            }
        } else {
            /* one bit: 1 = an 8-bit literal, 0 = 11-bit position, 4-bit length */
            if (bb >> 31) {
                c = (unsigned int)(bb >> 23 & 0xFF);
                DROP(9);
                len = 0;
            } else {
                from = (unsigned int)(bb >> 20 & 0x7FF) + 17;
                len = (unsigned int)(bb >> 16 & 0xF) + 2;
                DROP(16);
            }
        }
        if (!len) {
            win[wpos++] = (unsigned char)c;
            left--;
            if (wpos == wsize) {
                if (!flush(z, wsize)) return z->err;
                wpos = 0;
            }
            continue;
        }
        if (len > left) len = (unsigned int)left;
        left -= len;
        while (len--) {
            from &= wmask;
            win[wpos++] = win[from++];
            if (wpos == wsize) {
                if (!flush(z, wsize)) return z->err;
                wpos = 0;
            }
        }
    }
    return flush(z, wpos) ? ARC_OK : z->err;
}

/* ---- extract ----------------------------------------------------- */

#define STORECHUNK 16384        /* a stored member moves this much per read/write */

static int unstore(struct arcsrc *s, const struct arcent *e, struct arcsink *out, unsigned int *crc)
{
    unsigned char *buf;
    unsigned long left = e->size, off = e->dataoff;
    long n, got;
    int r = ARC_OK;
    if (e->size > e->packed) return ARC_ECORRUPT;
    if (!(buf = arc_alloc(left < STORECHUNK ? (left ? left : 1) : STORECHUNK))) return ARC_ENOMEM;
    while (left) {
        n = left < STORECHUNK ? (long)left : STORECHUNK;
        got = rd(s, off, buf, n);
        if (got < 0) {
            r = ARC_EREAD;
            break;
        }
        if (got < n) {
            r = ARC_ECORRUPT;
            break;
        }
        *crc = arc_crc16(*crc, buf, n);
        if (!out->write(out->ctx, buf, n)) {
            r = ARC_ESTOP;
            break;
        }
        off += n;
        left -= n;
    }
    arc_free(buf);
    return r;
}

int lha_extract(struct arcsrc *s, const struct arcent *e, struct arcsink *out)
{
    struct dec *z;
    unsigned int crc = 0;
    int r, bits;
    unsigned long wsize;

    switch (e->method) {
    case LHA_LHD: return ARC_OK;
    case LHA_LH0:
    case LHA_LZ4:
        r = unstore(s, e, out, &crc);
        return r ? r : crc == (e->aux1 & 0xFFFF) ? ARC_OK : ARC_ECRC;
    case LHA_LH1: case LHA_LH4: bits = 12; break;
    case LHA_LH5: bits = 13; break;
    case LHA_LH6: bits = 15; break;
    case LHA_LH7: bits = 16; break;
    case LHA_LZS: bits = 11; break;
    case LHA_LZ5: bits = 12; break;
    default: return ARC_EUNSUP;
    }
    wsize = 1UL << bits;
    if (!(z = arc_alloc(sizeof *z))) return ARC_ENOMEM;
    if (!(z->win = arc_alloc(wsize))) {
        arc_free(z);
        return ARC_ENOMEM;
    }
    z->s = s;
    z->out = out;
    z->roff = e->dataoff;
    z->rend = e->dataoff + e->packed;
    if (z->rend > s->size || z->rend < z->roff) z->rend = s->size;
    z->ip = z->ie = z->ibuf;
    z->wsize = (unsigned int)wsize;
    /* the packers may refer back into the window before the first
     * byte: LhA starts it as spaces, LArc's -lz5- with a pattern */
    memset(z->win, ' ', wsize);
    switch (e->method) {
    case LHA_LH1: r = unlh1(z, e->size); break;
    case LHA_LZS: r = unlarc(z, e->size, 0); break;
    case LHA_LZ5: lz5init(z->win); r = unlarc(z, e->size, 1); break;
    default:
        r = unstatic(z, e->size, bits + 1 > 14 ? bits + 1 : 14, bits > 13 ? 5 : 4);
    }
    if (!r && z->err) r = z->err;
    if (!r && z->crc != (e->aux1 & 0xFFFF)) r = ARC_ECRC;
    arc_free(z->win);
    arc_free(z);
    return r;
}
