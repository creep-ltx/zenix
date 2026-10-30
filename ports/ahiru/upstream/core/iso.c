/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* iso - see iso.h. Every number is taken from the big-endian half of
 * the format's both-endian fields: the 68k half, so no swapping. */
#include "iso.h"

#define SEC 2048

static unsigned long b32(const unsigned char *b)
{
    return (unsigned long)b[0] << 24 | (unsigned long)b[1] << 16 | (unsigned long)b[2] << 8 | b[3];
}

/* one sector into v->buf; a sector past the end means a damaged image */
static int rdsec(struct isovol *v, unsigned long sec)
{
    long n;
    if (sec >= v->src->size / SEC) return ARC_ECORRUPT;
    n = v->src->read(v->src->ctx, sec * SEC, v->buf, SEC);
    if (n < 0) return ARC_EREAD;
    return n == SEC ? ARC_OK : ARC_ECORRUPT;
}

/* the root record of a volume descriptor in v->buf */
static void takeroot(struct isovol *v)
{
    v->root = b32(v->buf + 156 + 6);
    v->rootsz = b32(v->buf + 156 + 14);
}

/* does the root's "." record carry the SUSP "SP" mark? Then Rock Ridge */
static int hasrr(struct isovol *v)
{
    const unsigned char *b = v->buf;
    int len, il, o;
    if (rdsec(v, v->root)) return 0;
    len = b[0];
    il = b[32];
    if (len < 34 || 33 + il > len) return 0;
    o = 33 + il + !(il & 1);
    return o + 7 <= len && b[o] == 'S' && b[o + 1] == 'P' && b[o + 4] == 0xBE && b[o + 5] == 0xEF;
}

int iso_open(struct arcsrc *s, struct isovol *v)
{
    unsigned long sec, jroot = 0, jsz = 0;
    int i, found = 0, e;
    v->src = s;
    v->joliet = v->rr = 0;
    v->volname[0] = 0;
    v->cebuf = 0;
    if (!(v->buf = arc_alloc(SEC))) return ARC_ENOMEM;
    /* the descriptor set starts at 16; a bounded scan is plenty */
    for (sec = 16; sec < 48; sec++) {
        const unsigned char *b = v->buf;
        if ((e = rdsec(v, sec))) break;
        if (b[1] != 'C' || b[2] != 'D' || b[3] != '0' || b[4] != '0' || b[5] != '1') break;
        if (b[0] == 255) break;
        if ((b[130] << 8 | b[131]) != SEC) continue;        /* only 2048-byte blocks */
        if (b[0] == 1 && !found) {
            takeroot(v);
            for (i = 0; i < 32; i++) v->volname[i] = b[40 + i];
            for (i = 32; i > 0 && v->volname[i - 1] == ' '; i--)
                ;
            v->volname[i] = 0;
            found = 1;
        } else if (b[0] == 2 && b[88] == '%' && b[89] == '/' && (b[90] == '@' || b[90] == 'C' || b[90] == 'E')) {
            jroot = b32(b + 156 + 6);
            jsz = b32(b + 156 + 14);
        }
    }
    if (!found) {
        arc_free(v->buf);
        v->buf = 0;
        return ARC_EUNSUP;
    }
    /* Rock Ridge keeps case and long names on the main tree; else Joliet */
    if (hasrr(v)) v->rr = 1;
    else if (jroot) {
        v->root = jroot;
        v->rootsz = jsz;
        v->joliet = 1;
    }
    return ARC_OK;
}

void iso_close(struct isovol *v)
{
    if (v->buf) arc_free(v->buf);
    if (v->cebuf) arc_free(v->cebuf);
    v->buf = v->cebuf = 0;
}

/* the Rock Ridge name in the system use area, or 0 when there is none.
 * NM pieces join; a CE entry says the area goes on in another sector
 * (a long name that did not fit the record): that is followed too, a
 * few times at most (a loop in a damaged image) */
static int rrname(struct isovol *v, const unsigned char *b, int o, int end, char *out)
{
    int n = 0, l, k, got = 0, hops = 0;
    for (;;) {
        unsigned long ce = 0, ceoff = 0, celen = 0;
        while (o + 4 <= end) {
            l = b[o + 2];
            if (l < 4 || o + l > end) break;
            if (b[o] == 'S' && b[o + 1] == 'T') break;
            if (b[o] == 'N' && b[o + 1] == 'M' && l >= 5 && !(b[o + 4] & 6)) {
                for (k = 5; k < l && n < 255; k++) out[n++] = b[o + k];
                got = 1;
            } else if (b[o] == 'C' && b[o + 1] == 'E' && l >= 28) {
                ce = b32(b + o + 8);
                ceoff = b32(b + o + 16);
                celen = b32(b + o + 24);
            }
            o += l;
        }
        if (!ce || ++hops > 4 || ceoff >= SEC || ceoff + celen > SEC) break;
        if (!v->cebuf && !(v->cebuf = arc_alloc(SEC))) break;
        if (ce >= v->src->size / SEC || v->src->read(v->src->ctx, ce * SEC, v->cebuf, SEC) != SEC) break;
        b = v->cebuf;
        o = (int)ceoff;
        end = (int)(ceoff + celen);
    }
    out[n] = 0;
    return got && n > 0;
}

/* decode one directory record into e; 0 = skip it ("." and "..") */
static int record(struct isovol *v, const unsigned char *b, int len, struct arcent *e)
{
    int il = b[32], i, n = 0, c;
    const unsigned char *nm = b + 33;
    if (il == 1 && nm[0] <= 1) return 0;
    e->isdir = (b[25] & 2) != 0;
    e->dataoff = b32(b + 6);
    e->size = e->isdir ? 0 : b32(b + 14);
    e->packed = 0;
    e->aux1 = b32(b + 14);              /* a directory's own byte size */
    e->prot = 0;
    e->comment[0] = 0;
    e->method = 0;
    arc_datestamp(1900 + b[18], b[19], b[20], b[21], b[22], b[23], &e->days, &e->mins, &e->ticks);
    if (v->rr && rrname(v, b, 33 + il + !(il & 1), len, e->name)) return 1;
    if (v->joliet) {
        for (i = 0; i + 1 < il && n < 255; i += 2) {
            c = nm[i] << 8 | nm[i + 1];
            if (c == ';') break;
            e->name[n++] = c > 255 ? '?' : (char)c;
        }
    } else {
        for (i = 0; i < il && n < 255; i++) {
            if (nm[i] == ';' && !e->isdir) break;
            e->name[n++] = nm[i];
        }
        if (n > 1 && e->name[n - 1] == '.') n--;        /* "NAME." stores a dot */
    }
    e->name[n] = 0;
    return n > 0;
}

/* walk one directory's records; fn returns 0 to stop (then 1 is returned) */
static int scan(struct isovol *v, unsigned long ext, unsigned long size, arc_addfn fn, void *ctx)
{
    struct arcent e;
    unsigned long secs = (size + SEC - 1) / SEC, s;
    int pos, len, r;
    if (size > v->src->size) return ARC_ECORRUPT;
    for (s = 0; s < secs; s++) {
        if ((r = rdsec(v, ext + s))) return r;
        /* records never straddle sectors; a 0 length pads to the next */
        for (pos = 0; pos < SEC && (len = v->buf[pos]) != 0; pos += len) {
            if (len < 34 || pos + len > SEC || 33 + v->buf[pos + 32] > len) return ARC_ECORRUPT;
            if (record(v, v->buf + pos, len, &e) && !fn(ctx, &e)) return 1;
        }
    }
    return ARC_OK;
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) ? c + 32 : c;
}

struct findctx {
    const char *part;
    int plen;
    struct arcent *e;
};

static int findone(void *ctx, const struct arcent *e)
{
    struct findctx *f = ctx;
    int i;
    for (i = 0; i < f->plen; i++)
        if (lower((unsigned char)e->name[i]) != lower((unsigned char)f->part[i])) return 1;
    if (e->name[i]) return 1;
    *f->e = *e;
    return 0;
}

int iso_find(struct isovol *v, const char *path, struct arcent *e)
{
    struct findctx f;
    unsigned long ext = v->root, sz = v->rootsz;
    int r;
    e->name[0] = 0;
    e->isdir = 1;
    e->dataoff = ext;
    e->aux1 = sz;
    e->size = 0;
    f.e = e;
    while (*path) {
        while (*path == '/') path++;
        if (!*path) break;
        if (!e->isdir) return ARC_ECORRUPT;
        f.part = path;
        for (f.plen = 0; path[f.plen] && path[f.plen] != '/'; f.plen++)
            ;
        path += f.plen;
        if ((r = scan(v, ext, sz, findone, &f)) < 0) return r;
        if (r != 1) return ARC_ECORRUPT;        /* not there */
        ext = e->dataoff;
        sz = e->aux1;
    }
    return ARC_OK;
}

int iso_list(struct isovol *v, const char *dirpath, arc_addfn add, void *ctx)
{
    struct arcent d;
    int r;
    if ((r = iso_find(v, dirpath, &d))) return r;
    if (!d.isdir) return ARC_ECORRUPT;
    r = scan(v, d.dataoff, d.aux1, add, ctx);
    return r < 0 ? r : ARC_OK;
}

int iso_extract(struct isovol *v, const struct arcent *e, struct arcsink *out)
{
    unsigned long off = e->dataoff * SEC, left = e->size;
    unsigned char *buf;
    long bsz = 16384, n;
    int r = ARC_OK;
    if (e->isdir) return ARC_EUNSUP;
    if (!left) return ARC_OK;           /* empty files may point anywhere */
    if (e->dataoff > v->src->size / SEC || left > v->src->size - off) return ARC_ECORRUPT;
    /* a bigger buffer when there is one, the sector buffer when not */
    if (!(buf = arc_alloc(bsz))) {
        buf = v->buf;
        bsz = SEC;
    }
    while (left) {
        n = left < (unsigned long)bsz ? (long)left : bsz;
        if (v->src->read(v->src->ctx, off, buf, n) != n) {
            r = ARC_EREAD;
            break;
        }
        if (!out->write(out->ctx, buf, n)) {
            r = ARC_ESTOP;
            break;
        }
        off += n;
        left -= n;
    }
    if (buf != v->buf) arc_free(buf);
    return r;
}
