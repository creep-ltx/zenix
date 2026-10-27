/* SPDX-License-Identifier: GPL-2.0-only */
/* Strict signature checks over the libraries' (zc_verify.c): Ed25519 with R's encoding checked, ECDSA with exactly
   one encoding per signature. The ECDSA ones have br_ecdsa_vrfy's shape, for BearSSL's TLS engine and x509_minimal. */
#ifndef ZC_VERIFY_H
#define ZC_VERIFY_H
#include <stddef.h>
#include <stdint.h>
#include "../bearssl/inc/bearssl.h"

/* 0 when the signature verifies, -1 otherwise (crypto_ed25519_check plus RFC 8032 5.1.3's rule for R) */
int zc_ed25519_check(const unsigned char sig[64], const unsigned char pub[32], const unsigned char *msg, size_t n);
/* 1 when sig is a DER SEQUENCE of two positive, minimally encoded INTEGERs and nothing else */
int zc_ecdsa_der_strict(const unsigned char *sig, size_t n);
uint32_t zc_ecdsa_vrfy_asn1(const br_ec_impl *impl, const void *hash, size_t hash_len, const br_ec_public_key *pk,
                            const void *sig, size_t sig_len);
/* raw r || s: exactly twice the curve's field length (32, 48, 66 bytes) */
uint32_t zc_ecdsa_vrfy_raw(const br_ec_impl *impl, const void *hash, size_t hash_len, const br_ec_public_key *pk,
                           const void *sig, size_t sig_len);
#endif
