/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* ptplay - a ProTracker 2.3 replayer in plain C.
 *
 * No Amiga calls and no malloc (memory through arc_alloc/arc_free), so
 * it is proven on the host first (tests/pttrace.c renders it and is
 * compared with OpenMPT) and then under vamos. It plays the way the
 * real PT2.3 replay routine does - its effects, its quirks - and
 * instead of poking Paula it fills a "Paula state" per channel each
 * tick: what PT2.3 writes to AUDxLC/LEN/PER/VOL and which channels it
 * restarts (DMA off, then on). The program writes that to the chip
 * (src/paula.c) or renders it (tests/pttrace.c).
 *
 * Sample pointers are offsets into the program's sample memory: PT_PAD
 * zero bytes (the "nothing" a channel plays before any instrument),
 * then the module's sample data as it stands in the file. Every LC/LEN
 * pair it writes stays inside that memory, whatever the file says:
 * sample lengths past the end of the file, loops past the end of a
 * sample and pattern numbers past the patterns stored are clamped
 * when the module is opened.
 *
 * Formats: 31 instruments with M.K. M!K! M&K! FLT4 4CHN, and the
 * 15-instrument Soundtracker layout (no magic; recognised by sanity).
 */
#ifndef CF_PTPLAY_H
#define CF_PTPLAY_H

#define PT_PAD 4            /* zero bytes before the sample data */

/* what pt_probe reads from the first 1084 bytes */
struct ptinfo {
    int ninst;                      /* 31, or 15 for a Soundtracker module */
    int npat;                       /* patterns the order list uses (PT counts all 128) */
    int nstored;                    /* of those, how many the file holds */
    int songlen;                    /* 1..128 */
    unsigned long hdrsize;          /* bytes from the file's start: header + stored patterns */
    unsigned long smpoff;           /* where the sample data starts in the file */
    unsigned long smplen;           /* sample bytes the file holds (0 when cut short) */
    char title[21];
};

/* what the replay wrote to one Paula channel this tick */
struct ptvoice {
    unsigned long lc;               /* AUDxLC now (an offset into the sample memory) */
    unsigned short len;             /* AUDxLEN now, in words, >= 1 */
    unsigned short per;             /* AUDxPER */
    unsigned short vol;             /* AUDxVOL as written (Paula plays >= 64 as 64) */
    unsigned long tlc;              /* when trig: LC and LEN at the restart */
    unsigned short tlen;
    unsigned char trig;             /* restarted this tick: DMA off, LC/LEN, DMA on, then lc/len */
};

struct ptinst {
    unsigned long off;              /* in the sample memory */
    unsigned short len, rep, replen;    /* words, clamped */
    unsigned char vol, ft;
};

struct ptchan {
    unsigned short note, cmd;       /* the cell: n_note, n_cmd (MUST stay a pair) */
    unsigned long start, loopstart, wavestart;
    int peroff;                     /* finetune * 37 into the period table */
    unsigned short length, replen, period, wantedperiod;
    unsigned char finetune, volume, toneportdirec, toneportspeed;
    unsigned char vibratocmd, vibratopos, tremolocmd, tremolopos;
    unsigned char wavecontrol, glissfunk, sampleoffset, pattpos, loopcount, funkoffset;
};

struct ptmod {
    struct ptvoice v[4];            /* the Paula state, read after each pt_tick */
    int songpos, row;               /* the next row to read: position and row */
    int curpos, currow;             /* the row read last (what is sounding) */
    int songlen;
    int speed, bpm;                 /* ticks per row; CIA tempo (125 = 50 ticks/s) */
    int ended;                      /* F00 was played: PT stops here */
    int filter;                     /* 1 = the LED filter is on (E00), 0 = off (E01) */
    unsigned long ticks;            /* ticks played */
    /* ---- the replay's own ---- */
    struct ptchan c[4];
    struct ptinst in[31];
    const unsigned char *patt;      /* the stored patterns */
    const unsigned char *order;     /* 128 entries */
    int nstored;
    unsigned char *smp;             /* the sample memory (the funk effect EFx writes into it) */
    unsigned long smpsize;          /* PT_PAD + sample bytes */
    unsigned short pattpos;         /* row * 16, as PT keeps it */
    unsigned char counter, pbreakpos, posjumpflag, pbreakflag, lowmask;
    unsigned char pattdelaytime, pattdelaytime2;
};

/* is this a module? b holds the first n bytes of a file of fsize bytes */
int pt_probe(const unsigned char *b, unsigned long n, unsigned long fsize, struct ptinfo *pi);

/* set up a replay. hdr: the first pi->hdrsize bytes of the file; smp:
 * PT_PAD + pi->smplen bytes, the file's sample data at smp + PT_PAD.
 * Both must outlive the replay; hdr may be freed only if not needed -
 * the patterns are read from it while playing. Clears the first word
 * of each sample that does not loop (as PT does, so it ends silent).
 * NULL when out of memory. */
struct ptmod *pt_open(const unsigned char *hdr, const struct ptinfo *pi, unsigned char *smp);
void pt_close(struct ptmod *m);

/* one tick: the replay's work for one CIA (or vertical blank) interrupt */
void pt_tick(struct ptmod *m);

#endif
