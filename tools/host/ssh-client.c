/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * A PC client over net/ssh/ (DESIGN D21 10 step 1), for tools/host/ssh-test.py: it logs in to a real sshd the way
 * Zenix's ssh will (the same core, keys, known_hosts and config code), with the PC's sockets and poll.
 *   ssh-client [-p PORT] [-l USER] [-i KEYFILE] [-x PASSPHRASE] [-P PASSWORD] [-k KNOWN_HOSTS] [-y] [-t]
 *              [-m METHODS] [-r BYTES] [-v] [-F CONFIG] HOST [COMMAND]
 * -y: an unknown host key is trusted and written to KNOWN_HOSTS (else refused); -P: the password, also every
 * keyboard-interactive answer; -m: the methods allowed (letters k, i, p: publickey, keyboard-interactive, password;
 * default all); -r: the client rekeys after BYTES; -F: a config file looked up for HOST. The exit status is the
 * remote command's, 255 for the client's own errors (as OpenSSH's ssh). With -v the steps and, at the end, the
 * number of key exchanges ("ssh-client: kex N") go to standard error.
 *   ssh-client -G FILE [-x PASSPHRASE] [-a ROUNDS] [-C COMMENT]
 * makes an Ed25519 key with net/ssh/'s writer, as Zenix's ssh-keygen will: FILE and FILE.pub.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "../../net/ssh/ssh.h"
#include "../../crypto/monocypher/monocypher-ed25519.h"

static int sock = -1, verbose;
static unsigned char *rbuf_heap;

static void die(const char *m)
{
    fprintf(stderr, "ssh-client: %s\n", m);
    exit(255);
}

static void out_send(void *u, const unsigned char *b, long n)
{
    (void)u;
    while (n > 0) {
        ssize_t k = write(sock, b, (size_t)n);

        if (k < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            die("write to the socket failed");
        }
        b += k;
        n -= k;
    }
}

static void out_data(void *u, int ext, const unsigned char *b, long n)
{
    (void)u;
    while (n > 0) {
        ssize_t k = write(ext ? 2 : 1, b, (size_t)n);

        if (k <= 0) {
            if (k < 0 && errno == EINTR)
                continue;
            exit(255);
        }
        b += k;
        n -= k;
    }
}

static void out_text(void *u, int kind, const char *s, long n)
{
    (void)u;
    if (kind == SSH_TEXT_PROGRESS || (kind & 0xff) == SSH_TEXT_DEBUG) {
        if (!verbose && !(kind & SSH_TEXT_ALWAYS))
            return;
    }
    fprintf(stderr, "ssh-client: [%d] %.*s\n", kind & 0xff, (int)n, s);
}

static unsigned char *grow(void *u, unsigned char *old, long need)
{
    unsigned char *nb = realloc(rbuf_heap, (size_t)need);

    (void)u;
    (void)old;
    if (nb)
        rbuf_heap = nb;
    if (verbose && nb)
        fprintf(stderr, "ssh-client: receive buffer grown to %ld\n", need);
    return nb;
}

static int seed(void *u, unsigned char *b, size_t n)
{
    int fd = open("/dev/urandom", O_RDONLY);

    (void)u;
    if (fd < 0 || read(fd, b, n) != (ssize_t)n)
        return -5;
    close(fd);
    return 0;
}

static char *readfile(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    char *b;

    *n = 0;
    if (!f)
        return 0;
    b = malloc(1 << 20);
    *n = (long)fread(b, 1, (1 << 20) - 1, f);
    b[*n] = 0;
    fclose(f);
    return b;
}

static void cfg_report(void *u, long line, const char *what)
{
    fprintf(stderr, "ssh-client: %s line %ld: %s\n", (const char *)u, line, what);
}

int main(int argc, char **argv)
{
    struct ssh s;
    struct zc_rng rng;
    static unsigned char sbuf[SSH_SBUF_MIN], inbuf[65536], net[65536], sk[64], work[SSH_KEY_WORK];
    const char *port = "22", *user = getenv("USER"), *keyfile = 0, *pass = 0, *khfile = 0, *passphrase = 0,
               *methods = "kip", *cfgfile = 0, *host;
    int yes = 0, tty = 0, c, have_key = 0, stdin_open = 1;
    long innum = 0, inoff = 0, netn = 0, netoff = 0, rekey = 0;
    char cmd[4096] = "";
    struct addrinfo hints, *ai;
    static struct ssh_cfg cfg;

    const char *genfile = 0, *comment = "zenix-test";
    unsigned rounds = 16;

    while ((c = getopt(argc, argv, "p:l:i:x:P:k:ytm:r:vF:G:a:C:")) != -1) {
        switch (c) {
        case 'G': genfile = optarg; break;
        case 'a': rounds = (unsigned)atoi(optarg); break;
        case 'C': comment = optarg; break;
        case 'p': port = optarg; break;
        case 'l': user = optarg; break;
        case 'i': keyfile = optarg; break;
        case 'x': passphrase = optarg; break;
        case 'P': pass = optarg; break;
        case 'k': khfile = optarg; break;
        case 'y': yes = 1; break;
        case 't': tty = 1; break;
        case 'm': methods = optarg; break;
        case 'r': rekey = atol(optarg); break;
        case 'v': verbose = 1; break;
        case 'F': cfgfile = optarg; break;
        default: die("usage: ssh-client [-p PORT] [-l USER] [-i KEY] [-x PASS] [-P PW] [-k KH] [-y] [-t] [-m kip] "
                     "[-r BYTES] [-v] [-F CONFIG] HOST [COMMAND]");
        }
    }
    if (genfile) {
        unsigned char seedb[32], pk[32], salt[16], chk[4], blob[SSH_ED25519_BLOB];
        static char text[4096], line[512];
        long n;
        FILE *f;
        char pubname[1024];

        if (seed(0, seedb, 32) || seed(0, salt, 16) || seed(0, chk, 4))
            die("no random seed");
        crypto_ed25519_key_pair(sk, pk, seedb);
        n = ssh_key_write(text, sizeof text, sk, comment, passphrase, salt, rounds, chk, work, sizeof work);
        if (n < 0) {
            fprintf(stderr, "ssh-client: ssh_key_write: error %ld\n", n);
            exit(255);
        }
        {
            int fd = open(genfile, O_WRONLY | O_CREAT | O_TRUNC, 0600);    /* mode 600, as ssh-keygen (D21 4.4) */

            f = fd < 0 ? 0 : fdopen(fd, "w");
        }
        if (!f || fwrite(text, 1, (size_t)n, f) != (size_t)n)
            die("cannot write the key");
        fclose(f);
        ssh_key_blob(blob, pk);
        n = ssh_pubkey_line(line, sizeof line, blob, SSH_ED25519_BLOB, comment);
        snprintf(pubname, sizeof pubname, "%s.pub", genfile);
        f = fopen(pubname, "w");
        if (n < 0 || !f || fprintf(f, "%s\n", line) < 0)
            die("cannot write the public key");
        fclose(f);
        zc_wipe(sk, sizeof sk);
        return 0;
    }
    if (optind >= argc)
        die("no host");
    host = argv[optind++];
    for (; optind < argc; optind++) {
        if (*cmd)
            strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1);
        strncat(cmd, argv[optind], sizeof cmd - strlen(cmd) - 1);
    }
    if (cfgfile) {
        long n;
        char *t = readfile(cfgfile, &n);

        if (!t)
            die("cannot read the config file");
        ssh_config(t, n, host, &cfg, cfg_report, (void *)cfgfile);
        if (cfg.hostname[0]) host = cfg.hostname;
        if (cfg.user[0]) user = cfg.user;
        if (cfg.port) { static char pb[8]; snprintf(pb, sizeof pb, "%d", cfg.port); port = pb; }
        if (cfg.identity[0] && !keyfile) keyfile = cfg.identity;
        free(t);
    }
    if (keyfile) {
        long n;
        char *t = readfile(keyfile, &n);
        int r;

        if (!t)
            die("cannot read the key file");
        r = ssh_key_read(t, n, passphrase, sk, 0, 0, work, sizeof work);
        zc_wipe(t, (size_t)n);
        free(t);
        if (r != SSH_KEY_OK) {
            fprintf(stderr, "ssh-client: the key file: error %d\n", r);
            exit(255);
        }
        have_key = 1;
    }
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai) != 0)
        die("unknown host");
    sock = socket(ai->ai_family, ai->ai_socktype, 0);
    if (sock < 0 || connect(sock, ai->ai_addr, ai->ai_addrlen) < 0)
        die("cannot connect");
    freeaddrinfo(ai);
    if (zc_rng_seed(&rng, seed, 0) < 0)
        die("no random seed");

    memset(&s, 0, sizeof s);
    rbuf_heap = malloc(SSH_RBUF_MIN);
    s.send = out_send;
    s.data = out_data;
    s.text = out_text;
    s.grow = grow;
    s.rng = &rng;
    s.version = "SSH-2.0-ZenixSSH_test";
    s.user = user ? user : "root";
    s.rbuf = rbuf_heap;
    s.rcap = SSH_RBUF_MIN;
    s.sbuf = sbuf;
    s.scap = sizeof sbuf;
    s.id_sk = have_key ? sk : 0;
    s.methods = (strchr(methods, 'k') ? SSH_AUTH_PUBKEY : 0) | (strchr(methods, 'i') ? SSH_AUTH_KBDINT : 0) |
                (strchr(methods, 'p') ? SSH_AUTH_PASSWORD : 0);
    s.pty = tty;
    s.term = "xterm-16color";
    s.cols = 80;
    s.rows = 32;
    s.command = *cmd ? cmd : 0;
    s.rekey_bytes = (unsigned long)rekey;
    ssh_start(&s);
    ssh_tick(&s, (unsigned long)time(0));

    while (s.state != SSH_ST_FAILED && s.state != SSH_ST_CLOSED) {
        struct pollfd p[2];
        int np = 1;

        /* the caller's answers */
        if (s.need == SSH_NEED_HOSTKEY) {
            long kn = 0, line = 0;
            char *kt = khfile ? readfile(khfile, &kn) : 0;
            int r = ssh_kh_check(kt ? kt : "", kn, host, atoi(port), s.hostkey, s.hostkeylen, &line);
            char fp[SSH_FP_LEN];

            free(kt);
            ssh_fingerprint(fp, s.hostkey, s.hostkeylen);
            if (r == SSH_KH_OK) {
                ssh_hostkey(&s, 1);
            } else if (r == SSH_KH_CHANGED || r == SSH_KH_REVOKED) {
                fprintf(stderr, "ssh-client: HOST KEY %s for %s (%s): %s line %ld\n",
                        r == SSH_KH_CHANGED ? "CHANGED" : "REVOKED", host, fp, khfile, line);
                ssh_hostkey(&s, 0);
            } else if (yes && khfile) {
                char l[512];
                long ln = ssh_kh_line(l, sizeof l, host, atoi(port), s.hostkey, s.hostkeylen);
                FILE *f = fopen(khfile, "a");

                fprintf(stderr, "ssh-client: new host %s, key %s: trusted\n", host, fp);
                if (!f || ln < 0 || fwrite(l, 1, (size_t)ln, f) != (size_t)ln)
                    die("cannot write known_hosts");
                fclose(f);
                ssh_hostkey(&s, 1);
            } else {
                fprintf(stderr, "ssh-client: unknown host %s, key %s: not trusted\n", host, fp);
                ssh_hostkey(&s, 0);
            }
        } else if (s.need == SSH_NEED_PASSWORD) {
            ssh_password(&s, pass);
        } else if (s.need == SSH_NEED_KBDINT) {
            const char *a[SSH_KBD_PROMPTS];
            int i;

            if (verbose)
                for (i = 0; i < s.kbd_n; i++)
                    fprintf(stderr, "ssh-client: prompt: %s\n", s.kbd_prompt[i]);
            for (i = 0; i < s.kbd_n; i++)
                a[i] = pass ? pass : "";
            ssh_kbdint(&s, pass ? a : 0);
        }
        if (s.state == SSH_ST_FAILED || s.state == SSH_ST_CLOSED)
            break;
        /* bytes held from the socket first (the core may have taken only some) */
        if (netoff < netn && s.need == SSH_NEED_NONE) {
            long k = ssh_input(&s, net + netoff, netn - netoff);

            if (k < 0)
                break;
            netoff += k;
            if (netoff < netn)
                continue;
        }
        if (s.need != SSH_NEED_NONE)
            continue;
        /* keys or data for the session */
        while (s.state == SSH_ST_SESSION && inoff < innum) {   /* as much as the window and the keys allow */
            long k = ssh_write(&s, inbuf + inoff, innum - inoff);

            if (!k)
                break;
            inoff += k;
        }
        if (s.state == SSH_ST_SESSION && !stdin_open && inoff == innum)
            ssh_eof(&s);
        p[0].fd = sock;
        p[0].events = POLLIN;
        if (s.state == SSH_ST_SESSION && stdin_open && inoff == innum) {
            p[1].fd = 0;
            p[1].events = POLLIN;
            np = 2;
        }
        if (poll(p, (nfds_t)np, 1000) < 0 && errno != EINTR)
            die("poll failed");
        ssh_tick(&s, (unsigned long)time(0));
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t k = read(sock, net, sizeof net);

            if (k <= 0) {
                if (s.state == SSH_ST_CLOSED)
                    break;
                die(s.state == SSH_ST_SESSION ? "the connection closed" : "the connection closed before the session");
            }
            netn = k;
            netoff = 0;
        }
        if (np == 2 && (p[1].revents & (POLLIN | POLLHUP))) {
            ssize_t k = read(0, inbuf, sizeof inbuf);

            if (k <= 0)
                stdin_open = 0;
            else {
                innum = k;
                inoff = 0;
            }
        }
    }
    if (verbose)
        fprintf(stderr, "ssh-client: kex %lu\n", s.kex_count);
    free(s.rbuf);
    if (s.state == SSH_ST_FAILED) {
        fprintf(stderr, "ssh-client: %s\n", s.err);
        ssh_wipe(&s);
        return 255;
    }
    ssh_wipe(&s);
    zc_wipe(sk, sizeof sk);
    return s.exit_status < 0 ? 255 : s.exit_status;
}
