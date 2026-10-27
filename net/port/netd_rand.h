/* SPDX-License-Identifier: GPL-2.0-only */
/* netd's random numbers (net/port/rand.c, DESIGN D19 9.3) */
#ifndef ZENIX_NETD_RAND_H
#define ZENIX_NETD_RAND_H
#include "lwip/arch.h"

#define NETD_SEED_BYTES 48      /* 32 for ChaCha20's key, 16 for the ISN key */
void netd_rand_seed(const u8_t seed[NETD_SEED_BYTES]);   /* mixed into the keys; call again to reseed */
u32_t netd_rand(void);
/* the primitives, for the tests against RFC 8439 and the SipHash paper */
void netd_chacha20_block(const u32_t key[8], u32_t counter, const u32_t nonce[3], u8_t block[64]);
void netd_siphash24(const u32_t key[4], const u8_t *m, unsigned len, u8_t out[8]);
#endif
