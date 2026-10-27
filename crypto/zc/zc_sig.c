/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The detached signature file of update bundles and zpkg's package list (DESIGN D21 6.1-6.2, frozen once shipped):
 * 80 bytes, "ZSG1", format 1, algorithm 1 (Ed25519), two zero bytes, the key id, the signature. The message is the
 * label, a NUL and the digest the verifier computed (never one read from a file). tools/zsign.py makes them.
 */
#include <string.h>
#include "zc.h"
#include "../bearssl/inc/bearssl.h"
#include "zc_verify.h"

void zc_sig_keyid(unsigned char id[8], const unsigned char pub[32])
{
    br_sha256_context c;
    unsigned char d[32];

    br_sha256_init(&c);
    br_sha256_update(&c, pub, 32);
    br_sha256_out(&c, d);
    memcpy(id, d, 8);
}

int zc_sig_message(unsigned char *msg, const char *label, const unsigned char digest[32])
{
    size_t n = 0;

    while (n <= ZC_SIG_LABELMAX && label[n])
        n++;
    if (n == 0 || n > ZC_SIG_LABELMAX)
        return -1;
    memcpy(msg, label, n);
    msg[n] = 0;
    memcpy(msg + n + 1, digest, 32);
    return (int)n + 33;
}

int zc_sig_verify(const unsigned char *sig, size_t n, const char *label, const unsigned char digest[32],
                  const unsigned char (*pub)[32], int npub, int *which)
{
    unsigned char msg[ZC_SIG_LABELMAX + 33], id[8];
    int len, i, tried = 0;

    if (which)
        *which = -1;
    if (n != ZC_SIG_SIZE || memcmp(sig, "ZSG1", 4) != 0 || sig[4] != 1 || sig[5] != 1 || sig[6] != 0 || sig[7] != 0)
        return ZC_SIG_EFORMAT;
    len = zc_sig_message(msg, label, digest);
    if (len < 0)
        return ZC_SIG_ELABEL;
    for (i = 0; i < npub; i++) {
        zc_sig_keyid(id, pub[i]);
        if (memcmp(id, sig + 8, 8) != 0)
            continue;
        tried = 1;
        if (zc_ed25519_check(sig + 16, pub[i], msg, (size_t)len) == 0) {
            if (which)
                *which = i;
            return ZC_SIG_OK;
        }
    }
    return tried ? ZC_SIG_EBAD : ZC_SIG_EKEY;
}
