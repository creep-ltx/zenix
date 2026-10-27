/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * net/ssh/'s parsers on their own (DESIGN D21 10 step 1; tools/host/crypto-test.sh part ssh runs it before the sshd
 * tests): the text cleaner, base64, known_hosts (patterns, negation, ports, hashed names, markers, damaged lines),
 * the config subset (first value wins, Match, quotes, "=", bad values), key files (damaged ones refused, a key
 * written and read back, protected), and the core's refusals before any server is involved.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../net/ssh/ssh.h"
#include "../../crypto/bearssl/inc/bearssl.h"
#include "../../crypto/monocypher/monocypher-ed25519.h"

static int fails, checks;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        printf("FAIL ssh-core-test: %s\n", what);
        fails++;
    }
}

static int clean_is(const char *in, long n, const char *want)
{
    char out[256];
    long k = ssh_clean(out, in, n);

    return k == (long)strlen(want) && !memcmp(out, want, (size_t)k);
}

static void test_clean(void)
{
    check(clean_is("plain text\tand tab\n", 19, "plain text\tand tab\n"), "clean: plain text kept");
    check(clean_is("a\x1b[31mred", 9, "a?[31mred"), "clean: ESC becomes ?");
    check(clean_is("line\r\nnext\rx", 12, "line\nnext?x"), "clean: CR LF -> LF, a lone CR -> ?");
    check(clean_is("\x7f\x07\x00z", 4, "???z"), "clean: DEL, BEL, NUL -> ?");
    check(clean_is("\xc2\x9b" "2J", 4, "??2J"), "clean: C1 CSI (U+009B in UTF-8) -> ? for each byte");
    check(clean_is("\x9b" "2J", 3, "?2J"), "clean: a raw 0x9B (the Amiga console's CSI) -> ?");
    check(clean_is("\xc3\xa5\xe2\x82\xac\xf0\x9f\x98\x80", 9, "\xc3\xa5\xe2\x82\xac\xf0\x9f\x98\x80"),
          "clean: valid UTF-8 (a-ring, euro, an emoji) kept");
    check(clean_is("\xc0\xaf\xe0\x80\xaf", 5, "?????"), "clean: overlong forms -> ? per byte");
    check(clean_is("\xed\xa0\x80", 3, "???"), "clean: a UTF-16 surrogate -> ?");
    check(clean_is("\xe2\x82", 2, "??"), "clean: a cut sequence -> ?");
}

static void test_b64(void)
{
    unsigned char b[64];
    char t[64];

    check(ssh_b64_decode(b, sizeof b, "aGVsbG8=", 8) == 5 && !memcmp(b, "hello", 5), "base64: decode with padding");
    check(ssh_b64_decode(b, sizeof b, "aGVsbG8", 7) == 5, "base64: decode without padding");
    check(ssh_b64_decode(b, sizeof b, "aGV*bG8=", 8) == -1, "base64: a bad character refused");
    check(ssh_b64_decode(b, sizeof b, "aGVsbG9=", 8) == -1, "base64: leftover bits refused");
    check(ssh_b64_decode(b, 3, "aGVsbG8=", 8) == -1, "base64: no room refused");
    check(ssh_b64_encode(t, sizeof t, (const unsigned char *)"hello", 5, 1) == 8 && !memcmp(t, "aGVsbG8=", 8),
          "base64: encode");
    check(ssh_b64_encode(t, sizeof t, (const unsigned char *)"hello", 5, 0) == 7, "base64: encode unpadded");
}

/* a hashed known_hosts name: |1|base64(salt)|base64(HMAC-SHA1(salt, name)) */
static void hashed_name(char *out, const char *name)
{
    unsigned char salt[20], mac[20];
    br_hmac_key_context k;
    br_hmac_context h;
    long n;

    memset(salt, 0x5c, 20);
    br_hmac_key_init(&k, &br_sha1_vtable, salt, 20);
    br_hmac_init(&h, &k, 0);
    br_hmac_update(&h, name, strlen(name));
    br_hmac_out(&h, mac);
    strcpy(out, "|1|");
    n = ssh_b64_encode(out + 3, 40, salt, 20, 1);
    out[3 + n] = '|';
    n += 4 + ssh_b64_encode(out + 4 + n, 40, mac, 20, 1);
    out[n] = 0;
}

static void test_known_hosts(void)
{
    unsigned char k1[SSH_ED25519_BLOB], k2[SSH_ED25519_BLOB], pk[32];
    char l1[200], l2[200], text[4096], hn[100];
    long line, n;

    memset(pk, 1, 32);
    ssh_key_blob(k1, pk);
    memset(pk, 2, 32);
    ssh_key_blob(k2, pk);
    ssh_pubkey_line(l1, sizeof l1, k1, SSH_ED25519_BLOB, 0);
    ssh_pubkey_line(l2, sizeof l2, k2, SSH_ED25519_BLOB, 0);

    n = snprintf(text, sizeof text, "# comment\n\npc.lan,10.0.0.5 %s\n*.test,!bad.test %s\n[pc.lan]:2222 %s\n", l1, l1, l2);
    check(ssh_kh_check(text, n, "pc.lan", 22, k1, SSH_ED25519_BLOB, &line) == SSH_KH_OK && line == 3, "kh: a plain name");
    check(ssh_kh_check(text, n, "PC.LAN", 22, k1, SSH_ED25519_BLOB, &line) == SSH_KH_OK, "kh: names in any case");
    check(ssh_kh_check(text, n, "10.0.0.5", 22, k1, SSH_ED25519_BLOB, &line) == SSH_KH_OK, "kh: the second name");
    check(ssh_kh_check(text, n, "pc.lan", 22, k2, SSH_ED25519_BLOB, &line) == SSH_KH_CHANGED && line == 3,
          "kh: another key of the same type: changed, its line");
    check(ssh_kh_check(text, n, "a.test", 22, k1, SSH_ED25519_BLOB, &line) == SSH_KH_OK, "kh: a * pattern");
    check(ssh_kh_check(text, n, "bad.test", 22, k1, SSH_ED25519_BLOB, &line) == SSH_KH_UNKNOWN, "kh: a !pattern");
    check(ssh_kh_check(text, n, "pc.lan", 2222, k2, SSH_ED25519_BLOB, &line) == SSH_KH_OK && line == 5,
          "kh: [host]:port");
    check(ssh_kh_check(text, n, "pc.lan", 2222, k1, SSH_ED25519_BLOB, &line) == SSH_KH_CHANGED,
          "kh: [host]:port with another key");
    check(ssh_kh_check(text, n, "other", 22, k1, SSH_ED25519_BLOB, &line) == SSH_KH_UNKNOWN, "kh: unknown host");
    n = snprintf(text, sizeof text, "pc ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAAQC7\n");
    check(ssh_kh_check(text, n, "pc", 22, k1, SSH_ED25519_BLOB, &line) == SSH_KH_OTHERTYPE,
          "kh: known with another key type only");
    n = snprintf(text, sizeof text, "@revoked * %s\npc %s\n@cert-authority pc %s\n", l1, l1, l2);
    check(ssh_kh_check(text, n, "pc", 22, k1, SSH_ED25519_BLOB, &line) == SSH_KH_REVOKED && line == 1,
          "kh: @revoked wins");
    check(ssh_kh_check(text, n, "pc", 22, k2, SSH_ED25519_BLOB, &line) == SSH_KH_CHANGED,
          "kh: @cert-authority lines are not keys");
    hashed_name(hn, "[pc.lan]:2200");
    n = snprintf(text, sizeof text, "%s %s\n", hn, l1);
    check(ssh_kh_check(text, n, "pc.lan", 2200, k1, SSH_ED25519_BLOB, &line) == SSH_KH_OK, "kh: a hashed name");
    check(ssh_kh_check(text, n, "pc.lan", 22, k1, SSH_ED25519_BLOB, &line) == SSH_KH_UNKNOWN,
          "kh: a hashed name is per port");
    n = snprintf(text, sizeof text, "pc\npc ssh-ed25519\npc ssh-ed25519 !!!!\n|1|zz|yy %s\npc %s", l1, l1);
    check(ssh_kh_check(text, n, "pc", 22, k1, SSH_ED25519_BLOB, &line) == SSH_KH_OK && line == 5,
          "kh: damaged lines skipped, the last line without a newline read");
    n = ssh_kh_line(text, sizeof text, "Pc.Lan", 2222, k1, SSH_ED25519_BLOB);
    check(n > 0 && !strncmp(text, "[pc.lan]:2222 ssh-ed25519 AAAA", 30) && text[n - 1] == '\n',
          "kh: the line written");
    check(ssh_kh_check(text, n, "pc.lan", 2222, k1, SSH_ED25519_BLOB, &line) == SSH_KH_OK, "kh: and read back");
}

static const char *last_report;
static long last_line;

static void report(void *u, long line, const char *what)
{
    (void)u;
    last_report = what;
    last_line = line;
}

static void test_config(void)
{
    struct ssh_cfg c;
    const char *t = "Port 2200\n"
                    "Host pc pc.*\n"
                    "  HostName=192.168.1.9\n"
                    "  user \"me too\"\n"
                    "  PORT 23\n"
                    "  IdentityFile ~/.ssh/id_pc\n"
                    "  ServerAliveInterval 30\n"
                    "Host pc\n"
                    "  User nobody\n"
                    "Match host x\n"
                    "  User matched\n"
                    "Host !nas *\n"
                    "  User everyone\n"
                    "  Port 99999\n"
                    "  Compression yes\n";
    int bad;

    bad = ssh_config(t, (long)strlen(t), "pc", &c, report, 0);
    check(!strcmp(c.hostname, "192.168.1.9"), "config: HostName=value");
    check(!strcmp(c.user, "me too"), "config: a quoted value; the first value wins");
    check(c.port == 2200, "config: Port before any Host applies to all (and wins)");
    check(!strcmp(c.identity, "~/.ssh/id_pc") && c.alive == 30, "config: IdentityFile, ServerAliveInterval");
    check(bad == 3 && last_line == 15, "config: Match, a bad Port and an unknown keyword reported");
    ssh_config(t, (long)strlen(t), "nas", &c, 0, 0);
    check(c.user[0] == 0 && c.hostname[0] == 0 && c.alive == -1, "config: a negated pattern excludes the host");
    ssh_config(t, (long)strlen(t), "other", &c, 0, 0);
    check(!strcmp(c.user, "everyone"), "config: Host *");
    bad = ssh_config("Host \"open\nUser x\n", 17, "open", &c, 0, 0);
    check(bad == 1, "config: an open quote reported");
}

static void test_keys(void)
{
    unsigned char seed[32], sk[64], pk[32], sk2[64], work[SSH_KEY_WORK], salt[16], chk[4] = { 1, 2, 3, 4 };
    static char text[4096];
    char comment[64];
    long n;
    int r;

    memset(seed, 9, 32);
    memset(salt, 7, 16);
    crypto_ed25519_key_pair(sk, pk, seed);
    n = ssh_key_write(text, sizeof text, sk, "me@zenix", 0, salt, 16, chk, work, sizeof work);
    check(n > 0 && !strncmp(text, "-----BEGIN OPENSSH " "PRIVATE KEY-----\n", 36), "keys: written");
    r = ssh_key_read(text, n, 0, sk2, comment, sizeof comment, work, sizeof work);
    check(r == SSH_KEY_OK && !memcmp(sk, sk2, 64) && !strcmp(comment, "me@zenix"), "keys: read back, comment");
    {
        long i;
        int z = 1;

        for (i = 0; i < (long)sizeof work; i++)
            z &= work[i] == 0;
        check(z, "keys: the work space is wiped after reading");
    }
    text[60] = text[60] == 'A' ? 'B' : 'A';            /* a changed character inside the base64 */
    r = ssh_key_read(text, n, 0, sk2, 0, 0, work, sizeof work);
    check(r != SSH_KEY_OK, "keys: a changed file refused");
    check(ssh_key_read("hello", 5, 0, sk2, 0, 0, work, sizeof work) == SSH_KEY_ENOTKEY, "keys: not a key");
    n = ssh_key_write(text, sizeof text, sk, "", "secret", salt, 2, chk, work, sizeof work);
    check(ssh_key_read(text, n, "secret", sk2, 0, 0, work, sizeof work) == SSH_KEY_OK && !memcmp(sk, sk2, 64),
          "keys: protected, read with the passphrase");
    check(ssh_key_read(text, n, "wrong", sk2, 0, 0, work, sizeof work) == SSH_KEY_EBADPASS, "keys: a wrong passphrase");
    check(ssh_key_read(text, n, 0, sk2, 0, 0, work, sizeof work) == SSH_KEY_ENEEDPASS, "keys: none given");
    check(ssh_key_write(text, sizeof text, sk, "", "x", salt, 100000, chk, work, sizeof work) == SSH_KEY_EROUNDS,
          "keys: too many rounds refused");
    check(ssh_key_write(text, 100, sk, "", 0, salt, 16, chk, work, sizeof work) == SSH_KEY_ESPACE,
          "keys: no room said so");
    {
        unsigned char blob[SSH_ED25519_BLOB], b2[100];
        char line[200], fp[SSH_FP_LEN];

        ssh_key_blob(blob, pk);
        n = ssh_pubkey_line(line, sizeof line, blob, SSH_ED25519_BLOB, "c o m");
        check(n > 0 && ssh_pubkey_parse(line, n, b2, sizeof b2) == SSH_ED25519_BLOB && !memcmp(b2, blob, 51),
              "keys: the public line, parsed back");
        line[0] = 'x';
        check(ssh_pubkey_parse(line, n, b2, sizeof b2) == SSH_KEY_EFORMAT, "keys: the type word must match the blob");
        ssh_fingerprint(fp, blob, SSH_ED25519_BLOB);
        check(strlen(fp) == 50 && !strncmp(fp, "SHA256:", 7), "keys: the fingerprint's form");
    }
}

/* the core with a scripted server: the bytes it sends back for a few bad starts */
static unsigned char sent[4096];
static long nsent;

static void cap_send(void *u, const unsigned char *b, long n)
{
    (void)u;
    if (nsent + n <= (long)sizeof sent)
        memcpy(sent + nsent, b, (size_t)n);
    nsent += n;
}

static int seed7(void *u, unsigned char *b, size_t n)
{
    (void)u;
    memset(b, 7, n);
    return 0;
}

static void start(struct ssh *s, struct zc_rng *r, unsigned char *rb, unsigned char *sb)
{
    memset(s, 0, sizeof *s);
    zc_rng_seed(r, seed7, 0);
    s->send = cap_send;
    s->rng = r;
    s->version = "SSH-2.0-Test";
    s->user = "u";
    s->rbuf = rb;
    s->rcap = SSH_RBUF_MIN;
    s->sbuf = sb;
    s->scap = SSH_SBUF_MIN;
    nsent = 0;
    ssh_start(s);
}

static void test_core(void)
{
    static struct ssh s;
    static unsigned char rb[SSH_RBUF_MIN], sb[SSH_SBUF_MIN], big[2000];
    struct zc_rng r;
    long k, i;

    start(&s, &r, rb, sb);
    check(nsent == 14 && !memcmp(sent, "SSH-2.0-Test\r\n", 14), "core: our version line");
    k = ssh_input(&s, (const unsigned char *)"Hello there\r\nSSH-1.5-old\r\n", 26);
    check(k == -1 && s.state == SSH_ST_FAILED && strstr(s.err, "SSH 2"), "core: SSH 1 refused");
    start(&s, &r, rb, sb);
    memset(big, 'x', sizeof big);
    k = ssh_input(&s, big, sizeof big);
    check(k == -1 && strstr(s.err, "255"), "core: a line over 255 bytes refused");
    start(&s, &r, rb, sb);
    for (i = 0; i < 1025 && s.state != SSH_ST_FAILED; i++)
        ssh_input(&s, (const unsigned char *)"junk\n", 5);
    check(s.state == SSH_ST_FAILED && i == 1025 && strstr(s.err, "1,024"), "core: at most 1,024 lines before");
    start(&s, &r, rb, sb);
    k = ssh_input(&s, (const unsigned char *)"SSH-2.0-X\r\n\x00\x00\x90\x00", 15);
    check(k == -1 && strstr(s.err, "packet length"), "core: a packet over 35,000 bytes ends it");
    start(&s, &r, rb, sb);
    k = ssh_input(&s, (const unsigned char *)"SSH-2.0-X\r\n\x00\x00\x00\x0c\x06\x02\x00\x00\x00\x00" "\0\0\0\0\0\0", 27);
    check(k == -1 && strstr(s.err, "not KEXINIT"), "core: a first packet other than KEXINIT ends it");
    check(nsent > 14 && sent[14 + 5] == 20, "core: our KEXINIT went out after the version lines");
    {
        static struct ssh s2;
        struct zc_rng r2;

        memset(&s2, 0, sizeof s2);
        memset(&r2, 0, sizeof r2);
        s2.send = cap_send;
        s2.rng = &r2;
        s2.version = "SSH-2.0-T";
        s2.rbuf = rb;
        s2.rcap = SSH_RBUF_MIN;
        s2.sbuf = sb;
        s2.scap = SSH_SBUF_MIN;
        ssh_start(&s2);
        check(s2.state == SSH_ST_FAILED, "core: an unseeded generator refused at the start");
    }
}

int main(void)
{
    test_clean();
    test_b64();
    test_known_hosts();
    test_config();
    test_keys();
    test_core();
    printf("ssh-core-test: %d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
