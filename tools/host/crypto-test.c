/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The crypto library on the PC (DESIGN D21 10 step 0; tools/host/crypto-test.sh runs it):
 *   crypto-test vectors      the known answers of tools/host/crypto-vectors.h (RFC 7748 with the 1,000-iteration
 *                            check and the all-zero cases, RFC 8032, RFC 8439, RFC 4231, FIPS 180-4, FIPS 197) on
 *                            both X25519 implementations, BearSSL's and Monocypher's ChaCha20 and Poly1305, the
 *                            random generator's erasure, zc's mpint, key derivation and ssh packet framing
 *   crypto-test long         also RFC 7748's 1,000,000 iterations (minutes)
 *   crypto-test oracle       commands on standard input, answers on standard output, one line each (hex; "-" is
 *                            empty), for tools/host/crypto-test.py: Wycheproof's vectors and the differential runs
 *                            against the PC's OpenSSL
 * Prints FAIL lines and exits 1 when something is wrong.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../crypto/zc/zc.h"
#include "../../crypto/zc/zc_verify.h"
#include "../../crypto/bearssl/inc/bearssl.h"
#include "../../crypto/monocypher/monocypher.h"
#include "../../crypto/monocypher/monocypher-ed25519.h"
#include "crypto-vectors.h"

static int fails, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        printf("FAIL crypto-test: %s\n", what);
        fails++;
    }
}

static int unhex(unsigned char *out, size_t cap, const char *h)
{
    size_t n = 0;

    if (!strcmp(h, "-"))
        return 0;
    while (h[0] && h[1]) {
        unsigned v;

        if (n >= cap || sscanf(h, "%2x", &v) != 1)
            return -1;
        out[n++] = (unsigned char)v;
        h += 2;
    }
    return h[0] ? -1 : (int)n;
}

static void hexout(const unsigned char *b, size_t n)
{
    size_t i;

    if (!n)
        putchar('-');
    for (i = 0; i < n; i++)
        printf("%02x", b[i]);
}

static int eqhex(const unsigned char *b, size_t n, const char *h)
{
    unsigned char t[512];
    int k = unhex(t, sizeof t, h);

    return k == (int)n && memcmp(t, b, n) == 0;
}

/* ---- hashes by name (BearSSL; "sha512m" is Monocypher's) ---- */
static const br_hash_class *hashclass(const char *a)
{
    if (!strcmp(a, "sha1")) return &br_sha1_vtable;
    if (!strcmp(a, "sha224")) return &br_sha224_vtable;
    if (!strcmp(a, "sha256")) return &br_sha256_vtable;
    if (!strcmp(a, "sha384")) return &br_sha384_vtable;
    if (!strcmp(a, "sha512")) return &br_sha512_vtable;
    return 0;
}

static int hashrun(const char *a, const unsigned char *m, size_t n, unsigned char *out)
{
    const br_hash_class *c;
    br_hash_compat_context x;

    if (!strcmp(a, "sha512m")) {
        crypto_sha512(out, m, n);
        return 64;
    }
    c = hashclass(a);
    if (!c)
        return -1;
    c->init(&x.vtable);
    c->update(&x.vtable, m, n);
    c->out(&x.vtable, out);
    return (int)((c->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK);
}

static const unsigned char *hashoid(const char *a)
{
    if (!strcmp(a, "sha1")) return BR_HASH_OID_SHA1;
    if (!strcmp(a, "sha224")) return BR_HASH_OID_SHA224;
    if (!strcmp(a, "sha256")) return BR_HASH_OID_SHA256;
    if (!strcmp(a, "sha384")) return BR_HASH_OID_SHA384;
    if (!strcmp(a, "sha512")) return BR_HASH_OID_SHA512;
    return 0;
}

/* ---- ChaCha20 (RFC 8439's 96-bit nonce form) both ways ---- */
static void chacha_ietf_mono(unsigned char *o, const unsigned char *i, size_t n, const unsigned char *k,
                             const unsigned char *nn, unsigned ctr)
{
    crypto_chacha20_ietf(o, i, n, k, nn, ctr);
}

static void chacha_ietf_bear(unsigned char *o, const unsigned char *i, size_t n, const unsigned char *k,
                             const unsigned char *nn, unsigned ctr)
{
    if (i)
        memcpy(o, i, n);
    else
        memset(o, 0, n);
    br_chacha20_ct_run(k, nn, ctr, o, n);
}

/* AEAD_CHACHA20_POLY1305 (RFC 8439 2.8): Monocypher's crypto_aead_lock (IETF nonce) and BearSSL's poly1305_ctmul32 */
static void aead_seal(int bear, const unsigned char *k, const unsigned char *nn, const unsigned char *aad, size_t an,
                      unsigned char *buf, size_t n, unsigned char tag[16])
{
    if (bear) {
        br_poly1305_ctmul32_run(k, nn, buf, n, aad, an, tag, br_chacha20_ct_run, 1);
    } else {
        crypto_aead_ctx c;

        crypto_aead_init_ietf(&c, k, nn);
        crypto_aead_write(&c, buf, tag, aad, an, buf, n);
        crypto_wipe(&c, sizeof c);
    }
}

static int aead_open(int bear, const unsigned char *k, const unsigned char *nn, const unsigned char *aad, size_t an,
                     unsigned char *buf, size_t n, const unsigned char tag[16])
{
    if (bear) {
        unsigned char t[16];

        br_poly1305_ctmul32_run(k, nn, buf, n, aad, an, t, br_chacha20_ct_run, 0);
        if (crypto_verify16(t, tag)) {
            return -1;              /* BearSSL decrypts first; the oracle only reports */
        }
        return 0;
    } else {
        crypto_aead_ctx c;
        int r;

        crypto_aead_init_ietf(&c, k, nn);
        r = crypto_aead_read(&c, buf, tag, aad, an, buf, n);
        crypto_wipe(&c, sizeof c);
        return r;
    }
}

/* AES-GCM: aes_ct + ghash_ctmul32 (D21 3.2) */
static int gcm(int enc, const unsigned char *k, size_t kn, const unsigned char *iv, size_t ivn,
               const unsigned char *aad, size_t an, unsigned char *buf, size_t n, unsigned char *tag, size_t tn)
{
    br_aes_ct_ctr_keys ak;
    br_gcm_context g;
    int ok = 1;

    br_aes_ct_ctr_init(&ak, k, kn);
    br_gcm_init(&g, &ak.vtable, br_ghash_ctmul32);
    br_gcm_reset(&g, iv, ivn);
    br_gcm_aad_inject(&g, aad, an);
    br_gcm_flip(&g);
    br_gcm_run(&g, enc, buf, n);
    if (enc)
        br_gcm_get_tag_trunc(&g, tag, tn);
    else
        ok = (int)br_gcm_check_tag_trunc(&g, tag, tn);
    return ok;
}

/* ---- the known answers ---- */
static void x25519_both(int bear, unsigned char *out, const unsigned char *k, const unsigned char *u, int *r)
{
    *r = bear ? zc_x25519_bearssl(out, k, u) : zc_x25519_monocypher(out, k, u);
}

static void test_x25519(int longrun)
{
    unsigned char k[32], u[32], o[32], t[32], a[32], b[32];
    char msg[128];
    int impl, i, r;
    static const char *const zero_u[] = {        /* points of small order, non-canonical forms, the top bit set (masked) */
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "0000000000000000000000000000000000000000000000000000000000000080",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b880",
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f11d7",
    };

    for (impl = 0; impl < 2; impl++) {
        const char *nm = impl ? "BearSSL" : "Monocypher";

        for (i = 0; i < (int)(sizeof x25519_vec / sizeof x25519_vec[0]); i++) {
            unhex(k, 32, x25519_vec[i].k);
            unhex(u, 32, x25519_vec[i].u);
            x25519_both(impl, o, k, u, &r);
            snprintf(msg, sizeof msg, "X25519 %s: RFC 7748 5.2 vector %d", nm, i + 1);
            check(r == 0 && eqhex(o, 32, x25519_vec[i].out), msg);
        }
        /* the iterations: k = u = 9; then k, u = X25519(k, u), k */
        memset(k, 0, 32);
        k[0] = 9;
        memcpy(u, k, 32);
        for (i = 1; i <= (longrun ? 1000000 : 1000); i++) {
            x25519_both(impl, o, k, u, &r);
            memcpy(u, k, 32);
            memcpy(k, o, 32);
            if (i == 1) {
                snprintf(msg, sizeof msg, "X25519 %s: RFC 7748 after one iteration", nm);
                check(eqhex(k, 32, x25519_iter1), msg);
            }
            if (i == 1000) {
                snprintf(msg, sizeof msg, "X25519 %s: RFC 7748 after 1,000 iterations", nm);
                check(eqhex(k, 32, x25519_iter1000), msg);
            }
        }
        if (longrun) {
            snprintf(msg, sizeof msg, "X25519 %s: RFC 7748 after 1,000,000 iterations", nm);
            check(eqhex(k, 32, x25519_iter1m), msg);
        }
        /* RFC 7748 6.1: the Diffie-Hellman example */
        unhex(a, 32, x25519_dh[0]);
        unhex(b, 32, x25519_dh[2]);
        memset(u, 0, 32);
        u[0] = 9;
        x25519_both(impl, o, a, u, &r);
        snprintf(msg, sizeof msg, "X25519 %s: RFC 7748 6.1 Alice's public key", nm);
        check(r == 0 && eqhex(o, 32, x25519_dh[1]), msg);
        x25519_both(impl, t, b, o, &r);
        snprintf(msg, sizeof msg, "X25519 %s: RFC 7748 6.1 the shared secret", nm);
        check(r == 0 && eqhex(t, 32, x25519_dh[4]), msg);
        /* the all-zero results are refused, and out is wiped */
        for (i = 0; i < (int)(sizeof zero_u / sizeof zero_u[0]); i++) {
            unhex(u, 32, zero_u[i]);
            memset(o, 0xAA, 32);
            x25519_both(impl, o, a, u, &r);
            snprintf(msg, sizeof msg, "X25519 %s: small-order point %d gives -1 and zeros", nm, i);
            memset(t, 0, 32);
            check(r == -1 && memcmp(o, t, 32) == 0, msg);
        }
    }
    /* zc_x25519_public agrees with RFC 7748 6.1 */
    unhex(a, 32, x25519_dh[0]);
    zc_x25519_public(o, a);
    check(eqhex(o, 32, x25519_dh[1]), "zc_x25519_public: RFC 7748 6.1");
}

static void test_ed25519(void)
{
    int i;
    char msg[128];

    for (i = 0; i < (int)(sizeof ed25519_vec / sizeof ed25519_vec[0]); i++) {
        const struct ed25519v *v = &ed25519_vec[i];
        unsigned char seed[32], pk[32], sk[64], pk2[32], sig[64], m[1100];
        int mn = unhex(m, sizeof m, v->msg[0] ? v->msg : "-");

        unhex(seed, 32, v->sk);
        unhex(pk, 32, v->pk);
        crypto_ed25519_key_pair(sk, pk2, seed);       /* (wipes seed) */
        snprintf(msg, sizeof msg, "Ed25519 RFC 8032 TEST %s: public key", v->name);
        check(memcmp(pk, pk2, 32) == 0, msg);
        crypto_ed25519_sign(sig, sk, m, (size_t)mn);
        snprintf(msg, sizeof msg, "Ed25519 RFC 8032 TEST %s: signature", v->name);
        check(eqhex(sig, 64, v->sig), msg);
        snprintf(msg, sizeof msg, "Ed25519 RFC 8032 TEST %s: verifies", v->name);
        check(crypto_ed25519_check(sig, pk, m, (size_t)mn) == 0, msg);
        sig[i % 64] ^= 1;
        snprintf(msg, sizeof msg, "Ed25519 RFC 8032 TEST %s: a changed signature fails", v->name);
        check(crypto_ed25519_check(sig, pk, m, (size_t)mn) != 0, msg);
        if (mn > 0) {
            sig[i % 64] ^= 1;
            m[0] ^= 0x80;
            snprintf(msg, sizeof msg, "Ed25519 RFC 8032 TEST %s: a changed message fails", v->name);
            check(crypto_ed25519_check(sig, pk, m, (size_t)mn) != 0, msg);
        }
    }
}

static void test_chacha_poly(void)
{
    unsigned char o[512], t[16], buf[512];
    char msg[128];
    int i, impl;

    for (impl = 0; impl < 2; impl++) {
        const char *nm = impl ? "BearSSL" : "Monocypher";

        for (i = 0; i < (int)(sizeof chacha_vec / sizeof chacha_vec[0]); i++) {
            const struct chachav *v = &chacha_vec[i];

            (impl ? chacha_ietf_bear : chacha_ietf_mono)(o, v->in, v->len, v->key, v->nonce, v->counter);
            snprintf(msg, sizeof msg, "ChaCha20 %s: RFC 8439 A.%d vector %d", nm, i < 5 ? 1 : 2, i < 5 ? i + 1 : i - 4);
            check(memcmp(o, v->out, v->len) == 0, msg);
        }
        for (i = 0; i < (int)(sizeof polykey_vec / sizeof polykey_vec[0]); i++) {
            const struct chachav *v = &polykey_vec[i];

            (impl ? chacha_ietf_bear : chacha_ietf_mono)(o, 0, 32, v->key, v->nonce, 0);
            snprintf(msg, sizeof msg, "Poly1305 key generation %s: RFC 8439 A.4 vector %d", nm, i + 1);
            check(memcmp(o, v->out, 32) == 0, msg);
        }
        for (i = 0; i < (int)(sizeof aead_vec / sizeof aead_vec[0]); i++) {
            const struct aeadv *v = &aead_vec[i];

            snprintf(msg, sizeof msg, "AEAD %s: RFC 8439 %s", nm, i ? "A.5 (decryption)" : "2.8.2");
            if (v->pt) {
                memcpy(buf, v->pt, v->len);
                aead_seal(impl, v->key, v->nonce, v->aad, v->aadlen, buf, v->len, t);
                check(memcmp(buf, v->ct, v->len) == 0 && memcmp(t, v->tag, 16) == 0, msg);
            }
            memcpy(buf, v->ct, v->len);
            check(aead_open(impl, v->key, v->nonce, v->aad, v->aadlen, buf, v->len, v->tag) == 0 &&
                  (!v->pt || memcmp(buf, v->pt, v->len) == 0) &&
                  (v->pt || memcmp(buf, "Internet-Drafts are draft documents", 35) == 0), msg);
            memcpy(buf, v->ct, v->len);
            buf[v->len / 2] ^= 4;
            snprintf(msg, sizeof msg, "AEAD %s: a changed byte fails", nm);
            check(aead_open(impl, v->key, v->nonce, v->aad, v->aadlen, buf, v->len, v->tag) != 0, msg);
        }
    }
    for (i = 0; i < (int)(sizeof poly_vec / sizeof poly_vec[0]); i++) {
        const struct polyv *v = &poly_vec[i];

        crypto_poly1305(t, v->msg, v->len, v->key);
        snprintf(msg, sizeof msg, "Poly1305 Monocypher: RFC 8439 A.3 vector %d", i + 1);
        check(memcmp(t, v->tag, 16) == 0, msg);
    }
}

static void test_sha_hmac_aes(void)
{
    unsigned char out[64], k[200], d[200], *mil;
    char msg[128];
    int i, n;
    static const char *const algs512[] = { "sha512", "sha512m" };

    mil = malloc(1000000);
    memset(mil, 'a', 1000000);
    for (i = 0; i < 4; i++) {
        const struct shav *v = &sha_vec[i];
        const char *mid = (i < 2) ? sha_m448 : sha_m896;
        int j, reps = !strcmp(v->alg, "sha512") ? 2 : 1;

        for (j = 0; j < reps; j++) {
            const char *a = j ? algs512[1] : v->alg;

            n = hashrun(a, (const unsigned char *)"abc", 3, out);
            snprintf(msg, sizeof msg, "%s: FIPS 180 \"abc\"", a);
            check(eqhex(out, (size_t)n, v->abc), msg);
            n = hashrun(a, (const unsigned char *)mid, strlen(mid), out);
            snprintf(msg, sizeof msg, "%s: FIPS 180 two-block message", a);
            check(eqhex(out, (size_t)n, v->mid), msg);
            n = hashrun(a, mil, 1000000, out);
            snprintf(msg, sizeof msg, "%s: FIPS 180 a million a", a);
            check(eqhex(out, (size_t)n, v->million), msg);
        }
    }
    free(mil);
    for (i = 0; i < (int)(sizeof hmac_vec / sizeof hmac_vec[0]); i++) {
        const struct hmacv *v = &hmac_vec[i];
        br_hmac_key_context hk;
        br_hmac_context hc;
        int kn = unhex(k, sizeof k, v->key), dn = unhex(d, sizeof d, v->data);

        br_hmac_key_init(&hk, &br_sha256_vtable, k, (size_t)kn);
        br_hmac_init(&hc, &hk, 0);
        br_hmac_update(&hc, d, (size_t)dn);
        br_hmac_out(&hc, out);
        snprintf(msg, sizeof msg, "HMAC-SHA-256 BearSSL: RFC 4231 test case %d", v->tc);
        check(eqhex(out, 32, v->s256), msg);
        br_hmac_key_init(&hk, &br_sha512_vtable, k, (size_t)kn);
        br_hmac_init(&hc, &hk, 0);
        br_hmac_update(&hc, d, (size_t)dn);
        br_hmac_out(&hc, out);
        snprintf(msg, sizeof msg, "HMAC-SHA-512 BearSSL: RFC 4231 test case %d", v->tc);
        check(eqhex(out, 64, v->s512), msg);
        crypto_sha512_hmac(out, k, (size_t)kn, d, (size_t)dn);
        snprintf(msg, sizeof msg, "HMAC-SHA-512 Monocypher: RFC 4231 test case %d", v->tc);
        check(eqhex(out, 64, v->s512), msg);
    }
    for (i = 0; i < 3; i++) {
        unsigned char key[32], blk[16], iv[16];
        int kn = unhex(key, 32, aes_vec[i].key);

        br_aes_ct_ctrcbc_keys ck;

        /* a CBC-MAC of one block from a zero IV is the block cipher itself */
        unhex(blk, 16, aes_pt);
        memset(iv, 0, 16);
        br_aes_ct_ctrcbc_init(&ck, key, (size_t)kn);
        br_aes_ct_ctrcbc_mac(&ck, iv, blk, 16);
        snprintf(msg, sizeof msg, "AES-%d ct: FIPS 197 appendix C", kn * 8);
        check(eqhex(iv, 16, aes_vec[i].out), msg);
    }
}

/* the generator: seeded through the callback, erases its key, refuses to run unseeded */
static int seed_fixed(void *u, unsigned char *b, size_t n)
{
    memset(b, *(int *)u, n);
    return 0;
}

static int seed_fail(void *u, unsigned char *b, size_t n)
{
    (void)u;
    (void)b;
    (void)n;
    return -11;
}

static void test_rng(void)
{
    struct zc_rng r, r2;
    unsigned char a[100], b[100], k0[32], z[100];
    int s = 7, i;

    memset(&r, 0, sizeof r);
    memset(z, 0, sizeof z);
    check(zc_rng_bytes(&r, a, sizeof a) == -1 && memcmp(a, z, sizeof a) == 0, "rng: unseeded gives -1 and zeros");
    check(zc_rng_seed(&r, seed_fail, 0) == -11 && !r.ready, "rng: the seed callback's error comes back");
    check(zc_rng_seed(&r, seed_fixed, &s) == 0, "rng: seeded");
    memcpy(k0, r.key, 32);
    zc_rng_bytes(&r, a, sizeof a);
    check(memcmp(r.key, k0, 32) != 0, "rng: the key changed after output (fast key erasure)");
    zc_rng_seed(&r2, seed_fixed, &s);
    for (i = 0; i < 100; i++)
        zc_rng_bytes(&r2, b + i, 1);
    check(memcmp(a, b, sizeof a) == 0, "rng: byte by byte gives the same stream");
    {
        unsigned char blk[64], key[32], n8[8] = { 0 };   /* the first 32 output bytes: block 0's second half */
        memset(key, 7, 32);
        crypto_chacha20_djb(blk, 0, 64, key, n8, 0);
        check(memcmp(a, blk + 32, 32) == 0, "rng: the output is ChaCha20's (second half of each block)");
    }
    zc_rng_wipe(&r);
    check(!r.ready && r.left == 0, "rng: wiped");
}

/* ssh: K as an mpint (forced cases: top bit set, leading zeros, zero), the key derivation against a straight
   computation, and the packet framing round trip with a wrong tag, a wrong sequence number and a changed length */
static void test_ssh(void)
{
    unsigned char k[32], mp[37], h[32], sid[32], out[100], ref[96], key[64], pkt[200], save[200], tag[16];
    size_t n;
    struct zc_chapol c;
    br_sha256_context s;
    char letter = 'C';

    memset(k, 0, 32);
    k[0] = 0x80;
    n = zc_ssh_mpint(mp, k);
    check(n == 37 && mp[3] == 33 && mp[4] == 0 && mp[5] == 0x80, "mpint: top bit set gets a zero byte");
    memset(k, 0, 32);
    k[2] = 0x7f;
    k[31] = 1;
    n = zc_ssh_mpint(mp, k);
    check(n == 34 && mp[3] == 30 && mp[4] == 0x7f, "mpint: leading zeros stripped");
    memset(k, 0, 32);
    k[1] = 0xff;
    n = zc_ssh_mpint(mp, k);
    check(n == 36 && mp[3] == 32 && mp[4] == 0 && mp[5] == 0xff, "mpint: a zero first byte, then the top bit");
    memset(k, 0, 32);
    n = zc_ssh_mpint(mp, k);
    check(n == 4 && mp[3] == 0, "mpint: zero is empty");

    memset(k, 0x91, 32);
    n = zc_ssh_mpint(mp, k);
    memset(h, 0x22, 32);
    memset(sid, 0x33, 32);
    zc_ssh_kdf(out, 100, mp, n, h, letter, sid);
    br_sha256_init(&s);
    br_sha256_update(&s, mp, n);
    br_sha256_update(&s, h, 32);
    br_sha256_update(&s, &letter, 1);
    br_sha256_update(&s, sid, 32);
    br_sha256_out(&s, ref);
    br_sha256_init(&s);
    br_sha256_update(&s, mp, n);
    br_sha256_update(&s, h, 32);
    br_sha256_update(&s, ref, 32);
    br_sha256_out(&s, ref + 32);
    br_sha256_init(&s);
    br_sha256_update(&s, mp, n);
    br_sha256_update(&s, h, 32);
    br_sha256_update(&s, ref, 64);
    br_sha256_out(&s, ref + 64);
    check(memcmp(out, ref, 96) == 0, "kdf: RFC 4253 7.2 with the key extension (K1, K2, K3)");
    zc_ssh_kdf(out, 64, mp, n, h, letter, sid);
    check(memcmp(out, ref, 64) == 0, "kdf: 64 bytes are the first two blocks");

    for (n = 0; n < 64; n++)
        key[n] = (unsigned char)n;
    zc_chapol_init(&c, key);
    memset(pkt, 0, sizeof pkt);
    pkt[3] = 28;                               /* length 28: padding length, payload, padding */
    pkt[4] = 8;
    memcpy(pkt + 5, "hello, ssh world!!!!", 20);
    memcpy(save, pkt, 32);
    zc_chapol_seal(&c, 7, pkt, 28, tag);
    check(zc_chapol_length(&c, 7, pkt) == 28, "chapol: the length decrypts");
    check(zc_chapol_length(&c, 8, pkt) != 28, "chapol: another sequence number gives another length");
    memcpy(save + 100, pkt, 32);
    check(zc_chapol_open(&c, 8, pkt, 28, tag) == -1 && memcmp(pkt, save + 100, 32) == 0,
          "chapol: a wrong sequence number fails the tag, nothing decrypted");
    tag[0] ^= 1;
    check(zc_chapol_open(&c, 7, pkt, 28, tag) == -1, "chapol: a wrong tag fails");
    tag[0] ^= 1;
    pkt[1] ^= 1;
    check(zc_chapol_open(&c, 7, pkt, 28, tag) == -1, "chapol: a changed length byte fails the tag");
    pkt[1] ^= 1;
    check(zc_chapol_open(&c, 7, pkt, 28, tag) == 0 && memcmp(pkt + 4, save + 4, 28) == 0,
          "chapol: the payload decrypts after the tag");
    {
        /* the parts by hand: the length with the second key at counter 0, the payload with the first at 1 */
        unsigned char n8[8] = { 0, 0, 0, 0, 0, 0, 0, 7 }, ks[64], l4[4];

        memcpy(pkt, save, 32);
        zc_chapol_seal(&c, 7, pkt, 28, tag);
        crypto_chacha20_djb(l4, save, 4, key + 32, n8, 0);
        crypto_chacha20_djb(ks, 0, 64, key, n8, 1);
        check(memcmp(l4, pkt, 4) == 0, "chapol: the length is ChaCha20 under the second key, counter 0");
        for (n = 0; n < 28; n++)
            ks[n] ^= save[4 + n];
        check(memcmp(ks, pkt + 4, 28) == 0, "chapol: the payload is ChaCha20 under the first key, counter 1");
        crypto_chacha20_djb(ks, 0, 32, key, n8, 0);
        crypto_poly1305(ref, pkt, 32, ks);
        check(memcmp(ref, tag, 16) == 0, "chapol: the tag is Poly1305 over length and payload, key from block 0");
    }
    zc_chapol_wipe(&c);
}

static void test_sig(void)
{
    unsigned char seed[32], sk[64], pk[2][32], d[32], msg[100], sig[80], id[8];
    br_sha256_context s;
    int n, which;

    memset(seed, 0x42, 32);
    crypto_ed25519_key_pair(sk, pk[1], seed);
    memset(pk[0], 0x11, 32);
    memset(d, 0x5a, 32);
    n = zc_sig_message(msg, ZC_SIG_LABEL_BUNDLE, d);
    check(n == 58 && msg[25] == 0 && memcmp(msg, "Zenix bundle signature v1", 25) == 0,
          "sig: the bundle message is 58 bytes (label, NUL, digest)");
    memcpy(sig, "ZSG1\1\1\0\0", 8);
    zc_sig_keyid(id, pk[1]);
    br_sha256_init(&s);
    br_sha256_update(&s, pk[1], 32);
    br_sha256_out(&s, msg + 60);
    check(memcmp(id, msg + 60, 8) == 0, "sig: the key id is SHA-256 of the public key, 8 bytes");
    n = zc_sig_message(msg, ZC_SIG_LABEL_BUNDLE, d);
    memcpy(sig + 8, id, 8);
    crypto_ed25519_sign(sig + 16, sk, msg, (size_t)n);
    check(zc_sig_verify(sig, 80, ZC_SIG_LABEL_BUNDLE, d, (const unsigned char (*)[32])pk, 2, &which) == ZC_SIG_OK &&
          which == 1, "sig: verifies with the second key");
    check(zc_sig_verify(sig, 80, ZC_SIG_LABEL_INDEX, d, (const unsigned char (*)[32])pk, 2, 0) == ZC_SIG_EBAD,
          "sig: another label fails");
    check(zc_sig_verify(sig, 79, ZC_SIG_LABEL_BUNDLE, d, (const unsigned char (*)[32])pk, 2, 0) == ZC_SIG_EFORMAT,
          "sig: 79 bytes is not a signature file");
    check(zc_sig_verify(sig, 80, ZC_SIG_LABEL_BUNDLE, d, (const unsigned char (*)[32])pk, 1, &which) == ZC_SIG_EKEY &&
          which == -1, "sig: an unknown key id");
    sig[4] = 2;
    check(zc_sig_verify(sig, 80, ZC_SIG_LABEL_BUNDLE, d, (const unsigned char (*)[32])pk, 2, 0) == ZC_SIG_EFORMAT,
          "sig: format 2 refused");
    sig[4] = 1;
    d[31] ^= 1;
    check(zc_sig_verify(sig, 80, ZC_SIG_LABEL_BUNDLE, d, (const unsigned char (*)[32])pk, 2, 0) == ZC_SIG_EBAD,
          "sig: another digest fails");
    check(zc_sig_message(msg, "", d) == -1, "sig: an empty label refused");
}

/* ---- the oracle (tools/host/crypto-test.py) ---- */
#define BIG 70000
static unsigned char A[BIG], B[BIG], C[BIG], D[BIG], E[BIG];

static int oracle(void)
{
    static char line[5 * BIG];
    char *w[10];

    setvbuf(stdout, 0, _IOLBF, 0);
    while (fgets(line, sizeof line, stdin)) {
        int nw = 0, an, bn, cn, dn, en, r;
        char *p = strtok(line, " \n");

        while (p && nw < 10) {
            w[nw++] = p;
            p = strtok(0, " \n");
        }
        if (!nw)
            continue;
#define H(i, X) ((i) < nw ? unhex(X, BIG, w[i]) : -1)
        if (!strcmp(w[0], "x25519") && nw == 4) {          /* x25519 bear|mono SCALAR U */
            an = H(2, A); bn = H(3, B);
            if (an != 32 || bn != 32) { puts("ERR"); continue; }
            r = !strcmp(w[1], "bear") ? zc_x25519_bearssl(C, A, B) : zc_x25519_monocypher(C, A, B);
            if (r) puts("ZERO"); else { hexout(C, 32); putchar('\n'); }
        } else if (!strcmp(w[0], "ed25519_pub") && nw == 2) {   /* SEED */
            if (H(1, A) != 32) { puts("ERR"); continue; }
            crypto_ed25519_key_pair(B, C, A);
            hexout(C, 32); putchar('\n');
        } else if (!strcmp(w[0], "ed25519_sign") && nw == 3) {  /* SEED MSG */
            unsigned char sk[64], pk[32], sig[64];
            an = H(1, A); bn = H(2, B);
            if (an != 32 || bn < 0) { puts("ERR"); continue; }
            crypto_ed25519_key_pair(sk, pk, A);
            crypto_ed25519_sign(sig, sk, B, (size_t)bn);
            hexout(sig, 64); putchar('\n');
        } else if (!strcmp(w[0], "ed25519_verify") && nw == 4) { /* PK MSG SIG (zc_ed25519_check) */
            an = H(1, A); bn = H(2, B); cn = H(3, C);
            if (an != 32 || bn < 0 || cn < 0) { puts("ERR"); continue; }
            puts(cn == 64 && zc_ed25519_check(C, A, B, (size_t)bn) == 0 ? "1" : "0");
        } else if (!strcmp(w[0], "aead_seal") && nw == 6) {     /* bear|mono KEY NONCE AAD PT -> CT TAG */
            an = H(2, A); bn = H(3, B); cn = H(4, C); dn = H(5, D);
            if (an != 32 || bn != 12 || cn < 0 || dn < 0) { puts("ERR"); continue; }
            aead_seal(!strcmp(w[1], "bear"), A, B, C, (size_t)cn, D, (size_t)dn, E);
            hexout(D, (size_t)dn); putchar(' '); hexout(E, 16); putchar('\n');
        } else if (!strcmp(w[0], "aead_open") && nw == 7) {     /* bear|mono KEY NONCE AAD CT TAG -> PT|FAIL */
            an = H(2, A); bn = H(3, B); cn = H(4, C); dn = H(5, D); en = H(6, E);
            if (an != 32 || bn != 12 || cn < 0 || dn < 0 || en < 0) { puts("ERR"); continue; }
            if (en != 16 || aead_open(!strcmp(w[1], "bear"), A, B, C, (size_t)cn, D, (size_t)dn, E)) puts("FAIL");
            else { hexout(D, (size_t)dn); putchar('\n'); }
        } else if (!strcmp(w[0], "gcm_seal") && nw == 6) {      /* KEY IV AAD PT TAGLEN */
            an = H(1, A); bn = H(2, B); cn = H(3, C); dn = H(4, D);
            if ((an != 16 && an != 24 && an != 32) || bn < 1 || cn < 0 || dn < 0) { puts("ERR"); continue; }
            gcm(1, A, (size_t)an, B, (size_t)bn, C, (size_t)cn, D, (size_t)dn, E, (size_t)atoi(w[5]));
            hexout(D, (size_t)dn); putchar(' '); hexout(E, (size_t)atoi(w[5])); putchar('\n');
        } else if (!strcmp(w[0], "gcm_open") && nw == 6) {      /* KEY IV AAD CT TAG */
            an = H(1, A); bn = H(2, B); cn = H(3, C); dn = H(4, D); en = H(5, E);
            if ((an != 16 && an != 24 && an != 32) || bn < 1 || cn < 0 || dn < 0 || en < 1 || en > 16) { puts("ERR"); continue; }
            if (!gcm(0, A, (size_t)an, B, (size_t)bn, C, (size_t)cn, D, (size_t)dn, E, (size_t)en)) puts("FAIL");
            else { hexout(D, (size_t)dn); putchar('\n'); }
        } else if (!strcmp(w[0], "hash") && nw == 3) {          /* ALG MSG */
            an = H(2, A);
            r = an < 0 ? -1 : hashrun(w[1], A, (size_t)an, B);
            if (r < 0) puts("ERR"); else { hexout(B, (size_t)r); putchar('\n'); }
        } else if (!strcmp(w[0], "hmac") && nw == 4) {          /* ALG KEY MSG */
            br_hmac_key_context hk;
            br_hmac_context hc;
            const br_hash_class *hc_ = hashclass(w[1]);
            an = H(2, A); bn = H(3, B);
            if (!hc_ || an < 0 || bn < 0) { puts("ERR"); continue; }
            br_hmac_key_init(&hk, hc_, A, (size_t)an);
            br_hmac_init(&hc, &hk, 0);
            br_hmac_update(&hc, B, (size_t)bn);
            r = (int)br_hmac_out(&hc, C);
            hexout(C, (size_t)r); putchar('\n');
        } else if (!strcmp(w[0], "rsa_vrfy") && nw == 6) {      /* ALG N E MSG SIG (PKCS#1 v1.5, i15) */
            br_rsa_public_key pk;
            unsigned char hv[64], got[64];
            const unsigned char *oid = hashoid(w[1]);
            int hn;
            an = H(2, A); bn = H(3, B); cn = H(4, C); dn = H(5, D);
            if (!oid || an < 1 || bn < 1 || cn < 0 || dn < 0) { puts("ERR"); continue; }
            hn = hashrun(w[1], C, (size_t)cn, hv);
            pk.n = A; pk.nlen = (size_t)an; pk.e = B; pk.elen = (size_t)bn;
            r = (int)br_rsa_i15_pkcs1_vrfy(D, (size_t)dn, oid, (size_t)hn, &pk, got);
            puts(r && memcmp(got, hv, (size_t)hn) == 0 ? "1" : "0");
        } else if ((!strcmp(w[0], "ecdsa_vrfy") || !strcmp(w[0], "ecdsa_vrfy_raw")) && nw == 6) {
            /* CURVE ALG POINT MSG SIG: asn1 (DER) or raw (r || s) */
            br_ec_public_key pk;
            unsigned char hv[64];
            int hn, curve = !strcmp(w[1], "p256") ? BR_EC_secp256r1 : !strcmp(w[1], "p384") ? BR_EC_secp384r1 :
                            !strcmp(w[1], "p521") ? BR_EC_secp521r1 : 0;
            an = H(3, A); bn = H(4, B); cn = H(5, C);
            if (!curve || an < 1 || bn < 0 || cn < 0) { puts("ERR"); continue; }
            hn = hashrun(w[2], B, (size_t)bn, hv);
            if (hn < 0) { puts("ERR"); continue; }
            pk.curve = curve; pk.q = A; pk.qlen = (size_t)an;
            r = !strcmp(w[0], "ecdsa_vrfy") ? (int)zc_ecdsa_vrfy_asn1(&br_ec_all_m15, hv, (size_t)hn, &pk, C, (size_t)cn)
                                             : (int)zc_ecdsa_vrfy_raw(&br_ec_all_m15, hv, (size_t)hn, &pk, C, (size_t)cn);
            puts(r ? "1" : "0");
        } else if (!strcmp(w[0], "ssh_seal") && nw == 4) {      /* KEY64 SEQ PKT(4+len) -> PKT TAG */
            struct zc_chapol c;
            an = H(1, A); bn = H(3, B);
            if (an != 64 || bn < 4) { puts("ERR"); continue; }
            zc_chapol_init(&c, A);
            zc_chapol_seal(&c, (uint32_t)strtoul(w[2], 0, 10), B, (size_t)bn - 4, C);
            hexout(B, (size_t)bn); putchar(' '); hexout(C, 16); putchar('\n');
        } else if (!strcmp(w[0], "ssh_open") && nw == 5) {      /* KEY64 SEQ ENC TAG -> LEN PLAIN | FAIL */
            struct zc_chapol c;
            uint32_t len;
            an = H(1, A); bn = H(3, B); cn = H(4, C);
            if (an != 64 || bn < 4 || cn != 16) { puts("ERR"); continue; }
            zc_chapol_init(&c, A);
            len = zc_chapol_length(&c, (uint32_t)strtoul(w[2], 0, 10), B);
            if (len != (uint32_t)bn - 4 || zc_chapol_open(&c, (uint32_t)strtoul(w[2], 0, 10), B, len, C)) puts("FAIL");
            else { hexout(B + 4, len); putchar('\n'); }
        } else if (!strcmp(w[0], "ssh_kdf") && nw == 6) {       /* K32 H SID LETTER N */
            unsigned char mp[37];
            size_t mn;
            an = H(1, A); bn = H(2, B); cn = H(3, C);
            if (an != 32 || bn != 32 || cn != 32 || atoi(w[5]) > 1000) { puts("ERR"); continue; }
            mn = zc_ssh_mpint(mp, A);
            zc_ssh_kdf(D, (size_t)atoi(w[5]), mp, mn, B, w[4][0], C);
            hexout(D, (size_t)atoi(w[5])); putchar('\n');
        } else if (!strcmp(w[0], "aes_ctr") && nw == 4) {       /* KEY IV16 DATA: 128-bit big-endian counter */
            br_aes_ct_ctrcbc_keys ck;
            an = H(1, A); bn = H(2, B); cn = H(3, C);
            if ((an != 16 && an != 24 && an != 32) || bn != 16 || cn < 0) { puts("ERR"); continue; }
            br_aes_ct_ctrcbc_init(&ck, A, (size_t)an);
            br_aes_ct_ctrcbc_ctr(&ck, B, C, (size_t)cn);
            hexout(C, (size_t)cn); putchar('\n');
        } else {
            puts("ERR");
        }
        fflush(stdout);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "oracle"))
        return oracle();
    if (argc < 2 || (strcmp(argv[1], "vectors") && strcmp(argv[1], "long"))) {
        fprintf(stderr, "usage: crypto-test vectors|long|oracle\n");
        return 2;
    }
    test_x25519(!strcmp(argv[1], "long"));
    test_ed25519();
    test_chacha_poly();
    test_sha_hmac_aes();
    test_rng();
    test_ssh();
    test_sig();
    printf("crypto-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
