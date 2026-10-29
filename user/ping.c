/* SPDX-License-Identifier: GPL-2.0-only */
/* ping: ICMP echo (DESIGN D19 6.4; docs/man/ping.txt), over an ICMP datagram socket as Linux's unprivileged ping:
 * netd sets the echo id and the checksum and gives this socket only the replies with its id, so no root is needed.
 * Each request carries its send time (the E-clock) and a sequence number; the round trip is measured on the reply. */
#include "socket.h"

#define ECLOCK 709379u                  /* the E-clock's ticks a second (PAL; NTSC 715,909: under 1% off) */
static volatile int stop;
static void on_int(int sig) { (void)sig; stop = 1; }

static u32 tenths_ms(u32 counts) { return (u32)((counts * 100u + 3547u) / 7094u); }   /* E-clock counts -> 0.1 ms */

int main(int argc, char **argv)
{
    u32 count = 0, size = 56, wait_s = 2, quiet = 0, interval = 100;   /* interval in ticks */
    const char *host = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-') { if (host) goto usage; host = a; continue; }
        if (!a[1] || a[2]) goto usage;
        if (a[1] == 'q') { quiet = 1; continue; }
        if (i + 1 >= argc) goto usage;
        const char *v = argv[++i];
        char *e;
        if (a[1] == 'i') {                              /* seconds, with tenths: 0.2 at least for users, as Linux */
            u32 t = (u32)strtoul(v, &e, 10) * 100;
            if (*e == '.' && isdigit((unsigned char)e[1])) { t += (u32)(e[1] - '0') * 10; e += 2; while (isdigit((unsigned char)*e)) e++; }
            if (*e || !t || !isdigit((unsigned char)*v)) goto usage;
            if (t < 20 && sys_geteuid() != 0) { dprintf(2, "ping: an interval below 0.2 s is root's\n"); return 2; }
            interval = t;
            continue;
        }
        unsigned long n = strtoul(v, &e, 10);
        if (*e || !*v) goto usage;
        if (a[1] == 'c' && n) count = (u32)n;
        else if (a[1] == 's' && n <= 1400) size = (u32)n;
        else if (a[1] == 'W' && n) wait_s = (u32)n;
        else goto usage;
    }
    if (!host) goto usage;
    struct addrinfo h, *res;
    memset(&h, 0, sizeof h);
    h.ai_family = AF_INET;
    h.ai_socktype = SOCK_DGRAM;
    int g = getaddrinfo(host, 0, &h, &res);
    if (g) { dprintf(2, "ping: %s: %s\n", host, gai_strerror(g)); return 2; }
    struct sockaddr_in to = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (s < 0) { dprintf(2, "ping: %s\n", zwhy("cannot make an ICMP socket")); return 2; }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_int;                             /* no SA_RESTART: ^C ends the wait, then the summary */
    sigaction(SIGINT, &sa, 0);
    char ip[16];
    strcpy(ip, inet_ntoa(to.sin_addr));
    dprintf(1, "PING %s (%s): %u data bytes\n", host, ip, size);
    oflush();
    static unsigned char out[8 + 1400], in[8 + 1400 + 64];
    u32 sent = 0, got = 0, tmin = 0xFFFFFFFF, tmax = 0, tsum = 0;
    u32 next = sys_ticks();
    while (!stop && (!count || sent < count)) {
        memset(out, 0, sizeof out);
        out[0] = 8;                                     /* echo request; netd sets the id and the checksum */
        out[6] = (unsigned char)(++sent >> 8);
        out[7] = (unsigned char)sent;
        for (u32 k = 0; k < size; k++) out[8 + k] = (unsigned char)k;
        u32 t0 = sys_hrtime();
        if (size >= 4) memcpy(out + 8, &t0, 4);
        if (sendto(s, out, 8 + size, 0, (struct sockaddr *)&to, sizeof to) < 0) {
            dprintf(2, "ping: %s\n", zwhy("send failed"));
        }
        next += interval;
        u32 until = (count && sent == count) ? sys_ticks() + wait_s * 100 : next;
        for (;;) {                                      /* replies until the next send (the last: until -W) */
            int left = (int)(until - sys_ticks());
            if (stop || left <= 0) break;
            struct pollfd p = { s, POLLIN, 0 };
            if (poll(&p, 1, (int)left * 10) <= 0) continue;
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int n = recvfrom(s, in, sizeof in, 0, (struct sockaddr *)&from, &fl);
            u32 t1 = sys_hrtime();
            if (n < 8 || in[0] != 0) continue;          /* echo replies only */
            u32 seq = (u32)in[6] << 8 | in[7], ts;
            if (n - 8 >= 4 && size >= 4) memcpy(&ts, in + 8, 4); else ts = t1;
            u32 rt = tenths_ms(t1 - ts);
            got++;
            tsum += rt;
            if (rt < tmin) tmin = rt;
            if (rt > tmax) tmax = rt;
            if (!quiet) {
                dprintf(1, "%d bytes from %s: icmp_seq=%u time=%u.%u ms\n", n, inet_ntoa(from.sin_addr), seq, rt / 10, rt % 10);
                oflush();
            }
            if (count && sent == count && got >= sent) break;
        }
    }
    dprintf(1, "\n--- %s ping statistics ---\n%u packets transmitted, %u received, %u%% packet loss\n", host, sent, got,
           sent ? (sent - (got > sent ? sent : got)) * 100 / sent : 0);
    if (got) dprintf(1, "rtt min/avg/max = %u.%u/%u.%u/%u.%u ms\n", tmin / 10, tmin % 10, tsum / got / 10, tsum / got % 10,
                    tmax / 10, tmax % 10);
    return got ? 0 : 1;
usage:
    dprintf(2, "usage: ping [-q] [-c COUNT] [-i SECONDS] [-s SIZE] [-W SECONDS] HOST\n");
    return 2;
}
