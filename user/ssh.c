/* SPDX-License-Identifier: GPL-2.0-only */
/* ssh [-p PORT] [-l USER] [-i KEY] [-e CHAR|none] [-t|-T] [-v] [user@]host [command...]: a secure shell client
 * (DESIGN D21 4, docs/man/ssh.txt). The protocol is net/ssh/ (system-call free, shared with the PC tests), the
 * arithmetic the crypto library (crypto/); this file gives them the socket, the keys, the files and the console.
 * The terminal side is telnet's Unix mode (D20, D21 4.3): the session's output through net/telnet/'s Unix filter
 * (UTF-8 shown as Latin-1) with the console's CONR_LFONLY rule, the keys through its key translation, 256 bytes a
 * screen write, nice 1. Host keys: OpenSSH's known_hosts, trust on first use, a changed key refused. The escape is
 * OpenSSH's: ~. ~^Z ~? ~~ after Return. The exit status is the remote command's, 255 for ssh's own errors. */
#include "socket.h"
#include "pw.h"
#include "../net/telnet/telnet.h"
#include "../net/ssh/ssh.h"
#include "../crypto/monocypher/monocypher-ed25519.h"

static int sock = -1, verbose, gone, quit, raw_on, rules_ok, rules_old, esc_char = '~', filter_on;
static const char *host;
static int port = 22;
static struct ssh s;
static struct tf tf;
static unsigned char *rbuf_heap;
static char home_ssh[ZPATH_MAX];

static void die(const char *fmt, ...);

static void term_off(void)
{
    if (rules_ok)
        sys_conrules(1, (u32)rules_old & CONR_LFONLY, CONR_LFONLY);
    rules_ok = 0;
    if (raw_on)
        sys_ttymode(0);
    raw_on = 0;
}

static unsigned char sk[64];            /* the identity's private key, wiped on every way out (audit 2026-10-28 S18) */

static void die(const char *fmt, ...)
{
    char m[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    term_off();
    dprintf(2, "\r\nssh: %s\n", m);
    ssh_wipe(&s);
    zc_wipe(sk, sizeof sk);
    sys_exit(255);
}

/* ---- the core's callbacks ---- */
static void out_send(void *u, const unsigned char *b, long n)
{
    (void)u;
    while (n > 0 && !gone) {
        int w = send(sock, b, (u32)n, 0);
        if (w <= 0) { gone = 1; return; }
        b += w;
        n -= w;
    }
}

static void writeall(int fd, const unsigned char *b, long n)
{
    while (n > 0) {
        int w = sys_write(fd, b, (u32)(n > 256 ? 256 : n));     /* 256 bytes a screen write (D20 6, D21 4.3) */
        if (w <= 0)
            return;
        b += w;
        n -= w;
    }
}

static void screen(void *u, const unsigned char *b, long n) { (void)u; writeall(1, b, n); }

static void out_data(void *u, int ext, const unsigned char *b, long n)
{
    (void)u;
    if (ext)
        writeall(2, b, n);
    else if (filter_on)
        tf_write(&tf, b, n);            /* the Unix mode: UTF-8 shown as Latin-1 */
    else
        writeall(1, b, n);              /* a command's bytes as they are (into a file, a pipe) */
}

static void out_text(void *u, int kind, const char *t, long n)
{
    (void)u;
    int k = kind & 0xff;
    if ((k == SSH_TEXT_PROGRESS || k == SSH_TEXT_DEBUG) && !verbose && !(kind & SSH_TEXT_ALWAYS))
        return;
    if (k == SSH_TEXT_PROGRESS)
        dprintf(2, "ssh: [%u.%02u s] %.*s\r\n", sys_ticks() / 100, sys_ticks() % 100, (int)n, t);
    else
        dprintf(2, "%.*s%s", (int)n, t, n && t[n - 1] == '\n' ? "" : "\r\n");
}

static unsigned char *grow(void *u, unsigned char *old, long need)
{
    (void)u; (void)old;
    unsigned char *nb = realloc(rbuf_heap, (unsigned long)need);
    if (nb)
        rbuf_heap = nb;
    return nb;
}

/* /dev/random: waits until the pool was seeded once (D21 5); said when it would wait */
static int seed(void *u, unsigned char *b, size_t n)
{
    (void)u;
    int fd = sys_open("/dev/random", O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return fd;
    int r = sys_read(fd, b, (u32)n);
    if (r == -EAGAIN) {
        dprintf(2, "%s: waiting for the random pool (type on the keyboard)\n", (const char *)u);
        sys_close(fd);
        fd = sys_open("/dev/random", O_RDONLY);
        if (fd < 0)
            return fd;
        r = sys_read(fd, b, (u32)n);
    }
    sys_close(fd);
    return r == (int)n ? 0 : r < 0 ? r : -EIO;
}

/* a whole small file into memory (NUL after it; zslurp, libz); 0 if it cannot be read */
static char *readfile(const char *path, long *n, u32 max)
{
    u32 l = 0;
    char *b = zslurp(path, max, &l);
    *n = (long)l;
    return b;
}

/* a line typed on the terminal (/dev/tty: standard input may be a pipe), echoed or not */
static int ask(const char *prompt, char *buf, int max, int echo)
{
    int t = sys_open("/dev/tty", O_RDWR), n = 0, rc = 0;
    if (t < 0)
        return -1;
    sys_write(t, prompt, strlen(prompt));
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    for (;;) {
        char c;
        if (sys_read(t, &c, 1) != 1 || c == 3 || c == 4) { rc = -1; break; }
        if (c == '\r' || c == '\n') break;
        if (c == 8 || c == 127) {
            if (n) { n--; if (echo) sys_write(t, "\b \b", 3); }
        } else if (c == 21) n = 0;
        else if ((unsigned char)c >= 32 && n < max - 1) {
            buf[n++] = c;
            if (echo) sys_write(t, &c, 1);
        }
    }
    sys_ttymode(raw_on ? TTY_RAW | TTY_NOSIG : 0);
    buf[n] = 0;
    sys_write(t, "\r\n", 2);
    sys_close(t);
    return rc;
}

/* ---- the host key: ~/.ssh/known_hosts (and root's /etc/ssh/ssh_known_hosts, read only) ---- */
static void hostkey(void)
{
    char fp[SSH_FP_LEN], kh[ZPATH_MAX];
    long n = 0, line = 0, line2 = 0, n2 = 0;
    snprintf(kh, sizeof kh, "%s/known_hosts", home_ssh);
    char *t = readfile(kh, &n, 1 << 20);
    int r = ssh_kh_check(t ? t : "", n, host, port, s.hostkey, s.hostkeylen, &line);
    int needs_nl = t && n > 0 && t[n - 1] != '\n';    /* a file without its last line end: a new line goes after one */
    free(t);
    ssh_fingerprint(fp, s.hostkey, s.hostkeylen);
    /* both files, as OpenSSH: a key revoked in either is refused (audit 2026-10-28 S15: root's revocation did not
     * count once the user's own file knew the key); otherwise known in either is known, then changed in either */
    char *g = readfile("/etc/ssh/ssh_known_hosts", &n2, 1 << 20);
    int r2 = ssh_kh_check(g ? g : "", n2, host, port, s.hostkey, s.hostkeylen, &line2);
    free(g);
    if (r != SSH_KH_REVOKED && (r2 == SSH_KH_REVOKED || (r != SSH_KH_OK && (r2 == SSH_KH_OK ||
        (r != SSH_KH_CHANGED && r2 == SSH_KH_CHANGED))))) {
        r = r2;
        line = line2;
        strcpy(kh, "/etc/ssh/ssh_known_hosts");
    }
    if (r == SSH_KH_OK) {
        ssh_hostkey(&s, 1);
        return;
    }
    if (r == SSH_KH_CHANGED) {
        dprintf(2, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                   "ssh: THE HOST KEY OF %s HAS CHANGED: refused\n"
                   "Someone could be listening, or the host was set up again.\n"
                   "The key it sent: ED25519 %s\n"
                   "The key known: %s line %ld. If the change is right, remove that line\n"
                   "(ssh-keygen -R %s) and connect again.\n", host, fp, kh, line, host);
        ssh_hostkey(&s, 0);
        die("host key verification failed");
    }
    if (r == SSH_KH_REVOKED) {          /* never "remove that line": that would undo the revocation (S15) */
        dprintf(2, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                   "ssh: THE HOST KEY OF %s IS REVOKED: refused\n"
                   "The key it sent: ED25519 %s is marked @revoked in %s line %ld.\n"
                   "A revoked key is never accepted.\n", host, fp, kh, line);
        ssh_hostkey(&s, 0);
        die("host key verification failed");
    }
    char a[96];
    dprintf(2, "The authenticity of host '%s' can't be established.\n%sED25519 key fingerprint is %s.\n", host,
            r == SSH_KH_OTHERTYPE ? "(It is known with a key of another type.)\n" : "", fp);
    for (;;) {
        if (ask("Are you sure you want to continue connecting (yes/no/[fingerprint])? ", a, sizeof a, 1) < 0 ||
            !strcmp(a, "no") || !strcmp(a, "n")) {
            ssh_hostkey(&s, 0);
            die("host key verification failed");
        }
        if (!strcmp(a, "yes") || !strcmp(a, fp))
            break;
        if (!strncmp(a, "SHA256:", 7)) {
            dprintf(2, "That is not the host's fingerprint.\n");
            continue;
        }
        dprintf(2, "Please type 'yes', 'no' or the fingerprint: ");
    }
    char l[400];
    long ln = ssh_kh_line(l, sizeof l, host, port, s.hostkey, s.hostkeylen);
    sys_mkdir(home_ssh);
    int fd = ln > 0 ? sys_open(kh, O_WRONLY | O_CREAT | O_APPEND) : -1;
    if (fd >= 0 && needs_nl && strcmp(kh, "/etc/ssh/ssh_known_hosts"))
        sys_write(fd, "\n", 1);         /* else the new line glued onto the last one spoilt both (S16) */
    if (fd < 0 || sys_write(fd, l, (u32)ln) != (int)ln)
        dprintf(2, "ssh: could not add the key to %s\n", kh);
    else
        dprintf(2, "Warning: added '%s' (ED25519) to the list of known hosts.\n", host);
    if (fd >= 0) sys_close(fd);
    ssh_hostkey(&s, 1);
}

/* ---- the identity: -i, else ~/.ssh/id_ed25519 if it is there (sk: above die) ---- */
static int load_key(const char *path, int must)
{
    struct ustat st;
    if (sys_stat(path, &st) < 0) {
        if (must)
            die("%s: %s", path, zwhy("cannot read"));
        return 0;
    }
    if (st.mode & 077) {
        dprintf(2, "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
                   "ssh: UNPROTECTED PRIVATE KEY FILE: %s (mode %o) is open to\n"
                   "others; it is ignored. chmod 600 %s\n", path, st.mode & 0777, path);
        return 0;
    }
    long n;
    char *t = readfile(path, &n, 16384);
    static unsigned char work[SSH_KEY_WORK];
    if (!t)
        die("%s: cannot read", path);
    int r = ssh_key_read(t, n, 0, sk, 0, 0, work, sizeof work);
    if (r == SSH_KEY_ENEEDPASS) {
        char pass[128];
        if (ask("Enter the passphrase for the key: ", pass, sizeof pass, 0) == 0) {
            dprintf(2, "ssh: unlocking the key (bcrypt: this takes a while)...\n");
            r = ssh_key_read(t, n, pass, sk, 0, 0, work, sizeof work);
        }
        zc_wipe(pass, sizeof pass);
    }
    zc_wipe(t, (u32)n);
    free(t);
    if (r != SSH_KEY_OK) {
        dprintf(2, "ssh: %s: %s (not used)\n", path, r == SSH_KEY_ETYPE ? "not an Ed25519 key" :
                r == SSH_KEY_EBADPASS ? "wrong passphrase" : r == SSH_KEY_ENOTKEY ? "not an OpenSSH private key" :
                "a damaged key file");
        return 0;
    }
    return 1;
}

/* ---- the keys: translated as telnet's Unix mode; the escape after Return ---- */
static unsigned char kbuf[1024];
static long kn, koff;
static int at_start = 1, esc_wait;

static void key_out(const unsigned char *b, long n)
{
    int bell = 0;
    if (kn - koff + 2 * n > (long)sizeof kbuf) {
        if (koff) { memmove(kbuf, kbuf + koff, (u32)(kn - koff)); kn -= koff; koff = 0; }
        if (kn + 2 * n > (long)sizeof kbuf) return;     /* typed faster than the session takes it: dropped */
    }
    kn += tk_keys(TN_UNIX, 1, b, n, kbuf + kn, &bell);
}

static void suspend(void)
{
    term_off();
    dprintf(2, "\r\n");
    sys_killsig(sys_getpid(), SIGTSTP);
    if (sys_isatty(0)) { sys_ttymode(TTY_RAW | TTY_NOSIG); raw_on = 1; }
    if (filter_on) {
        int r = sys_conrules(1, CONR_LFONLY, CONR_LFONLY);
        if (r >= 0) { rules_old = r; rules_ok = 1; }
    }
}

static void keys_in(const unsigned char *k, long n)
{
    for (long i = 0; i < n; i++) {
        unsigned char c = k[i];
        if (esc_wait) {
            esc_wait = 0;
            if (c == '.') { quit = 1; dprintf(2, "\r\nConnection to %s closed.\r\n", host); return; }
            if (c == 0x1A) { suspend(); at_start = 0; continue; }
            if (c == '?') {
                dprintf(2, "\r\nSupported escape sequences:\r\n %c.  close the connection\r\n %c^Z suspend ssh\r\n"
                        " %c?  this help\r\n %c%c  send the escape character\r\n", esc_char, esc_char, esc_char,
                        esc_char, esc_char);
                continue;
            }
            if (c != (unsigned char)esc_char) {
                unsigned char e = (unsigned char)esc_char;
                key_out(&e, 1);
            }
        } else if (at_start && esc_char >= 0 && c == (unsigned char)esc_char) {
            esc_wait = 1;
            continue;
        }
        key_out(&c, 1);
        at_start = c == '\r' || c == '\n';
    }
}

static const char USAGE[] = "usage: ssh [-p PORT] [-l USER] [-i KEY] [-e CHAR|none] [-t|-T] [-v] [user@]host [command...]\n";
static int usage(void)
{
    zusage(USAGE);
}

int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    const char *user = getenv("USER"), *keyfile = 0;
    int tflag = 0, i = 1;
    static char cmd[1024], userbuf[64];
    for (; zisopt(argc, argv, &i, USAGE); i++) {
        const char *o = argv[i];
        if (!strcmp(o, "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(o, "-l") && i + 1 < argc) user = argv[++i];
        else if (!strcmp(o, "-i") && i + 1 < argc) keyfile = argv[++i];
        else if (!strcmp(o, "-e") && i + 1 < argc) {
            const char *e = argv[++i];
            if (!strcmp(e, "none")) esc_char = -1;
            else if (e[0] == '^' && e[1] && !e[2]) esc_char = e[1] & 0x1F;
            else if (e[0] && !e[1]) esc_char = (unsigned char)e[0];
            else return usage();
        } else if (!strcmp(o, "-t")) tflag = 1;
        else if (!strcmp(o, "-T")) tflag = -1;
        else if (!strcmp(o, "-v")) verbose = 1;
        else zbadopt(o, USAGE);
    }
    if (i >= argc || port <= 0 || port > 65535)
        return usage();
    host = argv[i++];
    const char *at = strchr(host, '@');
    if (at) {
        u32 l = (u32)(at - host);
        if (l >= sizeof userbuf) return usage();
        memcpy(userbuf, host, l);
        userbuf[l] = 0;
        user = userbuf;
        host = at + 1;
    }
    for (; i < argc; i++) {
        if (strlen(cmd) + strlen(argv[i]) + 2 > sizeof cmd) die("the command is too long");
        if (*cmd) strcat(cmd, " ");
        strcat(cmd, argv[i]);
    }
    if (!user) {
        struct pw p;
        user = pw_byuid(sys_getuid(), &p) == 0 ? strdup(p.name) : "root";
    }
    const char *home = getenv("HOME");
    snprintf(home_ssh, sizeof home_ssh, "%s/.ssh", home && strcmp(home, "/") ? home : "");

    if (keyfile)
        load_key(keyfile, 1);
    else {
        char kp[ZPATH_MAX];
        snprintf(kp, sizeof kp, "%s/id_ed25519", home_ssh);
        if (!load_key(kp, 0))
            keyfile = 0;
        else
            keyfile = "default";
    }
    static struct zc_rng rng;
    static char me[] = "ssh";
    int e = zc_rng_seed(&rng, seed, me);
    if (e < 0)
        die("no random numbers: %s", zstrerror(e) ? zstrerror(e) : "failed");

    const char *why;
    if ((sock = zconnect(host, port, 0, &why)) < 0)
        die("%s port %d: %s", host, port, why);
    int pr = getpriority(PRIO_PROCESS, 0);      /* one level lower: netd keeps up, other consoles stay snappy */
    if (pr < 19)
        setpriority(PRIO_PROCESS, 0, pr + 1);
    signal(SIGPIPE, SIG_IGN);

    static unsigned char sbuf[SSH_SBUF_MIN], net[2048];
    int interactive = sys_isatty(0);
    int pty = tflag > 0 || (tflag == 0 && !*cmd && interactive);
    int sz = sys_ttyinfo();
    rbuf_heap = malloc(SSH_RBUF_MIN);
    if (!rbuf_heap)
        die("out of memory");
    s.send = out_send;
    s.data = out_data;
    s.text = out_text;
    s.grow = grow;
    s.rng = &rng;
    s.version = "SSH-2.0-Zenix_ssh_0.1";
    s.user = user;
    s.rbuf = rbuf_heap;
    s.rcap = SSH_RBUF_MIN;
    s.sbuf = sbuf;
    s.scap = sizeof sbuf;
    s.id_sk = keyfile ? sk : 0;
    s.methods = (keyfile ? SSH_AUTH_PUBKEY : 0) | SSH_AUTH_KBDINT | SSH_AUTH_PASSWORD;
    s.pty = pty;
    s.term = "xterm-16color";
    s.cols = sz & 0xFFFF ? sz & 0xFFFF : 80;
    s.rows = sz >> 16 ? sz >> 16 : 25;
    s.command = *cmd ? cmd : 0;
    tf.put = screen;
    tf.cols = s.cols;
    tf.rows = s.rows;
    tf_init(&tf, TN_UNIX);
    if (interactive)
        dprintf(2, "ssh: key exchange with %s (on a 68020 this takes a while)...\n", host);
    ssh_start(&s);
    ssh_tick(&s, sys_uptime());

    long netn = 0, netoff = 0;
    int stdin_open = 1, session_seen = 0;
    while (!quit && s.state != SSH_ST_FAILED && s.state != SSH_ST_CLOSED) {
        if (s.need == SSH_NEED_HOSTKEY)
            hostkey();
        else if (s.need == SSH_NEED_PASSWORD) {
            char pw[128], q[160];
            snprintf(q, sizeof q, "%s@%s's password: ", user, host);
            if (ask(q, pw, sizeof pw, 0) < 0)
                ssh_password(&s, 0);
            else
                ssh_password(&s, pw);
            zc_wipe(pw, sizeof pw);
        } else if (s.need == SSH_NEED_KBDINT) {
            static char ans[SSH_KBD_PROMPTS][128];
            const char *a[SSH_KBD_PROMPTS];
            int ok = 1;
            if (s.kbd_name[0]) dprintf(2, "%s\n", s.kbd_name);
            if (s.kbd_instruction[0]) dprintf(2, "%s\n", s.kbd_instruction);
            for (int k = 0; k < s.kbd_n && ok; k++) {
                ok = ask(s.kbd_prompt[k], ans[k], sizeof ans[k], s.kbd_echo[k]) == 0;
                a[k] = ans[k];
            }
            ssh_kbdint(&s, ok ? a : 0);
            zc_wipe(ans, sizeof ans);
        }
        if (s.state == SSH_ST_FAILED || s.state == SSH_ST_CLOSED)
            break;
        if (s.state == SSH_ST_SESSION && !session_seen) {
            session_seen = 1;
            if (keyfile) zc_wipe(sk, sizeof sk);        /* the key is not needed again (9.1) */
            if (pty && interactive) {
                sys_ttymode(TTY_RAW | TTY_NOSIG);       /* Ctrl-C and Ctrl-Z go to the other side */
                raw_on = 1;
            }
            if (pty && sys_isatty(1)) {
                filter_on = 1;
                int rr = sys_conrules(1, CONR_LFONLY, CONR_LFONLY);
                if (rr >= 0) { rules_old = rr; rules_ok = 1; }
            }
        }
        /* bytes held from the socket first (the core may take only some) */
        if (netoff < netn && s.need == SSH_NEED_NONE) {
            long k = ssh_input(&s, net + netoff, netn - netoff);
            if (k < 0)
                break;
            netoff += k;
            if (netoff < netn && s.need == SSH_NEED_NONE && k == 0)
                die("a packet too big for memory");
            continue;
        }
        if (s.need != SSH_NEED_NONE)
            continue;
        /* keys to the session: ssh_write until it takes nothing, then poll (D21 as built, choice for step 3) */
        while (s.state == SSH_ST_SESSION && koff < kn) {
            long k = ssh_write(&s, kbuf + koff, kn - koff);
            if (!k) break;
            koff += k;
        }
        if (koff == kn) koff = kn = 0;
        if (s.state == SSH_ST_SESSION && !stdin_open && koff == kn) {
            ssh_eof(&s);
            stdin_open = -1;
        }
        struct pollfd p[2];
        int np = 1;
        p[0].fd = sock; p[0].events = POLLIN; p[0].revents = 0;
        if (s.state == SSH_ST_SESSION && stdin_open > 0 && kn - koff < 512) {
            p[1].fd = 0; p[1].events = POLLIN; p[1].revents = 0;
            np = 2;
        }
        poll(p, (u32)np, 1000);
        ssh_tick(&s, sys_uptime());
        if (gone)
            die("the connection is gone");
        if (p[0].revents) {
            int k = recv(sock, net, sizeof net, 0);
            if (k <= 0) {
                if (s.state == SSH_ST_CLOSED) break;
                die(s.state == SSH_ST_SESSION ? "the connection closed" : "the connection closed before the session");
            }
            netn = k;
            netoff = 0;
        }
        if (np == 2 && p[1].revents) {
            unsigned char k[256];
            int r2 = sys_read(0, k, sizeof k);
            if (r2 <= 0)
                stdin_open = 0;
            else if (raw_on)
                keys_in(k, r2);
            else {                                  /* not a terminal: the bytes as they are */
                if (kn + r2 > (long)sizeof kbuf) { memmove(kbuf, kbuf + koff, (u32)(kn - koff)); kn -= koff; koff = 0; }
                memcpy(kbuf + kn, k, (u32)r2);
                kn += r2;
            }
        }
    }
    tf_flush(&tf);
    term_off();
    if (quit)
        ssh_close(&s);
    if (s.state == SSH_ST_FAILED) {
        dprintf(2, "ssh: %s\n", s.err);
        ssh_wipe(&s);
        zc_wipe(sk, sizeof sk);
        return 255;
    }
    if (verbose)
        dprintf(2, "ssh: %lu key exchange%s\n", s.kex_count, s.kex_count == 1 ? "" : "s");
    int st = s.exit_status;
    ssh_wipe(&s);
    zc_wipe(sk, sizeof sk);
    sys_close(sock);
    return quit ? 255 : st < 0 ? 255 : st;
}
