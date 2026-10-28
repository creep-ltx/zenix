/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/*
 * The SNTP client's core (DESIGN D21 8.3; RFC 4330 / RFC 5905 version 4), without system calls: the request, the
 * checks of a reply, the time it gives, the guard that keeps one false answer from poisoning the floor, and the
 * file /etc/sntp (frozen once shipped: "server HOST" lines, at most four, # comments, unknown words reported and
 * ignored). `sntp` (user/sntp.c, a root command that ifup starts) owns the sockets, the 10-second limit, the order
 * of the servers, SYS_settime and the writing of /var/lib/clock (crypto/zc/zc_clock.h's format).
 * Times are unsigned 32-bit Unix seconds (good until 2106); NTP's era is resolved by RFC 4330 section 3's rule (the
 * top bit set: 1968-2036, clear: 2036-2104). No 64-bit arithmetic.
 */
#include <stddef.h>
#include <stdint.h>

#define SNTP_PORT     123
#define SNTP_LEN      48                /* the packet without extensions or a MAC */
#define SNTP_SERVERS  4                 /* /etc/sntp's limit */
#define SNTP_HOSTMAX  255
#define SNTP_MAXWAIT  10                /* seconds sntp waits in all */
#define SNTP_AGREE    600               /* the floor rises when the answer is within 10 minutes of the battery clock */
#define SNTP_AHEAD    (400u * 86400u)   /* ... or, with no battery clock, at most 400 days past the floor */

/* ---- /etc/sntp ---- */
struct sntp_conf {
    int n;
    char host[SNTP_SERVERS][SNTP_HOSTMAX + 1];
};
/* warn(u, line, what): a line that was ignored and why (unknown word, a fifth server, a bad host name) */
typedef void (*sntp_warn)(void *u, int line, const char *what);
/* Parse the file's bytes (n of them; NUL bytes end a line's meaning). Returns the number of servers. */
int sntp_conf_parse(struct sntp_conf *c, const char *text, size_t n, sntp_warn warn, void *u);

/* ---- one exchange ---- */
/* The request: version 4, mode 3 (client), every field zero but the transmit timestamp, which is the 8 random bytes
   of nonce (the clock is not revealed; the reply's originate timestamp must be them). */
void sntp_request(unsigned char pkt[SNTP_LEN], const unsigned char nonce[8]);

struct sntp_answer {
    uint32_t rx_secs, rx_frac;          /* the server's receive time (Unix seconds, 2^-32 fractions) */
    uint32_t tx_secs, tx_frac;          /* its transmit time */
    int stratum, version, leap;
    char kod[5];                        /* a kiss of death's code ("DENY", "RSTR", "RATE"...), else "" */
};
#define SNTP_OK        0
#define SNTP_ESHORT   -1                /* shorter than 48 bytes */
#define SNTP_EMODE    -2                /* not a server's reply (mode 4) */
#define SNTP_EVERSION -3                /* not version 3 or 4 */
#define SNTP_EORIGIN  -4                /* its originate timestamp is not our nonce: not the answer to our request */
#define SNTP_EKOD     -5                /* a kiss of death (stratum 0): ask this server nothing more (a->kod) */
#define SNTP_ESTRATUM -6                /* stratum 16 or more (unsynchronised) */
#define SNTP_ELEAP    -7                /* leap indicator 3: the server's clock is not synchronised */
#define SNTP_ETIME    -8                /* no transmit time, or one before 1970 */
/* Check a reply (n bytes as received) against the nonce sent. SNTP_OK and *a filled, or SNTP_E*. */
int sntp_parse(struct sntp_answer *a, const unsigned char *pkt, size_t n, const unsigned char nonce[8]);
/* The time now by the answer: the transmit time plus half the round trip, rounded to the second. elapsed_ms: the
   local time between sending and receiving (sntp measures it with the tick); the server's own time between its
   receive and transmit stamps is taken out. */
uint32_t sntp_time(const struct sntp_answer *a, uint32_t elapsed_ms);

/* ---- the guard (8.3) ---- */
#define SNTP_SET    1                   /* set the clock (SYS_settime) */
#define SNTP_RAISE  2                   /* and raise the floor: write t to /var/lib/clock */
/* t: sntp_time's; floor: zc_clock_floor's; have_rtc: a battery clock was read at boot; now: SYS_time (the battery
   clock's time when have_rtc). An answer before the floor is refused (0): the time can never be before it. */
int sntp_judge(uint32_t t, uint32_t floor, int have_rtc, uint32_t now);
