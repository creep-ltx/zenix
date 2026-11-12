/* SPDX-License-Identifier: GPL-2.0-only */
/* play: sound files through the sound server (DESIGN D40 section 10, docs/man/play.txt): WAV, 8SVX, AIFF/AIFF-C or raw
 * bytes, from files or standard input, turned into 8-bit samples at a rate Paula plays (user/snddec.c). Ctrl-C ends
 * it at once: play catches no signal, so the kernel cancels the waiting write and sndd stops the channels. */
#include "libz.h"
#include "sndlib.h"
#include "snddec.h"

#define CHUNK 512               /* frames a write: under a half at most rates, so the server's ring is topped up in
                                 * small steps and Ctrl-C is felt at once */

void *sdec_alloc(unsigned n) { return malloc(n); }
void sdec_free(void *p) { free(p); }

struct src { int fd; const unsigned char *pre; int npre; };

static int rd(void *ctx, void *buf, int n)
{
    struct src *s = ctx;
    if (s->npre) {
        int k = n < s->npre ? n : s->npre;
        memcpy(buf, s->pre, (u32)k);
        s->pre += k;
        s->npre -= k;
        return k;
    }
    return sys_read(s->fd, buf, (u32)n);
}

static const char *fmtname[] = { "raw", "WAV", "8SVX", "AIFF" };
static int vol = 64, mono, quiet, raw, rawu, raw2, ms;
static unsigned rawrate = 8000;
static struct sndinfo si;

static void usage(void)
{
    dprintf(2, "usage: play [-v VOL] [-m] [-q] [-b MS] [-r [-u] [-2] [-R RATE]] [FILE... | -]\n"
               "       play -S   (the sound server's state)\n");
    sys_exit(2);
}

static void state(void)
{
    struct sndstate x;
    int r = snd_state(&x);
    if (r < 0) { dprintf(2, "play: %s\n", snd_why(r)); sys_exit(2); }
    oprintf("sndd: %s, clock %u Hz, %u stream(s), %u underrun(s) (%u late, %u empty), %u frame(s), %u wait limit(s), "
            "DMACONR %04x\n", x.info.flags & SNDI_NTSC ? "NTSC" : "PAL", x.info.clock, x.streams, x.info.underruns,
            x.late, x.empty, x.frames, x.waits, x.dmacon);
    for (int c = 0; c < 4; c++)
        oprintf("ch%d: %s per %u vol %u len %u reqs %u stream %u\n", c, x.dmacon & (1u << c) ? "on " : "off",
                x.ch[c].per, x.ch[c].vol, x.ch[c].len, x.ch[c].irqs, x.ch[c].owner & 0xFF);
    oflush();
    sys_exit(0);
}

/* play one source -> 0, 1 (this file could not be played), 2 (no sound) */
static int play(const char *name, int fd)
{
    unsigned char head[12];
    int n = 0;
    while (n < 12) {
        int r = sys_read(fd, head + n, (u32)(12 - n));
        if (r <= 0) break;
        n += r;
    }
    static struct sdec d;
    struct src s = { fd, head, n };
    memset(&d, 0, sizeof d);
    d.rd = rd;
    d.ctx = &s;
    d.fmt = raw ? SDF_RAW : sdec_sniff(head, n);
    if (d.fmt == SDF_RAW && !raw) {
        dprintf(2, "play: %s: not a WAV, 8SVX or AIFF file (raw bytes: -r)\n", name);
        return 1;
    }
    if (d.fmt == SDF_RAW) { d.rate = rawrate; d.chans = raw2 ? 2 : 1; d.bits = 8; d.usign = rawu; }
    if (sdec_open(&d) < 0) {
        dprintf(2, "play: %s: %s\n", name, d.why);
        sdec_close(&d);
        return 1;
    }
    sdec_out(&d, si.clock / SND_MINPER, mono);
    int st = d.mono ? 1 : 2, v = vol * d.vol / 64;
    int h = snd_open(d.outrate, d.mono ? 0 : SNDF_STEREO, (unsigned)v, (unsigned)ms);
    if (h < 0) {
        dprintf(2, "play: %s\n", snd_why(h));
        sdec_close(&d);
        return 2;
    }
    struct sndinfo hi;
    snd_info(&hi, h);
    if (!quiet)
        dprintf(1, "%s: %s %d-bit %s %u Hz -> %u Hz (period %u)%s\n", name, fmtname[d.fmt], d.bits,
                d.chans == 2 ? "stereo" : "mono", d.rate, hi.rate, hi.period,
                d.chans == 2 && d.mono ? ", mixed to mono" : "");
    static signed char buf[CHUNK * 2];
    int ret = 0, again = 1;
    for (;;) {
        int k = sdec_read(&d, buf, CHUNK);
        if (k < 0) { dprintf(2, "play: %s: %s\n", name, d.why); ret = 1; break; }
        if (!k) break;
        int r = snd_write(h, buf, (unsigned)(k * st));
        if (r == -ECONNRESET && again) {        /* the server ended (ZINE, a crash): a new stream, a gap */
            again = 0;
            h = snd_open(d.outrate, d.mono ? 0 : SNDF_STEREO, (unsigned)v, (unsigned)ms);
            if (h >= 0) r = snd_write(h, buf, (unsigned)(k * st));
            else r = h;
        }
        if (r < 0) { dprintf(2, "play: %s\n", snd_why(r)); ret = 2; break; }
        again = 1;
    }
    if (h >= 0) {
        if (!ret) snd_drain(h);
        snd_close(h);
    }
    sdec_close(&d);
    return ret;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-v") && i + 1 < argc) { vol = atoi(argv[++i]); if (vol < 0 || vol > 64) usage(); }
        else if (!strcmp(a, "-b") && i + 1 < argc) { ms = atoi(argv[++i]); if (ms < 20 || ms > 1000) usage(); }
        else if (!strcmp(a, "-R") && i + 1 < argc) { rawrate = (unsigned)atoi(argv[++i]); raw = 1; }
        else if (!strcmp(a, "-m")) mono = 1;
        else if (!strcmp(a, "-q")) quiet = 1;
        else if (!strcmp(a, "-r")) raw = 1;
        else if (!strcmp(a, "-u")) { rawu = 1; raw = 1; }
        else if (!strcmp(a, "-2")) { raw2 = 1; raw = 1; }
        else if (!strcmp(a, "-S")) state();
        else if (!strcmp(a, "--")) { i++; break; }
        else usage();
    }
    if (raw && (rawrate < 1000 || rawrate > 100000)) { dprintf(2, "play: -R: 1000 to 100000\n"); return 2; }
    int r = snd_info(&si, 0);
    if (r < 0) { dprintf(2, "play: %s\n", snd_why(r)); return 2; }
    int ret = 0;
    if (i >= argc)
        return play("-", 0);
    for (; i < argc; i++) {
        int fd = strcmp(argv[i], "-") ? sys_open(argv[i], O_RDONLY) : 0;
        if (fd < 0) { dprintf(2, "play: %s: %s\n", argv[i], zstrerror(fd) ? zstrerror(fd) : "cannot open"); ret = 1; continue; }
        int k = play(argv[i], fd);
        if (fd) sys_close(fd);
        if (k == 2) return 2;
        if (k) ret = 1;
    }
    return ret;
}
