/* SPDX-License-Identifier: GPL-2.0-only */
/* The sound server's client library (DESIGN D40 section 5; the protocol in user/sndproto.h). Every call answers >= 0
 * or -errno; -ECONNRESET means the server ended (ZINE, a crash): the stream is gone, open a new one. */
#ifndef SNDLIB_H
#define SNDLIB_H
#include "sndproto.h"

int snd_open(unsigned rate, unsigned flags, unsigned vol, unsigned half_ms);  /* -> a stream handle */
int snd_write(int h, const void *p, unsigned n);    /* blocks until all n bytes are in the server's ring */
int snd_drain(int h);                               /* blocks until everything written has played */
int snd_close(int h);
int snd_volume(int h, unsigned vol);
int snd_info(struct sndinfo *i, int h);
int snd_state(struct sndstate *s);                  /* the tests' state block */
int snd_bank(int h, const void *p, unsigned n);
int snd_frames(int h, const struct sndframe *f, unsigned n);    /* -> frames played so far */
int snd_pause(int h, int on);
const char *snd_why(int e);                         /* a short reason for an error of these calls */
#endif
