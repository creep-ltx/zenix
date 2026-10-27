/* SPDX-License-Identifier: GPL-2.0-only */
/* telnet's stream filter (DESIGN D20 4.4): rewrites of the stream between the decoder and the console, tracking only
 * whether it is inside an escape sequence or a music string; it never models the screen.
 * Both BBS modes: ANSI music swallowed as SyncTERM does (CSI |, CSI N, CSI M in the "all" mode, CSI = n M chooses;
 * the string ends at SO or at the first byte outside the music set, at most 1,024 bytes), FF -> ESC[2J ESC[H, SUB
 * dropped, CSI ? 33 h/l -> the iCE event, SyncTERM's font switch CSI Ps1 ; Ps2 SP D -> the mode event. The Amiga mode
 * also: 0x9B is CSI; CSI n u (with a number) dropped; CSI [0] SP p -> CSI ? 25 l/h; CSI [0] SP q answered with the
 * window bounds. The PC mode without the console's font: CP437 -> Latin-1 (3.5). The Unix mode: UTF-8 -> Latin-1. */
#include "telnet.h"

#define ESC 0x1B
#define F_GROUND 0
#define F_ESC    1
#define F_CSI    2
#define F_PASS   3                      /* an overlong sequence: as it is, to its final byte */
#define F_MUSIC  4

void tf_flush(struct tf *f)
{
    if (f->obn) {
        f->put(f->u, f->ob, f->obn);
        f->obn = 0;
    }
}

static void out1(struct tf *f, unsigned char c)
{
    if (f->obn == (int)sizeof f->ob)
        tf_flush(f);
    f->ob[f->obn++] = c;
}

static void outs(struct tf *f, const char *s)
{
    while (*s)
        out1(f, (unsigned char)*s++);
}

static void outn(struct tf *f, const unsigned char *b, int n)
{
    int i;
    for (i = 0; i < n; i++)
        out1(f, b[i]);
}

static void event(struct tf *f, int ev, int arg)
{
    tf_flush(f);                        /* what came before is drawn before the change */
    if (f->event)
        f->event(f->u, ev, arg);
}

void tf_init(struct tf *f, int mode)
{
    f->mode = mode;
    f->st = F_GROUND;
    f->musicmode = 1;                   /* BANSI, SyncTERM's default */
    f->musicn = f->seqn = f->need = f->obn = 0;
    f->ucs = 0;
}

void tf_setmode(struct tf *f, int mode)
{
    if (f->st == F_CSI)
        outn(f, f->seq, f->seqn);       /* a sequence cut by the switch goes as it was */
    f->st = F_GROUND;
    f->seqn = f->need = 0;
    f->mode = mode;
}

static int music_byte(unsigned char c)
{
    static const char set[] = "aAbBcCdDeEfFgGlLmMnNoOpPsStT.-+#<> ";
    const char *s;
    if (c >= '0' && c <= '9')
        return 1;
    for (s = set; *s; s++)
        if ((unsigned char)*s == c)
            return 1;
    return 0;
}

/* a byte of text, or a control, in a BBS mode */
static void ground(struct tf *f, unsigned char c)
{
    if (c == 0x0C) {                    /* FF: the BBS clear screen */
        outs(f, "\x1b[2J\x1b[H");
        return;
    }
    if (c == 0x1A)                      /* SUB: DOS's end of file */
        return;
    out1(f, f->mode == TN_PC && f->fallback ? tn_cp437_latin1[c] : c);
}

static void unix_byte(struct tf *f, unsigned char c)
{
    if (c < 0x80) {
        if (f->need) {
            out1(f, '?');               /* a sequence cut short */
            f->need = 0;
        }
        out1(f, c);
        return;
    }
    if (c < 0xC0) {
        if (!f->need) {
            out1(f, '?');               /* a continuation byte alone */
            return;
        }
        f->ucs = (f->ucs << 6) | (c & 0x3F);
        if (--f->need == 0)
            out1(f, f->ucs >= 0xA0 && f->ucs <= 0xFF ? (unsigned char)f->ucs : '?');
        return;
    }
    if (f->need)
        out1(f, '?');
    if (c >= 0xC2 && c <= 0xDF) {
        f->ucs = c & 0x1F;
        f->need = 1;
    } else if (c >= 0xE0 && c <= 0xEF) {
        f->ucs = c & 0x0F;              /* three bytes: never Latin-1 (overlong or above 0xFF): '?' at the end */
        f->need = 2;
    } else if (c >= 0xF0 && c <= 0xF4) {
        f->ucs = c & 0x07;
        f->need = 3;
    } else {
        f->need = 0;                    /* C0, C1 (overlong) and F5-FF are never UTF-8 */
        out1(f, '?');
    }
}

/* the parameters of the sequence in seq: the private marker, the numbers (-1: empty), the one intermediate */
struct csi {
    int priv, inter, final, np, p[8];
};

static void parse(const struct tf *f, struct csi *c)
{
    int i = 2, have = 0;
    c->priv = c->inter = 0;
    c->np = 0;
    c->final = f->seq[f->seqn - 1];
    if (i < f->seqn - 1 && f->seq[i] >= 0x3C && f->seq[i] <= 0x3F)
        c->priv = f->seq[i++];
    c->p[0] = -1;
    for (; i < f->seqn - 1; i++) {
        unsigned char b = f->seq[i];
        if (b >= '0' && b <= '9') {
            if (c->np < 8) {
                if (c->p[c->np] < 0)
                    c->p[c->np] = 0;
                if (c->p[c->np] < 100000)
                    c->p[c->np] = c->p[c->np] * 10 + (b - '0');
            }
            have = 1;
        } else if (b == ';') {
            if (c->np < 8)
                c->np++;
            if (c->np < 8)
                c->p[c->np] = -1;
            have = 1;
        } else if (b >= 0x20 && b <= 0x2F)
            c->inter = c->inter ? -1 : b;   /* -1: more than one */
        else
            c->inter = -1;              /* a parameter byte out of place: nothing of ours */
    }
    if (have && c->np < 8)
        c->np++;
}

/* v (0-65535) in decimal at d: the digits' count */
static int fmt_num(unsigned char *d, int v)
{
    unsigned char t[6];
    int k = 0, n = 0;
    if (v < 0)
        v = 0;
    do {
        t[k++] = (unsigned char)('0' + v % 10);
        v /= 10;
    } while (v && k < 5);
    while (k)
        d[n++] = t[--k];
    return n;
}

/* a whole sequence (ESC [ ... final) is in seq: act on it, or pass it on */
static void dispatch(struct tf *f)
{
    struct csi c;
    int i, p0;
    parse(f, &c);
    p0 = c.np > 0 ? c.p[0] : -1;
    f->st = F_GROUND;
    if (!c.priv && !c.inter) {
        if (c.final == '|' || (c.final == 'N' && f->musicmode >= 1) ||
            (c.final == 'M' && f->seqn == 3 && f->musicmode == 2)) {
            f->st = F_MUSIC;            /* swallowed with its string */
            f->musicn = 0;
            return;
        }
        if (c.final == 'u' && f->mode == TN_AMIGA && c.np > 0 && p0 >= 0)
            return;                     /* the Amiga's line length, not "restore the cursor" */
    }
    if (c.priv == '=' && !c.inter && c.final == 'M') {
        if (p0 <= 0)
            f->musicmode = 0;
        else if (p0 <= 2)
            f->musicmode = p0;
        return;
    }
    if (c.priv == '?' && !c.inter && (c.final == 'h' || c.final == 'l')) {
        int ice = 0, other = 0;
        for (i = 0; i < c.np; i++) {
            if (c.p[i] == 33)
                ice = 1;
            else
                other = 1;
        }
        if (ice) {
            event(f, TF_EV_ICE, c.final == 'h');
            if (!other)
                return;                 /* the console has nothing to do with it */
        }
    }
    if (!c.priv && c.inter == ' ' && c.final == 'D') {
        /* SyncTERM's font switch: only the main font (Ps1 0); 0 the PC, 37-42 the Amiga fonts (in the ROM's Topaz) */
        int p1 = c.np > 0 && c.p[0] > 0 ? c.p[0] : 0, p2 = c.np > 1 && c.p[1] > 0 ? c.p[1] : 0;
        if (p1 == 0 && (p2 == 0 || (p2 >= 37 && p2 <= 42))) {
            int m = p2 == 0 ? TN_PC : TN_AMIGA;
            if (m != f->mode) {
                tf_flush(f);
                f->mode = m;
                event(f, TF_EV_MODE, m);
            }
        }
        return;
    }
    if (f->mode == TN_AMIGA && !c.priv && c.inter == ' ') {
        if (c.final == 'p') {           /* the Amiga's cursor off (0) / on */
            outs(f, p0 == 0 ? "\x1b[?25l" : "\x1b[?25h");
            return;
        }
        if (c.final == 'q' && p0 <= 0) {/* the window bounds request: answered as the Amiga console does */
            unsigned char a[32];
            int k = 0;
            a[k++] = 0x9B;
            a[k++] = '1';
            a[k++] = ';';
            a[k++] = '1';
            a[k++] = ';';
            k += fmt_num(a + k, f->rows);
            a[k++] = ';';
            k += fmt_num(a + k, f->cols);
            a[k++] = ' ';
            a[k++] = 'r';
            f->answer(f->u, a, k);
            return;
        }
    }
    outn(f, f->seq, f->seqn);           /* not ours: as it came (ESC [ for an Amiga 0x9B) */
}

void tf_write(struct tf *f, const unsigned char *b, long n)
{
    long i;
    for (i = 0; i < n; i++) {
        unsigned char c = b[i];
        if (f->mode == TN_UNIX) {
            unix_byte(f, c);
            continue;
        }
again:
        switch (f->st) {
        case F_MUSIC:
            if (c == 0x0E) {            /* SO ends it */
                f->st = F_GROUND;
                break;
            }
            if (music_byte(c) && f->musicn < TF_MUSICMAX) {
                f->musicn++;
                break;
            }
            f->st = F_GROUND;           /* the first byte outside: ordinary output */
            goto again;
        case F_GROUND:
            if (c == ESC) {
                f->st = F_ESC;
                break;
            }
            if (c == 0x9B && f->mode == TN_AMIGA) {
                f->seq[0] = ESC;
                f->seq[1] = '[';
                f->seqn = 2;
                f->st = F_CSI;
                break;
            }
            ground(f, c);
            break;
        case F_ESC:
            if (c == '[') {
                f->seq[0] = ESC;
                f->seq[1] = '[';
                f->seqn = 2;
                f->st = F_CSI;
                break;
            }
            out1(f, ESC);               /* another escape: the console's; its next bytes as they come */
            f->st = F_GROUND;
            goto again;
        case F_CSI:
            if (c >= 0x20 && c <= 0x7E) {
                f->seq[f->seqn++] = c;
                if (c >= 0x40) {
                    dispatch(f);
                    break;
                }
                if (f->seqn == TF_SEQMAX) {
                    outn(f, f->seq, f->seqn);   /* too long to be ours: as it is */
                    f->seqn = 0;
                    f->st = F_PASS;
                }
                break;
            }
            if (c == 0x18 || c == 0x1A) {   /* CAN, SUB: the sequence is cancelled (ECMA-48): neither is drawn */
                f->seqn = 0;
                f->st = F_GROUND;
                break;
            }
            if (c == ESC) {
                outn(f, f->seq, f->seqn);   /* cut by another: the console cancels it */
                f->seqn = 0;
                f->st = F_ESC;
                break;
            }
            if (c < 0x20) {
                ground(f, c);           /* a control inside runs now (ECMA-48), before the sequence */
                break;
            }
            if (c == 0x7F)
                break;                  /* DEL inside: ignored */
            outn(f, f->seq, f->seqn);   /* an 8-bit byte inside: not a sequence after all */
            f->seqn = 0;
            f->st = F_GROUND;
            goto again;
        case F_PASS:
            if (c == ESC) {
                f->st = F_ESC;          /* the next one begins */
                break;
            }
            out1(f, c);
            if ((c >= 0x40 && c <= 0x7E) || c == 0x18 || c == 0x1A)
                f->st = F_GROUND;
            break;
        }
    }
    tf_flush(f);
}
