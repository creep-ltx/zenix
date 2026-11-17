/* SPDX-License-Identifier: GPL-2.0-only */
/* irc [-c FILE] [-n NICK] [-p PORT] [-t] [SERVER [#CHANNEL...]]: an IRC client for the terminal (DESIGN D58,
 * docs/man/irc.txt). The protocol and the windows are irc's core (net/irc/, shared with the PC tests); this file is
 * the screen (a title row, the window's lines, a status row, the input row; 80 columns first), the keys (configurable:
 * ~/.config/irc/keys in the format of files, show and modplay, display/keys.h), the connection (plain, or TLS through
 * /lib/irc/tlspipe, which carries the crypto library so that irc does not) and the reconnect. One process, one loop:
 * poll on the keys and the server, a tick a second for the clock, the keepalive and the reconnect. */
#include "socket.h"
#include "../net/irc/irc.h"
#include "../display/keys.h"

#define TLSPIPE "/lib/irc/tlspipe"
#define CONFMAX 8192
#define EDMAX 1000
#define HISTN 32
#define PING_IDLE 120                   /* seconds of silence before our PING */
#define DEAD_IDLE 300                   /* ... and before the connection is taken for lost */
#define CONNECT_SECS 60                 /* a TCP connection (TLS: also the handshake, up to a minute on a 68020) */

static struct irc S;
static int rows = 25, cols = 80, tty, quit, winch;

/* ---- output: one buffer, written when the loop waits ---- */
static char outb[4096];
static int outn;
static void flush(void) { if (outn) sys_write(1, outb, (u32)outn); outn = 0; }
static void putn(const char *s, int n)
{
    if (outn + n > (int)sizeof outb) flush();
    if (n > (int)sizeof outb) { sys_write(1, s, (u32)n); return; }
    memcpy(outb + outn, s, (u32)n);
    outn += n;
}
static void puts_(const char *s) { putn(s, (int)strlen(s)); }
static void putf(const char *fmt, ...)
{
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    putn(b, n < (int)sizeof b ? n : (int)sizeof b - 1);
}
static void at(int r) { putf("\033[%d;1H", r); }

/* ---- the connection ---- */
enum { N_OFF, N_WAIT, N_CONNECTING, N_TLS, N_ON };
static int nstate, sockfd = -1, rfd = -1, wfd = -1, tlspid = -1, retries, use_tls, port;
static char host[IRC_NAMEMAX], why[120];
static u32 retry_at, since, last_rx;
static char tlsl[160];
static int tlsn;
/* the lines to the server, paced (a burst of 5, then one every 2 s: the servers' flood rule) */
static char q[8192];
static int qn, qsent, tokens = 5;
static u32 token_at;

static char *memchr_(char *p, int c, int n)
{
    for (; n > 0; n--, p++)
        if (*p == c)
            return p;
    return 0;
}

static void qsend(void *u, const char *l, int n)
{
    (void)u;
    if (qn + n > (int)sizeof q) {
        irc_print(&S, 0, IRC_K_ERR, "-!- the send queue is full: a line was not sent");
        return;
    }
    /* PONG and QUIT go ahead of the paced lines (a server's PING is not answered after a pasted text), at the
     * first line not yet begun */
    if ((!strncmp(l, "PONG ", 5) || !strncmp(l, "QUIT ", 5)) && qn > qsent) {
        char *e = qsent && q[qsent - 1] != '\n' ? memchr_(q + qsent, '\n', qn - qsent) : 0;
        int at = e ? (int)(e - q) + 1 : qsent;
        memmove(q + at + n, q + at, (u32)(qn - at));
        memcpy(q + at, l, (u32)n);
        qn += n;
        if (tokens < 1) tokens = 1;
        return;
    }
    memcpy(q + qn, l, (u32)n);
    qn += n;
}

static void net_close(const char *reason)
{
    if (sockfd >= 0) sys_close(sockfd);
    if (rfd >= 0 && rfd != sockfd) sys_close(rfd);
    if (wfd >= 0 && wfd != sockfd) sys_close(wfd);
    sockfd = rfd = wfd = -1;
    if (tlspid > 0) {                   /* its input closed: it sends close_notify and ends; in a handshake it is
                                         * ended after 2 s */
        int st = -2;
        for (u32 t0 = sys_ticks(); (st = sys_waitnb(tlspid)) == -2 && sys_ticks() - t0 < 200;)
            sys_sleep(5);
        if (st == -2) {
            sys_kill(tlspid);
            st = sys_wait(tlspid);
        }
        if (reason && !why[0] && st == 3)
            snprintf(why, sizeof why, "TLS error");
    }
    tlspid = -1;
    qn = qsent = 0;
    if (nstate != N_OFF && nstate != N_WAIT && reason) {
        irc_lost(&S, why[0] ? why : reason);
        why[0] = 0;
    }
    nstate = N_OFF;
}

static void schedule_retry(void)
{
    static const unsigned char delay[] = { 5, 10, 30, 60 };
    if (!S.c.reconnect || quit || !host[0]) {
        nstate = N_OFF;
        return;
    }
    int d = delay[retries < 3 ? retries : 3];
    retries++;
    retry_at = sys_uptime() + (u32)d;
    nstate = N_WAIT;
    irc_print(&S, 0, IRC_K_INFO, "-!- connecting again in %d s (/connect now, /quit to stop)", d);
}

static void net_open(void)
{
    struct addrinfo h, *r = 0;
    char ps[8];
    int e;
    why[0] = 0;
    memset(&h, 0, sizeof h);
    h.ai_family = AF_INET;
    h.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%d", port);
    irc_print(&S, 0, IRC_K_INFO, "-!- connecting to %s port %d%s", host, port, use_tls ? " (TLS)" : "");
    S.now = sys_time();
    if ((e = getaddrinfo(host, ps, &h, &r)) != 0) {
        irc_print(&S, 0, IRC_K_ERR, "-!- %s: %s", host, e == EAI_NONAME ? "no such host" : gai_strerror(e));
        schedule_retry();
        return;
    }
    sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    e = sockfd < 0 ? zerrno : connect(sockfd, r->ai_addr, r->ai_addrlen) < 0 ? zerrno : 0;
    freeaddrinfo(r);
    if (e && e != EINPROGRESS) {
        irc_print(&S, 0, IRC_K_ERR, "-!- %s port %d: %s", host, port, zstrerror(e) ? zstrerror(e) : "cannot connect");
        if (sockfd >= 0) sys_close(sockfd);
        sockfd = -1;
        schedule_retry();
        return;
    }
    nstate = N_CONNECTING;
    since = sys_uptime();
}

/* the TCP connection is made: plain, the login; TLS, the helper with the socket as its descriptor 2 */
static void net_up(void)
{
    if (!use_tls) {
        rfd = wfd = sockfd;
        nstate = N_ON;
        last_rx = sys_uptime();
        irc_login(&S);
        return;
    }
    int in[2] = { -1, -1 }, out[2];
    /* the pipe to the helper never blocks irc: a stalled server fills it, the keys go on */
    if (sys_pipe2(in, O_NONBLOCK) < 0 || sys_pipe(out) < 0) {
        snprintf(why, sizeof why, "no pipes for TLS: %s", zwhy("failed"));
        if (in[0] >= 0) { sys_close(in[0]); sys_close(in[1]); }
        net_close("");
        schedule_retry();
        return;
    }
    char ps[8], *argv[4];
    snprintf(ps, sizeof ps, "%d", port);
    argv[0] = "tlspipe"; argv[1] = host; argv[2] = ps; argv[3] = 0;
    int io[3] = { in[0], out[1], sockfd };
    tlspid = sys_spawn(TLSPIPE, argv, io);
    sys_close(in[0]);
    sys_close(out[1]);
    sys_close(sockfd);                  /* the helper has it now */
    sockfd = -1;
    wfd = in[1];
    rfd = out[0];
    if (tlspid < 0) {
        irc_print(&S, 0, IRC_K_ERR, "-!- cannot start %s: %s (the irc package's TLS helper)", TLSPIPE,
                  zstrerror(tlspid) ? zstrerror(tlspid) : "failed");
        tlspid = -1;
        net_close(0);
        return;
    }
    tlsn = 0;
    nstate = N_TLS;
    irc_print(&S, 0, IRC_K_INFO, "-!- TLS handshake (on a 68020 this takes up to a minute)");
}

/* the helper's first line */
static void tls_line(void)
{
    if (!strncmp(tlsl, "info ", 5)) {     /* a word on the way (the random pool), the handshake goes on */
        irc_print(&S, 0, IRC_K_INFO, "-!- %s", tlsl + 5);
        tlsn = 0;
        return;
    }
    if (!strncmp(tlsl, "ok", 2)) {
        if (!strncmp(tlsl, "ok clock ", 9))
            irc_print(&S, 0, IRC_K_INFO, "-!- the clock is not set: certificate dates checked only against %s",
                      tlsl + 9);
        irc_print(&S, 0, IRC_K_INFO, "-!- TLS: the certificate of %s is good", host);
        nstate = N_ON;
        last_rx = sys_uptime();
        irc_login(&S);
        return;
    }
    snprintf(why, sizeof why, "%s", !strncmp(tlsl, "error ", 6) ? tlsl + 6 : "the TLS helper failed");
    net_close("");
    /* a certificate that is not trusted stays so: no retry for that, a lost network gets one */
    if (strstr(tlsl, "TLS:") && !strstr(tlsl, "stopped answering"))
        nstate = N_OFF;
    else
        schedule_retry();
}

static void net_read(void)
{
    static char b[1024];
    int n = sys_read(rfd, b, sizeof b);
    if (n <= 0 && rfd == sockfd && n < 0 && zerrno == EAGAIN)
        return;
    if (n <= 0) {
        net_close(n < 0 ? zwhy("read error") : "the server closed the connection");
        schedule_retry();
        return;
    }
    last_rx = sys_uptime();
    S.now = sys_time();
    if (nstate == N_TLS) {              /* the helper's line, then the stream */
        int i = 0;
        while (i < n && nstate == N_TLS) {
            char c = b[i++];
            if (c == '\n') {
                tlsl[tlsn] = 0;
                tls_line();
            } else if (tlsn < (int)sizeof tlsl - 1)
                tlsl[tlsn++] = c;
        }
        if (nstate == N_ON && i < n)
            irc_input(&S, b + i, n - i);
        return;
    }
    S.now = sys_time();
    irc_input(&S, b, n);
    if (retries && S.registered)
        retries = 0;
}

static void net_write(void)
{
    u32 t = sys_ticks();
    if (t - token_at >= 200) {          /* a line's allowance back every 2 s */
        int k = (int)((t - token_at) / 200);
        tokens = tokens + k > 5 ? 5 : tokens + k;
        token_at = t;
    }
    while (qsent < qn && tokens > 0) {
        char *e = memchr_(q + qsent, '\n', qn - qsent);
        int len = e ? (int)(e - (q + qsent)) + 1 : qn - qsent;
        int k = rfd == sockfd ? send(wfd, q + qsent, (u32)len, MSG_NOSIGNAL) : sys_write(wfd, q + qsent, (u32)len);
        if (k == -EAGAIN || (k < 0 && rfd == sockfd && zerrno == EAGAIN))
            return;
        if (k <= 0) {
            net_close(zwhy("write error"));
            schedule_retry();
            return;
        }
        qsent += k;
        if (k == len)
            tokens--;
    }
    if (qsent == qn)
        qn = qsent = 0;
}

/* ---- the screen: row 1 the title, rows 2..rows-2 the window's lines, rows-1 the status, rows the input ---- */
#define TOP 2
#define BOT (rows - 2)
#define H (rows - 3)
#define IND 6                           /* "HH:MM " */
static int dirty_all, dirty_status, dirty_input;
static char ed[EDMAX + 1];
static int en, ep;                      /* the input's bytes and the cursor (bytes) */

static const char *kind_sgr(int k)
{
    static const char *const s[] = { "\033[36m", "", "", "\033[1;33m", "\033[35m", "\033[31m", "\033[32m" };
    return k >= 0 && k <= IRC_K_NOTICE ? s[k] : "";
}

static int width(void) { return cols - 1 - IND > 10 ? cols - 1 - IND : 10; }

/* the bytes of the next row from t (n bytes left): whole characters that fit, cut after a space when one is in the
 * row's last third (words are not split) */
static u32 chunk(const char *t, u32 n, int w)
{
    u32 k = zu8fit(t, n, w);
    if (!k && n) {
        int c;
        return (u32)zu8next(t, n, &c);
    }
    if (k < n && t[k] != ' ')
        for (u32 j = k; j > k * 2 / 3; j--)
            if (t[j - 1] == ' ')
                return j;
    return k;
}

/* the rows a line takes */
static int nrows(const struct irc_line *l)
{
    u32 n = (u32)strlen(l->text), off = 0;
    int r = 0, w = width();
    do {
        off += chunk(l->text + off, n - off, w);
        r++;
    } while (off < n);
    return r;
}

/* row r (0-based) of line l at the cursor, then the rest of the row cleared */
static void draw_row(const struct irc_line *l, int r)
{
    const char *t = l->text;
    u32 n = (u32)strlen(t), off = 0, k = 0;
    int w = width();
    for (int i = 0; i <= r; i++) {
        k = chunk(t + off, n - off, w);
        if (i < r) off += k;
    }
    if (r == 0)
        putf("%02u:%02u ", l->t / 3600 % 24, l->t / 60 % 60);
    else
        puts_("      ");
    puts_(kind_sgr(l->kind));
    if (r == 0 && l->nick && l->nick <= k) {          /* the nick in a colour of its own */
        u32 h = 0;
        for (u32 i = 1; i + 1 < l->nick; i++) h = h * 31 + (unsigned char)t[i];
        if (l->kind == IRC_K_OWN) puts_("\033[1m");
        else if (l->kind == IRC_K_MSG) putf("\033[%um", 31 + h % 6);
        putn(t, l->nick);
        puts_("\033[m");
        puts_(kind_sgr(l->kind));
        putn(t + l->nick, (int)(k - l->nick));
    } else
        putn(t + off, (int)k);
    puts_("\033[m\033[K");
}

static const struct irc_line *line_at(const struct irc_win *w, int i)   /* i: 0 the newest */
{
    return i < w->n ? w->ring[(w->head + w->cap - 1 - i) % w->cap] : 0;
}

static void draw_title(void)
{
    struct irc_win *w = S.w[S.cur];
    char b[300];
    snprintf(b, sizeof b, " %s%s%s", S.cur ? w->name : "irc " IRC_VERSION, w->topic ? ": " : "",
             w->topic ? w->topic : S.cur ? "" : "  /help: the commands");
    at(1);
    puts_("\033[7m");
    u32 k = zu8fit(b, (u32)strlen(b), cols - 1);
    putn(b, (int)k);
    for (int c = zu8cols(b, k); c < cols - 1; c++) puts_(" ");
    puts_("\033[m");
}

static void draw_text(void)
{
    struct irc_win *w = S.w[S.cur];
    int row = BOT, i = w->scroll;
    while (row >= TOP) {
        const struct irc_line *l = line_at(w, i++);
        if (!l) {
            for (; row >= TOP; row--) { at(row); puts_("\033[K"); }
            break;
        }
        for (int r = nrows(l) - 1; r >= 0 && row >= TOP; r--, row--) {
            at(row);
            draw_row(l, r);
        }
    }
}

static void draw_status(void)
{
    char b[300];
    int o;
    u32 t = sys_time();
    struct irc_win *w = S.w[S.cur];
    o = snprintf(b, sizeof b, " %02u:%02u  %s  [%d:%s]", t / 3600 % 24, t / 60 % 60, S.me, S.cur + 1,
                 S.cur ? w->name : "status");
    if (w->kind == IRC_W_CHAN)
        o += snprintf(b + o, sizeof b - (u32)o, " %d nicks", w->nn);
    if (w->scroll)
        o += snprintf(b + o, sizeof b - (u32)o, "  -more-");
    if (nstate == N_WAIT)
        o += snprintf(b + o, sizeof b - (u32)o, "  (again in %d s)", (int)(retry_at - sys_uptime()));
    else if (nstate == N_CONNECTING || nstate == N_TLS)
        o += snprintf(b + o, sizeof b - (u32)o, "  (connecting)");
    else if (nstate == N_OFF)
        o += snprintf(b + o, sizeof b - (u32)o, "  (not connected)");
    else if (S.lag > 1)
        o += snprintf(b + o, sizeof b - (u32)o, "  lag %d s", S.lag);
    int first = 1;
    for (int i = 0; i < S.nw && o < (int)sizeof b - 8; i++)
        if (S.w[i]->act) {
            o += snprintf(b + o, sizeof b - (u32)o, "%s%d%s", first ? "  act: " : ",", i + 1,
                          S.w[i]->act == IRC_ACT_HI ? "!" : S.w[i]->act == IRC_ACT_MSG ? "+" : "");
            first = 0;
        }
    at(rows - 1);
    puts_("\033[7m");
    u32 k = zu8fit(b, (u32)strlen(b), cols - 1);
    putn(b, (int)k);
    for (int c = zu8cols(b, k); c < cols - 1; c++) puts_(" ");
    puts_("\033[m");
}

static void draw_input(void)
{
    char p[IRC_NAMEMAX + 4];
    snprintf(p, sizeof p, "[%s] ", S.cur ? S.w[S.cur]->name : "status");
    int pc = zu8cols(p, (u32)strlen(p)), room = cols - 1 - pc;
    if (room < 10) { p[0] = 0; pc = 0; room = cols - 1; }
    /* the part shown: from a character start so that the cursor is inside, at most room columns */
    u32 from = 0;
    while (zu8cols(ed + from, (u32)ep - from) > room - 1 && from < (u32)ep) {
        int c;
        from += (u32)zu8next(ed + from, (u32)en - from, &c);
    }
    at(rows);
    putn(p, (int)strlen(p));
    u32 k = zu8fit(ed + from, (u32)en - from, room);
    putn(ed + from, (int)k);
    puts_("\033[K");
    putf("\033[%d;%dH", rows, pc + zu8cols(ed + from, (u32)ep - from) + 1);
}

static void redraw(void)
{
    int sz = sys_ttyinfo();
    if (sz & 0xFFFF) cols = sz & 0xFFFF;
    if (sz >> 16) rows = sz >> 16;
    if (rows < 6) rows = 6;
    putf("\033[%d;%dr", TOP, BOT);
    draw_title();
    draw_text();
    dirty_status = dirty_input = 1;
    dirty_all = 0;
}

/* the core added a line: drawn at once at the bottom when its window is shown and not scrolled back */
static void shown(void *u, int w, const struct irc_line *l)
{
    (void)u;
    if (w < 0) {
        dirty_all = 1;
        return;
    }
    if (l->kind == IRC_K_HI && S.c.bell && tty)
        puts_("\007");
    dirty_status = 1;
    if (w != S.cur || S.w[w]->scroll || dirty_all || !tty)
        return;
    for (int r = 0, n = nrows(l); r < n; r++) {
        at(BOT);
        puts_("\n\r");
        draw_row(l, r);
    }
    dirty_input = 1;
}

/* ---- the keys ---- */
enum { A_NONE, A_SEND, A_BS, A_DEL, A_LEFT, A_RIGHT, A_HOME, A_END, A_KSTART, A_KEND, A_KWORD, A_HUP, A_HDOWN,
       A_COMPLETE, A_PGUP, A_PGDN, A_LUP, A_LDOWN, A_NEXT, A_PREV, A_WINDOW, A_ACTIVE, A_REDRAW, A_QUIT, A_N };
static const char *const aname[A_N] = { "none", "send", "backspace", "delete", "left", "right", "home", "end",
    "kill_start", "kill_end", "kill_word", "history_up", "history_down", "complete", "page_up", "page_down",
    "line_up", "line_down", "next_window", "prev_window", "window", "active_window", "redraw", "quit" };
static const char defkeys[] =
    "# irc's keys (docs/man/irc.txt): KEY ACTION [ARGUMENT], a key a line; <Name> for <Up> <Down> <Left> <Right>\n"
    "# <S-Up> <C-Up> ... <Return> <Esc> <Tab> <Space> <BS> <Del> <F1>..<F10> <C-x>; \"KEY none\" takes a key away.\n"
    "# Backspace sends <Del> on Zenix's console; the Del key acts as <C-d>, Home and End as <C-a> and <C-e>,\n"
    "# PageUp and PageDown as <S-Up> and <S-Down>.\n"
    "<Return> send\n<Del> backspace\n<BS> backspace\n<C-d> delete\n<Left> left\n<C-b> left\n<Right> right\n"
    "<C-f> right\n<C-a> home\n<C-e> end\n<C-u> kill_start\n<C-k> kill_end\n<C-w> kill_word\n<Up> history_up\n"
    "<Down> history_down\n<Tab> complete\n<S-Up> page_up\n<S-Down> page_down\n<C-Up> line_up\n<C-Down> line_down\n"
    "<C-n> next_window\n<C-p> prev_window\n<S-Right> next_window\n<S-Left> prev_window\n<C-x> active_window\n"
    "<F1> window 1\n<F2> window 2\n<F3> window 3\n<F4> window 4\n<F5> window 5\n<F6> window 6\n<F7> window 7\n"
    "<F8> window 8\n<F9> window 9\n<F10> window 10\n<C-l> redraw\n";
static struct kbind kb[80];
static struct keyset ks = { aname, A_N, kb, 0, 80, "" };

static int getb(int ms)
{
    unsigned char c;
    struct pollfd pf = { 0, POLLIN, 0 };
    if (poll(&pf, 1, ms) <= 0 || sys_read(0, &c, 1) != 1) return -1;
    return c;
}

/* a key: a character (a byte of UTF-8 text: 0x10000 + the byte), or the shared parser's code for a sequence */
static int read_key(void)
{
    unsigned char c;
    if (sys_read(0, &c, 1) != 1) return -1;
    if (c == 10) return 13;
    if (c >= 0x80) return 0x10000 | c;
    if (c != 27) return c;
    int s1 = getb(30), ch = 0, num = 0, mod = 0, semi = 0;
    if (s1 < 0) return 27;
    if (s1 == 'O') { ch = getb(30); return ch >= 'P' && ch <= 'S' ? KM_F1 + ch - 'P' : ch == 'A' ? KM_UP :
                     ch == 'B' ? KM_DOWN : ch == 'C' ? KM_RIGHT : ch == 'D' ? KM_LEFT : ch == 'H' ? 1 : ch == 'F' ? 5 : 0; }
    if (s1 != '[') return 0;
    while ((ch = getb(30)) >= 0 && ((ch >= '0' && ch <= '9') || ch == ';')) {
        if (ch == ';') semi = 1;
        else if (semi) mod = mod * 10 + ch - '0';
        else num = num * 10 + ch - '0';
    }
    if (ch >= 'A' && ch <= 'D') {       /* xterm's modified arrows: ESC [ 1 ; m A (2 Shift, 5 Ctrl) */
        int b = mod == 2 ? KM_SUP : mod >= 5 ? KM_CUP : KM_UP;
        static const unsigned char o[4] = { 0, 1, 3, 2 };   /* A up, B down, C right, D left */
        return b + o[ch - 'A'];
    }
    switch (ch) {
    case 'H': return 1;                 /* Home */
    case 'F': return 5;                 /* End */
    case 'Z': return 0;                 /* Shift-Tab */
    case 'P': case 'Q': case 'R': case 'S': return KM_F1 + ch - 'P';
    case '~':
        if (num == 1 || num == 7) return 1;
        if (num == 4 || num == 8) return 5;
        if (num == 3) return 4;         /* Del: as Ctrl-D */
        if (num == 5) return KM_SUP;
        if (num == 6) return KM_SDOWN;
        if (num == 2) return KM_HELP;   /* Help (Insert) */
        return num >= 11 && num <= 15 ? KM_F1 + num - 11 : num >= 17 && num <= 21 ? KM_F1 + num - 12 : 0;
    }
    return 0;
}

/* ---- the input line: editing, history, nick completion ---- */
static char *hist[HISTN];
static int nh, hpos = -1;
static char comp_pre[IRC_NAMEMAX];
static int comp_n = -1, comp_start, comp_len;

static void ed_set(const char *t)
{
    en = (int)strlen(t);
    if (en > EDMAX) en = EDMAX;
    memcpy(ed, t, (u32)en);
    ed[en] = 0;
    ep = en;
}

static void ed_insert(const char *t, int n)
{
    if (en + n > EDMAX) {
        puts_("\007");
        return;
    }
    memmove(ed + ep + n, ed + ep, (u32)(en - ep));
    memcpy(ed + ep, t, (u32)n);
    en += n;
    ep += n;
    ed[en] = 0;
}

static void ed_cut(int from, int to)
{
    memmove(ed + from, ed + to, (u32)(en - to));
    en -= to - from;
    ep = from;
    ed[en] = 0;
}

static int prevc(int p) { return p > 0 ? (int)zu8prev(ed, (u32)p) : 0; }
static int nextc(int p) { int c; return p < en ? p + zu8next(ed + p, (u32)(en - p), &c) : en; }

static void complete(void)
{
    if (comp_n < 0) {                   /* a new completion: the word before the cursor */
        int s = ep;
        while (s > 0 && ed[s - 1] != ' ') s--;
        if (ep - s <= 0 || ep - s >= (int)sizeof comp_pre) return;
        memcpy(comp_pre, ed + s, (u32)(ep - s));
        comp_pre[ep - s] = 0;
        comp_start = s;
        comp_len = ep - s;
        comp_n = 0;
    } else
        comp_n++;
    const char *m = irc_nick_match(&S, S.cur, comp_pre, (int)strlen(comp_pre), comp_n);
    if (!m && comp_n) {
        comp_n = 0;                     /* round again */
        m = irc_nick_match(&S, S.cur, comp_pre, (int)strlen(comp_pre), 0);
    }
    if (!m) { comp_n = -1; puts_("\007"); return; }
    char t[IRC_NAMEMAX + 3];
    snprintf(t, sizeof t, "%s%s", m, comp_start == 0 ? ": " : " ");
    ed_cut(comp_start, comp_start + comp_len);
    ed_insert(t, (int)strlen(t));
    comp_len = (int)strlen(t);
}

static void scroll_by(int d)
{
    struct irc_win *w = S.w[S.cur];
    int s = w->scroll + d;
    if (s > w->n - 1) s = w->n - 1;
    if (s < 0) s = 0;
    if (s != w->scroll) {
        w->scroll = s;
        draw_text();
        dirty_status = 1;
    }
}

static void submit(void)
{
    if (!en) return;
    if (!nh || strcmp(hist[(nh - 1) % HISTN], ed)) {
        char *c = strdup(ed);
        if (c) {
            free(hist[nh % HISTN]);
            hist[nh % HISTN] = c;
            nh++;
        }
    }
    hpos = -1;
    char *t = strdup(ed);
    ed_set("");
    if (!t) return;
    S.w[S.cur]->scroll = 0;
    S.now = sys_time();
    irc_command(&S, t);
    free(t);
}

static void act(int a, const char *arg)
{
    if (a != A_COMPLETE) comp_n = -1;
    switch (a) {
    case A_SEND: submit(); break;
    case A_BS: if (ep > 0) ed_cut(prevc(ep), ep); break;
    case A_DEL: if (ep < en) { int e = nextc(ep); int p = ep; ed_cut(p, e); } break;
    case A_LEFT: ep = prevc(ep); break;
    case A_RIGHT: ep = nextc(ep); break;
    case A_HOME: ep = 0; break;
    case A_END: ep = en; break;
    case A_KSTART: ed_cut(0, ep); break;
    case A_KEND: en = ep; ed[en] = 0; break;
    case A_KWORD: { int s = ep; while (s > 0 && ed[s - 1] == ' ') s--; while (s > 0 && ed[s - 1] != ' ') s--;
                    ed_cut(s, ep); break; }
    case A_HUP: case A_HDOWN: {
        int lo = nh > HISTN ? nh - HISTN : 0;
        if (a == A_HUP) hpos = hpos < 0 ? nh - 1 : hpos > lo ? hpos - 1 : hpos;
        else hpos = hpos < 0 ? -1 : hpos + 1 < nh ? hpos + 1 : -1;
        ed_set(hpos >= 0 && hpos >= lo ? hist[hpos % HISTN] : "");
        break;
    }
    case A_COMPLETE: complete(); break;
    case A_PGUP: scroll_by(H - 2); break;
    case A_PGDN: scroll_by(-(H - 2)); break;
    case A_LUP: scroll_by(1); break;
    case A_LDOWN: scroll_by(-1); break;
    case A_NEXT: irc_select(&S, (S.cur + 1) % S.nw); break;
    case A_PREV: irc_select(&S, (S.cur + S.nw - 1) % S.nw); break;
    case A_WINDOW: { int n = atoi(arg); if (n >= 1 && n <= S.nw) irc_select(&S, n - 1); break; }
    case A_ACTIVE: {                    /* the next window with something new, highlights first */
        int best = -1;
        for (int k = 1; k < S.nw; k++) {
            int i = (S.cur + k) % S.nw;
            if (S.w[i]->act && (best < 0 || S.w[i]->act > S.w[best]->act)) best = i;
        }
        if (best >= 0) irc_select(&S, best);
        break;
    }
    case A_REDRAW: dirty_all = 1; break;
    case A_QUIT: irc_command(&S, "/quit"); break;
    }
    dirty_input = 1;
}

static void keys_in(void)
{
    int k = read_key();
    if (k < 0) { quit = 1; return; }
    if (k & 0x10000) {                  /* text: a whole UTF-8 character */
        char t[4];
        int n = 1, need = (k & 0xE0) == 0xC0 ? 2 : (k & 0xF0) == 0xE0 ? 3 : (k & 0xF8) == 0xF0 ? 4 : 1;
        t[0] = (char)k;
        while (n < need) { int b = getb(30); if (b < 0 || (b & 0xC0) != 0x80) break; t[n++] = (char)b; }
        char c[8];                      /* a whole, valid character, not a C1 control: else nothing */
        if (n != need || need == 1 || irc_utf8(t, n, c, sizeof c) != n || memcmp(c, t, (u32)n)) return;
        comp_n = -1;
        ed_insert(t, n);
        dirty_input = 1;
        return;
    }
    int k2 = 0;
    if (keys_prefix(&ks, k)) {
        struct pollfd pf = { 0, POLLIN, 0 };
        if (poll(&pf, 1, 2000) <= 0 || (k2 = read_key()) < 0) return;
    }
    const struct kbind *b = keys_find(&ks, k, k2);
    if (b && b->act) { act(b->act, b->arg); return; }
    if (k >= 32 && k < 127) {
        char c = (char)k;
        comp_n = -1;
        ed_insert(&c, 1);
        dirty_input = 1;
    }
}

/* ---- the settings ---- */
static const char defconf[] =
    "# irc's settings (docs/man/irc.txt): a setting and its value a line; # a comment.\n"
    "# server irc.libera.chat\n# port 6697\n# tls on\n# nick yournick\n# user yourname\n# realname Your Name\n"
    "# password the server's password\n# sasl ACCOUNT PASSWORD\n# autojoin #channel #another\n"
    "# highlight word another\n# charset utf-8\n# quit Bye\n# scrollback 300\n# reconnect on\n# bell on\n";

static char *conf_read(const char *path, int must, char *msg, int mn)
{
    int fd = sys_open(path, O_RDONLY);
    struct ustat st;
    char *t;
    if (fd < 0) {
        if (must) { snprintf(msg, (u32)mn, "%s: %s", path, zstrerror(fd) ? zstrerror(fd) : "cannot open"); return 0; }
        return strdup("");
    }
    if (sys_fstat(fd, &st) < 0 || st.size > CONFMAX || !(t = malloc(st.size + 1))) {
        snprintf(msg, (u32)mn, "%s: too big (%d bytes at most)", path, CONFMAX);
        sys_close(fd);
        return 0;
    }
    int n = sys_read(fd, t, st.size);
    t[n > 0 ? n : 0] = 0;
    if ((st.mode & 077) && (!strncmp(t, "password", 8) || !strncmp(t, "sasl", 4) || strstr(t, "\npassword") ||
                            strstr(t, "\nsasl")))
        dprintf(2, "irc: %s holds a password and others may read it: chmod 600 %s\n", path, path);
    sys_close(fd);
    return t;
}

static void usage(void)
{
    dprintf(2, "usage: irc [-c FILE] [-n NICK] [-p PORT] [-t] [SERVER [#CHANNEL...]]\n       irc -K\n");
    sys_exit(2);
}

static void on_winch(int sig) { (void)sig; winch = 1; }

int main(int argc, char **argv)
{
    const char *cfile = 0, *onick = 0, *oserver = 0;
    int oport = 0, otls = 0, i;
    char msg[160], path[ZPATH_MAX];
    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        const char *o = argv[i];
        if (!strcmp(o, "-K")) { sys_write(1, defkeys, sizeof defkeys - 1); return 0; }
        else if (!strcmp(o, "-t")) otls = 1;
        else if (!strcmp(o, "-c") && i + 1 < argc) cfile = argv[++i];
        else if (!strcmp(o, "-n") && i + 1 < argc) onick = argv[++i];
        else if (!strcmp(o, "-p") && i + 1 < argc) { oport = atoi(argv[++i]); if (oport <= 0 || oport > 65535) usage(); }
        else usage();
    }
    if (i < argc) oserver = argv[i++];
    const char *home = getenv("HOME");
    if (!cfile && home && strlen(home) < 200) {
        snprintf(path, sizeof path, "%s/.config/irc/config", home);
        cfile = path;
        struct ustat st;
        if (sys_stat(path, &st) < 0) {  /* none: a commented one to fill in, readable by its owner only */
            char d[ZPATH_MAX];
            snprintf(d, sizeof d, "%s/.config", home);
            sys_mkdir(d);
            strcat(d, "/irc");
            sys_mkdir(d);
            int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd >= 0) {
                sys_write(fd, defconf, sizeof defconf - 1);
                sys_close(fd);
                sys_chmod(path, 0600);
            }
        }
    }
    char *text = cfile ? conf_read(cfile, cfile != path, msg, sizeof msg) : strdup("");
    if (!text) { dprintf(2, "irc: %s\n", msg); return 1; }
    if (irc_config(&S, text, msg, sizeof msg) < 0) { dprintf(2, "irc: %s: %s\n", cfile, msg); return 2; }
    if (oserver) { S.c.server = (char *)oserver; S.c.port = 0; S.c.tls = 0; }
    if (otls) S.c.tls = 1;
    if (oport) S.c.port = oport;
    if (onick) S.c.nick = (char *)onick;
    if (oserver && i < argc) {          /* the channels on the command line instead of autojoin */
        S.c.nautojoin = 0;
        for (; i < argc && S.c.nautojoin < IRC_WORDS; i++) S.c.autojoin[S.c.nautojoin++] = argv[i];
    }
    if (!S.c.server) {
        dprintf(2, "irc: no server: irc SERVER, or \"server NAME\" in %s\n", cfile ? cfile : "~/.config/irc/config");
        return 2;
    }
    if (!onick && (!S.c.nick || !*S.c.nick)) {
        const char *u = getenv("USER");
        if (u && *u) S.c.nick = (char *)u;
    }
    S.send = qsend;
    S.shown = shown;
    S.now = sys_time();
    irc_init(&S);
    keys_load(&ks, defkeys, sizeof defkeys - 1, "irc");
    tty = sys_isatty(0) && sys_isatty(1);
    if (!tty) { dprintf(2, "irc: the keys and the screen must be a terminal\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGWINCH, on_winch);
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    puts_("\033[2J");
    redraw();
    if (ks.bad[0]) irc_print(&S, 0, IRC_K_ERR, "-!- %s", ks.bad);
    irc_print(&S, 0, IRC_K_INFO, "-!- irc " IRC_VERSION ": /help for the commands; F1-F10 or Ctrl-N/Ctrl-P for the windows");
    snprintf(host, sizeof host, "%s", S.c.server);
    port = S.c.port;
    use_tls = S.c.tls;
    net_open();
    u32 last_sec = 0;
    while (!quit) {
        u32 up = sys_uptime();
        S.now = sys_time();
        if (S.want == IRC_WANT_QUIT) {  /* the QUIT out first (the server ends the connection) */
            S.want = 0;
            quit = 2;
            u32 t0 = sys_ticks();
            while (qn && nstate == N_ON && sys_ticks() - t0 < 300) { tokens = 5; net_write(); }
            break;
        }
        if (S.want == IRC_WANT_CONNECT) {
            S.want = 0;
            if (nstate != N_OFF && nstate != N_WAIT) {
                irc_command(&S, "/quote QUIT :changing servers");
                tokens = 5;
                net_write();
                net_close("changing servers");
            }
            snprintf(host, sizeof host, "%s", S.want_host);
            port = S.want_port;
            use_tls = S.want_tls;
            retries = 0;
            net_open();
        }
        S.want = 0;
        if (nstate == N_WAIT && (int)(up - retry_at) >= 0)
            net_open();
        if ((nstate == N_CONNECTING || nstate == N_TLS) && (int)(up - since) > CONNECT_SECS + (nstate == N_TLS ? 120 : 0)) {
            snprintf(why, sizeof why, "no answer in time");
            net_close("no answer in time");
            schedule_retry();
        }
        if (nstate == N_ON && (int)(up - last_rx) > DEAD_IDLE) {
            net_close("the server stopped answering");
            schedule_retry();
        } else if (nstate == N_ON && (int)(up - last_rx) > PING_IDLE && !S.ping_sent)
            irc_keepalive(&S);
        if (winch) { winch = 0; puts_("\033[2J"); dirty_all = 1; }
        if (up != last_sec) { last_sec = up; if (S.now % 60 == 0 || nstate == N_WAIT) dirty_status = 1; }
        if (dirty_all) redraw();
        if (dirty_status) { draw_title(); draw_status(); dirty_status = 0; dirty_input = 1; }
        if (dirty_input) { draw_input(); dirty_input = 0; }
        flush();
        struct pollfd p[4];
        int n = 0, ik = -1, ir = -1, iw = -1, iw2 = -1;
        ik = n; p[n].fd = 0; p[n].events = POLLIN; p[n++].revents = 0;
        if (nstate == N_CONNECTING) { iw = n; p[n].fd = sockfd; p[n].events = POLLOUT; p[n++].revents = 0; }
        if ((nstate == N_ON || nstate == N_TLS) && rfd >= 0) { ir = n; p[n].fd = rfd; p[n].events = POLLIN; p[n++].revents = 0; }
        if (nstate == N_ON && qn > qsent && tokens > 0) {     /* room to write: the socket, or the helper's pipe */
            if (wfd == rfd) p[ir].events |= POLLOUT;
            else { iw2 = n; p[n].fd = wfd; p[n].events = POLLOUT; p[n++].revents = 0; }
        }
        int wait = nstate == N_ON && qn > qsent && tokens <= 0 ? 200 : 1000;
        if (poll(p, (u32)n, wait) < 0) continue;
        if (iw >= 0 && p[iw].revents) {
            int e = 0;
            socklen_t el = sizeof e;
            if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &e, &el) < 0) e = zerrno;
            if (e) {
                snprintf(why, sizeof why, "%s port %d: %s", host, port, zstrerror(e) ? zstrerror(e) : "cannot connect");
                irc_print(&S, 0, IRC_K_ERR, "-!- %s", why);
                why[0] = 0;
                nstate = N_OFF;
                net_close(0);
                schedule_retry();
            } else
                net_up();
        }
        if (ir >= 0 && (p[ir].revents & (POLLIN | POLLHUP | POLLERR)) && rfd >= 0) net_read();
        if (nstate == N_ON && qn > qsent && (iw2 < 0 || p[iw2].revents || tokens <= 0)) net_write();
        if (p[ik].revents) keys_in();
    }
    S.now = sys_time();
    if (nstate != N_OFF) net_close(0);
    puts_("\033[r\033[2J\033[H");
    flush();
    sys_ttymode(0);
    if (quit == 2) dprintf(1, "irc: %s\n", S.quit);
    irc_free(&S);
    return 0;
}
