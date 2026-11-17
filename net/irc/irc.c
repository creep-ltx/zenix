/* SPDX-License-Identifier: GPL-2.0-only */
/* irc's core (net/irc/irc.h, DESIGN D58): the configuration, the server's messages, the commands typed, the windows
 * and their scrollback. No system calls: the program gives the time, the bytes and the callbacks. */
#include "irc.h"

/* ---- text ---- */

static int lower(int c)
{
    if (c >= 'A' && c <= ']')           /* RFC 1459: [ \ ] are the capitals of { | } */
        return c + 32;
    if (c == '^')                       /* and ^ of ~ */
        return '~';
    return c;
}

int irc_casecmp(const char *a, const char *b, int n)
{
    for (; n; n--, a++, b++) {
        int x = lower((unsigned char)*a), y = lower((unsigned char)*b);
        if (x != y)
            return x - y;
        if (!x)
            return 0;
    }
    return 0;
}

/* one character of UTF-8 at p (n bytes left): its length (1-4) and *cp, or 0 when the bytes are not one */
static int u8dec(const unsigned char *p, int n, u32 *cp)
{
    u32 c = p[0];
    int k, i;
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) k = 2, c &= 0x1F;
    else if (c >= 0xE0 && c <= 0xEF) k = 3, c &= 0x0F;
    else if (c >= 0xF0 && c <= 0xF4) k = 4, c &= 0x07;
    else return 0;
    if (n < k)
        return 0;
    for (i = 1; i < k; i++) {
        if ((p[i] & 0xC0) != 0x80)
            return 0;
        c = c << 6 | (p[i] & 0x3F);
    }
    if ((k == 3 && c < 0x800) || (k == 4 && (c < 0x10000 || c > 0x10FFFF)) || (c >= 0xD800 && c <= 0xDFFF))
        return 0;
    *cp = c;
    return k;
}

static int u8enc(u32 c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | (c >> 12 & 0x3F));
    o[2] = (char)(0x80 | (c >> 6 & 0x3F)); o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* clean UTF-8: every valid sequence kept, every other byte read as ISO-8859-1; C0 controls (but \1 when keep1, tab
 * as a space), DEL and C1 dropped, mIRC's format codes (^B ^O ^V ^] ^_ ^^ ^Q, ^C with its colours) dropped */
static int clean(const char *in, int n, char *out, int cap, int keep1)
{
    const unsigned char *p = (const unsigned char *)in, *e = p + n;
    int o = 0;
    while (p < e) {
        u32 c;
        int k = u8dec(p, (int)(e - p), &c);
        if (!k) { c = *p; k = 1; }
        p += k;
        if (c == 3) {                   /* ^C, then up to two digits, then ,dd */
            int d = 0;
            while (p < e && d < 2 && *p >= '0' && *p <= '9') p++, d++;
            if (d && p + 1 < e && *p == ',' && p[1] >= '0' && p[1] <= '9') {
                p += 2;
                if (p < e && *p >= '0' && *p <= '9') p++;
            }
            continue;
        }
        if (c == 9) c = ' ';
        if ((c < 32 && !(c == 1 && keep1)) || (c >= 0x7F && c < 0xA0))
            continue;
        if (o + 4 >= cap)
            break;
        o += u8enc(c, out + o);
    }
    out[o] = 0;
    return o;
}

int irc_utf8(const char *in, int n, char *out, int cap)
{
    return clean(in, n, out, cap, 0);
}

int irc_b64(const unsigned char *in, int n, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0, i;
    for (i = 0; i < n; i += 3) {
        u32 v = (u32)in[i] << 16 | (i + 1 < n ? (u32)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[v >> 18];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* ---- to the server ---- */

static void sendf(struct irc *s, const char *fmt, ...)
{
    char b[IRC_LINEMAX + 4];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(b, IRC_LINEMAX - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > IRC_LINEMAX - 2) {
        n = IRC_LINEMAX - 2;
        irc_print(s, s->cur, IRC_K_ERR, "-!- a line over %d bytes: cut there", IRC_LINEMAX);
    }
    if (s->c.latin1) {                  /* the characters of ISO-8859-1 as one byte each, the others as '?' */
        int i = 0, o = 0;
        while (i < n) {
            u32 c;
            int k = u8dec((unsigned char *)b + i, n - i, &c);
            if (!k) { c = (unsigned char)b[i]; k = 1; }
            b[o++] = c < 256 ? (char)c : '?';
            i += k;
        }
        n = o;
    }
    b[n++] = '\r';
    b[n++] = '\n';
    if (s->connected)
        s->send(s->u, b, n);
}

/* ---- windows ---- */

int irc_win_find(const struct irc *s, const char *name)
{
    int i;
    for (i = 1; i < s->nw; i++)
        if (!irc_casecmp(s->w[i]->name, name, -1))
            return i;
    return -1;
}

int irc_win_open(struct irc *s, const char *name, int kind)
{
    struct irc_win *w;
    int i = irc_win_find(s, name);
    if (i >= 0 || s->nw == IRC_WINMAX)
        return i;
    w = calloc(1, sizeof *w);
    if (!w || !(w->ring = calloc((size_t)s->c.scrollback, sizeof *w->ring))) {
        free(w);
        return -1;
    }
    w->cap = s->c.scrollback;
    w->kind = kind;
    snprintf(w->name, sizeof w->name, "%s", name);
    s->w[s->nw] = w;
    return s->nw++;
}

static void nicks_clear(struct irc_win *w)
{
    int i;
    for (i = 0; i < w->nn; i++)
        free(w->nicks[i]);
    w->nn = 0;
}

static void win_free(struct irc_win *w)
{
    int i;
    for (i = 0; i < w->n; i++)
        free(w->ring[(w->head + w->cap - 1 - i) % w->cap]);
    nicks_clear(w);
    free(w->nicks);
    free(w->ring);
    free(w->topic);
    free(w);
}

void irc_win_close(struct irc *s, int i)
{
    if (i <= 0 || i >= s->nw)
        return;
    win_free(s->w[i]);
    memmove(&s->w[i], &s->w[i + 1], (size_t)(s->nw - i - 1) * sizeof s->w[0]);
    s->nw--;
    if (s->cur >= i && s->cur > 0)
        s->cur--;
    irc_select(s, s->cur);
}

void irc_select(struct irc *s, int i)
{
    if (i < 0 || i >= s->nw)
        return;
    s->cur = i;
    s->w[i]->act = IRC_ACT_NONE;
    if (s->shown)
        s->shown(s->u, -1, 0);          /* everything to draw again */
}

static int words_hit(const struct irc *s, const char *t)
{
    const char *w[IRC_WORDS + 1];
    int nw = 0, i;
    w[nw++] = s->me;
    for (i = 0; i < s->c.nhilite; i++)
        w[nw++] = s->c.hilite[i];
    for (i = 0; i < nw; i++) {
        int l = (int)strlen(w[i]);
        const char *p;
        if (!l)
            continue;
        for (p = t; *p; p++)
            if (!irc_casecmp(p, w[i], l) && (p == t || !isalnum((unsigned char)p[-1])) &&
                !isalnum((unsigned char)p[l]))
                return 1;
    }
    return 0;
}

static void add_line(struct irc *s, int wi, int kind, const char *text)
{
    struct irc_win *w = s->w[wi];
    int n = (int)strlen(text), act;
    struct irc_line *l = malloc(sizeof *l + (size_t)n + 1);
    if (!l)
        return;                         /* no memory: the line is not kept (the program's free memory says why) */
    l->t = s->now;
    l->kind = (unsigned char)kind;
    l->nick = 0;
    memcpy(l->text, text, (size_t)n + 1);
    if (kind == IRC_K_MSG || kind == IRC_K_OWN || kind == IRC_K_HI) {
        const char *e = text[0] == '<' ? strchr(text, '>') : 0;
        l->nick = e ? (unsigned short)(e - text + 1) : 0;
    }
    if (w->n == w->cap)
        free(w->ring[w->head]);
    else
        w->n++;
    w->ring[w->head] = l;
    w->head = (w->head + 1) % w->cap;
    if (w->scroll)                      /* reading back: the same lines stay on the screen */
        w->scroll = w->scroll + 1 < w->n ? w->scroll + 1 : w->n - 1;
    act = kind == IRC_K_HI ? IRC_ACT_HI : kind == IRC_K_INFO || kind == IRC_K_ERR ? IRC_ACT_INFO : IRC_ACT_MSG;
    if (w->kind == IRC_W_QUERY && kind == IRC_K_MSG)
        act = IRC_ACT_HI;
    if (wi != s->cur && act > w->act)
        w->act = act;
    if (s->shown)
        s->shown(s->u, wi, l);
}

void irc_print(struct irc *s, int w, int kind, const char *fmt, ...)
{
    char b[IRC_LINEMAX * 2], c[IRC_LINEMAX * 2];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n >= (int)sizeof b)
        n = sizeof b - 1;
    clean(b, n, c, sizeof c, 0);
    if (w < 0 || w >= s->nw)
        w = 0;
    add_line(s, w, kind, c);
}

/* ---- members ---- */

static int nick_index(const struct irc_win *w, const char *nick)
{
    int i;
    for (i = 0; i < w->nn; i++)
        if (!irc_casecmp(w->nicks[i] + 1, nick, -1))
            return i;
    return -1;
}

static void nick_add(struct irc_win *w, const char *nick, char pfx)
{
    int i = nick_index(w, nick), l = (int)strlen(nick);
    char *e;
    if (i >= 0) {
        w->nicks[i][0] = pfx;
        return;
    }
    if (w->nn == w->ncap) {
        int nc = w->ncap ? w->ncap * 2 : 16;
        char **n = realloc(w->nicks, (size_t)nc * sizeof *n);
        if (!n)
            return;
        w->nicks = n;
        w->ncap = nc;
    }
    if (!(e = malloc((size_t)l + 2)))
        return;
    e[0] = pfx;
    memcpy(e + 1, nick, (size_t)l + 1);
    w->nicks[w->nn++] = e;
}

static int nick_del(struct irc_win *w, const char *nick)
{
    int i = nick_index(w, nick);
    if (i < 0)
        return 0;
    free(w->nicks[i]);
    w->nicks[i] = w->nicks[--w->nn];
    return 1;
}

const char *irc_nick_match(const struct irc *s, int wi, const char *p, int n, int nth)
{
    const struct irc_win *w;
    int i;
    if (wi < 0 || wi >= s->nw)
        return 0;
    w = s->w[wi];
    if (w->kind == IRC_W_QUERY)
        return !nth && !irc_casecmp(w->name, p, n) ? w->name : 0;
    for (i = 0; i < w->nn; i++)
        if (!irc_casecmp(w->nicks[i] + 1, p, n) && irc_casecmp(w->nicks[i] + 1, s->me, -1) && !nth--)
            return w->nicks[i] + 1;
    return 0;
}

/* ---- the configuration ---- */

static int onoff(const char *v)
{
    return !strcmp(v, "on") || !strcmp(v, "yes") || !strcmp(v, "1") || !strcmp(v, "true");
}

static char *word(char **p)
{
    char *s = *p, *w;
    while (*s == ' ' || *s == '\t') s++;
    w = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) *s++ = 0;
    *p = s;
    return w;
}

int irc_config(struct irc *s, char *text, char *err, int errn)
{
    struct irc_cfg *c = &s->c;
    char *p = text;
    int ln = 0;
    free(c->text);
    memset(c, 0, sizeof *c);
    c->text = text;
    c->port = 0;
    c->scrollback = 300;
    c->reconnect = 1;
    c->bell = 1;
    while (p && *p) {
        char *l = p, *e = strchr(p, '\n'), *k, *v;
        if (e) *e = 0;
        p = e ? e + 1 : 0;
        ln++;
        e = l + strlen(l);
        while (e > l && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        k = word(&l);
        while (*l == ' ' || *l == '\t') l++;
        v = l;                          /* the rest of the line */
        if (!*k || *k == '#')
            continue;
        if (!*v) {
            snprintf(err, (size_t)errn, "line %d: %s needs a value", ln, k);
            return -1;
        }
        if (!strcmp(k, "server")) c->server = word(&v);
        else if (!strcmp(k, "port")) c->port = atoi(v);
        else if (!strcmp(k, "tls")) c->tls = onoff(v);
        else if (!strcmp(k, "nick")) c->nick = word(&v);
        else if (!strcmp(k, "user")) c->user = word(&v);
        else if (!strcmp(k, "realname")) c->realname = v;
        else if (!strcmp(k, "password")) c->pass = v;
        else if (!strcmp(k, "sasl")) { c->sasl_user = word(&v); c->sasl_pass = v; }
        else if (!strcmp(k, "quit")) c->quitmsg = v;
        else if (!strcmp(k, "charset")) c->latin1 = !strcmp(v, "latin1") || !strcmp(v, "iso-8859-1");
        else if (!strcmp(k, "scrollback")) c->scrollback = atoi(v);
        else if (!strcmp(k, "reconnect")) c->reconnect = onoff(v);
        else if (!strcmp(k, "bell")) c->bell = onoff(v);
        else if (!strcmp(k, "autojoin") || !strcmp(k, "highlight")) {
            char **a = k[0] == 'a' ? c->autojoin : c->hilite;
            int *na = k[0] == 'a' ? &c->nautojoin : &c->nhilite;
            while (*v) {
                char *w = word(&v);
                if (!*w)
                    break;
                if (*na == IRC_WORDS) {
                    snprintf(err, (size_t)errn, "line %d: more than %d words for %s", ln, IRC_WORDS, k);
                    return -1;
                }
                a[(*na)++] = w;
            }
        } else {
            snprintf(err, (size_t)errn, "line %d: unknown setting '%s'", ln, k);
            return -1;
        }
        if ((!strcmp(k, "port") && (c->port <= 0 || c->port > 65535)) ||
            (!strcmp(k, "scrollback") && (c->scrollback < 20 || c->scrollback > 5000))) {
            snprintf(err, (size_t)errn, "line %d: %s out of range", ln, k);
            return -1;
        }
        if (c->sasl_user && !strcmp(k, "sasl") && !*c->sasl_pass) {
            snprintf(err, (size_t)errn, "line %d: sasl needs a name and a password", ln);
            return -1;
        }
    }
    return 0;
}

void irc_init(struct irc *s)
{
    if (!s->c.nick || !*s->c.nick)
        s->c.nick = "zenix";
    if (!s->c.user || !*s->c.user)
        s->c.user = s->c.nick;
    if (!s->c.realname || !*s->c.realname)
        s->c.realname = "Zenix IRC";
    if (!s->c.port)
        s->c.port = s->c.tls ? 6697 : 6667;
    if (s->c.scrollback < 20)
        s->c.scrollback = 300;
    snprintf(s->me, sizeof s->me, "%s", s->c.nick);
    strcpy(s->prefix, "@+");
    strcpy(s->pmodes, "ov");
    strcpy(s->chanmodes[0], "beI");
    strcpy(s->chanmodes[1], "k");
    strcpy(s->chanmodes[2], "l");
    strcpy(s->chanmodes[3], "imnpst");
    strcpy(s->chantypes, "#&");
    s->lag = -1;
    s->nw = 0;
    irc_win_open(s, "", IRC_W_STATUS);
}

void irc_free(struct irc *s)
{
    while (s->nw > 0)
        win_free(s->w[--s->nw]);
    free(s->c.text);
    s->c.text = 0;
}

/* ---- the connection ---- */

void irc_login(struct irc *s)
{
    s->connected = 1;
    s->registered = 0;
    s->tries = 0;
    s->inn = 0;
    s->cut = 0;
    s->ping_sent = 0;
    s->lag = -1;
    s->sasl = 0;
    if (s->c.sasl_user) {
        s->sasl = 1;
        sendf(s, "CAP REQ :sasl");
    }
    if (s->c.pass)
        sendf(s, "PASS :%s", s->c.pass);
    sendf(s, "NICK %s", s->me);
    sendf(s, "USER %s 0 * :%s", s->c.user, s->c.realname);
}

void irc_lost(struct irc *s, const char *why)
{
    int i;
    s->connected = 0;
    s->registered = 0;
    for (i = 1; i < s->nw; i++)
        if (s->w[i]->kind == IRC_W_CHAN && s->w[i]->joined) {
            s->w[i]->joined = 0;
            s->w[i]->rejoin = 1;
            nicks_clear(s->w[i]);
            irc_print(s, i, IRC_K_ERR, "-!- disconnected: %s", why);
        }
    irc_print(s, 0, IRC_K_ERR, "-!- disconnected: %s", why);
}

void irc_keepalive(struct irc *s)
{
    if (s->registered) {
        s->ping_sent = s->now ? s->now : 1;
        sendf(s, "PING :zirc");
    }
}

/* ---- the server's messages ---- */

struct msg {
    char *nick, *uh, *cmd;
    char *p[16];
    int np;
};

static int parse(char *l, struct msg *m)
{
    memset(m, 0, sizeof *m);
    if (*l == '@') {                    /* IRCv3 tags: not asked for, skipped */
        while (*l && *l != ' ') l++;
        while (*l == ' ') l++;
    }
    if (*l == ':') {
        char *x;
        m->nick = ++l;
        while (*l && *l != ' ') l++;
        if (*l) *l++ = 0;
        if ((x = strchr(m->nick, '!'))) {
            *x = 0;
            m->uh = x + 1;
        }
        while (*l == ' ') l++;
    }
    m->cmd = l;
    while (*l && *l != ' ') l++;
    while (*l == ' ') *l++ = 0;
    while (*l && m->np < 16) {
        if (*l == ':') {
            m->p[m->np++] = l + 1;
            break;
        }
        m->p[m->np++] = l;
        while (*l && *l != ' ') l++;
        while (*l == ' ') *l++ = 0;
    }
    if (!m->nick)
        m->nick = "";
    return *m->cmd ? 0 : -1;
}

static const char *P(const struct msg *m, int i)
{
    return i < m->np ? m->p[i] : "";
}

static int is_chan(const struct irc *s, const char *t)
{
    return *t && strchr(s->chantypes, *t);
}

static int is_me(const struct irc *s, const char *n)
{
    return !irc_casecmp(n, s->me, -1);
}

/* the window a message to target from nick belongs in (a query opened for a private one) */
static int target_win(struct irc *s, const char *target, const char *nick, int open)
{
    int w;
    if (is_chan(s, target))
        return irc_win_find(s, target);
    if (!*nick || strchr(nick, '.'))    /* a server */
        return 0;
    w = irc_win_find(s, nick);
    /* a stranger opens a window, not every one: 4 stay for channels, a name too long for a window gets none */
    if (w < 0 && open && s->nw < IRC_QUERYMAX && strlen(nick) < IRC_NAMEMAX)
        w = irc_win_open(s, nick, IRC_W_QUERY);
    return w;
}

static void ctcp(struct irc *s, const struct msg *m, char *body, int notice)
{
    char *arg = strchr(body, ' '), *e = strchr(body, 1);
    int w;
    if (e) *e = 0;
    if (arg) *arg++ = 0;
    if (!strcmp(body, "ACTION") && !notice) {
        w = target_win(s, P(m, 0), m->nick, 1);
        irc_print(s, w, words_hit(s, arg ? arg : "") ? IRC_K_HI : IRC_K_ACT, "* %s %s", m->nick, arg ? arg : "");
        return;
    }
    if (notice) {
        irc_print(s, s->cur, IRC_K_NOTICE, "-!- CTCP %s reply from %s: %s", body, m->nick, arg ? arg : "");
        return;
    }
    irc_print(s, 0, IRC_K_INFO, "-!- CTCP %s from %s", body, m->nick);
    if (s->ctcp_at && s->now - s->ctcp_at < 2)
        return;                         /* a flood of requests is not answered */
    s->ctcp_at = s->now ? s->now : 1;
    if (!strcmp(body, "VERSION"))
        sendf(s, "NOTICE %s :\1VERSION Zenix irc " IRC_VERSION "\1", m->nick);
    else if (!strcmp(body, "PING"))
        sendf(s, "NOTICE %s :\1PING %.64s\1", m->nick, arg ? arg : "");
}

static void privmsg(struct irc *s, const struct msg *m, int notice)
{
    const char *t = P(m, 0);
    char *x = m->np > 1 ? m->p[1] : "";
    int w;
    if (x[0] == 1) {
        ctcp(s, m, x + 1, notice);
        return;
    }
    if (notice && (!*m->nick || strchr(m->nick, '.') || !s->registered)) {
        irc_print(s, 0, IRC_K_NOTICE, "-%s- %s", *m->nick ? m->nick : "server", x);
        return;
    }
    w = target_win(s, t, m->nick, !notice);
    if (notice) {
        irc_print(s, w >= 0 ? w : s->cur, IRC_K_NOTICE, "-%s- %s", m->nick, x);
        return;
    }
    if (w < 0)                          /* every window taken: the status window says whose it is */
        irc_print(s, 0, IRC_K_HI, "<%s:%s> %s", m->nick, t, x);
    else
        irc_print(s, w, words_hit(s, x) ? IRC_K_HI : IRC_K_MSG, "<%s> %s", m->nick, x);
}

static void names_done(struct irc *s, int w)
{
    char b[IRC_LINEMAX];
    struct irc_win *c = s->w[w];
    int i, o = 0, ops = 0;
    c->names_open = 0;
    for (i = 0; i < c->nn; i++)
        ops += c->nicks[i][0] != ' ';
    irc_print(s, w, IRC_K_INFO, "-!- %s: %d nicks (%d with a mode)", c->name, c->nn, ops);
    for (i = 0; i < c->nn; i++) {       /* a few lines of names, each under a screen's width */
        int l = (int)strlen(c->nicks[i]);
        if (o && o + l + 1 > 70) {
            irc_print(s, w, IRC_K_INFO, "%s", b);
            o = 0;
        }
        o += snprintf(b + o, sizeof b - (size_t)o, "%s%s", o ? " " : "",
                      c->nicks[i][0] == ' ' ? c->nicks[i] + 1 : c->nicks[i]);
    }
    if (o)
        irc_print(s, w, IRC_K_INFO, "%s", b);
}

static void mode(struct irc *s, const struct msg *m)
{
    const char *t = P(m, 0), *f;
    int w = irc_win_find(s, t), add = 1, a = 2, i;
    char all[IRC_LINEMAX];
    int o = 0;
    for (i = 1; i < m->np && o < (int)sizeof all - 2; i++)
        o += snprintf(all + o, sizeof all - (size_t)o, "%s%s", i > 1 ? " " : "", m->p[i]);
    all[o] = 0;
    if (!is_chan(s, t)) {
        irc_print(s, 0, IRC_K_INFO, "-!- mode %s %s", t, all);
        return;
    }
    if (w < 0)
        return;
    irc_print(s, w, IRC_K_INFO, "-!- mode %s [%s] by %s", t, all, m->nick);
    for (f = P(m, 1); *f; f++) {
        const char *pm;
        if (*f == '+' || *f == '-') { add = *f == '+'; continue; }
        if ((pm = strchr(s->pmodes, *f))) {            /* a member's mode: the prefix */
            const char *n = P(m, a++);
            int k = nick_index(s->w[w], n);
            if (k >= 0)
                s->w[w]->nicks[k][0] = add ? s->prefix[pm - s->pmodes] : ' ';
        } else if (strchr(s->chanmodes[0], *f) || strchr(s->chanmodes[1], *f) ||
                   (add && strchr(s->chanmodes[2], *f)))
            a++;                        /* a mode with an argument */
    }
}

static void isupport(struct irc *s, const struct msg *m)
{
    int i;
    for (i = 1; i + 1 < m->np; i++) {
        const char *v = m->p[i];
        if (!strncmp(v, "PREFIX=(", 8)) {
            const char *e = strchr(v + 8, ')');
            int n = e ? (int)(e - v - 8) : 0;
            if (n > 0 && n < (int)sizeof s->pmodes && (int)strlen(e + 1) == n) {
                memcpy(s->pmodes, v + 8, (size_t)n);
                s->pmodes[n] = 0;
                memcpy(s->prefix, e + 1, (size_t)n + 1);
            }
        } else if (!strncmp(v, "CHANTYPES=", 10) && strlen(v + 10) < sizeof s->chantypes && v[10])
            strcpy(s->chantypes, v + 10);
        else if (!strncmp(v, "CHANMODES=", 10)) {
            const char *p = v + 10;
            int k;
            for (k = 0; k < 4; k++) {
                int n = 0;
                while (p[n] && p[n] != ',') n++;
                if (n >= (int)sizeof s->chanmodes[k]) n = sizeof s->chanmodes[k] - 1;
                memcpy(s->chanmodes[k], p, (size_t)n);
                s->chanmodes[k][n] = 0;
                p += n;
                if (*p == ',') p++;
            }
        }
    }
}

static void welcome(struct irc *s)
{
    int i;
    s->registered = 1;
    for (i = 0; i < s->c.nautojoin; i++)
        if (irc_win_find(s, s->c.autojoin[i]) < 0)
            sendf(s, "JOIN %s", s->c.autojoin[i]);
    for (i = 1; i < s->nw; i++)         /* after a reconnect: the channels we were in */
        if (s->w[i]->kind == IRC_W_CHAN && s->w[i]->rejoin) {
            s->w[i]->rejoin = 0;
            sendf(s, "JOIN %s", s->w[i]->name);
        }
}

static void numeric(struct irc *s, const struct msg *m, int n)
{
    char b[IRC_LINEMAX];
    int i, o = 0, w;
    switch (n) {
    case 1:                             /* the nick the server took (it may have cut it) */
        if (*P(m, 0) && strlen(P(m, 0)) < sizeof s->me)
            strcpy(s->me, P(m, 0));
        welcome(s);
        break;
    case 5:
        isupport(s, m);
        return;
    case 332:                           /* the topic */
        if ((w = irc_win_find(s, P(m, 1))) > 0) {
            free(s->w[w]->topic);
            s->w[w]->topic = strdup(P(m, 2));
            irc_print(s, w, IRC_K_INFO, "-!- topic: %s", P(m, 2));
        }
        return;
    case 333:
        return;
    case 353:                           /* names: "= #chan :@a +b c" */
        if ((w = irc_win_find(s, P(m, 2))) > 0) {
            char *p = m->np > 3 ? m->p[3] : "";
            if (!s->w[w]->names_open) {
                nicks_clear(s->w[w]);
                s->w[w]->names_open = 1;
            }
            while (*p) {
                char *nk = word(&p), pf = ' ';
                if (!*nk) break;
                while (*nk && strchr(s->prefix, *nk)) {   /* the highest prefix first */
                    if (pf == ' ') pf = *nk;
                    nk++;
                }
                if (*nk) nick_add(s->w[w], nk, pf);
            }
            return;
        }
        break;
    case 366:
        if ((w = irc_win_find(s, P(m, 1))) > 0) {
            names_done(s, w);
            return;
        }
        break;
    case 433:                           /* the nick is taken: while registering, another */
        if (!s->registered && s->tries < 9) {
            int l = (int)strlen(s->me);
            if (l < 30) {
                s->me[l] = '_';
                s->me[l + 1] = 0;
            } else
                s->me[l - 1] = (char)('1' + s->tries);
            s->tries++;
            sendf(s, "NICK %s", s->me);
        }
        break;
    case 903:                           /* SASL done */
        sendf(s, "CAP END");
        break;
    case 902: case 904: case 905: case 906: case 907:
        irc_print(s, 0, IRC_K_ERR, "-!- SASL: the login failed (%d)", n);
        sendf(s, "CAP END");
        return;
    }
    for (i = 1; i < m->np && o < (int)sizeof b - 2; i++)
        o += snprintf(b + o, sizeof b - (size_t)o, "%s%s", i > 1 ? " " : "", m->p[i]);
    b[o < (int)sizeof b ? o : (int)sizeof b - 1] = 0;
    w = (n >= 400 && n < 600) || (n >= 311 && n <= 319) || n == 330 || n == 338 || n == 378 || n == 671 ||
        n == 301 || n == 331 || n == 341 || n == 324 || n == 329 || n == 367 || n == 368 ? s->cur : 0;
    irc_print(s, w, n >= 400 && n < 600 ? IRC_K_ERR : IRC_K_INFO, "%s%s", n >= 311 && n <= 319 ? "-!- " : "", b);
}

static void message(struct irc *s, char *l)
{
    struct msg m;
    const char *c;
    int w, i;
    if (parse(l, &m) < 0)
        return;
    c = m.cmd;
    if (isdigit((unsigned char)c[0]) && isdigit((unsigned char)c[1]) && isdigit((unsigned char)c[2]) && !c[3]) {
        numeric(s, &m, atoi(c));
        return;
    }
    if (!strcmp(c, "PING")) {
        sendf(s, "PONG :%s", P(&m, 0));
    } else if (!strcmp(c, "PONG")) {
        if (s->ping_sent) {
            s->lag = (int)(s->now - s->ping_sent);
            s->ping_sent = 0;
        }
    } else if (!strcmp(c, "PRIVMSG") || !strcmp(c, "NOTICE")) {
        if (m.np >= 2 && *m.p[0])       /* a target and a text, else nothing to show */
            privmsg(s, &m, c[0] == 'N');
    } else if (!strcmp(c, "JOIN")) {
        const char *ch = P(&m, 0);
        if (is_me(s, m.nick)) {
            if ((w = irc_win_open(s, ch, IRC_W_CHAN)) < 0) {
                irc_print(s, 0, IRC_K_ERR, "-!- %s: no window free (%d at most): /part it or /close one", ch,
                          IRC_WINMAX);
                return;
            }
            s->w[w]->joined = 1;
            nicks_clear(s->w[w]);
            irc_select(s, w);
        } else if ((w = irc_win_find(s, ch)) > 0)
            nick_add(s->w[w], m.nick, ' ');
        if (w > 0)
            irc_print(s, w, IRC_K_INFO, "-!- %s [%s] has joined %s", m.nick, m.uh ? m.uh : "", ch);
    } else if (!strcmp(c, "PART")) {
        if ((w = irc_win_find(s, P(&m, 0))) <= 0)
            return;
        if (is_me(s, m.nick))
            irc_win_close(s, w);
        else {
            nick_del(s->w[w], m.nick);
            irc_print(s, w, IRC_K_INFO, "-!- %s has left %s (%s)", m.nick, P(&m, 0), P(&m, 1));
        }
    } else if (!strcmp(c, "KICK")) {
        if ((w = irc_win_find(s, P(&m, 0))) <= 0)
            return;
        irc_print(s, w, is_me(s, P(&m, 1)) ? IRC_K_HI : IRC_K_INFO, "-!- %s was kicked from %s by %s (%s)", P(&m, 1),
                  P(&m, 0), m.nick, P(&m, 2));
        if (is_me(s, P(&m, 1))) {
            s->w[w]->joined = 0;
            nicks_clear(s->w[w]);
        } else
            nick_del(s->w[w], P(&m, 1));
    } else if (!strcmp(c, "QUIT")) {
        for (i = 1; i < s->nw; i++)
            if (nick_del(s->w[i], m.nick) || (s->w[i]->kind == IRC_W_QUERY && !irc_casecmp(s->w[i]->name, m.nick, -1)))
                irc_print(s, i, IRC_K_INFO, "-!- %s has quit (%s)", m.nick, P(&m, 0));
    } else if (!strcmp(c, "NICK")) {
        const char *nn = P(&m, 0);
        int me = is_me(s, m.nick);
        if (!*nn || strlen(nn) >= IRC_NAMEMAX)
            return;
        if (me)
            snprintf(s->me, sizeof s->me, "%s", nn);
        for (i = 1; i < s->nw; i++) {
            int k = nick_index(s->w[i], m.nick);
            if (k >= 0) {
                char pf = s->w[i]->nicks[k][0];
                nick_del(s->w[i], m.nick);
                nick_add(s->w[i], nn, pf);
            } else if (!(s->w[i]->kind == IRC_W_QUERY && !irc_casecmp(s->w[i]->name, m.nick, -1)))
                continue;
            if (s->w[i]->kind == IRC_W_QUERY && !irc_casecmp(s->w[i]->name, m.nick, -1))
                snprintf(s->w[i]->name, sizeof s->w[i]->name, "%s", nn);
            irc_print(s, i, IRC_K_INFO, "-!- %s is now known as %s", m.nick, nn);
        }
        if (me)
            irc_print(s, 0, IRC_K_INFO, "-!- you are now known as %s", nn);
    } else if (!strcmp(c, "TOPIC")) {
        if ((w = irc_win_find(s, P(&m, 0))) > 0) {
            free(s->w[w]->topic);
            s->w[w]->topic = strdup(P(&m, 1));
            irc_print(s, w, IRC_K_INFO, "-!- %s changed the topic to: %s", m.nick, P(&m, 1));
        }
    } else if (!strcmp(c, "MODE")) {
        mode(s, &m);
    } else if (!strcmp(c, "CAP")) {
        const char *sub = P(&m, 1);
        if (!strcmp(sub, "ACK") && strstr(P(&m, 2), "sasl") && s->sasl == 1) {
            s->sasl = 2;
            sendf(s, "AUTHENTICATE PLAIN");
        } else if (!strcmp(sub, "NAK") || (!strcmp(sub, "ACK") && s->sasl != 1)) {
            if (!strcmp(sub, "NAK"))
                irc_print(s, 0, IRC_K_ERR, "-!- the server has no SASL: logging in without it");
            sendf(s, "CAP END");
        }
    } else if (!strcmp(c, "AUTHENTICATE")) {
        if (!strcmp(P(&m, 0), "+") && s->sasl == 2) {
            unsigned char raw[300];
            char b64[404];
            int un = (int)strlen(s->c.sasl_user), pn = (int)strlen(s->c.sasl_pass);
            if (2 * un + pn + 2 > (int)sizeof raw) {
                irc_print(s, 0, IRC_K_ERR, "-!- SASL: the name and password are too long");
                sendf(s, "AUTHENTICATE *");
                return;
            }
            memcpy(raw, s->c.sasl_user, (size_t)un);                /* authzid, authcid, password */
            raw[un] = 0;
            memcpy(raw + un + 1, s->c.sasl_user, (size_t)un);
            raw[2 * un + 1] = 0;
            memcpy(raw + 2 * un + 2, s->c.sasl_pass, (size_t)pn);
            irc_b64(raw, 2 * un + 2 + pn, b64);
            memset(raw, 0, sizeof raw);
            sendf(s, "AUTHENTICATE %s", b64);
            if (strlen(b64) == 400)     /* a piece of exactly 400: an empty one says it is the last */
                sendf(s, "AUTHENTICATE +");
            memset(b64, 0, sizeof b64);
            s->sasl = 3;
        }
    } else if (!strcmp(c, "ERROR")) {
        irc_print(s, 0, IRC_K_ERR, "-!- the server: %s", P(&m, 0));
    } else if (!strcmp(c, "INVITE")) {
        irc_print(s, s->cur, IRC_K_HI, "-!- %s invites you to %s", m.nick, P(&m, 1));
    } else {
        char b[IRC_LINEMAX];
        int o = 0;
        for (i = 0; i < m.np && o < (int)sizeof b - 2; i++)
            o += snprintf(b + o, sizeof b - (size_t)o, " %s", m.p[i]);
        b[o < (int)sizeof b ? o : (int)sizeof b - 1] = 0;
        irc_print(s, 0, IRC_K_INFO, "%s%s", c, b);
    }
}

void irc_input(struct irc *s, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        char ch = b[i];
        if (ch == '\n' || ch == '\r') {
            if (s->inn) {
                char l[IRC_LINEMAX * 2];
                clean(s->in, s->inn, l, sizeof l, 1);
                s->inn = 0;
                message(s, l);
            }
            if (s->cut) {
                irc_print(s, 0, IRC_K_ERR, "-!- the server sent a line over %d bytes: %d bytes cut", IRC_LINEMAX,
                          s->cut);
                s->cut = 0;
            }
            continue;
        }
        if (!ch)
            ch = ' ';                   /* NUL is not allowed in a message */
        if (s->inn < IRC_LINEMAX)
            s->in[s->inn++] = ch;
        else
            s->cut++;
    }
}

/* ---- the commands typed ---- */

/* text to target in pieces of at most IRC_TEXTMAX bytes (whole characters, at a space when one is near) */
static void say_to(struct irc *s, const char *target, const char *t, int action)
{
    int w = irc_win_find(s, target);
    int n = (int)strlen(t);
    if (!s->registered) {
        irc_print(s, s->cur, IRC_K_ERR, "-!- not connected");
        return;
    }
    while (n > 0) {
        int k = n;
        if (k > IRC_TEXTMAX) {
            int j;
            k = IRC_TEXTMAX;
            while (k > 0 && ((unsigned char)t[k] & 0xC0) == 0x80) k--;
            if (!k)
                k = IRC_TEXTMAX;        /* (never with clean text: irc_command's caller cleans it) */
            for (j = k; j > k - 60 && j > 0; j--)
                if (t[j] == ' ') { k = j; break; }
        }
        if (action) {
            sendf(s, "PRIVMSG %s :\1ACTION %.*s\1", target, k, t);
            irc_print(s, w >= 0 ? w : s->cur, IRC_K_ACT, "* %s %.*s", s->me, k, t);
        } else {
            sendf(s, "PRIVMSG %s :%.*s", target, k, t);
            if (w >= 0)
                irc_print(s, w, IRC_K_OWN, "<%s> %.*s", s->me, k, t);
            else
                irc_print(s, s->cur, IRC_K_OWN, "<%s:%s> %.*s", s->me, target, k, t);
        }
        t += k;
        n -= k;
        if (n > 0 && *t == ' ') {
            t++;
            n--;
        }
    }
}

static const char *const help[] = {
    "/join #CHANNEL [KEY]    /part [#CHANNEL] [WHY]   /msg NICK TEXT",
    "/query NICK [TEXT]      /me TEXT                 /nick NICK",
    "/names [#CHANNEL]       /topic [TEXT]            /whois NICK",
    "/notice NICK TEXT       /close                   /window N",
    "/connect [HOST [PORT [tls]]]   /quit [WHY]       /quote LINE",
    "/clear   /help.  Other /COMMANDs go to the server as they are (/mode, /kick, /away...)",
    0 };

void irc_command(struct irc *s, const char *typed)
{
    char b[1024], *a, *cmd;
    struct irc_win *cw = s->w[s->cur];
    if (!*typed)
        return;
    if (strlen(typed) >= sizeof b) {
        irc_print(s, s->cur, IRC_K_ERR, "-!- a line over %d bytes: not sent", (int)sizeof b - 1);
        return;
    }
    if (typed[0] != '/' || typed[1] == '/') {           /* "//text" is text that starts with a slash */
        if (typed[0] == '/') typed++;
        if (s->cur == 0)
            irc_print(s, 0, IRC_K_ERR, "-!- this is the status window: /join a channel or /query a nick");
        else
            say_to(s, cw->name, typed, 0);
        return;
    }
    snprintf(b, sizeof b, "%s", typed + 1);
    a = b;
    cmd = word(&a);
    while (*a == ' ') a++;
    for (char *p = cmd; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit")) {
        snprintf(s->quit, sizeof s->quit, "%s", *a ? a : s->c.quitmsg ? s->c.quitmsg : "Zenix irc");
        sendf(s, "QUIT :%s", s->quit);
        s->want = IRC_WANT_QUIT;
    } else if (!strcmp(cmd, "connect") || !strcmp(cmd, "server") || !strcmp(cmd, "reconnect")) {
        char *h = cmd[0] == 'r' ? "" : word(&a), *p = word(&a), *t = word(&a);
        snprintf(s->want_host, sizeof s->want_host, "%s", *h ? h : s->c.server ? s->c.server : "");
        s->want_tls = *t ? !strcmp(t, "tls") : *h ? 0 : s->c.tls;
        s->want_port = *p ? atoi(p) : *h ? (s->want_tls ? 6697 : 6667) : s->c.port;
        if (!strcmp(p, "tls")) {
            s->want_tls = 1;
            s->want_port = 6697;
        }
        if (!s->want_host[0] || s->want_port <= 0 || s->want_port > 65535)
            irc_print(s, s->cur, IRC_K_ERR, "-!- usage: /connect HOST [PORT [tls]]");
        else
            s->want = IRC_WANT_CONNECT;
    } else if (!strcmp(cmd, "join") || !strcmp(cmd, "j")) {
        char *ch = word(&a);
        if (!*ch)
            irc_print(s, s->cur, IRC_K_ERR, "-!- usage: /join #CHANNEL [KEY]");
        else if (irc_win_find(s, ch) > 0 && s->w[irc_win_find(s, ch)]->joined)
            irc_select(s, irc_win_find(s, ch));
        else
            sendf(s, *a ? "JOIN %s %s" : "JOIN %s", ch, a);
    } else if (!strcmp(cmd, "part") || !strcmp(cmd, "leave")) {
        const char *ch = cw->name;
        if (is_chan(s, a)) ch = word(&a);
        if (!is_chan(s, ch))
            irc_print(s, s->cur, IRC_K_ERR, "-!- usage: /part [#CHANNEL] [WHY]");
        else if (irc_win_find(s, ch) > 0 && !s->w[irc_win_find(s, ch)]->joined)
            irc_win_close(s, irc_win_find(s, ch));
        else
            sendf(s, "PART %s :%s", ch, *a ? a : s->c.quitmsg ? s->c.quitmsg : "");
    } else if (!strcmp(cmd, "msg") || !strcmp(cmd, "query") || !strcmp(cmd, "q")) {
        char *t = word(&a);
        if (!*t || (cmd[0] == 'm' && !*a)) {
            irc_print(s, s->cur, IRC_K_ERR, "-!- usage: /%s NICK %s", cmd, cmd[0] == 'm' ? "TEXT" : "[TEXT]");
            return;
        }
        if (cmd[0] == 'q' && !is_chan(s, t)) {
            int w = irc_win_open(s, t, IRC_W_QUERY);
            if (w < 0) {
                irc_print(s, s->cur, IRC_K_ERR, "-!- no window free (%d at most)", IRC_WINMAX);
                return;
            }
            irc_select(s, w);
        }
        if (*a)
            say_to(s, t, a, 0);
    } else if (!strcmp(cmd, "me")) {
        if (s->cur == 0)
            irc_print(s, 0, IRC_K_ERR, "-!- /me: not in the status window");
        else
            say_to(s, cw->name, a, 1);
    } else if (!strcmp(cmd, "notice")) {
        char *t = word(&a);
        if (!*t || !*a)
            irc_print(s, s->cur, IRC_K_ERR, "-!- usage: /notice NICK TEXT");
        else {
            sendf(s, "NOTICE %s :%s", t, a);
            irc_print(s, s->cur, IRC_K_NOTICE, "-> -%s- %s", t, a);
        }
    } else if (!strcmp(cmd, "nick")) {
        char *n = word(&a);
        if (!*n)
            irc_print(s, s->cur, IRC_K_ERR, "-!- usage: /nick NICK");
        else if (!s->connected)
            snprintf(s->me, sizeof s->me, "%s", n);
        else
            sendf(s, "NICK %s", n);
    } else if (!strcmp(cmd, "names")) {
        const char *ch = *a ? word(&a) : cw->name;
        if (!is_chan(s, ch))
            irc_print(s, s->cur, IRC_K_ERR, "-!- usage: /names [#CHANNEL]");
        else
            sendf(s, "NAMES %s", ch);
    } else if (!strcmp(cmd, "topic")) {
        if (cw->kind != IRC_W_CHAN)
            irc_print(s, s->cur, IRC_K_ERR, "-!- /topic: not in a channel");
        else if (*a)
            sendf(s, "TOPIC %s :%s", cw->name, a);
        else
            irc_print(s, s->cur, IRC_K_INFO, "-!- topic of %s: %s", cw->name, cw->topic ? cw->topic : "(none)");
    } else if (!strcmp(cmd, "whois") || !strcmp(cmd, "wi")) {
        char *n = word(&a);
        if (!*n && cw->kind == IRC_W_QUERY) n = cw->name;
        if (!*n)
            irc_print(s, s->cur, IRC_K_ERR, "-!- usage: /whois NICK");
        else
            sendf(s, "WHOIS %s", n);
    } else if (!strcmp(cmd, "close") || !strcmp(cmd, "wc")) {
        if (s->cur == 0)
            irc_print(s, 0, IRC_K_ERR, "-!- the status window stays");
        else {
            if (cw->kind == IRC_W_CHAN && cw->joined)
                sendf(s, "PART %s :%s", cw->name, s->c.quitmsg ? s->c.quitmsg : "");
            irc_win_close(s, s->cur);
        }
    } else if (!strcmp(cmd, "window") || !strcmp(cmd, "win") || !strcmp(cmd, "w")) {
        int n = atoi(a);
        if (n >= 1 && n <= s->nw)
            irc_select(s, n - 1);
        else
            irc_print(s, s->cur, IRC_K_ERR, "-!- usage: /window N (1-%d)", s->nw);
    } else if (!strcmp(cmd, "quote") || !strcmp(cmd, "raw")) {
        if (*a) sendf(s, "%s", a);
    } else if (!strcmp(cmd, "clear")) {
        struct irc_win *w = cw;
        while (w->n) {
            w->head = (w->head + w->cap - 1) % w->cap;
            free(w->ring[w->head]);
            w->n--;
        }
        w->scroll = 0;
        irc_select(s, s->cur);
    } else if (!strcmp(cmd, "help")) {
        int i;
        for (i = 0; help[i]; i++)
            irc_print(s, s->cur, IRC_K_INFO, "%s", help[i]);
        s->want = IRC_WANT_HELP;
    } else {                            /* the server's own commands as they are */
        for (char *p = cmd; *p; p++) *p = (char)toupper((unsigned char)*p);
        if (!s->connected)
            irc_print(s, s->cur, IRC_K_ERR, "-!- not connected");
        else
            sendf(s, *a ? "%s %s" : "%s", cmd, a);
    }
}
