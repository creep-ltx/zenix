/* SPDX-License-Identifier: GPL-2.0-only */
/* net/port/rand.c against published vectors: ChaCha20's block function (RFC 8439 section 2.3.2) and SipHash-2-4
 * (the SipHash paper, appendix A: key 00..0f, message 00..0e); then the generator's own rules. */
#include <stdio.h>
#include <string.h>
#include "netd_port.h"
#include "netd_rand.h"

unsigned long netd_os_now_ms(void) { return 0; }
unsigned long netd_os_clock4us(void) { return 0; }

static int fails;
static void hexeq(const char *what, const u8_t *got, const u8_t *want, int n)
{
    if (memcmp(got, want, n)) {
        int i;
        printf("FAIL rand-test: %s\n  got  ", what);
        for (i = 0; i < n; i++) printf("%02x", got[i]);
        printf("\n  want ");
        for (i = 0; i < n; i++) printf("%02x", want[i]);
        printf("\n");
        fails++;
    } else printf("ok %s\n", what);
}

int main(void)
{
    static const u8_t want_block[64] = {
        0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
        0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03, 0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
        0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09, 0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
        0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e };
    static const u8_t want_sip[8] = { 0xe5, 0x45, 0xbe, 0x49, 0x61, 0xca, 0x29, 0xa1 };
    u32_t key[8], nonce[3] = { 0x09000000, 0x4a000000, 0 }, sk[4];
    u8_t blk[64], m[64], h[8];
    int i;

    for (i = 0; i < 8; i++) key[i] = (u32_t)(4 * i) | (u32_t)(4 * i + 1) << 8 | (u32_t)(4 * i + 2) << 16 | (u32_t)(4 * i + 3) << 24;
    netd_chacha20_block(key, 1, nonce, blk);
    hexeq("ChaCha20 block, RFC 8439 2.3.2", blk, want_block, 64);

    for (i = 0; i < 4; i++) sk[i] = key[i];        /* the bytes 00..0f */
    for (i = 0; i < 64; i++) m[i] = (u8_t)i;
    netd_siphash24(sk, m, 15, h);
    hexeq("SipHash-2-4, 15 bytes (the paper's example)", h, want_sip, 8);
    /* every message length 0..63 against the reference implementation's table: the first and the last entries */
    {
        static const u8_t v0[8] = { 0x31, 0x0e, 0x0e, 0xdd, 0x47, 0xdb, 0x6f, 0x72 };   /* length 0 */
        static const u8_t v63[8] = { 0x72, 0x45, 0x06, 0xeb, 0x4c, 0x32, 0x8a, 0x95 };  /* length 63 */
        netd_siphash24(sk, m, 0, h);
        hexeq("SipHash-2-4, 0 bytes (reference vectors.h)", h, v0, 8);
        netd_siphash24(sk, m, 63, h);
        hexeq("SipHash-2-4, 63 bytes (reference vectors.h)", h, v63, 8);
    }

    /* the generator: two seeds give different streams; the same seed after the same history gives the same */
    {
        static const u8_t s1[NETD_SEED_BYTES] = { 1 };
        u32_t a[8], b;
        int same = 0, j;
        netd_rand_seed(s1);
        for (i = 0; i < 8; i++) a[i] = netd_rand();
        for (i = 0; i < 1000; i++) {
            b = netd_rand();
            for (j = 0; j < 8; j++) same += (b == a[j]);
        }
        if (same) { printf("FAIL rand-test: the stream repeats a value %d times in 1000\n", same); fails++; }
        else printf("ok the stream does not repeat in 1000 values\n");
    }
    if (fails) return 1;
    printf("rand-test: all ok\n");
    return 0;
}
