/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * libFuzzer harnesses for every parser of D21 steps 0-1 (DESIGN D21 11.1; tools/host/crypto-test.sh fuzz runs them
 * one at a time with a memory cap). One file, one target per build (-DFUZZ_<NAME>):
 *   SSHKEX   net/ssh/ from its first byte: the server's version line, preamble, plain packets, KEXINIT, the key
 *            exchange reply, the strict key exchange rules
 *   SSHPKT   net/ssh/ after the key exchange: each input is a list of payloads (u16 length, bytes) the harness seals
 *            with the session's chacha20-poly1305 keys, so they reach the authentication and channel layers; the first
 *            byte picks where the session stands (authentication steps, channel opening, the running session)
 *   SSHOPEN  the packet layer with damaged ciphertext: bytes straight after the keys are on (tags, lengths)
 *   KEYS     openssh-key-v1 files and public key lines
 *   KH       known_hosts text
 *   CONFIG   the ~/.ssh/config subset
 *   CLEAN    the text cleaner (and checks its output)
 *   SIG      the .sig file
 *   ANCHORS  the anchors file (zc_anchors_find over it, every hash in the input looked up)
 *   X509     certificate chains through the X.509 wrapper (the anchors from CRYPTO_FUZZ_ANCHORS), the EKU and issuer
 *            walks
 *   TLS      BearSSL's TLS 1.2 client engine fed server records, with D21 3.3's set-up and the wrapper
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../net/ssh/ssh.h"
#include "../../crypto/zc/zc_x509.h"
#include "../../crypto/zc/zc_verify.h"
#include "../../crypto/monocypher/monocypher-ed25519.h"

static int seed_fixed(void *u, unsigned char *b, size_t n)
{
    (void)u;
    memset(b, 0x42, n);
    return 0;
}

#if defined(FUZZ_SSHKEX) || defined(FUZZ_SSHPKT) || defined(FUZZ_SSHOPEN)
static unsigned char rbuf[SSH_RBUF_FULL], sbuf[SSH_SBUF_MIN];
static struct ssh s;
static struct zc_rng rng;
static unsigned char idsk[64];

static void nsend(void *u, const unsigned char *b, long n) { (void)u; (void)b; (void)n; }
static void ndata(void *u, int e, const unsigned char *b, long n)
{
    (void)u; (void)e;
    if (n > 0) { volatile unsigned char x = b[0]; x ^= b[n - 1]; (void)x; }    /* (touch the bytes for ASan) */
}
static void ntext(void *u, int k, const char *t, long n)
{
    long i;

    (void)u; (void)k;
    for (i = 0; i < n; i++)             /* the cleaner's promise: no control character but tab and newline */
        if (((unsigned char)t[i] < 0x20 && t[i] != '\t' && t[i] != '\n') || t[i] == 0x7f)
            abort();
}
static unsigned char *ngrow(void *u, unsigned char *old, long need) { (void)u; (void)need; return old == rbuf ? rbuf : 0; }

static void setup(void)
{
    unsigned char seed[32], pk[32];

    memset(&s, 0, sizeof s);
    zc_rng_seed(&rng, seed_fixed, 0);
    memset(seed, 3, 32);
    crypto_ed25519_key_pair(idsk, pk, seed);
    s.send = nsend;
    s.data = ndata;
    s.text = ntext;
    s.grow = ngrow;
    s.rng = &rng;
    s.version = "SSH-2.0-Fuzz";
    s.user = "fuzz";
    s.rbuf = rbuf;
    s.rcap = SSH_RBUF_MIN;
    s.sbuf = sbuf;
    s.scap = sizeof sbuf;
    s.id_sk = idsk;
    s.methods = SSH_AUTH_PUBKEY | SSH_AUTH_KBDINT | SSH_AUTH_PASSWORD;
    s.pty = 1;
    s.term = "xterm-16color";
    s.cols = 80;
    s.rows = 32;
    ssh_start(&s);
}

/* answer whatever the core asks, as a caller would */
static void answer(void)
{
    static const char *const a[SSH_KBD_PROMPTS] = { "a", "b", "c", "d", "e", "f", "g", "h" };

    if (s.need == SSH_NEED_HOSTKEY) ssh_hostkey(&s, 1);
    else if (s.need == SSH_NEED_PASSWORD) ssh_password(&s, "pw");
    else if (s.need == SSH_NEED_KBDINT) ssh_kbdint(&s, a);
}

static void feed(const unsigned char *b, long n)
{
    int guard = 0;

    while (n > 0 && s.state != SSH_ST_FAILED && guard++ < 100000) {
        long k;

        answer();
        k = ssh_input(&s, b, n);
        if (k < 0)
            break;
        b += k;
        n -= k;
        if (s.state == SSH_ST_SESSION) {
            ssh_write(&s, (const unsigned char *)"keys", 4);
            ssh_winsize(&s, 81, 25);
        }
    }
    answer();
}

/* the session as if the first key exchange were done: both keys known to the harness */
static const unsigned char rxkey[64] = { 1, 2, 3, 4, 5 }, txkey[64] = { 6, 7, 8 };
static void keyed(int where)
{
    setup();
    s.state = SSH_ST_AUTH;
    s.kex_count = 1;
    s.strict = 1;
    memset(s.server_version, 0, sizeof s.server_version);
    strcpy(s.server_version, "SSH-2.0-Fuzz");
    memset(s.sid, 9, 32);
    memset(s.hk_pk, 1, 32);
    zc_chapol_init(&s.rx, rxkey);
    zc_chapol_init(&s.tx, txkey);
    s.rx_on = s.tx_on = 1;
    s.ext_info_ok = 1;
    switch (where % 9) {
    case 0: s.auth_step = 1; break;                     /* SERVICE_REQUEST sent */
    case 1: s.auth_step = 2; strcpy(s.auth_methods, "publickey,keyboard-interactive,password"); break;
    case 2: s.auth_step = 3; break;                     /* the publickey query */
    case 3: s.auth_step = 5; break;                     /* keyboard-interactive */
    case 4: s.auth_step = 6; break;                     /* password */
    case 5: s.state = SSH_ST_OPEN; s.ch_step = 1; s.ch_lwin = SSH_WINDOW; break;
    case 6: s.state = SSH_ST_OPEN; s.ch_step = 2; s.ch_lwin = SSH_WINDOW; s.ch_rwin = 100000; s.ch_rmax = 32768; break;
    case 7: s.state = SSH_ST_OPEN; s.ch_step = 3; s.ch_lwin = SSH_WINDOW; s.ch_rwin = 100000; s.ch_rmax = 32768; break;
    default: s.state = SSH_ST_SESSION; s.ch_step = 4; s.ch_lwin = SSH_WINDOW; s.ch_rwin = 100000; s.ch_rmax = 32768;
    }
}
#endif

#ifdef FUZZ_SSHKEX
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    setup();
    feed(d, (long)n);
    ssh_wipe(&s);
    return 0;
}
#endif

#ifdef FUZZ_SSHPKT
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static unsigned char pkt[SSH_PKTMAX + 64];
    size_t i = 1;
    uint32_t seq = 0;
    struct zc_chapol c;

    if (n < 1)
        return 0;
    keyed(d[0]);
    zc_chapol_init(&c, rxkey);
    while (i + 2 <= n && s.state != SSH_ST_FAILED) {
        size_t pl = (size_t)d[i] << 8 | d[i + 1], len, pad;

        i += 2;
        if (pl > n - i)
            pl = n - i;
        if (pl == 0 || pl > 32000)
            break;
        pad = 8 - (1 + pl) % 8;
        if (pad < 4)
            pad += 8;
        len = 1 + pl + pad;
        pkt[0] = (unsigned char)(len >> 24);
        pkt[1] = (unsigned char)(len >> 16);
        pkt[2] = (unsigned char)(len >> 8);
        pkt[3] = (unsigned char)len;
        pkt[4] = (unsigned char)pad;
        memcpy(pkt + 5, d + i, pl);
        memset(pkt + 5 + pl, 0, pad);
        zc_chapol_seal(&c, seq++, pkt, len, pkt + 4 + len);
        feed(pkt, (long)(4 + len + 16));
        i += pl;
        if (s.rx_seq != seq)                            /* the core rekeyed or failed: its keys moved on */
            break;
    }
    ssh_wipe(&s);
    return 0;
}
#endif

#ifdef FUZZ_SSHOPEN
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    keyed(8);
    feed(d, (long)n);
    ssh_wipe(&s);
    return 0;
}
#endif

#ifdef FUZZ_KEYS
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static unsigned char work[SSH_KEY_WORK], sk[64], blob[2048];
    char comment[64];

    if (n < 1)
        return 0;
    ssh_key_read((const char *)d + 1, (long)n - 1, (d[0] & 1) ? "pass phrase" : 0, sk, comment, sizeof comment, work,
                 sizeof work);
    ssh_pubkey_parse((const char *)d, (long)n, blob, sizeof blob);
    return 0;
}
#endif

#ifdef FUZZ_KH
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    unsigned char blob[SSH_ED25519_BLOB], pk[32];
    long line;

    memset(pk, 1, 32);
    ssh_key_blob(blob, pk);
    ssh_kh_check((const char *)d, (long)n, "pc.lan", 22, blob, sizeof blob, &line);
    ssh_kh_check((const char *)d, (long)n, "Pc.Lan", 2222, blob, sizeof blob, &line);
    return 0;
}
#endif

#ifdef FUZZ_CONFIG
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    struct ssh_cfg c;

    ssh_config((const char *)d, (long)n, "pc", &c, 0, 0);
    if (strlen(c.hostname) >= sizeof c.hostname || strlen(c.user) >= sizeof c.user || c.port < 0 || c.port > 65535)
        abort();
    return 0;
}
#endif

#ifdef FUZZ_CLEAN
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    char *o = malloc(n + 1);
    long k = ssh_clean(o, (const char *)d, (long)n), i;

    if (k < 0 || k > (long)n)
        abort();
    for (i = 0; i < k; i++)
        if (((unsigned char)o[i] < 0x20 && o[i] != '\t' && o[i] != '\n') || o[i] == 0x7f)
            abort();
    if (ssh_clean(o, o, k) != k)        /* clean text stays as it is */
        abort();
    free(o);
    return 0;
}
#endif

#ifdef FUZZ_SIG
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    unsigned char pub[2][32], seed[32], sk[64], dg[32];
    static int ready;
    int which;

    if (!ready) {
        memset(seed, 5, 32);
        crypto_ed25519_key_pair(sk, pub[0], seed);
        memset(pub[1], 0x58, 32);
        ready = 1;
    }
    memset(dg, 0x11, 32);
    zc_sig_verify(d, n, ZC_SIG_LABEL_BUNDLE, dg, (const unsigned char (*)[32])pub, 2, &which);
    if (n >= 96) {
        zc_ed25519_check(d, d + 64, d + 96, n - 96);
        zc_ecdsa_der_strict(d, n);
    }
    return 0;
}
#endif

#if defined(FUZZ_ANCHORS) || defined(FUZZ_X509) || defined(FUZZ_TLS)
static const uint8_t *mem;
static size_t memn;
static unsigned char *anchors_file;
static size_t anchors_n;

static int memrd(void *u, unsigned long off, void *buf, unsigned long n)
{
    const uint8_t *m = u ? anchors_file : mem;
    size_t mn = u ? anchors_n : memn;

    if (off > mn || n > mn - off)
        return -1;
    memcpy(buf, m + off, n);
    return 0;
}

static int find_file(void *u, struct zc_x509 *x, const unsigned char hash[8], const unsigned char *dn, size_t dnlen)
{
    (void)u; (void)dn; (void)dnlen;
    return anchors_file ? zc_anchors_find(x, memrd, (void *)1, hash) : 0;
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    const char *p = getenv("CRYPTO_FUZZ_ANCHORS");
    FILE *f = p ? fopen(p, "rb") : 0;

    (void)argc; (void)argv;
    if (f) {
        anchors_file = malloc(1 << 20);
        anchors_n = fread(anchors_file, 1, 1 << 20, f);
        fclose(f);
    }
    return 0;
}
#endif

#ifdef FUZZ_ANCHORS
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static unsigned char chain[ZC_X509_CHAINMAX], store[16384];
    static struct zc_x509 x;
    size_t i;

    mem = d;
    memn = n;
    zc_x509_init(&x, chain, store, sizeof store, 0, 0);
    for (i = 0; i + 8 <= n && i < 16 * 12 + 16; i += 12) {     /* the hashes at the index's places, and others */
        zc_anchors_find(&x, memrd, 0, d + i);
        x.nta = 0;
        x.storeused = 0;
    }
    zc_x509_add_entry(&x, d, n);
    return 0;
}
#endif

#ifdef FUZZ_X509
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static unsigned char chain[ZC_X509_CHAINMAX], store[16384];
    static struct zc_x509 x;
    const br_x509_class **v = &x.vtable;
    size_t i = 0, il;

    zc_x509_init(&x, chain, store, sizeof store, find_file, 0);
    zc_x509_set_time(&x, 739900, 0);
    (*v)->start_chain(v, "test.zenix");
    while (i + 2 <= n) {                /* certificates, each a u16 length and its bytes */
        size_t l = (size_t)d[i] << 8 | d[i + 1];

        i += 2;
        if (l > n - i)
            l = n - i;
        (*v)->start_cert(v, (uint32_t)l);
        (*v)->append(v, d + i, l);
        (*v)->end_cert(v);
        zc_x509_eku_ok(d + i, l);
        zc_x509_issuer(d + i, l, &il);
        zc_x509_add_cert(&x, d + i, l);
        i += l;
    }
    (*v)->end_chain(v);
    (*v)->get_pkey(v, 0);
    return 0;
}
#endif

#ifdef FUZZ_TLS
int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
    static br_ssl_client_context cc;
    static struct zc_x509 x;
    static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI], chain[ZC_X509_CHAINMAX], store[16384];
    static const uint16_t suites[] = {
        BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256, BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384 };
    br_ssl_engine_context *e = &cc.eng;
    unsigned char seed[32];
    size_t i = 0;
    int guard = 0;

    zc_x509_init(&x, chain, store, sizeof store, find_file, 0);
    zc_x509_set_time(&x, 739900, 0);
    br_ssl_client_zero(&cc);
    br_ssl_engine_set_versions(e, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(e, suites, sizeof suites / sizeof suites[0]);
    br_ssl_engine_set_x509(e, &x.vtable);
    br_ssl_client_set_rsapub(&cc, br_rsa_i15_public);
    br_ssl_engine_set_rsavrfy(e, br_rsa_i15_pkcs1_vrfy);
    br_ssl_engine_set_ec(e, &br_ec_all_m15);
    br_ssl_engine_set_ecdsa(e, zc_ecdsa_vrfy_asn1);
    br_ssl_engine_set_hash(e, br_sha256_ID, &br_sha256_vtable);
    br_ssl_engine_set_hash(e, br_sha384_ID, &br_sha384_vtable);
    br_ssl_engine_set_hash(e, br_sha512_ID, &br_sha512_vtable);
    br_ssl_engine_set_prf_sha256(e, br_tls12_sha256_prf);
    br_ssl_engine_set_prf_sha384(e, br_tls12_sha384_prf);
    br_ssl_engine_set_chapol(e, &br_sslrec_in_chapol_vtable, &br_sslrec_out_chapol_vtable);
    br_ssl_engine_set_chacha20(e, br_chacha20_ct_run);
    br_ssl_engine_set_poly1305(e, br_poly1305_ctmul32_run);
    br_ssl_engine_set_gcm(e, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
    br_ssl_engine_set_aes_ctr(e, &br_aes_ct_ctr_vtable);
    br_ssl_engine_set_ghash(e, br_ghash_ctmul32);
    br_ssl_engine_set_all_flags(e, BR_OPT_NO_RENEGOTIATION);
    br_ssl_engine_set_buffer(e, iobuf, sizeof iobuf, 1);
    memset(seed, 7, sizeof seed);
    br_ssl_engine_inject_entropy(e, seed, sizeof seed);
    if (!br_ssl_client_reset(&cc, "test.zenix", 0))
        abort();
    while (guard++ < 10000) {
        unsigned st = br_ssl_engine_current_state(e);
        size_t len;
        unsigned char *b;

        if (st & BR_SSL_CLOSED)
            break;
        if (st & BR_SSL_SENDREC) {
            b = br_ssl_engine_sendrec_buf(e, &len);
            br_ssl_engine_sendrec_ack(e, len);
            continue;
        }
        if (st & BR_SSL_RECVAPP) {
            b = br_ssl_engine_recvapp_buf(e, &len);
            br_ssl_engine_recvapp_ack(e, len);
            continue;
        }
        if (st & BR_SSL_RECVREC) {
            size_t k;

            if (i >= n)
                break;
            b = br_ssl_engine_recvrec_buf(e, &len);
            k = n - i < len ? n - i : len;
            memcpy(b, d + i, k);
            br_ssl_engine_recvrec_ack(e, k);
            i += k;
            continue;
        }
        break;
    }
    return 0;
}
#endif
