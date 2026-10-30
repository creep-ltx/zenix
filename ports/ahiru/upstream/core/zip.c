/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* zip - see zip.h. No Amiga headers here, on purpose.
 *
 * Little-endian fields are read a byte at a time (the 68000 is
 * big-endian and faults on odd word reads). Every offset taken from
 * the archive is checked against the archive's size before it is
 * used, and every loop is bounded by the input or by the member's
 * stated size, so a damaged or crafted archive gives an error code. */
#include "zip.h"

#define SIG_LOCAL   0x04034b50UL
#define SIG_CENTRAL 0x02014b50UL
#define SIG_END     0x06054b50UL
#define SIG_END64   0x06064b50UL
#define SIG_LOC64   0x07064b50UL

#define RBUF 4096               /* the read buffer, listing and extracting */

static unsigned int g16(const unsigned char *p)
{
    return p[0] | (unsigned int)p[1] << 8;
}

static unsigned long g32(const unsigned char *p)
{
    return p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

/* exactly n bytes at off - anything less is a damaged archive */
static int rdx(struct arcsrc *s, unsigned long off, void *buf, long n)
{
    long r;
    if (off > s->size || (unsigned long)n > s->size - off) return ARC_ECORRUPT;
    r = s->read(s->ctx, off, buf, n);
    if (r < 0) return ARC_EREAD;
    return r == n ? ARC_OK : ARC_ECORRUPT;
}

/* ---- listing ------------------------------------------------------ */

struct lst {
    unsigned char b[RBUF];      /* a window on the archive */
    unsigned long off;          /* ... at this offset */
    long n;                     /* ... this many bytes valid */
    struct arcent e;
};

/* n bytes (n <= RBUF) at off through the window: a pointer, or NULL
 * with *err set */
static const unsigned char *get(struct arcsrc *s, struct lst *L, unsigned long off, long n, int *err)
{
    unsigned long m;
    long r;
    if (off >= L->off && L->n >= n && off - L->off <= (unsigned long)(L->n - n))
        return L->b + (off - L->off);
    *err = ARC_ECORRUPT;
    if (off > s->size || s->size - off < (unsigned long)n) return 0;
    m = s->size - off;
    if (m > RBUF) m = RBUF;
    L->n = 0;
    r = s->read(s->ctx, off, L->b, (long)m);
    if (r < 0) *err = ARC_EREAD;
    if (r < n) return 0;
    L->off = off;
    L->n = r;
    return L->b;
}

/* the end record: scan back from the end, at most 64K (its comment)
 * + 22 bytes; the last "PK\5\6" whose comment fits is it */
static int findend(struct arcsrc *s, unsigned char *b, unsigned long *endp)
{
    unsigned char r[22];
    unsigned long low, top, cs;
    long n, i;
    int err;
    if (s->size < 22) return ARC_ECORRUPT;
    top = s->size - 22;
    low = top > 65535 ? top - 65535 : 0;
    for (;;) {
        cs = top - low > RBUF - 4 ? top - (RBUF - 4) : low;
        n = (long)(top - cs) + 4;
        if ((err = rdx(s, cs, b, n))) return err;
        for (i = n - 4; i >= 0; i--)
            if (b[i] == 'P' && b[i + 1] == 'K' && b[i + 2] == 5 && b[i + 3] == 6) {
                if ((err = rdx(s, cs + i, r, 22))) return err;
                if (s->size - (cs + i) - 22 >= g16(r + 20)) {
                    *endp = cs + i;
                    return ARC_OK;
                }
            }
        if (cs == low) return ARC_ECORRUPT;
        top = cs - 1;
    }
}

/* AmigaDOS protection from the external attributes, by the host that
 * made the archive. Amiga (1): Info-ZIP keeps hsparwed in the high
 * word with r/w/e/d meaning "allowed" (the inverse of fib_Protection;
 * PKAZip stored them the other way round, told apart as UnZip does: by
 * w agreeing with the MS-DOS read-only bit); the a bit is dropped.
 * Unix (3): only a missing owner w becomes write/delete protected - a
 * missing x is the normal case for Amiga programs zipped on Linux, so
 * it does NOT clear e. The rest: the MS-DOS read-only bit. */
static unsigned long protof(int host, unsigned long ext)
{
    if (host == 1) {
        if ((ext & 1) == (ext >> 18 & 1)) ext ^= 0x000F0000UL;
        return ((ext >> 16) & 0xEF) ^ 0x0F;
    }
    if (host == 3 && (ext >> 16)) return (ext >> 16) & 0200 ? 0 : 5;
    return ext & 1 ? 5 : 0;
}

/* ZIP64: the 8-byte fields in extra block 1, in header order, for the
 * 32-bit fields that are all ones. FALSE when one is missing or does
 * not fit in 32 bits. */
static int zip64(const unsigned char *x, long m, unsigned long *v[3])
{
    long i, k, len;
    int j;
    for (i = 0; i + 4 <= m; i += 4 + len) {
        len = g16(x + i + 2);
        if (g16(x + i) != 1) continue;
        k = i + 4;
        for (j = 0; j < 3; j++)
            if (v[j] && *v[j] == 0xFFFFFFFFUL) {
                if (k + 8 > i + 4 + len || k + 8 > m || g32(x + k + 4)) return 0;
                *v[j] = g32(x + k);
                k += 8;
            }
        return 1;
    }
    return 0;
}

static int list2(struct arcsrc *s, struct lst *L, arc_addfn add, void *ctx)
{
    const unsigned char *p;
    struct arcent *e = &L->e;
    unsigned long endp = 0, cdoff, cdsize, cdend, delta, pos, next, *v[3];
    unsigned int nl, xl, cl, i, k, host, flags;
    int err;
    if ((err = findend(s, L->b, &endp))) return err;
    if (!(p = get(s, L, endp, 22, &err))) return err;
    cdsize = g32(p + 12);
    cdoff = g32(p + 16);
    cdend = endp;
    if (cdoff == 0xFFFFFFFFUL || cdsize == 0xFFFFFFFFUL || g16(p + 10) == 0xFFFF) {
        /* ZIP64: the locator just in front, the ZIP64 end record in
         * front of that (where the locator says, or right before it) */
        if (endp >= 20 + 56 && (p = get(s, L, endp - 20, 20, &err)) && g32(p) == SIG_LOC64) {
            if (g32(p + 12)) return ARC_EUNSUP;
            pos = g32(p + 8);
            if (!(p = get(s, L, pos, 56, &err)) || g32(p) != SIG_END64) {
                pos = endp - 20 - 56;
                if (!(p = get(s, L, pos, 56, &err)) || g32(p) != SIG_END64) return ARC_ECORRUPT;
            }
            if (g32(p + 44) || g32(p + 52)) return ARC_EUNSUP;
            cdsize = g32(p + 40);
            cdoff = g32(p + 48);
            cdend = pos;
        } else if (cdoff == 0xFFFFFFFFUL || cdsize == 0xFFFFFFFFUL)
            return ARC_ECORRUPT;
    }
    /* bytes in front of the archive (a self-extractor) move every
     * offset: the directory ends where the end record starts */
    if (cdsize > cdend || cdend - cdsize < cdoff) return ARC_ECORRUPT;
    delta = cdend - cdsize - cdoff;
    pos = cdoff + delta;
    while (pos < cdend) {
        if (!(p = get(s, L, pos, 46, &err))) return err;
        if (g32(p) != SIG_CENTRAL) return ARC_ECORRUPT;
        host = p[5];
        flags = g16(p + 8);
        nl = g16(p + 28);
        xl = g16(p + 30);
        cl = g16(p + 32);
        next = pos + 46 + nl + xl + cl;
        if (next > cdend) return ARC_ECORRUPT;
        for (i = 0; i < sizeof(*e); i++) ((char *)e)[i] = 0;
        e->method = g16(p + 10);
        e->aux1 = g32(p + 16);
        e->aux2 = flags;
        e->packed = g32(p + 20);
        e->size = g32(p + 24);
        e->dataoff = g32(p + 42);
        k = g16(p + 14);
        i = g16(p + 12);
        arc_datestamp(1980 + (k >> 9), (k >> 5) & 15, k & 31, i >> 11, (i >> 5) & 63, (i & 31) * 2,
                      &e->days, &e->mins, &e->ticks);
        e->prot = protof(host, g32(p + 38));
        if (host == 3 ? (g32(p + 38) >> 16 & 0170000) == 0040000 : (p[38] & 0x10) != 0) e->isdir = 1;
        if (e->packed == 0xFFFFFFFFUL || e->size == 0xFFFFFFFFUL || e->dataoff == 0xFFFFFFFFUL) {
            v[0] = &e->size;
            v[1] = &e->packed;
            v[2] = &e->dataoff;
            k = xl < RBUF ? xl : RBUF;
            if (!(p = get(s, L, pos + 46 + nl, k, &err))) return err;
            if (!zip64(p, k, v)) e->method = -1;
        }
        e->dataoff += delta;
        /* the name: the last byte says directory; at most 255 kept */
        if (nl) {
            if (!(p = get(s, L, pos + 46 + nl - 1, 1, &err))) return err;
            if (*p == '/' || *p == '\\') e->isdir = 1;
            k = nl < sizeof(e->name) ? nl : sizeof(e->name) - 1;
            if (!(p = get(s, L, pos + 46, k, &err))) return err;
            for (i = 0; i < k && p[i]; i++)
                e->name[i] = p[i] == '\\' && host != 3 && host != 1 ? '/' : p[i];
            e->name[i] = 0;
        }
        /* no leading '/', no trailing '/' */
        for (i = 0; e->name[i] == '/'; i++) ;
        if (i) for (k = 0; (e->name[k] = e->name[k + i]) != 0; k++) ;
        for (i = 0; e->name[i]; i++) ;
        while (i && e->name[i - 1] == '/') e->name[--i] = 0;
        if (cl) {
            k = cl < sizeof(e->comment) ? cl : sizeof(e->comment) - 1;
            if (!(p = get(s, L, pos + 46 + nl + xl, k, &err))) return err;
            for (i = 0; i < k && p[i]; i++) e->comment[i] = p[i] < ' ' ? ' ' : p[i];
            e->comment[i] = 0;
        }
        if (e->name[0] && !add(ctx, e)) return ARC_ESTOP;
        pos = next;
    }
    return ARC_OK;
}

int zip_list(struct arcsrc *s, arc_addfn add, void *ctx)
{
    struct lst *L;
    int err;
    if (!(L = arc_alloc(sizeof(*L)))) return ARC_ENOMEM;
    err = list2(s, L, add, ctx);
    arc_free(L);
    return err;
}

/* ---- inflate (RFC 1951) -------------------------------------------
 * Bits come LSB first from a 32-bit buffer filled a byte at a time
 * from a 4K input buffer. A Huffman code is decoded by one lookup of
 * the next FAST bits (codes are stored bit-reversed, so the table is
 * indexed by the raw bits); the few longer codes take the slow road,
 * a canonical walk one length at a time. Output goes to a 32K window
 * that is written out each time it fills. */

#define WSIZE 32768U
#define WMASK 32767U
#define FAST 9
#define FMASK ((1U << FAST) - 1)

struct huff {
    unsigned short fast[1 << FAST];     /* symbol << 4 | length; 0: a longer code */
    unsigned short cnt[16];             /* how many codes of each length */
    unsigned short sym[288];            /* the symbols in code order */
};

struct inf {
    unsigned char win[WSIZE];
    unsigned char ib[RBUF];
    struct huff lit, dst;
    unsigned char len[288 + 32];        /* code lengths, lit then distance */
    unsigned short lbase[29], dbase[30];
    unsigned char lext[29], dext[30];
    struct arcsrc *s;
    struct arcsink *out;
    unsigned long inoff, inleft;        /* the next packed byte, how many are left */
    const unsigned char *ip, *iend;
    int phantom;                        /* zero bytes fed past the end */
    int fixed;                          /* lit/dst hold the fixed codes */
    unsigned long size, crc;
};

/* more input: up to 4K; past the end a zero byte at a time, a few
 * (a code is looked at 15 bits ahead) - consuming one is an error,
 * found at the end */
static int fill(struct inf *z)
{
    long n, r;
    if (!z->inleft) {
        if (z->phantom >= 8) return ARC_ECORRUPT;
        z->phantom++;
        z->ib[0] = 0;
        n = 1;
    } else {
        n = z->inleft < RBUF ? (long)z->inleft : RBUF;
        r = z->s->read(z->s->ctx, z->inoff, z->ib, n);
        if (r < 0) return ARC_EREAD;
        if (r == 0) return ARC_ECORRUPT;
        n = r;
        z->inoff += n;
        z->inleft -= n;
    }
    z->ip = z->ib;
    z->iend = z->ib + n;
    return ARC_OK;
}

/* canonical codes from lengths; over-subscribed is damage, incomplete
 * is allowed (an unused code then fails when met) */
static int build(struct huff *h, const unsigned char *len, int n)
{
    unsigned short offs[16];
    int i, l, left, code, k, rev, c, j;
    for (l = 0; l < 16; l++) h->cnt[l] = 0;
    for (i = 0; i < n; i++) h->cnt[len[i]]++;
    left = 1;
    for (l = 1; l < 16; l++) {
        left = (left << 1) - h->cnt[l];
        if (left < 0) return ARC_ECORRUPT;
    }
    offs[1] = 0;
    for (l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->cnt[l];
    for (i = 0; i < n; i++)
        if (len[i]) h->sym[offs[len[i]]++] = i;
    for (i = 0; i < (1 << FAST); i++) h->fast[i] = 0;
    code = k = 0;
    for (l = 1; l <= FAST; l++) {
        for (i = h->cnt[l]; i > 0; i--, k++, code++) {
            for (rev = 0, c = code, j = l; j > 0; j--, c >>= 1) rev = rev << 1 | (c & 1);
            for (; rev < (1 << FAST); rev += 1 << l) h->fast[rev] = h->sym[k] << 4 | l;
        }
        code <<= 1;
    }
    return ARC_OK;
}

/* a code longer than FAST bits (bits holds at least 15) */
static int slow(const struct huff *h, unsigned long bits, int *lenp)
{
    int len, code = 0, first = 0, index = 0, count;
    for (len = 1; len < 16; len++) {
        code |= (int)bits & 1;
        bits >>= 1;
        count = h->cnt[len];
        if (code >= first && code - first < count) {
            *lenp = len;
            return h->sym[index + code - first];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* the window's first n bytes out */
static int flush(struct inf *z, unsigned int n)
{
    if (!n) return ARC_OK;
    z->crc = arc_crc32(z->crc, z->win, (long)n);
    return z->out->write(z->out->ctx, z->win, (long)n) ? ARC_OK : ARC_ESTOP;
}

#define NEED(n) while (bc < (n)) { \
        if (ip == iend) { if ((err = fill(z))) goto out; ip = z->ip; iend = z->iend; } \
        bb |= (unsigned long)*ip++ << bc; bc += 8; }
#define BITS(n) ((unsigned int)bb & ((1U << (n)) - 1))
#define DROP(n) (bb >>= (n), bc -= (n))
#define BAD { err = ARC_ECORRUPT; goto out; }
#define DECODE(h, v) { NEED(15); t = (h)->fast[(unsigned int)bb & FMASK]; \
        if (t) { DROP(t & 15); v = t >> 4; } \
        else { if ((v = slow(h, bb, &t)) < 0) BAD; DROP(t); } }
#define FLUSH { if ((err = flush(z, wp))) goto out; wp = 0; }

static const unsigned char order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

static int inflate(struct inf *z)
{
    unsigned char *win = z->win, *d, *q;
    const unsigned char *ip = z->ip, *iend = z->iend;
    unsigned long bb = 0, total = 0, left;
    unsigned int wp = 0, src, n, len, dist;
    int bc = 0, err = ARC_OK, last, type, t, sym, i, nlit, ndst;
    do {
        NEED(3);
        last = bb & 1;
        type = BITS(3) >> 1;
        DROP(3);
        if (type == 0) {
            /* stored: to a byte boundary, LEN, ~LEN, the bytes */
            DROP(bc & 7);
            NEED(16);
            len = BITS(16);
            DROP(16);
            NEED(16);
            if (BITS(16) != (~len & 0xFFFF)) BAD;
            DROP(16);
            if (len > z->size - total) BAD;
            total += len;
            for (; len && bc; len--) {
                win[wp++] = (unsigned char)bb;
                DROP(8);
                if (wp == WSIZE) FLUSH;
            }
            while (len) {
                if (ip == iend) {
                    if (!z->inleft) BAD;
                    if ((err = fill(z))) goto out;
                    ip = z->ip;
                    iend = z->iend;
                }
                n = iend - ip;
                if (n > len) n = len;
                if (n > WSIZE - wp) n = WSIZE - wp;
                len -= n;
                for (d = win + wp, wp += n; n; n--) *d++ = *ip++;
                if (wp == WSIZE) FLUSH;
            }
            continue;
        }
        if (type == 1) {
            if (!z->fixed) {
                for (i = 0; i < 288; i++) z->len[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                for (i = 0; i < 30; i++) z->len[288 + i] = 5;
                build(&z->lit, z->len, 288);
                build(&z->dst, z->len + 288, 30);
                z->fixed = 1;
            }
        } else if (type == 2) {
            /* dynamic: the code length code, then the lengths with it */
            z->fixed = 0;
            NEED(14);
            nlit = BITS(5) + 257;
            DROP(5);
            ndst = BITS(5) + 1;
            DROP(5);
            n = BITS(4) + 4;
            DROP(4);
            if (nlit > 286 || ndst > 30) BAD;
            for (i = 0; i < 19; i++) z->len[order[i]] = 0;
            for (i = 0; i < (int)n; i++) {
                NEED(3);
                z->len[order[i]] = BITS(3);
                DROP(3);
            }
            if ((err = build(&z->lit, z->len, 19))) goto out;
            for (i = 0; i < nlit + ndst;) {
                DECODE(&z->lit, sym);
                if (sym < 16) {
                    z->len[i++] = sym;
                    continue;
                }
                NEED(7);
                if (sym == 16) {
                    if (!i) BAD;
                    t = z->len[i - 1];
                    n = 3 + BITS(2);
                    DROP(2);
                } else if (sym == 17) {
                    t = 0;
                    n = 3 + BITS(3);
                    DROP(3);
                } else {
                    t = 0;
                    n = 11 + BITS(7);
                    DROP(7);
                }
                if (i + (int)n > nlit + ndst) BAD;
                while (n--) z->len[i++] = t;
            }
            if (!z->len[256]) BAD;
            if ((err = build(&z->lit, z->len, nlit))) goto out;
            if ((err = build(&z->dst, z->len + nlit, ndst))) goto out;
        } else
            BAD;
        /* the codes: literal, end of block, or length + distance */
        for (;;) {
            DECODE(&z->lit, sym);
            if (sym < 256) {
                if (total == z->size) BAD;
                total++;
                win[wp++] = sym;
                if (wp == WSIZE) FLUSH;
                continue;
            }
            if (sym == 256) break;
            sym -= 257;
            if (sym >= 29) BAD;
            len = z->lbase[sym];
            if ((t = z->lext[sym]) != 0) {
                NEED(5);
                len += BITS(t);
                DROP(t);
            }
            DECODE(&z->dst, sym);
            if (sym >= 30) BAD;
            dist = z->dbase[sym];
            if ((t = z->dext[sym]) != 0) {
                NEED(13);
                dist += BITS(t);
                DROP(t);
            }
            if (dist > total || len > z->size - total) BAD;
            total += len;
            src = (wp - dist) & WMASK;
            while (len) {
                n = len;
                if (n > WSIZE - wp) n = WSIZE - wp;
                if (n > WSIZE - src) n = WSIZE - src;
                len -= n;
                d = win + wp;
                q = win + src;
                wp += n;
                src = (src + n) & WMASK;
                while (n--) *d++ = *q++;
                if (wp == WSIZE) FLUSH;
            }
        }
    } while (!last);
    FLUSH;
    /* a zero fed past the end and used, or short: damage */
    left = z->size - total;
    if (z->phantom * 8 > bc || left) err = ARC_ECORRUPT;
out:
    return err;
}

/* ---- extract ------------------------------------------------------ */

int zip_extract(struct arcsrc *s, const struct arcent *e, struct arcsink *out)
{
    unsigned char h[30];
    unsigned long off;
    struct inf *z;
    int err, i, b, x;
    if (e->isdir) return ARC_OK;
    if ((e->aux2 & 0x41) || (e->method != 0 && e->method != 8)) return ARC_EUNSUP;
    if ((err = rdx(s, e->dataoff, h, 30))) return err;
    if (g32(h) != SIG_LOCAL) return ARC_ECORRUPT;
    off = e->dataoff + 30 + g16(h + 26) + g16(h + 28);
    if (off > s->size || e->packed > s->size - off) return ARC_ECORRUPT;
    if (e->method == 0 && e->packed != e->size) return ARC_ECORRUPT;
    if (!(z = arc_alloc(sizeof(*z)))) return ARC_ENOMEM;
    z->s = s;
    z->out = out;
    z->inoff = off;
    z->inleft = e->packed;
    z->size = e->size;
    z->ip = z->iend = z->ib;
    if (e->method == 0) {
        /* stored: straight through the input buffer */
        err = ARC_OK;
        while (!err && z->inleft) {
            if (!(err = fill(z))) {
                z->crc = arc_crc32(z->crc, z->ib, z->iend - z->ib);
                if (!out->write(out->ctx, z->ib, z->iend - z->ib)) err = ARC_ESTOP;
            }
        }
    } else {
        /* the length and distance bases: each step adds 1 << extra */
        for (i = 0, b = 3; i < 29; i++) {
            x = i < 8 || i == 28 ? 0 : (i - 4) >> 2;
            z->lbase[i] = i == 28 ? 258 : b;
            z->lext[i] = x;
            b += 1 << x;
        }
        for (i = 0, b = 1; i < 30; i++) {
            x = i < 4 ? 0 : (i - 2) >> 1;
            z->dbase[i] = b;
            z->dext[i] = x;
            b += 1 << x;
        }
        err = inflate(z);
    }
    if (!err && z->crc != e->aux1) err = ARC_ECRC;
    arc_free(z);
    return err;
}
