/* SPDX-License-Identifier: GPL-2.0-only */
/* ssh-keygen: the Amiga's own ssh key (DESIGN D21 4.4, docs/man/ssh-keygen.txt), in OpenSSH's formats.
 *   ssh-keygen [-t ed25519] [-f FILE] [-C COMMENT] [-N PASS] [-a ROUNDS] [-q]   make a key pair
 *   ssh-keygen -l [-f FILE]          the fingerprint of a key (private or .pub)
 *   ssh-keygen -y [-f FILE]          the public key line of a private key
 *   ssh-keygen -R HOST [-f FILE]     remove HOST's lines from known_hosts
 * The default FILE is ~/.ssh/id_ed25519 (~/.ssh made with mode 700, the key 600, FILE.pub 644). No passphrase by
 * default (owner decision 3, D21 13); -N PASS protects it with bcrypt (minutes on a 68020: -a 16 is OpenSSH's). */
#include "libz.h"
#include "../net/ssh/ssh.h"
#include "../crypto/monocypher/monocypher-ed25519.h"

static char home_ssh[ZPATH_MAX];

static int seed(void *u, unsigned char *b, size_t n)
{
    (void)u;
    int fd = sys_open("/dev/random", O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return fd;
    int r = sys_read(fd, b, (u32)n);
    if (r == -EAGAIN) {
        fprintf(2, "ssh-keygen: waiting for the random pool (type on the keyboard)\n");
        sys_close(fd);
        fd = sys_open("/dev/random", O_RDONLY);
        if (fd < 0)
            return fd;
        r = sys_read(fd, b, (u32)n);
    }
    sys_close(fd);
    return r == (int)n ? 0 : r < 0 ? r : -EIO;
}

static char *readfile(const char *path, long *n)
{
    *n = 0;
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    struct ustat st;
    if (sys_fstat(fd, &st) < 0 || st.size > (1u << 20)) { sys_close(fd); return 0; }
    char *b = malloc(st.size + 1);
    long got = 0;
    if (b)
        while (got < (long)st.size) {
            int r = sys_read(fd, b + got, st.size - (u32)got);
            if (r <= 0) break;
            got += r;
        }
    sys_close(fd);
    if (b) b[got] = 0;
    *n = got;
    return b;
}

static int writefile(const char *path, const char *data, long n, u32 mode)
{
    int fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return fd;
    sys_chmod(path, mode);                      /* before the secret goes in */
    int w = sys_write(fd, data, (u32)n);
    sys_close(fd);
    return w == (int)n ? 0 : -EIO;
}

static int die(const char *what, const char *path)
{
    fprintf(2, "ssh-keygen: %s%s%s\n", path ? path : "", path ? ": " : "", what);
    return 1;
}

static const char *keyerr(int r)
{
    return r == SSH_KEY_ETYPE ? "not an Ed25519 key" : r == SSH_KEY_EBADPASS ? "wrong passphrase" :
           r == SSH_KEY_ENOTKEY ? "not an OpenSSH key" : r == SSH_KEY_ENEEDPASS ? "protected: give the passphrase with -P" :
           r == SSH_KEY_ECIPHER ? "a cipher this ssh does not read" : "a damaged key file";
}

/* the public blob of FILE: a .pub line, or the private key's */
static int blob_of(const char *file, const char *pass, unsigned char *blob, char *comment, int ccap)
{
    long n;
    char *t = readfile(file, &n);
    if (!t)
        return die(zwhy("cannot read"), file);
    long b = ssh_pubkey_parse(t, n, blob, SSH_ED25519_BLOB + 16);
    if (b == SSH_ED25519_BLOB) {
        const char *c = t;                      /* the comment: the third word */
        for (int w = 0; w < 2 && *c; w++) { while (*c && *c != ' ') c++; while (*c == ' ') c++; }
        int k = 0;
        while (c[k] && c[k] != '\n' && c[k] != '\r' && k < ccap - 1) { comment[k] = c[k]; k++; }
        comment[k] = 0;
        free(t);
        return 0;
    }
    unsigned char sk[64];
    static unsigned char work[SSH_KEY_WORK];
    int r = ssh_key_read(t, n, pass, sk, comment, ccap, work, sizeof work);
    zc_wipe(t, (u32)n);
    free(t);
    if (r != SSH_KEY_OK)
        return die(keyerr(r), file);
    ssh_key_blob(blob, sk + 32);
    zc_wipe(sk, sizeof sk);
    return 0;
}

/* -R HOST: known_hosts without the lines naming HOST (plain names; [host]:port too), the old file kept as .old */
static int remove_host(const char *file, const char *hostname)
{
    long n;
    char *t = readfile(file, &n);
    if (!t)
        return die(zwhy("cannot read"), file);
    char *out = malloc((u32)n + 1);
    long o = 0;
    int removed = 0;
    if (!out)
        return die("out of memory", 0);
    for (char *l = t; *l;) {
        char *e = strchr(l, '\n');
        long len = e ? e - l + 1 : (long)strlen(l);
        int hit = 0;
        if (*l != '#' && *l != '|') {
            const char *p = l;
            if (*p == '@') { while (*p && *p != ' ') p++; while (*p == ' ') p++; }   /* a marker */
            const char *end = p;
            while (*end && *end != ' ' && *end != '\n') end++;
            for (const char *q = p; q < end;) {         /* the comma-separated names */
                const char *c = q;
                while (c < end && *c != ',') c++;
                long k = c - q;
                u32 hl = strlen(hostname);
                if ((k == (long)hl && !memcmp(q, hostname, hl)) ||
                    (k > (long)hl + 2 && q[0] == '[' && !memcmp(q + 1, hostname, hl) && q[hl + 1] == ']'))
                    hit = 1;
                q = c + 1;
            }
        }
        if (hit) removed++;
        else { memcpy(out + o, l, (u32)len); o += len; }
        l += len;
    }
    if (!removed) {
        printf("Host %s not found in %s\n", hostname, file);
        return 1;
    }
    char old[ZPATH_MAX];
    snprintf(old, sizeof old, "%s.old", file);
    sys_unlink(old);
    if (writefile(old, t, n, 0644) < 0 || writefile(file, out, o, 0644) < 0)
        return die(zwhy("cannot write"), file);
    printf("# Host %s found: %d line%s removed\n%s updated.\nOriginal contents retained as %s\n", hostname, removed,
           removed == 1 ? "" : "s", file, old);
    return 0;
}

static int usage(void)
{
    fprintf(2, "usage: ssh-keygen [-t ed25519] [-f FILE] [-C COMMENT] [-N PASS] [-a ROUNDS] [-q]\n"
               "       ssh-keygen -l [-f FILE]    ssh-keygen -y [-f FILE] [-P PASS]    ssh-keygen -R HOST [-f FILE]\n");
    return 2;
}

int main(int argc, char **argv)
{
    const char *file = 0, *comment = 0, *pass = "", *rhost = 0;
    int list = 0, pub = 0, quiet = 0, rounds = 16;
    for (int i = 1; i < argc; i++) {
        const char *o = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(o, "-t") && more) { if (strcmp(argv[++i], "ed25519")) { fprintf(2, "ssh-keygen: only ed25519 keys\n"); return 2; } }
        else if (!strcmp(o, "-f") && more) file = argv[++i];
        else if (!strcmp(o, "-C") && more) comment = argv[++i];
        else if ((!strcmp(o, "-N") || !strcmp(o, "-P")) && more) pass = argv[++i];
        else if (!strcmp(o, "-a") && more) rounds = atoi(argv[++i]);
        else if (!strcmp(o, "-R") && more) rhost = argv[++i];
        else if (!strcmp(o, "-l")) list = 1;
        else if (!strcmp(o, "-y")) pub = 1;
        else if (!strcmp(o, "-q")) quiet = 1;
        else return usage();
    }
    if (rounds < 1 || rounds > SSH_KEY_ROUNDS_MAX)
        return die("-a: rounds from 1 to 1024", 0);
    const char *home = getenv("HOME");
    snprintf(home_ssh, sizeof home_ssh, "%s/.ssh", home && strcmp(home, "/") ? home : "");
    static char def[ZPATH_MAX];
    if (rhost) {
        snprintf(def, sizeof def, "%s/known_hosts", home_ssh);
        return remove_host(file ? file : def, rhost);
    }
    snprintf(def, sizeof def, "%s/id_ed25519", home_ssh);
    if (!file)
        file = def;
    if (list || pub) {
        unsigned char blob[SSH_ED25519_BLOB + 16];
        char c[128], fp[SSH_FP_LEN], line[512];
        if (blob_of(file, *pass ? pass : 0, blob, c, sizeof c))
            return 1;
        if (list) {
            ssh_fingerprint(fp, blob, SSH_ED25519_BLOB);
            printf("256 %s %s (ED25519)\n", fp, c[0] ? c : "no comment");
        } else if (ssh_pubkey_line(line, sizeof line, blob, SSH_ED25519_BLOB, c) > 0)
            printf("%s\n", line);
        return 0;
    }

    /* a new key pair */
    struct ustat st;
    if (sys_stat(file, &st) == 0) {
        fprintf(2, "ssh-keygen: %s already exists: not overwritten (remove it first)\n", file);
        return 1;
    }
    if (file == def && sys_stat(home_ssh, &st) < 0) {
        if (sys_mkdir(home_ssh) < 0)
            return die(zwhy("cannot make it"), home_ssh);
        sys_chmod(home_ssh, 0700);
    }
    static char cbuf[96];
    if (!comment) {
        const char *u = getenv("USER"), *hn = getenv("HOSTNAME");     /* as hostname(1) */
        snprintf(cbuf, sizeof cbuf, "%s@%s", u ? u : "root", hn ? hn : "zenix");
        comment = cbuf;
    }
    struct zc_rng rng;
    int e = zc_rng_seed(&rng, seed, 0);
    if (e < 0)
        return die("no random numbers", 0);
    unsigned char sd[32], sk[64], pk[32], salt[16], chk[4], blob[SSH_ED25519_BLOB];
    zc_rng_bytes(&rng, sd, 32);
    zc_rng_bytes(&rng, salt, 16);
    zc_rng_bytes(&rng, chk, 4);
    zc_rng_wipe(&rng);
    if (!quiet)
        printf("Generating public/private ed25519 key pair.\n");
    crypto_ed25519_key_pair(sk, pk, sd);
    if (*pass && !quiet)
        printf("Protecting it with the passphrase (bcrypt, %d rounds: this takes a while)...\n", rounds);
    static char text[2048], line[512], pubname[ZPATH_MAX];
    static unsigned char work[SSH_KEY_WORK];
    long n = ssh_key_write(text, sizeof text, sk, comment, pass, salt, (unsigned)rounds, chk, work, sizeof work);
    zc_wipe(sk, sizeof sk);
    if (n < 0)
        return die("the key does not fit", 0);
    if (snprintf(pubname, sizeof pubname, "%s.pub", file) >= (int)sizeof pubname)
        return die("the name is too long", file);
    if (writefile(file, text, n, 0600) < 0) {
        zc_wipe(text, sizeof text);
        return die(zwhy("cannot write"), file);
    }
    zc_wipe(text, sizeof text);
    ssh_key_blob(blob, pk);
    long ln = ssh_pubkey_line(line, sizeof line - 1, blob, SSH_ED25519_BLOB, comment);
    if (ln < 0)
        return die("the public key does not fit", 0);
    line[ln++] = '\n';
    if (writefile(pubname, line, ln, 0644) < 0)
        return die(zwhy("cannot write"), pubname);
    if (!quiet) {
        char fp[SSH_FP_LEN];
        ssh_fingerprint(fp, blob, SSH_ED25519_BLOB);
        printf("Your identification has been saved in %s\nYour public key has been saved in %s\n"
               "The key fingerprint is:\n%s %s\n", file, pubname, fp, comment);
    }
    return 0;
}
