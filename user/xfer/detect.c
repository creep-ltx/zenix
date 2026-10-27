/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Tobias Karlsson. From CTerm (src/detect.c at 1cbb967,
 * MIT: see the notice at the end of zmodem.c); Zenix keeps the Zmodem part
 * (Hydra is later, DESIGN D20 5).
 *
 * detect - the start of a Zmodem session in what a host sends (telnet looks
 * in the raw bytes, before its decoder: the header has no 0xFF in it).
 */
#include <string.h>
#include "xfer.h"

#define ZDLE 0x18

/* "**" ZDLE "B0" and the type - 1 the host sends (ZRQINIT), 2 it waits
 * for files (ZRINIT). */
int zm_detect(const unsigned char *b, long n, long *at)
{
    long i;
    for (i = 0; i + 6 <= n; i++)
        if (b[i] == '*' && b[i + 1] == '*' && b[i + 2] == ZDLE &&
            b[i + 3] == 'B' && b[i + 4] == '0' &&
            (b[i + 5] == '0' || b[i + 5] == '1')) {
            if (at)
                *at = i;
            return b[i + 5] == '0' ? 1 : 2;
        }
    return 0;
}

/* A start split across two reads. tail: what the caller keeps per
 * connection between reads, SPLIT_TAIL + 1 bytes, zeroed at first
 * (tail[0]: how many follow). Only a start that begins in the read before
 * and ends in b is found here (one wholly in b is zm_detect's); then its
 * kind is returned and *had says how many of its bytes the read before
 * had - already on the screen. Either way the tail takes b's last bytes. */
int split_detect(unsigned char *tail, const unsigned char *b, long n,
                 int *had)
{
    unsigned char w[2 * SPLIT_TAIL];
    int tn = tail[0], k = n < SPLIT_TAIL ? (int)n : SPLIT_TAIL, kind = 0;
    long at = 0;
    if (tn > SPLIT_TAIL)                /* (never: the caller's own bytes) */
        tn = tail[0] = 0;
    memcpy(w, tail + 1, tn);
    memcpy(w + tn, b, k);
    if (tn && (kind = zm_detect(w, tn + k, &at)) && at >= tn)
        kind = 0;                       /* wholly in b */
    if (kind) {
        if (had)
            *had = tn - (int)at;
        tail[0] = 0;                    /* taken: not found twice */
        return kind;
    }
    if (n >= SPLIT_TAIL) {
        memcpy(tail + 1, b + n - SPLIT_TAIL, SPLIT_TAIL);
        tail[0] = SPLIT_TAIL;
    } else {
        int keep = tn + n > SPLIT_TAIL ? SPLIT_TAIL - (int)n : tn;
        memmove(tail + 1, tail + 1 + tn - keep, keep);
        memcpy(tail + 1 + keep, b, n);
        tail[0] = (unsigned char)(keep + n);
    }
    return 0;
}
