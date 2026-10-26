/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* SHA-256 (FIPS 180-4, user/sha256.c): one call for a buffer, or init/update/final for data that comes in pieces.
 * Plain C types only: zenixboot (an AmigaOS program with its own u32) uses it too. */
struct sha256 { unsigned int h[8], len, n; unsigned char buf[64]; };
void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, unsigned int len);
void sha256_final(struct sha256 *c, unsigned char out[32]);
void sha256(const void *data, unsigned int len, unsigned char out[32]);
