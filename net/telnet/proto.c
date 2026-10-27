/* SPDX-License-Identifier: GPL-2.0-only */
/* telnet's protocol (DESIGN D20 3.1, 3.2): the decoder, the encoder, the options by RFC 1143's Q method, TTYPE and
 * NAWS. No system calls; answers leave through t->send. */
#include "telnet.h"

/* decoder states */
#define S_DATA 0
#define S_IAC  1
#define S_VERB 2                        /* after WILL/WONT/DO/DONT: the option */
#define S_SB   3
#define S_SBIAC 4

static void send3(struct tn *t, int verb, int opt)
{
    unsigned char b[3];
    b[0] = TN_IAC;
    b[1] = (unsigned char)verb;
    b[2] = (unsigned char)opt;
    t->send(t->u, b, 3);
}

/* what this client agrees to (the table of 3.1) */
static int we_will(int opt) { return opt == TO_BINARY || opt == TO_SGA || opt == TO_TTYPE || opt == TO_NAWS; }
static int they_may(int opt) { return opt == TO_BINARY || opt == TO_ECHO || opt == TO_SGA; }

static void send_naws(struct tn *t)
{
    unsigned char b[14];
    int n = 0, i, v[4];
    v[0] = (t->cols >> 8) & 255;        /* two 16-bit numbers, big-endian (RFC 1073) */
    v[1] = t->cols & 255;
    v[2] = (t->rows >> 8) & 255;
    v[3] = t->rows & 255;
    b[n++] = TN_IAC;
    b[n++] = TN_SB;
    b[n++] = TO_NAWS;
    for (i = 0; i < 4; i++) {
        b[n++] = (unsigned char)v[i];
        if (v[i] == TN_IAC)
            b[n++] = TN_IAC;            /* a 255 is doubled */
    }
    b[n++] = TN_IAC;
    b[n++] = TN_SE;
    t->send(t->u, b, n);
}

static void send_ttype(struct tn *t)
{
    static const char ansi[] = "ANSI", xterm[] = "XTERM-16COLOR";
    const char *s = t->mode == TN_UNIX ? xterm : ansi;
    unsigned char b[32];
    int n = 0;
    b[n++] = TN_IAC;
    b[n++] = TN_SB;
    b[n++] = TO_TTYPE;
    b[n++] = 0;                         /* IS */
    while (*s)
        b[n++] = (unsigned char)*s++;
    b[n++] = TN_IAC;
    b[n++] = TN_SE;
    t->send(t->u, b, n);
}

/* our side became YES */
static void us_on(struct tn *t, int opt)
{
    if (opt == TO_NAWS)
        send_naws(t);                   /* the size at once when agreed */
}

/* ---- RFC 1143: asking ---- */

static void ask_us(struct tn *t, int opt)           /* we want to WILL opt */
{
    switch (t->us[opt]) {
    case TQ_NO:
        t->us[opt] = TQ_WANTYES;
        send3(t, TN_WILL, opt);
        break;
    case TQ_WANTNO:
        t->usq[opt] = 1;                /* OPPOSITE */
        break;
    case TQ_WANTYES:
        t->usq[opt] = 0;
        break;
    }
}

static void ask_him(struct tn *t, int opt)          /* we want them to DO opt */
{
    switch (t->him[opt]) {
    case TQ_NO:
        t->him[opt] = TQ_WANTYES;
        send3(t, TN_DO, opt);
        break;
    case TQ_WANTNO:
        t->himq[opt] = 1;
        break;
    case TQ_WANTYES:
        t->himq[opt] = 0;
        break;
    }
}

static void start(struct tn *t)
{
    if (t->started || t->raw)
        return;
    t->started = 1;
    ask_us(t, TO_BINARY);
    ask_him(t, TO_BINARY);
    ask_him(t, TO_SGA);
    ask_us(t, TO_NAWS);
}

/* ---- RFC 1143: receiving ---- */

static void got_will(struct tn *t, int opt)         /* they will: their side */
{
    if (opt >= TN_NOPT) {
        send3(t, TN_DONT, opt);         /* nothing past TN_NOPT is agreed: stateless refusal */
        return;
    }
    switch (t->him[opt]) {
    case TQ_NO:
        if (they_may(opt)) {
            t->him[opt] = TQ_YES;
            send3(t, TN_DO, opt);
        } else
            send3(t, TN_DONT, opt);
        break;
    case TQ_YES:
        break;                          /* already: no answer (no loop) */
    case TQ_WANTNO:
        if (t->himq[opt]) {
            t->him[opt] = TQ_YES;
            t->himq[opt] = 0;
        } else
            t->him[opt] = TQ_NO;        /* DONT answered by WILL: an error, taken as no */
        break;
    case TQ_WANTYES:
        if (t->himq[opt]) {
            t->him[opt] = TQ_WANTNO;
            t->himq[opt] = 0;
            send3(t, TN_DONT, opt);
        } else
            t->him[opt] = TQ_YES;
        break;
    }
}

static void got_wont(struct tn *t, int opt)
{
    if (opt >= TN_NOPT)
        return;
    switch (t->him[opt]) {
    case TQ_NO:
        break;
    case TQ_YES:
        t->him[opt] = TQ_NO;
        send3(t, TN_DONT, opt);
        break;
    case TQ_WANTNO:
        if (t->himq[opt]) {
            t->him[opt] = TQ_WANTYES;
            t->himq[opt] = 0;
            send3(t, TN_DO, opt);
        } else
            t->him[opt] = TQ_NO;
        break;
    case TQ_WANTYES:
        t->him[opt] = TQ_NO;
        t->himq[opt] = 0;
        break;
    }
}

static void got_do(struct tn *t, int opt)           /* they want us to: our side */
{
    if (opt >= TN_NOPT) {
        send3(t, TN_WONT, opt);
        return;
    }
    switch (t->us[opt]) {
    case TQ_NO:
        if (we_will(opt)) {
            t->us[opt] = TQ_YES;
            send3(t, TN_WILL, opt);
            us_on(t, opt);
        } else
            send3(t, TN_WONT, opt);
        break;
    case TQ_YES:
        break;
    case TQ_WANTNO:
        if (t->usq[opt]) {
            t->us[opt] = TQ_YES;
            t->usq[opt] = 0;
            us_on(t, opt);
        } else
            t->us[opt] = TQ_NO;
        break;
    case TQ_WANTYES:
        if (t->usq[opt]) {
            t->us[opt] = TQ_WANTNO;
            t->usq[opt] = 0;
            send3(t, TN_WONT, opt);
        } else {
            t->us[opt] = TQ_YES;
            us_on(t, opt);
        }
        break;
    }
}

static void got_dont(struct tn *t, int opt)
{
    if (opt >= TN_NOPT)
        return;
    switch (t->us[opt]) {
    case TQ_NO:
        break;
    case TQ_YES:
        t->us[opt] = TQ_NO;
        send3(t, TN_WONT, opt);
        break;
    case TQ_WANTNO:
        if (t->usq[opt]) {
            t->us[opt] = TQ_WANTYES;
            t->usq[opt] = 0;
            send3(t, TN_WILL, opt);
        } else
            t->us[opt] = TQ_NO;
        break;
    case TQ_WANTYES:
        t->us[opt] = TQ_NO;
        t->usq[opt] = 0;
        break;
    }
}

static void got_sb(struct tn *t)
{
    if (t->sbover || t->sbn < 1)
        return;                         /* too long: dropped whole */
    if (t->sb[0] == TO_TTYPE && t->sbn >= 2 && t->sb[1] == 1 && t->us[TO_TTYPE] == TQ_YES)
        send_ttype(t);                  /* SEND: our name (the same on a second SEND) */
}

void tn_init(struct tn *t, int port)
{
    int i;
    for (i = 0; i < TN_NOPT; i++)
        t->us[i] = t->usq[i] = t->him[i] = t->himq[i] = TQ_NO;
    t->started = 0;
    t->st = S_DATA;
    t->verb = 0;
    t->cr = 0;
    t->sbn = t->sbover = 0;
    if (port == 23)
        start(t);
}

long tn_decode(struct tn *t, const unsigned char *in, long n, unsigned char *out)
{
    long i, k = 0;
    if (t->raw) {
        for (i = 0; i < n; i++)
            out[i] = in[i];
        return n;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = in[i];
        switch (t->st) {
        case S_DATA:
            if (c == TN_IAC) {
                start(t);               /* their first IAC: our asks go first */
                t->st = S_IAC;
                break;
            }
            if (t->cr && c == 0 && t->him[TO_BINARY] != TQ_YES) {
                t->cr = 0;              /* CR NUL is CR */
                break;
            }
            t->cr = c == '\r';
            out[k++] = c;
            break;
        case S_IAC:
            t->st = S_DATA;
            if (c == TN_IAC) {
                t->cr = 0;
                out[k++] = TN_IAC;
            } else if (c >= TN_WILL && c <= TN_DONT) {
                t->verb = c;
                t->st = S_VERB;
            } else if (c == TN_SB) {
                t->sbn = t->sbover = 0;
                t->st = S_SB;
            }
            /* NOP, GA, AYT, BRK, DM, IP, AO, EC, EL, a stray SE: ignored */
            break;
        case S_VERB:
            t->st = S_DATA;
            if (t->verb == TN_WILL) got_will(t, c);
            else if (t->verb == TN_WONT) got_wont(t, c);
            else if (t->verb == TN_DO) got_do(t, c);
            else got_dont(t, c);
            break;
        case S_SB:
            if (c == TN_IAC)
                t->st = S_SBIAC;
            else if (t->sbn < TN_SBMAX)
                t->sb[t->sbn++] = c;
            else
                t->sbover = 1;
            break;
        case S_SBIAC:
            if (c == TN_IAC) {          /* a 255 inside */
                if (t->sbn < TN_SBMAX)
                    t->sb[t->sbn++] = c;
                else
                    t->sbover = 1;
                t->st = S_SB;
            } else if (c == TN_SE) {
                got_sb(t);
                t->st = S_DATA;
            } else {
                /* IAC and a command inside SB: the SB is broken: dropped, the command taken as one */
                t->st = S_IAC;
                i--;
            }
            break;
        }
    }
    return k;
}

long tn_encode(const struct tn *t, const unsigned char *in, long n, unsigned char *out)
{
    long i, k = 0;
    int bin = t->us[TO_BINARY] == TQ_YES;
    if (t->raw) {
        for (i = 0; i < n; i++)
            out[i] = in[i];
        return n;
    }
    for (i = 0; i < n; i++) {
        out[k++] = in[i];
        if (in[i] == TN_IAC)
            out[k++] = TN_IAC;
        else if (in[i] == '\r' && !bin)
            out[k++] = 0;
    }
    return k;
}

void tn_size(struct tn *t, int cols, int rows)
{
    t->cols = cols;
    t->rows = rows;
    if (!t->raw && t->us[TO_NAWS] == TQ_YES)
        send_naws(t);
}

int tn_on(const struct tn *t, int which, int opt)
{
    if (opt < 0 || opt >= TN_NOPT)
        return 0;
    return (which ? t->him[opt] : t->us[opt]) == TQ_YES;
}

int tn_local_echo(const struct tn *t)
{
    return t->mode == TN_UNIX && t->him[TO_ECHO] != TQ_YES;
}
