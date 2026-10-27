/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the entropy pool (kernel/random.c, DESIGN D19 9.3): BLAKE2s against RFC 7693 (the "abc" vector of
 * appendix B and the self-test of appendix E, keyed and unkeyed, every length), ChaCha20's block function against
 * RFC 8439 (2.3.2 and appendix A.1 #1, #2), the crediting rules (eighths of a bit, the 256-bit seeding, nothing
 * credited after it changes the state) and the seed file's rule (user/randseed.h). Run by tools/random-host-test.sh. */
#include <stdio.h>
#include <string.h>
#define RANDOM_HOST 1
#include "../kernel/random.c"
#define RANDSEED_HOST 1
#include "../user/randseed.h"

static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        fails++;
}

static int hexeq(const u8 *got, const char *hex, int n)
{
    for (int i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1 || got[i] != v)
            return 0;
    }
    return 1;
}

/* RFC 7693 appendix E */
static void selftest_seq(u8 *out, u32 len, u32 seed)
{
    u32 a = 0xDEAD4BADu * seed, b = 1;
    for (u32 i = 0; i < len; i++) {
        u32 t = a + b;
        a = b;
        b = t;
        out[i] = (u8)(t >> 24);
    }
}

static int blake2s_selftest(void)
{
    static const u8 res[32] = { 0x6A, 0x41, 0x1F, 0x08, 0xCE, 0x25, 0xAD, 0xCD, 0xFB, 0x02, 0xAB, 0xA6, 0x41, 0x45, 0x1C,
                                0xEC, 0x53, 0xC5, 0x98, 0xB2, 0x4F, 0x4F, 0xC7, 0x87, 0xFB, 0xDC, 0x88, 0x79, 0x7F, 0x4C,
                                0x1D, 0xFE };
    static const u32 md_len[4] = { 16, 20, 28, 32 }, in_len[6] = { 0, 3, 64, 65, 255, 1024 };
    u8 in[1024], md[32], key[32];
    struct b2s ctx;
    b2s_init(&ctx, 32, 0, 0);
    for (int i = 0; i < 4; i++) {
        u32 outlen = md_len[i];
        for (int j = 0; j < 6; j++) {
            u32 inlen = in_len[j];
            selftest_seq(in, inlen, inlen);
            b2s(md, outlen, 0, 0, in, inlen);
            b2s_update(&ctx, md, outlen);
            selftest_seq(key, outlen, outlen);
            b2s(md, outlen, key, outlen, in, inlen);
            b2s_update(&ctx, md, outlen);
        }
    }
    b2s_final(&ctx, md);
    return !memcmp(md, res, 32);
}

int main(void)
{
    u8 h[32], blk[64], key[32], nonce[12];
    b2s(h, 32, 0, 0, "abc", 3);
    check(hexeq(h, "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982", 32),
          "BLAKE2s-256(\"abc\") (RFC 7693 appendix B)");
    check(blake2s_selftest(), "BLAKE2s self-test: keyed and unkeyed, lengths 0-1024, outputs 16-32 (RFC 7693 appendix E)");
    {   /* byte-by-byte updates give the same hash as one update (the pool mixes a sample at a time) */
        u8 in[200], a[32], b2[32];
        for (int i = 0; i < 200; i++) in[i] = (u8)(i * 7);
        b2s(a, 32, 0, 0, in, 200);
        struct b2s s;
        b2s_init(&s, 32, 0, 0);
        for (int i = 0; i < 200; i++) b2s_update(&s, in + i, 1);
        b2s_final(&s, b2);
        check(!memcmp(a, b2, 32), "BLAKE2s: one byte at a time is the same as all at once");
    }
    for (int i = 0; i < 32; i++) key[i] = (u8)i;
    memcpy(nonce, "\0\0\0\x09\0\0\0\x4a\0\0\0\0", 12);
    chacha20_block(key, 1, nonce, blk);
    check(hexeq(blk, "10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
                     "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e", 64),
          "ChaCha20 block (RFC 8439 2.3.2)");
    memset(key, 0, 32);
    memset(nonce, 0, 12);
    chacha20_block(key, 0, nonce, blk);
    check(hexeq(blk, "76b8e0ada0f13d90405d6ae55386bd28bdd219b8a08ded1aa836efcc8b770dc7"
                     "da41597c5157488d7724e03fb8d84a376a43b8f41518a11cc387b669b2ee6586", 64),
          "ChaCha20 block, zero key and nonce (RFC 8439 A.1 #1)");
    chacha20_block(key, 1, nonce, blk);
    check(hexeq(blk, "9f07e7be5551387a98ba977c732d080dcb0f29a048e3656912c6533e32ee7aed"
                     "29b721769ce64e43d57133b074d839d531ed1f28510afb45ace10a1f4b794d6f", 64),
          "ChaCha20 block, counter 1 (RFC 8439 A.1 #2)");

    /* the pool: credits in eighths, the seeding at 256 bits */
    b2s_init(&pool, 32, 0, 0);
    check(!rnd_seeded && rnd_credit8 == 0, "a new pool is not seeded");
    int seeded_at = -1;
    for (int i = 0; i < 300; i++) {
        u16 v = (u16)(i * 31);
        if (rnd_mix(&v, 2, RND_CR_KEY) && seeded_at < 0)
            seeded_at = i + 1;
    }
    check(seeded_at == 256, "256 key events (1 bit each) seed the pool, not one fewer");
    check(rnd_credit8 == 256 * 8, "credit stops counting once seeded");
    rnd_seeded = 0;
    rnd_credit8 = 0;
    int n = 0;
    while (!rnd_mix("x", 1, RND_CR_SLIP))
        n++;
    check(n + 1 == 512, "SLIP frame ends: half a bit each (512 for 256 bits)");
    rnd_seeded = 0;
    rnd_credit8 = 0;
    for (int i = 0; i < 1000; i++)
        rnd_mix("m", 1, 0);
    check(!rnd_seeded && rnd_credit8 == 0, "uncredited samples (disk, mouse, frames) never seed it");
    check(rnd_mix("0123456789abcdef0123456789abcdef", 32, 32 * 64), "a 32-byte seed credited by root seeds it at once");

    /* the output: fast key erasure, no two reads alike, every length */
    u8 a[100], b[100];
    rnd_bytes(a, 100, 0);
    rnd_bytes(b, 100, 0);
    check(memcmp(a, b, 100) != 0, "two reads differ");
    u8 k0[32];
    memcpy(k0, crng_key, 32);
    rnd_bytes(a, 1, 0);
    check(memcmp(k0, crng_key, 32) != 0, "every read replaces the key (fast key erasure)");
    u8 big[300];
    memset(big, 0, sizeof big);
    rnd_bytes(big, sizeof big, 0);
    int zeros = 0;
    for (int i = 250; i < 300; i++) zeros += !big[i];
    check(zeros < 10, "a long read fills every byte (300 bytes: the erasure block, then four more)");

    /* the seed file's rule */
    check(seed_credit(1, SEED_F_SEEDED, 1, 1), "seed: credited from a seeded boot, rewritten on a persistent root");
    check(!seed_credit(1, SEED_F_SEEDED, 0, 1), "seed: never credited on a RAM-disk, read-only or rescue root");
    check(!seed_credit(1, SEED_F_SEEDED, 1, 0), "seed: not credited when the rewrite failed");
    check(!seed_credit(1, 0, 1, 1), "seed: not credited when the boot that wrote it was not seeded");
    check(!seed_credit(0, SEED_F_SEEDED, 1, 1), "seed: no file, no credit");
    check(seed_flags(1, 0, 0, 1) == SEED_F_SEEDED && seed_flags(0, 1, SEED_F_SEEDED, 1) == SEED_F_SEEDED &&
          seed_flags(0, 1, 0, 1) == 0 && seed_flags(0, 1, SEED_F_SEEDED, 0) == 0,
          "seed: the new file's flag (seeded now, or a creditable old seed)");
    printf(fails ? "random-host-test: %d FAILED\n" : "random-host-test: all ok\n", fails);
    return fails != 0;
}
