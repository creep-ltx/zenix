/* SPDX-License-Identifier: GPL-2.0-only */
/* mail's protocols (DESIGN D59 section 4): lines and literals over the program's connection, IMAP4rev1 (RFC 3501:
 * LOGIN, LIST, SELECT, FETCH, UID STORE/COPY, EXPUNGE, APPEND), POP3 (RFC 1939: USER/PASS, STAT, TOP, RETR, DELE,
 * RSET; STLS, RFC 2595) and SMTP (RFC 5321: EHLO, STARTTLS RFC 3207, AUTH PLAIN/LOGIN RFC 4954, MAIL, RCPT, DATA).
 * Written from the RFCs; no client's code. Every server line is untrusted: a line over MAIL_LINEMAX is cut (its end
 * still read), a literal over MAIL_LITMAX or a broken one ends the connection, counts and numbers are checked. */
#include "mail.h"

#define ISDIG(c) ((c) >= '0' && (c) <= '9')

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ncase(const char *a, const char *b, u32 n)      /* 0: the same n bytes, any case */
{
    for (; n; n--, a++, b++)
        if (lc((unsigned char)*a) != lc((unsigned char)*b))
            return 1;
    return 0;
}

/* ---- the connection's lines ---- */
static int fill(struct mconn *c)
{
    if (c->dead)
        return ME_NET;
    int k = c->rd(c->u, c->b, sizeof c->b);
    if (k <= 0) {
        c->dead = 1;
        return ME_NET;
    }
    c->bp = 0;
    c->bn = k;
    return 0;
}

static char tail[24];                   /* the last bytes of the last line read, cut or not (a literal's {N}) */
static int tailn;

int mc_line(struct mconn *c, char *out, int cap)
{
    int n = 0;
    c->cut = 0;
    tailn = 0;
    for (;;) {
        if (c->bp >= c->bn && fill(c) < 0)
            return ME_NET;
        char ch = c->b[c->bp++];
        if (ch == '\n')
            break;
        if (n < cap - 1)
            out[n++] = ch;
        else
            c->cut = 1;
        if (tailn == (int)sizeof tail)
            memmove(tail, tail + 1, sizeof tail - 1), tailn--;
        tail[tailn++] = ch;
    }
    if (n && out[n - 1] == '\r')
        n--;
    if (tailn && tail[tailn - 1] == '\r')
        tailn--;
    out[n] = 0;
    if (c->trace)
        c->trace(c->u, 0, out, n);
    return n;
}

int mc_read(struct mconn *c, char *out, u32 n)
{
    while (n) {
        if (c->bp >= c->bn && fill(c) < 0)
            return ME_NET;
        u32 k = (u32)(c->bn - c->bp);
        if (k > n) k = n;
        if (out) {
            memcpy(out, c->b + c->bp, k);
            out += k;
        }
        c->bp += (int)k;
        n -= k;
    }
    return 0;
}

static int mc_raw(struct mconn *c, const char *b, u32 n)
{
    if (c->dead)
        return ME_NET;
    if (c->wr(c->u, b, (int)n) < 0) {
        c->dead = 1;
        return ME_NET;
    }
    return 0;
}

int mc_send(struct mconn *c, const char *fmt, ...)
{
    static char b[1100];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b - 2, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof b - 2)
        return ME_PROTO;
    /* a login's line is never traced, and is wiped once sent */
    int secret = !ncase(b, "PASS ", 5) || strstr(b, " LOGIN ") || !ncase(b, "AUTH ", 5);
    if (c->trace)
        c->trace(c->u, 1, secret ? "(the login)" : b, secret ? 11 : n);
    b[n++] = '\r';
    b[n++] = '\n';
    int r = mc_raw(c, b, (u32)n);
    if (secret) memset(b, 0, sizeof b);
    return r;
}

/* TLS from here: nothing may be waiting from before (RFC 3207 5: bytes after the answer are an attack) */
static int to_tls(struct mconn *c, char *err, int errn)
{
    if (c->bp < c->bn) {
        snprintf(err, (u32)errn, "the server sent more after its STARTTLS answer: refused");
        return ME_PROTO;
    }
    if (!c->starttls || c->starttls(c->u, err, errn) < 0) {
        c->dead = 1;
        return ME_NET;
    }
    return 0;
}

/* ---- IMAP: a response read whole, its literals beside it ---- */
#define NLIT 8
struct iresp {
    struct sb t;                        /* the text; a literal is "\001" and its number */
    char *lit[NLIT];
    u32 litn[NLIT];
    int nlit;
};
static char line[MAIL_LINEMAX + 1];

static void ir_free(struct iresp *r)
{
    sb_free(&r->t);
    for (int i = 0; i < r->nlit; i++)
        free(r->lit[i]);
    r->nlit = 0;
}

/* a response: lines joined across literals; keep: the bytes of a literal kept (the rest read and dropped) */
static int ir_read(struct msess *s, struct iresp *r, u32 keep)
{
    struct mconn *c = s->c;
    memset(r, 0, sizeof *r);
    sb_init(&r->t, 65536);
    for (;;) {
        int n = mc_line(c, line, sizeof line);
        if (n < 0) {
            snprintf(s->err, sizeof s->err, "the connection to the server was lost");
            return ME_NET;
        }
        for (int i = 0; i < n; i++)
            if ((unsigned char)line[i] < 32 || line[i] == 127)
                line[i] = ' ';
        /* a literal: {N} or {N+} at the line's very end (read from the line's tail: also when it was cut) */
        u32 ln = 0;
        int lit = 0;
        if (tailn >= 3 && tail[tailn - 1] == '}') {
            int j = tailn - 2;
            if (tail[j] == '+') j--;
            int d = 0;
            while (j >= 0 && ISDIG(tail[j]) && d < 20) j--, d++;
            if (j >= 0 && tail[j] == '{' && d) {
                lit = 1;
                for (int k = j + 1; k < j + 1 + d; k++)
                    ln = d >= 10 ? MAIL_LITMAX + 1 : ln * 10 + (u32)(tail[k] - '0');
                if (!c->cut)            /* the {N} itself not in the text */
                    n -= tailn - j;
            }
        }
        if (n < 0) n = 0;
        sb_put(&r->t, line, (u32)n);
        if (!lit)
            break;
        if (ln > MAIL_LITMAX || r->nlit == NLIT) {
            snprintf(s->err, sizeof s->err, "the server sent a literal of %u bytes: the connection is ended", ln);
            c->dead = 1;
            return ME_PROTO;
        }
        u32 k = ln < keep ? ln : keep;
        char *p = malloc(k + 1);
        if (!p) {
            mc_read(c, 0, ln);
            snprintf(s->err, sizeof s->err, "out of memory");
            return ME_MEM;
        }
        if (mc_read(c, p, k) < 0 || mc_read(c, 0, ln - k) < 0) {
            free(p);
            snprintf(s->err, sizeof s->err, "the connection to the server was lost");
            return ME_NET;
        }
        p[k] = 0;
        r->lit[r->nlit] = p;
        r->litn[r->nlit] = k;
        sb_putc(&r->t, 1);
        sb_putc(&r->t, '0' + r->nlit++);
    }
    if (r->t.nomem) {
        snprintf(s->err, sizeof s->err, "out of memory");
        return ME_MEM;
    }
    if (!r->t.p) sb_put(&r->t, "", 0);
    if (r->t.nomem || !r->t.p) {
        snprintf(s->err, sizeof s->err, "out of memory");
        return ME_MEM;
    }
    return 0;
}

/* ---- tokens of a response ---- */
enum { T_END, T_ATOM, T_STR, T_OPEN, T_CLOSE, T_NIL };
struct tk { int type; char *s; u32 n; };
struct pz { char *p; struct iresp *r; };

static int next(struct pz *z, struct tk *t)
{
    char *p = z->p;
    while (*p == ' ') p++;
    t->s = p;
    t->n = 0;
    if (!*p) { z->p = p; return t->type = T_END; }
    if (*p == '(') { z->p = p + 1; return t->type = T_OPEN; }
    if (*p == ')') { z->p = p + 1; return t->type = T_CLOSE; }
    if (*p == 1) {                      /* a literal */
        int i = p[1] - '0';
        z->p = p + (p[1] ? 2 : 1);
        if (i < 0 || i >= z->r->nlit) return t->type = T_NIL;
        t->s = z->r->lit[i];
        t->n = z->r->litn[i];
        return t->type = T_STR;
    }
    if (*p == '"') {                    /* unescaped in place */
        char *o = ++p, *b = p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            *o++ = *p++;
        }
        if (*p) p++;
        t->s = b;
        t->n = (u32)(o - b);
        z->p = p;
        return t->type = T_STR;
    }
    int br = 0;                         /* an atom; BODY[HEADER.FIELDS (A B)]<0> is one */
    while (*p && *p != 1 && (br || (*p != ' ' && *p != '(' && *p != ')'))) {
        if (*p == '[') br++;
        else if (*p == ']' && br) br--;
        p++;
    }
    t->n = (u32)(p - t->s);
    z->p = p;
    if (t->n == 3 && !ncase(t->s, "NIL", 3)) return t->type = T_NIL;
    return t->type = T_ATOM;
}

static void skipval(struct pz *z, struct tk *t)   /* t: the value's first token */
{
    int d = t->type == T_OPEN;
    while (d > 0 && next(z, t) != T_END) {
        if (t->type == T_OPEN) d++;
        else if (t->type == T_CLOSE) d--;
    }
}

static u32 num(const struct tk *t)
{
    u32 v = 0;
    for (u32 i = 0; i < t->n && ISDIG(t->s[i]) && v < 400000000u; i++)
        v = v * 10 + (u32)(t->s[i] - '0');
    return v;
}

/* ---- an IMAP command and its responses ---- */
typedef int (*each_fn)(struct msess *s, struct iresp *r, struct pz *z, u32 n, void *arg);   /* "* N WORD": z after WORD */

static int untagged(struct msess *s, struct iresp *r, each_fn each, void *arg)
{
    struct pz z = { r->t.p + 1, r };
    struct tk a, b;
    if (next(&z, &a) != T_ATOM) return 0;
    if (a.n == 3 && !ncase(a.s, "BYE", 3)) {
        s->c->dead = 1;                 /* the server leaves after this */
        return 0;
    }
    if (ISDIG(a.s[0])) {
        char *save = z.p;
        if (next(&z, &b) == T_ATOM && b.n == 6 && !ncase(b.s, "EXISTS", 6))
            s->exists = (int)num(&a);
        z.p = save;
        if (each) return each(s, r, &z, num(&a), arg);
        return 0;
    }
    if (each) {
        z.p = a.s;
        return each(s, r, &z, 0, arg);
    }
    return 0;
}

/* the tagged answer: 0 OK, ME_NO with the server's words, ME_NET, ME_PROTO */
static int tagged(struct msess *s, const char *tag, each_fn each, void *arg, u32 keep)
{
    struct iresp r;
    int e;
    for (;;) {
        if ((e = ir_read(s, &r, keep)) < 0) {
            ir_free(&r);
            s->c->dead = 1;
            return e;
        }
        char *t = r.t.p;
        u32 tl = (u32)strlen(tag);
        if (t[0] == '*' && t[1] == ' ') {
            e = untagged(s, &r, each, arg);
            ir_free(&r);
            if (e < 0) {
                s->c->dead = 1;         /* the rest of the answer unread: the connection is out of step */
                if (e == ME_MEM) snprintf(s->err, sizeof s->err, "out of memory");
                return e;
            }
            if (s->c->dead) {
                snprintf(s->err, sizeof s->err, "the server ended the connection");
                return ME_NET;
            }
            continue;
        }
        if (!strncmp(t, tag, tl) && t[tl] == ' ') {
            char *w = t + tl + 1;
            if (!ncase(w, "OK", 2)) {
                ir_free(&r);
                return 0;
            }
            w += !ncase(w, "NO ", 3) ? 3 : !ncase(w, "BAD ", 4) ? 4 : 0;
            mail_clean(w, (u32)strlen(w), s->err, 180);      /* the server's words: clean before shown */
            ir_free(&r);
            return ME_NO;
        }
        ir_free(&r);                    /* "+" or another tag: not ours, dropped */
    }
}

static char tagb[16];
static const char *newtag(struct msess *s)
{
    snprintf(tagb, sizeof tagb, "A%03d", ++s->tag);
    return tagb;
}

static int icmd(struct msess *s, each_fn each, void *arg, u32 keep, const char *fmt, ...)
{
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof b) {
        snprintf(s->err, sizeof s->err, "a command too long");
        return ME_PROTO;
    }
    const char *tag = newtag(s);
    if (mc_send(s->c, "%s %s", tag, b) < 0) {
        snprintf(s->err, sizeof s->err, "the connection to the server was lost");
        return ME_NET;
    }
    return tagged(s, tag, each, arg, keep);
}

/* a literal's bytes after its "{N}" line was sent: the server's "+" first */
static int send_lit(struct msess *s, const char *tag, const char *b, u32 n)
{
    for (;;) {
        int k = mc_line(s->c, line, sizeof line);
        if (k < 0) {
            snprintf(s->err, sizeof s->err, "the connection to the server was lost");
            return ME_NET;
        }
        if (line[0] == '+')
            break;
        if (!strncmp(line, tag, strlen(tag))) {
            mail_clean(line + strlen(tag), (u32)strlen(line + strlen(tag)), s->err, 180);
            return ME_NO;
        }
    }
    return mc_raw(s->c, b, n) < 0 ? ME_NET : 0;
}

const char *imap_quote(char *out, int cap, const char *s)
{
    int o = 0;
    out[o++] = '"';
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c >= 127 || o >= cap - 4)
            return 0;
        if (c == '"' || c == '\\') out[o++] = '\\';
        out[o++] = (char)c;
    }
    out[o++] = '"';
    out[o] = 0;
    return out;
}

/* ---- POP3 ---- */
static int pop_ok(struct msess *s)
{
    int n = mc_line(s->c, line, sizeof line);
    if (n < 0) {
        snprintf(s->err, sizeof s->err, "the connection to the server was lost");
        return ME_NET;
    }
    if (!strncmp(line, "+OK", 3))
        return 0;
    char cl[190];
    mail_clean(line, (u32)n, cl, sizeof cl);
    snprintf(s->err, sizeof s->err, "%s", !strncmp(cl, "-ERR", 4) ? (cl[4] ? cl + 5 : "refused") : cl);
    return !strncmp(line, "-ERR", 4) ? ME_NO : ME_PROTO;
}

static int pop_cmd(struct msess *s, const char *fmt, ...)
{
    char b[600];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof b) {
        snprintf(s->err, sizeof s->err, "a command too long");
        return ME_PROTO;
    }
    if (mc_send(s->c, "%s", b) < 0) {
        snprintf(s->err, sizeof s->err, "the connection to the server was lost");
        return ME_NET;
    }
    return pop_ok(s);
}

/* a multi-line answer's lines (dot-unstuffed) into b: CR LF between them; at most max bytes kept, *cut when more */
static int pop_multi(struct msess *s, struct sb *b, int *cut)
{
    for (;;) {
        int n = mc_line(s->c, line, sizeof line);
        if (n < 0) {
            snprintf(s->err, sizeof s->err, "the connection to the server was lost");
            return ME_NET;
        }
        if (n == 1 && line[0] == '.' && !s->c->cut)
            return 0;
        const char *p = line[0] == '.' ? line + 1 : line;
        sb_put(b, p, (u32)(n - (p - line)));
        sb_put(b, "\r\n", 2);
        if ((b->full || s->c->cut) && cut)
            *cut = 1;
    }
}

/* ---- the session ---- */
int ms_login(struct msess *s, int tls, const char *user, const char *pass, int plain_ok)
{
    struct iresp r;
    int e;
    s->err[0] = 0;
    s->tag = 0;
    if (s->pop) {
        if ((e = pop_ok(s)) < 0) return e;
        if (tls == TLS_STARTTLS) {
            if ((e = pop_cmd(s, "STLS")) < 0) {
                if (e == ME_NO) snprintf(s->err, sizeof s->err, "the server will not start TLS (STLS)");
                return e;
            }
            if ((e = to_tls(s->c, s->err, sizeof s->err)) < 0) return e;
        }
        if (tls == TLS_OFF && !plain_ok) {
            snprintf(s->err, sizeof s->err, "the password is not sent without TLS (\"tls on\", or \"plain_auth on\")");
            return ME_NO;
        }
        if ((e = pop_cmd(s, "USER %s", user)) < 0) return e;
        return pop_cmd(s, "PASS %s", pass);
    }
    if ((e = ir_read(s, &r, 1024)) < 0) {
        ir_free(&r);
        return e;
    }
    int pre = !strncmp(r.t.p, "* PREAUTH", 9), ok = pre || !strncmp(r.t.p, "* OK", 4);
    if (!ok) {
        char cl[190];
        mail_clean(r.t.p, r.t.n, cl, sizeof cl);
        snprintf(s->err, sizeof s->err, "the server said: %.170s", cl);
        ir_free(&r);
        return ME_PROTO;
    }
    ir_free(&r);
    if (tls == TLS_STARTTLS) {
        if ((e = icmd(s, 0, 0, 1024, "STARTTLS")) < 0) {
            if (e == ME_NO) snprintf(s->err, sizeof s->err, "the server will not start TLS (STARTTLS)");
            return e;
        }
        if ((e = to_tls(s->c, s->err, sizeof s->err)) < 0) return e;
    }
    if (pre)
        return 0;
    if (tls == TLS_OFF && !plain_ok) {
        snprintf(s->err, sizeof s->err, "the password is not sent without TLS (\"tls on\", or \"plain_auth on\")");
        return ME_NO;
    }
    /* LOGIN user pass: each a quoted string, or a literal when it holds what a string may not */
    char qu[300], qp[300];
    const char *a = imap_quote(qu, sizeof qu, user), *b = imap_quote(qp, sizeof qp, pass);
    if (a && b)
        return icmd(s, 0, 0, 1024, "LOGIN %s %s", a, b);
    const char *tag = newtag(s);
    if (a) {
        if (mc_send(s->c, "%s LOGIN %s {%u}", tag, a, (u32)strlen(pass)) < 0) return ME_NET;
    } else {
        if (mc_send(s->c, "%s LOGIN {%u}", tag, (u32)strlen(user)) < 0) return ME_NET;
        if ((e = send_lit(s, tag, user, (u32)strlen(user))) < 0) return e;
        if (b) {
            if (mc_send(s->c, " %s", b) < 0) return ME_NET;
            return tagged(s, tag, 0, 0, 1024);
        }
        if (mc_send(s->c, " {%u}", (u32)strlen(pass)) < 0) return ME_NET;
    }
    if ((e = send_lit(s, tag, pass, (u32)strlen(pass))) < 0) return e;
    if (mc_raw(s->c, "\r\n", 2) < 0) return ME_NET;
    return tagged(s, tag, 0, 0, 1024);
}

static void box_free(struct msess *s)
{
    for (int i = 0; i < s->nbox; i++)
        free(s->box[i].name);
    free(s->box);
    s->box = 0;
    s->nbox = 0;
}

static int box_add(struct msess *s, const char *name, u32 n)
{
    if (s->nbox >= MAIL_BOXMAX)
        return 0;
    if (!s->box && !(s->box = calloc(MAIL_BOXMAX, sizeof *s->box)))
        return ME_MEM;
    char sh[256], tmp[256];
    char *raw = malloc(n + 1 + sizeof sh);
    if (!raw)
        return ME_MEM;
    memcpy(raw, name, n);
    raw[n] = 0;
    int k = mutf7_decode(raw, tmp, sizeof tmp);
    k = k < 0 ? mail_clean(raw, n, sh, sizeof sh) : mail_clean(tmp, (u32)k, sh, sizeof sh);
    char *shown = raw + n + 1;
    memcpy(shown, sh, (u32)k + 1);
    int at = s->nbox;
    if (n == 5 && !ncase(name, "INBOX", 5)) {        /* INBOX first */
        memmove(s->box + 1, s->box, sizeof *s->box * (u32)s->nbox);
        at = 0;
    }
    s->box[at].name = raw;
    s->box[at].shown = shown;
    s->nbox++;
    return 0;
}

static int each_list(struct msess *s, struct iresp *r, struct pz *z, u32 n, void *arg)
{
    (void)r; (void)n; (void)arg;
    struct tk t;
    if (next(z, &t) != T_ATOM || t.n != 4 || ncase(t.s, "LIST", 4)) return 0;
    if (next(z, &t) != T_OPEN) return 0;
    int nosel = 0;
    while (next(z, &t) == T_ATOM)
        if ((t.n == 9 && !ncase(t.s, "\\Noselect", 9)) || (t.n == 12 && !ncase(t.s, "\\NonExistent", 12)))
            nosel = 1;
    if (t.type != T_CLOSE) return 0;
    next(z, &t);                        /* the delimiter */
    if (next(z, &t) != T_ATOM && t.type != T_STR) return 0;
    if (nosel || !t.n || t.n > 250) return 0;
    for (u32 i = 0; i < t.n; i++)
        if ((unsigned char)t.s[i] < 32) return 0;
    return box_add(s, t.s, t.n);
}

int ms_folders(struct msess *s)
{
    box_free(s);
    if (s->pop)
        return box_add(s, "INBOX", 5);
    return icmd(s, each_list, 0, 1024, "LIST \"\" \"*\"");
}

int ms_findbox(const struct msess *s, const char *shown)
{
    for (int i = 0; i < s->nbox; i++)
        if (!strcmp(s->box[i].shown, shown) || !strcmp(s->box[i].name, shown))
            return i;
    for (int i = 0; i < s->nbox; i++)
        if (!ncase(s->box[i].shown, shown, (u32)strlen(shown) + 1))
            return i;
    return -1;
}

static void items_free(struct msess *s)
{
    for (int i = 0; i < s->nm; i++)
        free(s->m[i].from);
    free(s->m);
    s->m = 0;
    s->nm = 0;
}

/* the index's line of a message from its header fields */
static int item_set(struct mitem *it, const char *h, u32 n)
{
    char *f = mime_header(h, n, "From"), *sj = mime_header(h, n, "Subject"), *d = mime_header(h, n, "Date");
    char *ct = mime_header(h, n, "Content-Type");
    int e = 0;
    if (!f || !sj || !d || !ct) {
        e = ME_MEM;
        goto out;
    }
    /* who: the name, else the address */
    char nb[200], *who = f;
    struct maddr a;
    char er[8];
    if (addr_parse(f, &a, 1, nb, sizeof nb, er, sizeof er) == 1)
        who = a.name && *a.name ? a.name : a.addr;
    u32 wl = (u32)strlen(who), sl = (u32)strlen(sj);
    free(it->from);
    if (!(it->from = malloc(wl + sl + 2))) {
        e = ME_MEM;
        goto out;
    }
    memcpy(it->from, who, wl + 1);
    it->subj = it->from + wl + 1;
    memcpy(it->subj, sj, sl + 1);
    it->date = mime_date(d);
    if (!ncase(ct, "multipart/mixed", 15))
        it->flags |= MF_ATTACH;
out:
    free(f); free(sj); free(d); free(ct);
    return e;
}

struct idx { u32 lo, hi; int got; };

static int each_fetch(struct msess *s, struct iresp *r, struct pz *z, u32 seq, void *arg)
{
    (void)r;
    struct idx *x = arg;
    struct tk t;
    if (!seq || next(z, &t) != T_ATOM || t.n != 5 || ncase(t.s, "FETCH", 5)) return 0;
    if (!x || seq < x->lo || seq > x->hi) return 0;
    struct mitem *it = &s->m[x->hi - seq];
    if (next(z, &t) != T_OPEN) return 0;
    it->seq = seq;
    while (next(z, &t) == T_ATOM) {
        struct tk v;
        if (t.n == 3 && !ncase(t.s, "UID", 3)) {
            if (next(z, &v) == T_ATOM) it->uid = num(&v);
        } else if (t.n == 11 && !ncase(t.s, "RFC822.SIZE", 11)) {
            if (next(z, &v) == T_ATOM) it->size = num(&v);
        } else if (t.n == 5 && !ncase(t.s, "FLAGS", 5)) {
            if (next(z, &v) != T_OPEN) { skipval(z, &v); continue; }
            it->flags &= MF_ATTACH;
            while (next(z, &v) == T_ATOM) {
                if (v.n == 5 && !ncase(v.s, "\\Seen", 5)) it->flags |= MF_SEEN;
                else if (v.n == 8 && !ncase(v.s, "\\Deleted", 8)) it->flags |= MF_DELETED;
                else if (v.n == 9 && !ncase(v.s, "\\Answered", 9)) it->flags |= MF_ANSWERED;
                else if (v.n == 8 && !ncase(v.s, "\\Flagged", 8)) it->flags |= MF_FLAGGED;
            }
        } else if (t.n > 5 && !ncase(t.s, "BODY[", 5)) {
            next(z, &v);
            if (v.type == T_STR) {
                int e = item_set(it, v.s, v.n);
                if (e < 0) return e;
            } else
                skipval(z, &v);
        } else {
            next(z, &v);
            skipval(z, &v);
        }
    }
    if (it->uid && !it->from) {          /* no header came: shown as nothing rather than not at all */
        int e = item_set(it, "", 0);
        if (e < 0) return e;
    }
    x->got++;
    if (s->progress && x->got % 20 == 0)
        s->progress(s->u, x->got, (int)(x->hi - x->lo + 1));
    return 0;
}

int ms_select(struct msess *s, const char *name, int limit)
{
    int e;
    items_free(s);
    s->exists = 0;
    if (limit < 1) limit = 1;
    if (s->pop) {
        if ((e = pop_cmd(s, "STAT")) < 0) return e;
        s->exists = atoi(line + 4);
        if (s->exists < 0 || s->exists > 1000000) s->exists = 0;
        snprintf(s->cur, sizeof s->cur, "INBOX");
        u32 hi = (u32)s->exists, lo = hi > (u32)limit ? hi - (u32)limit + 1 : 1;
        if (!hi) return 0;
        if (!(s->m = calloc(hi - lo + 1, sizeof *s->m))) return ME_MEM;
        for (int q = (int)hi; q >= (int)lo; q--) {
            struct sb b;
            sb_init(&b, MAIL_HDRMAX);
            if ((e = pop_cmd(s, "TOP %u 0", (u32)q)) < 0 || (e = pop_multi(s, &b, 0)) < 0) {
                sb_free(&b);
                if (e == ME_NO) continue;           /* gone meanwhile */
                return e;
            }
            struct mitem *it = &s->m[s->nm];
            it->uid = it->seq = (u32)q;
            it->flags = MF_SEEN;
            e = item_set(it, b.p ? b.p : "", b.n);
            sb_free(&b);
            if (e < 0) return e;
            s->nm++;
            if (s->progress && s->nm % 20 == 0) s->progress(s->u, s->nm, (int)(hi - lo + 1));
        }
        return 0;
    }
    char q[300];
    if (!imap_quote(q, sizeof q, name)) {
        snprintf(s->err, sizeof s->err, "a folder's name the server cannot be sent");
        return ME_PROTO;
    }
    if ((e = icmd(s, 0, 0, 1024, "SELECT %s", q)) < 0) return e;
    snprintf(s->cur, sizeof s->cur, "%s", name);
    if (s->exists <= 0) return 0;
    struct idx x;
    x.hi = (u32)s->exists;
    x.lo = x.hi > (u32)limit ? x.hi - (u32)limit + 1 : 1;
    x.got = 0;
    if (!(s->m = calloc(x.hi - x.lo + 1, sizeof *s->m))) return ME_MEM;
    e = icmd(s, each_fetch, &x, MAIL_HDRMAX, "FETCH %u:%u (UID FLAGS RFC822.SIZE BODY.PEEK[HEADER.FIELDS (DATE FROM SUBJECT CONTENT-TYPE)])",
             x.lo, x.hi);
    /* the slots that came, in order */
    int k = 0;
    for (u32 i = 0; i <= x.hi - x.lo; i++) {
        if (s->m[i].uid && s->m[i].from) s->m[k++] = s->m[i];
        else free(s->m[i].from);
    }
    s->nm = k;
    return e;
}

struct fet { char *raw; u32 n; };
static int each_body(struct msess *s, struct iresp *r, struct pz *z, u32 seq, void *arg)
{
    (void)s; (void)r;
    struct fet *f = arg;
    struct tk t;
    if (!seq || next(z, &t) != T_ATOM || t.n != 5 || ncase(t.s, "FETCH", 5)) return 0;
    if (next(z, &t) != T_OPEN) return 0;
    while (next(z, &t) == T_ATOM) {
        struct tk v;
        next(z, &v);
        if (t.n >= 6 && !ncase(t.s, "BODY[]", 6) && v.type == T_STR && !f->raw) {
            /* the literal is taken over (r frees what it still holds) */
            for (int i = 0; i < r->nlit; i++)
                if (r->lit[i] == v.s) {
                    f->raw = v.s;
                    f->n = v.n;
                    r->lit[i] = 0;
                }
            if (!f->raw && (f->raw = malloc(v.n + 1))) {
                memcpy(f->raw, v.s, v.n);
                f->raw[v.n] = 0;
                f->n = v.n;
            }
        } else
            skipval(z, &v);
    }
    return 0;
}

int ms_fetch(struct msess *s, int i, char **raw, u32 *n, int *cut)
{
    int e;
    *raw = 0;
    *n = 0;
    *cut = 0;
    if (i < 0 || i >= s->nm) return ME_PROTO;
    struct mitem *it = &s->m[i];
    if (s->pop) {
        struct sb b;
        sb_init(&b, MAIL_MSGMAX);
        if ((e = pop_cmd(s, "RETR %u", it->seq)) < 0 || (e = pop_multi(s, &b, cut)) < 0) {
            sb_free(&b);
            return e;
        }
        *n = b.n;
        if (!(*raw = sb_take(&b))) return ME_MEM;
        return 0;
    }
    struct fet f = { 0, 0 };
    if (it->size > MAIL_MSGMAX) {
        *cut = 1;
        e = icmd(s, each_body, &f, MAIL_MSGMAX, "UID FETCH %u (BODY.PEEK[]<0.%u>)", it->uid, MAIL_MSGMAX);
    } else
        e = icmd(s, each_body, &f, MAIL_MSGMAX, "UID FETCH %u (BODY.PEEK[])", it->uid);
    if (e < 0) {
        free(f.raw);
        return e;
    }
    if (!f.raw) {
        snprintf(s->err, sizeof s->err, "the server did not send the message (deleted elsewhere?)");
        return ME_NO;
    }
    if (f.n >= MAIL_MSGMAX) *cut = 1;
    *raw = f.raw;
    *n = f.n;
    return 0;
}

int ms_flag(struct msess *s, int i, int flag, int on)
{
    static const char *const nm[] = { "\\Seen", "\\Deleted", "\\Answered" };
    if (i < 0 || i >= s->nm) return ME_PROTO;
    struct mitem *it = &s->m[i];
    int k = flag == MF_SEEN ? 0 : flag == MF_DELETED ? 1 : flag == MF_ANSWERED ? 2 : -1, e;
    if (k < 0) return ME_PROTO;
    if (s->pop) {
        if (flag == MF_DELETED) {
            if (on) {
                if ((e = pop_cmd(s, "DELE %u", it->seq)) < 0) return e;
            } else {                    /* POP3 takes back every deletion or none: again for the others */
                if ((e = pop_cmd(s, "RSET")) < 0) return e;
                for (int j = 0; j < s->nm; j++)
                    if (j != i && (s->m[j].flags & MF_DELETED) && (e = pop_cmd(s, "DELE %u", s->m[j].seq)) < 0)
                        return e;
            }
        }
    } else if ((e = icmd(s, 0, 0, 1024, "UID STORE %u %cFLAGS.SILENT (%s)", it->uid, on ? '+' : '-', nm[k])) < 0)
        return e;
    if (on) it->flags |= (unsigned char)flag;
    else it->flags &= (unsigned char)~flag;
    return 0;
}

int ms_copy(struct msess *s, int i, const char *folder)
{
    char q[300];
    if (s->pop) {
        snprintf(s->err, sizeof s->err, "POP3 has one folder: nothing can be moved");
        return ME_NO;
    }
    if (i < 0 || i >= s->nm) return ME_PROTO;
    if (!imap_quote(q, sizeof q, folder)) {
        snprintf(s->err, sizeof s->err, "a folder's name the server cannot be sent");
        return ME_PROTO;
    }
    return icmd(s, 0, 0, 1024, "UID COPY %u %s", s->m[i].uid, q);
}

int ms_expunge(struct msess *s)
{
    if (s->pop) {                       /* the deletions happen as the session ends */
        int e = pop_cmd(s, "QUIT");
        s->c->dead = 1;
        return e;
    }
    return icmd(s, 0, 0, 1024, "EXPUNGE");
}

int ms_append(struct msess *s, const char *folder, const char *msg, u32 n)
{
    char q[300];
    int e;
    if (s->pop) return 0;
    if (!imap_quote(q, sizeof q, folder)) {
        snprintf(s->err, sizeof s->err, "a folder's name the server cannot be sent");
        return ME_PROTO;
    }
    const char *tag = newtag(s);
    if (mc_send(s->c, "%s APPEND %s (\\Seen) {%u}", tag, q, n) < 0) return ME_NET;
    if ((e = send_lit(s, tag, msg, n)) < 0) return e;
    if (mc_raw(s->c, "\r\n", 2) < 0) return ME_NET;
    return tagged(s, tag, 0, 0, 1024);
}

void ms_logout(struct msess *s)
{
    if (s->c->dead) return;
    if (s->pop) pop_cmd(s, "QUIT");
    else icmd(s, 0, 0, 1024, "LOGOUT");
}

void ms_free(struct msess *s)
{
    items_free(s);
    box_free(s);
}

/* ---- modified UTF-7 (RFC 3501 5.1.3): a folder's name as UTF-8 ---- */
int mutf7_decode(const char *in, char *out, int cap)
{
    int o = 0;
    while (*in) {
        if (*in != '&') {
            if ((unsigned char)*in >= 0x80) return -1;
            if (o >= cap - 1) return -1;
            out[o++] = *in++;
            continue;
        }
        in++;
        if (*in == '-') {
            if (o >= cap - 1) return -1;
            out[o++] = '&';
            in++;
            continue;
        }
        u32 bits = 0, nb = 0, hi = 0;
        for (; *in && *in != '-'; in++) {
            int c = *in, v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 :
                    c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == ',' ? 63 : -1;
            if (v < 0) return -1;
            bits = bits << 6 | (u32)v;
            nb += 6;
            if (nb >= 16) {
                nb -= 16;
                u32 u = (bits >> nb) & 0xFFFF;
                if (u >= 0xD800 && u < 0xDC00) { hi = u; continue; }
                if (u >= 0xDC00 && u < 0xE000) {
                    if (!hi) return -1;
                    u = 0x10000 + ((hi - 0xD800) << 10) + (u - 0xDC00);
                    hi = 0;
                }
                if (o >= cap - 5) return -1;
                if (u < 0x80) out[o++] = (char)u;
                else if (u < 0x800) { out[o++] = (char)(0xC0 | u >> 6); out[o++] = (char)(0x80 | (u & 63)); }
                else if (u < 0x10000) { out[o++] = (char)(0xE0 | u >> 12); out[o++] = (char)(0x80 | (u >> 6 & 63));
                                        out[o++] = (char)(0x80 | (u & 63)); }
                else { out[o++] = (char)(0xF0 | u >> 18); out[o++] = (char)(0x80 | (u >> 12 & 63));
                       out[o++] = (char)(0x80 | (u >> 6 & 63)); out[o++] = (char)(0x80 | (u & 63)); }
            }
        }
        if (*in != '-') return -1;
        in++;
    }
    out[o] = 0;
    return o;
}

/* ---- SMTP ---- */
/* a reply: its code (all its lines'), the last line's text in t; < 0 the connection */
static int smtp_reply(struct mconn *c, char *t, int tn, u32 *caps)
{
    for (;;) {
        int n = mc_line(c, line, sizeof line);
        if (n < 0) return ME_NET;
        if (n < 3 || !ISDIG(line[0]) || !ISDIG(line[1]) || !ISDIG(line[2]) || (n > 3 && line[3] != ' ' && line[3] != '-'))
            return ME_PROTO;
        if (caps && n > 4) {            /* EHLO's keywords */
            const char *k = line + 4;
            if (!ncase(k, "STARTTLS", 8)) *caps |= 1;
            if (!ncase(k, "AUTH", 4) && (k[4] == ' ' || k[4] == '=')) {
                *caps |= 2;
                for (const char *p = k + 5; *p; p++)
                    if (!ncase(p, "PLAIN", 5) && (p[5] == ' ' || !p[5])) *caps |= 4;
                    else if (!ncase(p, "LOGIN", 5) && (p[5] == ' ' || !p[5])) *caps |= 8;
            }
        }
        if (n == 3 || line[3] == ' ') {
            mail_clean(n > 4 ? line + 4 : "", n > 4 ? (u32)(n - 4) : 0, t, tn);
            return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
        }
    }
}

static int smtp_do(struct mconn *c, int want, char *err, int errn, const char *what, const char *fmt, ...)
{
    char b[720], t[200];
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b, sizeof b, fmt, ap);
        va_end(ap);
        if (n < 0 || n >= (int)sizeof b) {
            snprintf(err, (u32)errn, "%s: a command too long", what);
            return ME_PROTO;
        }
        int r = mc_send(c, "%s", b);
        memset(b, 0, sizeof b);         /* (an AUTH line) */
        if (r < 0) {
            snprintf(err, (u32)errn, "the connection to the mail server was lost");
            return ME_NET;
        }
    }
    int r = smtp_reply(c, t, sizeof t, 0);
    if (r < 0) {
        snprintf(err, (u32)errn, r == ME_NET ? "the connection to the mail server was lost" : "the mail server's answer is not SMTP");
        return r;
    }
    if (r / 100 != want / 100) {
        snprintf(err, (u32)errn, "%s: the server said %d %s", what, r, t);
        return ME_NO;
    }
    return r;
}

static int ehlo(struct mconn *c, u32 *caps, char *err, int errn)
{
    char t[200];
    *caps = 0;
    if (mc_send(c, "EHLO zenix") < 0) return ME_NET;
    int r = smtp_reply(c, t, sizeof t, caps);
    if (r < 0) {
        snprintf(err, (u32)errn, "the mail server's answer is not SMTP");
        return r;
    }
    if (r / 100 == 2) return 0;
    return smtp_do(c, 250, err, errn, "HELO", "HELO zenix") < 0 ? ME_NO : 0;
}

int smtp_send(struct mconn *c, int tls, const char *user, const char *pass, int plain_ok, const char *from,
              char *const *rcpt, int nrcpt, const char *msg, u32 n, char *err, int errn)
{
    u32 caps;
    int e;
    if ((e = smtp_do(c, 220, err, errn, "the greeting", 0)) < 0) return e;
    if ((e = ehlo(c, &caps, err, errn)) < 0) return e;
    if (tls == TLS_STARTTLS) {
        if (!(caps & 1)) {
            snprintf(err, (u32)errn, "the mail server offers no STARTTLS: not sent");
            return ME_NO;
        }
        if ((e = smtp_do(c, 220, err, errn, "STARTTLS", "STARTTLS")) < 0) return e;
        if ((e = to_tls(c, err, errn)) < 0) return e;
        if ((e = ehlo(c, &caps, err, errn)) < 0) return e;
    }
    if (user && *user) {
        if (tls == TLS_OFF && !plain_ok) {
            snprintf(err, (u32)errn, "the password is not sent without TLS (\"smtp_tls\", or \"plain_auth on\")");
            return ME_NO;
        }
        u32 ul = (u32)strlen(user), pl = (u32)strlen(pass);
        if (ul > 255 || pl > 255) {
            snprintf(err, (u32)errn, "the user name or password is too long");
            return ME_NO;
        }
        char raw[520], b64[700];
        if ((caps & 4) || !(caps & 8)) {
            raw[0] = 0;
            memcpy(raw + 1, user, ul);
            raw[ul + 1] = 0;
            memcpy(raw + ul + 2, pass, pl);
            b64_encode((const unsigned char *)raw, (int)(ul + pl + 2), b64);
            e = smtp_do(c, 235, err, errn, "the login", "AUTH PLAIN %s", b64);
        } else {
            b64_encode((const unsigned char *)user, (int)ul, b64);
            if ((e = smtp_do(c, 334, err, errn, "the login", "AUTH LOGIN")) >= 0 &&
                (e = smtp_do(c, 334, err, errn, "the login", "%s", b64)) >= 0) {
                b64_encode((const unsigned char *)pass, (int)pl, b64);
                if (c->trace) c->trace(c->u, 1, "(the password)", 14);
                void (*tr)(void *, int, const char *, int) = c->trace;
                c->trace = 0;
                e = smtp_do(c, 235, err, errn, "the login", "%s", b64);
                c->trace = tr;
            }
        }
        memset(raw, 0, sizeof raw);
        memset(b64, 0, sizeof b64);
        if (e < 0) return e;
    }
    if ((e = smtp_do(c, 250, err, errn, "the sender", "MAIL FROM:<%s>", from)) < 0) return e;
    for (int i = 0; i < nrcpt; i++)
        if ((e = smtp_do(c, 250, err, errn, rcpt[i], "RCPT TO:<%s>", rcpt[i])) < 0) return e;
    if ((e = smtp_do(c, 354, err, errn, "DATA", "DATA")) < 0) return e;
    /* the message, lines with CR LF, a line starting with a dot doubled (RFC 5321 4.5.2) */
    char ob[1024];
    u32 on = 0, i = 0;
    int bol = 1;
    while (i < n) {
        if (on > sizeof ob - 4) {
            if (mc_raw(c, ob, on) < 0) goto lost;
            on = 0;
        }
        char ch = msg[i++];
        if (bol && ch == '.') ob[on++] = '.';
        bol = 0;
        if (ch == '\r') continue;
        if (ch == '\n') {
            ob[on++] = '\r';
            bol = 1;
        }
        ob[on++] = ch;
    }
    if (!bol) { ob[on++] = '\r'; ob[on++] = '\n'; }
    if (mc_raw(c, ob, on) < 0) goto lost;
    if (c->trace) c->trace(c->u, 1, "(the message)", 13);
    if ((e = smtp_do(c, 250, err, errn, "the message", ".")) < 0) return e;
    smtp_do(c, 221, err, errn, "QUIT", "QUIT");
    return 0;
lost:
    snprintf(err, (u32)errn, "the connection to the mail server was lost");
    return ME_NET;
}
