/* SPDX-License-Identifier: GPL-2.0-only */
/* mail [-c FILE] [-f FOLDER]   mail [-c FILE] -s SUBJECT ADDRESS...   mail -K: an e-mail client for the terminal
 * (DESIGN D59, docs/man/mail.txt). The protocols, the MIME reader and the messages written are mail's core (net/mail/,
 * shared with the PC tests); this file is the screen (a title row, the folders, the index or the message, a row of
 * keys and a row for messages; 80 columns first), the keys (configurable: ~/.config/mail/keys, display/keys.h), the
 * connections (plain, or TLS through /lib/tls/tlspipe, which carries the crypto library so that mail does not; also
 * after STARTTLS), the password (the settings, a command's output, or asked), and the editor. Online only: nothing is
 * kept on the disk but a draft while it is written. */
#include "socket.h"
#include "../net/mail/mail.h"
#include "../display/keys.h"

#define TLSPIPE "/lib/tls/tlspipe"
#define TLS_SECS 240                    /* the helper's first line (a handshake on a 68020 takes up to a minute) */
#define READ_SECS 120                   /* a server's answer */

static struct mail_cfg cf;
static int batch;
/* the screen: libz's one layer (user/zscr.c: the buffered output, the size, SIGWINCH, the reverse lines) */
#define rows zscr_rows
#define cols zscr_cols
#define at(r) zscr_at(r, 1)
#define putw zscr_putw

static const char *mchr(const char *p, int c, u32 n)
{
    for (; n; n--, p++)
        if (*p == c) return p;
    return 0;
}

/* the message row */
static char msg[200];
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (batch) return;                  /* mail -s: said at the end, on standard error */
    at(rows);
    putw(msg, cols - 1);
    oflush();
}

/* ---- a connection: a socket, or the TLS helper's two pipes ---- */
struct conn { int sock, rfd, wfd, pid; const char *host; int port; };
static struct conn IN = { -1, -1, -1, -1, 0, 0 }, OUT = { -1, -1, -1, -1, 0, 0 };

static int c_rd(void *u, char *b, int n)
{
    struct conn *c = u;
    struct pollfd p = { c->rfd, POLLIN, 0 };
    int r = poll(&p, 1, READ_SECS * 1000);
    if (r <= 0) return -ETIMEDOUT;
    int k = c->pid > 0 ? sys_read(c->rfd, b, (u32)n) : recv(c->rfd, b, (u32)n, 0);
    if (k < 0 && c->pid <= 0 && zerrno == EAGAIN) return c_rd(u, b, n);
    return k < 0 ? -1 : k;
}

static int c_wr(void *u, const char *b, int n)
{
    struct conn *c = u;
    while (n > 0) {
        int k = c->pid > 0 ? sys_write(c->wfd, b, (u32)n) : send(c->wfd, b, (u32)n, MSG_NOSIGNAL);
        if (k < 0 && c->pid <= 0 && zerrno == EAGAIN) {
            struct pollfd p = { c->wfd, POLLOUT, 0 };
            if (poll(&p, 1, READ_SECS * 1000) <= 0) return -ETIMEDOUT;
            continue;
        }
        if (k <= 0) return -1;
        b += k;
        n -= k;
    }
    return 0;
}

static void c_close(struct conn *c)
{
    if (c->sock >= 0) sys_close(c->sock);
    if (c->rfd >= 0 && c->rfd != c->sock) sys_close(c->rfd);
    if (c->wfd >= 0 && c->wfd != c->sock && c->wfd != c->rfd) sys_close(c->wfd);
    c->sock = c->rfd = c->wfd = -1;
    if (c->pid > 0) {                   /* its input closed: close_notify, the end; else ended after 2 s */
        int st = -2;
        for (u32 t0 = sys_ticks(); (st = sys_waitnb(c->pid)) == -2 && sys_ticks() - t0 < 200;)
            sys_sleep(5);
        if (st == -2) {
            sys_kill(c->pid);
            sys_wait(c->pid);
        }
    }
    c->pid = -1;
}

/* the TLS helper over the connected socket; its first line: "ok", "ok clock DATE", "info TEXT"..., "error TEXT" */
static int c_tls(void *u, char *err, int errn)
{
    struct conn *c = u;
    int in[2] = { -1, -1 }, out[2] = { -1, -1 };
    if (sys_pipe(in) < 0 || sys_pipe(out) < 0) {
        snprintf(err, (u32)errn, "no pipes for TLS: %s", zwhy("failed"));
        if (in[0] >= 0) { sys_close(in[0]); sys_close(in[1]); }
        return -1;
    }
    char ps[8], *argv[4];
    snprintf(ps, sizeof ps, "%d", c->port);
    argv[0] = "tlspipe"; argv[1] = (char *)c->host; argv[2] = ps; argv[3] = 0;
    int io[3] = { in[0], out[1], c->sock };
    int pid = sys_spawn(TLSPIPE, argv, io);
    sys_close(in[0]);
    sys_close(out[1]);
    sys_close(c->sock);                 /* the helper has it now */
    c->sock = -1;
    c->wfd = in[1];
    c->rfd = out[0];
    if (pid < 0) {
        snprintf(err, (u32)errn, "cannot start %s: %s (the package tls)", TLSPIPE, zstrerror(pid) ? zstrerror(pid) : "failed");
        return -1;
    }
    c->pid = pid;
    say("TLS with %s (on a 68020 this takes up to a minute)", c->host);
    char l[160];
    for (;;) {
        int n = 0;
        for (;;) {
            unsigned char ch;
            struct pollfd p = { c->rfd, POLLIN, 0 };
            if (poll(&p, 1, TLS_SECS * 1000) <= 0 || sys_read(c->rfd, &ch, 1) != 1) {
                snprintf(err, (u32)errn, "TLS: no answer from %s", c->host);
                return -1;
            }
            if (ch == '\n') break;
            if (n < (int)sizeof l - 1) l[n++] = (char)ch;
        }
        l[n] = 0;
        if (!strncmp(l, "info ", 5)) { say("%s", l + 5); continue; }
        if (!strncmp(l, "ok", 2)) {
            if (!strncmp(l, "ok clock ", 9))
                say("the clock is not set: certificate dates checked only against %s", l + 9);
            return 0;
        }
        snprintf(err, (u32)errn, "%s", !strncmp(l, "error ", 6) ? l + 6 : "the TLS helper failed");
        return -1;
    }
}

static int c_open(struct conn *c, struct mconn *m, const char *host, int port, int tls, char *err, int errn)
{
    const char *w;
    memset(m, 0, sizeof *m);
    m->u = c;
    m->rd = c_rd;
    m->wr = c_wr;
    m->starttls = c_tls;
    c->host = host;
    c->port = port;
    say("connecting to %s port %d", host, port);
    if ((c->sock = zconnect(host, port, ZC_NONBLOCK, &w)) < 0) {   /* the one connection helper (socket.h) */
        snprintf(err, (u32)errn, "%s port %d: %s", host, port, w);
        c->sock = -1;
        c_close(c);
        return -1;
    }
    c->rfd = c->wfd = c->sock;
    c->pid = -1;
    if (tls == TLS_ON && c_tls(c, err, errn) < 0) {
        c_close(c);
        return -1;
    }
    return 0;
}

/* ---- passwords: the settings, a command's first line, or asked ---- */
static int prompt(const char *q, char *out, int cap, int echo, const char *init, int complete);

static int run_cmd(const char *cmd, char *out, int cap)
{
    int p[2];
    if (sys_pipe(p) < 0) return -1;
    char *argv[4] = { "sh", "-c", (char *)cmd, 0 };
    int io[3] = { 0, p[1], 2 };
    int pid = sys_spawn("/bin/sh", argv, io);
    sys_close(p[1]);
    if (pid < 0) { sys_close(p[0]); return -1; }
    int n = 0, k;
    while (n < cap - 1 && (k = sys_read(p[0], out + n, (u32)(cap - 1 - n))) > 0) n += k;
    char sink[64];
    while (sys_read(p[0], sink, sizeof sink) > 0) {}
    sys_close(p[0]);
    int st = sys_wait(pid);
    out[n] = 0;
    char *e = strchr(out, '\n');
    if (e) *e = 0;
    return st == 0 && out[0] ? 0 : -1;
}

static char pw_in[256], pw_out[256];
static int get_pass(char *pw, const char *pass, const char *cmd, const char *who, int ask)
{
    if (pw[0]) return 0;
    if (pass) { snprintf(pw, 256, "%s", pass); return 0; }
    if (cmd) {
        if (run_cmd(cmd, pw, 256) < 0) {
            say("the password command failed: %s", cmd);
            pw[0] = 0;
            return -1;
        }
        return 0;
    }
    if (!ask) return -1;
    char q[200];
    snprintf(q, sizeof q, "Password for %s: ", who);
    return prompt(q, pw, 256, 0, "", 0) > 0 ? 0 : -1;
}

/* a folder's name as shown (the server's name is never printed: it may hold any byte) */
static const char *shown(const char *raw);

/* ---- the reading session ---- */
static struct mconn IC;
static struct msess S;
static int logged_in;

static void progress(void *u, int done, int total)
{
    (void)u;
    say("reading the index: %d of %d", done, total);
}

static int session(void)                /* connected and logged in: 0, else -1 (said) */
{
    char err[200];
    const char *host = cf.imap ? cf.imap : cf.pop;
    if (!host) {
        say("no server to read from: \"imap HOST\" or \"pop HOST\" in ~/.config/mail/config");
        return -1;
    }
    c_close(&IN);
    if (get_pass(pw_in, cf.pass, cf.passcmd, cf.user ? cf.user : "the server", 1) < 0) {
        say("no password: not logged in");
        return -1;
    }
    if (c_open(&IN, &IC, host, cf.port, cf.tls, err, sizeof err) < 0) {
        say("%s", err);
        return -1;
    }
    S.c = &IC;
    S.pop = cf.pop != 0;
    S.progress = progress;
    say("logging in to %s", host);
    int e = ms_login(&S, cf.tls, cf.user ? cf.user : "", pw_in, cf.plain_auth);
    if (e < 0) {
        if (e == ME_NO) pw_in[0] = 0;   /* asked again next time */
        say("%s: %s", host, S.err);
        c_close(&IN);
        return -1;
    }
    logged_in = 1;
    return 0;
}

static void reselected(void);

/* a lost connection taken up again, the folder selected again: 0 as it was, 1 made again, -1 not (said) */
static int ensure(void)
{
    if (logged_in && !IC.dead) return 0;
    char cur[256];
    snprintf(cur, sizeof cur, "%s", S.cur[0] ? S.cur : cf.folder);
    logged_in = 0;
    if (session() < 0) return -1;
    int e = ms_select(&S, cur, cf.limit);
    reselected();
    if (e < 0) { say("%s: %s", shown(cur), S.err); return -1; }
    return 1;
}

/* an operation's failure said; a lost connection is taken up again at the next key */
static int failed(int e, const char *what)
{
    if (e >= 0) return 0;
    if (e == ME_NET || IC.dead) {
        logged_in = 0;
        c_close(&IN);
        say("%s: %s (connecting again at the next key)", what, S.err);
    } else
        say("%s: %s", what, S.err);
    return 1;
}

/* ---- the screen ---- */
enum { V_INDEX, V_PAGER, V_FOLDERS, V_HELP };
static int view = V_INDEX, icur, itop, fcur, ftop;
/* the message read: raw, parsed, and its rows (offsets into ptext) */
static char *praw;
static struct mmsg pm;
static int pidx = -1, pall, phdr, prow, nrows_, rcap;
static u32 puid;                        /* the message read, by its UID (its index changes when the folder is read again) */
static struct sb ptext;
static u32 *roff;
static unsigned short *rlen;

#define TOP 2
#define H (rows - 3)

static void keyhints(void);

static const char *shown(const char *raw)
{
    static char b[256];
    int i = ms_findbox(&S, raw);
    if (i >= 0) return S.box[i].shown;
    char t[256];
    int k = mutf7_decode(raw, t, sizeof t);
    if (k >= 0) mail_clean(t, (u32)k, b, sizeof b);
    else mail_clean(raw, (u32)strlen(raw), b, sizeof b);
    return b;
}

static void draw_title(const char *t)
{
    char b[300];
    snprintf(b, sizeof b, " %s", t);
    zscr_bar(1, b);
}

static int unread(void)
{
    int n = 0;
    for (int i = 0; i < S.nm; i++)
        if (!(S.m[i].flags & MF_SEEN)) n++;
    return n;
}

static const char *mon3[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void index_row(int r, int i)
{
    at(r);
    if (i >= S.nm) { oputs("\033[K"); return; }
    struct mitem *it = &S.m[i];
    char d[12] = "      ";
    if (it->date) {
        int y, mo, dd, h, mi, s, wd, y2, mo2, d2;
        mail_civil(sys_time(), &y2, &mo2, &d2, &h, &mi, &s, &wd);
        mail_civil(it->date, &y, &mo, &dd, &h, &mi, &s, &wd);
        if (y == y2 && mo == mo2 && dd == d2) snprintf(d, sizeof d, " %02d:%02d", h, mi);
        else if (y == y2) snprintf(d, sizeof d, "%s %2d", mon3[mo - 1], dd);
        else snprintf(d, sizeof d, "  %4d", y);
    }
    if (i == icur) oputs("\033[7m");
    else if (!(it->flags & MF_SEEN)) oputs("\033[1m");
    char f[4];
    f[0] = it->flags & MF_DELETED ? 'D' : !(it->flags & MF_SEEN) ? 'N' : it->flags & MF_ANSWERED ? 'r' : ' ';
    f[1] = it->flags & MF_ATTACH ? '+' : ' ';
    f[2] = ' ';
    f[3] = 0;
    oputs(f);
    oputs(d);
    oputs("  ");
    int fw = cols >= 80 ? 20 : cols / 4;
    putw(it->from, fw);
    oputs("  ");
    int rest = cols - 1 - 3 - 6 - 2 - fw - 2;
    putw(it->subj[0] ? it->subj : "(no subject)", rest > 0 ? rest : 0);
    oputs("\033[m");
}

static void draw_index(void)
{
    char t[300];
    if (icur >= S.nm) icur = S.nm - 1;
    if (icur < 0) icur = 0;
    if (icur < itop) itop = icur;
    if (icur >= itop + H) itop = icur - H + 1;
    if (itop < 0) itop = 0;
    const char *bn = shown(S.cur);
    snprintf(t, sizeof t, "mail %s  %s  %d message%s, %d unread%s", MAIL_VERSION, bn, S.exists, S.exists == 1 ? "" : "s",
             unread(), S.exists > S.nm ? "  (the newest shown: limit)" : "");
    draw_title(t);
    for (int r = 0; r < H; r++) index_row(TOP + r, itop + r);
    if (!S.nm) { at(TOP); oputs("  (no messages)"); }
}

static void draw_folders(void)
{
    if (fcur >= S.nbox) fcur = S.nbox - 1;
    if (fcur < 0) fcur = 0;
    if (fcur < ftop) ftop = fcur;
    if (fcur >= ftop + H) ftop = fcur - H + 1;
    char t[100];
    snprintf(t, sizeof t, "mail %s  folders (%d)", MAIL_VERSION, S.nbox);
    draw_title(t);
    for (int r = 0; r < H; r++) {
        int i = ftop + r;
        at(TOP + r);
        if (i >= S.nbox) { oputs("\033[K"); continue; }
        if (i == fcur) oputs("\033[7m");
        oputs(!strcmp(S.box[i].name, S.cur) ? " * " : "   ");
        putw(S.box[i].shown, cols - 4);
        oputs("\033[m");
    }
}

/* rows: the text cut at the width (words kept whole where a space is in the row's last third) */
static void wrap(void)
{
    nrows_ = 0;
    u32 w = (u32)(cols - 1), o = 0, n = ptext.n;
    const char *t = ptext.p ? ptext.p : "";
    while (o < n || (!nrows_ && !n)) {
        const char *e = mchr(t + o, '\n', n - o);
        u32 ln = e ? (u32)(e - (t + o)) : n - o, k = 0;
        do {
            u32 left = ln - k, f = zu8fit(t + o + k, left, (int)w);
            if (!f && left) { int c; f = (u32)zu8next(t + o + k, left, &c); }
            if (f < left && t[o + k + f] != ' ')
                for (u32 j = f; j > f * 2 / 3; j--)
                    if (t[o + k + j - 1] == ' ') { f = j; break; }
            if (nrows_ == rcap) {
                int nc = rcap ? rcap * 2 : 256;
                u32 *a = realloc(roff, (u32)nc * sizeof *a);
                if (!a) return;
                roff = a;
                unsigned short *b = realloc(rlen, (u32)nc * sizeof *b);
                if (!b) return;
                rlen = b;
                rcap = nc;
            }
            roff[nrows_] = o + k;
            rlen[nrows_++] = (unsigned short)f;
            k += f;
        } while (k < ln);
        o += ln + 1;
        if (!e) break;
    }
}

static const char *const hlabel[] = { "Date", "From", "To", "Cc", "Reply-To", "Subject" };
static void pager_build(void)
{
    sb_free(&ptext);
    sb_init(&ptext, MAIL_TEXTMAX + 65536);
    phdr = 0;
    if (pall) {                         /* every header, each line clean */
        const char *h = pm.hdr;
        u32 n = pm.hdrlen, i = 0;
        char l[300];
        while (i < n) {
            u32 j = i;
            while (j < n && h[j] != '\n') j++;
            int k = mail_clean(h + i, j - i, l, sizeof l);
            sb_put(&ptext, l, (u32)k);
            sb_putc(&ptext, '\n');
            phdr++;
            i = j + 1;
        }
    } else {
        char *v[6] = { pm.date, pm.from, pm.to, pm.cc, pm.replyto, pm.subject };
        for (int i = 0; i < 6; i++)
            if (v[i] && *v[i] && (i != 4 || strcmp(pm.replyto, pm.from))) {
                sb_printf(&ptext, "%s: ", hlabel[i]);
                sb_puts(&ptext, v[i]);
                sb_putc(&ptext, '\n');
                phdr++;
            }
    }
    sb_putc(&ptext, '\n');
    sb_puts(&ptext, pm.text ? pm.text : "");
    sb_putc(&ptext, '\n');
    if (pm.cut) sb_puts(&ptext, "\n[the text is longer: only its first megabyte is shown]\n");
    if (pm.natt || pm.moreatt) {
        sb_puts(&ptext, "\n-- attachments (s saves one) --\n");
        for (int i = 0; i < pm.natt; i++) {
            u32 sz = pm.att[i].size;
            sb_printf(&ptext, "[%d] %s  (%s, %u %s)\n", i + 1, pm.att[i].name[0] ? pm.att[i].name : "(no name)",
                      pm.att[i].type, sz >= 10240 ? (sz + 512) / 1024 : sz, sz >= 10240 ? "KB" : "bytes");
        }
        if (pm.moreatt) sb_printf(&ptext, "(%d more parts not listed)\n", pm.moreatt);
    }
    wrap();
}

static void draw_pager(void)
{
    char t[300];
    const char *bn = shown(S.cur);
    if (view == V_HELP) snprintf(t, sizeof t, "mail %s  the keys (~/.config/mail/keys)", MAIL_VERSION);
    else snprintf(t, sizeof t, "%s  message %d of %d%s", bn, pidx + 1, S.nm, pm.html ? "  (from HTML)" : "");
    draw_title(t);
    if (prow > nrows_ - H) prow = nrows_ - H;
    if (prow < 0) prow = 0;
    const char *x = ptext.p ? ptext.p : "";
    for (int r = 0; r < H; r++) {
        int i = prow + r;
        at(TOP + r);
        if (i >= nrows_) { oputs("\033[K"); continue; }
        const char *s = x + roff[i];
        int n = rlen[i];
        int bol = roff[i] == 0 || x[roff[i] - 1] == '\n';
        if (view == V_PAGER && i < phdr && bol) {
            const char *c = mchr(s, ':', (u32)n);
            if (c) {
                oputs("\033[1m");
                oputn(s, (u32)(c - s) + 1);
                oputs("\033[m");
                oputn(c + 1, n - (int)(c - s) - 1);
                oputs("\033[K");
                continue;
            }
        }
        int q = view == V_PAGER && i > phdr && s[0] == '>';
        for (int j = i; !q && view == V_PAGER && j > phdr && j > i - 20; j--)    /* a quote's wrapped rows too */
            if (roff[j] == 0 || x[roff[j] - 1] == '\n') { q = x[roff[j]] == '>'; break; }
        if (q) oputs("\033[36m");
        oputn(s, n);
        oputs(q ? "\033[m\033[K" : "\033[K");
    }
}

static void draw(void)
{
    zscr_size();
    if (rows < 6) rows = 6;
    if (cols < 30) cols = 30;
    if (view == V_INDEX) draw_index();
    else if (view == V_FOLDERS) draw_folders();
    else draw_pager();
    keyhints();
    at(rows);
    putw(msg, cols - 1);
    oflush();
}

/* ---- the keys ---- */
enum { A_NONE, A_UP, A_DOWN, A_PGUP, A_PGDN, A_TOP, A_BOTTOM, A_OPEN, A_BACK, A_QUIT, A_FOLDERS, A_COMPOSE, A_REPLY,
       A_REPLYALL, A_FORWARD, A_DELETE, A_UNDELETE, A_READ, A_MOVE, A_SAVE, A_SYNC, A_HEADERS, A_NEXT, A_PREV,
       A_HELP, A_REDRAW, A_N };
static const char *const aname[A_N] = { "none", "up", "down", "page_up", "page_down", "top", "bottom", "open", "back",
    "quit", "folders", "compose", "reply", "reply_all", "forward", "delete", "undelete", "toggle_read", "move", "save",
    "sync", "headers", "next", "prev", "help", "redraw" };
static const char defkeys[] =
    "# mail's keys (docs/man/mail.txt): KEY ACTION, a key a line; <Name> for <Up> <Down> <Left> <Right> <S-Up>\n"
    "# <Return> <Esc> <Tab> <Space> <BS> <Del> <F1>..<F10> <C-x>; \"KEY none\" takes a key away. Home and End act as\n"
    "# <C-a> and <C-e>, PageUp and PageDown as <S-Up> and <S-Down>.\n"
    "<Up> up\nk up\n<Down> down\nj down\n<S-Up> page_up\n- page_up\n<S-Down> page_down\n<Space> page_down\n"
    "<C-a> top\n< top\n<C-e> bottom\n> bottom\n<Return> open\n<Right> open\nq back\n<Left> back\nQ quit\n"
    "c folders\nm compose\nr reply\ng reply_all\nf forward\nd delete\nu undelete\nN toggle_read\nM move\ns save\n"
    "$ sync\nh headers\nn next\np prev\n? help\n<F1> help\n<Help> help\n<C-l> redraw\n";
static struct kbind kb[96];
static struct keyset ks = { aname, A_N, kb, 0, 96, "" };

static void keyhints(void)
{
    static const unsigned char hi[3][9] = {
        { A_OPEN, A_COMPOSE, A_REPLY, A_DELETE, A_READ, A_FOLDERS, A_SYNC, A_HELP, A_BACK },
        { A_BACK, A_REPLY, A_REPLYALL, A_FORWARD, A_DELETE, A_SAVE, A_HEADERS, A_HELP, 0 },
        { A_OPEN, A_BACK, A_HELP, 0, 0, 0, 0, 0, 0 } };
    static const char *const lbl[A_N] = { [A_OPEN] = "open", [A_COMPOSE] = "new", [A_REPLY] = "reply",
        [A_DELETE] = "delete", [A_READ] = "read/new", [A_FOLDERS] = "folders", [A_SYNC] = "sync", [A_HELP] = "keys",
        [A_BACK] = "back", [A_REPLYALL] = "all", [A_FORWARD] = "forward", [A_SAVE] = "save", [A_HEADERS] = "headers" };
    int v = view == V_INDEX ? 0 : view == V_FOLDERS ? 2 : 1;
    char b[300];
    int o = 0;
    for (int i = 0; i < 9 && hi[v][i] && o < (int)sizeof b - 40; i++) {
        int a = hi[v][i];
        if (view == V_HELP && a != A_BACK) continue;
        for (int k = 0; k < ks.n; k++)
            if (kb[k].act == a && !kb[k].k2) {
                char kn[12];
                keys_name(kb[k].k1, kn);
                if (kn[0] == '<' && kn[1]) {     /* <Return> -> Return */
                    memmove(kn, kn + 1, strlen(kn));
                    kn[strlen(kn) - 1] = 0;
                }
                o += snprintf(b + o, sizeof b - (u32)o, "%s:%s  ", kn,
                              a == A_BACK && view == V_INDEX ? "quit" : lbl[a] ? lbl[a] : aname[a]);
                break;
            }
    }
    b[o] = 0;
    char l[310];
    snprintf(l, sizeof l, " %s", b);
    zscr_bar(rows - 1, l);              /* the status line (the keys), above the message row */
}

/* a line typed on the message row: its length, -1 when given up (Esc, Ctrl-G, Ctrl-C); complete: Tab cycles the
 * folders' names */
static int prompt(const char *q, char *out, int cap, int echo, const char *init, int complete)
{
    struct zled e;                      /* libz's line editor: the keys every typed line has */
    int tab = 0;
    char pre[256] = "";
    zled_init(&e, out, cap, init);
    for (;;) {
        if (echo)
            zled_show(&e, rows, 1, cols - 1, q);
        else {
            at(rows);
            oputs(q);
            oputs("\033[K");
        }
        oflush();
        int k = keys_tty(0), r = k < 0 ? ZLED_CANCEL : zled_key(&e, k);
        if (r == ZLED_CANCEL || r == ZLED_EOF) { msg[0] = 0; return -1; }
        if (r == ZLED_DONE) { msg[0] = 0; return e.n; }
        if (r == ZLED_OTHER && k == 9 && complete && S.nbox) {    /* the folders that start with what was typed, in turn */
            if (!tab) snprintf(pre, sizeof pre, "%s", out);
            int pl = (int)strlen(pre), nm = 0, seen = 0;
            for (int i = 0; i < S.nbox; i++)
                if (!strncmp(S.box[i].shown, pre, (u32)pl)) nm++;
            for (int i = 0; nm && i < S.nbox; i++)
                if (!strncmp(S.box[i].shown, pre, (u32)pl) && seen++ == tab % nm) {
                    zled_set(&e, S.box[i].shown);
                    break;
                }
            tab++;
            continue;
        }
        tab = 0;
    }
}

static int yesno(const char *q, const char *keys)  /* the key typed of keys, or 0 */
{
    at(rows);
    oputs(q);
    oputs("\033[K");
    oflush();
    int k = keys_tty(0);
    msg[0] = 0;
    return k > 0 && k < 127 && strchr(keys, k) ? k : 0;
}

/* ---- reading ---- */
static void pager_close(void)
{
    free(praw);
    praw = 0;
    mime_free(&pm);
    sb_free(&ptext);
    pidx = -1;
    if (view == V_PAGER) view = V_INDEX;
}

/* the index was read again: the message shown found again by its UID (POP3's numbers are not UIDs: closed) */
static void reselected(void)
{
    if (pidx >= 0) {
        int j = 0;
        while (j < S.nm && S.m[j].uid != puid) j++;
        if (j >= S.nm || S.pop) pager_close();
        else pidx = j;
    }
    if (icur >= S.nm) icur = S.nm - 1;
    if (icur < 0) icur = 0;
}

/* message i, found again after a reconnect: its index, or -1 (said) */
static int target_of(int i)
{
    if (i < 0 || i >= S.nm) return -1;
    u32 uid = S.m[i].uid;
    int r = ensure();
    if (r < 0) return -1;
    if (r > 0 && S.pop) {
        say("connected again: POP3's numbers may have changed, so nothing was done (try again)");
        return -1;
    }
    for (i = 0; i < S.nm && S.m[i].uid != uid; i++) {}
    if (i >= S.nm) { say("the message is gone"); return -1; }
    return i;
}
static int target(void) { return target_of(view == V_PAGER ? pidx : icur); }   /* the one read, or the cursor's */

static int pager_open(int i)            /* message i fetched and parsed */
{
    if ((i = target_of(i)) < 0) return -1;
    pager_close();
    say("reading the message (%u KB)", (S.m[i].size + 1023) / 1024);
    u32 n;
    int cut, e = ms_fetch(&S, i, &praw, &n, &cut);
    if (failed(e, "reading")) return -1;
    if (mime_parse(&pm, praw, n) < 0) { say("out of memory for this message"); pager_close(); return -1; }
    pidx = i;
    puid = S.m[i].uid;
    icur = i;
    prow = 0;
    pall = 0;
    pager_build();
    msg[0] = 0;
    if (cut) say("only the first %u KB of this message were read", MAIL_MSGMAX / 1024);
    if (!(S.m[i].flags & MF_SEEN) && !S.pop) failed(ms_flag(&S, i, MF_SEEN, 1), "marking it read");
    else S.m[i].flags |= MF_SEEN;
    view = V_PAGER;
    return 0;
}

static void help_open(void)
{
    sb_free(&ptext);
    sb_init(&ptext, 65536);
    sb_puts(&ptext, "Each key and what it does (q goes back). Change them in ~/.config/mail/keys.\n\n");
    for (int a = 1; a < A_N; a++) {
        char line[200];
        int o = snprintf(line, sizeof line, "%-12s", aname[a]);
        for (int k = 0; k < ks.n; k++)
            if (kb[k].act == a) {
                char kn[12];
                keys_name(kb[k].k1, kn);
                o += snprintf(line + o, sizeof line - (u32)o, " %s", kn);
                if (o > 150) break;
            }
        sb_puts(&ptext, line);
        sb_putc(&ptext, '\n');
    }
    sb_puts(&ptext, "\nIn a question on the last row: Return answers, Esc gives up, Tab completes a folder.\n");
    phdr = 0;
    prow = 0;
    wrap();
    view = V_HELP;
}

static int select_box(const char *name)
{
    if (ensure() < 0) return -1;
    pager_close();
    say("opening %s", shown(name));
    int e = ms_select(&S, name, cf.limit);
    if (failed(e, shown(name))) return -1;
    icur = itop = 0;
    msg[0] = 0;
    view = V_INDEX;
    return 0;
}

/* ---- writing ---- */
static char draft_path[ZPATH_MAX];


static int write_file(const char *path, const char *t, u32 n, int mode)
{
    int fd = sys_open(path, O_WRONLY | O_CREAT | (mode == 0600 ? O_EXCL : O_TRUNC));   /* a private file: a new one */
    if (fd < 0) return fd;
    sys_chmod(path, (u32)mode);
    int w = n ? sys_write(fd, t, n) : 0;
    sys_close(fd);
    return w == (int)n ? 0 : -EIO;
}

static int editor(const char *path)
{
    const char *e = cf.editor ? cf.editor : getenv("EDITOR");
    char line[ZPATH_MAX], prog[ZPATH_MAX], *av[14];
    int n = 0;
    snprintf(line, sizeof line, "%s", e && *e ? e : "/bin/edit");
    for (char *s = line; *s && n < 12;) {             /* the editor may carry words ("sumi -x") */
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        av[n++] = s;
        while (*s && *s != ' ') s++;
    }
    if (!n) return -1;
    av[n++] = (char *)path;
    av[n] = 0;
    if (strchr(av[0], '/')) snprintf(prog, sizeof prog, "%s", av[0]);
    else if (!path_find(av[0], prog, sizeof prog)) { say("no editor %s", av[0]); return -1; }
    oputs("\033[r\033[2J\033[H");
    oflush();
    sys_ttymode(0);
    int pid = sys_spawn(prog, av, 0);
    int st = pid < 0 ? pid : sys_wait(pid);
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    oputs("\033[2J");
    if (pid < 0) { say("cannot start %s: %s", prog, zstrerror(pid) ? zstrerror(pid) : "failed"); return -1; }
    return st;
}

static int send_now(const char *m, u32 n, char *const *rcpt, int nr, const char *from, int ask)
{
    char err[200];
    static struct mconn oc;             /* (its 2 KB buffer not on the stack) */
    if (!cf.smtp) {
        say("no server to send through: \"smtp HOST\" in ~/.config/mail/config");
        return -1;
    }
    const char *user = cf.smtp_user ? cf.smtp_user : cf.user;
    const char *pass = cf.smtp_pass ? cf.smtp_pass : cf.smtp_user ? 0 : cf.pass;
    const char *pcmd = cf.smtp_passcmd ? cf.smtp_passcmd : cf.smtp_user ? 0 : cf.passcmd;
    if (user && !pass && !pcmd && !cf.smtp_user && pw_in[0]) pass = pw_in;      /* the one asked for reading */
    if (user && get_pass(pw_out, pass, pcmd, user, ask) < 0) {
        say("no password for sending");
        return -1;
    }
    if (c_open(&OUT, &oc, cf.smtp, cf.smtp_port, cf.smtp_tls, err, sizeof err) < 0) {
        say("%s", err);
        return -1;
    }
    say("sending to %d recipient%s", nr, nr == 1 ? "" : "s");
    int e = smtp_send(&oc, cf.smtp_tls, user, pw_out, cf.plain_auth, from, rcpt, nr, m, n, err, sizeof err);
    c_close(&OUT);
    if (e < 0) {
        if (e == ME_NO && strstr(err, "login")) pw_out[0] = 0;
        say("not sent: %s", err);
        return -1;
    }
    return 0;
}

static void compose(int kind)
{
    const char *home = getenv("HOME");
    if (!cf.from) { say("no sender: \"from Your Name <you@example.org>\" in ~/.config/mail/config"); return; }
    int src = -1, from_index = 0;
    if (kind != DRAFT_NEW) {            /* the message answered: the one read, or the one under the cursor */
        if (view != V_PAGER) {
            if (!S.nm || pager_open(icur) < 0) return;
            view = V_INDEX;
            from_index = 1;
        }
        src = pidx;
    }
    char *d = mail_draft(kind, kind == DRAFT_NEW ? 0 : &pm, cf.from, 0);
    if (!d) { say("out of memory"); return; }
    if (!home || !*home) { say("no HOME: nowhere private for the draft"); free(d); return; }
    snprintf(draft_path, sizeof draft_path, "%s/.config/mail/draft-%d", home, sys_getpid());
    sys_unlink(draft_path);
    if (write_file(draft_path, d, (u32)strlen(d), 0600) < 0) {
        say("cannot write %s", draft_path);
        free(d);
        return;
    }
    u32 srcuid = src >= 0 ? S.m[src].uid : 0;
    for (;;) {
        editor(draft_path);
        char *t = zslurp(draft_path, MAIL_TEXTMAX + 131072, 0);
        if (!t) { say("the draft cannot be read: not sent (%s)", draft_path); break; }
        if (!strcmp(t, d)) {
            free(t);
            say("not sent: the message was not changed");
            sys_unlink(draft_path);
            break;
        }
        static char *rcpt[MAIL_RCPTMAX], rb[MAIL_RCPTMAX * 64], from[300], err[200];   /* (not on the stack) */
        int nr = 0;
        u32 n = 0;
        u32 rnd = sys_hrtime() ^ (u32)sys_getpid() << 16;
        char *m = mail_build(t, sys_time(), rnd, &n, rcpt, &nr, rb, sizeof rb, from, sizeof from, err, sizeof err);
        free(t);
        int k;
        if (!m) {
            snprintf(msg, sizeof msg, "%s", err);
            draw();
            k = yesno("Not sent. e edit again, q drop it: ", "eq");
            if (k == 'e') continue;
            sys_unlink(draft_path);
            say("dropped");
            break;
        }
        char q[200];
        snprintf(q, sizeof q, "Send to %s%s? y send, e edit, q drop it: ", rcpt[0], nr > 1 ? " and others" : "");
        draw();
        k = yesno(q, "yeq");
        if (k == 'e') { free(m); continue; }
        if (k != 'y') { free(m); sys_unlink(draft_path); say("dropped"); break; }
        int sent;
        while ((sent = send_now(m, n, rcpt, nr, from, 1)) < 0) {
            draw();
            k = yesno("y try again, e edit, q drop it: ", "yeq");
            if (k != 'y') break;
        }
        if (sent < 0) {
            free(m);
            if (k == 'e') continue;
            sys_unlink(draft_path);
            say("dropped");
            break;
        }
        sys_unlink(draft_path);
        int saved = 0;
        if (cf.sent && !S.pop && ensure() == 0)
            saved = !failed(ms_append(&S, cf.sent, m, n), "keeping a copy");
        free(m);
        if (srcuid && kind != DRAFT_FWD)
            for (int i = 0; i < S.nm; i++)
                if (S.m[i].uid == srcuid && !S.pop) failed(ms_flag(&S, i, MF_ANSWERED, 1), "marking it answered");
        if (msg[0] == 0 || saved) say("sent%s", saved ? ", a copy kept in the sent folder" : "");
        else say("sent; %s", msg);
        break;
    }
    free(d);
    if (from_index) pager_close();
}

/* ---- the attachments ---- */
static void save_att(void)
{
    if (view != V_PAGER || !pm.natt) { say("no attachments"); return; }
    char a[16], path[ZPATH_MAX];
    int i = 0;
    if (pm.natt > 1) {
        char q[60];
        snprintf(q, sizeof q, "Save which (1-%d)? ", pm.natt);
        if (prompt(q, a, sizeof a, 1, "", 0) <= 0) return;
        i = atoi(a) - 1;
        if (i < 0 || i >= pm.natt) { say("no attachment %s", a); return; }
    }
    char def[120];
    snprintf(def, sizeof def, "%s", pm.att[i].name[0] ? pm.att[i].name : "attachment");
    if (prompt("Save as: ", path, sizeof path, 1, def, 0) <= 0) return;
    struct ustat st;
    if (sys_stat(path, &st) == 0 && yesno("It exists. Write over it? y/n ", "yn") != 'y') { say("not saved"); return; }
    u32 n;
    char *d = mime_decode_part(&pm.att[i], &n);
    if (!d) { say("out of memory"); return; }
    int e = write_file(path, d, n, 0644);
    free(d);
    if (e < 0) say("%s: %s", path, zstrerror(e) ? zstrerror(e) : "cannot write");
    else say("saved %u bytes to %s", n, path);
}

/* ---- the actions ---- */
static int quit;

static void move_cur(int d)
{
    if (view == V_FOLDERS) fcur += d;
    else if (view == V_INDEX) icur += d;
    else prow += d;
}

static void act(int a)
{
    int *cur = view == V_FOLDERS ? &fcur : &icur;
    msg[0] = 0;
    if (view == V_HELP && a != A_UP && a != A_DOWN && a != A_PGUP && a != A_PGDN && a != A_TOP && a != A_BOTTOM &&
        a != A_REDRAW && a != A_QUIT) {
        if (a == A_BACK || a == A_HELP || a == A_OPEN) view = pidx >= 0 ? (pager_build(), V_PAGER) : V_INDEX;
        return;
    }
    switch (a) {
    case A_UP: move_cur(-1); break;
    case A_DOWN: move_cur(1); break;
    case A_PGUP: move_cur(-(H - 1)); break;
    case A_PGDN: move_cur(H - 1); break;
    case A_TOP: if (view == V_PAGER || view == V_HELP) prow = 0; else *cur = 0; break;
    case A_BOTTOM: if (view == V_PAGER || view == V_HELP) prow = nrows_; else *cur = view == V_FOLDERS ? S.nbox - 1 : S.nm - 1; break;
    case A_OPEN:
        if (view == V_INDEX && S.nm) pager_open(icur);
        else if (view == V_FOLDERS && S.nbox) select_box(S.box[fcur].name);
        break;
    case A_BACK:
        if (view == V_PAGER) { pager_close(); view = V_INDEX; }
        else if (view == V_FOLDERS) view = V_INDEX;
        else quit = 1;
        break;
    case A_QUIT: quit = 1; break;
    case A_FOLDERS:
        if (ensure() < 0) break;
        if (failed(ms_folders(&S), "the folders")) break;
        fcur = 0;
        for (int i = 0; i < S.nbox; i++) if (!strcmp(S.box[i].name, S.cur)) fcur = i;
        if (view == V_PAGER) pager_close();
        view = V_FOLDERS;
        break;
    case A_COMPOSE: compose(DRAFT_NEW); break;
    case A_REPLY: if (view != V_FOLDERS) compose(DRAFT_REPLY); break;
    case A_REPLYALL: if (view != V_FOLDERS) compose(DRAFT_ALL); break;
    case A_FORWARD: if (view != V_FOLDERS) compose(DRAFT_FWD); break;
    case A_DELETE: case A_UNDELETE: case A_READ: {
        if (view == V_FOLDERS || !S.nm) break;
        int i = target();
        if (i < 0) break;
        if (a == A_READ && S.pop) { say("POP3 keeps no read marks"); break; }
        int f = a == A_READ ? MF_SEEN : MF_DELETED, on = a == A_DELETE ? 1 : a == A_UNDELETE ? 0 : !(S.m[i].flags & MF_SEEN);
        if (!failed(ms_flag(&S, i, f, on), aname[a])) {
            if (a == A_DELETE) say("deleted (%s removes it from the server)", S.pop ? "quitting" : "sync");
            if (view == V_INDEX && a != A_READ && icur < S.nm - 1) icur++;
        }
        break;
    }
    case A_MOVE: {
        if (view == V_FOLDERS || !S.nm) break;
        if (S.pop) { say("POP3 has one folder: nothing can be moved"); break; }
        if (!S.nbox && (ensure() < 0 || failed(ms_folders(&S), "the folders"))) break;
        char to[256];
        if (prompt("Move to (Tab: the folders): ", to, sizeof to, 1, "", 1) <= 0) break;
        int b = ms_findbox(&S, to);
        if (b < 0) { say("no folder %s", to); break; }
        int i = target();
        if (i < 0) break;
        if (failed(ms_copy(&S, i, S.box[b].name), "moving")) break;
        if (failed(ms_flag(&S, i, MF_DELETED, 1), "moving")) break;
        say("moved to %s (deleted here: sync removes it)", S.box[b].shown);
        if (view == V_INDEX && icur < S.nm - 1) icur++;
        break;
    }
    case A_SAVE: save_att(); break;
    case A_SYNC: {
        if (view == V_FOLDERS) break;
        char cur[256];
        snprintf(cur, sizeof cur, "%s", S.cur);
        if (ensure() < 0) break;
        say("removing the deleted messages");
        if (failed(ms_expunge(&S), "sync")) break;
        if (S.pop) { logged_in = 0; c_close(&IN); }
        select_box(cur);
        break;
    }
    case A_HEADERS: if (view == V_PAGER) { pall = !pall; prow = 0; pager_build(); } break;
    case A_NEXT: case A_PREV: {
        int d = a == A_NEXT ? 1 : -1;
        if (view == V_PAGER) { int j = pidx + d; if (j >= 0 && j < S.nm) pager_open(j); }
        else move_cur(d);
        break;
    }
    case A_HELP: help_open(); break;
    case A_REDRAW: oputs("\033[2J"); break;
    }
}

/* ---- the settings ---- */
static const char defconf[] =
    "# mail's settings (docs/man/mail.txt): a setting and its value a line; # a comment.\n"
    "# Keep this file to yourself (mode 600): mail does not use a password from a file others may read.\n"
    "# Reading: imap HOST (or pop HOST); tls on, starttls or off; the port follows tls unless set.\n"
    "# imap imap.example.org\n# tls on\n# port 993\n# user you@example.org\n"
    "# The password, or a command that prints it (else mail asks):\n# password secret\n# passcmd cat ~/.mailpass\n"
    "# Sending (smtp_tls starttls on 587 or on on 465; smtp_user, smtp_password and smtp_passcmd if not the same):\n"
    "# smtp smtp.example.org\n# smtp_tls starttls\n# smtp_port 587\n# from Your Name <you@example.org>\n"
    "# A copy of what you send (IMAP), the first folder, the editor, the newest messages read:\n"
    "# sent Sent\n# folder INBOX\n# editor edit\n# limit 300\n"
    "# Send the password without TLS (a test server at home only):\n# plain_auth off\n";

static int load_config(const char *cfile, int must)
{
    char e[160];
    int fd = sys_open(cfile, O_RDONLY);
    struct ustat st;
    if (fd < 0) {
        if (must) { dprintf(2, "mail: %s: %s\n", cfile, zstrerror(fd) ? zstrerror(fd) : "cannot open"); return -1; }
        return mail_config(&cf, strdup(""), e, sizeof e);
    }
    char *t = sys_fstat(fd, &st) < 0 ? 0 : zslurpfd(fd, MAIL_CONFMAX, 0);   /* the file checked below is the one read */
    sys_close(fd);
    if (!t) {
        dprintf(2, "mail: %s: %s\n", cfile, zwhy("cannot read"));
        return -1;
    }
    if (mail_config(&cf, t, e, sizeof e) < 0) {
        dprintf(2, "mail: %s: %s\n", cfile, e);
        return -1;
    }
    /* another user's file, a password others may read, or a file others may change (passcmd): refused */
    if (st.uid != (u32)sys_getuid()) {
        dprintf(2, "mail: %s: not your file\n", cfile);
        return -1;
    }
    if ((st.mode & 022) || (cf.has_pass && (st.mode & 044))) {
        dprintf(2, "mail: %s: others may %s it: chmod 600 %s\n", cfile, st.mode & 022 ? "change" : "read", cfile);
        return -1;
    }
    return 0;
}

static const char USAGE[] = "usage: mail [-c FILE] [-f FOLDER]\n       mail [-c FILE] -s SUBJECT ADDRESS... < TEXT\n       mail -K\n";
static void usage(void)
{
    zusage(USAGE);
}

/* mail -s: the text from standard input to the addresses, through the settings' SMTP server */
static int send_batch(const char *subj, char **to, int nto)
{
    struct sb e;
    sb_init(&e, MAIL_TEXTMAX + 4096);
    if (!cf.from) { dprintf(2, "mail: no sender: \"from\" in the settings\n"); return 1; }
    for (int i = -1; i < nto; i++)      /* the subject and the addresses: no control characters (no header smuggled in) */
        for (const char *p = i < 0 ? subj : to[i]; *p; p++)
            if ((unsigned char)*p < 32 || *p == 127) {
                dprintf(2, "mail: a control character in the %s\n", i < 0 ? "subject" : "address");
                return 2;
            }
    sb_printf(&e, "From: %s\nTo: ", cf.from);
    for (int i = 0; i < nto; i++) { if (i) sb_puts(&e, ", "); sb_puts(&e, to[i]); }
    sb_puts(&e, "\nSubject: ");
    sb_puts(&e, subj);
    sb_puts(&e, "\n\n");
    static char b[1024];
    int k;
    while ((k = sys_read(0, b, sizeof b)) > 0) sb_put(&e, b, (u32)k);
    if (e.full || e.nomem) { dprintf(2, "mail: the text is too long\n"); return 1; }
    static char *rcpt[MAIL_RCPTMAX], rb[MAIL_RCPTMAX * 64], from[300], err[200];
    int nr = 0;
    u32 n;
    char *m = mail_build(e.p, sys_time(), sys_hrtime() ^ (u32)sys_getpid() << 16, &n, rcpt, &nr, rb, sizeof rb, from,
                         sizeof from, err, sizeof err);
    sb_free(&e);
    if (!m) { dprintf(2, "mail: %s\n", err); return 1; }
    batch = 1;
    int r = send_now(m, n, rcpt, nr, from, 0);
    free(m);
    if (r < 0) { dprintf(2, "mail: %s\n", msg); return 1; }
    return 0;
}


int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    const char *cfile = 0, *folder = 0, *subj = 0;
    int i;
    char path[ZPATH_MAX];
    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        const char *o = argv[i];
        if (!strcmp(o, "-K")) { sys_write(1, defkeys, sizeof defkeys - 1); return 0; }
        else if (!strcmp(o, "-c") && i + 1 < argc) cfile = argv[++i];
        else if (!strcmp(o, "-f") && i + 1 < argc) folder = argv[++i];
        else if (!strcmp(o, "-s") && i + 1 < argc) subj = argv[++i];
        else if (!strcmp(o, "--")) { i++; break; }
        else zbadopt(o, USAGE);
    }
    if ((subj && i >= argc) || (!subj && i < argc)) usage();
    const char *home = getenv("HOME");
    int made = 0;
    if (!cfile && home && strlen(home) < 200) {
        char d[ZPATH_MAX];
        snprintf(d, sizeof d, "%s/.config", home);
        sys_mkdir(d);
        strcat(d, "/mail");
        sys_mkdir(d);
        sys_chmod(d, 0700);                         /* the drafts and the settings: yours only */
        snprintf(path, sizeof path, "%s/config", d);
        cfile = path;
        struct ustat st;
        if (sys_stat(path, &st) < 0) {  /* none: a commented one to fill in, the owner's only */
            write_file(path, defconf, sizeof defconf - 1, 0600);
            made = 1;
        }
    }
    if (!cfile) { dprintf(2, "mail: no HOME: mail -c FILE\n"); return 2; }
    if (load_config(cfile, cfile != path) < 0) return 2;
    signal(SIGPIPE, SIG_IGN);
    if (subj) return send_batch(subj, argv + i, argc - i);
    if (!sys_isatty(0) || !sys_isatty(1)) { dprintf(2, "mail: the keys and the screen must be a terminal (mail -s sends)\n"); return 2; }
    if (!cf.imap && !cf.pop) {
        dprintf(2, "mail: no server: \"imap HOST\" (or \"pop HOST\") in %s%s\n", cfile, made ? " (written now, all commented out)" : "");
        return 2;
    }
    keys_load(&ks, defkeys, sizeof defkeys - 1, "mail");
    zscr_init();
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    oputs("\033[2J");
    if (ks.bad[0]) snprintf(msg, sizeof msg, "%s", ks.bad);
    draw();
    if (session() == 0) {
        int e = ms_select(&S, folder ? folder : cf.folder, cf.limit);
        if (e < 0) failed(e, shown(folder ? folder : cf.folder));
        else msg[0] = 0;
        if (!S.cur[0]) snprintf(S.cur, sizeof S.cur, "%s", folder ? folder : cf.folder);
    }
    while (!quit) {
        if (zscr_resized()) { oputs("\033[2J"); if (view == V_PAGER) pager_build(); else if (view == V_HELP) wrap(); }
        draw();
        int k = keys_tty(0);
        if (k < 0) break;
        if (k & 0x10000) continue;
        int k2 = 0;
        if (keys_prefix(&ks, k)) {
            struct pollfd pf = { 0, POLLIN, 0 };
            if (poll(&pf, 1, 2000) <= 0 || (k2 = keys_tty(0)) < 0) continue;
        }
        const struct kbind *b = keys_find(&ks, k, k2);
        if (b && b->act) act(b->act);
    }
    /* the end: deleted messages removed if wanted (POP3: RSET when not) */
    int nd = 0;
    for (int j = 0; j < S.nm; j++) if (S.m[j].flags & MF_DELETED) nd++;
    if (nd && logged_in && !IC.dead) {
        char q[100];
        snprintf(q, sizeof q, "Remove the %d deleted message%s from the server? y/n ", nd, nd == 1 ? "" : "s");
        if (yesno(q, "yn") == 'y') failed(ms_expunge(&S), "removing");
        else if (S.pop) { struct msess *s = &S; mc_send(s->c, "RSET"); char l[100]; mc_line(s->c, l, sizeof l); }
    }
    ms_logout(&S);
    c_close(&IN);
    oputs("\033[r\033[2J\033[H");
    oflush();
    sys_ttymode(0);
    pager_close();
    ms_free(&S);
    memset(pw_in, 0, sizeof pw_in);
    memset(pw_out, 0, sizeof pw_out);
    mail_cfg_free(&cf);
    return 0;
}
