/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* svx - IFF 8SVX sounds, and playing one through the Paula state of
 * ptplay.h. Plain C, no Amiga calls.
 *
 * svx_parse walks the FORM's chunks (VHDR, CHAN, NAME, BODY) through
 * an arcsrc, so a sound of any size is parsed without loading it.
 * svx_load puts the samples into the program's sample memory (PT_PAD
 * zero bytes, then left, then right), decoding Fibonacci-delta BODYs.
 * svx_tick then plays them on Paula channels 0 (left) and 1 (right;
 * a mono sound plays on both): the one-shot part once, then the
 * repeat part for ever - split into blocks Paula can take (a LEN is
 * at most 65535 words), the next block's LC/LEN written while the
 * current one plays, the way a long sample is chained on the chip.
 */
#ifndef CF_SVX_H
#define CF_SVX_H

#include "arc.h"
#include "ptplay.h"

struct svxinfo {
    unsigned long oneshot, repeat;  /* samples per channel (the first octave) */
    unsigned long rate;             /* samples a second (0 in the file: 8363) */
    int volume;                     /* 0..64 */
    int stereo;                     /* CHAN said both: BODY is left, then right */
    int fibonacci;                  /* sCompression 1 */
    int octaves;
    unsigned long body, bodylen;    /* the BODY chunk's data in the file */
    unsigned long memsize;          /* the sample memory svx_load needs */
    char name[64];                  /* NAME, or "" */
};

/* ARC_OK, ARC_EUNSUP (compression other than 0/1, not 8SVX) or ARC_ECORRUPT */
int svx_parse(struct arcsrc *src, struct svxinfo *si);

/* reads the samples into mem (si->memsize bytes); ARC_OK or an error */
int svx_load(struct arcsrc *src, const struct svxinfo *si, unsigned char *mem);

/* Fibonacci-delta: n packed bytes (pad, start value, then 4-bit codes)
 * to at most max samples; returns how many it wrote */
unsigned long svx_fib(const unsigned char *in, unsigned long n, unsigned char *out, unsigned long max);

/* one channel's road through the blocks */
struct svxchan {
    unsigned long once, oncelen;    /* the one-shot part: offset, bytes */
    unsigned long loop, looplen;    /* the repeat part (looplen 0: none) */
    unsigned long blk1, blk2;       /* the bytes of one block of each part (even, <= 131068) */
    unsigned long at, left;         /* the block playing: offset in its part, bytes */
    int inloop;
};

struct svxplay {
    struct ptvoice v[4];            /* as ptplay's: what to write to Paula */
    struct svxchan c[2];
    unsigned short per;             /* the period (124 at the least: Paula's limit) */
    unsigned long clocks;           /* Paula clocks played into the block */
    unsigned long played;           /* samples played (for the status line) */
    int started, done;              /* done: a one-shot sound has ended */
    int steady;                     /* a repeat part of one block is in Paula: it loops alone */
};

/* set up the playing; clock is Paula's (3546895 PAL, 3579545 NTSC) */
void svx_start(struct svxplay *p, const struct svxinfo *si, unsigned long clock);
/* one tick: cpt Paula clocks have passed since the last */
void svx_tick(struct svxplay *p, unsigned long cpt);

#endif
