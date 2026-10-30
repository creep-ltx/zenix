/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* paula - sound with no libraries: ProTracker modules (core/ptplay.c)
 * and 8SVX sounds (core/svx.c) played straight on Paula, so every
 * Amiga from Kickstart 1.3 plays them.
 *
 * The OS-friendly way: audio.device gives the four channels
 * (ADCMD_ALLOCATE, no waiting: a failure is a message, another
 * program's sound is never cut) and a lock (ADCMD_LOCK: if a program
 * of higher priority takes them, the interrupt stops touching the
 * chip at once and the status says so). The ticks come from a CIA-B
 * timer (ciab.resource, AddICRVector - there on 1.3) at the module's
 * tempo, or from a vertical-blank server when both CIA-B timers are
 * taken. The samples are copied into chip memory. Stopping undoes it
 * all: interrupt out, DMA off, volumes 0, filter back, channels and
 * memory freed.
 *
 * One sound at a time: snd_load, snd_start, snd_status..., snd_stop,
 * snd_free.
 */
#ifndef CF_PAULA_H
#define CF_PAULA_H

#define SND_NONE 0
#define SND_MOD 1
#define SND_SVX 2

struct sndstat {
    int kind;
    int pos, songlen, row;      /* a module: the position and row sounding */
    int ended;                  /* F00 stopped the song, or a one-shot sound has played */
    int lost;                   /* another program took the sound channels */
    unsigned long ticks;        /* timer ticks so far */
    unsigned long irqs, linesum, linemax;   /* the interrupt's cost, in raster lines (64 us) */
    unsigned long replines;     /* of that, the replay's own work (no DMA waits, no chip writes) */
};

/* the kind of a sound file from its first bytes (1084 make a module sure) */
int snd_sniff(const unsigned char *b, long n, long fsize);

/* loads path as kind (SND_NONE: sniff it): FALSE with why[] said */
int snd_load(const char *path, int kind, char *why, int whysize);
/* a line about what is loaded: the song's title, or the sound's rate */
const char *snd_about(void);
/* takes the channels, starts the timer: FALSE with why[] said */
int snd_start(char *why, int whysize);
void snd_status(struct sndstat *s);
/* stops: interrupt out, DMA off, volumes 0, filter back, channels freed */
void snd_stop(void);
/* frees what snd_load took (stops first if still playing) */
void snd_free(void);
/* how the last stop got the ADCMD_LOCK request back: 0 with the
 * channels (ADCMD_FREE), 1 by AbortIO, 2 not at all (left, not waited for) */
extern int snd_lockback;
/* a test hook: TRUE = tick from the vertical blank even when a CIA-B timer is free */
extern int snd_forcevb;
/* what drives the ticks: "CIA-B timer A", "CIA-B timer B", "vertical blank" */
const char *snd_timername(void);

#endif
