// SPDX-License-Identifier: GPL-2.0-only
/* The LZX reader (DESIGN D33, docs/d33-archives.md, whose section on the format says how each fact was established: by
 * experiments on what the real LZX 1.21 writes, read as data, never from another program's source). The window, 64 KB, is
 * the only large block. */
#include "arcread.h"

#define WIN 65536u
#define WMASK (WIN - 1)
#define MAINSYMS 768
#define HDR 31
#define KMAIN 10
#define KPRE 7
#define KALN 7
#define INBUF 4096

struct lz {
    arc_u8 win[WIN];
    arc_u8 lens[MAINSYMS], aln[8], pre[20];
    arc_u16 maintab[1 << KMAIN], mainsyms[MAINSYMS];
    arc_u16 pretab[1 << KPRE], presyms[20];
    arc_u16 alntab[1 << KALN], alnsyms[8];
    struct hf hmain, hpre, haln;
    const struct arcsrc *src;
    unsigned long inoff, inend;         /* the stream's next byte in the archive and its end */
    arc_u8 in[INBUF];
    int inpos, inlen, past;             /* past: words given after the end of the stream, as zeros */
    arc_u32 bb;                         /* the next bits, lowest first */
    int nb;
    /* where the stream has come to */
    unsigned long group;                /* its first byte in the archive: which group this is */
    unsigned long total, done;          /* the bytes made, the bytes handed on or passed over */
    long left;                          /* the bytes left of the block */
    arc_u32 lastd;
    int type, err;
    /* a group whose stream failed: its later members answer the same at once. Decoding from its start again for each
     * would make a small damaged archive cost members x the bytes before the damage. */
    unsigned long badgroup;
    int badrc;
};
static struct lz *L;

static const arc_u16 sbase[32] = { 0, 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536,
                                   2048, 3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152 };
static const arc_u8 lbits[16] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6 };
static const arc_u16 lbase[16] = { 3, 4, 5, 6, 7, 9, 11, 15, 19, 27, 35, 51, 67, 99, 131, 195 };
#define SBITS(s) ((s) < 4 ? 0 : ((s) - 2) >> 1)

static long src_read(const struct arcsrc *s, unsigned long off, void *buf, long n)
{
    long r;
    if (off >= s->size) return 0;
    if ((unsigned long)n > s->size - off) n = (long)(s->size - off);
    r = s->read(s->ctx, off, buf, n);
    return r;
}

static unsigned next16(struct lz *z)
{
    unsigned w;
    if (z->inpos + 2 > z->inlen) {
        long want = INBUF, r;
        int keep = z->inlen - z->inpos, i;
        for (i = 0; i < keep; i++) z->in[i] = z->in[z->inpos + (unsigned)i];
        z->inpos = 0;
        z->inlen = keep;
        if (z->inoff < z->inend) {
            if ((unsigned long)(want - keep) > z->inend - z->inoff) want = (long)(z->inend - z->inoff) + keep;
            r = src_read(z->src, z->inoff, z->in + keep, want - keep);
            if (r < 0) { z->err = ARC_EREAD; r = 0; }
            z->inoff += (unsigned long)r;
            z->inlen = keep + (int)r;
        }
        if (z->inlen < 2) {                 /* the end: zeros, counted; the decoder stops soon after */
            z->past++;
            z->inlen = z->inpos = 0;
            return 0;
        }
    }
    w = (unsigned)z->in[z->inpos] << 8 | z->in[z->inpos + 1];
    z->inpos += 2;
    return w;
}

#define NEED(z) do { if ((z)->nb < 16) { (z)->bb |= (arc_u32)next16(z) << (z)->nb; (z)->nb += 16; } } while (0)
static inline unsigned getn(struct lz *z, int k)    /* k <= 16 */
{
    unsigned v;
    NEED(z);
    v = z->bb & ((1u << k) - 1);
    z->bb >>= k;
    z->nb -= k;
    return v;
}
/* one code; -1: none such */
static inline int dec(struct lz *z, const struct hf *h)
{
    unsigned e;
    int l, s;
    NEED(z);
    e = h->tab[z->bb & ((1u << h->kbits) - 1)];
    if (e) {
        l = (int)(e >> 12);
        z->bb >>= l;
        z->nb -= l;
        return (int)(e & 0xFFF);
    }
    s = hf_long(h, arc_rev16(z->bb & 0xFFFF), &l);
    if (s < 0) return -1;
    z->bb >>= l;
    z->nb -= l;
    return s;
}

/* the lengths of lens[a..e), each half with a table of its own and its own runs (the format section of the note) */
static int rdlens(struct lz *z, int a, int e, int second)
{
    int i = a, r, k;
    for (k = 0; k < 20; k++) z->pre[k] = (arc_u8)getn(z, 4);
    if ((r = hf_build(&z->hpre, z->pre, 20))) return r;
    while (i < e) {
        int s = dec(z, &z->hpre), n;
        if (s < 0 || z->past > 2) return ARC_ECORRUPT;
        if (s < 17) {
            z->lens[i] = (arc_u8)((z->lens[i] + 17 - s) % 17);
            i++;
        } else if (s == 19) {
            int s2, v;
            n = (int)getn(z, 1) + (second ? 3 : 4);
            s2 = dec(z, &z->hpre);
            if (s2 < 0 || s2 > 16) return ARC_ECORRUPT;
            v = (z->lens[i] + 17 - s2) % 17;
            while (n-- > 0 && i < e) z->lens[i++] = (arc_u8)v;
        } else {
            if (s == 17) n = (int)getn(z, 4) + (second ? 3 : 4);
            else n = second ? (int)getn(z, 6) + 19 : (int)getn(z, 5) + 20;
            while (n-- > 0 && i < e) z->lens[i++] = 0;
        }
    }
    return 0;
}

static int blockhead(struct lz *z)
{
    int k, r;
    unsigned sz;
    z->type = (int)getn(z, 3);
    if (z->past > 2) return ARC_ECORRUPT;
    if (z->type == 1) return ARC_EUNSUP;            /* never seen; the note says why this is no guess */
    if (z->type != 2 && z->type != 3) return ARC_ECORRUPT;
    if (z->type == 3) {
        for (k = 0; k < 8; k++) z->aln[k] = (arc_u8)getn(z, 3);
        if ((r = hf_build(&z->haln, z->aln, 8))) return r;
    }
    sz = getn(z, 8) << 16;
    sz |= getn(z, 8) << 8;
    sz |= getn(z, 8);
    z->left = (long)sz;
    if ((r = rdlens(z, 0, 256, 0)) || (r = rdlens(z, 256, MAINSYMS, 1))) return r;
    return hf_build(&z->hmain, z->lens, MAINSYMS);
}

/* decode until `upto` bytes exist or the ring holds all it may without losing what is not handed on yet */
static int run(struct lz *z, unsigned long upto)
{
    unsigned long total = z->total, room = z->done + WIN - 300;
    if (upto > room) upto = room;
    while (total < upto) {
        int s;
        if (z->left <= 0) {
            int r;
            z->total = total;
            if ((r = blockhead(z))) return r;
            if (z->err) return z->err;
            continue;
        }
        s = dec(z, &z->hmain);
        if (s < 0 || z->past > 2) { z->total = total; return ARC_ECORRUPT; }
        if (s < 256) {
            z->win[total & WMASK] = (arc_u8)s;
            total++;
            z->left--;
        } else {
            unsigned m = (unsigned)s - 256, sl = m & 31, h = m >> 5, d, n;
            if (sl == 0) d = z->lastd;
            else {
                int eb = SBITS(sl);
                unsigned v;
                if (z->type == 3 && eb >= 3) {
                    int a;
                    v = getn(z, eb - 3) << 3;
                    a = dec(z, &z->haln);
                    if (a < 0) { z->total = total; return ARC_ECORRUPT; }
                    v |= (unsigned)a;
                } else v = eb ? getn(z, eb) : 0;
                d = sbase[sl] + v;
                z->lastd = d;
            }
            n = lbase[h] + (lbits[h] ? getn(z, lbits[h]) : 0);
            /* A distance before the group's first byte reads the window as it starts, all zeros: LZX archives of the older header
             * (4C 5A 58 00 00 00 0A 00 00 00) do use it, found on real ones (the note's format section). */
            if (d == 0 || d >= WIN) { z->total = total; return ARC_ECORRUPT; }
            z->left -= (long)n;
            {
                unsigned long t = total;
                unsigned from = (unsigned)(t - d);
                total = t + n;
                while (n--) { z->win[t & WMASK] = z->win[from & WMASK]; t++; from++; }
            }
        }
    }
    z->total = total;
    return z->err;
}

static int zstart(struct lz *z, const struct arcsrc *src, unsigned long group, unsigned long packed)
{
    int k;
    z->src = src;
    z->group = group;
    z->inoff = group;
    z->inend = group + packed;
    z->inpos = z->inlen = z->past = 0;
    z->bb = 0;
    z->nb = 0;
    z->total = z->done = 0;
    z->left = 0;
    z->lastd = 1;
    z->err = 0;
    for (k = 0; k < MAINSYMS; k++) z->lens[k] = 0;
    for (k = 0; k < (int)WIN; k++) z->win[k] = 0;
    z->hmain.tab = z->maintab; z->hmain.syms = z->mainsyms; z->hmain.kbits = KMAIN; z->hmain.rev = 1;
    z->hpre.tab = z->pretab; z->hpre.syms = z->presyms; z->hpre.kbits = KPRE; z->hpre.rev = 1;
    z->haln.tab = z->alntab; z->haln.syms = z->alnsyms; z->haln.kbits = KALN; z->haln.rev = 1;
    return 0;
}

void lzx_end(void)
{
    if (L) { arc_free(L); L = 0; }
}

static int stored(const struct arcsrc *src, const struct arcent *e, const struct arcsink *out, arc_u32 *crc)
{
    unsigned long left = e->size, off = e->dataoff + e->aux1;
    arc_u8 *b = arc_alloc(INBUF);
    int rc = 0;
    if (!b) return ARC_ENOMEM;
    while (left && !rc) {
        long n = left > INBUF ? INBUF : (long)left, r = src_read(src, off, b, n);
        if (r < 0) rc = ARC_EREAD;
        else if (r != n) rc = ARC_ECORRUPT;
        else {
            *crc = (arc_u32)arc_crc32(*crc, b, n);
            if (out && !out->write(out->ctx, b, n)) rc = ARC_ESTOP;
            off += (unsigned long)n;
            left -= (unsigned long)n;
        }
    }
    arc_free(b);
    return rc;
}

int lzx_extract(const struct arcsrc *src, const struct arcent *e, const struct arcsink *out)
{
    arc_u32 crc = 0;
    int rc = 0;
    unsigned long start, end;
    if (e->method == AM_DIR) return 0;
    if (e->method == AM_REFUSED) return ARC_ECORRUPT;
    if (e->method != 0 && e->method != 2) return ARC_EUNSUP;
    if (!arc_crcready()) { (void)arc_crc32(0, "", 0); if (!arc_crcready()) return ARC_ENOMEM; }
    if (e->method == 0) {
        if (e->aux1 > e->aux3 || e->size > e->aux3 - e->aux1) return ARC_ECORRUPT;     /* inside its group, or itself */
        if ((rc = stored(src, e, out, &crc))) return rc;
        return crc == e->aux2 ? 0 : ARC_ECRC;
    }
    start = e->aux1;
    end = start + e->size;
    if (!L) {
        L = arc_alloc(sizeof *L);
        if (!L) return ARC_ENOMEM;
        L->group = (unsigned long)-1;
    }
    if (L->src == src && L->badgroup == e->dataoff && L->badrc) return L->badrc;
    if (L->group != e->dataoff || L->src != src || L->done > start || L->err) {
        L->badrc = 0;
        L->badgroup = 0;
        zstart(L, src, e->dataoff, e->aux3);
    }
    while (L->done < end) {
        unsigned long a, b;
        if (L->total <= L->done && (rc = run(L, end))) {
            L->group = (unsigned long)-1;
            L->badgroup = e->dataoff;
            L->badrc = rc;
            return rc;
        }
        a = L->done;
        b = L->total < end ? L->total : end;
        if (a < start) a = start < b ? start : b;
        while (a < b) {             /* the part in this member, a piece at a time round the ring */
            unsigned p = (unsigned)(a & WMASK);
            unsigned long n = b - a;
            if (n > WIN - p) n = WIN - p;
            crc = (arc_u32)arc_crc32(crc, L->win + p, (long)n);
            if (out && !out->write(out->ctx, L->win + p, (long)n)) { L->group = (unsigned long)-1; return ARC_ESTOP; }
            a += n;
        }
        L->done = b;
    }
    return crc == e->aux2 ? 0 : ARC_ECRC;
}

/* ---- the list ---- */
static arc_u32 le32(const arc_u8 *p) { return p[0] | (arc_u32)p[1] << 8 | (arc_u32)p[2] << 16 | (arc_u32)p[3] << 24; }

/* The year field: 8-29 are 1978-1999; LZX's year-2000 patch adds 58-63 = 2000-2005, 30-57 = 2006-2033, 0-7 = 2034-2041 (read
 * from an archive made for that purpose, the note's format section). */
static void lzx_date(arc_u32 v, struct arcent *e)
{
    int day = (int)(v >> 27), mon = (int)((v >> 23) & 15), y = (int)((v >> 17) & 63);
    int h = (int)((v >> 12) & 31), mi = (int)((v >> 6) & 63), s = (int)(v & 63), year;
    if (y < 8) year = 2034 + y;
    else if (y < 30) year = 1970 + y;
    else if (y < 58) year = 1976 + y;
    else year = 1942 + y;
    if (day < 1 || mon > 11 || h > 23 || mi > 59 || s > 59) return;
    arc_datestamp(year, mon + 1, day, h, mi, s, &e->days, &e->mins, &e->ticks);
}

/* the header at off in h (531+ bytes), checked. Returns its whole length, 0 when off is the end, < 0 an error */
static long rdhdr(const struct arcsrc *src, unsigned long off, arc_u8 *h)
{
    long r;
    int nl, cl;
    arc_u32 want, crc;
    if (off == src->size) return 0;
    r = src_read(src, off, h, HDR);
    if (r < 0) return ARC_EREAD;
    if (r < HDR) return ARC_ECORRUPT;
    nl = h[30];
    cl = h[14];
    if (nl + cl) {
        r = src_read(src, off + HDR, h + HDR, nl + cl);
        if (r < 0) return ARC_EREAD;
        if (r != nl + cl) return ARC_ECORRUPT;
    }
    want = le32(h + 26);
    h[26] = h[27] = h[28] = h[29] = 0;
    crc = (arc_u32)arc_crc32(0, h, HDR + nl + cl);
    if (crc != want) return ARC_ECORRUPT;
    return HDR + nl + cl;
}

static int emit(const arc_u8 *h, unsigned long data, unsigned long inside, unsigned long gpacked, unsigned long shown,
                struct arcent *e, arc_addfn add, void *ctx)
{
    int nl = h[30], cl = h[14], bad, k, attr = h[0];
    { char *c = (char *)e; unsigned k2; for (k2 = 0; k2 < sizeof *e; k2++) c[k2] = 0; }
    e->size = le32(h + 2);
    e->packed = shown;
    e->dataoff = data;
    e->aux1 = inside;
    e->aux2 = le32(h + 22);
    e->aux3 = gpacked;
    e->method = h[11] == 0 || h[11] == 2 ? h[11] : AM_OTHER;
    /* the attribute byte: r 1, w 2, d 4, e 8, a 16, h 32, s 64, p 128. arcent.prot is AmigaDOS's: rwed set = denied */
    e->prot = (attr & 1 ? 0 : 8) | (attr & 2 ? 0 : 4) | (attr & 8 ? 0 : 2) | (attr & 4 ? 0 : 1) |
              (attr & 16 ? 16 : 0) | (attr & 128 ? 32 : 0) | (attr & 64 ? 64 : 0) | (attr & 32 ? 128 : 0);
    lzx_date((arc_u32)h[18] << 24 | (arc_u32)h[19] << 16 | (arc_u32)h[20] << 8 | h[21], e);
    if (nl > 0 && h[HDR + nl - 1] == '/') {
        e->isdir = 1;
        e->method = AM_DIR;
    }
    bad = arc_fixname(h + HDR, nl, e->name, sizeof e->name);
    if (bad && e->isdir && !e->name[0] && h[HDR] != '/') return 0;     /* "./": this folder, nothing to do */
    if (bad) {
        e->method = AM_REFUSED;
        if (!arc_list_refused) return ARC_ECORRUPT;
    }
    for (k = 0; k < cl && k + 1 < (int)sizeof e->comment; k++) {
        unsigned c = h[HDR + nl + k];
        e->comment[k] = c < 0x20 || c >= 0x7F ? '_' : (char)c;
    }
    return add(ctx, e) ? 0 : ARC_ESTOP;
}

int lzx_list(const struct arcsrc *src, arc_addfn add, void *ctx)
{
    unsigned long p = 10;
    int rc = 0;
    arc_u8 *h;
    struct arcent *e;
    (void)arc_crc32(0, "", 0);
    if (!arc_crcready()) return ARC_ENOMEM;
    if (src->size < 10) return ARC_ECORRUPT;
    h = arc_alloc(HDR + 512);
    e = arc_alloc(sizeof *e);
    if (!h || !e) { rc = ARC_ENOMEM; goto out; }
    if (src_read(src, 0, h, 10) != 10 || h[0] != 'L' || h[1] != 'Z' || h[2] != 'X') { rc = ARC_ECORRUPT; goto out; }
    while (!rc) {
        long n = rdhdr(src, p, h);
        unsigned long size, pack, d;
        if (n == 0) break;
        if (n < 0) { rc = (int)n; break; }
        size = le32(h + 2);
        pack = le32(h + 6);
        d = p + (unsigned long)n;
        if ((h[12] & 1) && pack == 0) {
            /* a merged group: its members' headers, then one stream after the last. Walk to it, then list them. */
            unsigned long q = d, tot = size, gp = 0, stream;
            for (;;) {
                long m = rdhdr(src, q, h);
                unsigned long sz;
                if (m <= 0) { rc = m ? (int)m : ARC_ECORRUPT; break; }
                sz = le32(h + 2);
                if (tot + sz < tot) { rc = ARC_ECORRUPT; break; }
                tot += sz;
                q += (unsigned long)m;
                gp = le32(h + 6);
                if (gp) break;
                if (!(h[12] & 1)) { rc = ARC_ECORRUPT; break; }
            }
            if (rc) break;
            stream = q;
            if (gp > src->size - stream || stream > src->size) { rc = ARC_ECORRUPT; break; }
            {
                unsigned long r = p, inside = 0;
                while (r < stream && !rc) {
                    long m = rdhdr(src, r, h);
                    if (m <= 0) { rc = m ? (int)m : ARC_ECORRUPT; break; }
                    r += (unsigned long)m;
                    rc = emit(h, stream, inside, gp, r >= stream ? gp : 0, e, add, ctx);
                    inside += le32(h + 2);
                }
            }
            p = stream + gp;
            continue;
        }
        if (pack > src->size || d > src->size - pack) { rc = ARC_ECORRUPT; break; }
        if (h[11] == 0 && pack != size && !(h[30] && h[HDR + h[30] - 1] == '/')) { rc = ARC_ECORRUPT; break; }
        rc = emit(h, d, 0, pack, pack, e, add, ctx);
        p = d + pack;
    }
out:
    if (h) arc_free(h);
    if (e) arc_free(e);
    return rc == ARC_ESTOP ? 0 : rc;
}
