/* SPDX-License-Identifier: GPL-2.0-only */
/* The SNTP client's core (DESIGN D21 8.3): see sntp.h. No system calls, no allocation, no 64-bit arithmetic. */
#include <string.h>
#include "sntp.h"

#define NTP_UNIX 2208988800u            /* seconds from 1900 to 1970 */

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static int sp(int c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static int host_ok(const char *s, size_t n)
{
    size_t i;

    if (n == 0 || n > SNTP_HOSTMAX)
        return 0;
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') ||
              s[i] == '.' || s[i] == '-'))
            return 0;
    return 1;
}

int sntp_conf_parse(struct sntp_conf *c, const char *t, size_t n, sntp_warn warn, void *u)
{
    size_t i = 0;
    int line = 0;

    memset(c, 0, sizeof *c);
    while (i < n) {
        size_t s = i, e, w, we, h, he;

        while (i < n && t[i] != '\n')
            i++;
        e = i++;
        line++;
        for (w = s; w < e && t[w] && t[w] != '#'; w++)  /* a comment or a NUL ends the line's meaning */
            ;
        e = w;
        for (w = s; w < e && sp(t[w]); w++)
            ;
        if (w == e)
            continue;
        for (we = w; we < e && !sp(t[we]); we++)
            ;
        for (h = we; h < e && sp(t[h]); h++)
            ;
        for (he = h; he < e && !sp(t[he]); he++)
            ;
        if (we - w != 6 || memcmp(t + w, "server", 6)) {
            if (warn)
                warn(u, line, "unknown word, line ignored");
            continue;
        }
        if (!host_ok(t + h, he - h)) {
            if (warn)
                warn(u, line, "no host name, or a bad one, line ignored");
            continue;
        }
        {
            size_t x;

            for (x = he; x < e && sp(t[x]); x++)
                ;
            if (x < e && warn)
                warn(u, line, "words after the host name ignored");
        }
        if (c->n == SNTP_SERVERS) {
            if (warn)
                warn(u, line, "more than four servers, line ignored");
            continue;
        }
        memcpy(c->host[c->n], t + h, he - h);
        c->host[c->n][he - h] = 0;
        c->n++;
    }
    return c->n;
}

void sntp_request(unsigned char pkt[SNTP_LEN], const unsigned char nonce[8])
{
    memset(pkt, 0, SNTP_LEN);
    pkt[0] = 0 << 6 | 4 << 3 | 3;       /* LI 0, VN 4, mode 3 */
    memcpy(pkt + 40, nonce, 8);
}

/* NTP seconds to Unix seconds by RFC 4330 section 3's era rule; -1 before 1970 */
static int ntp_unix(uint32_t s, uint32_t *u)
{
    if (s & 0x80000000u) {              /* 1968-2036 */
        if (s < NTP_UNIX)
            return -1;
        *u = s - NTP_UNIX;
    } else {                            /* 2036-2104: s + 2^32 - NTP_UNIX */
        *u = s + (0xFFFFFFFFu - NTP_UNIX) + 1u;
    }
    return 0;
}

int sntp_parse(struct sntp_answer *a, const unsigned char *p, size_t n, const unsigned char nonce[8])
{
    int mode;

    memset(a, 0, sizeof *a);
    if (n < SNTP_LEN)
        return SNTP_ESHORT;
    a->leap = p[0] >> 6;
    a->version = p[0] >> 3 & 7;
    mode = p[0] & 7;
    a->stratum = p[1];
    if (mode != 4)
        return SNTP_EMODE;
    if (a->version != 3 && a->version != 4)
        return SNTP_EVERSION;
    if (memcmp(p + 24, nonce, 8))
        return SNTP_EORIGIN;            /* checked before anything else is believed, a kiss of death included */
    if (a->stratum == 0) {
        int i;

        for (i = 0; i < 4; i++)
            a->kod[i] = p[12 + i] >= 0x20 && p[12 + i] < 0x7f ? (char)p[12 + i] : '?';
        a->kod[4] = 0;
        return SNTP_EKOD;
    }
    if (a->stratum >= 16)
        return SNTP_ESTRATUM;
    if (a->leap == 3)
        return SNTP_ELEAP;
    if (!be32(p + 40) && !be32(p + 44))
        return SNTP_ETIME;
    if (ntp_unix(be32(p + 40), &a->tx_secs))
        return SNTP_ETIME;
    a->tx_frac = be32(p + 44);
    if (ntp_unix(be32(p + 32), &a->rx_secs)) {
        a->rx_secs = a->tx_secs;        /* no usable receive time: no server time taken out */
        a->rx_frac = a->tx_frac;
    } else {
        a->rx_frac = be32(p + 36);
    }
    return SNTP_OK;
}

/* a 2^-32 fraction in milliseconds */
static uint32_t frac_ms(uint32_t f)
{
    return ((f >> 16) * 1000u) >> 16;
}

uint32_t sntp_time(const struct sntp_answer *a, uint32_t elapsed_ms)
{
    uint32_t server_ms = 0, delay, ms;

    /* the server's time between its stamps, when they are in order and less than the round trip */
    if (a->tx_secs > a->rx_secs || (a->tx_secs == a->rx_secs && a->tx_frac >= a->rx_frac)) {
        uint32_t ds = a->tx_secs - a->rx_secs;

        if (ds < 1000000u) {
            uint32_t txm = frac_ms(a->tx_frac), rxm = frac_ms(a->rx_frac);

            server_ms = ds * 1000u + txm - rxm; /* (unsigned: txm < rxm only when ds >= 1) */
        } else {
            server_ms = 0xFFFFFFFFu;
        }
    }
    delay = server_ms < elapsed_ms ? elapsed_ms - server_ms : 0;
    ms = frac_ms(a->tx_frac) + delay / 2 + 500;    /* rounded to the second */
    if (a->tx_secs > 0xFFFFFFFFu - ms / 1000)
        return 0xFFFFFFFFu;
    return a->tx_secs + ms / 1000;
}

int sntp_judge(uint32_t t, uint32_t floor, int have_rtc, uint32_t now)
{
    if (t < floor)
        return 0;
    if (have_rtc)
        return SNTP_SET | ((t >= now ? t - now : now - t) <= SNTP_AGREE ? SNTP_RAISE : 0);
    return SNTP_SET | (t - floor <= SNTP_AHEAD ? SNTP_RAISE : 0);
}
