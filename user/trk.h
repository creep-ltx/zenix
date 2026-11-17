/* SPDX-License-Identifier: GPL-2.0-only */
/* The tracker player's replayers behind one face (DESIGN D56, docs/d56-tracker-player.md): a module of any format it
 * plays, one tick at a time, into each channel's registers for that tick. Plain C with no system calls (malloc and
 * free only, from libz or the PC's C library), so the PC's tests build it as it is (tools/trk-host-test.sh). */
#ifndef TRK_H
#define TRK_H

#define TRK_MAXCH 32            /* channels a module may have */

#define TV_TRIG 1               /* the sample starts this tick at start (len bytes), then loops at loop (looplen) */
#define TV_LOOP 8               /* only the next loop is this one's (ProTracker's instrument without a note) */
#define TV_CUT 16               /* the channel is silent from now (its sample stopped) */

/* a channel's registers this tick. Places are bytes of 8-bit signed sample data from sp (every format's samples are
 * turned into 8-bit signed at load); looplen 0: no loop (silence after len). per: a Paula period (the Amiga formats),
 * else 0 and hz16 the rate in 1/16 Hz (the PC formats). */
struct tvoice {
    int flags, per, vol, pan;   /* vol 0-64 with every volume applied; pan 0 left .. 128 middle .. 255 right */
    unsigned hz16;
    const signed char *sp;
    unsigned start, len, loop, looplen;
};

enum { TF_MOD = 1, TF_MED, TF_S3M, TF_XM };

struct trk {
    int fmt;
    char kind[24];              /* "ProTracker", "8-channel MOD", "OctaMED MMD1", "Scream Tracker 3", ... */
    char title[33];
    int nch, songlen, ninst;    /* channels, positions, instruments (or samples) */
    int native;                 /* Paula can play it as it is: <= 4 channels, periods only, one bank of samples */
    const signed char *bank;    /* native: every sample inside bank .. bank + banksize */
    unsigned banksize;
    /* the song's state, after the last trk_tick: the row that tick played */
    int pos, row, rows, pat, speed, tempo, tick;
    int ended;                  /* the song came to its end (or looped back, or was stopped) */
    int stop;                   /* the song asked to stop (F00, FFE) */
    unsigned tnum, tden;        /* the next tick lasts tnum / tden seconds */
    struct tvoice v[TRK_MAXCH];
    const struct trkops *ops;
    void *st;                   /* the replayer's state */
};

struct trkops {
    void (*tick)(struct trk *);
    void (*go)(struct trk *, int pos);
    int (*insname)(struct trk *, int i, char *out, int n);     /* the instrument's name, 0-based; its length */
    int (*rows)(struct trk *, int pat);                         /* the rows of pattern pat */
    void (*cell)(struct trk *, int pat, int row, int ch, char out[16]);    /* "C-2 01 v64 A0F" (13 at most) */
    void (*done)(struct trk *);
};

/* read a module (the whole file in memory: the data must stay, and may be changed in place) -> 0, or -1 with why
 * (64 bytes) said */
int trk_load(struct trk *t, unsigned char *data, unsigned size, char *why);
static inline void trk_tick(struct trk *t) { t->ops->tick(t); }
static inline void trk_goto(struct trk *t, int pos) { t->ops->go(t, pos); }
void trk_free(struct trk *t);
/* the next tick's length in the units of a clock of hz ticks a second */
unsigned trk_ticklen(const struct trk *t, unsigned hz);
/* a note's name ("C-2") for a note number 0 = C-0 .. 119 = B-9; "---" for -1, "===" for -2 (note off) */
void trk_notename(int note, char out[4]);

/* each format's loader: -> 1 it is this format (loaded, or -1 with why), 0 not this format */
int mod_trk(struct trk *t, unsigned char *d, unsigned size, char *why);
int med_trk(struct trk *t, unsigned char *d, unsigned size, char *why);
int s3m_trk(struct trk *t, unsigned char *d, unsigned size, char *why);
int xm_trk(struct trk *t, unsigned char *d, unsigned size, char *why);

/* shared helpers for the replayers */
void trk_say(char *why, const char *s);
/* a pattern cell as text: "C-2 01 64 A0F" (note, instrument, volume column, effect; dots for none) */
void trk_cellfmt(char out[16], const char *note, int ins, int vol, int eff, int prm);
extern const unsigned short trk_sine[32];       /* the vibrato's sine: 0..255, the first half of a wave */
void *trk_alloc(unsigned n);                    /* zeroed; NULL when there is no memory */
void trk_release(void *p);
#endif
