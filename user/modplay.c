/* SPDX-License-Identifier: GPL-2.0-only */
/* modplay: the tracker music player (DESIGN D56, docs/d56-tracker-player.md; docs/man/modplay.txt). Plays MOD (4 to 32
 * channels), MED/OctaMED (MMD0-MMD3), Scream Tracker 3 and FastTracker 2 modules, a list of files or a folder, through
 * the sound server (D40). A module Paula can play as it is (a 4-channel MOD or MED) goes to the server as frames, one
 * a tick, on all four channels; the others are mixed here into one 8-bit stereo stream (user/trkmix.c). On a terminal
 * it shows a screen of its own: the song, the pattern being heard or VU meters, the instruments, the list. Every key
 * can be changed in ~/.config/modplay/keys. */
#include "libz.h"
#include "sndlib.h"
#include "trk.h"
#include "trkmix.h"
#include "../display/keys.h"

#define BATCH 4                 /* frames a call (native): the keys are looked at between calls */
#define HIST 64                 /* what the screen needs of the ticks made and not yet heard (a queue of 16, a ring
                                 * of 0.2 s: at most about 30) */
#define MAXFILE (8u << 20)

/* ---- the options and the list ---- */
static int vol = 64, repeat, quiet, line, shuffle, rate, optrate, bench;
static char **list;
static int nlist, cur;
static unsigned char *order;    /* the play order of the list (shuffled or not) */

/* ---- the song ---- */
static struct trk t;
static struct mix mx;
static unsigned char *file;
static unsigned fsize;
static int h = -1, mixing;
static unsigned eclock, clock;
static struct sndinfo si;

/* what the screen shows of a tick, kept until it is heard */
struct hist { unsigned short pos; unsigned char speed, lvl[TRK_MAXCH]; unsigned char tempo; short row, pat; unsigned us; };   /* us: the tick's length */
static struct hist hist[HIST];
static unsigned made, heard;    /* ticks made and heard (counted from the song's start) */
static unsigned long long playms;       /* the time heard, in microseconds */
static unsigned char level[TRK_MAXCH];

/* ---- the screen ---- */
static int tty, keys, view, page = -1, chofs, sel, msgticks;
/* the screen: libz's one layer (user/zscr.c: the buffered output, the size and SIGWINCH): zscr_rows, zscr_cols */
static char msg[80];

enum { V_PATTERN, V_INSTR, V_LIST, V_HELP, V_N };

/* ---- the keys ---- */
enum { A_NONE, A_QUIT, A_PAUSE, A_NEXT, A_PREV, A_FWD, A_BACK, A_VOLUP, A_VOLDN, A_REPEAT, A_SHUFFLE, A_VIEW,
       A_PATTERN, A_INSTR, A_LIST, A_HELP, A_UP, A_DOWN, A_PLAY, A_CHLEFT, A_CHRIGHT, A_TOP, A_N };
static const char *const aname[A_N] = { "none", "quit", "pause", "next", "prev", "forward", "back", "volume_up",
    "volume_down", "repeat", "shuffle", "view", "pattern", "instruments", "list", "help", "up", "down", "play",
    "channels_left", "channels_right", "playing" };
static const char *const ahelp[A_N] = { "", "quit", "pause and go on", "the next song", "the previous song",
    "the next pattern (position)", "the previous pattern", "louder", "softer", "repeat: off, the list, the song",
    "shuffle the list", "the next view", "the pattern view", "the instruments", "the list", "these keys",
    "up in the list", "down in the list", "play the song chosen in the list", "the channels shown: left",
    "the channels shown: right", "the list: back to the song playing" };
static const char defkeys[] =
    "# modplay's keys (docs/man/modplay.txt): KEY ACTION, a key a line; <Name> for <Up> <Down> <Left> <Right> <Return>\n"
    "# <Esc> <Tab> <Space> <BS> <Del> <F1>..<F10> <C-x>; \"KEY none\" takes a key away. Ctrl-C always quits.\n"
    "q quit\n<Esc> quit\n<Space> pause\nn next\np prev\n. forward\n<Right> forward\n, back\n<Left> back\n"
    "+ volume_up\n= volume_up\n- volume_down\nr repeat\ns shuffle\n<Tab> view\n1 pattern\n2 instruments\n3 list\n"
    "? help\n<F1> help\n<Help> help\n<Up> up\nk up\n<Down> down\nj down\n<Return> play\n[ channels_left\n] channels_right\n"
    "g playing\n";
static struct kbind kb[64];
static struct keyset ks = { aname, A_N, kb, 0, 64, "" };

static const char USAGE[] = "usage: modplay [-v VOL] [-l] [-s] [-q] [-z] [-r RATE] FILE|FOLDER...\n       modplay -K\n";
static void usage(void)
{
    zusage(USAGE);
}

static void keys_init(void) { keys_load(&ks, defkeys, sizeof defkeys - 1, "modplay"); }

/* a key from the terminal: a character, or the shared parser's code for an escape sequence (display/keys.h) */
static int getb(int ms)                 /* a byte of an escape sequence, or -1 when none comes in time */
{
    unsigned char c;
    struct pollfd pf = { 0, POLLIN, 0 };
    if (poll(&pf, 1, ms) <= 0 || sys_read(0, &c, 1) != 1) return -1;
    return c;
}

static int read_key(void)
{
    unsigned char c;
    if (sys_read(0, &c, 1) != 1) return -1;
    if (c == 10) return 13;
    if (c != 27) return c;
    int s1 = getb(30), ch = 0;
    if (s1 < 0) return 27;                              /* Esc alone */
    if (s1 == 'O') { ch = getb(30); return ch >= 'P' && ch <= 'S' ? KM_F1 + ch - 'P' : 0; }
    if (s1 != '[') return 0;
    int num = 0;
    while ((ch = getb(30)) >= 0 && ((ch >= '0' && ch <= '9') || ch == ';')) if (ch != ';') num = num * 10 + ch - '0';
    switch (ch) {
    case 'A': return KM_UP;
    case 'B': return KM_DOWN;
    case 'C': return KM_RIGHT;
    case 'D': return KM_LEFT;
    case 'P': case 'Q': case 'R': case 'S': return KM_F1 + ch - 'P';
    case '~': return num == 3 ? 127 : num >= 11 && num <= 15 ? KM_F1 + num - 11 : num >= 17 && num <= 21 ? KM_F1 + num - 12 : 0;
    }
    return 0;
}

static int action(int k)
{
    int k2 = 0;
    if (keys_prefix(&ks, k)) {                          /* a two-key binding: its second key within 2 s */
        struct pollfd pf = { 0, POLLIN, 0 };
        if (poll(&pf, 1, 2000) <= 0 || (k2 = read_key()) < 0) return A_NONE;
    }
    const struct kbind *b = keys_find(&ks, k, k2);
    return b ? b->act : A_NONE;
}

/* ---- output ---- */
#define flush oflush
static void put(const char *s, int n) { oputn(s, n < 0 ? (u32)strlen(s) : (u32)n); }
static void at(int r, const char *s)       /* a whole line at screen row r (1-based), cut to the width */
{
    zscr_at(r, 1);
    int w = 0, k = 0;
    while (s[k] && w < zscr_cols) {
        unsigned char c = (unsigned char)s[k];
        int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        for (int i = 1; i < len; i++) if (!s[k + i]) { len = i; break; }   /* a cut character: up to the end */
        if (c == 27) {                                  /* an attribute: no width */
            int e = k + 1;
            while (s[e] && !(s[e] >= 'A' && s[e] <= 'z' && s[e] != '[')) e++;
            len = s[e] ? e - k + 1 : e - k;
            w--;
        }
        k += len;
        w++;
    }
    put(s, k);
    put("\033[0m\033[K", -1);
}

/* ---- loading ---- */
static int load(const char *path, char *why)
{
    struct ustat st;
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0 || sys_fstat(fd, &st) < 0) {
        snprintf(why, 64, "%s", zstrerror(fd) ? zstrerror(fd) : "cannot open");
        if (fd >= 0) sys_close(fd);
        return -1;
    }
    if (st.size < 64 || st.size > MAXFILE) { sys_close(fd); snprintf(why, 64, st.size < 64 ? "not a module (size)" : "over 8 MB"); return -1; }
    file = malloc(st.size + 1);         /* + 1: an odd bank is sent rounded up to even, its last byte 0 */
    if (!file) { sys_close(fd); snprintf(why, 64, "no memory for %u bytes", st.size); return -1; }
    unsigned n = 0;
    while (n < st.size) {
        int r = sys_read(fd, file + n, st.size - n);
        if (r <= 0) break;
        n += (unsigned)r;
    }
    sys_close(fd);
    file[n] = 0;
    fsize = n;
    if (trk_load(&t, file, fsize, why) < 0) { free(file); file = 0; return -1; }
    return 0;
}

static void unload(void)
{
    if (h >= 0) snd_close(h);
    h = -1;
    if (mixing) mix_free(&mx);
    mixing = 0;
    trk_free(&t);
    free(file);
    file = 0;
}

static int open_stream(void)
{
    if (!mixing) {
        h = snd_open(0, SNDF_FRAMES, (unsigned)vol, 0);
        if (h < 0) return h;
        unsigned n = (t.banksize + 1) & ~1u;
        if (n > SND_BANKMAX) return -EFBIG;
        return snd_bank(h, n ? (const void *)t.bank : (const void *)file, n ? n : 2);
    }
    h = snd_open((unsigned)rate, SNDF_STEREO, (unsigned)vol, 100);
    return h < 0 ? h : 0;
}

static signed char pcm[2 * 8192];       /* the mixer's output, a tenth of a second (a tick of 0.25 s at most) */

/* ---- making ticks ---- */
static void remember(unsigned us)
{
    struct hist *x = &hist[made % HIST];
    x->pos = (unsigned char)t.pos;
    x->row = (short)t.row;
    x->pat = (short)t.pat;
    x->speed = (unsigned char)t.speed;
    x->tempo = (unsigned char)(t.tempo > 255 ? 255 : t.tempo);
    x->us = us;
    for (int c = 0; c < t.nch; c++) {
        const struct tvoice *v = &t.v[c];
        if (v->flags & TV_TRIG) level[c] = (unsigned char)v->vol;
        else if (v->flags & TV_CUT) level[c] = 0;
        else level[c] = (unsigned char)(level[c] > v->vol ? (level[c] * 7 + v->vol) / 8 : level[c]);
        x->lvl[c] = level[c];
    }
    made++;
}

static void frame(struct sndframe *f)
{
    unsigned len = trk_ticklen(&t, eclock), us = trk_ticklen(&t, 1000000);   /* this tick's: a change counts from the next */
    trk_tick(&t);
    memset(f, 0, sizeof *f);
    f->ticks = len;
    for (int c = 0; c < 4; c++) {
        const struct tvoice *v = &t.v[c];
        struct sndch *x = &f->ch[c];
        x->flags = SNDC_VOL;
        x->volume = (unsigned short)(c < t.nch ? v->vol : 0);
        if (c >= t.nch) continue;
        int per = v->per;
        while (per > 0 && per < SND_FMINPER) per *= 2;  /* above Paula's B-3: an octave lower (D56 3) */
        if (per >= SND_FMINPER && per <= SND_FMAXPER) { x->flags |= SNDC_PER; x->period = (unsigned short)per; }
        unsigned base = v->sp ? (unsigned)(v->sp - t.bank) : 0;
        if (v->flags & (TV_TRIG | TV_LOOP)) {
            x->loop = v->looplen >= 2 ? base + v->loop : SND_SILENT;
            x->looplen = v->looplen >= 2 ? (v->looplen > 131070 ? 131070 : v->looplen) : 2;
        }
        if (v->flags & TV_TRIG) {
            x->flags |= SNDC_TRIG;
            x->start = v->len >= 2 ? base + v->start : SND_SILENT;
            x->len = v->len >= 2 ? (v->len > 131070 ? 131070 : v->len & ~1u) : 2;
            if (x->start != SND_SILENT && (x->start & 1)) { x->start &= ~1u; }
        } else if (v->flags & TV_LOOP)
            x->flags |= SNDC_LOOP;
        if (v->flags & TV_CUT) { x->flags = SNDC_VOL; x->volume = 0; }
        if (x->loop != SND_SILENT && (x->loop & 1)) x->loop &= ~1u;
    }
    remember(us);
}

/* ---- the screen ---- */
static const char *const vu[9] = { " ", "\xE2\x96\x81", "\xE2\x96\x82", "\xE2\x96\x83", "\xE2\x96\x84",
                                   "\xE2\x96\x85", "\xE2\x96\x86", "\xE2\x96\x87", "\xE2\x96\x88" };

static const char *base(const char *p)
{
    const char *b = strrchr(p, '/');
    return b ? b + 1 : p;
}

static void draw_head(const struct hist *x)
{
    char b[256];
    unsigned s = (unsigned)(playms / 1000000);
    snprintf(b, sizeof b, "\033[1m %-40.40s\033[0m %s, %d ch %s  %d/%d", t.title[0] ? t.title : base(list[order[cur]]),
             t.kind, t.nch, mixing ? "mixed" : "Paula", cur + 1, nlist);
    at(1, b);
    snprintf(b, sizeof b, " pos %03d/%03d  pat %02X  row %02X  spd %2d  bpm %3d  vol %2d  %u:%02u  %s%s%s", x->pos,
             t.songlen, x->pat, x->row & 0xFF, x->speed, x->tempo, vol, s / 60, s % 60,
             repeat == 2 ? "[repeat song]" : repeat == 1 ? "[repeat list]" : "", shuffle ? " [shuffle]" : "",
             msgticks ? msg : "");
    at(2, b);
    /* the meters: two cells a channel (one when more than 39 fit not) */
    int n = 0, w = t.nch * 2 + 1 <= zscr_cols ? 2 : 1;
    n += snprintf(b + n, sizeof b - (u32)n, " ");
    for (int c = 0; c < t.nch && n < (int)sizeof b - 8; c++) {
        int l = x->lvl[c] * 8 / 64;
        for (int k = 0; k < w && n < (int)sizeof b - 8; k++) n += snprintf(b + n, sizeof b - (u32)n, "%s", vu[l]);
    }
    at(3, b);
}

static int cellw(void)
{
    int room = zscr_cols - 4, n = t.nch - chofs;
    if (n * 14 <= room) return 13;
    if (n * 7 <= room || room / 14 < 3) return room / 7 >= 3 ? 6 : 3;
    return 13;
}

static void draw_row(int r, int pat, int row, int hl)
{
    char b[512], cell[16];
    int n = snprintf(b, sizeof b, "%s%02X\033[0m", hl ? "\033[7m" : "", row & 0xFF);
    int w = cellw();
    for (int c = chofs; c < t.nch && n + w + 2 < zscr_cols + 12; c++) {
        t.ops->cell(&t, pat, row, c, cell);
        cell[w] = 0;
        n += snprintf(b + n, sizeof b - (u32)n, "%s\xE2\x94\x82%s", hl ? "\033[7m" : "", cell);
    }
    at(r, b);
}

static void draw_pattern(const struct hist *x, int force)
{
    int top = 5, hgt = zscr_rows - top, prow = t.ops->rows(&t, x->pat);
    if (hgt < 1) return;
    int pg = x->row / hgt;
    static int lastrow = -1, lastpat = -1;
    if (force || pg != page || x->pat != lastpat) {
        page = pg;
        for (int i = 0; i < hgt; i++) {
            int r = pg * hgt + i;
            if (r < prow) draw_row(top + i, x->pat, r, r == x->row);
            else at(top + i, "");
        }
    } else if (x->row != lastrow) {
        if (lastrow >= pg * hgt && lastrow < prow) draw_row(top + lastrow - pg * hgt, x->pat, lastrow, 0);
        draw_row(top + x->row - pg * hgt, x->pat, x->row, 1);
    }
    lastrow = x->row;
    lastpat = x->pat;
}

static void draw_instr(void)
{
    char b[128], nm[48];
    int top = 5, hgt = zscr_rows - top, half = (t.ninst + 1) / 2;
    int two = zscr_cols >= 80 && half <= hgt;
    for (int i = 0; i < hgt; i++) {
        int n = 0;
        b[0] = 0;
        for (int k = 0; k < (two ? 2 : 1); k++) {
            int j = two ? i + k * half : i;
            if (j < t.ninst) {
                t.ops->insname(&t, j, nm, 32);
                n += snprintf(b + n, sizeof b - (u32)n, " %02X %-34.34s", j + 1, nm);
            }
        }
        at(top + i, b);
    }
}

static void draw_list(void)
{
    char b[300];
    int top = 5, hgt = zscr_rows - top, first = sel - hgt / 2;
    if (first > nlist - hgt) first = nlist - hgt;
    if (first < 0) first = 0;
    for (int i = 0; i < hgt; i++) {
        int j = first + i;
        if (j < nlist)
            snprintf(b, sizeof b, "%s%c %s", j == sel ? "\033[7m" : "", order[cur] == j ? '>' : ' ', list[j]);
        else
            b[0] = 0;
        at(top + i, b);
    }
}

static void draw_help(void)
{
    char b[160];
    int top = 5, hgt = zscr_rows - top, r = 0;
    for (int a = 1; a < A_N && r < hgt; a++) {
        int n = snprintf(b, sizeof b, " %-24s", ahelp[a]), shown = 0;
        for (int i = 0; i < ks.n && shown < 3; i++) {
            if (ks.b[i].act != a) continue;
            char kn[12], k2[12] = "";
            keys_name(ks.b[i].k1, kn);
            if (ks.b[i].k2) keys_name(ks.b[i].k2, k2);
            n += snprintf(b + n, sizeof b - (u32)n, " %s%s", kn, k2);
            shown++;
        }
        at(top + r++, b);
    }
    while (r < hgt) at(top + r++, "");
}

static void draw(int force)
{
    if (!tty || !made) return;
    const struct hist *x = &hist[(heard ? heard - 1 : 0) % HIST];
    if (made - heard >= HIST) x = &hist[(made - HIST + 1) % HIST];
    draw_head(x);
    if (force) {
        static const char *const tabs[V_N] = { "1 pattern", "2 instruments", "3 list", "? keys" };
        char b[128];
        int n = 0;
        for (int i = 0; i < V_N; i++)
            n += snprintf(b + n, sizeof b - (u32)n, " %s%s\033[0m", i == view ? "\033[7m" : "", tabs[i]);
        at(4, b);
    }
    if (view == V_PATTERN) draw_pattern(x, force);
    else if (force) {
        if (view == V_INSTR) draw_instr();
        else if (view == V_LIST) draw_list();
        else draw_help();
    }
    flush();
}

static void say(const char *s)
{
    snprintf(msg, sizeof msg, "  %s", s);
    msgticks = 150;
}

/* ---- the one-line status (-s) ---- */
static void status(void)
{
    static int lastrow = -1, lastpos = -1;
    const struct hist *x = &hist[(heard ? heard - 1 : 0) % HIST];
    if (x->row == lastrow && x->pos == lastpos) return;
    lastrow = x->row;
    lastpos = x->pos;
    char b[120];
    int n = snprintf(b, sizeof b, "\rpos %03d/%03d pat %02d row %02d spd %2d bpm %3d ", x->pos, t.songlen, x->pat,
                     x->row, x->speed, x->tempo);
    for (int c = 0; c < t.nch && c < 8; c++) n += snprintf(b + n, sizeof b - (u32)n, "|%2d", x->lvl[c]);
    sys_write(1, b, (u32)n);
}

/* the mixing rate: -r's, else 16,000 Hz up to 8 channels and 11,025 Hz above (what the stock 68020 keeps up with,
 * D56 section 5) */
static int songrate(void) { return optrate ? optrate : t.nch <= 8 ? 16000 : 11025; }

/* ---- playing one song: -> what comes next: 1 next, -1 previous, 0 the list's end or quit, 2 a chosen one ---- */
static int quitting;
static const char *errmsg;      /* why the sound stopped (said when the screen is gone) */

static int play_song(void)
{
    made = heard = 0;
    playms = 0;
    memset(level, 0, sizeof level);
    page = -1;
    chofs = 0;
    mixing = !t.native;
    rate = songrate();
    if (mixing && mix_init(&mx, (unsigned)rate, clock, t.nch) < 0) { say("no memory for the mixer"); return 1; }
    int r = open_stream();
    if (r == -EFBIG) {                          /* Paula's bank holds 1 MB: a bigger module is mixed instead */
        snd_close(h);
        h = -1;
        mixing = 1;
        r = mix_init(&mx, (unsigned)rate, clock, t.nch) < 0 ? -ENOMEM : open_stream();
        if (r == -ENOMEM) mixing = 0;
    }
    if (r < 0) {
        errmsg = snd_why(r);
        return -2;
    }
    if (!quiet && !tty)
        dprintf(1, "%s: \"%s\", %s, %d channels, %d positions (%s, %s)\n", list[order[cur]], t.title, t.kind, t.nch,
                t.songlen, mixing ? "mixed" : "Paula", si.flags & SNDI_NTSC ? "NTSC" : "PAL");
    static struct sndframe f[BATCH];
    unsigned acc = 0, written = 0;          /* the mixer: the frames' remainder; the frames written */
    static unsigned atfr[HIST];             /* the mixer: each tick's first frame */
    int paused = 0, again = 1, next = 1;
    if (tty) { put("\033[2J", -1); draw(1); }
    for (;;) {
        struct pollfd pf = { 0, POLLIN, 0 };
        if (tty && zscr_resized()) {    /* a terminal window's new size: everything drawn anew */
            if (zscr_rows < 8) zscr_rows = 8;
            if (zscr_cols < 40) zscr_cols = 40;
            put("\033[2J", -1);
            draw(1);
        }
        while (keys && poll(&pf, 1, paused ? 200 : 0) > 0 && (pf.revents & POLLIN)) {
            int k = read_key();
            if (k < 0) break;
            if (k == 3) { quitting = 1; next = 0; goto out; }
            int a = action(k);
            switch (a) {
            case A_QUIT: quitting = 1; next = 0; goto out;
            case A_PAUSE:
                paused ^= 1;
                if (!mixing) snd_pause(h, paused);
                say(paused ? "paused" : "");
                break;
            case A_NEXT: next = 1; goto out;
            case A_PREV: next = -1; goto out;
            case A_FWD: case A_BACK: trk_goto(&t, t.pos + (a == A_FWD ? 1 : -1)); break;
            case A_VOLUP: case A_VOLDN:
                vol += a == A_VOLUP ? 4 : -4;
                vol = vol < 0 ? 0 : vol > 64 ? 64 : vol;
                snd_volume(h, (unsigned)vol);
                break;
            case A_REPEAT: repeat = (repeat + 1) % 3; break;
            case A_SHUFFLE: shuffle ^= 1; say(shuffle ? "shuffle on (from the next song)" : "shuffle off"); break;
            case A_VIEW: view = (view + 1) % V_N; draw(1); break;
            case A_PATTERN: view = V_PATTERN; draw(1); break;
            case A_INSTR: view = V_INSTR; draw(1); break;
            case A_LIST: view = V_LIST; sel = order[cur]; draw(1); break;
            case A_HELP: view = V_HELP; draw(1); break;
            case A_UP: case A_DOWN:
                if (view == V_LIST) { sel += a == A_UP ? -1 : 1; sel = sel < 0 ? 0 : sel >= nlist ? nlist - 1 : sel; draw(1); }
                break;
            case A_TOP: sel = order[cur]; if (view == V_LIST) draw(1); break;
            case A_PLAY:
                if (view == V_LIST) {
                    for (int i = 0; i < nlist; i++) if (order[i] == sel) cur = i;
                    next = 2;
                    goto out;
                }
                break;
            case A_CHLEFT: case A_CHRIGHT:
                chofs += a == A_CHRIGHT ? 1 : -1;
                chofs = chofs < 0 ? 0 : chofs >= t.nch ? t.nch - 1 : chofs;
                draw(1);
                break;
            }
            if (a != A_NONE) draw(0);
        }
        if (paused) { draw(0); continue; }
        if (t.ended && repeat != 2) break;
        if (t.stop) break;
        if (!mixing) {
            int k = 0;
            for (; k < BATCH && !(t.ended && repeat != 2) && !t.stop; k++) frame(&f[k]);
            if (t.ended && repeat == 2 && !t.stop) t.ended = 0;
            if (!k) break;
            r = snd_frames(h, f, (unsigned)k);
            if (r == -ECONNRESET && again) {        /* the server ended (ZINE, a crash): open again, the bank again */
                again = 0;
                r = open_stream();
                if (r >= 0) r = snd_frames(h, f, (unsigned)k);
            }
            if (r < 0) { errmsg = snd_why(r); next = -2; break; }
            again = 1;
            while (heard < (unsigned)r && heard < made) {
                const struct hist *x = &hist[heard % HIST];
                playms += x->us;
                heard++;
            }
            if ((unsigned)r > heard) heard = (unsigned)r > made ? made : (unsigned)r;
        } else {
            /* a tenth of a second at a time: ticks mixed into pcm, then written (the write waits for room) */
            int fr = 0;
            while (fr < rate / 10 && !(t.ended && repeat != 2) && !t.stop) {
                acc += (unsigned)rate * t.tnum;
                unsigned len = acc / (t.tden ? t.tden : 1);
                acc -= len * (t.tden ? t.tden : 1);
                if (len > 8192u - (unsigned)fr) len = 8192u - (unsigned)fr;
                unsigned us = trk_ticklen(&t, 1000000);
                trk_tick(&t);
                atfr[made % HIST] = written + (unsigned)fr;
                remember(us);
                mix_set(&mx, t.v);
                mix_render(&mx, pcm + 2 * fr, (int)len);
                fr += (int)len;
            }
            if (t.ended && repeat == 2 && !t.stop) t.ended = 0;
            if (!fr) break;
            r = snd_write(h, pcm, (unsigned)fr * 2);
            if (r == -ECONNRESET && again) { again = 0; r = open_stream(); if (r >= 0) r = snd_write(h, pcm, (unsigned)fr * 2); }
            if (r < 0) { errmsg = snd_why(r); next = -2; break; }
            again = 1;
            written += (unsigned)fr;
            /* heard: what was written less the ring the server holds (two halves of 100 ms, about one and a half
             * of them still to play) */
            unsigned lag = (unsigned)rate * 15 / 100, now = written > lag ? written - lag : 0;
            while (heard < made && (made - heard >= HIST || atfr[heard % HIST] <= now)) {
                const struct hist *x = &hist[heard % HIST];
                playms += x->us;
                heard++;
            }
        }
        if (msgticks) msgticks--;
        if (tty) draw(0);
        else if (line) status();
    }
    if (next == 1 && !quitting) snd_drain(h);
out:
    if (line && !tty) sys_write(1, "\n", 1);
    if (next == -2) return -2;
    return next;
}

/* -B SECONDS: mix that much of the first song without sound and say how long it took (the E-clock), for measuring
 * the mixer on a machine (D56 section 5) */
static int do_bench(const char *path)
{
    char why[64];
    if (load(path, why) < 0) { dprintf(2, "modplay: %s: %s\n", path, why); return 1; }
    rate = songrate();
    if (mix_init(&mx, (unsigned)rate, clock, t.nch) < 0) { dprintf(2, "modplay: no memory for the mixer\n"); return 1; }
    unsigned acc = 0, total = (unsigned)rate * (unsigned)bench, done = 0, ticks = 0, tmix = 0, ttick = 0;
    unsigned fnv = 2166136261u;             /* FNV-1a of the bytes: the PC's test makes the same (D56 section 7) */
    while (done < total) {
        acc += (unsigned)rate * t.tnum;
        unsigned len = acc / (t.tden ? t.tden : 1);
        acc -= len * (t.tden ? t.tden : 1);
        if (len > 8192) len = 8192;
        if (len > total - done) len = total - done;
        u32 e0 = sys_hrtime();
        trk_tick(&t);
        mix_set(&mx, t.v);
        u32 e1 = sys_hrtime();
        mix_render(&mx, pcm, (int)len);
        u32 e2 = sys_hrtime();
        ttick += e1 - e0;
        tmix += e2 - e1;
        for (unsigned i = 0; i < 2 * len; i++) fnv = (fnv ^ (unsigned char)pcm[i]) * 16777619u;
        done += len;
        ticks++;
    }
    unsigned all = ttick + tmix, real = (unsigned)((unsigned long long)eclock * (unsigned)bench);
    dprintf(1, "%s: %s, %d channels, %u Hz: %u s of sound in %u ms (%u%% of the time): replayer %u ms, mixer %u ms,"
            " %u ticks, fnv %08x\n", path, t.kind, t.nch, (unsigned)rate, (unsigned)bench,
            (unsigned)((unsigned long long)all * 1000 / eclock), (unsigned)((unsigned long long)all * 100 / real),
            (unsigned)((unsigned long long)ttick * 1000 / eclock), (unsigned)((unsigned long long)tmix * 1000 / eclock), ticks, fnv);
    unload();
    return 0;
}

static unsigned rnd = 12345;
static void shuffle_list(void)
{
    rnd ^= sys_hrtime();
    for (int i = nlist - 1; i > 0; i--) {
        rnd = rnd * 1103515245u + 12345u;
        int j = (int)((rnd >> 8) % (unsigned)(i + 1));
        unsigned char x = order[i]; order[i] = order[j]; order[j] = x;
    }
}

static int add(const char *p)
{
    char **nl = realloc(list, (u32)(nlist + 1) * sizeof *nl);
    if (!nl || nlist >= 255) return -1;
    list = nl;
    list[nlist] = strdup(p);
    if (!list[nlist]) return -1;
    nlist++;
    return 0;
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int add_arg(const char *p)
{
    struct ustat st;
    if (sys_stat(p, &st) == 0 && (st.mode & 0170000) == 0040000) {
        char **names;
        int n = dir_list(p, &names);
        if (n < 0) { dprintf(2, "modplay: %s: %s\n", p, zwhy("cannot read")); return 0; }
        qsort(names, (u32)n, sizeof *names, cmpstr);
        for (int i = 0; i < n; i++) {
            char *f = path_join(p, names[i]);
            struct ustat s2;
            if (f && sys_stat(f, &s2) == 0 && (s2.mode & 0170000) == 0100000 && add(f) < 0) {
                dprintf(2, "modplay: the list holds at most 255 songs\n");
                free(f);
                dir_free(names, n);
                return -1;
            }
            free(f);
        }
        dir_free(names, n);
        return 0;
    }
    if (add(p) < 0) { dprintf(2, "modplay: the list holds at most 255 songs\n"); return -1; }
    return 0;
}

int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int i = 1;
    for (; zisopt(argc, argv, &i, USAGE); i++) {
        if (!strcmp(argv[i], "-v") && i + 1 < argc) { vol = atoi(argv[++i]); if (vol < 0 || vol > 64) usage(); }
        else if (!strcmp(argv[i], "-r") && i + 1 < argc) { optrate = atoi(argv[++i]); if (optrate < 4000 || optrate > 28000) usage(); }
        else if (!strcmp(argv[i], "-l")) repeat = 2;
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-s")) line = 1;
        else if (!strcmp(argv[i], "-z")) shuffle = 1;
        else if (!strcmp(argv[i], "-B") && i + 1 < argc) { bench = atoi(argv[++i]); if (bench < 1 || bench > 600) usage(); }
        else if (!strcmp(argv[i], "-K")) { sys_write(1, defkeys, sizeof defkeys - 1); return 0; }
        else zbadopt(argv[i], USAGE);
    }
    if (i >= argc) usage();
    for (; i < argc; i++) if (add_arg(argv[i]) < 0) return 1;
    if (!nlist) { dprintf(2, "modplay: nothing to play\n"); return 1; }
    order = malloc((u32)nlist);
    if (!order) return 1;
    for (int k = 0; k < nlist; k++) order[k] = (unsigned char)k;
    if (shuffle) shuffle_list();
    keys_init();
    int r = snd_info(&si, 0);
    if (r < 0) { dprintf(2, "modplay: %s\n", snd_why(r)); return 2; }
    eclock = si.eclock;
    clock = si.clock;
    if (bench) return do_bench(list[0]);
    if (!quiet) {
        keys = sys_ttymode(TTY_RAW | TTY_NOSIG) >= 0;
        if (keys && !line) {
            tty = 1;
            zscr_init();                /* the size, and its changes followed (SIGWINCH) */
            if (zscr_rows < 8) zscr_rows = 8;
            if (zscr_cols < 40) zscr_cols = 40;
            put("\033[?25l\033[2J", -1);
            flush();
        }
    }
    int ret = 0, dir = 1;
    static char lasterr[160];
    for (int tries = 0; cur >= 0 && cur < nlist && !quitting;) {
        char why[64];
        const char *path = list[order[cur]];
        if (load(path, why) < 0) {
            if (tty) {
                snprintf(msg, sizeof msg, "  %s: %s", base(path), why);
                msgticks = 100;
                snprintf(lasterr, sizeof lasterr, "modplay: %s: %s\n", path, why);
            }
            else dprintf(2, "modplay: %s: %s\n", path, why);
            ret = 1;
            if (++tries >= nlist) break;
            cur += dir;
            if (cur >= nlist && repeat == 1) cur = 0;
            if (cur < 0) cur = 0;
            continue;
        }
        tries = 0;
        int nx = play_song();
        unload();
        if (nx == -2) { ret = 2; break; }
        if (quitting || nx == 0) break;
        if (nx == 2) continue;
        dir = nx;
        cur += nx;
        if (cur >= nlist) {
            if (repeat != 1) break;
            if (shuffle) shuffle_list();
            cur = 0;
        }
        if (cur < 0) cur = 0;
    }
    if (tty) { put("\033[?25h\033[2J\033[H", -1); flush(); }
    if (tty && lasterr[0]) dprintf(2, "%s", lasterr);
    if (errmsg) dprintf(2, "modplay: %s\n", errmsg);
    if (ks.bad[0] && !quiet) dprintf(2, "modplay: %s\n", ks.bad);
    if (keys) sys_ttymode(0);
    return ret;
}
