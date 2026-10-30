/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* lzx - see lzx.h. No Amiga headers here, on purpose.
 *
 * The archive: a 10-byte info header ("LZX"...), then members, each a
 * 31-byte header + name + filenote. Members whose header says 0 packed
 * bytes share the packed data of the next member that has some (a
 * "merged" group): that data follows its header. Numbers are little-
 * endian except the date. The header CRC-32 covers header (CRC field
 * zeroed), name and note.
 *
 * The packed stream (mode 2) is LZ77 over a 64K window with Huffman
 * codes, in blocks that each start with their code tables. The bit
 * reader takes 16 bits at a time, low bit first, as unlzx does it.
 */
#include "lzx.h"

#define HDRLEN 31
#define HDRMAX (HDRLEN + 255 + 255)

#define WIN 65536UL
#define BUFTOP (258 + WIN)      /* the window, and 258 bytes it wraps into */
#define BUFLEN (BUFTOP + 512)   /* room for a match started just before BUFTOP */
#define INSZ 8192               /* packed bytes read at a time (even) */
#define PAD 32                  /* zeros made up past the end of the packed data */

static unsigned long le32(const unsigned char *p)
{
    return p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

static unsigned long be32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}

/* ---- listing ----------------------------------------------------- */

/* the member header at off into h (HDRMAX bytes): its whole length, 0
 * when there is no valid header there (the end, damage), ARC_EREAD */
static long gethdr(struct arcsrc *s, unsigned long off, unsigned char *h)
{
    unsigned char c[4];
    long n, len;
    int i;
    if (off >= s->size || s->size - off < HDRLEN) return 0;
    n = s->size - off < HDRMAX ? (long)(s->size - off) : HDRMAX;
    if ((n = s->read(s->ctx, off, h, n)) < 0) return ARC_EREAD;
    if (n < HDRLEN) return 0;
    len = HDRLEN + h[30] + h[14];
    if (len > n) return 0;
    for (i = 0; i < 4; i++) {
        c[i] = h[26 + i];
        h[26 + i] = 0;
    }
    n = arc_crc32(0, h, len) == le32(c);
    for (i = 0; i < 4; i++) h[26 + i] = c[i];
    return n ? len : 0;
}

/* the arcent of the header in h (name, note, size, date, bits) */
static void fillent(struct arcent *e, const unsigned char *h)
{
    const unsigned char *p = h + HDRLEN;
    unsigned long v;
    int n, i, a, y, mo, d, hr, mi, sc;

    n = h[30];
    for (i = 0; i < n && p[i]; i++) e->name[i] = p[i];
    e->isdir = 0;
    while (i > 0 && e->name[i - 1] == '/') {    /* "dir/": a directory LZX stored */
        i--;
        e->isdir = 1;
    }
    e->name[i] = 0;
    p += n;
    n = h[14];
    for (i = 0; i < n && i < (int)sizeof e->comment - 1 && p[i]; i++) e->comment[i] = p[i];
    e->comment[i] = 0;

    e->size = le32(h + 2);
    e->packed = le32(h + 6);

    /* day 5 bits, month 4 (from 0), year 6, hour 5, minute 6, second 6 */
    e->days = e->mins = e->ticks = 0;
    if ((v = be32(h + 18))) {
        sc = (int)(v & 63);
        mi = (int)(v >> 6 & 63);
        hr = (int)(v >> 12 & 31);
        y = (int)(v >> 17 & 63);
        mo = (int)(v >> 23 & 15) + 1;
        d = (int)(v >> 27);
        y = y >= 58 ? 1942 + y : y < 8 ? 2006 + y : 1970 + y;    /* see lzx.h */
        if (mo <= 12 && d >= 1 && hr < 24 && mi < 60 && sc < 60)
            arc_datestamp(y, mo, d, hr, mi, sc, &e->days, &e->mins, &e->ticks);
    }

    /* LZX: 1 r, 2 w, 4 d, 8 e set = allowed; 16 a, 32 h, 64 s, 128 p.
     * AmigaDOS: d e w r in bits 0-3 set = NOT allowed; a p s h in 4-7. */
    a = h[0];
    e->prot = (a & 4 ? 0 : 1) | (a & 8 ? 0 : 2) | (a & 2 ? 0 : 4) | (a & 1 ? 0 : 8)
            | (a & 16) | (a & 128 ? 32 : 0) | (a & 64) | (a & 32 ? 128 : 0);
}

int lzx_list(struct arcsrc *s, arc_addfn add, void *ctx)
{
    struct arcent *e;
    unsigned char *h;
    unsigned long off, end, run, upos, gpacked;
    long n;
    int r = ARC_OK, mode = 0, empty;

    if (!(e = arc_alloc(sizeof *e + HDRMAX))) return ARC_ENOMEM;
    h = (unsigned char *)(e + 1);
    n = s->size >= 10 ? s->read(s->ctx, 0, h, 10) : 0;
    if (n < 0) r = ARC_EREAD;
    else if (n < 10 || h[0] != 'L' || h[1] != 'Z' || h[2] != 'X') r = ARC_ECORRUPT;
    off = 10;
    while (!r && off < s->size) {
        /* a run of members: up to the first with packed data, which
         * the data of them all follows (most runs are one member) */
        end = off;
        gpacked = 0;
        while ((n = gethdr(s, end, h)) > 0) {
            end += n;
            if ((gpacked = le32(h + 6))) {
                mode = h[11];
                break;
            }
        }
        if (n < 0) {
            r = (int)n;
            break;
        }
        if (end == off) {           /* not a header where one must be */
            r = ARC_ECORRUPT;
            break;
        }
        /* the members again, now that the run's data is known */
        upos = 0;
        empty = 1;
        for (run = off; run < end; run += n) {
            if ((n = gethdr(s, run, h)) <= 0) {
                r = n ? (int)n : ARC_ECORRUPT;
                goto done;
            }
            fillent(e, h);
            e->dataoff = gpacked ? end : 0;
            e->aux1 = gpacked;
            e->aux2 = upos;
            e->aux3 = le32(h + 22);
            e->method = gpacked ? mode : h[11];
            upos += e->size;
            if (e->size) empty = 0;
            if (!add(ctx, e)) goto done;
        }
        if (!gpacked) {
            /* the headers ended with no data after them: right only for
             * empty members (directories) at the very end */
            if (end != s->size || !empty) r = ARC_ECORRUPT;
            break;
        }
        if (gpacked > s->size - end) r = ARC_ECORRUPT;     /* cut short */
        off = end + gpacked;
    }
done:
    arc_free(e);
    return r;
}

/* ---- the decoder --------------------------------------------------- */

static const unsigned char tone[32] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14
};
static const unsigned short ttwo[32] = {
    0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192,
    256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152
};
static const unsigned short tthree[16] = {
    0, 1, 3, 7, 15, 31, 63, 127, 255, 511, 1023, 2047, 4095, 8191, 16383, 32767
};
static const unsigned char tfour[34] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
};

struct dec {
    struct arcsrc *s;
    unsigned long inoff;        /* where the next packed bytes are read */
    unsigned long inleft;       /* packed bytes not read yet */
    unsigned long over;         /* zeros made up past the end */
    int err;
    const unsigned char *sp, *se;       /* the packed bytes in in[] */
    unsigned long control;      /* the bit reader: bits, low first */
    int shift;
    unsigned long lastoff;
    long length;                /* bytes left in this block */
    int method, havelit;
    unsigned char offlen[8], h20len[20], litlen[768];
    unsigned short offtab[128], h20tab[256], littab[8192];
    unsigned char in[INSZ + PAD + 2];
    unsigned char buf[BUFLEN];
    /* kept between members of one group, so the next one carries on */
    unsigned long group;        /* the group's dataoff */
    unsigned long pos, dest;    /* buf: next byte out, end of what is made */
    unsigned long at;           /* the stream offset of buf[pos] */
};

/* the next packed bytes into in[]; past the end zeros, and an error
 * once the decoder has eaten more of them than a real stream can */
static void fillin(struct dec *d)
{
    long n = 0, want = INSZ, i;
    if (d->inleft) {
        if ((unsigned long)want > d->inleft) want = (long)d->inleft;
        n = d->s->read(d->s->ctx, d->inoff, d->in, want);
        if (n < 0) {
            d->err = ARC_EREAD;
            n = 0;
        }
        if (n < want) d->inleft = 0;        /* cut short: zeros from here on */
        else {
            d->inoff += n;
            d->inleft -= n;
        }
    }
    d->sp = d->in;
    d->se = d->in + n;
    if (!d->inleft) {
        i = (n + PAD + 1) & ~1L;
        d->over += i - n;
        if (d->over > 2 * PAD && !d->err) d->err = ARC_ECORRUPT;
        while (n < i) d->in[n++] = 0;
        d->se = d->in + n;
    }
}

/* the bit reader on locals (control, shift, sp, se), as unlzx: NEED
 * tops up 16 bits once fewer than 16 are left, DROP takes n bits,
 * DROP1 one (the long-code walks) */
#define NEED() \
    if (shift < 0) { \
        if (sp >= se) { d->sp = sp; fillin(d); sp = d->sp; se = d->se; } \
        shift += 16; \
        control += (unsigned long)sp[0] << (8 + shift); \
        control += (unsigned long)sp[1] << shift; \
        sp += 2; \
    }
#define DROP(n) { control >>= (n); shift -= (int)(n); NEED() }
#define DROP1() { \
    if (!shift) { \
        if (sp >= se) { d->sp = sp; fillin(d); sp = d->sp; se = d->se; } \
        shift = 15; \
        control += (unsigned long)sp[0] << 24; \
        control += (unsigned long)sp[1] << 16; \
        sp += 2; \
    } else shift--; \
    control >>= 1; }

#define LOAD() control = d->control; shift = d->shift; sp = d->sp; se = d->se
#define SAVE() d->control = control; d->shift = shift; d->sp = sp

static unsigned long rev(unsigned long v, int bits)
{
    unsigned long r = 0;
    while (bits--) {
        r = r << 1 | (v & 1);
        v >>= 1;
    }
    return r;
}

/* a decode table from code lengths: codes up to tbits long are looked
 * up directly, longer ones walk a tree stored past the direct part.
 * FALSE when the lengths are not one complete code (or would not fit) */
static int build(int nsym, int tbits, const unsigned char *len, unsigned short *tab, unsigned long tsize)
{
    unsigned long pos = 0, mask = 1UL << tbits, bmask = mask >> 1, leaf, step, fill, next;
    int bit, sym;

    for (bit = 1; bit <= tbits; bit++, bmask >>= 1)
        for (sym = 0; sym < nsym; sym++)
            if (len[sym] == bit) {
                leaf = rev(pos, tbits);
                if ((pos += bmask) > mask) return 0;
                for (step = 1UL << bit, fill = bmask; fill; fill--, leaf += step)
                    tab[leaf] = (unsigned short)sym;
            }
    if (pos == mask) return 1;
    for (leaf = pos; leaf < mask; leaf++) tab[rev(leaf, tbits)] = 0;
    next = mask >> 1;
    pos <<= 16;
    mask <<= 16;
    bmask = 32768;
    for (; bit <= 16; bit++, bmask >>= 1)
        for (sym = 0; sym < nsym; sym++)
            if (len[sym] == bit) {
                leaf = rev(pos >> 16, tbits);
                for (fill = 0; fill < (unsigned long)(bit - tbits); fill++) {
                    if (!tab[leaf]) {
                        if ((next << 1) + 1 >= tsize) return 0;
                        tab[next << 1] = tab[(next << 1) + 1] = 0;
                        tab[leaf] = (unsigned short)next++;
                    }
                    leaf = (unsigned long)tab[leaf] << 1 | (pos >> (15 - fill) & 1);
                    if (leaf >= tsize) return 0;
                }
                tab[leaf] = (unsigned short)sym;
                if ((pos += bmask) > mask) return 0;
            }
    return pos == mask;
}

/* the same bit reader as functions on d, for the block headers: they
 * are rare, so small beats fast there */
static void dropn(struct dec *d, int n)
{
    d->control >>= n;
    if ((d->shift -= n) < 0) {
        if (d->sp >= d->se) fillin(d);
        d->shift += 16;
        d->control += (unsigned long)d->sp[0] << (8 + d->shift);
        d->control += (unsigned long)d->sp[1] << d->shift;
        d->sp += 2;
    }
}

static void drop1(struct dec *d)
{
    if (!d->shift) {
        if (d->sp >= d->se) fillin(d);
        d->shift = 15;
        d->control += (unsigned long)d->sp[0] << 24;
        d->control += (unsigned long)d->sp[1] << 16;
        d->sp += 2;
    } else d->shift--;
    d->control >>= 1;
}

static int getb(struct dec *d, int n)
{
    int v = (int)(d->control & tthree[n]);
    dropn(d, n);
    return v;
}

/* one symbol of the 20-symbol code the literal lengths are sent in,
 * -1 when the walk does not end */
static int h20(struct dec *d)
{
    unsigned int sym = d->h20tab[d->control & 63];
    int k = 0;
    if (sym < 20) {
        dropn(d, d->h20len[sym]);
        return (int)sym;
    }
    do {
        sym = d->h20tab[(d->control >> 6 & 1) + (sym << 1)];
        drop1(d);
    } while (sym >= 20 && ++k < 16);
    if (sym >= 20) return -1;
    dropn(d, 6);
    return (int)sym;
}

/* a block header: method, length, code tables */
static int tables(struct dec *d)
{
    int i, n, fix, pos, max, sym, cnt, v;

    dropn(d, 0);
    d->method = getb(d, 3);
    if (d->method < 1 || d->method > 3) return ARC_ECORRUPT;     /* only 1, 2 and 3 exist */
    if (d->method == 3) {
        for (i = 0; i < 8; i++) d->offlen[i] = (unsigned char)getb(d, 3);
        if (!build(8, 7, d->offlen, d->offtab, 128)) return ARC_ECORRUPT;
    }
    d->length = (long)getb(d, 8) << 16;
    d->length += (long)getb(d, 8) << 8;
    d->length += getb(d, 8);
    if (d->method == 1) return d->havelit ? ARC_OK : ARC_ECORRUPT;    /* the last block's tables */
    pos = 0;
    fix = 1;
    for (max = 256; max <= 768; max += 512, fix--) {
        for (i = 0; i < 20; i++) d->h20len[i] = (unsigned char)getb(d, 4);
        if (!build(20, 6, d->h20len, d->h20tab, 256)) return ARC_ECORRUPT;
        while (pos < max) {
            if ((sym = h20(d)) < 0) return ARC_ECORRUPT;
            if (sym == 17 || sym == 18) {
                n = sym == 17 ? 4 : 6 - fix;
                cnt = (sym == 17 ? 3 : 19) + getb(d, n) + fix;
                for (; pos < max && cnt; cnt--) d->litlen[pos++] = 0;
            } else if (sym == 19) {
                cnt = (int)(d->control & 1) + 3 + fix;
                drop1(d);
                if ((sym = h20(d)) < 0 || sym > 16) return ARC_ECORRUPT;
                v = tfour[d->litlen[pos] + 17 - sym];
                for (; pos < max && cnt; cnt--) d->litlen[pos++] = (unsigned char)v;
            } else {
                d->litlen[pos] = tfour[d->litlen[pos] + 17 - sym];
                pos++;
            }
        }
    }
    if (!build(768, 12, d->litlen, d->littab, 8192)) return ARC_ECORRUPT;
    d->havelit = 1;
    return ARC_OK;
}

/* unpack into buf from dest until dest reaches end (a match may run
 * up to 321 bytes past it); returns the new dest */
static unsigned long block(struct dec *d, unsigned long dest, unsigned long end)
{
    unsigned char *buf = d->buf, *p, *q;
    const unsigned short *lt = d->littab;
    const unsigned char *ll = d->litlen;
    const unsigned char *sp, *se;
    unsigned long control, sym, cnt, n, t, last = d->lastoff;
    int shift, k, m3 = d->method == 3;

    LOAD();
    do {
        sym = lt[control & 4095];
        if (sym >= 768) {
            DROP(12)
            k = 0;
            do {
                sym = lt[(control & 1) + (sym << 1)];
                DROP1()
            } while (sym >= 768 && ++k < 8);
            if (sym >= 768) goto bad;
        } else DROP(ll[sym])
        if (sym < 256) {
            buf[dest++] = (unsigned char)sym;
            continue;
        }
        sym -= 256;
        t = sym & 31;
        cnt = ttwo[t];
        n = tone[t];
        if (n >= 3 && m3) {
            n -= 3;
            cnt += (control & tthree[n]) << 3;
            DROP(n)
            t = d->offtab[control & 127] & 7;
            cnt += t;
            n = d->offlen[t];
        } else {
            cnt += control & tthree[n];
            if (!cnt) cnt = last;
        }
        DROP(n)
        last = cnt;
        if (last > WIN) goto bad;
        t = sym >> 5 & 15;
        n = tone[t];
        cnt = ttwo[t] + 3 + (control & tthree[n]);
        DROP(n)
        q = buf + (last < dest ? dest - last : dest + WIN - last);
        p = buf + dest;
        dest += cnt;
        do *p++ = *q++; while (--cnt);
    } while (dest < end);
    SAVE();
    d->lastoff = last;
    return dest;
bad:
    SAVE();
    if (!d->err) d->err = ARC_ECORRUPT;
    return dest;
}

/* the group's stream from its start; bytes skip.. skip+want to out */
static int unpack(struct dec *d, unsigned long skip, unsigned long want, struct arcsink *out, unsigned long *crc)
{
    unsigned char *buf = d->buf;
    unsigned long pos = d->pos, dest = d->dest, end, n, i;
    int r = ARC_OK;

    while (want) {
        if (pos == dest) {
            if (d->err) {
                r = d->err;
                break;
            }
            if (d->length <= 0 && (r = tables(d))) break;
            if (dest >= BUFTOP) {   /* keep the last 64K: wrap the tail */
                n = dest - WIN;
                for (i = 0; i < n; i++) buf[i] = buf[WIN + i];
                pos = dest = n;
            }
            end = dest + (unsigned long)d->length;
            if (end > BUFTOP) end = BUFTOP;
            n = dest;
            dest = block(d, dest, end);
            d->length -= (long)(dest - n);
        }
        n = dest - pos;
        if (skip) {
            i = n < skip ? n : skip;
            skip -= i;
            pos += i;
            d->at += i;
            n -= i;
        }
        if (n > want) n = want;
        if (n) {
            *crc = arc_crc32(*crc, buf + pos, (long)n);
            pos += n;
            d->at += n;
            want -= n;
            if (!out->write(out->ctx, buf + pos - n, (long)n)) {
                r = ARC_ESTOP;
                break;
            }
        }
    }
    d->pos = pos;
    d->dest = dest;
    return r;
}

/* the decoder of the last group, while the program walks through it:
 * extracting member k+1 after member k then costs only its own bytes,
 * not the whole group again (150 small files: 1 pass, not 75) */
static struct dec *keep;

void lzx_end(void)
{
    if (keep) arc_free(keep);
    keep = 0;
}

/* ---- extract ------------------------------------------------------- */

#define COPYSZ 8192

int lzx_extract(struct arcsrc *s, const struct arcent *e, struct arcsink *out)
{
    unsigned long crc = 0, off, left;
    unsigned char *b;
    struct dec *d;
    long n;
    int r = ARC_OK;

    if (!e->size) return e->aux3 ? ARC_ECRC : ARC_OK;
    if (!e->aux1) return ARC_ECORRUPT;      /* listed, but no data was found */
    if (e->method == 0) {
        if (e->aux2 > e->aux1 || e->size > e->aux1 - e->aux2) return ARC_ECORRUPT;
        if (!(b = arc_alloc(COPYSZ))) return ARC_ENOMEM;
        off = e->dataoff + e->aux2;
        for (left = e->size; left && !r; left -= n, off += n) {
            n = left < COPYSZ ? (long)left : COPYSZ;
            if (s->read(s->ctx, off, b, n) != n)
                r = off > s->size || (unsigned long)n > s->size - off ? ARC_ECORRUPT : ARC_EREAD;
            else {
                crc = arc_crc32(crc, b, n);
                if (!out->write(out->ctx, b, n)) r = ARC_ESTOP;
            }
        }
        arc_free(b);
    } else if (e->method == 2) {
        d = keep;
        keep = 0;
        if (d && (d->s != s || d->group != e->dataoff || e->aux2 < d->at)) {
            arc_free(d);
            d = 0;
        }
        if (!d) {
            if (!(d = arc_alloc(sizeof *d))) return ARC_ENOMEM;
            d->s = s;
            d->inoff = e->dataoff;
            d->inleft = e->aux1;
            d->sp = d->se = d->in;
            d->shift = -16;
            d->lastoff = 1;
            d->group = e->dataoff;
            d->pos = d->dest = BUFTOP;
        }
        r = unpack(d, e->aux2 - d->at, e->size, out, &crc);
        if (r) arc_free(d);
        else keep = d;
    } else return ARC_EUNSUP;
    if (!r && crc != e->aux3) r = ARC_ECRC;
    return r;
}
