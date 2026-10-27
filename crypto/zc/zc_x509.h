/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The X.509 wrapper and the trust anchors file (DESIGN D21 8.1-8.2): a br_x509_class around BearSSL's x509_minimal
 * that buffers the server's chain (at most ZC_X509_CHAINMAX bytes), loads only the anchors whose subject name hash
 * matches an issuer in the chain (from /etc/ssl/anchors through the caller's find callback, which may also add the
 * owner's own roots from /etc/ssl/local/), then replays the chain into x509_minimal, and checks what BearSSL leaves
 * out: the leaf's Extended Key Usage must name serverAuth when it is there; ECDSA signatures have one encoding
 * (zc_verify.h). x509_minimal is set up with SHA-256/384/512 only (no SHA-1), RSA from 2048 bits, RSA PKCS#1 v1.5
 * and ECDSA through the m15/i15 code. No memory of its own: the caller gives the chain buffer and the anchor store.
 */
#ifndef ZC_X509_H
#define ZC_X509_H

#include "zc.h"
#include "../bearssl/inc/bearssl.h"

#define ZC_X509_CHAINMAX (16 * 1024)    /* a longer chain is refused (8.1) */
#define ZC_X509_MAXCERTS 8              /* certificates in a chain */
#define ZC_X509_TAMAX    16             /* anchors loaded for one chain */
#define ZC_X509_ERR_EKU     1001        /* the leaf's Extended Key Usage has no serverAuth */
#define ZC_X509_ERR_CHAIN   1002        /* the chain is longer than ZC_X509_CHAINMAX or ZC_X509_MAXCERTS */
#define ZC_X509_ERR_ANCHORS 1003        /* more matching anchors than ZC_X509_TAMAX, or the store is full */
#define ZC_X509_ERR_BADFILE 1004        /* the anchors file is damaged */

struct zc_x509;
/* Add the anchors for one issuer name: hash is the first 8 bytes of SHA-256 of the DER name (dn, dnlen). Call
 * zc_anchors_find and/or zc_x509_add_cert; return 0, or an error number that ends the validation. */
typedef int (*zc_x509_find)(void *u, struct zc_x509 *x, const unsigned char hash[8], const unsigned char *dn,
                            size_t dnlen);

struct zc_x509 {
    const br_x509_class *vtable;        /* first: BearSSL's engine calls through it */
    br_x509_minimal_context mini;
    unsigned char *chain;               /* the caller's: ZC_X509_CHAINMAX bytes */
    size_t chainlen;
    size_t certlen[ZC_X509_MAXCERTS];
    int ncert;
    size_t want;                        /* bytes the current certificate still announced */
    int err;
    const char *server_name;
    br_x509_trust_anchor ta[ZC_X509_TAMAX];
    size_t nta;
    unsigned char *store;               /* the caller's: the anchors' names and keys */
    size_t storecap, storeused;
    zc_x509_find find;
    void *u;
    int timeset;
    uint32_t days, seconds;
    void *tctx;
    br_x509_time_check tcb;
};

void zc_x509_init(struct zc_x509 *x, unsigned char *chainbuf, unsigned char *store, size_t storecap,
                  zc_x509_find find, void *u);
/* The time to judge dates by: days since 1 January 0 AD and seconds in the day (BearSSL's form; the Unix epoch is
   day 719528), or a callback that judges them itself (D21 8.3: the floor without a trusted clock). */
void zc_x509_set_time(struct zc_x509 *x, uint32_t days, uint32_t seconds);
void zc_x509_set_time_callback(struct zc_x509 *x, void *ctx, br_x509_time_check cb);

/* For find callbacks: an anchors-file entry (8.1's layout), or a CA certificate's DER (the owner's PEM roots). */
int zc_x509_add_entry(struct zc_x509 *x, const unsigned char *e, size_t n);
int zc_x509_add_cert(struct zc_x509 *x, const unsigned char *der, size_t n);

/* The anchors file through a reader (rd: n bytes at off, 0 or an error): every entry whose hash matches is added. */
typedef int (*zc_reader)(void *u, unsigned long off, void *buf, unsigned long n);
int zc_anchors_find(struct zc_x509 *x, zc_reader rd, void *ru, const unsigned char hash[8]);

/* The leaf's EKU check alone (1 when absent or naming serverAuth); exposed for the tests and fuzzing */
int zc_x509_eku_ok(const unsigned char *der, size_t n);
/* The issuer name of a certificate (DER, with its tag); 0 when the certificate does not parse that far */
const unsigned char *zc_x509_issuer(const unsigned char *der, size_t n, size_t *len);

#endif
