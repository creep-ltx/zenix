/* SPDX-License-Identifier: GPL-2.0-only */
/* host NAME...: look a name up through the network server (DESIGN D19 4.5, 6.4; docs/man/host.txt), as BIND's host
 * prints it: "NAME has address A" for each address. netd answers from /etc/hosts, its cache, then the name servers
 * (/etc/resolv.conf's, or DHCP's); a name without a dot gets resolv.conf's search domain. One getaddrinfo a name. */
#include "socket.h"

int main(int argc, char **argv)
{
    int bad = 0, n = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            dprintf(2, "usage: host NAME...\n");
            return 2;
        }
        n++;
    }
    if (!n) {
        dprintf(2, "usage: host NAME...\n");
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        const char *name = argv[i];
        struct in_addr in;
        if (inet_aton(name, &in)) {             /* an address: no PTR lookups in lwIP's resolver */
            dprintf(2, "host: %s: an address; reverse lookups are not supported\n", name);
            bad = 1;
            continue;
        }
        struct addrinfo h, *r = 0;
        memset(&h, 0, sizeof h);
        h.ai_family = AF_INET;
        h.ai_socktype = SOCK_STREAM;            /* one entry an address */
        int e = getaddrinfo(name, 0, &h, &r);
        if (e == EAI_NONAME) {
            oprintf("Host %s not found\n", name);
            bad = 1;
        } else if (e == EAI_AGAIN) {
            dprintf(2, "host: %s: no answer from the name servers\n", name);
            bad = 1;
        } else if (e == EAI_FAIL) {
            dprintf(2, "host: %s: %s\n", name, zerrno == ENETDOWN ? "no name server (/etc/resolv.conf), or the network "
                    "server is not running" : gai_strerror(e));
            bad = 1;
        } else if (e) {
            dprintf(2, "host: %s: %s\n", name, gai_strerror(e));
            bad = 1;
        } else {
            for (struct addrinfo *a = r; a; a = a->ai_next)
                oprintf("%s has address %s\n", name, inet_ntoa(((struct sockaddr_in *)a->ai_addr)->sin_addr));
            freeaddrinfo(r);
        }
        oflush();
    }
    return bad;
}
