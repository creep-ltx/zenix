/* SPDX-License-Identifier: GPL-2.0-only */
/* modplay's replayer (DESIGN D40 section 10): a ProTracker 4-channel module, one tick at a time, into each channel's
 * registers for that tick (period, volume, a sample started). No system calls and no library: the PC's test builds
 * it as it is (tools/sound-host-test.sh). */
#ifndef MODREP_H
#define MODREP_H

#define MOD_TRIG 1              /* the sample starts this tick (start/len, then loop/looplen) */
#define MOD_LOOP 8              /* an instrument without a note: the next loop is this sample's */
#define MOD_MAXCH 32            /* channels: 4 for Paula; 6, 8, ... 32 (FastTracker's tags) through the mixer (D56) */

struct modsmp { unsigned off, len, loop, looplen; int finetune, vol; };  /* bytes, from the bank's start */
struct modout { int flags, period, vol; unsigned start, len, loop, looplen; };
struct modch {
    int smp, period, vol, finetune;     /* the note's state */
    unsigned start, len, loop, looplen;
    int eff, prm;                       /* this row's effect */
    int ptarget, pspeed, gliss;         /* tone portamento */
    int vpos, vspeed, vdepth, vwave;    /* vibrato (vpos 0..63) */
    int tpos, tspeed, tdepth, twave;    /* tremolo */
    int loopr, loopn;                   /* E6: the pattern loop's row and count */
    int offmem;                         /* 9xx: the last offset */
    int dper;                           /* EDx: the delayed note's period (0: none) */
    int trig;                           /* the output's MOD_TRIG / MOD_LOOP this tick */
    int operiod, ovol;                  /* the output (vibrato, tremolo, arpeggio added) */
};
struct mod {
    const unsigned char *pat;           /* the patterns (1,024 bytes each) */
    unsigned banksize;                  /* the sample data after the patterns, up to the file's end */
    int nsmp, songlen, npat, nch;      /* nch: 4, or the tag's (D56) */
    unsigned char order[128];
    struct modsmp s[32];                /* 1..nsmp */
    char title[21];
    /* the song's state */
    int speed, bpm, tick, row, pos;
    int brk, brkrow, jump;              /* a B or D this row: go to jump (-1: the next position), row brkrow */
    int ploop, plrow;                   /* an E6 jumps back this row, to that row */
    int pdelay, pdelayn, rep;           /* EE: the delay asked, rows still to repeat, a repeat now */
    int ended;                          /* the order list was played to its end (or a jump back, or F00) */
    int stop;                           /* F00 */
    struct modch c[MOD_MAXCH];
    struct modout out[MOD_MAXCH];       /* the last tick's registers (nch of them) */
};

/* read the module (the whole file in memory: data must stay) -> 0, or -1 with why (64 bytes) said. maxch: the most
 * channels the caller plays (4: modplay before D56 and zintro; MOD_MAXCH: the tracker player) */
int mod_loadn(struct mod *m, const unsigned char *data, unsigned size, char *why, int maxch);
int mod_load(struct mod *m, const unsigned char *data, unsigned size, char *why);
/* the offset in the file of the sample data (the bank modplay sends to the server) */
unsigned mod_bankoff(const struct mod *m);
/* compute one tick into m->out, then move on to the next */
void mod_tick(struct mod *m);
/* the length of a tick in E-clock ticks: E-clock x 2.5 / BPM (ProTracker's CIA timing) */
unsigned mod_ticklen(const struct mod *m, unsigned eclock);
/* go to position pos, row 0 (n and p in modplay) */
void mod_goto(struct mod *m, int pos);
/* a note's name ("C-2") for a period, "---" if none */
void mod_notename(int period, char out[4]);
extern const short mod_periods[16][36];
#endif
