/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Signature checks the libraries leave lenient, made strict (found by Wycheproof's vectors in
 * tools/host/crypto-test.sh, 2026-10-27; DESIGN D21 9.3, "every length checked"):
 * - Monocypher's Ed25519 verification accepts an R that encodes y = 1 (or y = p - 1) with the sign bit of x set;
 *   x is 0 there, so RFC 8032 5.1.3 says the decoding fails (Wycheproof ed25519 tcId 151). Not a forgery, a second
 *   encoding of a valid signature; refused here before Monocypher is called.
 * - BearSSL's ECDSA verification of DER signatures takes BER (long-form lengths, missing or extra leading zeros:
 *   Wycheproof ecdsa tcIds 6, 8, 84, 128); its raw form takes signatures of any even length. Again no forgery, but
 *   a signature has exactly one valid encoding here.
 */
#include <string.h>
#include "zc.h"
#include "zc_verify.h"
#include "../monocypher/monocypher-ed25519.h"

int zc_ed25519_check(const unsigned char sig[64], const unsigned char pub[32], const unsigned char *msg, size_t n)
{
    static const unsigned char one[32] = { 1 };
    static const unsigned char pm1[32] = {          /* p - 1 = 2^255 - 20, little-endian */
        0xec, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f };
    unsigned char y[32];

    if (sig[31] & 0x80) {                           /* the sign bit of x set: x must not be 0 */
        memcpy(y, sig, 32);
        y[31] &= 0x7f;
        if (!memcmp(y, one, 32) || !memcmp(y, pm1, 32))
            return -1;
    }
    return crypto_ed25519_check(sig, pub, msg, n) ? -1 : 0;
}

/* one DER INTEGER, positive and minimal; returns the bytes taken or 0 */
static size_t der_int(const unsigned char *p, size_t n)
{
    size_t l;

    if (n < 3 || p[0] != 0x02)
        return 0;
    l = p[1];
    if (l == 0 || l > 127 || l > n - 2)
        return 0;
    if (p[2] & 0x80)                                /* negative */
        return 0;
    if (p[2] == 0 && (l == 1 || !(p[3] & 0x80)))    /* a leading zero that is not needed (zero itself fails too) */
        return 0;
    return l + 2;
}

int zc_ecdsa_der_strict(const unsigned char *sig, size_t n)
{
    size_t body, off, a, b;

    if (n < 8 || sig[0] != 0x30)
        return 0;
    if (sig[1] < 0x80) {
        body = sig[1];
        off = 2;
    } else if (sig[1] == 0x81 && n > 3 && sig[2] >= 0x80) {    /* long form only when needed (P-521) */
        body = sig[2];
        off = 3;
    } else {
        return 0;
    }
    if (off + body != n)
        return 0;
    a = der_int(sig + off, n - off);
    if (!a)
        return 0;
    b = der_int(sig + off + a, n - off - a);
    return b && off + a + b == n;
}

static size_t field_len(int curve)
{
    switch (curve) {
    case BR_EC_secp256r1: return 32;
    case BR_EC_secp384r1: return 48;
    case BR_EC_secp521r1: return 66;
    default: return 0;
    }
}

uint32_t zc_ecdsa_vrfy_asn1(const br_ec_impl *impl, const void *hash, size_t hash_len, const br_ec_public_key *pk,
                            const void *sig, size_t sig_len)
{
    if (!zc_ecdsa_der_strict(sig, sig_len))
        return 0;
    return br_ecdsa_i15_vrfy_asn1(impl, hash, hash_len, pk, sig, sig_len);
}

uint32_t zc_ecdsa_vrfy_raw(const br_ec_impl *impl, const void *hash, size_t hash_len, const br_ec_public_key *pk,
                           const void *sig, size_t sig_len)
{
    size_t f = field_len(pk->curve);

    if (!f || sig_len != 2 * f)
        return 0;
    return br_ecdsa_i15_vrfy_raw(impl, hash, hash_len, pk, sig, sig_len);
}
