/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * crypto/zc/: Zenix's own part of the crypto library (DESIGN D21, docs/d21-crypto-ssh-https.md 3.5): random bytes
 * through a callback, wiping, X25519 with the all-zero check, ssh's chacha20-poly1305@openssh.com packet framing and
 * key derivation (RFC 4253 7.2 with the key extension), the detached signature file (6.2) and the X.509 wrapper with
 * the trust-anchor file (8.1, zc_x509.h). Plain C without system calls and without memory of its own (the caller
 * holds the structs): the same files build for Zenix programs, zenixboot and the PC tests (tools/host/crypto-test.sh).
 * The primitives are BearSSL's and Monocypher's (crypto/bearssl/, crypto/monocypher/); nothing here is a primitive
 * of our own.
 */
#ifndef ZC_H
#define ZC_H

#include <stddef.h>
#include <stdint.h>

/* ---- wiping (9.1): through a volatile pointer, so the compiler cannot drop it before a free ---- */
void zc_wipe(void *p, size_t n);

/* ---- random bytes (5, 4.2): one ChaCha20 generator per process, with fast key erasure, seeded once ----
 * seed: the caller's source of 32 bytes (Zenix: a read of /dev/random, which waits until the pool was seeded once);
 * it fills exactly n bytes and returns 0, or returns a negative error number that zc_rng_seed hands back. */
typedef int (*zc_seedfn)(void *u, unsigned char *buf, size_t n);
struct zc_rng {
    unsigned char key[32];
    unsigned char buf[32];              /* output not yet handed out (the second half of the last block) */
    unsigned int left;                  /* bytes left in buf, taken from its end */
    int ready;                          /* seeded */
};
int zc_rng_seed(struct zc_rng *r, zc_seedfn seed, void *u);    /* 0, or the callback's error */
/* n random bytes; the generator must be seeded (an unseeded one fills out with zeros and returns -1, never silently
   weak bytes) */
int zc_rng_bytes(struct zc_rng *r, void *out, size_t n);
void zc_rng_wipe(struct zc_rng *r);

/* ---- X25519 (RFC 7748) with the all-zero check (RFC 8422, RFC 8731; 3.2) ----
 * Both implementations are here until step 2 of D21 times them on the A1200 and picks one (3.2): zc_x25519() is the
 * chosen one (BearSSL's c25519_m15 unless the build defines ZC_X25519_MONOCYPHER). The scalar and the point are
 * RFC 7748's little-endian strings in both. Returns 0, or -1 when the shared secret is all zeros (the connection
 * must end); out is then zeros. */
int zc_x25519(unsigned char out[32], const unsigned char scalar[32], const unsigned char point[32]);
void zc_x25519_public(unsigned char pub[32], const unsigned char scalar[32]);
int zc_x25519_bearssl(unsigned char out[32], const unsigned char scalar[32], const unsigned char point[32]);
int zc_x25519_monocypher(unsigned char out[32], const unsigned char scalar[32], const unsigned char point[32]);

/* ---- ssh: the shared secret as an mpint, the key derivation, chacha20-poly1305@openssh.com (4.2) ---- */

/* K (the X25519 output read as a big-endian number) as an ssh mpint with its 4-byte length: leading zeros stripped,
   a zero byte added when the top bit is set. out needs 37 bytes; returns the bytes written (4 for K = 0). */
size_t zc_ssh_mpint(unsigned char out[37], const unsigned char k[32]);

/* RFC 4253 7.2 with SHA-256 and the key extension: HASH(K || H || letter || session_id), then K2 = HASH(K || H ||
   K1)... until n bytes. kmp: K as an mpint (zc_ssh_mpint), klen its bytes. */
void zc_ssh_kdf(unsigned char *out, size_t n, const unsigned char *kmp, size_t klen, const unsigned char h[32],
                char letter, const unsigned char sid[32]);

/* chacha20-poly1305@openssh.com (OpenSSH's PROTOCOL.chacha20poly1305): the 64 bytes from the key exchange make two
   keys, the first 32 for the payload and the Poly1305 key, the second 32 for the length. */
#define ZC_CHAPOL_TAG 16
struct zc_chapol {
    unsigned char kmain[32];
    unsigned char klen[32];
};
void zc_chapol_init(struct zc_chapol *c, const unsigned char key[64]);
void zc_chapol_wipe(struct zc_chapol *c);
/* Seal a packet in place: pkt holds the 4-byte length and len bytes after it (padding length, payload, padding);
   the length bytes and the rest are encrypted, the 16-byte tag over both written to tag. */
void zc_chapol_seal(const struct zc_chapol *c, uint32_t seq, unsigned char *pkt, size_t len, unsigned char tag[16]);
/* The length of a received packet: decrypts a copy of its first 4 bytes. The caller checks it against its buffer
   and the 35,000-byte limit and acts on nothing else until zc_chapol_open has checked the tag. */
uint32_t zc_chapol_length(const struct zc_chapol *c, uint32_t seq, const unsigned char enc[4]);
/* Check the tag over the encrypted length and len bytes (constant time), then decrypt the len bytes after the
   length in place (the length bytes stay encrypted). Returns 0, or -1 for a bad tag (nothing decrypted). */
int zc_chapol_open(const struct zc_chapol *c, uint32_t seq, unsigned char *pkt, size_t len,
                   const unsigned char tag[16]);

/* ---- the detached signature file (D21 6.1-6.2, frozen once shipped) ----
 * 80 bytes: "ZSG1", format 1, algorithm 1 (Ed25519), two zero bytes, the key id (the first 8 bytes of SHA-256 of
 * the 32-byte public key), the 64-byte signature. The signed message is the label, a NUL and the 32-byte digest
 * the verifier computed itself. */
#define ZC_SIG_SIZE 80
#define ZC_SIG_LABEL_BUNDLE "Zenix bundle signature v1"
#define ZC_SIG_LABEL_INDEX "Zenix package index v1"
#define ZC_SIG_LABELMAX 64              /* a longer label is refused */
#define ZC_SIG_OK        0
#define ZC_SIG_EFORMAT  -1              /* not an 80-byte ZSG1 format 1 Ed25519 file */
#define ZC_SIG_EKEY     -2              /* signed by a key that is not among the given ones */
#define ZC_SIG_EBAD     -3              /* the signature does not verify */
#define ZC_SIG_ELABEL   -4              /* the label is empty, too long or holds a NUL */
void zc_sig_keyid(unsigned char id[8], const unsigned char pub[32]);
/* Verify sig (n bytes as read) over label and digest against the npub public keys; *which (may be NULL) gets the
   index of the key that verified. Every key whose id matches is tried. */
int zc_sig_verify(const unsigned char *sig, size_t n, const char *label, const unsigned char digest[32],
                  const unsigned char (*pub)[32], int npub, int *which);
/* The message (label, NUL, digest) into msg (room for ZC_SIG_LABELMAX + 33); returns its length, or -1 */
int zc_sig_message(unsigned char *msg, const char *label, const unsigned char digest[32]);

#endif
