/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* term - Ahiru inside a terminal: a CTerm tab or pane, or any Amiga
 * console (CON:, KingCON, ...). No window or screen of its own.
 *
 * The drawing, the keys, the mouse and the size come from tui.library,
 * the terminal programs' shared toolkit (tui/ in the AmigaLibs repo,
 * 0.3 or newer): it keeps a grid and sends only what changed, so
 * term_flush() (and term_sig(), before every Wait) is when things show.
 * This file turns its events into Ahiru's (UiEv, KEY_*) and keeps
 * Ahiru's own Wait(): term_sig() is the library's signal, term_event()
 * takes what came without waiting. Without the library Ahiru opens on
 * its screen instead.
 *
 * Keys as before: Shift with the arrows is a page / the top / the end,
 * F1-F10, Help, Esc then the key for Alt+key; a paste comes as its
 * characters (controls left out). */
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/tui.h>
#include <string.h>
#include "ahiru.h"

#define ID_CTRM  0x4354524DUL    /* CTerm's console says this to DISK_INFO */
#define TUINEEDS 3               /* tui.library 0.3: TuiSignal, TuiPoll, TC_PEN */

struct Library *TuiBase;
int termcterm;                   /* the console is CTerm's */
static struct Tui *t;
static int pend[256], pendn, pendh;     /* keys still to give: a paste, Alt+key */
static int picshown, piccol, picrow, picw, pich;    /* a sixel picture on screen */

/* the library's size as Ahiru's (too small a terminal: the old size) */
static void sync(void)
{
    if (t->Rows >= 5 && t->Cols >= 20) {
        nrows = t->Rows;
        ncols = t->Cols > MAXCOLS ? MAXCOLS : t->Cols;
    }
    if (t->CellH > 3 && t->CellW > 3) {
        ch = t->CellH;              /* a cell in pixels: picture previews */
        cw = t->CellW;
    }
}

/* drawing over the picture: its cells are sent again, as the old
 * immediate drawing overwrote it */
static void overpic(int col, int row, int w)
{
    if (!picshown || row < picrow || row >= picrow + pich || col >= piccol + picw || col + w <= piccol)
        return;
    TuiDirty(t, piccol, picrow, picw, pich);
    picshown = 0;
}

/* ---- output -------------------------------------------------------- */

void term_flush(void)
{
    TuiFlush(t);
}

/* pens: in CTerm the ANSI sixteen (Ahiru's 16-colour palette is in that
 * order); in another console the screen's own pens (TC_PEN, as picked in
 * ui.c). Pen 0 as a background is the terminal's own, so CTerm's theme
 * and background picture show through. */
void term_put(int col, int row, const char *s, int len, int fg, int bg)
{
    if (len <= 0) return;
    overpic(col, row, len);
    if (termcterm)
        TuiPut(t, col, row, (CONST_STRPTR)s, len, fg, bg ? bg : TC_DEFAULT, 0);
    else
        TuiPut(t, col, row, (CONST_STRPTR)s, len, TC_PEN(fg), bg ? TC_PEN(bg) : TC_DEFAULT, 0);
}

/* UTF-8 text (a UTF-8 file's row): CTerm shows every character, a plain
 * console the nearest; wide ones take two cells (tui.library 0.5) */
int term_pututf8(int col, int row, const char *s, int len, int fg, int bg)
{
    if (len <= 0) return 0;
    overpic(col, row, len);
    if (termcterm)
        return (int)TuiPutUTF8(t, col, row, (CONST_STRPTR)s, len, fg, bg ? bg : TC_DEFAULT, 0);
    return (int)TuiPutUTF8(t, col, row, (CONST_STRPTR)s, len, TC_PEN(fg), bg ? TC_PEN(bg) : TC_DEFAULT, 0);
}

/* the columns a character takes: tui.library 0.5's widths (2 for Chinese,
 * Japanese, emoji, 0 for an accent that joins the letter before); with an
 * older one only the common accents are known */
int term_cw(unsigned long c)
{
    LONG w;
    if (c < 0x300) return 1;
    if (!TuiBase || (TuiBase->lib_Version == 0 && TuiBase->lib_Revision < 5))
        return c < 0x370 ? 0 : 1;
    w = TuiCharWidth(c);
    return w < 0 ? 1 : (int)w;
}

void term_clear(int row, int col, int w)
{
    if (w <= 0) return;
    overpic(col, row, w);
    TuiFill(t, col, row, w, ' ', termcterm ? 7 : TC_PEN(1), TC_DEFAULT, 0);
}

void term_clearall(void)
{
    overpic(0, 0, MAXCOLS);
    if (picshown) {                 /* below row 0 */
        TuiDirty(t, piccol, picrow, picw, pich);
        picshown = 0;
    }
    TuiClear(t);
}

/* ---- input --------------------------------------------------------- */

static void pendadd(int k)
{
    if (pendn < (int)(sizeof(pend) / sizeof(pend[0]))) {
        pend[(pendh + pendn) % 256] = k;
        pendn++;
    }
}

/* a paste as keys: its characters (UTF-8 in CTerm, else Latin-1), no
 * controls - a newline in it would act */
static void pasted(const UBYTE *s, long n)
{
    long i = 0;
    while (i < n) {
        ULONG c = s[i++];
        if (termcterm && c >= 0xC2 && c <= 0xDF && i < n && (s[i] & 0xC0) == 0x80)
            c = ((c & 0x1F) << 6) | (s[i++] & 0x3F);
        else if (termcterm && c >= 0xE0) {     /* beyond Latin-1: left out */
            while (i < n && (s[i] & 0xC0) == 0x80) i++;
            continue;
        }
        if (c >= 32 && c != 0x7F && !(c >= 0x80 && c < 0xA0)) pendadd((int)c);
    }
}

/* a library key as Ahiru's (0: none of Ahiru's) */
static int key(int k)
{
    int b = K_BASE(k), sh = (k & TM_SHIFT) != 0;
    switch (b) {
    case K_UP: return sh ? KEY_PGUP : KEY_UP;
    case K_DOWN: return sh ? KEY_PGDN : KEY_DOWN;
    case K_LEFT: return sh ? KEY_TOP : KEY_LEFT;
    case K_RIGHT: return sh ? KEY_END : KEY_RIGHT;
    case K_HOME: return KEY_TOP;
    case K_END: return KEY_END;
    case K_PGUP: return KEY_PGUP;
    case K_PGDN: return KEY_PGDN;
    case K_HELP: return KEY_HELP;
    case K_ENTER: return 13;
    case K_BS: return 8;
    case K_DEL: return 127;
    case K_TAB: return sh ? 0 : 9;
    case K_ESC: return 27;
    case K_INS: return 0;
    }
    if (b >= K_F1 && b < K_F1 + 10) return KEY_F1 + (b - K_F1);
    if (b >= 0x100) return 0;           /* F11, F12, beyond Latin-1 */
    if (k & TM_CTRL) return (b - 'a' + 1) & 31;
    if (k & TM_ALT) {                   /* Esc, then the key */
        pendadd(b);
        return 27;
    }
    return b;
}

ULONG term_sig(void)
{
    if (!t) return 0;
    TuiFlush(t);                    /* what was drawn shows before the wait */
    return TuiSignal(t);
}

int term_event(UiEv *e)
{
    TuiEv te;
    for (;;) {
        memset(e, 0, sizeof(*e));
        if (pendn) {
            e->type = EV_KEY;
            e->key = pend[pendh];
            pendh = (pendh + 1) % 256;
            pendn--;
            CurrentTime(&e->secs, &e->mics);
            return 1;
        }
        if (!TuiPoll(t, &te)) {
            /* nothing to do: what is drawn shows now, before a long
             * preview or the wait (the old drawing went out as it came) */
            TuiFlush(t);
            return 0;
        }
        e->secs = te.secs;
        e->mics = te.mics;
        switch (te.type) {
        case TE_KEY:
            if (!(e->key = key(te.key))) continue;
            e->type = EV_KEY;
            return 1;
        case TE_MOUSE:
            if (te.mact == TB_PRESS) e->type = EV_CLICK;
            else if (te.mact == TB_WHEELUP || te.mact == TB_WHEELDOWN) {
                e->type = EV_WHEEL;
                e->key = te.mact == TB_WHEELUP ? -1 : 1;
            } else
                continue;
            e->col = te.col;
            e->row = te.row;
            return 1;
        case TE_PASTE:
            pasted(te.text, te.len);
            continue;
        case TE_RESIZE:
            TuiResized(t);
            sync();
            picshown = 0;
            e->type = EV_RESIZE;
            return 1;
        case TE_END:                /* the terminal has gone: Ahiru closes */
            e->type = EV_CLOSE;
            return 1;
        }
    }
}

int term_keywaiting(void)
{
    return pendn || TuiKeyWaiting(t);
}

/* ---- opening and closing -------------------------------------------- */

/* the console under Input()/Output(), or FALSE: no terminal here */
int term_possible(int *isctrm)
{
    struct FileHandle *fh;
    struct InfoData *id;
    BPTR in = Input(), o = Output();
    *isctrm = 0;
    if (!in || !o || !IsInteractive(in) || !IsInteractive(o)) return 0;
    fh = (struct FileHandle *)BADDR(in);
    if (!fh->fh_Type) return 0;     /* NIL: */
    if ((id = AllocVec(sizeof(*id), MEMF_PUBLIC | MEMF_CLEAR)) != NULL) {
        if (DoPkt(fh->fh_Type, ACTION_DISK_INFO, MKBADDR(id), 0, 0, 0, 0))
            *isctrm = id->id_DiskType == ID_CTRM;
        FreeVec(id);
    }
    return 1;
}

int term_open(void)
{
    if (!term_possible(&termcterm)) return 0;
    if (!TuiBase) {
        TuiBase = OpenLibrary("tui.library", 0);
        if (TuiBase && TuiBase->lib_Version == 0 && TuiBase->lib_Revision < TUINEEDS) {
            CloseLibrary(TuiBase);
            TuiBase = NULL;
        }
        if (!TuiBase) {
            static const char m[] = "Ahiru: inside the terminal needs tui.library 0.3 or newer in LIBS:\n";
            Write(Output(), (APTR)m, sizeof(m) - 1);
            return 0;
        }
    }
    if (!(t = TuiOpen(0))) {
        CloseLibrary(TuiBase);
        TuiBase = NULL;
        return 0;
    }
    termcterm = t->CTerm;
    pendn = pendh = picshown = 0;
    sync();
    TuiTitle(t, "Ahiru");
    dbg("term: tui.library open");
    return 1;
}

void term_close(void)
{
    if (!t) return;
    TuiClose(t);
    t = NULL;
    CloseLibrary(TuiBase);
    TuiBase = NULL;
}

/* the terminal to a command for a while (a Shell command, an editor),
 * and back: wait 1 asks for a key first, so its output can be read */
void term_suspend(void)
{
    TuiSuspend(t);
}

void term_resume(int wait)
{
    if (wait) TuiKeyAfter(t, (CONST_STRPTR)" Ahiru: a key goes back ");
    TuiResume(t);
    sync();
    picshown = 0;
}

/* ---- pictures: a thumbnail as sixel, for CTerm ---------------------- */

/* the sixel is made in a buffer of its own, then goes out at its cell
 * after everything drawn so far (TuiRaw) */
static UBYTE *sx;
static long sxlen, sxcap;

static void sxout(const char *s, int n)
{
    if (sxlen + n > sxcap) {
        long cap = (sxlen + n) * 2 + 4096;
        UBYTE *q = AllocVec(cap, MEMF_ANY);
        if (!q) return;
        if (sx) {
            CopyMem(sx, q, sxlen);
            FreeVec(sx);
        }
        sx = q;
        sxcap = cap;
    }
    CopyMem((APTR)s, sx + sxlen, n);
    sxlen += n;
}

static void sxs(const char *s)
{
    sxout(s, (int)strlen(s));
}

static void sxnum(int v)
{
    char b[12];
    int i = sizeof(b);
    if (v < 0) v = 0;
    do b[--i] = (char)('0' + v % 10); while ((v /= 10) && i);
    sxout(b + i, (int)sizeof(b) - i);
}

/* rgb (tw x th, 3 bytes a pixel) at a cell: the 216-colour cube with
 * a 4 x 4 ordered dither, six rows a band, runs packed (!n) */
void term_sixel(const unsigned char *rgb, int tw, int th, int col, int row)
{
    static const unsigned char bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    unsigned char *idx, used[216], *line;
    int x, y, i, c, band;
    char b[2];
    if (tw <= 0 || th <= 0 || !t) return;
    if (!(idx = AllocVec((ULONG)tw * th + tw, MEMF_ANY))) return;
    line = idx + (long)tw * th;
    sxlen = 0;
    for (y = 0; y < th; y++)
        for (x = 0; x < tw; x++) {
            const unsigned char *p = rgb + ((long)y * tw + x) * 3;
            int d = bayer[(y & 3) * 4 + (x & 3)] * 51 / 16;   /* 0..47: one step's worth */
            int r = (p[0] * 5 + d) / 255, g = (p[1] * 5 + d) / 255, bl = (p[2] * 5 + d) / 255;
            if (r > 5) r = 5;
            if (g > 5) g = 5;
            if (bl > 5) bl = 5;
            idx[(long)y * tw + x] = (unsigned char)(r * 36 + g * 6 + bl);
        }
    sxs("\x1bP0;1;0q\"1;1;");
    sxnum(tw);
    sxout(";", 1);
    sxnum(th);
    memset(used, 0, sizeof(used));
    for (i = 0; i < (long)tw * th; i++) used[idx[i]] = 1;
    for (c = 0; c < 216; c++)
        if (used[c]) {
            sxout("#", 1);
            sxnum(c);
            sxs(";2;");
            sxnum((c / 36) * 20);
            sxout(";", 1);
            sxnum((c / 6 % 6) * 20);
            sxout(";", 1);
            sxnum((c % 6) * 20);
        }
    for (band = 0; band < th; band += 6) {
        int rows = th - band < 6 ? th - band : 6, first = 1;
        memset(used, 0, sizeof(used));
        for (y = 0; y < rows; y++)
            for (x = 0; x < tw; x++) used[idx[(long)(band + y) * tw + x]] = 1;
        for (c = 0; c < 216; c++) {
            int run = 0, last = -1;
            if (!used[c]) continue;
            for (x = 0; x < tw; x++) {
                int bits = 0;
                for (y = 0; y < rows; y++)
                    if (idx[(long)(band + y) * tw + x] == c) bits |= 1 << y;
                line[x] = (unsigned char)(63 + bits);
            }
            if (!first) sxout("$", 1);
            first = 0;
            sxout("#", 1);
            sxnum(c);
            for (x = 0; x <= tw; x++) {
                int v = x < tw ? line[x] : -1;
                if (v == last) {
                    run++;
                    continue;
                }
                if (last >= 0) {
                    if (run > 3) {
                        sxout("!", 1);
                        sxnum(run);
                        b[0] = (char)last;
                        sxout(b, 1);
                    } else
                        while (run--) {
                            b[0] = (char)last;
                            sxout(b, 1);
                        }
                }
                last = v;
                run = 1;
            }
        }
        sxout("-", 1);
    }
    sxs("\x1b\\");
    FreeVec(idx);
    if (!sx) return;
    TuiFlush(t);                    /* the text first, then the picture over it */
    TuiRaw(t, col, row, (CONST_STRPTR)sx, sxlen);
    picshown = 1;
    piccol = col;
    picrow = row;
    picw = cw > 0 ? (tw + cw - 1) / cw : 1;
    pich = ch > 0 ? (th + ch - 1) / ch : 1;
    if (sxcap > 65536) {            /* a big one is not kept */
        FreeVec(sx);
        sx = NULL;
        sxcap = 0;
    }
}
