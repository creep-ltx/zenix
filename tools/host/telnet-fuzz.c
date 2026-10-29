/* SPDX-License-Identifier: GPL-2.0-only */
/* telnet's core under libFuzzer (DESIGN D20 8.1): the decoder (IAC, options, SB), the filter (music strings, CSI, 0x9B,
 * UTF-8) and the keys, on any bytes, in every mode. Besides the sanitizers it checks the bounds the program relies
 * on: the decoder never gives more than it got, answers stay in proportion to what came (no negotiation loop), a
 * screen write is at most 256 bytes, the encoder and the keys at most double. Built by tools/host/telnet-test.sh fuzz.
 * The first byte chooses: bits 0-1 the mode (0 and 3 Unix, 1 PC, 2 Amiga), bit 2 the CP437 fallback, bit 3 raw,
 * bit 4 port 23; the second the size of the pieces the rest is fed in (1-64). */
#include <stdlib.h>
#include <string.h>
#include "../../net/telnet/telnet.h"

static long sent, screen, answered;
static void tsend(void *u, const unsigned char *b, long n) { (void)u; (void)b; sent += n; }
static void sput(void *u, const unsigned char *b, long n)
{
    (void)u; (void)b;
    if (n < 1 || n > 256)
        abort();
    screen += n;
}
static void sans(void *u, const unsigned char *b, long n) { (void)u; (void)b; if (n < 1 || n > 32) abort(); answered += n; }
static struct tf *cur;
static void sev(void *u, int ev, int arg)
{
    (void)u;
    if ((ev != TF_EV_ICE && ev != TF_EV_MODE) || (ev == TF_EV_MODE && arg != TN_PC && arg != TN_AMIGA))
        abort();
    if (ev == TF_EV_MODE && cur->mode != arg)
        abort();                        /* the filter has switched before it tells */
}

int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n)
{
    static unsigned char out[65536 * 2];
    struct tn t;
    struct tf f;
    unsigned long i, piece;
    int mode, bell = 0;
    long k;
    if (n < 2 || n > 65536)
        return 0;
    mode = (d[0] & 3) == 1 ? TN_PC : (d[0] & 3) == 2 ? TN_AMIGA : TN_UNIX;
    piece = (d[1] & 63) + 1;
    memset(&t, 0, sizeof t);
    t.send = tsend;
    t.mode = mode;
    t.cols = 80;
    t.rows = d[1];
    t.raw = (d[0] >> 3) & 1;
    memset(&f, 0, sizeof f);
    f.put = sput;
    f.answer = sans;
    f.event = sev;
    f.cols = 80;
    f.rows = 32;
    f.fallback = (d[0] >> 2) & 1;
    cur = &f;
    sent = screen = answered = 0;
    tn_init(&t, (d[0] & 16) ? 23 : 2323);
    tf_init(&f, mode);
    d += 2;
    n -= 2;
    for (i = 0; i < n; i += piece) {
        unsigned long m = n - i < piece ? n - i : piece;
        k = tn_decode(&t, d + i, (long)m, out);
        if (k < 0 || (unsigned long)k > m)
            abort();
        tf_write(&f, out, k);
        if (f.seqn < 0 || f.seqn > TF_SEQMAX || f.musicn > TF_MUSICMAX || f.obn != 0)
            abort();
    }
    if (sent > 64 + 7 * (long)n || screen > 64 + 7 * (long)n || answered > 32 * (long)n)
        abort();                        /* out of proportion: a loop or a runaway rewrite */
    k = tn_encode(&t, d, (long)n, out);
    if (k < (long)n || k > 2 * (long)n)
        abort();
    k = tk_keys(mode, n & 1, d, (long)n, out, &bell);
    if (k < 0 || k > 2 * (long)n || bell > (long)n)
        abort();
    tn_size(&t, n > 1 ? d[0] | d[1] << 8 : 80, 255);
    return 0;
}
