/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ssh's user authentication (RFC 4252, RFC 4256; DESIGN D21 4.2) in OpenSSH's order: "none" for the list, then
 * publickey with an Ed25519 key (first the query without a signature, so a key the server will not take costs no
 * signature, then the signed request), keyboard-interactive, password (three tries). The caller answers the
 * questions through need (SSH_NEED_PASSWORD, SSH_NEED_KBDINT).
 */
#include <string.h>
#include "ssh_int.h"
#include "../../crypto/monocypher/monocypher-ed25519.h"

#define A_SERVICE  1                    /* SERVICE_REQUEST sent */
#define A_NONE     2                    /* "none" sent */
#define A_PKQUERY  3                    /* publickey without a signature sent */
#define A_PKSIGN   4                    /* publickey with the signature sent */
#define A_KBDINT   5                    /* keyboard-interactive in progress */
#define A_PASSWORD 6                    /* password sent */
#define A_ASKING   7                    /* waiting for the caller (need) */

static struct wr *request(struct ssh *s, const char *method)
{
    struct wr *w = pkt_begin(s, M_USERAUTH_REQUEST);

    if (!w)
        return 0;
    wr_cstr(w, s->user);
    wr_cstr(w, "ssh-connection");
    wr_cstr(w, method);
    return w;
}

void auth_begin(struct ssh *s)
{
    struct wr *w = pkt_begin(s, M_SERVICE_REQUEST);

    if (!w)
        return;
    wr_cstr(w, "ssh-userauth");
    pkt_send(s);
    s->auth_step = A_SERVICE;
}

static int offered(struct ssh *s, const char *m)
{
    return nl_has((const unsigned char *)s->auth_methods, (long)strlen(s->auth_methods), m);
}

static void pk_request(struct ssh *s, int sign)
{
    unsigned char blob[SSH_ED25519_BLOB];
    struct wr *w = request(s, "publickey");

    if (!w)
        return;
    ssh_key_blob(blob, s->id_sk + 32);
    wr_u8(w, sign);
    wr_cstr(w, "ssh-ed25519");
    wr_str(w, blob, SSH_ED25519_BLOB);
    if (sign) {
        /* the signature covers string session_id, then this payload so far (RFC 4252 7); built behind it */
        long n = w->n;
        unsigned char *m = w->p + n + 128, sig[64];

        if (w->bad || n + 128 + 36 + n + 80 > w->cap) {
            ssh_fail(s, "a packet did not fit the send buffer");
            return;
        }
        m[0] = 0; m[1] = 0; m[2] = 0; m[3] = 32;
        memcpy(m + 4, s->sid, 32);
        memcpy(m + 36, w->p, (size_t)n);
        crypto_ed25519_sign(sig, s->id_sk, m, (size_t)(36 + n));
        zc_wipe(m, (size_t)(36 + n));
        wr_u32(w, 4 + 11 + 4 + 64);
        wr_cstr(w, "ssh-ed25519");
        wr_str(w, sig, 64);
    }
    pkt_send(s);
    s->auth_step = sign ? A_PKSIGN : A_PKQUERY;
    ssh_say(s, SSH_TEXT_PROGRESS, sign ? "publickey: signed request sent" : "publickey: asking if the key will do");
}

/* the next method, OpenSSH's order; the end when none is left */
static void next(struct ssh *s)
{
    if ((s->methods & SSH_AUTH_PUBKEY) && s->id_sk && !(s->auth_tried & SSH_AUTH_PUBKEY) && offered(s, "publickey")) {
        pk_request(s, 0);
        return;
    }
    if ((s->methods & SSH_AUTH_KBDINT) && !(s->auth_tried & SSH_AUTH_KBDINT) && offered(s, "keyboard-interactive")) {
        struct wr *w = request(s, "keyboard-interactive");

        if (!w)
            return;
        wr_cstr(w, "");                 /* language */
        wr_cstr(w, "");                 /* submethods */
        pkt_send(s);
        s->auth_step = A_KBDINT;
        return;
    }
    if ((s->methods & SSH_AUTH_PASSWORD) && !(s->auth_tried & SSH_AUTH_PASSWORD) && offered(s, "password") &&
        s->pw_tries < 3) {
        s->auth_step = A_ASKING;
        s->need = SSH_NEED_PASSWORD;
        return;
    }
    ssh_failf(s, "authentication failed; the server offers", (const unsigned char *)s->auth_methods,
              (long)strlen(s->auth_methods));
}

static void info_request(struct ssh *s, struct rd *r)
{
    const unsigned char *nm, *in, *p;
    long nn, inn, pn, used = 0;
    unsigned long np, i;

    nm = rd_str(r, &nn);
    in = rd_str(r, &inn);
    rd_str(r, &pn);                     /* language */
    np = rd_u32(r);
    if (r->bad) {
        ssh_fail(s, "a damaged keyboard-interactive request");
        return;
    }
    if (nn > (long)sizeof s->kbd_name - 1) nn = (long)sizeof s->kbd_name - 1;
    nn = ssh_clean(s->kbd_name, (const char *)nm, nn);
    s->kbd_name[nn] = 0;
    if (inn > (long)sizeof s->kbd_instruction - 1) inn = (long)sizeof s->kbd_instruction - 1;
    inn = ssh_clean(s->kbd_instruction, (const char *)in, inn);
    s->kbd_instruction[inn] = 0;
    if (np > SSH_KBD_PROMPTS) {
        ssh_say(s, SSH_TEXT_PROGRESS, "keyboard-interactive: too many prompts, method given up");
        np = 0;                         /* answered with none: the server counts it a failure */
        s->auth_tried |= SSH_AUTH_KBDINT;
    }
    for (i = 0; i < np; i++) {
        p = rd_str(r, &pn);
        s->kbd_echo[i] = rd_bool(r);
        if (r->bad) {
            ssh_fail(s, "a damaged keyboard-interactive prompt");
            return;
        }
        if (pn > (long)sizeof s->kbd_text - used - 1)
            pn = (long)sizeof s->kbd_text - used - 1;
        s->kbd_prompt[i] = s->kbd_text + used;
        used += ssh_clean(s->kbd_text + used, (const char *)p, pn);
        s->kbd_text[used++] = 0;
        if (used >= (long)sizeof s->kbd_text)
            used = (long)sizeof s->kbd_text - 1;
    }
    s->kbd_n = (int)np;
    if (np == 0) {                      /* nothing to ask (or too much): answered at once with no answers */
        struct wr *w = pkt_begin(s, M_USERAUTH_INFO_RESPONSE);

        if (w) {
            wr_u32(w, 0);
            pkt_send(s);
        }
        return;
    }
    s->auth_step = A_ASKING;
    s->need = SSH_NEED_KBDINT;
}

int auth_packet(struct ssh *s, unsigned type, struct rd *r)
{
    switch (type) {
    case M_SERVICE_ACCEPT: {
        struct wr *w;

        if (s->auth_step != A_SERVICE)
            return 0;
        w = request(s, "none");
        if (!w)
            return 1;
        pkt_send(s);
        s->auth_step = A_NONE;
        return 1;
    }
    case M_USERAUTH_BANNER: {
        long n;
        const unsigned char *b = rd_str(r, &n);

        if (b && s->text) {
            /* cleaned in place: the packet is ours until the next one */
            n = ssh_clean((char *)b, (const char *)b, n);
            s->text(s->u, SSH_TEXT_BANNER, (const char *)b, n);
        }
        return 1;
    }
    case M_USERAUTH_FAILURE: {
        long n;
        const unsigned char *m = rd_str(r, &n);

        if (r->bad) {
            ssh_fail(s, "a damaged authentication failure");
            return 1;
        }
        if (n > (long)sizeof s->auth_methods - 1)
            n = (long)sizeof s->auth_methods - 1;
        n = ssh_clean(s->auth_methods, (const char *)m, n);
        s->auth_methods[n] = 0;
        if (s->auth_step == A_PKQUERY || s->auth_step == A_PKSIGN)
            s->auth_tried |= SSH_AUTH_PUBKEY;
        if (s->auth_step == A_KBDINT)
            s->auth_tried |= SSH_AUTH_KBDINT;
        if (s->auth_step == A_PASSWORD)
            ssh_say(s, SSH_TEXT_PROGRESS, "password: refused");
        next(s);
        return 1;
    }
    case M_USERAUTH_PK_OK:              /* (60: PK_OK, INFO_REQUEST or PASSWD_CHANGEREQ by the method in flight) */
        if (s->auth_step == A_PKQUERY) {
            long an, bn;
            const unsigned char *a = rd_str(r, &an), *b = rd_str(r, &bn);
            unsigned char blob[SSH_ED25519_BLOB];

            ssh_key_blob(blob, s->id_sk + 32);
            if (r->bad || an != 11 || memcmp(a, "ssh-ed25519", 11) || bn != SSH_ED25519_BLOB ||
                memcmp(b, blob, SSH_ED25519_BLOB)) {
                ssh_fail(s, "the server's PK_OK names another key");
                return 1;
            }
            pk_request(s, 1);
            return 1;
        }
        if (s->auth_step == A_KBDINT) {
            info_request(s, r);
            return 1;
        }
        if (s->auth_step == A_PASSWORD) {
            ssh_fail(s, "the server wants the password changed: change it on the server first");
            return 1;
        }
        return 0;
    case M_USERAUTH_SUCCESS:
        if (s->auth_step == A_SERVICE || s->auth_step == A_ASKING)
            return 0;
        s->ext_info_ok = 0;
        s->state = SSH_ST_OPEN;
        zc_wipe(s->kbd_text, sizeof s->kbd_text);
        ssh_say(s, SSH_TEXT_PROGRESS, "authenticated");
        chan_begin(s);
        return 1;
    }
    return 0;
}

void ssh_password(struct ssh *s, const char *password)
{
    struct wr *w;

    if (s->need != SSH_NEED_PASSWORD)
        return;
    s->need = SSH_NEED_NONE;
    if (!password) {
        s->auth_tried |= SSH_AUTH_PASSWORD;
        next(s);
        return;
    }
    w = request(s, "password");
    if (!w)
        return;
    wr_u8(w, 0);
    wr_cstr(w, password);
    pkt_send(s);                        /* (encrypted in place: no plain copy stays in the send buffer) */
    s->pw_tries++;
    s->auth_step = A_PASSWORD;
}

void ssh_kbdint(struct ssh *s, const char *const *answers)
{
    struct wr *w;
    int i;

    if (s->need != SSH_NEED_KBDINT)
        return;
    s->need = SSH_NEED_NONE;
    w = pkt_begin(s, M_USERAUTH_INFO_RESPONSE);
    if (!w)
        return;
    if (!answers) {                     /* given up: an empty answer, the server counts a failure */
        s->auth_tried |= SSH_AUTH_KBDINT;
        wr_u32(w, 0);
    } else {
        wr_u32(w, (unsigned long)s->kbd_n);
        for (i = 0; i < s->kbd_n; i++)
            wr_cstr(w, answers[i]);
    }
    pkt_send(s);
    zc_wipe(s->kbd_text, sizeof s->kbd_text);
    s->auth_step = A_KBDINT;
}
