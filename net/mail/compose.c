/* SPDX-License-Identifier: GPL-2.0-only */
/* mail's settings and the messages it writes (DESIGN D59 sections 3 and 6): ~/.config/mail/config read, addresses
 * (RFC 5322 3.4) read from what the user typed, the editor's text made a message (Date, Message-ID, MIME-Version;
 * RFC 2047 words for names and subjects outside ASCII; quoted-printable for a body outside ASCII or with long lines;
 * CR LF lines; Bcc kept off the message), and the text a new message, a reply and a forward start with. */
#include "mail.h"

typedef unsigned char u8;
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ncase(const char *a, const char *b, u32 n)
{
    for (; n; n--, a++, b++)
        if (lc((u8)*a) != lc((u8)*b)) return 1;
    return 0;
}
static int ws(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* ---- the settings ---- */
static int onoff(const char *v, int tls)
{
    if (!strcmp(v, "on") || !strcmp(v, "yes")) return 1;
    if (!strcmp(v, "off") || !strcmp(v, "no")) return 0;
    if (tls && !strcmp(v, "starttls")) return TLS_STARTTLS;
    return -1;
}

int mail_config(struct mail_cfg *c, char *text, char *err, int errn)
{
    static const char *const words[] = { "imap", "pop", "user", "password", "passcmd", "smtp", "smtp_user",
        "smtp_password", "smtp_passcmd", "from", "sent", "editor", "folder", "port", "tls", "smtp_port", "smtp_tls",
        "limit", "plain_auth" };
    memset(c, 0, sizeof *c);
    c->text = text;
    c->textn = (u32)strlen(text);
    c->tls = TLS_ON;
    c->smtp_tls = TLS_STARTTLS;
    c->limit = 300;
    int ln = 0, port = 0, sport = 0;
    char *p = text;
    while (*p) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        ln++;
        char *k = p;
        p = e ? e + 1 : p + strlen(p);
        while (*k == ' ' || *k == '\t') k++;
        if (!*k || *k == '#' || *k == '\r') continue;
        char *v = k;
        while (*v && !ws((u8)*v)) v++;
        if (*v) *v++ = 0;
        while (*v == ' ' || *v == '\t') v++;
        char *ve = v + strlen(v);
        while (ve > v && ws((u8)ve[-1])) *--ve = 0;
        int w = -1;
        for (int i = 0; i < (int)(sizeof words / sizeof words[0]); i++)
            if (!strcmp(k, words[i])) w = i;
        if (w < 0) {
            snprintf(err, (u32)errn, "line %d: no such setting: %.40s", ln, k);
            return -1;
        }
        if (!*v) {
            snprintf(err, (u32)errn, "line %d: %s needs a value", ln, k);
            return -1;
        }
        char **s[13] = { &c->imap, &c->pop, &c->user, &c->pass, &c->passcmd, &c->smtp, &c->smtp_user, &c->smtp_pass,
                         &c->smtp_passcmd, &c->from, &c->sent, &c->editor, &c->folder };
        if (w < 13) {
            *s[w] = v;
            if (w == 3 || w == 7) c->has_pass = 1;
            continue;
        }
        int n = atoi(v);
        if (w == 13 || w == 15) {
            if (n < 1 || n > 65535) {
                snprintf(err, (u32)errn, "line %d: %s: a port is 1-65535", ln, k);
                return -1;
            }
            if (w == 13) port = n; else sport = n;
        } else if (w == 17) {
            if (n < 10 || n > 5000) {
                snprintf(err, (u32)errn, "line %d: limit: 10-5000 messages", ln);
                return -1;
            }
            c->limit = n;
        } else {
            int o = onoff(v, w != 18);
            if (o < 0) {
                snprintf(err, (u32)errn, "line %d: %s: on, off%s", ln, k, w != 18 ? " or starttls" : "");
                return -1;
            }
            if (w == 14) c->tls = o;
            else if (w == 16) c->smtp_tls = o;
            else c->plain_auth = o;
        }
    }
    if (c->imap && c->pop) {
        snprintf(err, (u32)errn, "imap and pop both set: one of them");
        return -1;
    }
    c->port = port ? port : c->pop ? (c->tls == TLS_ON ? 995 : 110) : (c->tls == TLS_ON ? 993 : 143);
    c->smtp_port = sport ? sport : c->smtp_tls == TLS_ON ? 465 : 587;
    if (!c->folder) c->folder = "INBOX";
    return 0;
}

void mail_cfg_free(struct mail_cfg *c)
{
    if (c->text) {
        memset(c->text, 0, c->textn);              /* the passwords gone from memory (the parser cut it with NULs) */
        free(c->text);
    }
    c->text = 0;
}

/* ---- addresses ---- */
static int addr_ok(const char *a)
{
    const char *at = strrchr(a, '@');
    u32 n = (u32)strlen(a);
    if (!at || at == a || !at[1] || n > 254) return 0;
    for (const char *p = a; *p; p++)
        if ((u8)*p <= 32 || (u8)*p >= 127 || strchr("<>()[],;:\"\\", *p)) return 0;
    return 1;
}

static int addr_x(const char *s, struct maddr *a, int max, char *buf, int bufn, char *err, int errn, int skip)
{
    int na = 0, o = 0;
    while (*s) {
        /* one address: to a ',' outside quotes, <> and () */
        const char *b = s;
        int q = 0, ang = 0, par = 0;
        for (; *s; s++) {
            if (*s == '\\' && q && s[1]) { s++; continue; }
            if (*s == '"') q = !q;
            else if (q) continue;
            else if (*s == '<') ang = 1;
            else if (*s == '>') ang = 0;
            else if (*s == '(') par++;
            else if (*s == ')' && par) par--;
            else if (*s == ',' && !ang && !par) break;
        }
        const char *e = s;
        if (*s) s++;
        while (b < e && ws((u8)*b)) b++;
        while (e > b && ws((u8)e[-1])) e--;
        if (b == e) continue;
        if (e - b >= 2 && e[-1] == ';' && e[-2] == ':') continue;       /* "undisclosed-recipients:;" */
        if (na >= max) {
            if (skip) return na;
            snprintf(err, (u32)errn, "more than %d addresses", max);
            return -1;
        }
        const char *lt = 0, *gt = 0;
        q = 0;
        for (const char *p = b; p < e; p++) {
            if (*p == '"') q = !q;
            else if (!q && *p == '<') lt = p;
            else if (!q && *p == '>' && lt) gt = p;
        }
        char name[200], addr[260];
        int nn = 0, an = 0;
        if (lt && gt && gt > lt) {
            const char *ab = lt + 1, *ae = gt;
            while (ab < ae && *ab == ' ') ab++;
            while (ae > ab && ae[-1] == ' ') ae--;
            for (const char *p = ab; p < ae && an < 259; p++)
                addr[an++] = *p;
            const char *ne = lt;
            while (ne > b && ws((u8)ne[-1])) ne--;
            for (const char *p = b; p < ne && nn < 199; p++) {
                if (*p == '"') continue;
                if (*p == '\\' && p + 1 < ne) p++;
                name[nn++] = *p;
            }
        } else {
            par = 0;
            for (const char *p = b; p < e && an < 259; p++) {     /* a bare address, (comments) dropped */
                if (*p == '(') par++;
                else if (*p == ')' && par) par--;
                else if (!par) addr[an++] = *p;
            }
            while (an && ws((u8)addr[an - 1])) an--;
        }
        addr[an] = 0;
        name[nn] = 0;
        if (!addr_ok(addr)) {
            if (skip) continue;
            char cl[80];
            mail_clean(b, (u32)(e - b), cl, sizeof cl);
            snprintf(err, (u32)errn, "not an address: %s", cl);
            return -1;
        }
        char cn[200];
        nn = mail_clean(name, (u32)nn, cn, sizeof cn);
        if (o + nn + an + 2 > bufn) {
            if (skip) return na;
            snprintf(err, (u32)errn, "too many addresses");
            return -1;
        }
        a[na].name = buf + o;
        memcpy(buf + o, cn, (u32)nn + 1);
        o += nn + 1;
        a[na].addr = buf + o;
        memcpy(buf + o, addr, (u32)an + 1);
        o += an + 1;
        na++;
    }
    return na;
}

int addr_parse(const char *s, struct maddr *a, int max, char *buf, int bufn, char *err, int errn)
{
    return addr_x(s, a, max, buf, bufn, err, errn, 0);
}
int addr_parse_some(const char *s, struct maddr *a, int max, char *buf, int bufn)
{
    char e[8];
    return addr_x(s, a, max, buf, bufn, e, sizeof e, 1);
}

static int ascii(const char *s)
{
    for (; *s; s++)
        if ((u8)*s >= 127 || (u8)*s < 32) return 0;
    return 1;
}

/* text outside ASCII as RFC 2047 B words (whole characters, at most 45 bytes a word), folded */
static void words(struct sb *b, const char *s)
{
    u32 n = (u32)strlen(s), i = 0;
    while (i < n) {
        u32 k = n - i < 45 ? n - i : 45;
        while (k < n - i && k && ((u8)s[i + k] & 0xC0) == 0x80) k--;
        char e[64];
        b64_encode((const u8 *)s + i, (int)k, e);
        sb_printf(b, "%s=?UTF-8?B?%s?=", i ? "\r\n " : "", e);
        i += k;
    }
}

static int special(const char *s)
{
    for (; *s; s++)
        if (strchr("()<>@,;:\\\".[]", *s)) return 1;
    return 0;
}

/* an address as written: name <addr>, the name quoted or as words when it needs to be */
static void put_addr(struct sb *b, const struct maddr *a, int enc)
{
    if (!a->name[0]) {
        sb_puts(b, a->addr);
        return;
    }
    if (enc && !ascii(a->name))
        words(b, a->name);
    else if (special(a->name)) {
        sb_putc(b, '"');
        for (const char *p = a->name; *p; p++) {
            if (*p == '"' || *p == '\\') sb_putc(b, '\\');
            sb_putc(b, *p);
        }
        sb_putc(b, '"');
    } else
        sb_puts(b, a->name);
    sb_printf(b, " <%s>", a->addr);
}

static void put_list(struct sb *b, const char *h, const struct maddr *a, int n)
{
    u32 col = (u32)strlen(h) + 2;
    sb_printf(b, "%s: ", h);
    for (int i = 0; i < n; i++) {
        struct sb t;
        sb_init(&t, 1024);
        put_addr(&t, &a[i], 1);
        if (i) {
            if (col + 2 + t.n > 76) {   /* folded before this one */
                sb_puts(b, ",\r\n ");
                col = 1;
            } else {
                sb_puts(b, ", ");
                col += 2;
            }
        }
        if (t.p) sb_put(b, t.p, t.n);
        col += t.n;
        if (t.nomem) b->nomem = 1;
        sb_free(&t);
    }
    sb_puts(b, "\r\n");
}

/* a header's value from the edited text (folded lines joined), clean: into out */

char *mail_build(const char *edited, u32 now, u32 rnd, u32 *n, char **rcpt, int *nrcpt, char *rbuf, int rbufn,
                 char *from, int fromn, char *err, int errn)
{
    static const char *const known[] = { "From", "To", "Cc", "Bcc", "Subject", "Reply-To", "In-Reply-To", "References" };
    char *val[8] = { 0 };
    struct sb other, out;
    sb_init(&other, 4096);
    sb_init(&out, MAIL_TEXTMAX + 65536);
    char *ret = 0, *ab = 0;
    const char *p = edited;
    *nrcpt = 0;
    int ln = 0;
    /* the headers */
    while (*p && *p != '\n' && !(*p == '\r' && p[1] == '\n')) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        ln++;
        const char *c = p;
        while (c < e && *c != ':' && !ws((u8)*c)) c++;
        if (c == p || c >= e || *c != ':') {
            char cl[60];
            mail_clean(p, (u32)(e - p), cl, sizeof cl);
            snprintf(err, (u32)errn, "line %d: not a header (\"Name: value\"; a blank line before the text): %s", ln, cl);
            goto out;
        }
        /* its continuation lines */
        const char *ve = e;
        while (*ve == '\n' && (ve[1] == ' ' || ve[1] == '\t')) {
            ve++;
            while (*ve && *ve != '\n') ve++;
            ln++;
        }
        u32 hn = (u32)(c - p);
        char *v = malloc((u32)(ve - c) * 2 + 8);
        if (!v) { snprintf(err, (u32)errn, "out of memory"); goto out; }
        int vn = mail_clean(c + 1, (u32)(ve - c - 1), v, (int)(ve - c) * 2 + 8);
        int s = 0;
        while (s < vn && v[s] == ' ') s++;
        while (vn > s && v[vn - 1] == ' ') vn--;
        memmove(v, v + s, (u32)(vn - s));
        v[vn - s] = 0;
        int k = -1;
        for (int i = 0; i < 8; i++)
            if (strlen(known[i]) == hn && !ncase(p, known[i], hn)) k = i;
        if (k >= 0) {
            free(val[k]);
            val[k] = v;
        } else {
            static const char *const own[] = { "Date", "Message-ID", "MIME-Version", "Content-Type",
                                               "Content-Transfer-Encoding" };
            int ok = 1;
            for (u32 q = 0; q < hn; q++)
                if ((u8)p[q] <= 32 || (u8)p[q] >= 127) ok = 0;
            for (int q = 0; q < 5; q++)
                if (strlen(own[q]) == hn && !ncase(p, own[q], hn)) ok = 0;
            if (ok && v[0] && ascii(v)) {
                sb_put(&other, p, hn);
                sb_printf(&other, ": %s\r\n", v);
            }
            free(v);
        }
        p = *ve ? ve + 1 : ve;
    }
    if (*p == '\r') p++;
    if (*p == '\n') p++;
    /* the addresses */
    struct maddr fa[1], ra[MAIL_RCPTMAX], rp[4];
    char fb[300], rpb[600];
    if (!(ab = malloc(4096))) { snprintf(err, (u32)errn, "out of memory"); goto out; }
    int nf = val[0] ? addr_parse(val[0], fa, 1, fb, sizeof fb, err, errn) : 0;
    if (nf < 0) goto out;
    if (nf != 1) { snprintf(err, (u32)errn, "From: one address (the \"from\" setting fills it in)"); goto out; }
    int nr = 0, cnt[3];
    for (int h = 1; h <= 3; h++) {
        int k = val[h] && *val[h] ? addr_parse(val[h], ra + nr, MAIL_RCPTMAX - nr, ab + (h - 1) * 1360, 1360, err, errn) : 0;
        if (k < 0) goto out;
        cnt[h - 1] = k;
        nr += k;
    }
    if (!nr) { snprintf(err, (u32)errn, "no recipient: To, Cc or Bcc"); goto out; }
    int nrp = val[5] && *val[5] ? addr_parse(val[5], rp, 4, rpb, sizeof rpb, err, errn) : 0;
    if (nrp < 0) goto out;
    /* the envelope */
    int o = 0;
    for (int i = 0; i < nr; i++) {
        int l = (int)strlen(ra[i].addr) + 1, dup = 0;
        for (int j = 0; j < i; j++)
            if (!ncase(ra[i].addr, ra[j].addr, (u32)l)) dup = 1;
        if (dup) continue;
        if (o + l > rbufn) { snprintf(err, (u32)errn, "too many recipients"); goto out; }
        memcpy(rbuf + o, ra[i].addr, (u32)l);
        rcpt[(*nrcpt)++] = rbuf + o;
        o += l;
    }
    snprintf(from, (u32)fromn, "%s", fa[0].addr);
    /* the headers written */
    int y, mo, d, h, mi, sec, wd;
    static const char *const wdn[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    mail_civil(now, &y, &mo, &d, &h, &mi, &sec, &wd);
    sb_printf(&out, "Date: %s, %d %s %d %02d:%02d:%02d +0000\r\n", wdn[wd], d, mon[mo - 1], y, h, mi, sec);
    sb_puts(&out, "From: ");
    put_addr(&out, fa, 1);
    sb_puts(&out, "\r\n");
    if (cnt[0]) put_list(&out, "To", ra, cnt[0]);
    if (cnt[1]) put_list(&out, "Cc", ra + cnt[0], cnt[1]);
    if (nrp) put_list(&out, "Reply-To", rp, nrp);
    sb_puts(&out, "Subject: ");
    if (!val[4] || ascii(val[4])) sb_puts(&out, val[4] ? val[4] : "");
    else words(&out, val[4]);
    sb_puts(&out, "\r\n");
    const char *dom = strrchr(fa[0].addr, '@') + 1;
    sb_printf(&out, "Message-ID: <%08x.%08x@%s>\r\n", now, rnd, dom);
    for (int i = 6; i < 8; i++)
        if (val[i] && *val[i] && ascii(val[i])) sb_printf(&out, "%s: %s\r\n", known[i], val[i]);
    if (other.p) sb_put(&out, other.p, other.n);
    /* the body: clean UTF-8, tabs kept; quoted-printable when not ASCII or with a line over 998 bytes */
    struct sb body;
    sb_init(&body, MAIL_TEXTMAX);
    mime_keep(&body, p, (u32)strlen(p));
    while (body.n && ws((u8)body.p[body.n - 1])) body.n--;
    int qp = 0, run = 0;
    for (u32 i = 0; i < body.n; i++) {
        if ((u8)body.p[i] >= 0x80) qp = 1;
        run = body.p[i] == '\n' ? 0 : run + 1;
        if (run > 998) qp = 1;
    }
    int asc = 1;
    for (u32 i = 0; i < body.n; i++) if ((u8)body.p[i] >= 0x80) asc = 0;
    sb_printf(&out, "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=%s\r\nContent-Transfer-Encoding: %s\r\n\r\n",
              asc ? "us-ascii" : "utf-8", qp ? "quoted-printable" : "7bit");
    int col = 0;
    for (u32 i = 0; i < body.n; i++) {
        u8 ch = (u8)body.p[i];
        if (ch == '\n') {
            sb_puts(&out, "\r\n");
            col = 0;
            continue;
        }
        if (!qp) { sb_putc(&out, ch); continue; }
        int last = i + 1 >= body.n || body.p[i + 1] == '\n';
        char t[4];
        int tn;
        if ((ch >= 33 && ch <= 126 && ch != '=') || ((ch == ' ' || ch == '\t') && !last)) { t[0] = (char)ch; tn = 1; }
        else { snprintf(t, sizeof t, "=%02X", ch); tn = 3; }
        if (col + tn > 75) {
            sb_puts(&out, "=\r\n");
            col = 0;
        }
        sb_put(&out, t, (u32)tn);
        col += tn;
    }
    sb_puts(&out, "\r\n");
    int nomem = body.nomem;
    sb_free(&body);
    if (out.nomem || nomem) { snprintf(err, (u32)errn, "out of memory"); goto out; }
    if (out.full) { snprintf(err, (u32)errn, "the message is too big"); goto out; }
    *n = out.n;
    ret = sb_take(&out);
out:
    free(ab);
    for (int i = 0; i < 8; i++) free(val[i]);
    sb_free(&other);
    if (!ret) sb_free(&out);
    return ret;
}

/* ---- the text the editor starts with ---- */
static void addr_line(struct sb *b, const char *h, const struct maddr *a, int n)
{
    sb_printf(b, "%s: ", h);
    for (int i = 0; i < n; i++) {
        if (i) sb_puts(b, ", ");
        put_addr(b, &a[i], 0);
    }
    sb_putc(b, '\n');
}

static void quote(struct sb *b, const char *t)
{
    while (*t) {
        const char *e = strchr(t, '\n');
        u32 n = e ? (u32)(e - t) : (u32)strlen(t);
        sb_puts(b, *t == '>' ? ">" : n ? "> " : ">");
        sb_put(b, t, n);
        sb_putc(b, '\n');
        t += n + (e ? 1 : 0);
    }
}

char *mail_draft(int kind, const struct mmsg *m, const char *from, const char *to)
{
    struct sb b;
    sb_init(&b, MAIL_TEXTMAX + 65536);
    sb_printf(&b, "From: %s\n", from ? from : "");
    if (kind == DRAFT_NEW || !m) {
        sb_printf(&b, "To: %s\nCc: \nBcc: \nSubject: \n\n", to ? to : "");
        return sb_take(&b);
    }
    char err[8], *ab = malloc(4096), mb[300], pb[600];
    struct maddr me[1], who[2], all[MAIL_RCPTMAX + 1];
    if (!ab) { sb_free(&b); return 0; }
    int nme = from ? addr_parse(from, me, 1, mb, sizeof mb, err, sizeof err) : 0;
    if (kind == DRAFT_FWD) {
        sb_puts(&b, "To: \nCc: \nBcc: \n");
    } else {
        const char *rt = m->replyto && *m->replyto ? m->replyto : m->from;
        int nw = addr_parse_some(rt, who, 2, pb, sizeof pb);
        addr_line(&b, "To", who, nw);
        int na = 0;
        if (kind == DRAFT_ALL) {        /* everyone else: To and Cc, without you and without whoever is in To */
            na = addr_parse_some(m->to, all, MAIL_RCPTMAX, ab, 2048);
            na += addr_parse_some(m->cc, all + na, MAIL_RCPTMAX - na, ab + 2048, 2048);
            int o = 0;
            for (int i = 0; i < na; i++) {
                int drop = nme == 1 && !ncase(all[i].addr, me[0].addr, (u32)strlen(me[0].addr) + 1);
                for (int j = 0; j < nw; j++)
                    if (!ncase(all[i].addr, who[j].addr, (u32)strlen(who[j].addr) + 1)) drop = 1;
                for (int j = 0; j < o; j++)
                    if (!ncase(all[i].addr, all[j].addr, (u32)strlen(all[j].addr) + 1)) drop = 1;
                if (!drop) all[o++] = all[i];
            }
            na = o;
        }
        addr_line(&b, "Cc", all, na);
        sb_puts(&b, "Bcc: \n");
    }
    const char *sj = m->subject ? m->subject : "";
    const char *pre = kind == DRAFT_FWD ? "Fwd: " : "Re: ";
    u32 pl = (u32)strlen(pre) - 2;      /* "Re" or "Fwd", then ':' */
    if (!ncase(sj, pre, pl) && sj[pl] == ':') pre = "";
    sb_printf(&b, "Subject: %s", pre);
    sb_puts(&b, sj);
    sb_putc(&b, '\n');
    if (kind != DRAFT_FWD && m->msgid && *m->msgid && ascii(m->msgid)) {
        sb_printf(&b, "In-Reply-To: %s\n", m->msgid);
        sb_puts(&b, "References: ");
        if (m->refs && *m->refs && ascii(m->refs) && strlen(m->refs) < 900) {
            sb_puts(&b, m->refs);
            sb_putc(&b, ' ');
        }
        sb_puts(&b, m->msgid);
        sb_putc(&b, '\n');
    }
    sb_putc(&b, '\n');
    if (kind == DRAFT_FWD) {
        sb_puts(&b, "\n\n-------- Forwarded message --------\n");
        const char *hn[5] = { "From", "Date", "Subject", "To", "Cc" };
        const char *hv[5] = { m->from, m->date, m->subject, m->to, m->cc };
        for (int i = 0; i < 5; i++)
            if (hv[i] && *hv[i]) {
                sb_printf(&b, "%s: ", hn[i]);
                sb_puts(&b, hv[i]);
                sb_putc(&b, '\n');
            }
        sb_putc(&b, '\n');
        sb_puts(&b, m->text ? m->text : "");
        sb_putc(&b, '\n');
    } else {
        sb_puts(&b, "On ");
        sb_puts(&b, m->date && *m->date ? m->date : "a day");
        sb_puts(&b, ", ");
        sb_puts(&b, m->from ? m->from : "");
        sb_puts(&b, " wrote:\n");
        quote(&b, m->text ? m->text : "");
        sb_putc(&b, '\n');
    }
    free(ab);
    return sb_take(&b);
}
