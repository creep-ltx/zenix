/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* SHA-256 (FIPS 180-4, user/sha.c): one call for a buffer, or init/update/final for data that comes in pieces */
struct sha256 { u32 h[8]; u32 len, n; unsigned char buf[64]; };
void sha256_init(struct sha256 *c);
void sha256_update(struct sha256 *c, const void *data, u32 len);
void sha256_final(struct sha256 *c, unsigned char out[32]);
void sha256(const void *data, u32 len, unsigned char out[32]);
