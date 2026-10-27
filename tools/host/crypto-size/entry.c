/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * For the linked m68k sizes of D21 7.1 (tools/host/crypto-test.sh size): one entry per row (-DROW_<NAME>) that
 * calls what that program calls, so --gc-sections keeps exactly that. The C library is a stand-in (stubs.c),
 * counted apart: on Zenix it is libz.
 */
#include "../../../crypto/zc/zc.h"
#include "../../../crypto/zc/zc_verify.h"
#include "../../../crypto/zc/zc_x509.h"
#include "../../../crypto/monocypher/monocypher.h"
#include "../../../crypto/monocypher/monocypher-ed25519.h"
#include "../../../net/ssh/ssh.h"

typedef void (*fn)(void);
void size_entry(void);
extern volatile unsigned char b[600];         /* (in stubs.c: counted with the stand-ins) */

#ifdef ROW_X25519_MONO          /* note 7.1 row 1: X25519 only (Monocypher), as zc uses it (public keys by the ladder) */
void size_entry(void) { zc_x25519_monocypher((void *)b, (void *)b, (void *)b); }
#endif
#ifdef ROW_X25519_BEAR          /* the other X25519: BearSSL's c25519_m15 through zc (3.2: step 2 picks one) */
void size_entry(void) { zc_x25519_bearssl((void *)b, (void *)b, (void *)b); }
#endif
#ifdef ROW_ED25519_VERIFY       /* note 7.1 row 2: Ed25519 verification only (zenixboot's, zpkg's share) */
void size_entry(void) { crypto_ed25519_check((void *)b, (void *)b, (void *)b, 32); }
#endif
#ifdef ROW_SIG                  /* zenixboot's real call: the .sig check (strict Ed25519 + SHA-256) */
void size_entry(void) { int w; zc_sig_verify((void *)b, 80, ZC_SIG_LABEL_BUNDLE, (void *)b, (void *)b, 2, &w); }
#endif
#ifdef ROW_SSH_PRIMS            /* note 7.1 row 3: ssh's primitives as the note measured them */
void size_entry(void)
{
    br_sha256_context s; br_rsa_public_key pk = { (void *)b, 256, (void *)b, 3 };
    br_ec_public_key ek = { BR_EC_secp256r1, (void *)b, 65 };
    crypto_x25519((void *)b, (void *)b, (void *)b);
    crypto_ed25519_sign((void *)b, (void *)b, (void *)b, 32);
    crypto_ed25519_check((void *)b, (void *)b, (void *)b, 32);
    crypto_chacha20_djb((void *)b, (void *)b, 64, (void *)b, (void *)b, 0);
    crypto_poly1305((void *)b, (void *)b, 64, (void *)b);
    br_sha256_init(&s); br_sha256_update(&s, (void *)b, 3); br_sha256_out(&s, (void *)b);
    br_rsa_i15_pkcs1_vrfy((void *)b, 256, BR_HASH_OID_SHA512, 64, &pk, (void *)b);
    br_ecdsa_i15_vrfy_raw(&br_ec_p256_m15, (void *)b, 32, &ek, (void *)b, 64);
}
#endif
#if defined(ROW_SSH) || defined(ROW_SSH_STEP3)   /* net/ssh/ with what ssh and ssh-keygen call (step 3; +3b's parsers) */
void size_entry(void)
{
    static struct ssh s;
    static const char *const a[1] = { "x" };
    static struct ssh_cfg c;
    long l;
    ssh_start(&s); ssh_input(&s, (void *)b, 10); ssh_hostkey(&s, 1); ssh_password(&s, "x"); ssh_kbdint(&s, a);
    ssh_write(&s, (void *)b, 10); ssh_eof(&s); ssh_close(&s); ssh_winsize(&s, 80, 32); ssh_tick(&s, 1);
    ssh_rekey(&s); ssh_keepalive(&s); ssh_wipe(&s); ssh_clean((void *)b, (void *)b, 10);
    ssh_key_read((void *)b, 10, 0, (void *)b, (void *)b, 10, (void *)b, 10);
    ssh_key_write((void *)b, 10, (void *)b, "c", 0, (void *)b, 16, (void *)b, (void *)b, 10);
    ssh_pubkey_line((void *)b, 10, (void *)b, 51, 0); ssh_pubkey_parse((void *)b, 10, (void *)b, 10);
    ssh_fingerprint((void *)b, (void *)b, 51); ssh_kh_check((void *)b, 10, "h", 22, (void *)b, 51, &l);
    ssh_kh_line((void *)b, 10, "h", 22, (void *)b, 51);
#ifdef ROW_SSH
    ssh_config((void *)b, 10, "h", &c, 0, 0);
#endif
    crypto_ed25519_key_pair((void *)b, (void *)b, (void *)b);
}
#endif
#if defined(ROW_TLS) || defined(ROW_TLS_X509)  /* note 7.1 row 4: a TLS 1.2 client (BearSSL), with D21 3.3's set-up */
static br_ssl_client_context cc;
static br_x509_minimal_context xc;
static struct zc_x509 zx;
static unsigned char iob[BR_SSL_BUFSIZE_BIDI];
static const uint16_t suites[] = {
    BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256, BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256, BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384, BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384 };
static int tcb(void *c, uint32_t a, uint32_t b2, uint32_t c2, uint32_t d) { (void)c; (void)a; (void)b2; (void)c2; (void)d; return 0; }
void size_entry(void)
{
    br_ssl_engine_context *e = &cc.eng;
    size_t n;
    br_ssl_client_zero(&cc);
    br_ssl_engine_set_versions(e, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(e, suites, 6);
#ifdef ROW_TLS_X509
    zc_x509_init(&zx, (void *)iob, (void *)iob, 100, 0, 0);
    br_ssl_engine_set_x509(e, &zx.vtable);
    zc_anchors_find(&zx, 0, 0, (void *)b);
    zc_x509_add_cert(&zx, (void *)b, 10);
    zc_x509_set_time_callback(&zx, 0, tcb);
    br_ssl_engine_set_ecdsa(e, zc_ecdsa_vrfy_asn1);
    (void)xc;
#else
    br_x509_minimal_init(&xc, &br_sha256_vtable, 0, 0);
    br_x509_minimal_set_hash(&xc, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(&xc, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(&xc, br_sha512_ID, &br_sha512_vtable);
    br_x509_minimal_set_rsa(&xc, br_rsa_i15_pkcs1_vrfy);
    br_x509_minimal_set_ecdsa(&xc, &br_ec_all_m15, br_ecdsa_i15_vrfy_asn1);
    br_x509_minimal_set_time_callback(&xc, 0, tcb);
    br_ssl_engine_set_x509(e, &xc.vtable);
    br_ssl_engine_set_ecdsa(e, br_ecdsa_i15_vrfy_asn1);
    (void)zx;
#endif
    br_ssl_client_set_rsapub(&cc, br_rsa_i15_public);
    br_ssl_engine_set_rsavrfy(e, br_rsa_i15_pkcs1_vrfy);
    br_ssl_engine_set_ec(e, &br_ec_all_m15);
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
    br_ssl_engine_set_buffer(e, iob, sizeof iob, 1);
    br_ssl_engine_inject_entropy(e, iob, 32);
    br_ssl_client_reset(&cc, "x", 0);
    br_ssl_engine_sendapp_buf(e, &n); br_ssl_engine_sendapp_ack(e, n);
    br_ssl_engine_recvapp_buf(e, &n); br_ssl_engine_recvapp_ack(e, n);
    br_ssl_engine_sendrec_buf(e, &n); br_ssl_engine_sendrec_ack(e, n);
    br_ssl_engine_recvrec_buf(e, &n); br_ssl_engine_recvrec_ack(e, n);
    br_ssl_engine_flush(e, 0); br_ssl_engine_close(e); (void)br_ssl_engine_current_state(e);
}
#endif
