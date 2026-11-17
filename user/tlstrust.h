/* SPDX-License-Identifier: GPL-2.0-only */
/* What a Zenix TLS client needs around the crypto library's glue (crypto/zc/zc_tls.h), one copy for every program
 * that makes TLS connections (DESIGN D13's rule; wget, irc's TLS helper /lib/irc/tlspipe, DESIGN D58): the clock's
 * rules (D21 8.3: is the clock trusted, the floor), the random seed for a handshake (D21 5), and the trust anchors
 * (D21 8.1: /etc/ssl/anchors and the owner's PEM roots in /etc/ssl/local/). Messages go through tt_say, which the
 * program sets (wget's "wget: ..." on standard error). Moved out of wget.c unchanged (2026-11-17). */
#pragma once
#include "libz.h"
#include "../crypto/zc/zc_tls.h"

extern void (*tt_say)(const char *fmt, ...);     /* the program's messages (may be null: none) */

int tt_clock_trusted(void);                     /* the battery clock read at boot, or sntp's guarded answer */
u32 tt_clock_floor(u32 build_date);             /* the newer of the build date and /var/lib/clock */
void tt_civil(u32 t, int *y, int *m, int *d);    /* seconds since 1970 -> a date */
int tt_seed32(unsigned char *b);                 /* 32 bytes from /dev/random: 0, or a negative error */
/* /etc/ssl/anchors opened and /etc/ssl/local/ read, once (the later calls do nothing); said when there are none */
void tt_anchors_open(void);
/* zc_tls_init's find: the anchors for an issuer's name hash */
int tt_find(void *u, struct zc_x509 *x, const unsigned char hash[8], const unsigned char *dn, size_t dnlen);
