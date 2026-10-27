/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ssh's pieces in the crypto library (DESIGN D21 4.2): K as an mpint, the key derivation of RFC 4253 7.2 with the
 * key extension, and chacha20-poly1305@openssh.com (OpenSSH's PROTOCOL.chacha20poly1305): ChaCha20 in DJB's form
 * (64-bit nonce = the sequence number, big-endian; 64-bit block counter) from Monocypher, Poly1305 from Monocypher,
 * SHA-256 from BearSSL.
 */
#include <string.h>
#include "zc.h"
#include "../bearssl/inc/bearssl.h"
#include "../monocypher/monocypher.h"

size_t zc_ssh_mpint(unsigned char out[37], const unsigned char k[32])
{
    size_t i = 0, n;

    while (i < 32 && k[i] == 0)
        i++;
    n = 32 - i;
    if (n && (k[i] & 0x80)) {
        out[4] = 0;
        memcpy(out + 5, k + i, n);
        n++;
    } else {
        memcpy(out + 4, k + i, n);
    }
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    out[3] = (unsigned char)n;
    return n + 4;
}

void zc_ssh_kdf(unsigned char *out, size_t n, const unsigned char *kmp, size_t klen, const unsigned char h[32],
                char letter, const unsigned char sid[32])
{
    br_sha256_context c, base;
    unsigned char d[32];
    size_t got = 0;

    br_sha256_init(&base);
    br_sha256_update(&base, kmp, klen);
    br_sha256_update(&base, h, 32);
    c = base;
    br_sha256_update(&c, &letter, 1);
    br_sha256_update(&c, sid, 32);
    br_sha256_out(&c, d);
    for (;;) {
        size_t k = n - got < 32 ? n - got : 32;

        memcpy(out + got, d, k);
        got += k;
        if (got >= n)
            break;
        /* K2 = HASH(K || H || K1), K3 = HASH(K || H || K1 || K2), ...: the output so far, appended */
        c = base;
        br_sha256_update(&c, out, got);
        br_sha256_out(&c, d);
    }
    zc_wipe(d, sizeof d);
    zc_wipe(&c, sizeof c);
    zc_wipe(&base, sizeof base);
}

void zc_chapol_init(struct zc_chapol *c, const unsigned char key[64])
{
    memcpy(c->kmain, key, 32);
    memcpy(c->klen, key + 32, 32);
}

void zc_chapol_wipe(struct zc_chapol *c)
{
    zc_wipe(c, sizeof *c);
}

static void nonce(unsigned char n[8], uint32_t seq)
{
    n[0] = n[1] = n[2] = n[3] = 0;
    n[4] = (unsigned char)(seq >> 24);
    n[5] = (unsigned char)(seq >> 16);
    n[6] = (unsigned char)(seq >> 8);
    n[7] = (unsigned char)seq;
}

/* the Poly1305 key: the first 32 bytes of block 0 of the main key's stream */
static void polykey(const struct zc_chapol *c, const unsigned char n[8], unsigned char pk[32])
{
    static const unsigned char zero[32];

    crypto_chacha20_djb(pk, zero, 32, c->kmain, n, 0);
}

void zc_chapol_seal(const struct zc_chapol *c, uint32_t seq, unsigned char *pkt, size_t len, unsigned char tag[16])
{
    unsigned char n[8], pk[32];

    nonce(n, seq);
    crypto_chacha20_djb(pkt, pkt, 4, c->klen, n, 0);
    crypto_chacha20_djb(pkt + 4, pkt + 4, len, c->kmain, n, 1);
    polykey(c, n, pk);
    crypto_poly1305(tag, pkt, len + 4, pk);
    zc_wipe(pk, sizeof pk);
}

uint32_t zc_chapol_length(const struct zc_chapol *c, uint32_t seq, const unsigned char enc[4])
{
    unsigned char n[8], b[4];
    uint32_t v;

    nonce(n, seq);
    crypto_chacha20_djb(b, enc, 4, c->klen, n, 0);
    v = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    zc_wipe(b, sizeof b);
    return v;
}

int zc_chapol_open(const struct zc_chapol *c, uint32_t seq, unsigned char *pkt, size_t len,
                   const unsigned char tag[16])
{
    unsigned char n[8], pk[32], t[16];
    int bad;

    nonce(n, seq);
    polykey(c, n, pk);
    crypto_poly1305(t, pkt, len + 4, pk);
    zc_wipe(pk, sizeof pk);
    bad = crypto_verify16(t, tag);
    zc_wipe(t, sizeof t);
    if (bad)
        return -1;
    crypto_chacha20_djb(pkt + 4, pkt + 4, len, c->kmain, n, 1);
    return 0;
}
