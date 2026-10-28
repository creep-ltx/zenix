/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sntp on the PC over net/sntp/'s core (DESIGN D21 8.3, tools/host/tls-test.sh sntp), POSIX UDP sockets standing in
 * for Zenix's (the main session's user/sntp.c). Not shipped; a test driver.
 *   sntp-client --conf FILE --clockfile FILE --build T [--rtc NOW] [--wait-ms N] [--port P]
 * FILE is /etc/sntp's format; the servers are asked in order, each until it answers or the time is up (SNTP_MAXWAIT
 * seconds in all, --wait-ms in the tests); a reply that is not the answer to our request (another nonce) is dropped
 * and the wait goes on; a kiss of death or a refused answer moves on to the next server. The first accepted answer
 * is judged against the floor (the build time and the clock file) and, with --rtc, the battery clock's time.
 * stdout: "conf: line N: WHAT", "server HOST: ok T | refused CODE | kod CODE | timeout", then
 * "result set T raise 0|1" or "result none"; with raise 1 the clock file is rewritten (zc_clock_line).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include "../../net/sntp/sntp.h"
#include "../../crypto/zc/zc_clock.h"

static long now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void warn(void *u, int line, const char *what)
{
    (void)u;
    printf("conf: line %d: %s\n", line, what);
}

int main(int argc, char **argv)
{
    const char *conf = 0, *clockfile = 0;
    uint32_t build = 0, rtc = 0, floor;
    int have_rtc = 0, wait_ms = SNTP_MAXWAIT * 1000, port = SNTP_PORT, i, have_file = 0;
    uint32_t file = 0;
    static char text[4096];
    struct sntp_conf c;
    long deadline;
    size_t n;
    FILE *f;

    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--conf"))
            conf = argv[i + 1];
        else if (!strcmp(argv[i], "--clockfile"))
            clockfile = argv[i + 1];
        else if (!strcmp(argv[i], "--build"))
            build = (uint32_t)strtoul(argv[i + 1], 0, 10);
        else if (!strcmp(argv[i], "--rtc"))
            rtc = (uint32_t)strtoul(argv[i + 1], 0, 10), have_rtc = 1;
        else if (!strcmp(argv[i], "--wait-ms"))
            wait_ms = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--port"))
            port = atoi(argv[i + 1]);
        else
            return 2;
    }
    if (!conf || !clockfile || !(f = fopen(conf, "rb")))
        return 2;
    n = fread(text, 1, sizeof text, f);
    fclose(f);
    sntp_conf_parse(&c, text, n, warn, 0);
    if ((f = fopen(clockfile, "rb"))) {
        char b[ZC_CLOCK_LINE];
        size_t k = fread(b, 1, sizeof b, f);

        fclose(f);
        have_file = zc_clock_parse(b, k, &file) == 0;
    }
    floor = zc_clock_floor(build, have_file, file);
    deadline = now_ms() + wait_ms;
    for (i = 0; i < c.n; i++) {
        struct sockaddr_in sin;
        unsigned char q[SNTP_LEN], nonce[8], p[512];
        struct sntp_answer a;
        int s, r, fd = open("/dev/urandom", O_RDONLY), done = 0;
        long sent, mine;

        if (fd < 0 || read(fd, nonce, 8) != 8)
            return 3;
        close(fd);
        memset(&sin, 0, sizeof sin);
        sin.sin_family = AF_INET;
        sin.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, c.host[i], &sin.sin_addr) != 1) {
            printf("server %s: refused not-an-address\n", c.host[i]);
            continue;
        }
        s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0 || connect(s, (struct sockaddr *)&sin, sizeof sin))
            return 3;
        sntp_request(q, nonce);
        sent = now_ms();
        /* each server its share of what is left, so one that answers garbage cannot use up the others' time */
        mine = sent + (deadline - sent) / (c.n - i);
        if (send(s, q, sizeof q, 0) != (ssize_t)sizeof q) {
            close(s);
            continue;
        }
        while (!done) {
            struct pollfd pf = { s, POLLIN, 0 };
            long left = mine - now_ms();
            ssize_t k;

            if (left <= 0 || poll(&pf, 1, (int)left) != 1) {
                printf("server %s: timeout\n", c.host[i]);
                break;
            }
            k = recv(s, p, sizeof p, 0);
            if (k < 0)
                break;
            r = sntp_parse(&a, p, (size_t)k, nonce);
            if (r == SNTP_EORIGIN || r == SNTP_ESHORT) {
                printf("server %s: dropped %d\n", c.host[i], r);
                continue;               /* not our answer: keep waiting for it */
            }
            done = 1;
            if (r == SNTP_EKOD) {
                printf("server %s: kod %s\n", c.host[i], a.kod);
                break;
            }
            if (r) {
                printf("server %s: refused %d\n", c.host[i], r);
                break;
            }
            {
                uint32_t t = sntp_time(&a, (uint32_t)(now_ms() - sent));
                int j = sntp_judge(t, floor, have_rtc, rtc);

                if (!j) {
                    printf("server %s: refused before-the-floor %u\n", c.host[i], t);
                    break;
                }
                printf("server %s: ok %u\n", c.host[i], t);
                printf("result set %u raise %d\n", t, (j & SNTP_RAISE) != 0);
                if (j & SNTP_RAISE) {
                    char b[ZC_CLOCK_LINE];
                    int len = zc_clock_line(b, t);

                    if (!(f = fopen(clockfile, "wb")) || fwrite(b, 1, (size_t)len, f) != (size_t)len)
                        return 3;
                    fclose(f);
                }
                close(s);
                return 0;
            }
        }
        close(s);
    }
    printf("result none\n");
    return 1;
}
