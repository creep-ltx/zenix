/* SPDX-License-Identifier: GPL-2.0-only */
/* The certificate time and /var/lib/clock (DESIGN D21 8.3; zc_clock.h). No system calls, no 64-bit arithmetic. */
#include "zc_clock.h"

static const char magic[] = "zenix-clock 1 ";

int zc_clock_parse(const char *buf, size_t n, uint32_t *secs)
{
    size_t i, m = sizeof magic - 1;
    uint32_t v = 0;

    if (n < m + 1)
        return -1;
    for (i = 0; i < m; i++)
        if (buf[i] != magic[i])
            return -1;
    if (buf[n - 1] == '\n')
        n--;
    if (n <= m || (buf[m] == '0' && n > m + 1))
        return -1;                      /* no leading zeros: one spelling for each number */
    for (i = m; i < n; i++) {
        unsigned d = (unsigned)(unsigned char)buf[i] - '0';

        if (d > 9 || v > (0xFFFFFFFFu - d) / 10)
            return -1;
        v = v * 10 + d;
    }
    *secs = v;
    return 0;
}

int zc_clock_line(char *buf, uint32_t secs)
{
    char t[11];
    int k = 0, n = 0;
    size_t i;

    for (i = 0; magic[i]; i++)
        buf[n++] = magic[i];
    do {
        t[k++] = (char)('0' + secs % 10);
        secs /= 10;
    } while (secs);
    while (k)
        buf[n++] = t[--k];
    buf[n++] = '\n';
    buf[n] = 0;
    return n;
}

uint32_t zc_clock_floor(uint32_t build, int has_file, uint32_t file)
{
    return has_file && file > build ? file : build;
}

void zc_clock_days(uint32_t t, uint32_t *days, uint32_t *secs)
{
    *days = t / 86400u + ZC_UNIX_DAY0;
    *secs = t % 86400u;
}

/* is (d1, s1) before (d2, s2)? */
static int before(uint32_t d1, uint32_t s1, uint32_t d2, uint32_t s2)
{
    return d1 < d2 || (d1 == d2 && s1 < s2);
}

int zc_clock_check(void *ctx, uint32_t nbd, uint32_t nbs, uint32_t nad, uint32_t nas)
{
    struct zc_clock *c = ctx;
    uint32_t d, s;

    if (c->trusted) {
        zc_clock_days(c->now > c->floor ? c->now : c->floor, &d, &s);
        if (c->now < c->floor)
            c->used_floor = 1;
        if (before(d, s, nbd, nbs))
            return -1;
        return before(nad, nas, d, s) ? 1 : 0;
    }
    /* no trusted time: only "not after", against the floor (a certificate that expired before it is refused) */
    c->used_floor = 1;
    zc_clock_days(c->floor, &d, &s);
    return before(nad, nas, d, s) ? 1 : 0;
}
