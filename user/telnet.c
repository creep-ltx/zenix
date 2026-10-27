/* SPDX-License-Identifier: GPL-2.0-only */
/* telnet HOST [PORT]: a terminal to another computer over the network (DESIGN D20, docs/man/telnet.txt). Three modes
 * (D20 3.5): an Amiga board (-a, the default: Latin-1 in the console's Topaz, 0x9B as CSI and the Amiga console's own
 * codes), a PC board (-p: CP437 through the PC font, /usr/share/consolefonts/cp437-8x8.psf, loaded with SYS_console
 * CON_FONT) and a Unix host (-x, xterm-like). One process, one loop: poll on the socket and the keys (D16); the
 * protocol, the stream filter and the key translation are telnet's core (net/telnet/, shared with the PC tests). The
 * console draws, with the mode's rules (CON_RULES on standard output, D20 4.3); when the console refuses the rules or
 * the font (standard output is a file, a pipe; no memory), telnet says so once and goes on: in the PC mode CP437 is
 * then shown as its nearest Latin-1 characters. Ctrl-] opens the local prompt.
 * The SLIP plan (D20 section 6): the screen is written at most 256 bytes a call and telnet runs one nice level lower
 * than it was started at, so a woken netd (nice 0) takes the CPU at the next return to user mode. */
#include "socket.h"
#include "../net/telnet/telnet.h"
#include "xfer/xfer.h"

static int sock = -1, eof_keys, quit, rules_ok, rules_old, raw_on, echo_note, font_note, font_on, ice_on, ice_opt;
static int esc_key = 0x1D;                      /* Ctrl-] */
static int rawtcp;
static const char *host, *dldir;
static struct tn tn;
static struct tf tf;

static void sendall(void *u, const unsigned char *b, long n)
{
    (void)u;
    while (n > 0) {
        int w = send(sock, b, (u32)n, 0);
        if (w <= 0) { quit = 1; return; }       /* the connection is gone: the loop ends */
        b += w;
        n -= w;
    }
}

static void screen(void *u, const unsigned char *b, long n)
{
    (void)u;
    while (n > 0) {                             /* at most 256 bytes a call (the SLIP plan) */
        long k = n > 256 ? 256 : n;
        sys_write(1, b, (u32)k);
        b += k;
        n -= k;
    }
}

static void answer(void *u, const unsigned char *b, long n)   /* the filter's answers to the other side */
{
    unsigned char e[128];
    while (n > 0) {
        long k = n > 64 ? 64 : n;
        sendall(u, e, tn_encode(&tn, b, k, e));
        b += k;
        n -= k;
    }
}

/* the console's rules for each mode (D20 3.5), and the bits telnet changes (the rest are left as they were) */
#define RULES_MINE (CONR_PCCTRL | CONR_WRAPNOW | CONR_CLRHOME | CONR_BRIGHT | CONR_ICE | CONR_CGAPAL | CONR_LFONLY)
static u32 mode_rules(int m)
{
    if (m == TN_UNIX)
        return CONR_LFONLY;
    u32 r = CONR_WRAPNOW | CONR_CLRHOME | CONR_BRIGHT | CONR_CGAPAL | (ice_on ? CONR_ICE : 0);
    return m == TN_PC ? r | CONR_PCCTRL | CONR_LFONLY : r;
}

#define PCFONT "/usr/share/consolefonts/cp437-8x8.psf"
static unsigned char psf[2052 + 1];

/* the PC font on the console (PC mode) or not (the others); refused: the PC mode shows CP437 as Latin-1. Dropped
 * by telnet itself (the end, z, the prompt) the screen is cleared first: the board's bytes would be junk in Topaz
 * (4.2); dropped at the board's own font switch, the screen is left as the board draws it next (clear 0) */
static int drop_clears = 1;
static void font_set(int want)
{
    if (!want) {
        if (font_on) {
            if (drop_clears)
                sys_write(1, "\033[2J\033[H", 7);
            sys_confont(1, 0, 0);
        }
        font_on = 0;
        tf.fallback = 0;
        return;
    }
    if (font_on)
        return;
    int r = -ENOENT, fd = sys_open(PCFONT, O_RDONLY);
    if (fd >= 0) {
        int n = sys_read(fd, psf, sizeof psf);
        sys_close(fd);
        r = n < 0 ? n : sys_confont(1, psf, (u32)n);
    }
    font_on = r >= 0;
    tf.fallback = !font_on;
    if (!font_on && !font_note++)
        fprintf(2, "telnet: the PC font is not available here (%s): CP437 is shown as its nearest Latin-1 characters\r\n",
                zstrerror(r));
}

/* the console as the mode wants it: rules and font; the bits before are kept for the end */
static void mode_apply(void)
{
    int r = sys_conrules(1, mode_rules(tn.mode), RULES_MINE);
    if (r >= 0 && !rules_ok)
        rules_old = r;
    rules_ok = r >= 0;
    if (!rules_ok && !echo_note++)
        fprintf(2, "telnet: the console's rules are not available here (%s): the screen is drawn without them\r\n",
                zstrerror(r));
    font_set(tn.mode == TN_PC);
}

/* the console: raw keys, the mode's rules and font; undone on the way out and at a suspend */
static void term_on(void)
{
    if (sys_isatty(0)) {
        sys_ttymode(TTY_RAW | TTY_NOSIG);       /* every key, Ctrl-C included, goes to the other side */
        raw_on = 1;
    }
    mode_apply();
}

static void term_off(void)
{
    font_set(0);
    if (rules_ok)
        sys_conrules(1, (u32)rules_old & RULES_MINE, RULES_MINE);
    rules_ok = 0;
    if (raw_on)
        sys_ttymode(0);
    raw_on = 0;
}

/* a mode chosen at the prompt (the screen cleared first) or by the board's font switch (the filter switched itself) */
static void mode_to(int m, int from_board)
{
    if (!from_board) {
        if (m == tn.mode)
            return;
        tf_flush(&tf);
        tf_setmode(&tf, m);
        if (!font_on)
            sys_write(1, "\033[2J\033[H", 7);
    }
    tn.mode = m;                                /* the keys follow (the terminal type was told once, at the start) */
    drop_clears = !from_board;
    mode_apply();
    drop_clears = 1;
}

/* the filter's events: iCE switched by the board (CSI ? 33 h/l), the board's font switch */
static void tf_event(void *u, int ev, int arg)
{
    (void)u;
    if (ev == TF_EV_ICE) {
        ice_on = arg || ice_opt;
        if (rules_ok)
            sys_conrules(1, ice_on ? CONR_ICE : 0, CONR_ICE);
    } else if (ev == TF_EV_MODE)
        mode_to(arg, 1);
}

static void keys_out(const unsigned char *k, long n)
{
    unsigned char t[128], e[256];
    int bell = 0;
    while (n > 0) {
        long c = n > 64 ? 64 : n;
        long m = tk_keys(tn.mode, k, c, t, &bell);
        if (tn_local_echo(&tn))
            screen(0, t, m);
        sendall(0, e, tn_encode(&tn, t, m, e));
        k += c;
        n -= c;
    }
    if (bell)
        sys_write(1, "\007", 1);
}

/* ---- Zmodem (DESIGN D20 5): rz or sz started when the board starts a transfer, the socket relayed to it ---- */

static int bottom_row(void)
{
    int rows = sys_ttyinfo() >> 16;
    return rows ? rows : 25;
}

/* a line of telnet's own on the bottom row (never its last column), the board's cursor kept */
static void bottom(const char *t)
{
    char line[120];
    int n = snprintf(line, sizeof line, "\0337\033[%d;1H\033[m\033[2K%.78s\0338", bottom_row(), t);
    sys_write(1, line, (u32)n);
}

/* a line typed on the bottom row (Return ends it; Esc, Ctrl-C or Ctrl-X cancel: -1) */
static int ask_line(const char *q, char *buf, int max)
{
    int n = 0;
    char shown[120];
    buf[0] = 0;
    for (;;) {
        snprintf(shown, sizeof shown, "%s%s", q, n > 70 - (int)strlen(q) ? buf + n - (70 - (int)strlen(q)) : buf);
        bottom(shown);
        unsigned char k[64];
        int r = sys_read(0, k, sizeof k);
        if (r <= 0)
            return -1;
        for (int i = 0; i < r; i++) {
            unsigned char c = k[i];
            if (c == '\r' || c == '\n') {
                bottom("");
                return n;
            }
            if (c == 0x1B || c == 0x03 || c == 0x18) {
                bottom("");
                return -1;
            }
            if ((c == 0x7F || c == 0x08) && n > 0)
                buf[--n] = 0;
            else if (c >= 0x20 && c != 0x7F && n < max - 1) {
                buf[n++] = (char)c;
                buf[n] = 0;
            }
        }
    }
}

/* the data coding both ways (none with -r) */
static long dec(const unsigned char *in, long n, unsigned char *out)
{
    if (rawtcp) { memcpy(out, in, (u32)n); return n; }
    return tn_decode(&tn, in, n, out);
}

static void to_board(const unsigned char *b, long n)
{
    static unsigned char e[2 * 512];
    while (n > 0) {
        long k = n > 512 ? 512 : n;
        if (rawtcp)
            sendall(0, b, k);
        else
            sendall(0, e, tn_encode(&tn, b, k, e));
        b += k;
        n -= k;
    }
}

static void board_cancel(void)                  /* Zmodem's give-up: ten CANs and ten backspaces */
{
    static const unsigned char c[20] = { 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8 };
    to_board(c, sizeof c);
}

/* rz/sz's standard error in its -T form: "P text" progress (the bottom row), "M text" messages (a line on the screen),
 * "R n" and n bytes that came after the session (the board's prompt: to the screen) */
static struct { char line[160]; int n; long rest; } ep;
static void err_bytes(const unsigned char *b, long n)
{
    while (n > 0) {
        if (ep.rest > 0) {
            long k = n < ep.rest ? n : ep.rest;
            tf_write(&tf, b, k);
            ep.rest -= k;
            b += k;
            n -= k;
            continue;
        }
        unsigned char c = *b++;
        n--;
        if (c != '\n') {
            if (ep.n < (int)sizeof ep.line - 1)
                ep.line[ep.n++] = (char)c;
            continue;
        }
        ep.line[ep.n] = 0;
        ep.n = 0;
        char *t = ep.line + (ep.line[0] && ep.line[1] == ' ' ? 2 : 0);
        for (char *q = t; *q; q++)
            if ((unsigned char)*q < 0x20)
                *q = '?';                       /* only text of our own reaches the console */
        if (ep.line[0] == 'P') {
            char l[120];
            snprintf(l, sizeof l, "Zmodem: %s", t);
            bottom(l);
        } else if (ep.line[0] == 'R')
            ep.rest = atoi(t);
        else
            printf("\r\033[K%s: %s\r\n", ep.line[0] == 'M' ? "Zmodem" : "rz", t);
    }
}

static void xfer_menu(int *cancel)
{
    bottom("telnet> transfer: c cancel it, Return go on: ");
    char c = 0;
    if (sys_read(0, &c, 1) == 1 && (c == 'c' || c == 'q'))
        *cancel = 1;
    bottom("");
}

/* After a cancel the board's sender has written ahead (its socket buffers hold up to hundreds of KB of the file):
 * that is dropped, not drawn. Until a second without bytes (at most a minute), only what came after the last ZDLE
 * (0x18, in every Zmodem frame and never in a board's text) is kept: the board's prompt, shown then; 1 KB of text
 * with no ZDLE in it ends the wait at once. */
static void drain(void)
{
    static unsigned char raw[1024], data[1024], text[1024];
    long tn_ = 0;
    u32 t0 = sys_ticks();
    printf("\r\033[KZmodem: dropping the rest of the cancelled transfer...\r\n");
    while (sys_ticks() - t0 < 6000 && !quit) {
        struct pollfd p = { sock, POLLIN, 0 };
        if (poll(&p, 1, 1000) <= 0)
            break;
        int k = recv(sock, raw, sizeof raw, 0);
        if (k <= 0) { quit = 1; break; }
        long m = dec(raw, k, data);
        for (long i = 0; i < m; i++) {
            if (data[i] == 0x18)
                tn_ = 0;
            else if (tn_ < (long)sizeof text)
                text[tn_++] = data[i];
        }
        if (tn_ == (long)sizeof text)
            break;
    }
    tf_write(&tf, text, tn_);
}

/* run rz (kind 1: the board sends) or sz (kind 2: the board receives files) with the bytes that came with the start;
 * the socket relayed to it through the telnet coding until it ends. Keys: Ctrl-X five times or the escape key's
 * prompt cancel it. */
static void transfer(int kind, const unsigned char *pre, long npre, char **files, int nfiles)
{
    static unsigned char pend[2048], buf[1024];
    long np = 0, pp = 0;
    int in[2] = { -1, -1 }, out[2] = { -1, -1 }, err[2] = { -1, -1 };
    if (npre > (long)sizeof pend)
        npre = sizeof pend;                     /* (never: a read is 1 KB) */
    memcpy(pend, pre, (u32)npre);
    np = npre;
    if (sys_pipe2(in, O_NONBLOCK) < 0 || sys_pipe(out) < 0 || sys_pipe(err) < 0) {
        printf("\r\ntelnet: no pipes for the transfer: %s\r\n", zwhy("failed"));
        board_cancel();
        goto close;
    }
    char *argv[40];
    int a = 0;
    argv[a++] = kind == 1 ? "rz" : "sz";
    argv[a++] = "-T";
    if (!rawtcp && !tn_on(&tn, kind == 1 ? 1 : 0, TO_BINARY))
        argv[a++] = "-e";                       /* no BINARY that way: every control escaped (CR NUL folding) */
    if (kind == 1 && dldir) {
        argv[a++] = "-d";
        argv[a++] = (char *)dldir;
    }
    if (kind == 2) {
        argv[a++] = "--";
        for (int i = 0; i < nfiles && a < 39; i++)
            argv[a++] = files[i];
    }
    argv[a] = 0;
    int io[3] = { in[0], out[1], err[1] };
    int pid = sys_spawn("/bin/rz", argv, io);
    sys_close(in[0]); sys_close(out[1]); sys_close(err[1]);
    in[0] = out[1] = err[1] = -1;
    if (pid < 0) {
        printf("\r\ntelnet: cannot start %s: %s\r\n", argv[0], zwhy("failed"));
        board_cancel();
        goto close;
    }
    memset(&ep, 0, sizeof ep);
    int out_open = 1, err_open = 1, sock_ok = 1, cx = 0, cancel = 0, killed = 0;
    u32 cancel_at = 0;
    while (out_open || err_open) {
        struct pollfd p[5];
        int n = 0, io_ = -1, ie = -1, is = -1, iw = -1, ik = -1;
        if (out_open) { io_ = n; p[n].fd = out[0]; p[n].events = POLLIN; p[n++].revents = 0; }
        if (err_open) { ie = n; p[n].fd = err[0]; p[n].events = POLLIN; p[n++].revents = 0; }
        if (sock_ok && pp >= np) { is = n; p[n].fd = sock; p[n].events = POLLIN; p[n++].revents = 0; }
        if (in[1] >= 0 && pp < np) { iw = n; p[n].fd = in[1]; p[n].events = POLLOUT; p[n++].revents = 0; }
        if (!eof_keys) { ik = n; p[n].fd = 0; p[n].events = POLLIN; p[n++].revents = 0; }
        if (poll(p, (u32)n, cancel ? 200 : -1) < 0)
            continue;
        if (io_ >= 0 && p[io_].revents) {       /* the program's bytes to the board */
            int k = sys_read(out[0], buf, sizeof buf);
            if (k <= 0) out_open = 0;
            else to_board(buf, k);
        }
        if (ie >= 0 && p[ie].revents) {
            int k = sys_read(err[0], buf, sizeof buf);
            if (k <= 0) err_open = 0;
            else err_bytes(buf, k);
        }
        if (is >= 0 && p[is].revents) {         /* the board's bytes to the program */
            int k = recv(sock, buf, sizeof buf, 0);
            if (k <= 0) {
                sock_ok = 0;                    /* the board is gone: the program sees the end of its input */
                if (in[1] >= 0) { sys_close(in[1]); in[1] = -1; }
                quit = 1;
            } else {
                np = dec(buf, k, pend);
                pp = 0;
            }
        }
        if (iw >= 0 && p[iw].revents) {
            int w = sys_write(in[1], pend + pp, (u32)(np - pp));
            if (w > 0)
                pp += w;
            else if (w != -EAGAIN && w != -EINTR) {
                sys_close(in[1]);               /* the program has ended: what is left goes to the screen below */
                in[1] = -1;
            }
        }
        if (ik >= 0 && p[ik].revents) {
            unsigned char k[64];
            int r = sys_read(0, k, sizeof k);
            if (r <= 0)
                eof_keys = 1;
            for (int i = 0; i < r; i++) {
                if (k[i] == 0x18) {
                    if (++cx >= 5) cancel = 1;
                } else {
                    cx = 0;
                    if (esc_key >= 0 && k[i] == esc_key)
                        xfer_menu(&cancel);
                }
            }
        }
        if (cancel && !killed) {
            killed = 1;
            cancel_at = sys_ticks();
            sys_killsig(pid, SIGTERM);          /* the program cancels (Zmodem's ten CANs) and keeps what came */
        } else if (killed == 1 && sys_ticks() - cancel_at > 500) {
            killed = 2;
            sys_kill(pid);                      /* it did not end within 5 s: ended, and the board told here */
            board_cancel();
        }
    }
    sys_wait(pid);
    bottom("");
    if (cancel)
        drain();                                /* the rest of the cancelled transfer: not for the screen */
    else if (pp < np && in[1] < 0)
        tf_write(&tf, pend + pp, np - pp);      /* came after the program's end: the board's screen */
    tf_flush(&tf);
close:
    for (int i = 0; i < 2; i++) {
        if (in[i] >= 0) sys_close(in[i]);
        if (out[i] >= 0) sys_close(out[i]);
        if (err[i] >= 0) sys_close(err[i]);
    }
}

/* an upload: the files asked for on the bottom row, then sz; nothing typed: the board is told to give up */
static void upload(const unsigned char *pre, long npre)
{
    static char line[256];
    char *f[32];
    int nf = 0;
    if (ask_line("Zmodem upload: send which files? ", line, sizeof line) > 0)
        for (char *q = line; *q && nf < 32;) {
            while (*q == ' ') *q++ = 0;
            if (!*q) break;
            f[nf++] = q;
            while (*q && *q != ' ') q++;
        }
    if (!nf) {
        board_cancel();
        printf("\r\033[KZmodem: no files: the upload is cancelled\r\n");
        return;
    }
    transfer(2, pre, npre, f, nf);
}

/* the socket's bytes: a Zmodem start (looked for in the raw bytes, before the decoder: its header holds no 0xFF)
 * starts rz or sz with the start and every byte after it; the rest goes through the decoder to the screen */
static unsigned char ztail[SPLIT_TAIL + 1];
static void from_net(const unsigned char *raw, long n)
{
    static unsigned char data[1024 + 8];
    static const unsigned char head[] = "**\x18" "B0";
    long at = 0, k = 0;
    int had = 0, kind = split_detect(ztail, raw, n, &had);
    if (!kind)
        kind = zm_detect(raw, n, &at);
    if (!kind) {
        tf_write(&tf, data, dec(raw, n, data));
        return;
    }
    if (had)
        at = 0;                                 /* it began in the read before: those bytes are on the screen */
    tf_write(&tf, data, dec(raw, at, data));
    tf_flush(&tf);
    if (had) {
        memcpy(data, head, (u32)had);
        k = had;
    }
    k += dec(raw + at, n - at, data + k);
    memset(ztail, 0, sizeof ztail);
    if (kind == 1)
        transfer(1, data, k, 0, 0);
    else
        upload(data, k);
}

/* Ctrl-]: the local prompt on the bottom row (never its last column), one key */
static void menu(void)
{
    printf("\0337\033[%d;1H\033[m\033[2Ktelnet> q quit, z suspend, a/p/x mode, d/u down/upload, ^] itself, Return: ",
           bottom_row());
    char c = 0;
    if (sys_read(0, &c, 1) != 1) c = 'q';
    printf("\033[2K\0338");
    if (c == 'q') {
        quit = 2;
    } else if (c == 'z') {
        term_off();
        printf("\r\n");
        sys_killsig(sys_getpid(), SIGTSTP);     /* the shell's job control; it goes on here after fg */
        term_on();
    } else if (c == 'a' || c == 'p' || c == 'x') {
        mode_to(c == 'a' ? TN_AMIGA : c == 'p' ? TN_PC : TN_UNIX, 0);
    } else if (c == 'd') {
        transfer(1, 0, 0, 0, 0);                /* a board that sends without Zmodem's start header */
    } else if (c == 'u') {
        upload(0, 0);
    } else if (c == esc_key) {
        unsigned char k = (unsigned char)c;
        keys_out(&k, 1);
    }
}

static void keys_in(void)
{
    unsigned char k[64];
    int n = sys_read(0, k, sizeof k);
    if (n <= 0) {                               /* the keys ended (a pipe, a file): the other side may go on */
        eof_keys = 1;
        return;
    }
    int from = 0;
    for (int i = 0; i < n && !quit; i++)
        if (esc_key >= 0 && k[i] == esc_key) {
            keys_out(k + from, i - from);
            menu();
            from = i + 1;
        }
    if (!quit)
        keys_out(k + from, n - from);
}

/* ---- the phone book (DESIGN D20 9 step 5): ~/.phonebook, a line per place, "NAME HOST [PORT] [amiga|pc|unix]
 * [ice]", '#' starts a comment; `telnet NAME` calls it, `telnet` alone lists it ---- */
struct book { char name[32], host[64]; int port, mode, ice; };

/* the book's entry for name (case does not matter), or with name 0 each entry listed; 1 found, 0 not */
static int book_find(const char *name, struct book *b)
{
    const char *home = getenv("HOME");
    static char path[ZPATH_MAX];
    static struct rd r;
    if (!home || snprintf(path, sizeof path, "%s/.phonebook", strcmp(home, "/") ? home : "") >= (int)sizeof path)
        return 0;
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    rd_init(&r, fd);
    char *line = 0;
    u32 cap = 0;
    int ln = 0, found = 0, listed = 0;
    while (!found && rd_getline(&r, &line, &cap) >= 0) {
        ln++;
        char *c = strchr(line, '#');
        if (c) *c = 0;
        char *w[8];
        int nw = 0;
        for (char *q = line; *q && nw < 8;) {
            while (*q == ' ' || *q == '\t' || *q == '\r') *q++ = 0;
            if (!*q) break;
            w[nw++] = q;
            while (*q && *q != ' ' && *q != '\t' && *q != '\r') q++;
        }
        if (!nw)
            continue;
        if (nw < 2 || strlen(w[0]) >= sizeof b->name || strlen(w[1]) >= sizeof b->host) {
            fprintf(2, "telnet: %s line %d: not \"NAME HOST [PORT] [amiga|pc|unix] [ice]\" (ignored)\n", path, ln);
            continue;
        }
        if (name) {
            int same = 1;
            for (int j = 0; same && (w[0][j] || name[j]); j++)
                same = tolower((unsigned char)w[0][j]) == tolower((unsigned char)name[j]);
            if (!same)
                continue;
        }
        strcpy(b->name, w[0]);
        strcpy(b->host, w[1]);
        b->port = 23;
        b->mode = 0;
        b->ice = 0;
        for (int j = 2; j < nw; j++) {
            if (isdigit((unsigned char)w[j][0]) && atoi(w[j]) > 0 && atoi(w[j]) <= 65535) b->port = atoi(w[j]);
            else if (!strcmp(w[j], "amiga")) b->mode = TN_AMIGA;
            else if (!strcmp(w[j], "pc")) b->mode = TN_PC;
            else if (!strcmp(w[j], "unix")) b->mode = TN_UNIX;
            else if (!strcmp(w[j], "ice")) b->ice = 1;
            else fprintf(2, "telnet: %s line %d: unknown word %s (ignored)\n", path, ln, w[j]);
        }
        if (name)
            found = 1;
        else {
            if (!listed++)
                printf("the phone book (%s):\n", path);
            printf("  %-16s %s %d%s%s\n", b->name, b->host, b->port, b->mode == TN_PC ? " pc" : b->mode == TN_UNIX ?
                   " unix" : b->mode == TN_AMIGA ? " amiga" : "", b->ice ? " ice" : "");
        }
    }
    free(line);
    sys_close(fd);
    return found;
}

static int usage(void)
{
    fprintf(2, "usage: telnet [-a|-p|-x] [-i] [-r] [-d DIR] [-e CHAR|none] HOST|NAME [PORT]\n");
    return 2;
}

int main(int argc, char **argv)
{
    int mode = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o = argv[i];
        if (!strcmp(o, "-x")) mode = TN_UNIX;
        else if (!strcmp(o, "-a")) mode = TN_AMIGA;
        else if (!strcmp(o, "-p")) mode = TN_PC;
        else if (!strcmp(o, "-i")) ice_opt = 1;
        else if (!strcmp(o, "-r")) rawtcp = 1;
        else if (!strcmp(o, "-d") && i + 1 < argc) dldir = argv[++i];
        else if (!strcmp(o, "-e") && i + 1 < argc) {
            const char *e = argv[++i];
            if (!strcmp(e, "none")) esc_key = -1;
            else if (e[0] == '^' && e[1] && !e[2]) esc_key = e[1] == '?' ? 0x7F : (e[1] & 0x1F);
            else if (e[0] && !e[1]) esc_key = (unsigned char)e[0];
            else return usage();
        } else return usage();
    }
    if (i >= argc || argc - i > 2) {
        struct book b;
        usage();
        if (i >= argc)
            book_find(0, &b);                   /* `telnet` alone: the phone book too */
        return 2;
    }
    host = argv[i];
    const char *pstr = i + 1 < argc ? argv[i + 1] : "23";
    static struct book bk;
    static char bport[8];
    if (book_find(host, &bk)) {                 /* a name in the phone book: its host, port and mode (options win) */
        host = bk.host;
        if (i + 1 >= argc) {
            snprintf(bport, sizeof bport, "%d", bk.port);
            pstr = bport;
        }
        if (!mode) mode = bk.mode;
        ice_opt |= bk.ice;
    }
    if (!mode) mode = TN_AMIGA;                 /* the owner's default: an Amiga board (D20 3.5) */
    if (dldir) {
        struct ustat st;
        if (sys_stat(dldir, &st) < 0 || (st.mode & S_IFMT) != S_IFDIR) {
            fprintf(2, "telnet: %s: not a directory\n", dldir);
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);                   /* a transfer program that ended: its pipe says so, no signal */
    int port = atoi(pstr);
    if (port <= 0 || port > 65535) { fprintf(2, "telnet: %s: not a port number\n", pstr); return 2; }

    struct addrinfo h, *r = 0;
    memset(&h, 0, sizeof h);
    h.ai_family = AF_INET;
    h.ai_socktype = SOCK_STREAM;
    printf("Trying %s...\n", host);
    int e = getaddrinfo(host, pstr, &h, &r);
    if (e) {
        fprintf(2, "telnet: %s: %s\n", host, e == EAI_NONAME ? "no such host" : gai_strerror(e));
        return 1;
    }
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0 || connect(sock, r->ai_addr, r->ai_addrlen) < 0) {
        fprintf(2, "telnet: %s port %d: %s\n", host, port, zstrerror(zerrno) ? zstrerror(zerrno) : "cannot connect");
        freeaddrinfo(r);
        return 1;
    }
    freeaddrinfo(r);
    printf("Connected to %s. Escape key is %s.\n", host, esc_key < 0 ? "off" : "Ctrl-]");
    int pr = getpriority(PRIO_PROCESS, 0);      /* one level lower than started at (the SLIP plan) */
    if (pr < 19)
        setpriority(PRIO_PROCESS, 0, pr + 1);

    int sz = sys_ttyinfo();
    tn.mode = tf.mode = mode;
    tn.send = sendall;
    tn.cols = tf.cols = sz & 0xFFFF ? sz & 0xFFFF : 80;
    tn.rows = tf.rows = sz >> 16 ? sz >> 16 : 25;
    tn.raw = rawtcp;
    tn_init(&tn, port);
    tf.put = screen;
    tf.answer = answer;
    tf.event = tf_event;
    tf_init(&tf, mode);
    ice_on = ice_opt;
    if (mode != TN_UNIX && sys_isatty(1)) {     /* the owner's rule: said once, at the start of a call */
        int col = sys_console(CON_COLOURS, 0);
        if (col > 0 && col < 16)                /* the screen takes 40 KB a plane: 16 colours are 4 planes */
            printf("telnet: %d colours: ANSI art loses its shading; `colours 16` in /etc/console shows it fully "
                   "(%d KB more chip RAM)\n", col, 40 * (col == 8 ? 1 : col == 4 ? 2 : 3));
    }
    term_on();

    static unsigned char in[1024];
    struct pollfd p[2];
    while (!quit) {
        p[0].fd = sock; p[0].events = POLLIN; p[0].revents = 0;
        p[1].fd = 0; p[1].events = POLLIN; p[1].revents = 0;
        if (poll(p, eof_keys ? 1 : 2, -1) < 0)
            continue;                           /* a signal (SIGCONT after a suspend) */
        if (p[0].revents) {
            int n = recv(sock, in, sizeof in, 0);
            if (n <= 0) { quit = 1; break; }
            from_net(in, n);
        }
        if (!eof_keys && p[1].revents && !quit)
            keys_in();
    }
    tf_flush(&tf);
    term_off();
    sys_close(sock);
    printf(quit == 2 ? "\r\nConnection closed.\n" : "\r\nConnection closed by %s.\n", host);
    return 0;
}
