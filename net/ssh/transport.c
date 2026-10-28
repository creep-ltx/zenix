/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ssh's transport (DESIGN D21 4.2): the version lines, the binary packets (plain during the first exchange, then
 * chacha20-poly1305@openssh.com), the key exchange curve25519-sha256 (RFC 8731) with OpenSSH's strict key exchange
 * (servers without it are refused: Terrapin), the host key's signature, the keys by RFC 4253 7.2 with the key
 * extension, rekeying by either side, and the messages the server may send at any time.
 */
#include <string.h>
#include "ssh_int.h"
#include "../../crypto/zc/zc_verify.h"
#include "../../crypto/bearssl/inc/bearssl_hash.h"

/* our offers, in order (3.3, step 3's column); ext-info-c and the strict marker only in the first KEXINIT. The MAC
   list is never used (chacha20-poly1305 carries its own tag and OpenSSH skips the MAC choice for it) but some
   servers want a name they know there. */
static const char KEX_FIRST[] = "curve25519-sha256,curve25519-sha256@libssh.org,ext-info-c,kex-strict-c-v00@openssh.com";
static const char KEX_LATER[] = "curve25519-sha256,curve25519-sha256@libssh.org";
static const char HOSTKEYS[] = "ssh-ed25519";
static const char CIPHER[] = "chacha20-poly1305@openssh.com";
static const char MACS[] = "hmac-sha2-256-etm@openssh.com";
static const char COMP[] = "none";

/* ---- failing, telling ---- */
void ssh_say(struct ssh *s, int kind, const char *text)
{
    if (s->text)
        s->text(s->u, kind, text, (long)strlen(text));
}

static void wipe_secrets(struct ssh *s)
{
    zc_wipe(s->eph_sk, sizeof s->eph_sk);
    zc_wipe(s->kmp, sizeof s->kmp);
    zc_wipe(s->ikex, sizeof s->ikex);
    zc_wipe(&s->hctx, sizeof s->hctx);
    zc_chapol_wipe(&s->rx_next);
}

void ssh_fail(struct ssh *s, const char *why)
{
    if (s->state == SSH_ST_FAILED)
        return;
    s->state = SSH_ST_FAILED;
    s->need = SSH_NEED_NONE;
    s->err = why;
    wipe_secrets(s);
}

/* "what: <the server's text, cleaned>" into errbuf */
void ssh_failf(struct ssh *s, const char *what, const unsigned char *srv, long n)
{
    long k = (long)strlen(what), room = (long)sizeof s->errbuf - 1;

    if (k > room)
        k = room;
    memcpy(s->errbuf, what, (size_t)k);
    if (srv && n > 0 && k + 2 < room) {
        s->errbuf[k++] = ':';
        s->errbuf[k++] = ' ';
        if (n > room - k)
            n = room - k;
        k += ssh_clean(s->errbuf + k, (const char *)srv, n);
    }
    s->errbuf[k] = 0;
    ssh_fail(s, s->errbuf);
}

/* ---- sending ---- */
struct wr *pkt_begin(struct ssh *s, unsigned type)
{
    if (s->state == SSH_ST_FAILED)
        return 0;
    /* from our KEXINIT until our NEWKEYS only the transport's own messages go out (RFC 4253 7.1) */
    if (s->in_kex && !(type <= M_DEBUG || type == M_KEXINIT || type == M_NEWKEYS || type == M_KEX_ECDH_INIT))
        return 0;
    s->w.p = s->sbuf + 5;
    s->w.cap = s->scap - 5 - 255 - ZC_CHAPOL_TAG;
    s->w.n = 0;
    s->w.bad = 0;
    wr_u8(&s->w, type);
    return &s->w;
}

void pkt_send(struct ssh *s)
{
    unsigned char *b = s->sbuf;
    long p = s->w.n, pad, len;

    if (s->state == SSH_ST_FAILED)
        return;
    if (s->w.bad) {
        ssh_fail(s, "a packet did not fit the send buffer");
        return;
    }
    /* padding: at least 4, the packet a multiple of 8 (with an AEAD cipher the length field is not counted) */
    pad = 8 - ((s->tx_on ? 1 : 5) + p) % 8;
    if (pad < 4)
        pad += 8;
    len = 1 + p + pad;
    b[0] = (unsigned char)(len >> 24);
    b[1] = (unsigned char)(len >> 16);
    b[2] = (unsigned char)(len >> 8);
    b[3] = (unsigned char)len;
    b[4] = (unsigned char)pad;
    zc_rng_bytes(s->rng, b + 5 + p, (size_t)pad);
    if (s->tx_on) {
        zc_chapol_seal(&s->tx, (uint32_t)s->tx_seq, b, (size_t)len, b + 4 + len);
        len += ZC_CHAPOL_TAG;
    }
    s->send(s->u, b, 4 + len);
    s->tx_seq = (s->tx_seq + 1) & 0xFFFFFFFFUL;
    s->kex_bytes += (unsigned long)(4 + len);
}

/* ---- the key exchange ---- */
static void hash_str(struct ssh *s, const void *b, long n)
{
    unsigned char l[4];

    l[0] = (unsigned char)((unsigned long)n >> 24);
    l[1] = (unsigned char)((unsigned long)n >> 16);
    l[2] = (unsigned char)((unsigned long)n >> 8);
    l[3] = (unsigned char)n;
    br_sha256_update(&s->hctx, l, 4);
    br_sha256_update(&s->hctx, b, (size_t)n);
}

static void send_kexinit(struct ssh *s)
{
    struct wr *w;
    unsigned char cookie[16];

    if (s->kex_ours)
        return;
    s->in_kex = 1;                      /* (before pkt_begin: the KEXINIT itself is allowed) */
    w = pkt_begin(s, M_KEXINIT);
    if (!w)
        return;
    zc_rng_bytes(s->rng, cookie, 16);
    wr_bytes(w, cookie, 16);
    wr_cstr(w, s->kex_count ? KEX_LATER : KEX_FIRST);
    wr_cstr(w, HOSTKEYS);
    wr_cstr(w, CIPHER);
    wr_cstr(w, CIPHER);
    wr_cstr(w, MACS);
    wr_cstr(w, MACS);
    wr_cstr(w, COMP);
    wr_cstr(w, COMP);
    wr_cstr(w, "");
    wr_cstr(w, "");
    wr_u8(w, 0);                        /* first_kex_packet_follows */
    wr_u32(w, 0);
    if (w->bad || w->n > (long)sizeof s->ikex) {
        ssh_fail(s, "our KEXINIT does not fit");
        return;
    }
    memcpy(s->ikex, w->p, (size_t)w->n);
    s->ikexlen = (int)w->n;
    s->kex_ours = 1;
    pkt_send(s);
    ssh_say(s, SSH_TEXT_PROGRESS, s->kex_count ? "rekeying" : "key exchange");
}

/* the first name of ours the server also lists (the client's order decides, RFC 4253 7.1) */
static int agree(const char *ours, const unsigned char *theirs, long n, char *out, int cap)
{
    const char *p = ours;

    while (*p) {
        const char *e = strchr(p, ',');
        int k = e ? (int)(e - p) : (int)strlen(p);

        if (k < cap) {
            memcpy(out, p, (size_t)k);
            out[k] = 0;
            if (nl_has(theirs, n, out))
                return 1;
        }
        if (!e)
            break;
        p = e + 1;
    }
    return 0;
}

static void got_kexinit(struct ssh *s, const unsigned char *pay, long paylen, struct rd *r)
{
    const unsigned char *l[10];
    long ln[10];
    char kex[40], tmp[40];
    int i, follows;
    struct wr *w;

    if (s->kex_theirs) {
        ssh_fail(s, "a second KEXINIT in one key exchange");
        return;
    }
    if (r->n < 16) {
        ssh_fail(s, "a damaged KEXINIT");
        return;
    }
    r->p += 16;                         /* the cookie */
    r->n -= 16;
    for (i = 0; i < 10; i++)
        l[i] = rd_str(r, &ln[i]);
    follows = rd_bool(r);
    rd_u32(r);
    if (r->bad) {
        ssh_fail(s, "a damaged KEXINIT");
        return;
    }
    /* the strict key exchange marker counts in the first KEXINIT only (PROTOCOL 1.9) */
    if (!s->kex_count) {
        if (!nl_has(l[0], ln[0], "kex-strict-s-v00@openssh.com")) {
            ssh_fail(s, "the server has no strict key exchange (kex-strict-s-v00@openssh.com, the Terrapin fix of "
                        "OpenSSH 9.6 and Dropbear 2024.84): refused");
            return;
        }
        s->strict = 1;
    }
    if (!agree(KEX_LATER, l[0], ln[0], kex, sizeof kex)) {
        ssh_fail(s, "no key exchange in common (Zenix's ssh speaks curve25519-sha256)");
        return;
    }
    if (!nl_has(l[1], ln[1], HOSTKEYS)) {
        ssh_fail(s, "no host key type in common (Zenix's ssh takes ssh-ed25519)");
        return;
    }
    if (!nl_has(l[2], ln[2], CIPHER) || !nl_has(l[3], ln[3], CIPHER)) {
        ssh_fail(s, "no cipher in common (Zenix's ssh speaks chacha20-poly1305@openssh.com)");
        return;
    }
    if (!nl_has(l[6], ln[6], COMP) || !nl_has(l[7], ln[7], COMP)) {
        ssh_fail(s, "the server insists on compression");
        return;
    }
    /* a guessed packet follows: skipped unless the server's first choices are what was agreed */
    if (follows) {
        long k = 0;

        while (k < ln[0] && l[0][k] != ',')
            k++;
        memcpy(tmp, l[0], (size_t)(k < (long)sizeof tmp - 1 ? k : (long)sizeof tmp - 1));
        tmp[k < (long)sizeof tmp - 1 ? k : (long)sizeof tmp - 1] = 0;
        if (strcmp(tmp, kex) != 0 || (ln[1] < (long)strlen(HOSTKEYS) || memcmp(l[1], HOSTKEYS, strlen(HOSTKEYS)) ||
                                      (ln[1] > (long)strlen(HOSTKEYS) && l[1][strlen(HOSTKEYS)] != ',')))
            s->ignore_next = 1;
    }
    s->kex_theirs = 1;
    send_kexinit(s);                    /* the server started this one */
    if (s->state == SSH_ST_FAILED)
        return;
    br_sha256_init(&s->hctx);
    hash_str(s, s->version, (long)strlen(s->version));
    hash_str(s, s->server_version, (long)strlen(s->server_version));
    hash_str(s, s->ikex, s->ikexlen);
    hash_str(s, pay, paylen);
    zc_wipe(s->ikex, sizeof s->ikex);
    if (zc_rng_bytes(s->rng, s->eph_sk, 32) < 0) {
        ssh_fail(s, "the random generator is not seeded");
        return;
    }
    zc_x25519_public(s->eph_pk, s->eph_sk);
    w = pkt_begin(s, M_KEX_ECDH_INIT);
    if (!w)
        return;
    wr_str(w, s->eph_pk, 32);
    pkt_send(s);
    s->kex_waitreply = 1;
}

/* our NEWKEYS, then our side on the new keys (both directions' keys are made here) */
static void send_newkeys(struct ssh *s)
{
    unsigned char k[64];
    struct wr *w = pkt_begin(s, M_NEWKEYS);

    if (!w)
        return;
    pkt_send(s);
    if (s->strict)
        s->tx_seq = 0;
    zc_ssh_kdf(k, 64, s->kmp, (size_t)s->kmplen, s->h, 'C', s->sid);
    zc_chapol_init(&s->tx, k);
    zc_ssh_kdf(k, 64, s->kmp, (size_t)s->kmplen, s->h, 'D', s->sid);
    zc_chapol_init(&s->rx_next, k);
    zc_wipe(k, sizeof k);
    zc_wipe(s->kmp, sizeof s->kmp);
    s->tx_on = 1;
    s->kex_newkeys_out = 1;
    s->in_kex = 0;
    if (s->kex_count)                   /* (during the first exchange there is no session yet) */
        chan_after_kex(s);
}

static void got_reply(struct ssh *s, struct rd *r)
{
    const unsigned char *ks, *qs, *sig, *t, *pk, *sg;
    long ksn, qsn, sign, tn, pkn, sgn;
    struct rd kr, sr;
    unsigned char k[32];

    if (!s->kex_waitreply) {
        ssh_fail(s, "a key exchange reply nobody asked for");
        return;
    }
    s->kex_waitreply = 0;
    ks = rd_str(r, &ksn);
    qs = rd_str(r, &qsn);
    sig = rd_str(r, &sign);
    if (r->bad || qsn != 32 || ksn > SSH_HOSTKEY_MAX) {
        ssh_fail(s, "a damaged key exchange reply");
        return;
    }
    kr.p = ks; kr.n = ksn; kr.bad = 0;
    t = rd_str(&kr, &tn);
    pk = rd_str(&kr, &pkn);
    if (kr.bad || kr.n || tn != 11 || memcmp(t, "ssh-ed25519", 11) || pkn != 32) {
        ssh_fail(s, "the server's host key is not an ssh-ed25519 key");
        return;
    }
    sr.p = sig; sr.n = sign; sr.bad = 0;
    t = rd_str(&sr, &tn);
    sg = rd_str(&sr, &sgn);
    if (sr.bad || sr.n || tn != 11 || memcmp(t, "ssh-ed25519", 11) || sgn != 64) {
        ssh_fail(s, "the host key's signature is not an ssh-ed25519 signature");
        return;
    }
    if (s->kex_count && memcmp(pk, s->hk_pk, 32) != 0) {
        ssh_fail(s, "the server's host key changed during a rekey");
        return;
    }
    if (zc_x25519(k, s->eph_sk, qs) < 0) {
        zc_wipe(s->eph_sk, 32);
        ssh_fail(s, "the server's key exchange value gives an all-zero secret (RFC 8731): refused");
        return;
    }
    zc_wipe(s->eph_sk, 32);
    s->kmplen = (int)zc_ssh_mpint(s->kmp, k);
    zc_wipe(k, sizeof k);
    hash_str(s, ks, ksn);
    hash_str(s, s->eph_pk, 32);
    hash_str(s, qs, 32);
    br_sha256_update(&s->hctx, s->kmp, (size_t)s->kmplen);
    br_sha256_out(&s->hctx, s->h);
    zc_wipe(&s->hctx, sizeof s->hctx);
    if (zc_ed25519_check(sg, pk, s->h, 32) != 0) {
        ssh_fail(s, "the host key's signature over the key exchange does not verify");
        return;
    }
    if (!s->kex_count) {
        memcpy(s->sid, s->h, 32);
        memcpy(s->hostkey, ks, (size_t)ksn);
        s->hostkeylen = (int)ksn;
        memcpy(s->hk_pk, pk, 32);
        ssh_say(s, SSH_TEXT_PROGRESS, "the host key's signature verified");
        s->need = SSH_NEED_HOSTKEY;     /* NEWKEYS waits for the caller's known_hosts answer */
        return;
    }
    send_newkeys(s);
}

static void got_newkeys(struct ssh *s)
{
    if (!s->kex_newkeys_out) {
        ssh_fail(s, "NEWKEYS before the key exchange was done");
        return;
    }
    s->rx = s->rx_next;
    zc_chapol_wipe(&s->rx_next);
    s->rx_on = 1;
    if (s->strict)
        s->rx_seq = 0;
    s->kex_ours = s->kex_theirs = s->kex_newkeys_out = 0;
    s->kex_count++;
    s->kex_bytes = 0;
    s->kex_time = s->now;
    if (s->kex_count == 1) {
        s->state = SSH_ST_AUTH;
        s->ext_info_ok = 1;
        ssh_say(s, SSH_TEXT_PROGRESS, "keys in use");
        auth_begin(s);
    }
}

static void got_ext_info(struct ssh *s, struct rd *r)
{
    unsigned long n = rd_u32(r), i;

    for (i = 0; i < n && !r->bad; i++) {
        long kn, vn;
        const unsigned char *k = rd_str(r, &kn), *v = rd_str(r, &vn);

        if (!r->bad && kn == 15 && !memcmp(k, "server-sig-algs", 15)) {
            if (vn > (long)sizeof s->server_sig_algs - 1)
                vn = (long)sizeof s->server_sig_algs - 1;
            vn = ssh_clean(s->server_sig_algs, (const char *)v, vn);
            s->server_sig_algs[vn] = 0;
        }
    }
}

static void unimplemented(struct ssh *s, unsigned long seq)
{
    struct wr *w = pkt_begin(s, M_UNIMPLEMENTED);

    if (!w)
        return;
    wr_u32(w, seq);
    pkt_send(s);
}

static void maybe_rekey(struct ssh *s)
{
    unsigned long lb = s->rekey_bytes ? s->rekey_bytes : SSH_REKEY_BYTES;
    unsigned long ls = s->rekey_secs ? s->rekey_secs : SSH_REKEY_SECS;

    if (s->kex_count && !s->kex_ours && !s->kex_theirs && s->state != SSH_ST_FAILED &&
        (s->kex_bytes >= lb || s->now - s->kex_time >= ls))
        send_kexinit(s);
}

static void dispatch(struct ssh *s, const unsigned char *pay, long n, unsigned long seq)
{
    unsigned type = pay[0];
    struct rd r;

    r.p = pay + 1;
    r.n = n - 1;
    r.bad = 0;
    /* a wrong guess's packet: a key exchange message; during the first exchange only one curve25519's own (30, 31)
     * could be it, any other goes on to the strict rule below (audit 2026-10-28 S20: a type 40 was skipped there) */
    if (s->ignore_next && type >= 30 && type <= 49 && (s->kex_count || type <= 31)) {
        s->ignore_next = 0;
        return;
    }
    /* strict key exchange: until the first NEWKEYS only KEXINIT (first), the reply and NEWKEYS (PROTOCOL 1.9) */
    if (!s->kex_count) {
        if (!s->kex_theirs && type != M_KEXINIT) {
            ssh_fail(s, "the server's first packet is not KEXINIT (strict key exchange)");
            return;
        }
        if (type != M_KEXINIT && type != M_KEX_ECDH_REPLY && type != M_NEWKEYS) {
            ssh_fail(s, "an unexpected message during the first key exchange (strict key exchange: Terrapin)");
            return;
        }
    }
    switch (type) {
    case M_DISCONNECT: {
        long dn;
        const unsigned char *d;

        rd_u32(&r);
        d = rd_str(&r, &dn);
        if (s->text && d) {
            long k = dn > 512 ? 512 : dn;
            char tb[512];

            k = ssh_clean(tb, (const char *)d, k);
            s->text(s->u, SSH_TEXT_DISCONNECT, tb, k);
        }
        ssh_failf(s, "the server ended the connection", d, d ? dn : 0);
        return;
    }
    case M_IGNORE:
        return;
    case M_DEBUG: {
        long dn;
        int always = rd_bool(&r);
        const unsigned char *d = rd_str(&r, &dn);

        if (s->text && d) {
            char tb[256];
            long k = ssh_clean(tb, (const char *)d, dn > 255 ? 255 : dn);

            s->text(s->u, SSH_TEXT_DEBUG | (always ? SSH_TEXT_ALWAYS : 0), tb, k);
        }
        return;
    }
    case M_UNIMPLEMENTED:
        ssh_say(s, SSH_TEXT_PROGRESS, "the server did not know a message of ours");
        return;
    case M_KEXINIT:
        got_kexinit(s, pay, n, &r);
        return;
    case M_KEX_ECDH_REPLY:
        got_reply(s, &r);
        return;
    case M_NEWKEYS:
        got_newkeys(s);
        return;
    case M_EXT_INFO:
        if (s->ext_info_ok) {
            got_ext_info(s, &r);
            return;
        }
        break;
    case M_SERVICE_ACCEPT:
        if (auth_packet(s, type, &r))
            return;
        break;
    case M_GLOBAL_REQUEST: {
        long k;

        rd_str(&r, &k);
        if (!r.bad && rd_bool(&r)) {    /* want-reply: REQUEST_FAILURE (keepalive@openssh.com among them) */
            if (s->in_kex) {
                s->pend_gfail++;
            } else {
                struct wr *w = pkt_begin(s, M_REQUEST_FAILURE);

                if (w)
                    pkt_send(s);
            }
        }
        return;
    }
    case M_REQUEST_SUCCESS:
    case M_REQUEST_FAILURE:             /* answers to our keepalive */
        return;
    default:
        if (type >= 50 && type <= 79 && s->state == SSH_ST_AUTH && auth_packet(s, type, &r))
            return;
        if (type >= 90 && type <= 127 && s->state >= SSH_ST_OPEN && chan_packet(s, type, &r))
            return;
        break;
    }
    unimplemented(s, seq);
}

/* ---- receiving ---- */
static void consume(struct ssh *s, long k)
{
    if (k < s->rlen)
        memmove(s->rbuf, s->rbuf + k, (size_t)(s->rlen - k));
    s->rlen -= k;
}

static int version_line(struct ssh *s)
{
    long i;

    for (i = 0; i < s->rlen && s->rbuf[i] != '\n'; i++)
        ;
    if (i == s->rlen) {
        if (s->rlen > SSH_LINEMAX)
            ssh_fail(s, "the server's first line is longer than 255 bytes");
        return 0;
    }
    if (i + 1 > SSH_LINEMAX) {
        ssh_fail(s, "the server sent a line longer than 255 bytes");
        return 0;
    }
    {
        long len = (i > 0 && s->rbuf[i - 1] == '\r') ? i - 1 : i;

        if (len >= 4 && !memcmp(s->rbuf, "SSH-", 4)) {
            if (!(len >= 8 && !memcmp(s->rbuf, "SSH-2.0-", 8)) && !(len >= 9 && !memcmp(s->rbuf, "SSH-1.99-", 9))) {
                ssh_fail(s, "the server does not speak SSH 2");
                return 0;
            }
            memcpy(s->server_version, s->rbuf, (size_t)len);
            s->server_version[len] = 0;
            consume(s, i + 1);
            s->state = SSH_ST_KEX;
            send_kexinit(s);
            return 1;
        }
        if (++s->lines > SSH_PREAMBLE_MAX) {
            ssh_fail(s, "no SSH version line in the server's first 1,024 lines");
            return 0;
        }
        if (s->text) {
            char tb[SSH_LINEMAX + 1];
            long k = ssh_clean(tb, (const char *)s->rbuf, len);

            s->text(s->u, SSH_TEXT_PREAMBLE, tb, k);
        }
        consume(s, i + 1);
        return 1;
    }
}

/* one packet if it is all there: 1 when one was handled, 0 when more bytes are needed */
static int packet(struct ssh *s)
{
    unsigned long len;
    long need;
    unsigned pad;
    unsigned long seq;

    if (s->rlen < 4)
        return 0;
    if (s->rx_on) {
        if (!s->rpkt) {
            len = zc_chapol_length(&s->rx, (uint32_t)s->rx_seq, s->rbuf);
            /* the length is used for the bounds only until the tag is checked (4.2) */
            if (len < 8 || len > SSH_PKTMAX || len % 8) {
                ssh_fail(s, "a bad packet length (a damaged or attacked connection)");
                return 0;
            }
            s->rpkt = len;
        }
        len = s->rpkt;
        need = 4 + (long)len + ZC_CHAPOL_TAG;
    } else {
        len = (unsigned long)s->rbuf[0] << 24 | (unsigned long)s->rbuf[1] << 16 | (unsigned long)s->rbuf[2] << 8 |
              s->rbuf[3];
        if (len < 12 || len > SSH_PKTMAX || (4 + len) % 8) {
            ssh_fail(s, "a bad packet length");
            return 0;
        }
        need = 4 + (long)len;
    }
    if (need > s->rcap) {
        unsigned char *nb = s->grow ? s->grow(s->u, s->rbuf, SSH_RBUF_FULL) : 0;

        if (!nb) {
            ssh_fail(s, "a packet larger than the receive buffer (no memory to grow it)");
            return 0;
        }
        s->rbuf = nb;
        s->rcap = SSH_RBUF_FULL;
    }
    if (s->rlen < need)
        return 0;
    if (s->rx_on && zc_chapol_open(&s->rx, (uint32_t)s->rx_seq, s->rbuf, len, s->rbuf + 4 + len) != 0) {
        ssh_fail(s, "a packet failed its authentication tag (a damaged or attacked connection)");
        return 0;
    }
    pad = s->rbuf[4];
    if (pad < 4 || (unsigned long)pad + 2 > len) {
        ssh_fail(s, "a bad padding length");
        return 0;
    }
    seq = s->rx_seq;
    s->rx_seq = (s->rx_seq + 1) & 0xFFFFFFFFUL;
    s->kex_bytes += (unsigned long)need;
    s->rpkt = 0;
    dispatch(s, s->rbuf + 5, (long)(len - pad - 1), seq);
    if (s->state != SSH_ST_FAILED)
        consume(s, need);
    maybe_rekey(s);
    return 1;
}

static void process(struct ssh *s)
{
    while (s->state != SSH_ST_FAILED && s->need == SSH_NEED_NONE) {
        int r = s->state == SSH_ST_VERSION ? version_line(s) : packet(s);

        if (!r)
            break;
    }
}

long ssh_input(struct ssh *s, const unsigned char *b, long n)
{
    long took = 0;

    while (n > 0 && s->state != SSH_ST_FAILED && s->need == SSH_NEED_NONE) {
        long room = s->rcap - s->rlen, k;

        if (room <= 0)
            break;                      /* (a full buffer always holds a whole packet: process took it) */
        k = n < room ? n : room;
        memcpy(s->rbuf + s->rlen, b, (size_t)k);
        s->rlen += k;
        took += k;
        b += k;
        n -= k;
        process(s);
    }
    return s->state == SSH_ST_FAILED ? -1 : took;
}

void ssh_start(struct ssh *s)
{
    long vn = (long)strlen(s->version);

    memset(&s->state, 0, sizeof *s - (size_t)((char *)&s->state - (char *)s));
    s->exit_status = -1;
    if (vn > SSH_LINEMAX - 2 || s->rcap < SSH_RBUF_MIN || s->scap < SSH_SBUF_MIN || !s->rng || !s->rng->ready) {
        ssh_fail(s, "ssh_start: a bad version line, a small buffer or an unseeded generator");
        return;
    }
    memcpy(s->sbuf, s->version, (size_t)vn);
    s->sbuf[vn] = '\r';
    s->sbuf[vn + 1] = '\n';
    s->send(s->u, s->sbuf, vn + 2);
}

void ssh_hostkey(struct ssh *s, int ok)
{
    if (s->need != SSH_NEED_HOSTKEY)
        return;
    s->need = SSH_NEED_NONE;
    if (!ok) {
        ssh_fail(s, "the host key was not accepted");
        return;
    }
    send_newkeys(s);
    process(s);
}

void ssh_tick(struct ssh *s, unsigned long now)
{
    if (!s->kex_time && !s->now)
        s->kex_time = now;
    s->now = now;
    maybe_rekey(s);
}

int ssh_rekey(struct ssh *s)
{
    if (!s->kex_count || s->kex_ours || s->kex_theirs || s->state == SSH_ST_FAILED)
        return -1;
    send_kexinit(s);
    return 0;
}

void ssh_wipe(struct ssh *s)
{
    wipe_secrets(s);
    zc_chapol_wipe(&s->tx);
    zc_chapol_wipe(&s->rx);
    zc_wipe(s->sid, sizeof s->sid);
    zc_wipe(s->h, sizeof s->h);
    zc_wipe(s->kbd_text, sizeof s->kbd_text);
}
