/* SPDX-License-Identifier: GPL-2.0-only */
/* netd's configuration files (DESIGN D19 6.1): /etc/network/interfaces (a small subset of Debian's format),
 * /etc/resolv.conf and /etc/hosts. Read at netd's start and on a reload, never per request. A word netd does not
 * know is reported ("netd: FILE:LINE: ...") and ignored; a limit that is hit is reported too. */
#include "netd_int.h"

#define LINEMAX 256
#define NWORDS 12

/* the next line of text into buf (at most LINEMAX - 1 bytes; a longer line is reported and cut), comments gone,
 * split into words; returns the number of words, -1 at the end */
struct reader { const char *t; unsigned long len, pos; int line; const char *file; int reports; };
static int next_line(struct reader *r, char *buf, char **w)
{
    unsigned n = 0;
    int nw = 0, cut = 0;
    char *p;
    if (r->pos >= r->len) return -1;
    while (r->pos < r->len && r->t[r->pos] != '\n') {
        char c = r->t[r->pos++];
        if (n < LINEMAX - 1) buf[n++] = c; else cut = 1;
    }
    if (r->pos < r->len) r->pos++;
    buf[n] = 0;
    r->line++;
    if (cut) { netd_logf("netd: %s:%d: line longer than %d bytes, cut", r->file, r->line, LINEMAX - 1); r->reports++; }
    for (p = buf; *p; p++) if (*p == '#') { *p = 0; break; }
    for (p = buf; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\r') *p++ = 0;
        if (!*p) break;
        if (nw == NWORDS) { netd_logf("netd: %s:%d: more than %d words, the rest ignored", r->file, r->line, NWORDS); r->reports++; break; }
        w[nw++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;
    }
    return nw;
}
static void report(struct reader *r, const char *what, const char *word)
{
    netd_logf("netd: %s:%d: %s '%s', ignored", r->file, r->line, what, word);
    r->reports++;
}

int netd_parse_ip4(const char *s, u32_t *a)
{
    unsigned long part[4];
    int i;
    for (i = 0; i < 4; i++) {
        unsigned long v = 0;
        int d = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned long)(*s++ - '0'); if (++d > 3) return 0; }
        if (!d || v > 255) return 0;
        part[i] = v;
        if (i < 3) { if (*s++ != '.') return 0; }
    }
    if (*s) return 0;
    *a = lwip_htonl(part[0] << 24 | part[1] << 16 | part[2] << 8 | part[3]);
    return 1;
}
static int parse_prefix(const char *s, u32_t *addr, u32_t *mask)
{
    char b[24];
    const char *sl = strchr(s, '/');
    unsigned long pl = 0;
    unsigned n;
    if (!sl || (unsigned)(sl - s) >= sizeof b) return 0;
    n = (unsigned)(sl - s);
    memcpy(b, s, n);
    b[n] = 0;
    if (!netd_parse_ip4(b, addr)) return 0;
    for (sl++; *sl >= '0' && *sl <= '9'; sl++) pl = pl * 10 + (unsigned long)(*sl - '0');
    if (*sl || pl > 32 || sl[-1] == '/') return 0;
    *mask = lwip_htonl(pl ? 0xfffffffful << (32 - pl) : 0);
    return 1;
}
static int name_ok(const char *s, unsigned max)
{
    unsigned n = 0;
    for (; *s; s++, n++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.') || n >= max) return 0;
    return n > 0;
}

int netd_parse_interfaces(const char *text, unsigned long len, struct netd_config *c)
{
    struct reader r = { text, len, 0, 0, "/etc/network/interfaces", 0 };
    char buf[LINEMAX], *w[NWORDS], autos[NETD_IFMAX * 2][NETD_IFNAME];
    struct netd_ifcfg *cur = NULL;
    int nw, i, j, nauto = 0, skip = 0;
    memset(c->ifs, 0, sizeof c->ifs);
    c->nifs = 0;
    while ((nw = next_line(&r, buf, w)) >= 0) {
        if (!nw) continue;
        if (!strcmp(w[0], "auto")) {
            for (i = 1; i < nw; i++) {
                if (!name_ok(w[i], NETD_IFNAME - 1)) { report(&r, "bad interface name", w[i]); continue; }
                if (nauto == NETD_IFMAX * 2) { report(&r, "too many auto interfaces", w[i]); continue; }
                strcpy(autos[nauto++], w[i]);
            }
            continue;
        }
        if (!strcmp(w[0], "iface")) {
            cur = NULL;
            skip = 1;
            if (nw < 4 || strcmp(w[2], "inet")) { report(&r, "not 'iface NAME inet METHOD'", nw > 1 ? w[1] : w[0]); continue; }
            if (!name_ok(w[1], NETD_IFNAME - 1)) { report(&r, "bad interface name", w[1]); continue; }
            if (strcmp(w[3], "dhcp") && strcmp(w[3], "static")) { report(&r, "unknown method (dhcp or static)", w[3]); continue; }
            for (i = 0; i < c->nifs; i++) if (!strcmp(c->ifs[i].name, w[1])) break;
            if (i < c->nifs) { report(&r, "a second stanza for", w[1]); continue; }
            if (c->nifs == NETD_IFMAX) { report(&r, "too many interfaces (4), stanza", w[1]); continue; }
            cur = &c->ifs[c->nifs++];
            strcpy(cur->name, w[1]);
            cur->method = !strcmp(w[3], "dhcp") ? NETD_INET_DHCP : NETD_INET_STATIC;
            for (i = 4; i < nw; i++) report(&r, "unknown word", w[i]);
            skip = 0;
            continue;
        }
        if (!cur) {
            if (!skip) report(&r, "outside a stanza", w[0]);
            continue;
        }
        if (!strcmp(w[0], "driver") && nw >= 2) {
            if (strchr(w[1], '/') || strlen(w[1]) >= sizeof cur->driver) { report(&r, "bad driver name", w[1]); continue; }
            strcpy(cur->driver, w[1]);
            cur->args[0] = 0;
            for (i = 2; i < nw; i++) {
                if (strlen(cur->args) + strlen(w[i]) + 2 > sizeof cur->args) { report(&r, "driver arguments too long", w[i]); break; }
                if (i > 2) strcat(cur->args, " ");
                strcat(cur->args, w[i]);
            }
        } else if (!strcmp(w[0], "address") && nw == 2) {
            u32_t a, m;
            if (!parse_prefix(w[1], &a, &m)) { report(&r, "not ADDRESS/PREFIX", w[1]); continue; }
            cur->addr = a;
            cur->mask = m;
        } else if ((!strcmp(w[0], "gateway") || !strcmp(w[0], "pointopoint")) && nw == 2) {
            u32_t a;
            if (!netd_parse_ip4(w[1], &a)) { report(&r, "not an IPv4 address", w[1]); continue; }
            if (w[0][0] == 'g') cur->gateway = a; else cur->ptp = a;
        } else if (!strcmp(w[0], "mtu") && nw == 2) {
            unsigned long v = 0;
            const char *p = w[1];
            while (*p >= '0' && *p <= '9' && v < 100000) v = v * 10 + (unsigned long)(*p++ - '0');
            if (*p || v < 68 || v > 1500) { report(&r, "bad mtu (68-1500)", w[1]); continue; }
            cur->mtu = (unsigned short)v;
        } else if (!strcmp(w[0], "hostname") && nw == 2) {
            if (!name_ok(w[1], sizeof cur->hostname - 1)) { report(&r, "bad host name", w[1]); continue; }
            strcpy(cur->hostname, w[1]);
        } else {
            report(&r, "unknown word", w[0]);
            continue;
        }
    }
    for (j = 0; j < nauto; j++) {
        for (i = 0; i < c->nifs; i++) if (!strcmp(c->ifs[i].name, autos[j])) { c->ifs[i].is_auto = 1; break; }
        if (i == c->nifs) { netd_logf("netd: /etc/network/interfaces: auto %s has no stanza, ignored", autos[j]); r.reports++; }
    }
    for (i = 0; i < c->nifs; i++) {
        if (!c->ifs[i].driver[0]) { netd_logf("netd: /etc/network/interfaces: %s has no driver line", c->ifs[i].name); r.reports++; }
        if (c->ifs[i].method == NETD_INET_STATIC && !c->ifs[i].addr) {
            netd_logf("netd: /etc/network/interfaces: %s is static but has no address", c->ifs[i].name);
            r.reports++;
        }
    }
    return r.reports;
}

int netd_parse_resolv(const char *text, unsigned long len, struct netd_config *c)
{
    struct reader r = { text, len, 0, 0, "/etc/resolv.conf", 0 };
    char buf[LINEMAX], *w[NWORDS];
    int nw;
    c->ndns = 0;
    c->search[0] = 0;
    while ((nw = next_line(&r, buf, w)) >= 0) {
        if (!nw) continue;
        if (!strcmp(w[0], "nameserver") && nw >= 2) {
            u32_t a;
            if (strchr(w[1], ':')) continue;            /* IPv6: netd has none, not an error in the file */
            if (!netd_parse_ip4(w[1], &a)) { report(&r, "not an IPv4 address", w[1]); continue; }
            if (c->ndns == 2) { report(&r, "only two name servers are used, not", w[1]); continue; }
            c->dns[c->ndns++] = a;
        } else if ((!strcmp(w[0], "search") || !strcmp(w[0], "domain")) && nw >= 2) {
            if (strlen(w[1]) >= sizeof c->search) { report(&r, "search domain too long", w[1]); continue; }
            strcpy(c->search, w[1]);
        } else if (!strcmp(w[0], "options") || !strcmp(w[0], "sortlist")) {
            /* standard words netd does not use */
        } else report(&r, "unknown word", w[0]);
    }
    return r.reports;
}

/* ---- /etc/hosts ---- */
#define HOSTS_MAX 64
#define HOSTNAME_MAX 64
struct hostent_z { u32_t addr; char name[HOSTNAME_MAX]; };
static struct hostent_z *hosts;
static int nhosts;

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

int netd_hosts_load(const char *text, unsigned long len)
{
    struct reader r = { text, len, 0, 0, "/etc/hosts", 0 };
    char buf[LINEMAX], *w[NWORDS];
    int nw, i;
    if (!hosts && !(hosts = netd_malloc(HOSTS_MAX * sizeof *hosts))) { netd_logf("netd: /etc/hosts: no memory"); return 1; }
    nhosts = 0;
    while ((nw = next_line(&r, buf, w)) >= 0) {
        u32_t a;
        if (!nw) continue;
        if (strchr(w[0], ':')) continue;                /* IPv6 lines (::1 localhost): not an error */
        if (!netd_parse_ip4(w[0], &a)) { report(&r, "not an IPv4 address", w[0]); continue; }
        if (nw < 2) { report(&r, "an address without a name", w[0]); continue; }
        for (i = 1; i < nw; i++) {
            if (strlen(w[i]) >= HOSTNAME_MAX) { report(&r, "name too long", w[i]); continue; }
            if (nhosts == HOSTS_MAX) { report(&r, "more than 64 names, not", w[i]); continue; }
            hosts[nhosts].addr = a;
            strcpy(hosts[nhosts].name, w[i]);
            nhosts++;
        }
    }
    return r.reports;
}

int netd_hosts_find(const char *name, u32_t *addrs, int max)
{
    int i, n = 0;
    for (i = 0; i < nhosts && n < max; i++)
        if (ieq(hosts[i].name, name)) addrs[n++] = hosts[i].addr;
    return n;
}

int netd_config_trusted(unsigned long mode, unsigned uid)
{
    return uid == 0 && !(mode & 022);
}
