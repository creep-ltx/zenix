/* SPDX-License-Identifier: GPL-2.0-only */
/* cryptotest: the crypto library on the m68k (DESIGN D21 10 step 2, docs/man/cryptotest.txt).
 *   cryptotest           the known answers (tools/host/crypto-vectors.h, the same the PC tests use): X25519 both
 *                        ways (RFC 7748, the all-zero results refused), Ed25519 (RFC 8032), ChaCha20, Poly1305 and
 *                        the AEAD (RFC 8439), SHA-256/512 and HMAC (FIPS 180, RFC 4231), ssh's packet framing
 *   cryptotest time [N]  the time of each operation by the E-clock, the best of N runs (default 3): BearSSL's and
 *                        Monocypher's X25519 side by side, Ed25519 sign and verify, an ssh login's arithmetic,
 *                        chacha20-poly1305@openssh.com and SHA-256 in KB/s
 *   cryptotest mul       the multiplier check of D21 9.2: 100,000 muls.l and mulu.w with operands 0, 1, all ones
 *                        and random; a time that depends on the operands is said
 * Emulator times are not the machine's: measure on the real A1200 and 060 (docs/a1200-checklist-network.md). */
#include "libz.h"
#include "../crypto/zc/zc.h"
#include "../crypto/bearssl/inc/bearssl.h"
#include "../crypto/monocypher/monocypher.h"
#include "../crypto/monocypher/monocypher-ed25519.h"
#include "../tools/host/crypto-vectors.h"

static int fails, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        dprintf(1, "FAIL cryptotest: %s\n", what);
        fails++;
    }
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static int unhex(unsigned char *out, u32 cap, const char *h)
{
    u32 n = 0;
    if (!strcmp(h, "-"))
        return 0;
    while (h[0] && h[1]) {
        int a = hexval(h[0]), b = hexval(h[1]);
        if (n >= cap || a < 0 || b < 0)
            return -1;
        out[n++] = (unsigned char)(a << 4 | b);
        h += 2;
    }
    return h[0] ? -1 : (int)n;
}

static int eqhex(const unsigned char *b, u32 n, const char *h)
{
    static unsigned char t[600];
    int k = unhex(t, sizeof t, h);
    return k == (int)n && memcmp(t, b, n) == 0;
}

static int x25519(int bear, unsigned char *o, const unsigned char *k, const unsigned char *u)
{
    return bear ? zc_x25519_bearssl(o, k, u) : zc_x25519_monocypher(o, k, u);
}

static void test_x25519(void)
{
    unsigned char k[32], u[32], o[32], t[32], a[32], b[32];
    char msg[96];
    static const char *const zero_u[] = {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    };
    for (int impl = 0; impl < 2; impl++) {
        const char *nm = impl ? "BearSSL" : "Monocypher";
        for (u32 i = 0; i < sizeof x25519_vec / sizeof x25519_vec[0]; i++) {
            unhex(k, 32, x25519_vec[i].k);
            unhex(u, 32, x25519_vec[i].u);
            int r = x25519(impl, o, k, u);
            snprintf(msg, sizeof msg, "X25519 %s: RFC 7748 5.2 vector %u", nm, i + 1);
            check(r == 0 && eqhex(o, 32, x25519_vec[i].out), msg);
        }
        memset(k, 0, 32);
        k[0] = 9;
        memcpy(u, k, 32);
        x25519(impl, o, k, u);
        snprintf(msg, sizeof msg, "X25519 %s: RFC 7748 after one iteration", nm);
        check(eqhex(o, 32, x25519_iter1), msg);
        unhex(a, 32, x25519_dh[0]);
        unhex(b, 32, x25519_dh[2]);
        memset(u, 0, 32);
        u[0] = 9;
        x25519(impl, o, a, u);
        snprintf(msg, sizeof msg, "X25519 %s: RFC 7748 6.1 Alice's public key", nm);
        check(eqhex(o, 32, x25519_dh[1]), msg);
        int r = x25519(impl, t, b, o);
        snprintf(msg, sizeof msg, "X25519 %s: RFC 7748 6.1 the shared secret", nm);
        check(r == 0 && eqhex(t, 32, x25519_dh[4]), msg);
        for (u32 i = 0; i < sizeof zero_u / sizeof zero_u[0]; i++) {
            unhex(u, 32, zero_u[i]);
            memset(o, 0xAA, 32);
            r = x25519(impl, o, a, u);
            memset(t, 0, 32);
            snprintf(msg, sizeof msg, "X25519 %s: small-order point %u gives -1 and zeros", nm, i);
            check(r == -1 && !memcmp(o, t, 32), msg);
        }
    }
    unhex(a, 32, x25519_dh[0]);
    zc_x25519_public(o, a);
    check(eqhex(o, 32, x25519_dh[1]), "zc_x25519_public: RFC 7748 6.1");
}

static void test_ed25519(void)
{
    static unsigned char m[1100];
    char msg[96];
    for (u32 i = 0; i < sizeof ed25519_vec / sizeof ed25519_vec[0]; i++) {
        const struct ed25519v *v = &ed25519_vec[i];
        unsigned char seed[32], pk[32], sk[64], pk2[32], sig[64];
        int mn = unhex(m, sizeof m, v->msg[0] ? v->msg : "-");
        unhex(seed, 32, v->sk);
        unhex(pk, 32, v->pk);
        crypto_ed25519_key_pair(sk, pk2, seed);
        snprintf(msg, sizeof msg, "Ed25519 RFC 8032 TEST %s: public key", v->name);
        check(!memcmp(pk, pk2, 32), msg);
        crypto_ed25519_sign(sig, sk, m, (u32)mn);
        snprintf(msg, sizeof msg, "Ed25519 RFC 8032 TEST %s: signature", v->name);
        check(eqhex(sig, 64, v->sig), msg);
        snprintf(msg, sizeof msg, "Ed25519 RFC 8032 TEST %s: verifies", v->name);
        check(crypto_ed25519_check(sig, pk, m, (u32)mn) == 0, msg);
        sig[i % 64] ^= 1;
        snprintf(msg, sizeof msg, "Ed25519 RFC 8032 TEST %s: a changed signature fails", v->name);
        check(crypto_ed25519_check(sig, pk, m, (u32)mn) != 0, msg);
    }
}

static void test_chacha_poly(void)
{
    static unsigned char o[512], buf[512];
    unsigned char t[16];
    char msg[96];
    for (u32 i = 0; i < sizeof chacha_vec / sizeof chacha_vec[0]; i++) {
        const struct chachav *v = &chacha_vec[i];
        crypto_chacha20_ietf(o, v->in, v->len, v->key, v->nonce, v->counter);
        snprintf(msg, sizeof msg, "ChaCha20 Monocypher: RFC 8439 vector %u", i + 1);
        check(!memcmp(o, v->out, v->len), msg);
        if (v->in) memcpy(o, v->in, v->len); else memset(o, 0, v->len);
        br_chacha20_ct_run(v->key, v->nonce, v->counter, o, v->len);
        snprintf(msg, sizeof msg, "ChaCha20 BearSSL: RFC 8439 vector %u", i + 1);
        check(!memcmp(o, v->out, v->len), msg);
    }
    for (u32 i = 0; i < sizeof poly_vec / sizeof poly_vec[0]; i++) {
        const struct polyv *v = &poly_vec[i];
        crypto_poly1305(t, v->msg, v->len, v->key);
        snprintf(msg, sizeof msg, "Poly1305 Monocypher: RFC 8439 A.3 vector %u", i + 1);
        check(!memcmp(t, v->tag, 16), msg);
    }
    for (u32 i = 0; i < sizeof aead_vec / sizeof aead_vec[0]; i++) {
        const struct aeadv *v = &aead_vec[i];
        crypto_aead_ctx c;
        memcpy(buf, v->ct, v->len);
        crypto_aead_init_ietf(&c, v->key, v->nonce);
        int r = crypto_aead_read(&c, buf, v->tag, v->aad, v->aadlen, buf, v->len);
        snprintf(msg, sizeof msg, "AEAD Monocypher: RFC 8439 vector %u opens", i + 1);
        check(r == 0 && (!v->pt || !memcmp(buf, v->pt, v->len)), msg);
        memcpy(buf, v->ct, v->len);
        buf[v->len / 2] ^= 4;
        crypto_aead_init_ietf(&c, v->key, v->nonce);
        snprintf(msg, sizeof msg, "AEAD Monocypher: RFC 8439 vector %u changed fails", i + 1);
        check(crypto_aead_read(&c, buf, v->tag, v->aad, v->aadlen, buf, v->len) != 0, msg);
    }
}

static void test_sha_hmac(void)
{
    unsigned char out[64];
    static unsigned char k[200], d[200];
    char msg[96];
    br_sha256_context s;
    br_sha512_context s5;
    br_sha256_init(&s);
    br_sha256_update(&s, "abc", 3);
    br_sha256_out(&s, out);
    check(eqhex(out, 32, sha_vec[1].abc), "SHA-256: FIPS 180 \"abc\"");
    br_sha256_init(&s);
    br_sha256_update(&s, sha_m448, strlen(sha_m448));
    br_sha256_out(&s, out);
    check(eqhex(out, 32, sha_vec[1].mid), "SHA-256: FIPS 180 two-block message");
    br_sha512_init(&s5);
    br_sha512_update(&s5, sha_m896, strlen(sha_m896));
    br_sha512_out(&s5, out);
    check(eqhex(out, 64, sha_vec[3].mid), "SHA-512 BearSSL: FIPS 180 two-block message");
    crypto_sha512(out, (const unsigned char *)"abc", 3);
    check(eqhex(out, 64, sha_vec[3].abc), "SHA-512 Monocypher: FIPS 180 \"abc\"");
    for (u32 i = 0; i < sizeof hmac_vec / sizeof hmac_vec[0]; i++) {
        const struct hmacv *v = &hmac_vec[i];
        br_hmac_key_context hk;
        br_hmac_context hc;
        int kn = unhex(k, sizeof k, v->key), dn = unhex(d, sizeof d, v->data);
        br_hmac_key_init(&hk, &br_sha256_vtable, k, (u32)kn);
        br_hmac_init(&hc, &hk, 0);
        br_hmac_update(&hc, d, (u32)dn);
        br_hmac_out(&hc, out);
        snprintf(msg, sizeof msg, "HMAC-SHA-256: RFC 4231 test case %d", v->tc);
        check(eqhex(out, 32, v->s256), msg);
    }
}

static void test_ssh(void)
{
    unsigned char key[64], pkt[64], save[64], tag[16];
    struct zc_chapol c;
    for (int n = 0; n < 64; n++)
        key[n] = (unsigned char)n;
    zc_chapol_init(&c, key);
    memset(pkt, 0, sizeof pkt);
    pkt[3] = 28;
    pkt[4] = 8;
    memcpy(pkt + 5, "hello, ssh world!!!!", 20);
    memcpy(save, pkt, 32);
    zc_chapol_seal(&c, 7, pkt, 28, tag);
    check(zc_chapol_length(&c, 7, pkt) == 28, "chapol: the length decrypts");
    tag[0] ^= 1;
    check(zc_chapol_open(&c, 7, pkt, 28, tag) == -1, "chapol: a wrong tag fails");
    tag[0] ^= 1;
    check(zc_chapol_open(&c, 7, pkt, 28, tag) == 0 && !memcmp(pkt + 4, save + 4, 28), "chapol: round trip");
    zc_chapol_wipe(&c);
}

/* ---- times: the E-clock (CIA, SYS_hrtime), its rate measured against the 100 Hz tick ---- */
static u32 eclock_hz;

static void calibrate(void)
{
    u32 t = sys_ticks();
    while (sys_ticks() == t)
        ;
    u32 e0 = sys_hrtime(), t0 = sys_ticks();
    while (sys_ticks() - t0 < 100)
        ;
    eclock_hz = sys_hrtime() - e0;              /* one second (100 ticks) of E-clock */
}

static u32 us_of(u32 e) { return (u32)((unsigned long long)e * 1000000u / eclock_hz); }

static void say_time(const char *what, u32 best, u32 bytes)
{
    u32 us = us_of(best);
    if (bytes)
        dprintf(1, "time %-34s %7u.%03u ms  %u KB/s\n", what, us / 1000, us % 1000,
               us ? (u32)((unsigned long long)bytes * 1000000u / us / 1024) : 0);
    else
        dprintf(1, "time %-34s %7u.%03u ms\n", what, us / 1000, us % 1000);
}

#define TIMED(what, bytes, body) do { u32 best_ = ~0u; for (int r_ = 0; r_ < runs; r_++) { u32 e_ = sys_hrtime(); \
    body; e_ = sys_hrtime() - e_; if (e_ < best_) best_ = e_; } say_time(what, best_, bytes); } while (0)

static void times(int runs)
{
    static unsigned char buf[16384 + 64];
    unsigned char k[32], u[32], o[32], sk[64], pk[32], sig[64], seed[32], tag[16], key[64];
    calibrate();
    dprintf(1, "cryptotest: E-clock %u Hz; best of %d run%s (an emulator's times are not the machine's)\n", eclock_hz,
           runs, runs == 1 ? "" : "s");
    memset(k, 0x42, 32);
    memset(u, 0, 32);
    u[0] = 9;
    TIMED("X25519 BearSSL c25519_m15", 0, zc_x25519_bearssl(o, k, u));
    TIMED("X25519 Monocypher", 0, zc_x25519_monocypher(o, k, u));
    memset(seed, 7, 32);
    crypto_ed25519_key_pair(sk, pk, seed);
    TIMED("Ed25519 sign (64 bytes)", 0, crypto_ed25519_sign(sig, sk, buf, 64));
    TIMED("Ed25519 verify (64 bytes)", 0, crypto_ed25519_check(sig, pk, buf, 64));
    struct zc_chapol c;
    memset(key, 3, 64);
    zc_chapol_init(&c, key);
    buf[3] = 0x40; buf[2] = 0;                  /* a 16 KB packet */
    TIMED("chacha20-poly1305 seal 16 KB", 16384, zc_chapol_seal(&c, 1, buf, 16384, tag));
    TIMED("chacha20-poly1305 open 16 KB", 16384, zc_chapol_open(&c, 1, buf, 16384, tag));
    br_sha256_context s;
    TIMED("SHA-256 16 KB", 16384, (br_sha256_init(&s), br_sha256_update(&s, buf, 16384), br_sha256_out(&s, o)));
    zc_chapol_wipe(&c);
}

/* ---- the multiplier (D21 9.2): does muls.l or mulu.w take longer for some operands? ---- */
static u32 mul_run(u32 a, u32 b, int wide)
{
    u32 e = sys_hrtime(), x = a;
    if (wide)
        __asm__ volatile("move.l #9999,%%d1\n1:\n"
                         "move.l %1,%0\n muls.l %2,%0\n move.l %1,%0\n muls.l %2,%0\n move.l %1,%0\n muls.l %2,%0\n"
                         "move.l %1,%0\n muls.l %2,%0\n move.l %1,%0\n muls.l %2,%0\n move.l %1,%0\n muls.l %2,%0\n"
                         "move.l %1,%0\n muls.l %2,%0\n move.l %1,%0\n muls.l %2,%0\n move.l %1,%0\n muls.l %2,%0\n"
                         "move.l %1,%0\n muls.l %2,%0\n dbra %%d1,1b"
                         : "=&d"(x) : "d"(a), "d"(b) : "d1", "cc");
    else
        __asm__ volatile("move.l #9999,%%d1\n1:\n"
                         "move.l %1,%0\n mulu.w %2,%0\n move.l %1,%0\n mulu.w %2,%0\n move.l %1,%0\n mulu.w %2,%0\n"
                         "move.l %1,%0\n mulu.w %2,%0\n move.l %1,%0\n mulu.w %2,%0\n move.l %1,%0\n mulu.w %2,%0\n"
                         "move.l %1,%0\n mulu.w %2,%0\n move.l %1,%0\n mulu.w %2,%0\n move.l %1,%0\n mulu.w %2,%0\n"
                         "move.l %1,%0\n mulu.w %2,%0\n dbra %%d1,1b"
                         : "=&d"(x) : "d"(a), "d"(b) : "d1", "cc");
    (void)x;
    return sys_hrtime() - e;
}

static void mul_check(void)
{
    static const u32 ops[4][2] = { { 0, 0 }, { 1, 1 }, { 0xFFFFFFFFu, 0xFFFFFFFFu }, { 0x9E3779B9u, 0x7F4A7C15u } };
    static const char *const names[4] = { "0 x 0", "1 x 1", "all ones", "random" };
    calibrate();
    dprintf(1, "cryptotest: 100,000 multiplies each (E-clock %u Hz; an emulator's times are not the machine's)\n",
           eclock_hz);
    for (int wide = 1; wide >= 0; wide--) {
        u32 lo = ~0u, hi = 0;
        for (int i = 0; i < 4; i++) {
            u32 best = ~0u;
            for (int r = 0; r < 3; r++) {
                u32 t = mul_run(ops[i][0], ops[i][1], wide);
                if (t < best) best = t;
            }
            u32 us = us_of(best);
            dprintf(1, "mul %s %-9s %6u us\n", wide ? "muls.l" : "mulu.w", names[i], us);
            if (best < lo) lo = best;
            if (best > hi) hi = best;
        }
        dprintf(1, "mul %s: %s (spread %u%%)\n", wide ? "muls.l" : "mulu.w",
               hi - lo <= lo / 50 ? "the same time for every operand" : "THE TIME DEPENDS ON THE OPERANDS",
               lo ? (hi - lo) * 100 / lo : 0);
    }
}

static const char USAGE[] = "usage: cryptotest [time [N] | mul]\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    if (argc >= 2 && !strcmp(argv[1], "time")) {
        int n = argc >= 3 ? atoi(argv[2]) : 3;
        times(n > 0 && n < 100 ? n : 3);
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "mul")) {
        mul_check();
        return 0;
    }
    if (argc != 1) zusage(USAGE);
    test_x25519();
    test_ed25519();
    test_chacha_poly();
    test_sha_hmac();
    test_ssh();
    dprintf(1, "cryptotest: %d checks, %d failed: %s\n", checks, fails, fails ? "FAIL" : "ok");
    return fails != 0;
}
