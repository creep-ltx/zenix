/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OpenSSH's key formats for ssh and ssh-keygen (DESIGN D21 4.4; OpenSSH's PROTOCOL.key): the private key file
 * openssh-key-v1 (unprotected, or aes256-ctr with bcrypt_pbkdf as OpenSSH writes it), the public key blob and line,
 * and the SHA256 fingerprint. Ed25519 only.
 */
#include <string.h>
#include "ssh_int.h"
#include "../../crypto/bearssl/inc/bearssl.h"

int bcrypt_pbkdf(const char *pass, size_t passlen, const uint8_t *salt, size_t saltlen, uint8_t *key, size_t keylen,
                 unsigned int rounds);

/* (split, so that no line of the source reads as a key block to secret scanners) */
static const char BEGIN[] = "-----BEGIN OPENSSH " "PRIVATE KEY-----";
static const char END[] = "-----END OPENSSH " "PRIVATE KEY-----";
static const char MAGIC[] = "openssh-key-v1";       /* and its NUL */

void ssh_key_blob(unsigned char blob[SSH_ED25519_BLOB], const unsigned char pub[32])
{
    static const unsigned char head[15] = { 0, 0, 0, 11, 's', 's', 'h', '-', 'e', 'd', '2', '5', '5', '1', '9' };

    memcpy(blob, head, 15);
    blob[15] = 0; blob[16] = 0; blob[17] = 0; blob[18] = 32;
    memcpy(blob + 19, pub, 32);
}

static const char *find(const char *t, long n, const char *pat)
{
    long k = (long)strlen(pat), i;

    for (i = 0; i + k <= n; i++)
        if (t[i] == pat[0] && !memcmp(t + i, pat, (size_t)k))
            return t + i;
    return 0;
}

/* aes256-ctr over b (n bytes) with the 48 bytes of bcrypt_pbkdf: key, then the counter's start */
static void aes_ctr(const unsigned char kiv[48], unsigned char *b, long n)
{
    br_aes_ct_ctrcbc_keys k;
    unsigned char iv[16];

    br_aes_ct_ctrcbc_init(&k, kiv, 32);
    memcpy(iv, kiv + 32, 16);
    br_aes_ct_ctrcbc_ctr(&k, iv, b, (size_t)n);
    zc_wipe(&k, sizeof k);
    zc_wipe(iv, sizeof iv);
}

static int str_is(const unsigned char *p, long n, const char *s)
{
    return p && n == (long)strlen(s) && !memcmp(p, s, (size_t)n);
}

int ssh_key_read(const char *text, long n, const char *pass, unsigned char sk[64], char *comment, long cap,
                 unsigned char *work, long worklen)
{
    const char *b = find(text, n, BEGIN), *e;
    long bn, k, pn, cn, sn, kn, en, tn, pubn, privn, comn;
    const unsigned char *ciph, *kdf, *kopt, *pub, *t, *p2, *priv, *com, *salt;
    unsigned char *enc;
    struct rd r, o;
    unsigned long nkeys, c1, c2, rounds = 0;
    int rc = SSH_KEY_EFORMAT, protect;
    long i;

    if (!b)
        return SSH_KEY_ENOTKEY;
    b += sizeof BEGIN - 1;
    e = find(b, n - (b - text), END);
    if (!e)
        return SSH_KEY_EFORMAT;
    bn = ssh_b64_decode(work, worklen, b, e - b);
    if (bn < 0) {                       /* what was decoded before the bad character is wiped too (S18) */
        rc = bn == -1 && (e - b) / 4 * 3 > worklen ? SSH_KEY_ESPACE : SSH_KEY_EFORMAT;
        goto out;
    }
    if (bn < (long)sizeof MAGIC || memcmp(work, MAGIC, sizeof MAGIC)) {
        rc = SSH_KEY_ENOTKEY;
        goto out;
    }
    r.p = work + sizeof MAGIC;
    r.n = bn - (long)sizeof MAGIC;
    r.bad = 0;
    ciph = rd_str(&r, &cn);
    kdf = rd_str(&r, &kn);
    kopt = rd_str(&r, &k);
    nkeys = rd_u32(&r);
    pub = rd_str(&r, &pn);
    enc = (unsigned char *)rd_str(&r, &en);
    if (r.bad || r.n || nkeys != 1)
        goto out;
    protect = !str_is(ciph, cn, "none");
    if (!protect) {
        if (!str_is(kdf, kn, "none") || k)
            goto out;
        if (en % 8)
            goto out;
    } else {
        unsigned char kiv[48];

        if (!str_is(ciph, cn, "aes256-ctr") || !str_is(kdf, kn, "bcrypt")) {
            rc = SSH_KEY_ECIPHER;
            goto out;
        }
        o.p = kopt; o.n = k; o.bad = 0;
        salt = rd_str(&o, &sn);
        rounds = rd_u32(&o);
        if (o.bad || o.n || sn < 1 || sn > 64 || en % 16)
            goto out;
        if (rounds < 1 || rounds > SSH_KEY_ROUNDS_MAX) {
            rc = SSH_KEY_EROUNDS;
            goto out;
        }
        if (!pass) {
            rc = SSH_KEY_ENEEDPASS;
            goto out;
        }
        if (bcrypt_pbkdf(pass, strlen(pass), salt, (size_t)sn, kiv, sizeof kiv, (unsigned)rounds) != 0)
            goto out;
        aes_ctr(kiv, enc, en);
        zc_wipe(kiv, sizeof kiv);
    }
    o.p = enc; o.n = en; o.bad = 0;
    c1 = rd_u32(&o);
    c2 = rd_u32(&o);
    if (o.bad || c1 != c2) {
        rc = protect ? SSH_KEY_EBADPASS : SSH_KEY_EFORMAT;
        goto out;
    }
    t = rd_str(&o, &tn);
    if (o.bad)
        goto out;
    if (!str_is(t, tn, "ssh-ed25519")) {
        rc = SSH_KEY_ETYPE;
        goto out;
    }
    p2 = rd_str(&o, &pubn);
    priv = rd_str(&o, &privn);
    com = rd_str(&o, &comn);
    if (o.bad || pubn != 32 || privn != 64 || memcmp(priv + 32, p2, 32))
        goto out;
    for (i = 0; i < o.n; i++)           /* the padding: 1, 2, 3, ... */
        if (o.p[i] != (unsigned char)(i + 1))
            goto out;
    {
        unsigned char blob[SSH_ED25519_BLOB];

        ssh_key_blob(blob, p2);
        if (pn != SSH_ED25519_BLOB || memcmp(pub, blob, SSH_ED25519_BLOB))
            goto out;
    }
    memcpy(sk, priv, 64);
    if (comment && cap > 0) {
        if (comn > cap - 1)
            comn = cap - 1;
        comn = ssh_clean(comment, (const char *)com, comn);
        comment[comn] = 0;
    }
    rc = SSH_KEY_OK;
out:
    zc_wipe(work, (size_t)worklen);
    return rc;
}

long ssh_key_write(char *out, long cap, const unsigned char sk[64], const char *comment, const char *pass,
                   const unsigned char salt[16], unsigned rounds, const unsigned char check[4], unsigned char *work,
                   long worklen)
{
    struct wr w;
    unsigned char blob[SSH_ED25519_BLOB], *enc, kiv[48];
    long start, len, o = 0, i, b64n;
    int protect = pass && *pass, block = protect ? 16 : 8;
    unsigned char opts[24];

    if (protect && (rounds < 1 || rounds > SSH_KEY_ROUNDS_MAX))
        return SSH_KEY_EROUNDS;
    /* the binary form at the end of work, its base64 at the start of out */
    w.p = work;
    w.n = 0;
    w.cap = worklen;
    w.bad = 0;
    wr_bytes(&w, MAGIC, sizeof MAGIC);
    wr_cstr(&w, protect ? "aes256-ctr" : "none");
    wr_cstr(&w, protect ? "bcrypt" : "none");
    if (protect) {
        opts[0] = 0; opts[1] = 0; opts[2] = 0; opts[3] = 16;
        memcpy(opts + 4, salt, 16);
        opts[20] = (unsigned char)(rounds >> 24);
        opts[21] = (unsigned char)(rounds >> 16);
        opts[22] = (unsigned char)(rounds >> 8);
        opts[23] = (unsigned char)rounds;
        wr_str(&w, opts, 24);
    } else {
        wr_str(&w, "", 0);
    }
    wr_u32(&w, 1);
    ssh_key_blob(blob, sk + 32);
    wr_str(&w, blob, SSH_ED25519_BLOB);
    wr_u32(&w, 0);                      /* the private part's length, filled in below */
    start = w.n;
    wr_bytes(&w, check, 4);
    wr_bytes(&w, check, 4);
    wr_cstr(&w, "ssh-ed25519");
    wr_str(&w, sk + 32, 32);
    wr_str(&w, sk, 64);
    wr_cstr(&w, comment ? comment : "");
    for (i = 1; (w.n - start) % block; i++)
        wr_u8(&w, (unsigned)i);
    if (w.bad) {
        zc_wipe(work, (size_t)worklen);
        return SSH_KEY_ESPACE;
    }
    len = w.n - start;
    work[start - 4] = (unsigned char)(len >> 24);
    work[start - 3] = (unsigned char)(len >> 16);
    work[start - 2] = (unsigned char)(len >> 8);
    work[start - 1] = (unsigned char)len;
    enc = work + start;
    if (protect) {
        if (bcrypt_pbkdf(pass, strlen(pass), salt, 16, kiv, sizeof kiv, rounds) != 0) {
            zc_wipe(work, (size_t)worklen);
            return SSH_KEY_ECIPHER;
        }
        aes_ctr(kiv, enc, len);
        zc_wipe(kiv, sizeof kiv);
    }
    /* the armour: base64 in lines of 70, as ssh-keygen writes (encoded behind the binary in work first) */
    if ((long)sizeof BEGIN + 1 > cap)
        goto space;
    memcpy(out, BEGIN, sizeof BEGIN - 1);
    o = sizeof BEGIN - 1;
    out[o++] = '\n';
    b64n = ssh_b64_encode((char *)work + w.n, worklen - w.n, work, w.n, 1);
    if (b64n < 0)
        goto space;
    for (i = 0; i < b64n; i += 70) {
        long k = b64n - i < 70 ? b64n - i : 70;

        if (o + k + 1 > cap)
            goto space;
        memcpy(out + o, work + w.n + i, (size_t)k);
        o += k;
        out[o++] = '\n';
    }
    if (o + (long)sizeof END + 1 > cap)
        goto space;
    memcpy(out + o, END, sizeof END - 1);
    o += sizeof END - 1;
    out[o++] = '\n';
    zc_wipe(work, (size_t)worklen);
    return o;
space:
    zc_wipe(work, (size_t)worklen);
    zc_wipe(out, (size_t)cap);
    return SSH_KEY_ESPACE;
}

long ssh_pubkey_line(char *out, long cap, const unsigned char *blob, long bloblen, const char *comment)
{
    struct rd r;
    long tn, o, k;
    const unsigned char *t;

    r.p = blob; r.n = bloblen; r.bad = 0;
    t = rd_str(&r, &tn);
    if (r.bad || tn < 1 || tn > 64 || tn + 2 > cap)
        return SSH_KEY_ESPACE;
    memcpy(out, t, (size_t)tn);
    o = tn;
    out[o++] = ' ';
    k = ssh_b64_encode(out + o, cap - o, blob, bloblen, 1);
    if (k < 0)
        return SSH_KEY_ESPACE;
    o += k;
    if (comment && *comment) {
        long cn = (long)strlen(comment);

        if (o + 1 + cn > cap)
            return SSH_KEY_ESPACE;
        out[o++] = ' ';
        memcpy(out + o, comment, (size_t)cn);
        o += cn;
    }
    if (o >= cap)
        return SSH_KEY_ESPACE;
    out[o] = 0;
    return o;
}

long ssh_pubkey_parse(const char *line, long n, unsigned char *blob, long cap)
{
    long i = 0, t0, t1, b0, b1, bn, tn;
    struct rd r;
    const unsigned char *t;

    while (i < n && (line[i] == ' ' || line[i] == '\t'))
        i++;
    t0 = i;
    while (i < n && line[i] != ' ' && line[i] != '\t')
        i++;
    t1 = i;
    while (i < n && (line[i] == ' ' || line[i] == '\t'))
        i++;
    b0 = i;
    while (i < n && line[i] != ' ' && line[i] != '\t' && line[i] != '\n' && line[i] != '\r')
        i++;
    b1 = i;
    if (t1 == t0 || b1 == b0)
        return SSH_KEY_ENOTKEY;
    bn = ssh_b64_decode(blob, cap, line + b0, b1 - b0);
    if (bn < 0)
        return SSH_KEY_EFORMAT;
    r.p = blob; r.n = bn; r.bad = 0;
    t = rd_str(&r, &tn);
    if (r.bad || tn != t1 - t0 || memcmp(t, line + t0, (size_t)tn))
        return SSH_KEY_EFORMAT;
    return bn;
}

void ssh_fingerprint(char out[SSH_FP_LEN], const unsigned char *blob, long bloblen)
{
    br_sha256_context c;
    unsigned char d[32];
    long k;

    br_sha256_init(&c);
    br_sha256_update(&c, blob, (size_t)bloblen);
    br_sha256_out(&c, d);
    memcpy(out, "SHA256:", 7);
    k = ssh_b64_encode(out + 7, SSH_FP_LEN - 8, d, 32, 0);
    out[7 + (k < 0 ? 0 : k)] = 0;
}
