/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The TLS 1.2 client glue (DESIGN D21 3.3, 7.3, 8; step 7): BearSSL's client engine driven over a byte transport the
 * caller gives (two callbacks: the socket layer with its poll timeouts lives in the program, nothing here makes a
 * system call), with D21's set-up: TLS 1.2 only, ECDHE only (X25519, P-256, P-384), the suites ChaCha20-Poly1305,
 * AES-128-GCM, AES-256-GCM, each with the RSA form before the ECDSA one (RSA is several times cheaper to verify on a
 * 68020, 7.3); SHA-256/384/512 only, no SHA-1; no renegotiation; SNI always (the host name given); ALPN when asked;
 * the server's chain through zc_x509 (crypto/zc/zc_x509.h: the anchors it names, EKU, RSA from 2048 bits, one
 * ECDSA encoding) with the time rules of zc_clock.h; no revocation, no extended master secret (8.2).
 * Session reuse (7.3): kept in the process only, keyed by host and port, in a cache the caller owns; BearSSL resumes
 * by session ID only. Keep-alive is the HTTP layer's (net/http): a connection carries requests until either side
 * closes. A transport that ends without the server's close_notify is reported as ZC_TLS_ETRUNC (8.2: a body without
 * a length is then truncated).
 * Memory: the caller's, as everywhere in zc: the record buffer (ZC_TLS_BUFSIZE, BearSSL's one-way buffer: HTTP
 * sends and receives in turn), the chain buffer and the anchor store of zc_x509, and this struct (about 7 KB).
 */
#ifndef ZC_TLS_H
#define ZC_TLS_H

#include "zc_x509.h"
#include "zc_clock.h"

#define ZC_TLS_BUFSIZE   BR_SSL_BUFSIZE_MONO    /* 16,709 bytes */
#define ZC_TLS_SESSIONS  4                      /* remembered sessions (host and port), oldest replaced */
#define ZC_TLS_HOSTMAX   255

/* Errors (negative; the transport's own negative errno numbers are passed through as they are): */
#define ZC_TLS_ETRUNC   (-2001)         /* the connection ended without close_notify */
#define ZC_TLS_EENGINE  (-2002)         /* BearSSL ended it: zc_tls_error() gives its number */
#define ZC_TLS_EPARAM   (-2003)         /* a host name over 255 bytes, or no transport */
#define ZC_TLS_ESTATE   (-2004)         /* the server sent data nobody asked for, or the connection is closed */

/* The transport. send: some of the n bytes, the count taken (> 0), or a negative error number; recv: up to n bytes,
   the count (> 0), 0 when the peer closed, or a negative error number (a timeout is the caller's -ETIMEDOUT). */
struct zc_tls_io {
    void *u;
    long (*send)(void *u, const unsigned char *p, size_t n);
    long (*recv)(void *u, unsigned char *p, size_t n);
};

struct zc_tls_session {
    char host[ZC_TLS_HOSTMAX + 1];
    unsigned port;
    br_ssl_session_parameters p;        /* "" host: a free slot */
};
struct zc_tls_cache {
    struct zc_tls_session s[ZC_TLS_SESSIONS];
    unsigned next;
};

struct zc_tls {
    br_ssl_client_context cc;
    struct zc_x509 x;
    struct zc_tls_io io;
    struct zc_tls_cache *cache;         /* may be null: no resumption */
    struct zc_clock clock;
    const char *alpn[1];
    unsigned char offered[32];          /* the session ID offered for resumption */
    unsigned offered_len;
    char host[ZC_TLS_HOSTMAX + 1];
    unsigned port;
    int resumed;                        /* the handshake resumed a remembered session */
    int open;                           /* the handshake is done and the connection not yet closed */
    int ioerr;                          /* the transport's error, when one ended it */
};

/* Set up once per connection object: iobuf ZC_TLS_BUFSIZE bytes, chain ZC_X509_CHAINMAX, store storecap (the
   anchors' names and keys, at least 8 KB), find: the anchors for an issuer (zc_x509.h). cache may be null. */
void zc_tls_init(struct zc_tls *t, unsigned char *iobuf, unsigned char *chain, unsigned char *store, size_t storecap,
                 zc_x509_find find, void *fu, struct zc_tls_cache *cache);
/* The handshake with host:port over io: SNI and the certificate's name are host; clock the time rules (zc_clock.h,
   copied: t->clock.used_floor says afterwards whether the floor judged a date); seed 32 bytes from /dev/random for
   BearSSL's generator; alpn "http/1.1" or null. 0 when the connection is open, else a negative error. */
int zc_tls_connect(struct zc_tls *t, const struct zc_tls_io *io, const char *host, unsigned port,
                   const struct zc_clock *clock, const unsigned char seed[32], const char *alpn);
/* Send all n bytes (flushed): 0, or a negative error. */
int zc_tls_write(struct zc_tls *t, const void *p, size_t n);
/* Up to n bytes: the count (> 0), 0 when the server closed with close_notify, or a negative error (ZC_TLS_ETRUNC
   when the transport ended without one). */
long zc_tls_read(struct zc_tls *t, void *p, size_t n);
/* Send close_notify (best effort; the caller closes the socket after) and wipe the connection's keys. */
void zc_tls_close(struct zc_tls *t);
/* The BearSSL or zc_x509 error number that ended the connection (0 none), and a short English line for it. */
int zc_tls_error(const struct zc_tls *t);
const char *zc_tls_strerror(int err);
/* The selected ALPN protocol, or null. */
const char *zc_tls_alpn(const struct zc_tls *t);
/* The cache's master secrets wiped (at the program's end). */
void zc_tls_cache_wipe(struct zc_tls_cache *c);

/* The owner's own roots (/etc/ssl/local/, 8.1): the next CERTIFICATE block of a PEM text from *off, decoded into
   der (cap bytes). Its length (> 0), 0 when there is none left, or -1 for a damaged or too large block. */
long zc_pem_cert(const char *pem, size_t n, size_t *off, unsigned char *der, size_t cap);

#endif
