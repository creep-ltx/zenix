/* SPDX-License-Identifier: GPL-2.0-only */
/* The TLS 1.2 client glue (DESIGN D21 3.3, 7.3, 8; zc_tls.h). No system calls; the transport is the caller's. */
#include <string.h>
#include "zc_tls.h"
#include "zc_verify.h"
#include "../bearssl/src/inner.h"        /* br_ssl_engine_fail (as BearSSL's own ssl_io.c uses it) */

/* D21 3.3's order: ChaCha20-Poly1305, then AES-128-GCM, then AES-256-GCM; RSA before ECDSA in each */
static const uint16_t suites[] = {
    BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256, BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
};

void zc_tls_init(struct zc_tls *t, unsigned char *iobuf, unsigned char *chain, unsigned char *store, size_t storecap,
                 zc_x509_find find, void *fu, struct zc_tls_cache *cache)
{
    br_ssl_engine_context *e = &t->cc.eng;

    memset(t, 0, sizeof *t);
    t->cache = cache;
    zc_x509_init(&t->x, chain, store, storecap, find, fu);
    br_ssl_client_zero(&t->cc);
    br_ssl_engine_set_versions(e, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(e, suites, sizeof suites / sizeof suites[0]);
    br_ssl_engine_set_x509(e, &t->x.vtable);
    br_ssl_client_set_rsapub(&t->cc, br_rsa_i15_public);
    br_ssl_engine_set_rsavrfy(e, br_rsa_i15_pkcs1_vrfy);
    br_ssl_engine_set_ec(e, &br_ec_all_m15);
    br_ssl_engine_set_ecdsa(e, zc_ecdsa_vrfy_asn1);
    br_ssl_engine_set_hash(e, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_hash(e, br_sha384_ID, &br_sha384_vtable);
    br_ssl_engine_set_hash(e, br_sha512_ID, &br_sha512_vtable);
    br_ssl_engine_set_prf_sha256(e, br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(e, br_tls12_sha384_prf);
    br_ssl_engine_set_chapol(e, &br_sslrec_in_chapol_vtable, &br_sslrec_out_chapol_vtable);
    br_ssl_engine_set_chacha20(e, br_chacha20_ct_run);
    br_ssl_engine_set_poly1305(e, br_poly1305_ctmul32_run);
    br_ssl_engine_set_gcm(e, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
    br_ssl_engine_set_aes_ctr(e, &br_aes_ct_ctr_vtable);
    br_ssl_engine_set_ghash(e, br_ghash_ctmul32);
    br_ssl_engine_set_all_flags(e, BR_OPT_NO_RENEGOTIATION);
    br_ssl_engine_set_buffer(e, iobuf, ZC_TLS_BUFSIZE, 0);
}

static struct zc_tls_session *lookup(struct zc_tls_cache *c, const char *host, unsigned port)
{
    unsigned i;

    if (!c)
        return 0;
    for (i = 0; i < ZC_TLS_SESSIONS; i++)
        if (c->s[i].host[0] && c->s[i].port == port && !strcmp(c->s[i].host, host))
            return &c->s[i];
    return 0;
}

static void remember(struct zc_tls *t)
{
    struct zc_tls_session *s;
    br_ssl_session_parameters p;

    if (!t->cache)
        return;
    br_ssl_engine_get_session_parameters(&t->cc.eng, &p);
    s = lookup(t->cache, t->host, t->port);
    if (!p.session_id_len) {            /* the server keeps no sessions: forget any older one */
        if (s) {
            zc_wipe(s, sizeof *s);
        }
        zc_wipe(&p, sizeof p);
        return;
    }
    if (!s) {
        s = &t->cache->s[t->cache->next];
        t->cache->next = (t->cache->next + 1) % ZC_TLS_SESSIONS;
    }
    zc_wipe(s, sizeof *s);
    strcpy(s->host, t->host);
    s->port = t->port;
    s->p = p;
    zc_wipe(&p, sizeof p);
}

/* Run the engine until one of want's states (BR_SSL_SENDAPP, BR_SSL_RECVAPP) is reached: 0, or a negative error */
static int run(struct zc_tls *t, unsigned want)
{
    br_ssl_engine_context *e = &t->cc.eng;

    for (;;) {
        unsigned st = br_ssl_engine_current_state(e);
        unsigned char *b;
        size_t len;
        long k;

        if (st & BR_SSL_CLOSED) {
            t->open = 0;
            if (t->ioerr)
                return t->ioerr;
            return br_ssl_engine_last_error(e) ? ZC_TLS_EENGINE : ZC_TLS_ESTATE;
        }
        if (st & BR_SSL_SENDREC) {
            b = br_ssl_engine_sendrec_buf(e, &len);
            k = t->io.send(t->io.u, b, len);
            if (k <= 0 || (size_t)k > len) {
                t->ioerr = k < 0 ? (int)k : ZC_TLS_ETRUNC;
                br_ssl_engine_fail(e, BR_ERR_IO);
                continue;
            }
            br_ssl_engine_sendrec_ack(e, (size_t)k);
            continue;
        }
        if (st & want)
            return 0;
        if (st & BR_SSL_RECVAPP)        /* data to read while the caller wants to write: nobody asked for it */
            return ZC_TLS_ESTATE;
        if (st & BR_SSL_RECVREC) {
            b = br_ssl_engine_recvrec_buf(e, &len);
            k = t->io.recv(t->io.u, b, len);
            if (k <= 0 || (size_t)k > len) {
                t->ioerr = k < 0 ? (int)k : ZC_TLS_ETRUNC;   /* 0: the transport ended, no close_notify came */
                br_ssl_engine_fail(e, BR_ERR_IO);
                continue;
            }
            br_ssl_engine_recvrec_ack(e, (size_t)k);
            continue;
        }
        br_ssl_engine_flush(e, 0);      /* neither: push out what the engine holds */
    }
}

int zc_tls_connect(struct zc_tls *t, const struct zc_tls_io *io, const char *host, unsigned port,
                   const struct zc_clock *clock, const unsigned char seed[32], const char *alpn)
{
    br_ssl_engine_context *e = &t->cc.eng;
    struct zc_tls_session *s;
    br_ssl_session_parameters p;
    int r;

    if (!io || !io->send || !io->recv || !host || strlen(host) > ZC_TLS_HOSTMAX || !host[0])
        return ZC_TLS_EPARAM;
    t->io = *io;
    strcpy(t->host, host);
    t->port = port;
    t->clock = *clock;
    t->clock.used_floor = 0;
    t->resumed = 0;
    t->ioerr = 0;
    t->open = 0;
    zc_x509_set_time_callback(&t->x, &t->clock, zc_clock_check);
    if (alpn) {
        t->alpn[0] = alpn;
        br_ssl_engine_set_protocol_names(e, t->alpn, 1);
    } else {
        br_ssl_engine_set_protocol_names(e, 0, 0);
    }
    s = lookup(t->cache, host, port);
    t->offered_len = 0;
    if (s) {
        br_ssl_engine_set_session_parameters(e, &s->p);
        memcpy(t->offered, s->p.session_id, s->p.session_id_len);
        t->offered_len = s->p.session_id_len;
    }
    br_ssl_engine_inject_entropy(e, seed, 32);
    if (!br_ssl_client_reset(&t->cc, host, s != 0))
        return ZC_TLS_EENGINE;
    r = run(t, BR_SSL_SENDAPP | BR_SSL_RECVAPP);
    if (r < 0)
        return r;
    t->open = 1;
    br_ssl_engine_get_session_parameters(e, &p);
    t->resumed = t->offered_len && p.session_id_len == t->offered_len &&
                 !memcmp(p.session_id, t->offered, t->offered_len);
    zc_wipe(&p, sizeof p);
    remember(t);
    return 0;
}

int zc_tls_write(struct zc_tls *t, const void *p, size_t n)
{
    br_ssl_engine_context *e = &t->cc.eng;
    const unsigned char *s = p;
    int r;

    if (!t->open)
        return ZC_TLS_ESTATE;
    while (n) {
        unsigned char *b;
        size_t len;

        if ((r = run(t, BR_SSL_SENDAPP)) < 0)
            return r;
        b = br_ssl_engine_sendapp_buf(e, &len);
        if (len > n)
            len = n;
        memcpy(b, s, len);
        br_ssl_engine_sendapp_ack(e, len);
        s += len;
        n -= len;
    }
    br_ssl_engine_flush(e, 0);
    /* the records out before the caller waits for an answer */
    for (;;) {
        unsigned st = br_ssl_engine_current_state(e);
        unsigned char *b;
        size_t len;
        long k;

        if (st & BR_SSL_CLOSED)
            return run(t, 0);
        if (!(st & BR_SSL_SENDREC))
            return 0;
        b = br_ssl_engine_sendrec_buf(e, &len);
        k = t->io.send(t->io.u, b, len);
        if (k <= 0 || (size_t)k > len) {
            t->ioerr = k < 0 ? (int)k : ZC_TLS_ETRUNC;
            br_ssl_engine_fail(e, BR_ERR_IO);
            continue;
        }
        br_ssl_engine_sendrec_ack(e, (size_t)k);
    }
}

long zc_tls_read(struct zc_tls *t, void *p, size_t n)
{
    br_ssl_engine_context *e = &t->cc.eng;
    unsigned char *b;
    size_t len;
    int r;

    if (!t->open && !t->ioerr)
        return br_ssl_engine_current_state(e) & BR_SSL_CLOSED && !br_ssl_engine_last_error(e) ? 0 : ZC_TLS_ESTATE;
    if (!n)
        return 0;
    r = run(t, BR_SSL_RECVAPP);
    if (r < 0) {
        /* closed with close_notify (no error, no transport failure): the end of the data */
        if (!t->ioerr && (br_ssl_engine_current_state(e) & BR_SSL_CLOSED) && !br_ssl_engine_last_error(e))
            return 0;
        return r;
    }
    b = br_ssl_engine_recvapp_buf(e, &len);
    if (len > n)
        len = n;
    memcpy(p, b, len);
    br_ssl_engine_recvapp_ack(e, len);
    return (long)len;
}

void zc_tls_close(struct zc_tls *t)
{
    br_ssl_engine_context *e = &t->cc.eng;

    if (t->open && !(br_ssl_engine_current_state(e) & BR_SSL_CLOSED)) {
        br_ssl_engine_close(e);
        /* close_notify out; the server's answer is not waited for (the caller closes the socket) */
        while ((br_ssl_engine_current_state(e) & (BR_SSL_SENDREC | BR_SSL_CLOSED)) == BR_SSL_SENDREC) {
            unsigned char *b;
            size_t len;
            long k;

            b = br_ssl_engine_sendrec_buf(e, &len);
            k = t->io.send(t->io.u, b, len);
            if (k <= 0 || (size_t)k > len)
                break;
            br_ssl_engine_sendrec_ack(e, (size_t)k);
        }
    }
    t->open = 0;
    /* the keys: the engine's state (the record keys and the master secret) and the copy of the offered session ID */
    br_ssl_engine_fail(e, br_ssl_engine_last_error(e) ? br_ssl_engine_last_error(e) : BR_ERR_OK);
    zc_wipe(&e->session, sizeof e->session);
    zc_wipe(&e->in, sizeof e->in);
    zc_wipe(&e->out, sizeof e->out);
    zc_wipe(t->offered, sizeof t->offered);
}

int zc_tls_error(const struct zc_tls *t)
{
    return br_ssl_engine_last_error(&t->cc.eng);
}

const char *zc_tls_alpn(const struct zc_tls *t)
{
    return br_ssl_engine_get_selected_protocol((br_ssl_engine_context *)&t->cc.eng);
}

void zc_tls_cache_wipe(struct zc_tls_cache *c)
{
    zc_wipe(c, sizeof *c);
}

const char *zc_tls_strerror(int err)
{
    switch (err) {
    case 0: return "no error";
    case ZC_TLS_ETRUNC: return "the connection ended without TLS close_notify (truncated)";
    case ZC_TLS_EPARAM: return "bad host name";
    case ZC_TLS_ESTATE: return "the connection is closed, or the server sent data nobody asked for";
    case BR_ERR_IO: return "the connection failed";
    case BR_ERR_X509_EXPIRED: return "the certificate has expired or is not yet valid";
    case BR_ERR_X509_BAD_SERVER_NAME: return "the certificate is for another name";
    case BR_ERR_X509_NOT_TRUSTED: return "the certificate's root is not trusted (an unknown root, or a missing intermediate)";
    case BR_ERR_X509_TIME_UNKNOWN: return "the time is unknown, so the certificate cannot be checked";
    case BR_ERR_X509_WEAK_PUBLIC_KEY: return "the certificate's key is too weak (RSA below 2048 bits)";
    case BR_ERR_X509_BAD_SIGNATURE: return "a certificate's signature is wrong";
    case BR_ERR_X509_UNSUPPORTED: return "a certificate uses something not supported (RSA-PSS, SHA-1, another curve)";
    case BR_ERR_X509_CRITICAL_EXTENSION: return "a certificate has an unknown critical extension";
    case BR_ERR_X509_NOT_CA: return "a certificate in the chain is not a CA";
    case BR_ERR_X509_FORBIDDEN_KEY_USAGE: return "the certificate's key usage forbids this";
    case BR_ERR_X509_DN_MISMATCH: return "the chain is not in order";
    case ZC_X509_ERR_EKU: return "the certificate is not for TLS servers (Extended Key Usage)";
    case ZC_X509_ERR_CHAIN: return "the certificate chain is too long";
    case ZC_X509_ERR_ANCHORS: return "too many roots match the chain";
    case ZC_X509_ERR_BADFILE: return "the trust anchors file is damaged";
    case BR_ERR_BAD_SIGNATURE: return "the server's key exchange signature is wrong";
    case BR_ERR_BAD_VERSION: case BR_ERR_UNSUPPORTED_VERSION:
    case BR_ERR_RECV_FATAL_ALERT + 70:  /* protocol_version */
        return "this server needs TLS 1.3, which Zenix does not speak yet";
    case BR_ERR_RECV_FATAL_ALERT + 40:  /* handshake_failure */
    case BR_ERR_RECV_FATAL_ALERT + 71:  /* insufficient_security */
    case BR_ERR_BAD_CIPHER_SUITE:
        return "the server accepts none of our ciphers (a TLS 1.3-only or FIPS-mode server?)";
    }
    if (err >= BR_ERR_RECV_FATAL_ALERT && err < BR_ERR_RECV_FATAL_ALERT + 256)
        return "the server ended the handshake with an alert";
    if (err >= BR_ERR_SEND_FATAL_ALERT && err < BR_ERR_SEND_FATAL_ALERT + 256)
        return "the server's answer broke the protocol";
    if (err >= BR_ERR_X509_OK && err <= 63)
        return "the certificate is invalid";
    return "the TLS handshake failed";
}

/* ---- PEM roots ---- */
struct pemdst {
    unsigned char *p;
    size_t n, cap;
    int over;
};

static void pem_put(void *ctx, const void *src, size_t len)
{
    struct pemdst *d = ctx;

    if (d->over || len > d->cap - d->n) {
        d->over = 1;
        return;
    }
    memcpy(d->p + d->n, src, len);
    d->n += len;
}

long zc_pem_cert(const char *pem, size_t n, size_t *off, unsigned char *der, size_t cap)
{
    br_pem_decoder_context pc;
    struct pemdst d;
    int in = 0, cert = 0;

    br_pem_decoder_init(&pc);
    d.p = der;
    d.cap = cap;
    while (*off < n) {
        size_t k = br_pem_decoder_push(&pc, pem + *off, n - *off);

        *off += k;
        switch (br_pem_decoder_event(&pc)) {
        case BR_PEM_BEGIN_OBJ:
            in = 1;
            cert = !strcmp(br_pem_decoder_name(&pc), "CERTIFICATE") || !strcmp(br_pem_decoder_name(&pc), "X509 CERTIFICATE");
            d.n = 0;
            d.over = 0;
            br_pem_decoder_setdest(&pc, cert ? pem_put : 0, &d);
            break;
        case BR_PEM_END_OBJ:
            if (in && cert)
                return d.over || !d.n ? -1 : (long)d.n;
            in = 0;
            break;
        case BR_PEM_ERROR:
            return -1;
        }
    }
    return in ? -1 : 0;
}
