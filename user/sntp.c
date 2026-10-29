/* SPDX-License-Identifier: GPL-2.0-only */
/* sntp [-r]: set the clock from the network (DESIGN D21 8.3; docs/man/sntp.txt). Root only. The servers are
 * /etc/sntp's (at most four "server HOST" lines), asked in order over UDP port 123, each given its share of 10
 * seconds in all; the first accepted answer sets the clock (SYS_settime). The packets, their checks, the time and
 * the guard are net/sntp/'s core: an answer is believed only when it carries our random transmit stamp back, and it
 * raises the floor (/var/lib/clock, crypto/zc/zc_clock.h's format: the time certificates are never judged below)
 * only when it agrees with the battery clock, or without one lies at most 400 days past the floor. -r resets the
 * floor to the build date. ifup starts it when the network comes up. */
#include "socket.h"
#include "../net/sntp/sntp.h"
#include "../crypto/zc/zc_clock.h"
#include "builddate.h"
#include "clockfile.h"

/* Was the battery clock read at boot (so the guard compares with it; SYS_ticks 2)? If not, an answer raises the floor
 * only when it is at most 400 days past it (sntp_judge). */
static int have_rtc(void)
{
    int b = sys_clockbits();
    return b > 0 && (b & CLOCK_RTC);
}

static void civil(u32 t, char *out, u32 cap)
{
    static const unsigned char ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    u32 days = t / 86400, rem = t % 86400;
    int y = 1970, m;
    for (;;) {
        u32 yl = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
        if (days < yl)
            break;
        days -= yl;
        y++;
    }
    for (m = 0; m < 11; m++) {
        u32 l = ml[m] + (m == 1 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        if (days < l)
            break;
        days -= l;
    }
    snprintf(out, cap, "%d-%02d-%02d %02u:%02u:%02u UTC", y, m + 1, (int)days + 1, rem / 3600, rem / 60 % 60,
             rem % 60);
}

static void warn(void *u, int line, const char *what)
{
    (void)u;
    dprintf(2, "sntp: /etc/sntp line %d: %s (ignored)\n", line, what);
}

static u32 now_ms(void)
{
    return sys_ticks() * 10;
}

/* one server, until it answers or `until` (ms): 0 and *t, or -1 (said why) */
static int ask(const char *host, u32 until, uint32_t *t)
{
    struct addrinfo h, *r = 0;
    struct sntp_answer a;
    unsigned char q[SNTP_LEN], p[512], nonce[8];
    int s, e, fd;
    u32 sent;
    memset(&h, 0, sizeof h);
    h.ai_family = AF_INET;
    h.ai_socktype = SOCK_DGRAM;
    if ((e = getaddrinfo(host, "123", &h, &r)) != 0) {
        dprintf(2, "sntp: %s: %s\n", host, e == EAI_NONAME ? "no such host" : gai_strerror(e));
        return -1;
    }
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0 || connect(s, r->ai_addr, r->ai_addrlen) < 0) {
        dprintf(2, "sntp: %s: %s\n", host, zwhy("no socket"));
        freeaddrinfo(r);
        if (s >= 0)
            sys_close(s);
        return -1;
    }
    freeaddrinfo(r);
    /* the nonce need not be a key: /dev/urandom never waits (sntp runs early at boot, before the pool may be ready) */
    fd = sys_open("/dev/urandom", O_RDONLY);
    if (fd < 0 || sys_read(fd, nonce, 8) != 8) {
        dprintf(2, "sntp: no random numbers\n");
        if (fd >= 0)
            sys_close(fd);
        sys_close(s);
        return -1;
    }
    sys_close(fd);
    sntp_request(q, nonce);
    for (;;) {                          /* the network may still be coming up: try again until the share is used */
        sent = now_ms();
        if (send(s, q, sizeof q, 0) == (int)sizeof q)
            break;
        e = zerrno;
        if ((e != ENETUNREACH && e != ENETDOWN && e != EHOSTUNREACH) || (int)(until - sent) < 1000) {
            dprintf(2, "sntp: %s: %s\n", host, zstrerror(e) ? zstrerror(e) : "cannot send");
            sys_close(s);
            return -1;
        }
        sys_sleep(100);
    }
    for (;;) {
        struct pollfd pf;
        int left = (int)(until - now_ms()), k;
        pf.fd = s;
        pf.events = POLLIN;
        pf.revents = 0;
        if (left <= 0 || poll(&pf, 1, left) != 1) {
            dprintf(2, "sntp: %s: no answer\n", host);
            break;
        }
        k = recv(s, p, sizeof p, 0);
        if (k < 0) {
            dprintf(2, "sntp: %s: %s\n", host, zwhy("no answer"));
            break;
        }
        e = sntp_parse(&a, p, (size_t)k, nonce);
        if (e == SNTP_EORIGIN || e == SNTP_ESHORT)
            continue;                   /* not the answer to our request: wait on for it */
        if (e == SNTP_EKOD)
            dprintf(2, "sntp: %s: refused us (kiss of death %s)\n", host, a.kod);
        else if (e == SNTP_ESTRATUM || e == SNTP_ELEAP)
            dprintf(2, "sntp: %s: its clock is not synchronised\n", host);
        else if (e)
            dprintf(2, "sntp: %s: not a valid answer (%d)\n", host, e);
        else {
            *t = sntp_time(&a, now_ms() - sent);
            sys_close(s);
            return 0;
        }
        break;
    }
    sys_close(s);
    return -1;
}

int main(int argc, char **argv)
{
    static char text[4096];
    char d1[40], d2[40];
    struct sntp_conf c;
    uint32_t file = 0, floor, t;
    int reset = 0, fd, n = 0, i, rtc = have_rtc();
    u32 start, deadline;
    if (argc == 2 && !strcmp(argv[1], "-r"))
        reset = 1;
    else if (argc != 1) {
        dprintf(2, "usage: sntp [-r]\n");
        return 2;
    }
    if (sys_geteuid() != 0) {
        dprintf(2, "sntp: only root may set the clock\n");
        return 1;
    }
    if (reset) {
        int e = clockfile_write(ZENIX_BUILD_DATE);
        civil(ZENIX_BUILD_DATE, d1, sizeof d1);
        if (e < 0) {
            dprintf(2, "sntp: cannot write %s: %s\n", ZC_CLOCK_PATH, zstrerror(e) ? zstrerror(e) : "failed");
            return 1;
        }
        dprintf(1, "sntp: the floor reset to the build date, %s\n", d1);
        return 0;
    }
    if ((fd = sys_open("/etc/sntp", O_RDONLY)) < 0) {
        dprintf(2, "sntp: no /etc/sntp (a line \"server pool.ntp.org\" names a server)\n");
        return 1;
    }
    while (n < (int)sizeof text && (i = sys_read(fd, text + n, sizeof text - (u32)n)) > 0)
        n += i;
    if (n == (int)sizeof text && sys_read(fd, d1, 1) > 0)
        dprintf(2, "sntp: /etc/sntp: only its first %u bytes read\n", (unsigned)sizeof text);
    sys_close(fd);
    if (sntp_conf_parse(&c, text, (size_t)n, warn, 0) == 0) {
        dprintf(2, "sntp: /etc/sntp names no server\n");
        return 1;
    }
    floor = zc_clock_floor(ZENIX_BUILD_DATE, clockfile_read(&file) == 0, file);
    start = now_ms();
    deadline = start + SNTP_MAXWAIT * 1000;
    for (i = 0; i < c.n; i++) {
        u32 at = now_ms();
        int j;
        if ((int)(deadline - at) <= 0)
            break;
        if (ask(c.host[i], at + (deadline - at) / (u32)(c.n - i), &t))
            continue;
        u32 was = sys_time();
        j = sntp_judge(t, floor, rtc, was);
        civil(t, d1, sizeof d1);
        if (!j) {
            civil(floor, d2, sizeof d2);
            dprintf(2, "sntp: %s: its time %s is before the floor %s: refused\n", c.host[i], d1, d2);
            continue;
        }
        /* only an answer that passed the guard marks the clock trusted (CLOCK_NET): the rest clears the marks, so
         * neither wget's "not before" nor reboot's floor ever leans on an answer the guard did not accept */
        if ((j & SNTP_RAISE ? sys_settime_checked(t) : sys_settime(t)) < 0) {
            dprintf(2, "sntp: cannot set the clock: %s\n", zwhy("refused"));
            return 1;
        }
        dprintf(1, "sntp: the clock set from %s: %s (it was %ld s off)\n", c.host[i], d1, (long)t - (long)was);
        if (j & SNTP_RAISE) {
            int e = clockfile_write(t);     /* t is past the floor (sntp_judge): the file only rises */
            if (e < 0)
                dprintf(2, "sntp: cannot write %s: %s\n", ZC_CLOCK_PATH, zstrerror(e) ? zstrerror(e) : "failed");
        } else {
            civil(floor, d2, sizeof d2);
            dprintf(1, "sntp: the floor stays at %s (the answer is %s)\n", d2,
                   rtc ? "more than 10 minutes from the battery clock" : "more than 400 days past it");
        }
        return 0;
    }
    dprintf(2, "sntp: no server gave an accepted answer\n");
    return 1;
}
