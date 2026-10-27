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

static int sock = -1, eof_keys, quit, rules_ok, rules_old, raw_on, echo_note, font_note, font_on, ice_on, ice_opt;
static int esc_key = 0x1D;                      /* Ctrl-] */
static const char *host;
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

/* Ctrl-]: the local prompt on the bottom row (never its last column), one key */
static void menu(void)
{
    int rows = sys_ttyinfo() >> 16;
    printf("\0337\033[%d;1H\033[m\033[2Ktelnet> q quit, z suspend, a/p/x Amiga/PC/Unix, ^] itself, Return back: ",
           rows ? rows : 25);
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

static int usage(void)
{
    fprintf(2, "usage: telnet [-a|-p|-x] [-i] [-r] [-e CHAR|none] HOST [PORT]\n");
    return 2;
}

int main(int argc, char **argv)
{
    int mode = TN_AMIGA, rawtcp = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o = argv[i];
        if (!strcmp(o, "-x")) mode = TN_UNIX;
        else if (!strcmp(o, "-a")) mode = TN_AMIGA;
        else if (!strcmp(o, "-p")) mode = TN_PC;
        else if (!strcmp(o, "-i")) ice_opt = 1;
        else if (!strcmp(o, "-r")) rawtcp = 1;
        else if (!strcmp(o, "-e") && i + 1 < argc) {
            const char *e = argv[++i];
            if (!strcmp(e, "none")) esc_key = -1;
            else if (e[0] == '^' && e[1] && !e[2]) esc_key = e[1] == '?' ? 0x7F : (e[1] & 0x1F);
            else if (e[0] && !e[1]) esc_key = (unsigned char)e[0];
            else return usage();
        } else return usage();
    }
    if (i >= argc || argc - i > 2) return usage();
    host = argv[i];
    const char *pstr = i + 1 < argc ? argv[i + 1] : "23";
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

    static unsigned char in[1024], data[1024];
    struct pollfd p[2];
    while (!quit) {
        p[0].fd = sock; p[0].events = POLLIN; p[0].revents = 0;
        p[1].fd = 0; p[1].events = POLLIN; p[1].revents = 0;
        if (poll(p, eof_keys ? 1 : 2, -1) < 0)
            continue;                           /* a signal (SIGCONT after a suspend) */
        if (p[0].revents) {
            int n = recv(sock, in, sizeof in, 0);
            if (n <= 0) { quit = 1; break; }
            long m = rawtcp ? (memcpy(data, in, (u32)n), n) : tn_decode(&tn, in, n, data);
            tf_write(&tf, data, m);
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
