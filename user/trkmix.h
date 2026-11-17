/* SPDX-License-Identifier: GPL-2.0-only */
/* The tracker player's mixer (DESIGN D56 section 5): a module's channels, more than Paula's four or not Paula's kind,
 * mixed in the CPU into one 8-bit stereo stream. No interpolation, no volume ramps: each channel steps through its
 * 8-bit sample at its rate (16.16 fixed point) and adds sample x volume from a table into the left, right or middle
 * sum. Plain C (and a 68k loop), no system calls: the PC's tests build it as it is. */
#ifndef TRKMIX_H
#define TRKMIX_H
#include "trk.h"

#define MIX_CHUNK 512           /* frames a pass mixes at most (the sums' length) */
#define MIX_CLIP 4096           /* the clipping table's reach either side of 0: at the gain mix_init sets, any number of
                                 * channels sums to at most 3 x 1,016 (three at the gain of one) */

struct mixch {
    const signed char *p;       /* the segment playing (0: silent) */
    unsigned n;                 /* its length in bytes */
    unsigned x;                 /* the place in it, 16.16 */
    const signed char *lp;      /* the loop that follows it (ln 0: silence follows) */
    unsigned ln;
    unsigned step;              /* 16.16 bytes an output frame */
    int vol, side;              /* side 0 left, 1 right, 2 the middle (both) */
};

struct mix {
    unsigned rate, clock;       /* the output's frames a second; Paula's clock (a period's unit) */
    unsigned kper;              /* clock x 65536 / rate: a period's step is kper / per */
    int nch, amp;               /* amp: the gain x 256 into 8 bits */
    struct mixch c[TRK_MAXCH];
    short *acc;                 /* the three sums, MIX_CHUNK each */
    short (*vt)[256];           /* vt[vol][byte]: the signed byte x vol x amp / 256 */
    signed char *clip;          /* 2 x MIX_CLIP bytes: a sum -> its byte, clipped */
};

/* -> 0, or -1: no memory (about 44 KB: the volume table, the clipping table, the sums) */
int mix_init(struct mix *m, unsigned rate, unsigned clock, int nch);
void mix_free(struct mix *m);
/* a tick's registers into the channels */
void mix_set(struct mix *m, const struct tvoice *v);
/* n frames of left and right bytes (interleaved: L R L R ...) */
void mix_render(struct mix *m, signed char *out, int n);
#endif
