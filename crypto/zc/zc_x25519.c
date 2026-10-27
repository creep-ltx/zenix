/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * X25519 (RFC 7748) through BearSSL's c25519_m15 or Monocypher, with the all-zero result refused (RFC 8422 for TLS,
 * RFC 8731 for ssh; DESIGN D21 3.2: neither library checks it). D21's step 2 times both on the A1200 and keeps the
 * faster one as zc_x25519(); both are tested on the PC against RFC 7748 and each other.
 */
#include <string.h>
#include "zc.h"
#include "../bearssl/inc/bearssl.h"
#include "../monocypher/monocypher.h"

/* 0 when all 32 bytes are zero, constant time */
static int nonzero(const unsigned char b[32])
{
    unsigned char acc = 0;
    int i;

    for (i = 0; i < 32; i++)
        acc |= b[i];
    return (int)((0u - (unsigned)acc) >> 31) & 1;
}

int zc_x25519_bearssl(unsigned char out[32], const unsigned char scalar[32], const unsigned char point[32])
{
    unsigned char k[32];
    int i, ok;

    /* BearSSL takes the scalar as a big-endian number (it clamps k[31] and k[0]), the point as RFC 7748's
       little-endian u-coordinate, which it overwrites with the result (and clears the point's top bit first) */
    for (i = 0; i < 32; i++)
        k[i] = scalar[31 - i];
    memcpy(out, point, 32);
    ok = (int)br_ec_c25519_m15.mul(out, 32, k, 32, BR_EC_curve25519);
    zc_wipe(k, sizeof k);
    if (!ok || !nonzero(out)) {
        zc_wipe(out, 32);
        return -1;
    }
    return 0;
}

int zc_x25519_monocypher(unsigned char out[32], const unsigned char scalar[32], const unsigned char point[32])
{
    crypto_x25519(out, scalar, point);
    if (!nonzero(out)) {
        zc_wipe(out, 32);
        return -1;
    }
    return 0;
}

int zc_x25519(unsigned char out[32], const unsigned char scalar[32], const unsigned char point[32])
{
#ifdef ZC_X25519_MONOCYPHER
    return zc_x25519_monocypher(out, scalar, point);
#else
    return zc_x25519_bearssl(out, scalar, point);
#endif
}

void zc_x25519_public(unsigned char pub[32], const unsigned char scalar[32])
{
#ifdef ZC_X25519_MONOCYPHER
    crypto_x25519_public_key(pub, scalar);
#else
    static const unsigned char base[32] = { 9 };

    (void)zc_x25519_bearssl(pub, scalar, base);  /* 9 is of prime order: never zero for a clamped scalar */
#endif
}
