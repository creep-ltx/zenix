/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The X.509 wrapper and the anchors file on the PC (DESIGN D21 10 step 1; tools/host/crypto-test.py anchors drives
 * it with a test CA made there and with tools/mkanchors.py's file from Mozilla's roots):
 *   x509-test chain ANCHORS NAME DAYS SECONDS [-l LOCAL.der]... CERT.der...   the chain as a TLS server sends it,
 *            fed through the br_x509_class as BearSSL's engine does; prints the result (0, or the error number)
 *   x509-test lookup ANCHORS HASH                    every anchor with that name hash: "rsa N E DN" or "ec CURVE Q DN"
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../crypto/zc/zc_x509.h"

static FILE *af;
static unsigned char *locals[8];
static size_t locallen[8];
static int nlocal;

static int rd(void *u, unsigned long off, void *buf, unsigned long n)
{
    (void)u;
    if (fseek(af, (long)off, SEEK_SET) || fread(buf, 1, n, af) != n)
        return -1;
    return 0;
}

static unsigned char *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    unsigned char *b = malloc(1 << 20);

    if (!f) {
        fprintf(stderr, "x509-test: cannot read %s\n", p);
        exit(2);
    }
    *n = fread(b, 1, 1 << 20, f);
    fclose(f);
    return b;
}

static int find(void *u, struct zc_x509 *x, const unsigned char hash[8], const unsigned char *dn, size_t dnlen)
{
    int i, r;

    (void)u;
    (void)dn;
    (void)dnlen;
    if (af) {
        r = zc_anchors_find(x, rd, 0, hash);
        if (r)
            return r;
    }
    for (i = 0; i < nlocal; i++) {      /* the owner's own roots (/etc/ssl/local/): every one, the engine picks */
        r = zc_x509_add_cert(x, locals[i], locallen[i]);
        if (r)
            return r;
    }
    return 0;
}

static void hex(const unsigned char *b, size_t n)
{
    while (n--)
        printf("%02x", *b++);
}

int main(int argc, char **argv)
{
    static unsigned char chain[ZC_X509_CHAINMAX], store[16384];
    static struct zc_x509 x;
    int i;

    if (argc >= 4 && !strcmp(argv[1], "lookup")) {
        unsigned char h[8];
        size_t k;

        af = fopen(argv[2], "rb");
        for (i = 0; i < 8; i++)
            sscanf(argv[3] + 2 * i, "%2hhx", &h[i]);
        zc_x509_init(&x, chain, store, sizeof store, 0, 0);
        i = zc_anchors_find(&x, rd, 0, h);
        if (i) {
            printf("error %d\n", i);
            return 0;
        }
        for (k = 0; k < x.nta; k++) {
            br_x509_trust_anchor *t = &x.ta[k];

            if (t->pkey.key_type == BR_KEYTYPE_RSA) {
                printf("rsa ");
                hex(t->pkey.key.rsa.n, t->pkey.key.rsa.nlen);
                printf(" ");
                hex(t->pkey.key.rsa.e, t->pkey.key.rsa.elen);
            } else {
                printf("ec %d ", t->pkey.key.ec.curve);
                hex(t->pkey.key.ec.q, t->pkey.key.ec.qlen);
            }
            printf(" ");
            hex(t->dn.data, t->dn.len);
            printf(" %u\n", t->flags);
        }
        return 0;
    }
    if (argc >= 7 && !strcmp(argv[1], "chain")) {
        const br_x509_class **v = &x.vtable;
        unsigned r;

        af = strcmp(argv[2], "-") ? fopen(argv[2], "rb") : 0;
        zc_x509_init(&x, chain, store, sizeof store, find, 0);
        zc_x509_set_time(&x, (uint32_t)strtoul(argv[4], 0, 10), (uint32_t)strtoul(argv[5], 0, 10));
        for (i = 6; i < argc && !strcmp(argv[i], "-l"); i += 2)
            locals[nlocal] = slurp(argv[i + 1], &locallen[nlocal]), nlocal++;
        (*v)->start_chain(v, strcmp(argv[3], "-") ? argv[3] : 0);
        for (; i < argc; i++) {
            size_t n;
            unsigned char *c = slurp(argv[i], &n);
            size_t off;

            (*v)->start_cert(v, (uint32_t)n);
            for (off = 0; off < n; off += 100)      /* in pieces, as records bring it */
                (*v)->append(v, c + off, n - off < 100 ? n - off : 100);
            (*v)->end_cert(v);
            free(c);
        }
        r = (*v)->end_chain(v);
        printf("%u %s\n", r, r == 0 && (*v)->get_pkey(v, 0) ? "key" : "nokey");
        return 0;
    }
    fprintf(stderr, "usage: x509-test chain ANCHORS NAME DAYS SECONDS [-l LOCAL.der]... CERT.der... | "
                    "lookup ANCHORS HASH\n");
    return 2;
}
