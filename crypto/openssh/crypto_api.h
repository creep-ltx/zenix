/* SPDX-License-Identifier: GPL-2.0-only */
/* OpenSSH's crypto_api.h, as much as bcrypt_pbkdf.c uses: SHA-512 from Monocypher (DESIGN D21 3.2) */
#ifndef ZENIX_OPENSSH_CRYPTO_API_H
#define ZENIX_OPENSSH_CRYPTO_API_H
#include "../monocypher/monocypher-ed25519.h"
#define crypto_hash_sha512_BYTES 64U
static inline int crypto_hash_sha512(unsigned char *out, const void *in, unsigned long long n)
{
    crypto_sha512(out, (const uint8_t *)in, (size_t)n);
    return 0;
}
#endif
