/* SPDX-License-Identifier: GPL-2.0-only */
/* The sound file reader of play (DESIGN D40 section 9, docs/d40-sound.md): WAV, 8SVX, AIFF/AIFF-C and raw bytes, read
 * in order from any source (a file or a pipe: no seeking), turned into 8-bit signed samples for Paula. No system calls:
 * the PC's test (tools/sound-host-test.sh) builds it as it is. */
#ifndef SNDDEC_H
#define SNDDEC_H

enum { SDF_RAW, SDF_WAV, SDF_8SVX, SDF_AIFF };

struct sdec {
    int (*rd)(void *ctx, void *buf, int n);     /* bytes read, 0 at the end, < 0 an error */
    void *ctx;
    int fmt;                    /* SDF_* */
    unsigned rate;              /* the file's samples a second */
    int chans;                  /* 1 or 2 */
    int bits;                   /* 8 or 16 */
    int usign;                  /* 8-bit samples are unsigned (WAV, raw -u) */
    int big;                    /* 16-bit samples are big-endian (AIFF) */
    unsigned left;              /* bytes of sample data still to read */
    unsigned first;             /* 8SVX with several octaves: the bytes of the first */
    int vol;                    /* 8SVX's volume, 0-64 (64 elsewhere) */
    unsigned div;               /* sdec_out: this many of the file's frames make one of Paula's */
    unsigned outrate;           /* rate / div */
    int mono;                   /* sdec_out: stereo mixed to one channel */
    /* 8SVX: Fibonacci-delta packing, and the stereo body held in memory (left half, then right) */
    int fib, fibx[2], fibhalf[2];
    unsigned char *body;        /* the whole BODY, for stereo 8SVX (a pipe cannot be read twice) */
    unsigned bodylen, bodypos[2];
    unsigned char raw[4096];    /* the reader's buffer */
    int rawn, rawp;
    char why[64];               /* why sdec_open or sdec_read failed */
};

/* the format from the first bytes (at least 12), or SDF_RAW if none is known */
int sdec_sniff(const unsigned char *h, int n);
/* read the headers up to the sample data; the source is at the start. 0, or < 0 with d->why said. For SDF_RAW set rate,
 * chans, bits (8) and usign first: nothing is read. */
int sdec_open(struct sdec *d);
/* the output: at most maxrate samples a second (Paula's fastest), mono if asked or if the file is mono */
void sdec_out(struct sdec *d, unsigned maxrate, int mono);
/* up to n frames of 8-bit signed samples into out (left, right interleaved when the output is stereo) -> frames, 0 at
 * the end, < 0 an error (d->why) */
int sdec_read(struct sdec *d, signed char *out, int n);
/* free what sdec_open allocated */
void sdec_close(struct sdec *d);
/* the 80-bit IEEE extended number of an AIFF's COMM chunk as a whole number (0 if out of range) */
unsigned sdec_ext80(const unsigned char *p);

/* the hook for memory (play: malloc; the test: malloc) */
void *sdec_alloc(unsigned n);
void sdec_free(void *p);
#endif
