/* SPDX-License-Identifier: GPL-2.0-only */
/* A firmware mailbox property message of one tag (kernel/emmc.c, DESIGN D63): no system calls and no kernel state, so
 * the PC's test (tools/hvs-host-test.c) builds it too. Little-endian, as the VideoCore reads it. */
#pragma once

/* the message for tag with a value buffer of nwords (2 or more: a and b are its first two), its first two words a and b, the rest 0, into m (cap
 * words of room) -> the message's words (6 + nwords: the size, the request code, the tag, the buffer's size, the
 * request's length, the buffer, the end tag), or 0 when it does not fit */
static inline unsigned mbox_build(volatile void *mv, unsigned cap, unsigned tag, unsigned a, unsigned b,
                                  unsigned nwords)
{
    volatile unsigned *m = (volatile unsigned *)mv;
    unsigned n = 6 + nwords;
    if (n > cap)
        return 0;
    for (unsigned i = 0; i < n; i++)
        m[i] = 0;
    m[0] = __builtin_bswap32(n * 4);
    m[2] = __builtin_bswap32(tag);
    m[3] = __builtin_bswap32(nwords * 4);
    m[5] = __builtin_bswap32(a);
    m[6] = __builtin_bswap32(b);
    return n;
}
