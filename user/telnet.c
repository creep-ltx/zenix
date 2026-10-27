/* SPDX-License-Identifier: GPL-2.0-only */
/* telnet HOST [PORT]: a terminal to another computer over the network (DESIGN D20, docs/man/telnet.txt). Step 2: the
 * Unix mode (-x), xterm-like. One process, one loop: poll on the socket and the keys (D16); the protocol, the stream
 * filter and the key translation are telnet's core (net/telnet/, shared with the PC tests). The console draws: in the
 * Unix mode it gets CONR_LFONLY (SYS_console CON_RULES on standard output); when that is refused (standard output is a
 * file, a pipe), telnet says so once and goes on. Ctrl-] opens the local prompt.
 * The SLIP plan (D20 section 6): the screen is written at most 256 bytes a call and telnet runs one nice level lower
 * than it was started at, so a woken netd (nice 0) takes the CPU at the next return to user mode. */
#include "socket.h"
#include "../net/telnet/telnet.h"

static int sock = -1, eof_keys, quit, rules_ok, rules_old, raw_on, echo_note;
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

/* the console: raw keys, LF without CR (CONR_LFONLY); undone on the way out and at a suspend */
static void term_on(void)
{
    if (sys_isatty(0)) {
        sys_ttymode(TTY_RAW | TTY_NOSIG);       /* every key, Ctrl-C included, goes to the other side */
        raw_on = 1;
    }
    int r = sys_conrules(1, CONR_LFONLY, CONR_LFONLY);
    rules_ok = r >= 0;
    if (rules_ok)
        rules_old = r;
    else if (!echo_note++)
        fprintf(2, "telnet: the console's rules are not available here (%s): line ends are drawn as they come\r\n",
                zstrerror(r));
}

static void term_off(void)
{
    if (rules_ok)
        sys_conrules(1, rules_old & CONR_LFONLY, CONR_LFONLY);
    rules_ok = 0;
    if (raw_on)
        sys_ttymode(0);
    raw_on = 0;
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
    printf("\0337\033[%d;1H\033[m\033[2Ktelnet> q quit, z suspend, ^] send ^], Return back: ", rows ? rows : 25);
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
    fprintf(2, "usage: telnet [-x] [-r] [-e CHAR|none] HOST [PORT]\n");
    return 2;
}

int main(int argc, char **argv)
{
    int mode = TN_UNIX, rawtcp = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o = argv[i];
        if (!strcmp(o, "-x")) mode = TN_UNIX;
        else if (!strcmp(o, "-a") || !strcmp(o, "-p")) {
            fprintf(2, "telnet: %s: the BBS modes are not built yet (DESIGN D20 step 3); -x is the Unix mode\n", o);
            return 2;
        } else if (!strcmp(o, "-r")) rawtcp = 1;
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
    tf_init(&tf, mode);
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
