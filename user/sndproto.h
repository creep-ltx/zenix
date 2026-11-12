/* SPDX-License-Identifier: GPL-2.0-only */
/* The sound server's protocol (DESIGN D40 section 5, docs/d40-sound.md): port "audio", served by /sbin/sndd. Frozen
 * once merged: types, flags and the structs' first sizes only grow at the end. */
#ifndef SNDPROTO_H
#define SNDPROTO_H

#define SND_PORT "audio"

/* A stream is named by its handle: this sndd's start count (D11's SVC_SELF) << 8 | its slot 1..SND_MAXSTREAM. A handle
 * of an earlier sndd (one that ended: ZINE, a crash) is answered -ECONNRESET: open again. */
#define SND_OPEN    1           /* a0 rate (Hz), a1 SNDF_*, a2 volume 0-64, a3 a half's ms (20-1000, 0: 60) -> handle */
#define SND_WRITE   2           /* a0 stream, a1 the bytes (8-bit signed), a2 their count -> the count (blocks) */
#define SND_DRAIN   3           /* a0 stream -> 0 when everything written has played */
#define SND_CLOSE   4           /* a0 stream -> 0; its channels stop at once */
#define SND_VOLUME  5           /* a0 stream, a1 volume 0-64 -> the volume before */
#define SND_INFO    6           /* a0 struct sndinfo *, a1 its size, a2 a stream's handle or 0 -> the bytes written */
#define SND_BANK    7           /* a0 stream (SNDF_FRAMES), a1 sample data, a2 bytes (<= SND_BANKMAX) -> 0 */
#define SND_FRAMES  8           /* a0 stream, a1 struct sndframe *, a2 count (1-4096), a3 SNDFRAME_SIZE -> frames played */
#define SND_PAUSE   9           /* a0 a frames stream, a1 1 pause / 0 go on -> 0 (a PCM stream: -EINVAL) */
#define SND_STATE  10           /* tests only: a0 struct sndstate *, a1 its size -> the bytes written (not frozen) */

#define SNDF_STEREO 1           /* the bytes interleave left and right (channels 0 and 1) */
#define SNDF_FRAMES 2           /* all four channels, played by frames (SND_BANK, SND_FRAMES) */

#define SND_MAXSTREAM 8
#define SND_BANKMAX (1u << 20)  /* a frames stream's sample bank */
#define SND_QFRAMES 16          /* frames the server queues for a stream */
#define SND_MINPER 124          /* a PCM stream's least period (HRM: 123 PAL, 124 NTSC; 124 for both) */
#define SND_FMINPER 113         /* a frame's periods: ProTracker's B-3 (113) .. */
#define SND_FMAXPER 4095        /* .. 4,095 (a frame's DMA-off wait is two periods: at most 2.3 ms) */

/* SND_INFO's answer (first size SNDINFO_FIRST) */
#define SNDI_NTSC 1
struct sndinfo {
    unsigned size, clock, eclock, flags;        /* Paula's Hz, the E-clock's Hz, SNDI_* */
    unsigned period, rate;                      /* the stream's (a2), or 0 */
    unsigned underruns;                         /* of every stream since the server started */
    unsigned freech;                            /* the channels free, a mask (bit n: channel n) */
};
#define SNDINFO_FIRST 32
/* SND_STATE's answer, for the tests (not frozen): DMACONR, and per channel what sndd last wrote and how often it saw
 * the request bit */
struct sndchstate { unsigned short per, vol; unsigned lc, len, irqs, owner; };     /* owner: the stream, 0 free */
struct sndstate {
    struct sndinfo info;
    unsigned dmacon, streams, frames, waits;    /* DMACONR; open streams; frames applied; a wait's limit hit */
    unsigned late, empty;                       /* underruns: sndd late (a half started unsealed); nothing came */
    struct sndchstate ch[4];
};
#define SNDSTATE_SIZE ((unsigned)sizeof(struct sndstate))

/* a frames stream's tick (DESIGN D40 8.2): every place is a byte offset into the stream's bank, every length even;
 * the bank is followed by SND_SILENT, the offset of a silent word (4 zero bytes) */
#define SNDC_TRIG 1             /* start start/len, then loop at loop/looplen */
#define SNDC_PER  2
#define SNDC_VOL  4
#define SNDC_LOOP 8             /* only the next loop's place */
#define SNDC_STOP 16            /* the channel's DMA off */
#define SND_SILENT 0xFFFFFFFFu  /* as an offset: the bank's silent word */
struct sndch { unsigned short flags, period, volume, pad; unsigned start, len, loop, looplen; };
struct sndframe { unsigned ticks; struct sndch ch[4]; };    /* ticks: E-clock ticks to the next frame */
#define SNDFRAME_SIZE 100
#endif
