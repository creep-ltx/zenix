/* SPDX-License-Identifier: GPL-2.0-only */
/* netstat: the network's connections and listening sockets (DESIGN D19 6.4; docs/man/netstat.txt), from netd's table
 * (SOCK_NETSTAT, rows of struct unetstat). Linux's columns and options: -t TCP, -u UDP, -w ICMP echo sockets (none
 * of them: all three), -l listening only, -a both, else connections only; addresses are always numbers (-n). */
#include "socket.h"
#include "pw.h"

static const char *const tcp_state[] = { "CLOSED", "LISTEN", "SYN_SENT", "SYN_RECV", "ESTABLISHED", "FIN_WAIT1",
                                         "FIN_WAIT2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };

static void addr(char *b, u32 a, unsigned port, int star)
{
    struct in_addr in = { a };
    if (star && !port) snprintf(b, 24, "%s:*", inet_ntoa(in));
    else snprintf(b, 24, "%s:%u", inet_ntoa(in), port);
}

static const char USAGE[] = "usage: netstat [-tuwlan]\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int t = 0, u = 0, w = 0, l = 0, a = 0;
    for (int i = 1; i < argc; i++) {
        const char *p = argv[i];
        if (*p != '-' || !p[1]) zusage(USAGE);
        if (p[1] == '-') zbadopt(p, USAGE);
        while (*++p)
            switch (*p) {
            case 't': t = 1; break;
            case 'u': u = 1; break;
            case 'w': w = 1; break;
            case 'l': l = 1; break;
            case 'a': a = 1; break;
            case 'n': break;
            default: zbadch(*p, USAGE);
            }
    }
    if (!t && !u && !w) t = u = w = 1;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        dprintf(2, "netstat: %s\n", zwhy("no network"));
        return 1;
    }
    enum { MAXROWS = 64 };
    struct unetstat *rows = xmalloc(MAXROWS * sizeof *rows);
    struct usockreq r;
    memset(&r, 0, sizeof r);
    r.buf = (u32)rows;
    r.len = MAXROWS * sizeof *rows;
    r.flags = sizeof *rows;
    int n = sys_sock(SOCK_NETSTAT | (u32)sizeof r << 16, s, &r);
    sys_close(s);
    if (n < 0) {
        dprintf(2, "netstat: %s\n", zstrerror(n));
        return 1;
    }
    oprintf("Active Internet connections%s\n", a ? " (servers and established)" : l ? " (only servers)" : " (w/o servers)");
    oprintf("Proto Recv-Q Send-Q Local Address           Foreign Address         State       User\n");
    for (int i = 0; i < n; i++) {
        struct unetstat *e = &rows[i];
        int listening = e->proto == IPPROTO_TCP ? e->state == 1 : e->proto == IPPROTO_UDP && !e->raddr;
        if (e->proto == IPPROTO_TCP ? !t : e->proto == IPPROTO_UDP ? !u : !w) continue;
        if (e->proto == IPPROTO_UDP && !e->lport) continue;    /* not bound to anything yet (netstat's own socket) */
        if (!a && (l ? !listening : listening)) continue;
        char la[24], ra[24], un[32];
        addr(la, e->laddr, e->lport, 0);
        addr(ra, e->raddr, e->rport, 1);
        const char *st = e->proto == IPPROTO_TCP ? (e->state <= 10 ? tcp_state[e->state] : "?") : "";
        oprintf("%-5s %6u %6u %-23s %-23s %-11s %s\n", e->proto == IPPROTO_TCP ? "tcp" : e->proto == IPPROTO_UDP ? "udp" : "icmp",
                e->rxq, e->txq, la, ra, st, pw_name(e->uid, un));
    }
    if (n == MAXROWS) oprintf("netstat: only the first %d shown\n", MAXROWS);
    return 0;
}
