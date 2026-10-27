/* SPDX-License-Identifier: GPL-2.0-only */
/* ifconfig, ifup, ifdown: the network's interfaces (DESIGN D19 6.3, 6.4; docs/man/ifconfig.txt). One program under
 * three names (as more is less): ifconfig shows the interfaces netd has and changes one; ifup and ifdown bring
 * interfaces up from their stanza in /etc/network/interfaces and down again. Changes are root's. Everything goes to
 * netd through a socket (SOCK_IFLIST, SOCK_IFSET); a network server that was given up (five ends in a minute, D19 4.3
 * rule 12) is registered again by root's ifup or ifconfig ... up, which says so. */
#include "socket.h"

static const char *prog = "ifconfig";

static int sock(int quiet)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0 && !quiet) fprintf(2, "%s: the network server: %s\n", prog, zwhy("not reachable"));
    return s;
}

/* root's way back from "given up": register net again (svc), then a socket starts netd */
static int sock_again(void)
{
    int s = sock(1);
    if (s >= 0 || zerrno != ENETDOWN || sys_geteuid() != 0) {
        if (s < 0) fprintf(2, "%s: the network server: %s\n", prog, zwhy("not reachable"));
        return s;
    }
    struct ustat st;
    if (sys_stat("/sbin/netd", &st) < 0) { fprintf(2, "%s: there is no network server (/sbin/netd)\n", prog); return -1; }
    if (sys_svc_register("net", "/sbin/netd", 0) >= 0)
        fprintf(2, "%s: the network server was not running: registered /sbin/netd again\n", prog);
    return sock(0);
}

static int ifset(int s, const char *name, u32 op, u32 addr, u32 mask)
{
    struct unetifreq q;
    struct usockreq r;
    memset(&q, 0, sizeof q);
    strncpy(q.name, name, sizeof q.name - 1);
    q.op = op;
    q.addr = addr;
    q.mask = mask;
    memset(&r, 0, sizeof r);
    r.buf = (u32)&q;
    r.len = sizeof q;
    return sys_sock(SOCK_IFSET | (u32)sizeof r << 16, s, &r);
}

static void ip(char *b, u32 a) { struct in_addr in = { a }; strcpy(b, inet_ntoa(in)); }

static void show(const struct unetif *e)
{
    char a[16], m[16], g[16];
    ip(a, e->addr); ip(m, e->mask); ip(g, e->gw);
    oprintf("%s: flags=<%s%s%s%s>  mtu %u\n", e->name, e->flags & NETIF_UP ? "UP" : "DOWN",
            e->flags & NETIF_LOOPBACK ? ",LOOPBACK" : ",POINTOPOINT", e->flags & NETIF_LINK ? ",RUNNING" : "",
            e->flags & NETIF_DEFAULT ? ",DEFAULT" : "", e->mtu);
    oprintf("        inet %s  netmask %s", a, m);
    if (e->gw) oprintf("  gateway %s", g);
    oprintf("\n        RX frames %u  bytes %u  bad %u\n", e->rx_frames, e->rx_bytes, e->rx_bad);
    oprintf("        TX frames %u  bytes %u  dropped %u\n", e->tx_frames, e->tx_bytes, e->tx_drop);
}

static int list(int s, const char *only)
{
    enum { MAX = 8 };
    static struct unetif rows[MAX];
    struct usockreq r;
    memset(&r, 0, sizeof r);
    r.buf = (u32)rows;
    r.len = sizeof rows;
    r.flags = sizeof rows[0];
    int n = sys_sock(SOCK_IFLIST | (u32)sizeof r << 16, s, &r), found = 0;
    if (n < 0) { fprintf(2, "%s: %s\n", prog, zstrerror(n)); return 1; }
    for (int i = 0; i < n; i++) {
        if (only && strcmp(rows[i].name, only)) continue;
        if (found++) oputc('\n');
        show(&rows[i]);
    }
    if (only && !found) { fprintf(2, "%s: %s: no such interface (up)\n", prog, only); return 1; }
    if (!only && n && rows[0].dns[0]) {
        char d[16];
        oprintf("\nnameservers");
        for (int k = 0; k < 2 && rows[0].dns[k]; k++) { ip(d, rows[0].dns[k]); oprintf(" %s", d); }
        oputc('\n');
    }
    return 0;
}

static const char *why(int e)
{
    if (e == -EEXIST) return "already up";
    if (e == -ENOENT) return "not in /etc/network/interfaces (or not up)";
    if (e == -ENODEV) return "its driver is not there yet (only slip, DESIGN D19)";
    if (e == -EPERM) return "only root may change the network";
    if (e == -EACCES) return "/etc/network/interfaces must belong to root and be writable by root alone";
    return zstrerror(e);
}

/* ifup / ifdown [-a | IFACE...] */
static int updown(int argc, char **argv, int up)
{
    int all = 0, n = 0, bad = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) all = 1;
        else if (argv[i][0] == '-') { fprintf(2, "usage: %s -a | IFACE...\n", prog); return 2; }
        else n++;
    }
    if (!all && !n) { fprintf(2, "usage: %s -a | IFACE...\n", prog); return 2; }
    int s = up ? sock_again() : sock(0);
    if (s < 0) return 1;
    if (all) {
        int r = ifset(s, "", up ? NETIFREQ_UP : NETIFREQ_DOWN, 0, 0);
        if (r < 0) { fprintf(2, "%s -a: %s\n", prog, why(r)); bad = 1; }
    }
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        int r = ifset(s, argv[i], up ? NETIFREQ_UP : NETIFREQ_DOWN, 0, 0);
        if (r < 0) { fprintf(2, "%s: %s: %s\n", prog, argv[i], why(r)); bad = 1; }
    }
    sys_close(s);
    return bad;
}

/* an address with an optional /PREFIX: 1 if it is one */
static int addr_prefix(const char *t, u32 *a, u32 *m)
{
    char b[24];
    u32 n = 0;
    while (t[n] && t[n] != '/' && n < sizeof b - 1) { b[n] = t[n]; n++; }
    b[n] = 0;
    if (inet_pton(AF_INET, b, a) != 1) return 0;
    if (t[n] == '/') {
        char *e;
        unsigned long p = strtoul(t + n + 1, &e, 10);
        if (*e || p > 32 || !t[n + 1]) return 0;
        *m = p ? htonl(0xFFFFFFFFu << (32 - p)) : 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *b = strrchr(argv[0], '/');
    b = b ? b + 1 : argv[0];
    if (!strcmp(b, "ifup") || !strcmp(b, "ifdown")) {
        prog = b;
        return updown(argc, argv, b[2] == 'u');
    }
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "-a"))) {
        int s = sock(0);
        if (s < 0) return 1;
        int r = list(s, 0);
        sys_close(s);
        return r;
    }
    if (argv[1][0] == '-') goto usage;
    const char *name = argv[1];
    if (argc == 2) {
        int s = sock(0);
        if (s < 0) return 1;
        int r = list(s, name);
        sys_close(s);
        return r;
    }
    u32 a = 0, m = 0;
    int have = 0, up = -1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "up")) up = 1;
        else if (!strcmp(argv[i], "down")) up = 0;
        else if (!strcmp(argv[i], "netmask") && i + 1 < argc) { if (inet_pton(AF_INET, argv[++i], &m) != 1) goto usage; }
        else if (!have && addr_prefix(argv[i], &a, &m)) have = 1;
        else goto usage;
    }
    if (!have && m) goto usage;
    int s = up == 1 ? sock_again() : sock(0), r = 0;
    if (s < 0) return 1;
    if (up == 1 && (r = ifset(s, name, NETIFREQ_UP, 0, 0)) < 0 && r != -EEXIST) {
        fprintf(2, "ifconfig: %s: %s\n", name, why(r));
        return 1;
    }
    if (have && (r = ifset(s, name, NETIFREQ_ADDR, a, m)) < 0) {
        fprintf(2, "ifconfig: %s: %s\n", name, why(r));
        return 1;
    }
    if (up == 0 && (r = ifset(s, name, NETIFREQ_DOWN, 0, 0)) < 0) {
        fprintf(2, "ifconfig: %s: %s\n", name, why(r));
        return 1;
    }
    sys_close(s);
    return 0;
usage:
    fprintf(2, "usage: ifconfig [-a] [IFACE [ADDRESS[/PREFIX]] [netmask MASK] [up|down]]\n"
               "       ifup -a | IFACE...    ifdown -a | IFACE...\n");
    return 2;
}
