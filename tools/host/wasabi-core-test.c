/* SPDX-License-Identifier: GPL-2.0-only */
/* wasabid's pure parts (net/wasabi/pure.c, DESIGN D24) on the PC: /etc/wasabid.conf and its refusals, the address
 * filter, the frame receiver (pieces, TLS, sizes), the payload fields, the file mapping (AmigaDOS bits, DateStamps,
 * PUT's modes), the guard's names, the error codes of the main session's decision 2 and the formatter. Built and run
 * by tools/host/wasabi-test.sh core. Prints the failures; exit status 0 when there are none. */
#include <stdio.h>
#include <string.h>
#include "../../net/wasabi/wasabi_int.h"

struct wsb g;                                   /* pure.c needs none of it; the formatter's header names it */
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL wasabi-core-test: " __VA_ARGS__); printf("\n"); } } while (0)

static int conf(const char *t, struct wsb_conf *c, int *line, const char **why)
{
    return wsb_conf_parse(c, t, (uint32_t)strlen(t), line, why);
}

static void t_conf(void)
{
    struct wsb_conf c;
    int line;
    const char *why = "";
    CHECK(!conf("# the remote control\n  key  0123456789abcdef  \nport 1240\nallow 100.64.0.0/10\nallow 8.8.8.8\n"
                "name a1200-zenix\n\n", &c, &line, &why), "a good file: %s", why);
    CHECK(!strcmp(c.key, "0123456789abcdef") && c.port == 1240 && c.nallow == 2 && !c.allow_any &&
          c.allow_net[0] == 0x64400000u && c.allow_mask[0] == 0xFFC00000u && c.allow_net[1] == 0x08080808u &&
          c.allow_mask[1] == 0xFFFFFFFFu && !strcmp(c.name, "a1200-zenix"), "a good file's fields");
    CHECK(!conf("key 0123456789abcdef\r\n", &c, &line, &why) && c.port == WSB_PORT && !strcmp(c.name, "zenix"),
          "the defaults: port 1236, name zenix (and a CR at the end)");
    CHECK(!conf("key with spaces in it 1234\nallow any\n", &c, &line, &why) && c.allow_any &&
          !strcmp(c.key, "with spaces in it 1234"), "allow any; a key with spaces");
    static const struct { const char *t; int line; const char *why; } bad[] = {
        { "key 0123456789abcde\n", 1, "shorter" },
        { "key 0123456789abcdef\nkey 0123456789abcdef\n", 2, "second key" },
        { "port 1\n", 0, "no key" },
        { "key 0123456789abcdef\nport 0\n", 2, "port" },
        { "key 0123456789abcdef\nport 65536\n", 2, "port" },
        { "key 0123456789abcdef\nport 12a\n", 2, "port" },
        { "key 0123456789abcdef\nport 1\nport 2\n", 3, "second port" },
        { "key 0123456789abcdef\nname two words\n", 2, "name" },
        { "key 0123456789abcdef\nname 0123456789abcdef0123456789abcdef\n", 2, "name" },
        { "key 0123456789abcdef\nallow 1.2.3\n", 2, "CIDR" },
        { "key 0123456789abcdef\nallow 1.2.3.4/33\n", 2, "CIDR" },
        { "key 0123456789abcdef\nallow 256.1.1.1\n", 2, "CIDR" },
        { "key 0123456789abcdef\nallow 1.2.3.4 x\n", 2, "CIDR" },
        { "key 0123456789abcdef\nkeys 1\n", 2, "a word this file does not have" },
        { "key 0123456789abcdef\nallow 1.1.1.1\nallow 1.1.1.1\nallow 1.1.1.1\nallow 1.1.1.1\nallow 1.1.1.1\n"
          "allow 1.1.1.1\nallow 1.1.1.1\nallow 1.1.1.1\nallow 1.1.1.1\n", 10, "more than 8" },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int r = conf(bad[i].t, &c, &line, &why);
        CHECK(r == -EINVAL && line == bad[i].line && strstr(why, bad[i].why), "bad file %u: %d line %d (%s)", i, r,
              line, why);
    }
    char k[200];
    memset(k, 'k', sizeof k);
    memcpy(k, "key ", 4);
    k[4 + 128] = 0;
    CHECK(conf(k, &c, &line, &why) == -EINVAL && strstr(why, "longer"), "a key of 128 characters");
    k[4 + 127] = 0;
    CHECK(!conf(k, &c, &line, &why) && strlen(c.key) == 127, "a key of 127 characters");
    CHECK(wsb_conf_parse(&c, "key 0123456789abcdef\0x\n", 23, &line, &why) == -EINVAL && strstr(why, "NUL"), "a NUL");
    char longl[300];
    memset(longl, 'x', sizeof longl);
    longl[299] = 0;
    CHECK(conf(longl, &c, &line, &why) == -EINVAL && strstr(why, "too long"), "a line over 159 bytes");
}

static void t_allowed(void)
{
    struct wsb_conf c;
    memset(&c, 0, sizeof c);
    static const struct { uint32_t a; int ok; } t[] = {
        { 0x0A010203, 1 }, { 0xAC100001, 1 }, { 0xAC1FFFFF, 1 }, { 0xAC200001, 0 }, { 0xAC0F0001, 0 },
        { 0xC0A80101, 1 }, { 0xC0A90101, 0 }, { 0x7F000001, 1 }, { 0xA9FE0101, 1 }, { 0x08080808, 0 },
        { 0x64400001, 0 }, { 0x00000000, 0 }, { 0xFFFFFFFF, 0 },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        CHECK(wsb_allowed(&c, t[i].a) == t[i].ok, "the address filter: %08x", t[i].a);
    c.nallow = 1;
    c.allow_net[0] = 0x08080800;
    c.allow_mask[0] = 0xFFFFFF00;
    CHECK(wsb_allowed(&c, 0x080808FE) && !wsb_allowed(&c, 0x08080901), "an allow line");
    c.allow_any = 1;
    CHECK(wsb_allowed(&c, 0x08080901), "allow any");
}

/* feed in pieces of `step`; collect what comes out */
static uint8_t outbuf[200000];
static uint32_t outn;
static int frames, pieces;
static int feed(struct wsb_rx *r, const uint8_t *in, uint32_t n, uint32_t step, int stream_after_put)
{
    frames = pieces = 0;
    outn = 0;
    for (uint32_t i = 0; i < n;) {
        uint32_t k = n - i < step ? n - i : step;
        while (k) {
            uint32_t used, len;
            uint8_t tag;
            const uint8_t *piece;
            int last, x = wsb_rx_feed(r, in + i, k, &used, &tag, &piece, &len, &last);
            if (x < 0)
                return x;
            i += used;
            k -= used;
            if (x == WSB_RX_FRAME) {
                frames++;
                outbuf[outn++] = tag;
                memcpy(outbuf + outn, piece, len);
                outn += len;
                if (stream_after_put && tag == WT_PUT)
                    r->stream = 1;
                if (tag == WT_END)
                    r->stream = 0;
            } else if (x == WSB_RX_PIECE) {
                pieces++;
                memcpy(outbuf + outn, piece, len);
                outn += len;
            }
        }
    }
    return 0;
}
static uint32_t frame(uint8_t *p, uint8_t tag, uint32_t len, uint8_t fill)
{
    p[0] = tag;
    put32(p + 1, len);
    memset(p + 5, fill, len);
    return 5 + len;
}

static void t_rx(void)
{
    static uint8_t in[150000], buf[WSB_SMALL];
    struct wsb_rx r;
    uint32_t n = 0;
    n += frame(in + n, WT_HELLO, 10, 1);
    n += frame(in + n, WT_PING, 0, 0);
    n += frame(in + n, WT_RUN, WSB_SMALL, 2);
    for (uint32_t step = 1; step < 9000; step = step * 3 + 1) {
        wsb_rx_init(&r, buf);
        CHECK(!feed(&r, in, n, step, 0) && frames == 3 && outn == 3 + 10 + WSB_SMALL && outbuf[0] == WT_HELLO &&
              outbuf[11] == WT_PING && outbuf[12] == WT_RUN && outbuf[13 + WSB_SMALL - 1] == 2,
              "three frames in pieces of %u", step);
    }
    wsb_rx_init(&r, buf);
    uint8_t tls[] = { 0x16, 3, 1, 0, 5 };
    CHECK(feed(&r, tls, 5, 5, 0) == WSB_RX_TLS, "0x16 first: TLS");
    wsb_rx_init(&r, buf);
    uint8_t ok16[] = { WT_PING, 0, 0, 0, 0, 0x16, 0, 0, 0, 0 };
    CHECK(!feed(&r, ok16, 10, 10, 0) && frames == 2, "0x16 as a later tag is only a tag");
    wsb_rx_init(&r, buf);
    n = frame(in, WT_RUN, WSB_SMALL + 1, 0);
    CHECK(feed(&r, in, n, n, 0) == WSB_RX_BIG, "a command over 4 KB");
    wsb_rx_init(&r, buf);
    uint8_t huge[] = { WT_DATA, 0, 1, 0, 1 };
    r.stream = 1;
    CHECK(feed(&r, huge, 5, 5, 0) == WSB_RX_BIG, "DATA over 64 KB, even in a body");
    wsb_rx_init(&r, buf);
    n = frame(in, WT_DATA, 5000, 0);
    CHECK(feed(&r, in, n, n, 0) == WSB_RX_BIG, "DATA over 4 KB outside a body");
    /* a body: PUT, DATA 65536 and 3, END: the DATA in pieces, never buffered */
    wsb_rx_init(&r, buf);
    n = frame(in, WT_PUT, 12, 9);
    n += frame(in + n, WT_DATA, 65536, 7);
    n += frame(in + n, WT_DATA, 3, 8);
    n += frame(in + n, WT_END, 0, 0);
    n += frame(in + n, WT_PING, 0, 0);
    CHECK(!feed(&r, in, n, 1000, 1) && frames == 3 && pieces >= 66 && outn == 1 + 12 + 65539 + 1 + 1 &&
          outbuf[13] == 7 && outbuf[13 + 65536] == 8 && outbuf[outn - 1] == WT_PING && !r.stream,
          "a body's DATA as pieces (%d frames, %d pieces, %u bytes)", frames, pieces, outn);
    wsb_rx_init(&r, buf);
    n = frame(in, WT_PUT, 0, 0);
    n += frame(in + n, WT_DATA, 0, 0);
    n += frame(in + n, WT_END, 0, 0);
    CHECK(!feed(&r, in, n, 1, 1) && frames == 3 && !pieces, "an empty DATA in a body is a frame");
}

static void t_fields(void)
{
    uint8_t p[] = { 0, 0, 0, 7, 0, 3, 'a', 'b', 'c', 0, 2, 'x', 0, 0, 9 };
    uint32_t off = 0, v;
    char s[8];
    CHECK(!wsb_get32(p, sizeof p, &off, &v) && v == 7 && off == 4, "u32");
    CHECK(!wsb_getstr(p, sizeof p, &off, s, sizeof s) && !strcmp(s, "abc") && off == 9, "str");
    CHECK(wsb_getstr(p, sizeof p, &off, s, sizeof s) == -EINVAL && off == 9, "a str with a NUL inside");
    off = 4;
    CHECK(wsb_getstr(p, sizeof p, &off, s, 3) == -ENAMETOOLONG, "a str too long for its buffer");
    off = 13;
    CHECK(wsb_getstr(p, sizeof p, &off, s, sizeof s) == -EINVAL, "a str past the end");
    off = 14;
    CHECK(wsb_get16(p, sizeof p, &off, &v) == -EINVAL && wsb_get32(p, 2, &off, &v) == -EINVAL, "past the end");
    off = 0xFFFFFFF0u;
    CHECK(wsb_get32(p, sizeof p, &off, &v) == -EINVAL, "an offset beyond the length");
}

static void t_files(void)
{
    CHECK(wsb_mode_to_prot(0100644) == (2u | 1u << 11 | 1u << 15), "0644 -> ----rw-d, group and other r");
    CHECK(wsb_mode_to_prot(0755) == (1u << 11 | 1u << 9 | 1u << 15 | 1u << 13), "0755 -> rwed");
    CHECK(wsb_mode_to_prot(0) == 15, "0000 -> nothing allowed");
    CHECK(wsb_mode_to_prot(0777) == (0xFFu << 8 & 0xFF00u), "0777: every group and other bit");
    CHECK(wsb_prot_to_mode(0xFFFFFFFFu, 0, 0) == 0644 && wsb_prot_to_mode(0xFFFFFFFFu, 0, 1) == 0755 &&
          wsb_prot_to_mode(0xFFFFFFFFu, 0100600, 1) == 0600, "0xFFFFFFFF: the old mode, else 0755/0644");
    CHECK(wsb_prot_to_mode(0x80000000u | 04750, 0644, 0) == 04750, "bit 31: a Unix mode");
    CHECK(wsb_prot_to_mode(0, 0, 0) == 0755 && wsb_prot_to_mode(2, 0, 1) == 0644 && wsb_prot_to_mode(4, 0, 0) == 0555 &&
          wsb_prot_to_mode(15, 0, 0) == 0444, "AmigaDOS's bits: e -> x for all, w -> the owner's w");
    uint32_t d, m, t;
    wsb_datestamp(252460800u, &d, &m, &t);
    CHECK(!d && !m && !t, "1978-01-01 is DateStamp 0");
    wsb_datestamp(252460800u + 86400 + 61, &d, &m, &t);
    CHECK(d == 1 && m == 1 && t == 50, "a day, a minute and a second");
    wsb_datestamp(1000, &d, &m, &t);
    CHECK(!d && !m && !t, "before 1978: 0");
    wsb_datestamp(0xFFFFFFFFu, &d, &m, &t);
    CHECK(d == (0xFFFFFFFFu - 252460800u) / 86400 && t < 3000, "2106");
    CHECK(wsb_guarded("zup") && wsb_guarded("fsck.ext2") && wsb_guarded("mkfs.zenixfs") && wsb_guarded("dd") &&
          wsb_guarded("fdisk") && !wsb_guarded("zupx") && !wsb_guarded("fsck") && !wsb_guarded("ddd") &&
          !wsb_guarded("sh"), "the guard's names");
}

static void t_codes(void)
{
    static const struct { int e, w; uint32_t code; } t[] = {
        { ENOENT, 0, 205 }, { -ENOENT, 1, 205 }, { EEXIST, 1, 203 }, { EACCES, 1, 223 }, { EACCES, 0, 224 },
        { EPERM, 1, 223 }, { EPERM, 0, 224 }, { ENOSPC, 1, 221 }, { ENOTEMPTY, 1, 216 }, { ENOTDIR, 0, 212 },
        { EISDIR, 0, 212 }, { EBUSY, 1, 202 }, { ETXTBSY, 1, 202 }, { EROFS, 1, 214 }, { EIO, 0, 0 }, { 0, 0, 0 },
        { EINVAL, 1, 0 }, { ENAMETOOLONG, 0, 0 },
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
        CHECK(wsb_errcode(t[i].e, t[i].w) == t[i].code, "error %d (%s): code %u", t[i].e, t[i].w ? "write" : "read",
              t[i].code);
}

static void t_fmt(void)
{
    char s[64];
    struct wsb_buf b = { s, 0, sizeof s };
    bu(&b, 0); bc(&b, ' '); bu(&b, 4294967295u); bc(&b, ' '); bi(&b, -5); bc(&b, ' '); bi(&b, -2147483647 - 1);
    bc(&b, ' '); bx8(&b, 0x1fab); bc(&b, ' '); bip(&b, 0x0A141E28);
    CHECK(!strcmp(s, "0 4294967295 -5 -2147483648 0x00001fab 10.20.30.40"), "the formatter: %s", s);
    char t[5];
    struct wsb_buf c = { t, 0, sizeof t };
    bs(&c, "abcdefgh");
    CHECK(!strcmp(t, "abcd") && c.n == 4, "the formatter stops at its end");
    uint8_t q[4];
    put32(q, 0x01020304);
    CHECK(q[0] == 1 && q[3] == 4, "put32");
}

int main(void)
{
    t_conf();
    t_allowed();
    t_rx();
    t_fields();
    t_files();
    t_codes();
    t_fmt();
    printf("wasabi-core-test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
