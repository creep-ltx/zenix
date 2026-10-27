/* SPDX-License-Identifier: GPL-2.0-only */
/* The X.509 wrapper and the trust anchors file (DESIGN D21 8.1-8.2; zc_x509.h). Every byte of a certificate and of
   the anchors file is untrusted: each length is checked before use. */
#include <string.h>
#include "zc_x509.h"
#include "zc_verify.h"

/* ---- a small DER walk (what BearSSL's decoder does not give: the issuer name, the EKU extension) ---- */
/* the element at p: its header length and value length; 0 when it does not fit in n */
static int der(const unsigned char *p, size_t n, unsigned *tag, size_t *hdr, size_t *len)
{
    size_t l, k, i;

    if (n < 2)
        return 0;
    *tag = p[0];
    l = p[1];
    *hdr = 2;
    if (l & 0x80) {
        k = l & 0x7f;
        if (k == 0 || k > 3 || n < 2 + k)
            return 0;
        for (l = 0, i = 0; i < k; i++)
            l = (l << 8) | p[2 + i];
        *hdr = 2 + k;
    }
    if (l > n - *hdr)
        return 0;
    *len = l;
    return 1;
}

/* the tbsCertificate's elements: fills e[] with up to max (pointer, total length) pairs; returns their count */
static int tbs_elements(const unsigned char *c, size_t n, const unsigned char **e, size_t *el, int max)
{
    unsigned tag;
    size_t h, l, off;
    int k = 0;

    if (!der(c, n, &tag, &h, &l) || tag != 0x30)
        return 0;
    c += h;
    n = l;
    if (!der(c, n, &tag, &h, &l) || tag != 0x30)
        return 0;
    c += h;
    n = l;
    for (off = 0; off < n && k < max; k++) {
        if (!der(c + off, n - off, &tag, &h, &l))
            return 0;
        e[k] = c + off;
        el[k] = h + l;
        off += h + l;
    }
    return k;
}

const unsigned char *zc_x509_issuer(const unsigned char *der_, size_t n, size_t *len)
{
    const unsigned char *e[12];
    size_t el[12];
    int k = tbs_elements(der_, n, e, el, 12), v;

    v = (k > 0 && e[0][0] == 0xa0) ? 1 : 0;         /* [0] version */
    if (k < v + 4 || e[v][0] != 0x02 || e[v + 1][0] != 0x30 || e[v + 2][0] != 0x30)
        return 0;
    *len = el[v + 2];
    return e[v + 2];
}

int zc_x509_eku_ok(const unsigned char *c, size_t n)
{
    static const unsigned char eku_oid[] = { 0x06, 0x03, 0x55, 0x1d, 0x25 };
    static const unsigned char server_auth[] = { 0x06, 0x08, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x01 };
    const unsigned char *e[12], *x;
    size_t el[12], h, l, off, xn;
    unsigned tag;
    int k = tbs_elements(c, n, e, el, 12), i;

    if (k < 6)
        return 0;
    for (i = 0; i < k && e[i][0] != 0xa3; i++)      /* [3] extensions */
        ;
    if (i == k)
        return 1;
    if (!der(e[i], el[i], &tag, &h, &l) || !der(e[i] + h, l, &tag, &h, &xn) || tag != 0x30)
        return 0;
    x = e[i] + (el[i] - l) + h;                      /* the SEQUENCE OF Extension's contents */
    for (off = 0; off < xn;) {
        const unsigned char *ex;
        size_t eh, elen, p;

        if (!der(x + off, xn - off, &tag, &eh, &elen) || tag != 0x30)
            return 0;
        ex = x + off + eh;
        off += eh + elen;
        if (elen < sizeof eku_oid || memcmp(ex, eku_oid, sizeof eku_oid))
            continue;
        p = sizeof eku_oid;
        if (p < elen && ex[p] == 0x01) {            /* critical */
            if (!der(ex + p, elen - p, &tag, &h, &l))
                return 0;
            p += h + l;
        }
        /* OCTET STRING { SEQUENCE OF OID } */
        if (!der(ex + p, elen - p, &tag, &h, &l) || tag != 0x04)
            return 0;
        p += h;
        if (!der(ex + p, l, &tag, &h, &l) || tag != 0x30)
            return 0;
        p += h;
        {
            size_t q = 0;

            while (q < l) {
                size_t oh, ol;

                if (!der(ex + p + q, l - q, &tag, &oh, &ol) || tag != 0x06)
                    return 0;
                if (oh + ol == sizeof server_auth && !memcmp(ex + p + q, server_auth, sizeof server_auth))
                    return 1;
                q += oh + ol;
            }
        }
        return 0;                                   /* an EKU without serverAuth */
    }
    return 1;
}

/* ---- the anchors ---- */
static void *take(struct zc_x509 *x, size_t n)
{
    void *p;

    if (n > x->storecap - x->storeused)
        return 0;
    p = x->store + x->storeused;
    x->storeused += n;
    return p;
}

/* parse an entry already in the store (8.1's layout) into the next anchor */
static int adopt(struct zc_x509 *x, const unsigned char *e, size_t n)
{
    br_x509_trust_anchor *t;
    size_t nl, p;

    if (x->nta >= ZC_X509_TAMAX)
        return ZC_X509_ERR_ANCHORS;
    if (n < 4)
        return ZC_X509_ERR_BADFILE;
    nl = (size_t)e[2] << 8 | e[3];
    if (nl == 0 || nl > n - 4)
        return ZC_X509_ERR_BADFILE;
    t = &x->ta[x->nta];
    memset(t, 0, sizeof *t);
    t->dn.data = (unsigned char *)e + 4;
    t->dn.len = nl;
    t->flags = (e[0] & 1) ? BR_X509_TA_CA : 0;
    p = 4 + nl;
    if (e[1] == 1) {
        size_t ml, xl;

        if (n - p < 2)
            return ZC_X509_ERR_BADFILE;
        ml = (size_t)e[p] << 8 | e[p + 1];
        p += 2;
        if (ml == 0 || ml > n - p || n - p - ml < 1)
            return ZC_X509_ERR_BADFILE;
        t->pkey.key.rsa.n = (unsigned char *)e + p;
        t->pkey.key.rsa.nlen = ml;
        p += ml;
        xl = e[p++];
        if (xl == 0 || xl != n - p)
            return ZC_X509_ERR_BADFILE;
        t->pkey.key_type = BR_KEYTYPE_RSA;
        t->pkey.key.rsa.e = (unsigned char *)e + p;
        t->pkey.key.rsa.elen = xl;
    } else if (e[1] == 2) {
        size_t ql;

        if (n - p < 2 || (e[p] != BR_EC_secp256r1 && e[p] != BR_EC_secp384r1 && e[p] != BR_EC_secp521r1))
            return ZC_X509_ERR_BADFILE;
        t->pkey.key_type = BR_KEYTYPE_EC;
        t->pkey.key.ec.curve = e[p];
        ql = e[p + 1];
        p += 2;
        if (ql == 0 || ql != n - p)
            return ZC_X509_ERR_BADFILE;
        t->pkey.key.ec.q = (unsigned char *)e + p;
        t->pkey.key.ec.qlen = ql;
    } else {
        return ZC_X509_ERR_BADFILE;
    }
    x->nta++;
    return 0;
}

int zc_x509_add_entry(struct zc_x509 *x, const unsigned char *e, size_t n)
{
    unsigned char *s = take(x, n);
    size_t keep = x->storeused - n;
    int r;

    if (!s)
        return ZC_X509_ERR_ANCHORS;
    memcpy(s, e, n);
    r = adopt(x, s, n);
    if (r)
        x->storeused = keep;
    return r;
}

struct dnbuf {
    struct zc_x509 *x;
    unsigned char *p;
    size_t n;
    int full;
};

static void dn_append(void *ctx, const void *buf, size_t len)
{
    struct dnbuf *d = ctx;

    if (d->full || !take(d->x, len)) {
        d->full = 1;
        return;
    }
    memcpy(d->p + d->n, buf, len);
    d->n += len;
}

int zc_x509_add_cert(struct zc_x509 *x, const unsigned char *c, size_t n)
{
    br_x509_decoder_context *d;
    const br_x509_pkey *k;
    br_x509_trust_anchor *t;
    struct dnbuf db;
    size_t keep = x->storeused;
    static const size_t align = sizeof(void *);
    unsigned char *mem;

    if (x->nta >= ZC_X509_TAMAX)
        return ZC_X509_ERR_ANCHORS;
    /* the decoder's context (about 1 KB) lives in the store for the call, not on the stack */
    x->storeused = (x->storeused + align - 1) / align * align;
    mem = take(x, sizeof *d);
    if (!mem) {
        x->storeused = keep;
        return ZC_X509_ERR_ANCHORS;
    }
    d = (br_x509_decoder_context *)(void *)mem;
    db.x = x;
    db.p = x->store + x->storeused;
    db.n = 0;
    db.full = 0;
    br_x509_decoder_init(d, dn_append, &db);
    br_x509_decoder_push(d, c, n);
    k = br_x509_decoder_get_pkey(d);
    if (db.full || !k || br_x509_decoder_last_error(d) != 0) {
        x->storeused = keep;
        return db.full ? ZC_X509_ERR_ANCHORS : BR_ERR_X509_INVALID_VALUE;
    }
    t = &x->ta[x->nta];
    memset(t, 0, sizeof *t);
    t->dn.data = db.p;
    t->dn.len = db.n;
    t->flags = br_x509_decoder_isCA(d) ? BR_X509_TA_CA : 0;
    t->pkey = *k;
    /* the key's bytes point into the decoder's context: moved behind the name, the context given back */
    {
        unsigned char *a, *b2;
        size_t al, bl;
        int kt = k->key_type;           /* (k points into the context, which the move below overwrites) */

        if (kt == BR_KEYTYPE_RSA) {
            al = k->key.rsa.nlen;
            bl = k->key.rsa.elen;
        } else {
            al = k->key.ec.qlen;
            bl = 0;
        }
        a = take(x, al + bl);
        if (!a) {
            x->storeused = keep;
            return ZC_X509_ERR_ANCHORS;
        }
        b2 = a + al;
        if (kt == BR_KEYTYPE_RSA) {
            memcpy(a, k->key.rsa.n, al);
            memcpy(b2, k->key.rsa.e, bl);
            t->pkey.key.rsa.n = a;
            t->pkey.key.rsa.e = b2;
        } else {
            memcpy(a, k->key.ec.q, al);
            t->pkey.key.ec.q = a;
        }
        /* close the gap the context left: name and key slide down over it */
        {
            size_t used = (size_t)(x->store + x->storeused - db.p);

            memmove(mem, db.p, used);
            t->dn.data = mem;
            if (kt == BR_KEYTYPE_RSA) {
                t->pkey.key.rsa.n = mem + (a - db.p);
                t->pkey.key.rsa.e = mem + (b2 - db.p);
            } else {
                t->pkey.key.ec.q = mem + (a - db.p);
            }
            x->storeused = (size_t)(mem - x->store) + used;
        }
    }
    x->nta++;
    return 0;
}

static int rd_u32(zc_reader rd, void *ru, unsigned long off, unsigned long *v)
{
    unsigned char b[4];

    if (rd(ru, off, b, 4))
        return -1;
    *v = (unsigned long)b[0] << 24 | (unsigned long)b[1] << 16 | (unsigned long)b[2] << 8 | b[3];
    return 0;
}

/* read one entry (its size from its own fields) into the store and adopt it */
static int load_entry(struct zc_x509 *x, zc_reader rd, void *ru, unsigned long off)
{
    size_t keep = x->storeused, n = 0, piece[3];
    unsigned char *e = x->store + x->storeused;
    int i, r;

    /* three reads: flags, type and the name's length; the name and the next two bytes; the rest of the key */
    piece[0] = 4;
    for (i = 0; i < 3; i++) {
        if (i == 1)
            piece[1] = ((size_t)e[2] << 8 | e[3]) + 2;
        if (i == 2) {
            size_t nl = (size_t)e[2] << 8 | e[3];

            if (e[1] == 1)              /* RSA: the modulus, then its exponent's length byte */
                piece[2] = ((size_t)e[4 + nl] << 8 | e[5 + nl]) + 1;
            else                        /* EC: the point */
                piece[2] = e[5 + nl];
        }
        if (!take(x, piece[i])) {
            x->storeused = keep;
            return ZC_X509_ERR_ANCHORS;
        }
        if (rd(ru, off + n, e + n, piece[i])) {
            x->storeused = keep;
            return ZC_X509_ERR_BADFILE;
        }
        n += piece[i];
    }
    if (e[1] == 1) {                    /* RSA: the exponent */
        size_t xl = e[n - 1];

        if (!take(x, xl)) {
            x->storeused = keep;
            return ZC_X509_ERR_ANCHORS;
        }
        if (rd(ru, off + n, e + n, xl)) {
            x->storeused = keep;
            return ZC_X509_ERR_BADFILE;
        }
        n += xl;
    }
    r = adopt(x, e, n);
    if (r)
        x->storeused = keep;
    return r;
}

int zc_anchors_find(struct zc_x509 *x, zc_reader rd, void *ru, const unsigned char hash[8])
{
    unsigned char h[16];
    unsigned long count, ioff, doff, lo, hi, off;

    if (rd(ru, 0, h, 16) || memcmp(h, "ZTA1", 4) || h[4] != 0 || h[5] != 1)
        return ZC_X509_ERR_BADFILE;
    count = (unsigned long)h[6] << 8 | h[7];
    ioff = (unsigned long)h[8] << 24 | (unsigned long)h[9] << 16 | (unsigned long)h[10] << 8 | h[11];
    doff = (unsigned long)h[12] << 24 | (unsigned long)h[13] << 16 | (unsigned long)h[14] << 8 | h[15];
    if (ioff < 16 || doff < ioff || (doff - ioff) / 12 < count)
        return ZC_X509_ERR_BADFILE;
    /* the first index entry whose hash is not below ours */
    lo = 0;
    hi = count;
    while (lo < hi) {
        unsigned long mid = lo + (hi - lo) / 2;

        if (rd(ru, ioff + 12 * mid, h, 8))
            return ZC_X509_ERR_BADFILE;
        if (memcmp(h, hash, 8) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (; lo < count; lo++) {
        int r;

        if (rd(ru, ioff + 12 * lo, h, 8) || rd_u32(rd, ru, ioff + 12 * lo + 8, &off))
            return ZC_X509_ERR_BADFILE;
        if (memcmp(h, hash, 8))
            break;
        if (off > 0xFFFFFFFFUL - doff)
            return ZC_X509_ERR_BADFILE;
        r = load_entry(x, rd, ru, doff + off);
        if (r)
            return r;
    }
    return 0;
}

/* ---- the class ---- */
static void xs_start_chain(const br_x509_class **ctx, const char *server_name)
{
    struct zc_x509 *x = (struct zc_x509 *)(void *)ctx;

    x->server_name = server_name;
    x->chainlen = 0;
    x->ncert = 0;
    x->want = 0;
    x->err = 0;
    x->nta = 0;
    x->storeused = 0;
}

static void xs_start_cert(const br_x509_class **ctx, uint32_t length)
{
    struct zc_x509 *x = (struct zc_x509 *)(void *)ctx;

    if (x->err)
        return;
    if (x->ncert >= ZC_X509_MAXCERTS || length > ZC_X509_CHAINMAX - x->chainlen) {
        x->err = ZC_X509_ERR_CHAIN;
        return;
    }
    x->certlen[x->ncert] = 0;
    x->want = length;
}

static void xs_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
    struct zc_x509 *x = (struct zc_x509 *)(void *)ctx;

    if (x->err)
        return;
    if (len > x->want) {
        x->err = BR_ERR_X509_INNER_TRUNC;
        return;
    }
    memcpy(x->chain + x->chainlen, buf, len);
    x->chainlen += len;
    x->certlen[x->ncert] += len;
    x->want -= len;
}

static void xs_end_cert(const br_x509_class **ctx)
{
    struct zc_x509 *x = (struct zc_x509 *)(void *)ctx;

    if (x->err)
        return;
    if (x->want) {
        x->err = BR_ERR_X509_TRUNCATED;
        return;
    }
    x->ncert++;
}

static unsigned xs_end_chain(const br_x509_class **ctx)
{
    struct zc_x509 *x = (struct zc_x509 *)(void *)ctx;
    const br_x509_class **m = &x->mini.vtable;
    unsigned char done[ZC_X509_MAXCERTS][8];
    size_t off = 0;
    int i, j, nd = 0;
    unsigned r;

    if (x->err)
        return (unsigned)x->err;
    if (!x->ncert)
        return (unsigned)(x->err = BR_ERR_X509_EMPTY_CHAIN);
    if (!x->timeset && !x->tcb)
        return (unsigned)(x->err = BR_ERR_X509_TIME_UNKNOWN);
    /* the anchors for every issuer named in the chain (the root itself may be in it) */
    for (i = 0; i < x->ncert; off += x->certlen[i++]) {
        size_t il;
        const unsigned char *is = zc_x509_issuer(x->chain + off, x->certlen[i], &il);
        unsigned char d[32];
        br_sha256_context c;

        if (!is)
            return (unsigned)(x->err = BR_ERR_X509_INVALID_VALUE);
        br_sha256_init(&c);
        br_sha256_update(&c, is, il);
        br_sha256_out(&c, d);
        for (j = 0; j < nd && memcmp(done[j], d, 8); j++)
            ;
        if (j < nd)
            continue;
        memcpy(done[nd++], d, 8);
        if (x->find) {
            int e = x->find(x->u, x, d, is, il);

            if (e)
                return (unsigned)(x->err = e);
        }
    }
    br_x509_minimal_init(&x->mini, &br_sha256_vtable, x->ta, x->nta);
    br_x509_minimal_set_hash(&x->mini, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(&x->mini, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(&x->mini, br_sha512_ID, &br_sha512_vtable);
    br_x509_minimal_set_rsa(&x->mini, br_rsa_i15_pkcs1_vrfy);
    br_x509_minimal_set_ecdsa(&x->mini, &br_ec_all_m15, zc_ecdsa_vrfy_asn1);
    br_x509_minimal_set_minrsa(&x->mini, 256);
    if (x->tcb)
        br_x509_minimal_set_time_callback(&x->mini, x->tctx, x->tcb);
    else
        br_x509_minimal_set_time(&x->mini, x->days, x->seconds);
    (*m)->start_chain(m, x->server_name);
    for (i = 0, off = 0; i < x->ncert; off += x->certlen[i++]) {
        (*m)->start_cert(m, (uint32_t)x->certlen[i]);
        (*m)->append(m, x->chain + off, x->certlen[i]);
        (*m)->end_cert(m);
    }
    r = (*m)->end_chain(m);
    if (r)
        return (unsigned)(x->err = (int)r);
    if (!zc_x509_eku_ok(x->chain, x->certlen[0]))
        return (unsigned)(x->err = ZC_X509_ERR_EKU);
    return 0;
}

static const br_x509_pkey *xs_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
    struct zc_x509 *x = (struct zc_x509 *)(void *)ctx;
    const br_x509_class *const *m = &x->mini.vtable;

    if (x->err || !x->ncert)
        return 0;
    return (*m)->get_pkey(m, usages);
}

static const br_x509_class zc_x509_vtable = {
    sizeof(struct zc_x509), xs_start_chain, xs_start_cert, xs_append, xs_end_cert, xs_end_chain, xs_get_pkey
};

void zc_x509_init(struct zc_x509 *x, unsigned char *chainbuf, unsigned char *store, size_t storecap,
                  zc_x509_find find, void *u)
{
    memset(x, 0, sizeof *x);
    x->vtable = &zc_x509_vtable;
    x->chain = chainbuf;
    x->store = store;
    x->storecap = storecap;
    x->find = find;
    x->u = u;
}

void zc_x509_set_time(struct zc_x509 *x, uint32_t days, uint32_t seconds)
{
    x->days = days;
    x->seconds = seconds;
    x->timeset = 1;
}

void zc_x509_set_time_callback(struct zc_x509 *x, void *ctx, br_x509_time_check cb)
{
    x->tctx = ctx;
    x->tcb = cb;
}
