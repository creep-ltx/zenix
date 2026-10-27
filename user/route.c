/* SPDX-License-Identifier: GPL-2.0-only */
/* route: the network's routes (DESIGN D19 6.4; docs/man/route.txt). netd has one route per interface (its own net)
 * and one default route; route shows them, and root adds or removes the default (SOCK_ROUTE, SOCK_IFLIST). */
#include "socket.h"

static int route(int s, u32 op, u32 gw, struct uroute *out)
{
    struct uroute q;
    struct usockreq r;
    memset(&q, 0, sizeof q);
    q.op = op;
    q.gw = gw;
    memset(&r, 0, sizeof r);
    r.buf = (u32)&q;
    r.len = sizeof q;
    int e = sys_sock(SOCK_ROUTE | (u32)sizeof r << 16, s, &r);
    if (out) *out = q;
    return e;
}

static void col(u32 a) { struct in_addr in = { a }; oprintf("%-16s", inet_ntoa(in)); }

int main(int argc, char **argv)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { fprintf(2, "route: the network server: %s\n", zwhy("not reachable")); return 1; }
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "-n"))) {
        struct uroute d;
        static struct unetif rows[8];
        struct usockreq r;
        memset(&r, 0, sizeof r);
        r.buf = (u32)rows;
        r.len = sizeof rows;
        r.flags = sizeof rows[0];
        int n = sys_sock(SOCK_IFLIST | (u32)sizeof r << 16, s, &r), e = route(s, ROUTE_GET, 0, &d);
        if (n < 0 || e < 0) { fprintf(2, "route: %s\n", zstrerror(n < 0 ? n : e)); return 1; }
        oprintf("Kernel IP routing table\nDestination     Gateway         Genmask         Flags Iface\n");
        if (d.gw) { col(0); col(d.gw); col(0); oprintf("UG    %s\n", d.name); }
        for (int i = 0; i < n; i++) {
            if (!(rows[i].flags & NETIF_UP)) continue;
            col(rows[i].addr & rows[i].mask); col(0); col(rows[i].mask); oprintf("U     %s\n", rows[i].name);
        }
        return 0;
    }
    /* route add default [gw] GW, route del default (Linux's words; "default" may be 0.0.0.0) */
    int add = !strcmp(argv[1], "add"), del = !strcmp(argv[1], "del") || !strcmp(argv[1], "delete");
    if ((!add && !del) || argc < 3 || (strcmp(argv[2], "default") && strcmp(argv[2], "0.0.0.0"))) goto usage;
    u32 gw = 0;
    int i = 3;
    if (i < argc && !strcmp(argv[i], "gw")) i++;
    if (add) {
        if (i + 1 != argc || inet_pton(AF_INET, argv[i], &gw) != 1) goto usage;
    } else if (i != argc) goto usage;
    int e = route(s, add ? ROUTE_ADD : ROUTE_DEL, gw, 0);
    if (e < 0) {
        fprintf(2, "route: %s\n", e == -EPERM ? "only root may change the routes" :
                   e == -ENETUNREACH ? "no interface is up that reaches that gateway" : zstrerror(e));
        return 1;
    }
    return 0;
usage:
    fprintf(2, "usage: route [-n]\n       route add default [gw] GATEWAY\n       route del default\n");
    return 2;
}
