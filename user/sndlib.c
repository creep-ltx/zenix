/* SPDX-License-Identifier: GPL-2.0-only */
/* The sound server's client library (DESIGN D40 section 5): one synchronous call a request to port "audio" (a D11
 * service: the first call starts /sbin/sndd). */
#include "libz.h"
#include "sndlib.h"

static int port = -1;

static int call(unsigned type, unsigned a0, unsigned a1, unsigned a2, unsigned a3)
{
    if (port < 0) {
        port = sys_port_find(SND_PORT);
        if (port < 0)
            return -ENOENT;
    }
    struct umsg m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.a[0] = a0;
    m.a[1] = a1;
    m.a[2] = a2;
    m.a[3] = a3;
    return sys_call(port, &m);
}

int snd_open(unsigned rate, unsigned flags, unsigned vol, unsigned ms) { return call(SND_OPEN, rate, flags, vol, ms); }
int snd_write(int h, const void *p, unsigned n) { return call(SND_WRITE, (unsigned)h, (unsigned)p, n, 0); }
int snd_drain(int h) { return call(SND_DRAIN, (unsigned)h, 0, 0, 0); }
int snd_close(int h) { return call(SND_CLOSE, (unsigned)h, 0, 0, 0); }
int snd_volume(int h, unsigned vol) { return call(SND_VOLUME, (unsigned)h, vol, 0, 0); }
int snd_info(struct sndinfo *i, int h) { return call(SND_INFO, (unsigned)i, sizeof *i, (unsigned)h, 0); }
int snd_state(struct sndstate *s) { return call(SND_STATE, (unsigned)s, sizeof *s, 0, 0); }
int snd_bank(int h, const void *p, unsigned n) { return call(SND_BANK, (unsigned)h, (unsigned)p, n, 0); }
int snd_frames(int h, const struct sndframe *f, unsigned n)
{
    return call(SND_FRAMES, (unsigned)h, (unsigned)f, n, SNDFRAME_SIZE);
}
int snd_pause(int h, int on) { return call(SND_PAUSE, (unsigned)h, on != 0, 0, 0); }

const char *snd_why(int e)
{
    switch (e) {
    case -ENOENT: return "no sound server (/etc/svctab has no audio line)";
    case -EBUSY: return "sound is in use by another program";
    case -ENOMEM: return "not enough chip RAM";
    case -ECONNRESET: return "the sound server ended";
    }
    const char *t = zstrerror(e);
    return t ? t : "error";
}
