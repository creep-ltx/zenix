/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * known_hosts (DESIGN D21 4.4; OpenSSH's sshd(8) "SSH_KNOWN_HOSTS FILE FORMAT"): lines of [marker] hostpatterns
 * keytype base64 [comment]; patterns with *, ? and !, "[host]:port" for other ports than 22, hashed names
 * (|1|salt|hash, HMAC-SHA1 of the name); @revoked refuses a key, @cert-authority lines are ignored (no
 * certificates). The text is the file's, untrusted: a line that does not parse is skipped, as OpenSSH does.
 */
#include <string.h>
#include "ssh_int.h"
#include "../../crypto/bearssl/inc/bearssl.h"

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* glob match, case-insensitive (OpenSSH's match_pattern) */
static int glob(const char *p, long pn, const char *s, long sn)
{
    long pi = 0, si = 0, star = -1, mark = 0;

    while (si < sn) {
        if (pi < pn && (p[pi] == '?' || lower((unsigned char)p[pi]) == lower((unsigned char)s[si]))) {
            pi++;
            si++;
        } else if (pi < pn && p[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (star >= 0) {
            pi = star + 1;
            si = ++mark;
        } else {
            return 0;
        }
    }
    while (pi < pn && p[pi] == '*')
        pi++;
    return pi == pn;
}

/* |1|salt|hash: HMAC-SHA1 over the name with the salt as key */
static int hashed(const char *p, long pn, const char *name, long nn)
{
    unsigned char salt[64], want[20], got[20];
    long i = 3, sn, hn;
    br_hmac_key_context k;
    br_hmac_context h;

    while (i < pn && p[i] != '|')
        i++;
    if (i >= pn)
        return 0;
    sn = ssh_b64_decode(salt, sizeof salt, p + 3, i - 3);
    hn = ssh_b64_decode(want, sizeof want, p + i + 1, pn - i - 1);
    if (sn != 20 || hn != 20)
        return 0;
    br_hmac_key_init(&k, &br_sha1_vtable, salt, (size_t)sn);
    br_hmac_init(&h, &k, 0);
    br_hmac_update(&h, name, (size_t)nn);
    br_hmac_out(&h, got);
    return memcmp(got, want, 20) == 0;
}

/* does the host field match? 1 yes, 0 no; a negated pattern that matches makes it no whatever else matches */
static int hosts_match(const char *f, long fn, const char *name, long nn)
{
    long i = 0;
    int yes = 0;

    if (fn > 3 && !memcmp(f, "|1|", 3))
        return hashed(f, fn, name, nn);
    while (i < fn) {
        long j = i;
        int neg = 0;

        while (j < fn && f[j] != ',')
            j++;
        if (f[i] == '!') {
            neg = 1;
            i++;
        }
        if (j > i && glob(f + i, j - i, name, nn)) {
            if (neg)
                return 0;
            yes = 1;
        }
        i = j + 1;
    }
    return yes;
}

/* the name looked up: host, or "[host]:port" */
static long lookup_name(char *out, long cap, const char *host, int port)
{
    long hn = (long)strlen(host), o = 0, i;
    char pb[8];
    int pl = 0;

    if (port && port != 22) {
        int v = port;

        do {
            pb[pl++] = (char)('0' + v % 10);
            v /= 10;
        } while (v && pl < 6);
        if (hn + pl + 3 >= cap)
            return -1;
        out[o++] = '[';
        for (i = 0; i < hn; i++)
            out[o++] = (char)lower((unsigned char)host[i]);
        out[o++] = ']';
        out[o++] = ':';
        while (pl)
            out[o++] = pb[--pl];
    } else {
        if (hn >= cap)
            return -1;
        for (i = 0; i < hn; i++)
            out[o++] = (char)lower((unsigned char)host[i]);
    }
    out[o] = 0;
    return o;
}

static const unsigned char *blob_type(const unsigned char *b, long n, long *tn)
{
    struct rd r;
    const unsigned char *t;

    r.p = b; r.n = n; r.bad = 0;
    t = rd_str(&r, tn);
    return r.bad ? 0 : t;
}

int ssh_kh_check(const char *text, long n, const char *host, int port, const unsigned char *blob, long bloblen,
                 long *line)
{
    char name[300];
    long nn = lookup_name(name, sizeof name, host, port), i = 0, ln = 0, tn;
    long ok_line = 0, rev_line = 0, chg_line = 0, oth_line = 0;
    const unsigned char *btype = blob_type(blob, bloblen, &tn);
    unsigned char kb[1100];

    if (nn < 0 || !btype)
        return SSH_KH_UNKNOWN;
    while (i < n) {
        long e = i, f[4][2], nf = 0, j;
        int revoked = 0;

        while (e < n && text[e] != '\n')
            e++;
        ln++;
        /* split into up to four fields */
        j = i;
        while (j < e && nf < 4) {
            while (j < e && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r'))
                j++;
            if (j >= e || text[j] == '#')
                break;
            f[nf][0] = j;
            while (j < e && text[j] != ' ' && text[j] != '\t' && text[j] != '\r')
                j++;
            f[nf][1] = j;
            nf++;
        }
        i = e + 1;
        if (nf >= 1 && text[f[0][0]] == '@') {
            long mn = f[0][1] - f[0][0];

            if (mn == 8 && !memcmp(text + f[0][0], "@revoked", 8))
                revoked = 1;
            else
                continue;               /* @cert-authority and anything unknown: not for us */
            memmove(f[0], f[1], sizeof f[0] * 3);
            nf--;
        }
        if (nf < 3)
            continue;
        if (!hosts_match(text + f[0][0], f[0][1] - f[0][0], name, nn))
            continue;
        {
            long kn = ssh_b64_decode(kb, sizeof kb, text + f[2][0], f[2][1] - f[2][0]);
            int same_type = (f[1][1] - f[1][0] == tn) && !memcmp(text + f[1][0], btype, (size_t)tn);

            if (kn < 0)
                continue;
            if (kn == bloblen && !memcmp(kb, blob, (size_t)kn)) {
                if (revoked) {
                    if (!rev_line) rev_line = ln;
                } else if (!ok_line) {
                    ok_line = ln;
                }
            } else if (!revoked && same_type) {
                if (!chg_line) chg_line = ln;
            } else if (!revoked) {
                if (!oth_line) oth_line = ln;
            }
        }
    }
    if (rev_line) { *line = rev_line; return SSH_KH_REVOKED; }
    if (ok_line) { *line = ok_line; return SSH_KH_OK; }
    if (chg_line) { *line = chg_line; return SSH_KH_CHANGED; }
    if (oth_line) { *line = oth_line; return SSH_KH_OTHERTYPE; }
    *line = 0;
    return SSH_KH_UNKNOWN;
}

long ssh_kh_line(char *out, long cap, const char *host, int port, const unsigned char *blob, long bloblen)
{
    long o = lookup_name(out, cap, host, port), k;

    if (o < 0 || o + 2 > cap)
        return SSH_KEY_ESPACE;
    out[o++] = ' ';
    k = ssh_pubkey_line(out + o, cap - o, blob, bloblen, 0);
    if (k < 0 || o + k + 2 > cap)
        return SSH_KEY_ESPACE;
    o += k;
    out[o++] = '\n';
    out[o] = 0;
    return o;
}
