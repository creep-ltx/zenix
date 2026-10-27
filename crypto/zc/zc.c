/* SPDX-License-Identifier: GPL-2.0-only */
/* crypto/zc/: wiping and the per-process random generator (DESIGN D21 4.2, 5, 9.1) */
#include <string.h>
#include "zc.h"
#include "../monocypher/monocypher.h"

void zc_wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;

    while (n--)
        *v++ = 0;
}

int zc_rng_seed(struct zc_rng *r, zc_seedfn seed, void *u)
{
    int e;

    zc_rng_wipe(r);
    e = seed(u, r->key, sizeof r->key);
    if (e < 0) {
        zc_rng_wipe(r);
        return e;
    }
    r->ready = 1;
    return 0;
}

/* One ChaCha20 block (DJB's form, nonce 0, counter 0) under the current key: its first 32 bytes are the next key
   (fast key erasure: the key that made earlier output is gone), its last 32 the output. */
static void refill(struct zc_rng *r)
{
    static const unsigned char zero8[8];
    unsigned char block[64];

    crypto_chacha20_djb(block, 0, sizeof block, r->key, zero8, 0);
    memcpy(r->key, block, 32);
    memcpy(r->buf, block + 32, 32);
    zc_wipe(block, sizeof block);
    r->left = 32;
}

int zc_rng_bytes(struct zc_rng *r, void *out, size_t n)
{
    unsigned char *o = out;

    if (!r->ready) {
        memset(out, 0, n);
        return -1;
    }
    while (n) {
        size_t k;

        if (!r->left)
            refill(r);
        k = n < r->left ? n : r->left;
        memcpy(o, r->buf + 32 - r->left, k);
        zc_wipe(r->buf + 32 - r->left, k);
        r->left -= (unsigned)k;
        o += k;
        n -= k;
    }
    return 0;
}

void zc_rng_wipe(struct zc_rng *r)
{
    zc_wipe(r, sizeof *r);
}
