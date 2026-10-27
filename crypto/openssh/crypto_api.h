/* SPDX-License-Identifier: GPL-2.0-only */
/* OpenSSH's crypto_api.h, as much as bcrypt_pbkdf.c uses: SHA-512 from Monocypher (DESIGN D21 3.2) */
#ifndef ZENIX_OPENSSH_CRYPTO_API_H
#define ZENIX_OPENSSH_CRYPTO_API_H
#include "../monocypher/monocypher-ed25519.h"
#define crypto_hash_sha512_BYTES 64U
#define crypto_hash_sha512(out, in, n) (crypto_sha512((out), (const uint8_t *)(in), (n)), 0)
#endif
