/* SPDX-License-Identifier: GPL-2.0-only */
/* wasabid's pure parts (DESIGN D24): no OS layer call here, so the tests and the fuzzer call them directly. */
#include "wasabi_int.h"

/* ---- the formatter ---- */
void bsn(struct wsb_buf *b, const char *s, uint32_t n)
{
    while (n-- && b->n + 1 < b->cap)
        b->p[b->n++] = *s++;
    if (b->cap)
        b->p[b->n] = 0;
}
void bs(struct wsb_buf *b, const char *s) { bsn(b, s, (uint32_t)strlen(s)); }
void bc(struct wsb_buf *b, char ch) { bsn(b, &ch, 1); }
void bu(struct wsb_buf *b, uint32_t v)
{
    char t[11];
    int i = 11;
    do t[--i] = (char)('0' + v % 10); while (v /= 10);
    bsn(b, t + i, (uint32_t)(11 - i));
}
void bi(struct wsb_buf *b, int v)
{
    if (v < 0) { bc(b, '-'); bu(b, 0u - (uint32_t)v); } else bu(b, (uint32_t)v);
}
void bx8(struct wsb_buf *b, uint32_t v)
{
    char t[10] = "0x";
    for (int i = 0; i < 8; i++)
        t[2 + i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    bsn(b, t, 10);
}
void bip(struct wsb_buf *b, uint32_t a)
{
    for (int i = 3; i >= 0; i--) {
        bu(b, (a >> (8 * i)) & 255);
        if (i)
            bc(b, '.');
    }
}
void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* ---- the payload's fields ---- */
int wsb_get32(const uint8_t *p, uint32_t len, uint32_t *off, uint32_t *v)
{
    if (*off > len || len - *off < 4)
        return -EINVAL;
    *v = (uint32_t)p[*off] << 24 | (uint32_t)p[*off + 1] << 16 | (uint32_t)p[*off + 2] << 8 | p[*off + 3];
    *off += 4;
    return 0;
}
int wsb_get16(const uint8_t *p, uint32_t len, uint32_t *off, uint32_t *v)
{
    if (*off > len || len - *off < 2)
        return -EINVAL;
    *v = (uint32_t)p[*off] << 8 | p[*off + 1];
    *off += 2;
    return 0;
}
int wsb_getstr(const uint8_t *p, uint32_t len, uint32_t *off, char *out, uint32_t cap)
{
    uint32_t n, o = *off;
    if (wsb_get16(p, len, &o, &n))
        return -EINVAL;
    if (len - o < n)
        return -EINVAL;
    if (n >= cap)
        return -ENAMETOOLONG;
    memcpy(out, p + o, n);
    out[n] = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!out[i])                            /* a NUL inside would cut a path short: refused, not truncated */
            return -EINVAL;
    *off = o + n;
    return 0;
}

/* ---- frames ---- */
void wsb_rx_init(struct wsb_rx *r, uint8_t *buf)
{
    memset(r, 0, sizeof *r);
    r->buf = buf;
    r->st = RX_FIRST;
}
int wsb_rx_feed(struct wsb_rx *r, const uint8_t *in, uint32_t n, uint32_t *used, uint8_t *tag,
                const uint8_t **piece, uint32_t *len, int *last)
{
    *used = 0;
    while (*used < n) {
        uint32_t k, avail = n - *used;
        if (r->st == RX_FIRST) {
            if (in[*used] == 0x16)
                return WSB_RX_TLS;
            r->st = RX_HDR;
            r->got = 0;
        }
        if (r->st == RX_HDR) {
            r->hdr[r->got++] = in[(*used)++];
            if (r->got < 5)
                continue;
            r->tag = r->hdr[0];
            r->len = (uint32_t)r->hdr[1] << 24 | (uint32_t)r->hdr[2] << 16 | (uint32_t)r->hdr[3] << 8 | r->hdr[4];
            r->got = 0;
            if (r->len > WSB_MAXPAYLOAD)
                return WSB_RX_BIG;
            if (r->stream && r->tag == WT_DATA && r->len) {
                r->st = RX_BODY;
                continue;
            }
            if (r->len > WSB_SMALL)
                return WSB_RX_BIG;
            r->st = RX_SMALL;
            if (r->len)
                continue;
        } else if (r->st == RX_SMALL) {
            k = r->len - r->got < avail ? r->len - r->got : avail;
            memcpy(r->buf + r->got, in + *used, k);
            r->got += k;
            *used += k;
            if (r->got < r->len)
                continue;
        } else {                                /* RX_BODY */
            k = r->len - r->got < avail ? r->len - r->got : avail;
            *piece = in + *used;
            *len = k;
            r->got += k;
            *used += k;
            *last = r->got == r->len;
            if (*last) {
                r->st = RX_HDR;
                r->got = 0;
            }
            *tag = WT_DATA;
            return WSB_RX_PIECE;
        }
        *tag = r->tag;                          /* a whole small frame */
        *len = r->len;
        *piece = r->buf;
        r->st = RX_HDR;
        r->got = 0;
        return WSB_RX_FRAME;
    }
    return WSB_RX_NEED;
}

/* ---- /etc/wasabid.conf ---- */
static int isword(char ch) { return ch && ch != ' ' && ch != '\t'; }
static int parse_ip(const char **s, uint32_t *a)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t o = 0;
        int d = 0;
        while (**s >= '0' && **s <= '9' && d < 4) { o = o * 10 + (uint32_t)(*(*s)++ - '0'); d++; }
        if (!d || d > 3 || o > 255)
            return -1;
        v = v << 8 | o;
        if (i < 3 && *(*s)++ != '.')
            return -1;
    }
    *a = v;
    return 0;
}
static int parse_num(const char *s, uint32_t max, uint32_t *v)
{
    uint32_t n = 0;
    if (!*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || n > max / 10)
            return -1;
        n = n * 10 + (uint32_t)(*s - '0');
    }
    if (n > max)
        return -1;
    *v = n;
    return 0;
}
int wsb_conf_parse(struct wsb_conf *c, const char *text, uint32_t n, int *line, const char **why)
{
    char l[160];
    uint32_t i = 0;
    int ln = 0, have_port = 0;
    memset(c, 0, sizeof *c);
    c->port = WSB_PORT;
    memcpy(c->name, "zenix", 6);
    *line = 0;
    while (i < n) {
        uint32_t k = 0;
        ln++;
        while (i < n && text[i] != '\n') {
            if (!text[i] || k + 1 >= sizeof l) { *line = ln; *why = text[i] ? "a line is too long" : "a NUL byte"; return -EINVAL; }
            l[k++] = text[i++];
        }
        i++;
        while (k && (l[k - 1] == ' ' || l[k - 1] == '\t' || l[k - 1] == '\r'))
            k--;
        l[k] = 0;
        char *s = l;
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s || *s == '#')
            continue;
        char *w = s;
        while (isword(*s))
            s++;
        uint32_t wl = (uint32_t)(s - w);
        while (*s == ' ' || *s == '\t')
            s++;
        *line = ln;
        if (wl == 3 && !memcmp(w, "key", 3)) {
            uint32_t kl = (uint32_t)strlen(s);
            if (c->key[0]) { *why = "a second key line"; return -EINVAL; }
            if (kl < WSB_KEYMIN) { *why = "the key is shorter than 16 characters"; return -EINVAL; }
            if (kl > WSB_KEYMAX) { *why = "the key is longer than 127 characters"; return -EINVAL; }
            memcpy(c->key, s, kl + 1);
        } else if (wl == 4 && !memcmp(w, "port", 4)) {
            uint32_t v;
            if (have_port++) { *why = "a second port line"; return -EINVAL; }
            if (parse_num(s, 65535, &v) || !v) { *why = "not a port number (1-65535)"; return -EINVAL; }
            c->port = v;
        } else if (wl == 4 && !memcmp(w, "name", 4)) {
            uint32_t nl = 0;
            for (const char *t = s; *t; t++, nl++)
                if (!((*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z') || (*t >= '0' && *t <= '9') || *t == '.' ||
                      *t == '_' || *t == '-')) { *why = "a name is 1-31 of A-Z a-z 0-9 . _ -"; return -EINVAL; }
            if (!nl || nl > WSB_NAMEMAX) { *why = "a name is 1-31 of A-Z a-z 0-9 . _ -"; return -EINVAL; }
            memcpy(c->name, s, nl + 1);
        } else if (wl == 5 && !memcmp(w, "allow", 5)) {
            uint32_t a, bits = 32;
            const char *t = s;
            if (!strcmp(s, "any")) { c->allow_any = 1; continue; }
            if (c->nallow == WSB_ALLOWMAX) { *why = "more than 8 allow lines"; return -EINVAL; }
            if (parse_ip(&t, &a)) { *why = "not an address or CIDR (a.b.c.d/n)"; return -EINVAL; }
            if (*t == '/') {
                if (parse_num(t + 1, 32, &bits)) { *why = "not an address or CIDR (a.b.c.d/n)"; return -EINVAL; }
            } else if (*t) { *why = "not an address or CIDR (a.b.c.d/n)"; return -EINVAL; }
            c->allow_mask[c->nallow] = bits ? 0xFFFFFFFFu << (32 - bits) : 0;
            c->allow_net[c->nallow] = a & c->allow_mask[c->nallow];
            c->nallow++;
        } else {
            *why = "a word this file does not have (key, port, allow, name)";
            return -EINVAL;
        }
    }
    *line = 0;
    if (!c->key[0]) { *why = "no key line"; return -EINVAL; }
    return 0;
}

int wsb_allowed(const struct wsb_conf *c, uint32_t a)
{
    if (c->allow_any || (a >> 24) == 10 || (a >> 24) == 127 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8 ||
        (a >> 16) == 0xA9FE)
        return 1;
    for (int i = 0; i < c->nallow; i++)
        if ((a & c->allow_mask[i]) == c->allow_net[i])
            return 1;
    return 0;
}

/* ---- files ---- */
/* AmigaDOS: the owner's d e w r (bits 0-3) are set when NOT allowed; the group's (8-11) and the others' (12-15) are set
 * when allowed (dos.h's FIBF_GRP_* and FIBF_OTR_*) */
uint32_t wsb_mode_to_prot(uint32_t m)
{
    uint32_t p = 0;
    if (!(m & 0400)) p |= 8;
    if (!(m & 0200)) p |= 4 | 1;
    if (!(m & 0100)) p |= 2;
    if (m & 040) p |= 1u << 11;
    if (m & 020) p |= 1u << 10 | 1u << 8;
    if (m & 010) p |= 1u << 9;
    if (m & 04) p |= 1u << 15;
    if (m & 02) p |= 1u << 14 | 1u << 12;
    if (m & 01) p |= 1u << 13;
    return p;
}
uint32_t wsb_prot_to_mode(uint32_t prot, uint32_t old, int zexe)
{
    if (prot == 0xFFFFFFFFu)
        return old ? old & 07777 : zexe ? 0755 : 0644;
    if (prot & 0x80000000u)
        return prot & 07777;
    return 0444 | (prot & 2 ? 0 : 0111) | (prot & 4 ? 0 : 0200);
}
void wsb_datestamp(uint32_t t, uint32_t *days, uint32_t *mins, uint32_t *ticks)
{
    const uint32_t epoch = 252460800u;          /* 1978-01-01 in Unix seconds */
    t = t > epoch ? t - epoch : 0;
    *days = t / 86400;
    t %= 86400;
    *mins = t / 60;
    *ticks = t % 60 * 50;
}
int wsb_guarded(const char *n)
{
    return !strcmp(n, "zup") || !strcmp(n, "fdisk") || !strcmp(n, "dd") || !strncmp(n, "fsck.", 5) ||
           !strncmp(n, "mkfs.", 5);
}

uint32_t wsb_errcode(int e, int write)
{
    if (e < 0)
        e = -e;
    switch (e) {
    case ENOENT: return 205;
    case EEXIST: return 203;
    case EACCES: case EPERM: return write ? 223 : 224;
    case ENOSPC: return 221;
    case ENOTEMPTY: return 216;
    case ENOTDIR: case EISDIR: return 212;
    case EBUSY: case ETXTBSY: return 202;
    case EROFS: return 214;
    }
    return 0;
}
