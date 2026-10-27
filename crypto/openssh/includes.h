/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * What OpenSSH's blowfish.c, blf.h and bcrypt_pbkdf.c expect from OpenSSH's own includes.h (DESIGN D21 3.2): the
 * BSD integer names, blf.h found as <blf.h>, and the wiping calls mapped to crypto/zc/'s zc_wipe. The three files
 * are upstream's, unchanged (README).
 */
#ifndef ZENIX_OPENSSH_INCLUDES_H
#define ZENIX_OPENSSH_INCLUDES_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../zc/zc.h"

#define HAVE_BLF_H 1
#define HAVE_STDLIB_H 1
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef uint32_t u_int32_t;

#define explicit_bzero(p, n) zc_wipe((p), (n))
#define freezero(p, n) do { zc_wipe((p), (n)); free(p); } while (0)
/* bcrypt_pbkdf's failure path "overwrites the key with random in case the caller does not check": every Zenix
   caller checks; zeros do */
#define arc4random_buf(p, n) zc_wipe((p), (n))

int bcrypt_pbkdf(const char *pass, size_t passlen, const uint8_t *salt, size_t saltlen, uint8_t *key, size_t keylen,
                 unsigned int rounds);

#endif
