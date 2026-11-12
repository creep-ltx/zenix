/* SPDX-License-Identifier: GPL-2.0-only */
/* modplay: a ProTracker module on Paula's four channels through the sound server (DESIGN D40 sections 8 and 10,
 * docs/man/modplay.txt). The replayer (user/modrep.c) computes each tick's registers here; the server applies them at
 * their time from its queue, and its answer says which tick is being heard, which the status line shows. */
#include "libz.h"
#include "sndlib.h"
#include "modrep.h"

#define BATCH 4                 /* frames a call: the keys are looked at between calls (at most a queue, 320 ms) */
#define HIST 64                 /* what the status line needs of the frames sent and not yet heard */

static struct mod m;
static struct { unsigned char pos, row, pat, speed, bpm; short per[4]; unsigned char smp[4]; } hist[HIST];
static int vol = 64, loop, quiet, h = -1;
static unsigned eclock;
static unsigned char *file;
static unsigned fsize;

static void usage(void)
{
    dprintf(2, "usage: modplay [-v VOL] [-l] [-q] FILE\n");
    sys_exit(2);
}

static void restore(void) { sys_ttymode(0); }

static int load(const char *path)
{
    struct ustat st;
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0 || sys_fstat(fd, &st) < 0) {
        dprintf(2, "modplay: %s: %s\n", path, zstrerror(fd) ? zstrerror(fd) : "cannot open");
        return -1;
    }
    if (st.size < 600 || st.size > (8u << 20)) { dprintf(2, "modplay: %s: not a module (size)\n", path); return -1; }
    file = malloc(st.size + 1);         /* + 1: an odd bank is sent rounded up to even (start), its last byte 0 */
    if (!file) { dprintf(2, "modplay: %s: no memory for %u bytes\n", path, st.size); return -1; }
    unsigned n = 0;
    while (n < st.size) {
        int r = sys_read(fd, file + n, st.size - n);
        if (r <= 0) break;
        n += (unsigned)r;
    }
    sys_close(fd);
    file[n] = 0;
    fsize = n;
    return 0;
}

static int start(void)
{
    h = snd_open(0, SNDF_FRAMES, (unsigned)vol, 0);
    if (h < 0) return h;
    unsigned off = mod_bankoff(&m);
    return snd_bank(h, file + off, (fsize - off + 1) & ~1u);
}

/* the next tick into f, its line into hist[k] */
static void make(struct sndframe *f, unsigned k)
{
    unsigned bpmticks = mod_ticklen(&m, eclock);       /* the tempo of this tick: an F change counts from the next */
    hist[k % HIST].pos = (unsigned char)m.pos;
    hist[k % HIST].row = (unsigned char)m.row;
    hist[k % HIST].pat = m.order[m.pos];
    mod_tick(&m);
    hist[k % HIST].speed = (unsigned char)m.speed;
    hist[k % HIST].bpm = (unsigned char)m.bpm;
    memset(f, 0, sizeof *f);
    f->ticks = bpmticks;
    for (int c = 0; c < 4; c++) {
        const struct modout *o = &m.out[c];
        struct sndch *x = &f->ch[c];
        x->flags = SNDC_VOL;
        x->volume = (unsigned short)o->vol;
        if (o->period >= SND_FMINPER && o->period <= SND_FMAXPER) {
            x->flags |= SNDC_PER;
            x->period = (unsigned short)o->period;
        }
        if (o->flags & (MOD_TRIG | MOD_LOOP)) {
            x->loop = o->looplen >= 2 ? o->loop : SND_SILENT;
            x->looplen = o->looplen >= 2 ? o->looplen : 2;
        }
        if (o->flags & MOD_TRIG) {
            x->flags |= SNDC_TRIG;
            x->start = o->len >= 2 ? o->start : SND_SILENT;
            x->len = o->len >= 2 ? (o->len > 131070 ? 131070 : o->len) : 2;
            if (x->looplen > 131070) x->looplen = 131070;
        } else if (o->flags & MOD_LOOP)
            x->flags |= SNDC_LOOP;
        hist[k % HIST].per[c] = (short)(o->flags & MOD_TRIG ? o->period : -1);
        hist[k % HIST].smp[c] = (unsigned char)m.c[c].smp;
    }
}

static char notes[4][4];
static unsigned char nsmp[4];

static void show(unsigned played)
{
    static int lastrow = -1, lastpos = -1;
    if (quiet || !played) return;
    unsigned k = (played - 1) % HIST;
    for (int c = 0; c < 4; c++)
        if (hist[k].per[c] >= 0) { mod_notename(hist[k].per[c], notes[c]); nsmp[c] = hist[k].smp[c]; }
    if (hist[k].row == lastrow && hist[k].pos == lastpos) return;
    lastrow = hist[k].row;
    lastpos = hist[k].pos;
    char line[100];
    int n = snprintf(line, sizeof line, "\rpos %03d/%03d pat %02d row %02d spd %2d bpm %3d ", hist[k].pos, m.songlen,
                     hist[k].pat, hist[k].row, hist[k].speed, hist[k].bpm);
    for (int c = 0; c < 4; c++)
        n += snprintf(line + n, sizeof line - (u32)n, "|%s %02d", notes[c][0] ? notes[c] : "---", nsmp[c]);
    sys_write(1, line, (u32)n);
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-v") && i + 1 < argc) { vol = atoi(argv[++i]); if (vol < 0 || vol > 64) usage(); }
        else if (!strcmp(argv[i], "-l")) loop = 1;
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else usage();
    }
    if (i + 1 != argc) usage();
    if (load(argv[i]) < 0) return 1;
    char why[64];
    if (mod_load(&m, file, fsize, why) < 0) { dprintf(2, "modplay: %s: %s\n", argv[i], why); return 1; }
    if (fsize - mod_bankoff(&m) > SND_BANKMAX) {     /* (audit 2026-11-12 A6-5: the server's EINVAL said nothing) */
        dprintf(2, "modplay: %s: its samples are over 1 MB (the sound server's bank)\n", argv[i]);
        return 1;
    }
    struct sndinfo si;
    int r = snd_info(&si, 0);
    if (r < 0) { dprintf(2, "modplay: %s\n", snd_why(r)); return 2; }
    eclock = si.eclock;
    r = start();
    if (r < 0) { dprintf(2, "modplay: %s\n", snd_why(r)); return 2; }
    if (!quiet)
        dprintf(1, "%s: \"%s\", %d samples, %d positions, %d patterns (%s)\n", argv[i], m.title, m.nsmp, m.songlen,
                m.npat, si.flags & SNDI_NTSC ? "NTSC" : "PAL");
    int tty = !quiet && sys_ttymode(TTY_RAW | TTY_NOSIG) >= 0;    /* -q: no keys, the terminal left alone */
    static struct sndframe f[BATCH];
    unsigned sent = 0, played = 0;
    int paused = 0, ret = 0, again = 1, quit = 0;
    for (;;) {
        struct pollfd pf = { 0, POLLIN, 0 };
        if (tty && poll(&pf, 1, paused ? -1 : 0) > 0 && (pf.revents & POLLIN)) {
            char c;
            if (sys_read(0, &c, 1) == 1) {
                if (c == 'q' || c == 3 || c == 27) { quit = 1; break; }
                if (c == ' ') { paused ^= 1; snd_pause(h, paused); if (paused) continue; }
                if (c == 'n' || c == 'p') { mod_goto(&m, m.pos + (c == 'n' ? 1 : -1)); }
            }
        }
        if (paused) continue;
        if (m.ended && !loop) break;
        int k = 0;
        for (; k < BATCH && !(m.ended && !loop); k++) make(&f[k], sent + (unsigned)k);
        if (m.stop) m.ended = 1;
        if (m.ended && loop && !m.stop) m.ended = 0;
        if (!k) break;
        r = snd_frames(h, f, (unsigned)k);
        if (r == -ECONNRESET && again) {        /* the server ended (ZINE, a crash): open again, the bank again */
            again = 0;
            r = start();
            if (r >= 0) r = snd_frames(h, f, (unsigned)k);
        }
        if (r < 0) { dprintf(2, "\nmodplay: %s\n", snd_why(r)); ret = 2; break; }
        again = 1;
        sent += (unsigned)k;
        played = (unsigned)r;
        show(played);
    }
    if (!ret && !quit) snd_drain(h);
    snd_close(h);
    if (tty) restore();
    if (!quiet) sys_write(1, "\n", 1);
    return ret;
}
