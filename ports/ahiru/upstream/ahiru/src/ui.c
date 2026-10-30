/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* ui - the screen, the cell grid, keys, prompts and the pager. */
#include <exec/memory.h>
#include <intuition/screens.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <devices/inputevent.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/diskfont.h>
#include <stdlib.h>
#include <string.h>
#include "ahiru.h"

struct Screen *scr;
struct Window *win;
struct RastPort *rp;
static struct TextFont *font;
static struct TextAttr tattr;
static char fontname[64];
static int x0, y0, baseline;
static APTR oldwinptr;          /* the requester window before ours */
int ownscr, colors = 8, cw = 8, ch = 8, ncols = 80, nrows = 25, listh = 23;
int c0x, c0w, c1x, c1w, c2x, c2w;
const int *rolepen, *ansipen;
unsigned char rowbuf[MAXCOLS], rowattr[MAXCOLS];
int msgup;
int breakseen;      /* Ctrl-C arrived: whatever is waiting backs out */
char cfgfont[64] = "topaz.font/8";
int cfgmode = UI_AUTO;          /* MODE in Ahiru.config, or SCREEN/WINDOW/TERMINAL given */
int uimode = UI_SCREEN;         /* what is open now */
int fromwb;                     /* started from Workbench (main.c) */
int winbox[4] = { -1, -1, 0, 0 };   /* the window: left, top, width, height */
static int gzz;                 /* the window draws inside its borders */
int uiresized;

/* ---- palettes ---------------------------------------------------
 * 16: the classic ANSI sixteen. 8: the ANSI eight, the blue lifted
 * so directories read on black. 4: black, grey, sky blue and amber -
 * Workbench 1.x in spirit. */
static UWORD pal16[16] = {
    0x000, 0xA00, 0x0A0, 0xA50, 0x00A, 0xA0A, 0x0AA, 0xAAA,
    0x555, 0xF55, 0x5F5, 0xFF5, 0x55F, 0xF5F, 0x5FF, 0xFFF };
static UWORD pal8[8] = {
    0x000, 0xE33, 0x3C3, 0xEE3, 0x56F, 0xD3D, 0x3DD, 0xDDD };
static UWORD pal4[4] = { 0x000, 0xCCC, 0x59F, 0xF90 };

/*                          TEXT HI DIR EXE ARC IMG AUD DIM ACC MRK ERR INF SRC ASN LNK SCR HID */
static int roles16[RO_COUNT] = { 7, 15, 12, 10, 9, 11, 13, 8, 14, 11, 9, 8, 6, 14, 14, 10, 8 };
static int roles8[RO_COUNT]  = { 7, 7, 4, 2, 1, 3, 5, 6, 6, 3, 1, 6, 6, 6, 6, 2, 6 };
static int roles4[RO_COUNT]  = { 1, 1, 2, 1, 3, 3, 3, 2, 2, 3, 3, 2, 2, 2, 2, 1, 2 };
/* a Workbench window: pens 0 grey, 1 black, 2 white, 3 blue */
static const int rolesWB[RO_COUNT] = { 1, 2, 3, 1, 1, 1, 1, 1, 3, 2, 1, 1, 1, 3, 3, 1, 2 };

/* ANSI colour 0-15 (bold = +8) to pen */
static const int ansi16[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
static const int ansi8[16]  = { 0, 1, 2, 3, 4, 5, 6, 7, 7, 1, 2, 3, 4, 5, 6, 7 };
static const int ansi4[16]  = { 0, 3, 2, 3, 2, 3, 2, 1, 2, 3, 2, 3, 2, 3, 2, 1 };
static const int ansiWB[16] = { 0, 1, 1, 1, 3, 1, 3, 2, 1, 1, 1, 2, 3, 1, 3, 2 };

/* ---- the theme: PROGDIR:Ahiru.theme, written with these when missing -- */

#define THEMEFILE "PROGDIR:Ahiru.theme"
static const char *const rolename[RO_COUNT] = { "text", "highlight", "dir", "program", "archive", "image",
    "sound", "dim", "accent", "mark", "error", "icon", "source", "assign", "link", "script", "hidden" };

static void themewrite(void)
{
    static UWORD *const pals[3] = { pal4, pal8, pal16 };
    static int *const roles[3] = { roles4, roles8, roles16 };
    BPTR fh;
    char line[80];
    int d, i;
    if (!(fh = Open((STRPTR)THEMEFILE, MODE_NEWFILE))) return;
    FPuts(fh, (STRPTR)"; Ahiru - file manager: the colours. Edit freely; Ahiru reads this at start.\n"
                      "; A section for each screen: [4], [8] or [16] colours (COLORS in Ahiru.config).\n"
                      ";   colour N RRGGBB   the screen's colour N (hex; the Amiga keeps 4 bits a gun)\n"
                      ";   ROLE N            which colour draws it: text highlight dir program archive\n"
                      ";                     image sound dim accent mark error icon source assign\n"
                      ";                     link script hidden\n"
                      "; The file kinds' colours in ENVARC:FileColours (csh's, the family's) win over\n"
                      "; these where it gives one: dir link program script archive image sound\n"
                      "; source text icon hidden.\n");
    for (d = 0; d < 3; d++) {
        int n = 4 << d;
        fmt(line, "\n[%ld]\n", (long)n);
        FPuts(fh, (STRPTR)line);
        for (i = 0; i < n; i++) {
            UWORD c = pals[d][i];
            fmt(line, "colour %ld %02lx%02lx%02lx\n", (long)i, (long)((c >> 8 & 15) * 17), (long)((c >> 4 & 15) * 17),
                (long)((c & 15) * 17));
            FPuts(fh, (STRPTR)line);
        }
        for (i = 0; i < RO_COUNT; i++) {
            fmt(line, "%s %ld\n", rolename[i], (long)roles[d][i]);
            FPuts(fh, (STRPTR)line);
        }
    }
    Close(fh);
}

static int hexval(const char *s, int n)
{
    int v = 0, i;
    for (i = 0; i < n; i++) {
        int c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if ((c | 32) >= 'a' && (c | 32) <= 'f') v |= (c | 32) - 'a' + 10;
        else return -1;
    }
    return v;
}

void theme_load(void)
{
    BPTR fh;
    char line[120];
    UWORD *pal = NULL;
    int *roles = NULL, n = 0;
    if (!(fh = Open((STRPTR)THEMEFILE, MODE_OLDFILE))) {
        themewrite();
        return;
    }
    while (FGets(fh, (STRPTR)line, sizeof(line))) {
        char *p = line, *w;
        int l = (int)strlen(line), i;
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == ';') continue;
        if (*p == '[') {
            n = atoi(p + 1);
            pal = n == 4 ? pal4 : n == 8 ? pal8 : n == 16 ? pal16 : NULL;
            roles = n == 4 ? roles4 : n == 8 ? roles8 : n == 16 ? roles16 : NULL;
            continue;
        }
        if (!pal) continue;
        for (w = p; *w && *w != ' ' && *w != '\t'; w++) ;
        if (*w) *w++ = 0;
        while (*w == ' ' || *w == '\t') w++;
        if (nocase(p, "colour") == 0 || nocase(p, "color") == 0) {
            int idx = atoi(w), r, g, b;
            while (*w && *w != ' ') w++;
            while (*w == ' ') w++;
            if (*w == '#') w++;
            r = hexval(w, 2), g = hexval(w + 2, 2), b = hexval(w + 4, 2);
            if (idx >= 0 && idx < n && r >= 0 && g >= 0 && b >= 0)
                pal[idx] = (UWORD)((r >> 4) << 8 | (g >> 4) << 4 | (b >> 4));
            continue;
        }
        for (i = 0; i < RO_COUNT; i++)
            if (nocase(p, rolename[i]) == 0 && atoi(w) >= 0 && atoi(w) < n) roles[i] = atoi(w);
    }
    Close(fh);
}

static void openfont(void)
{
    char *sl;
    int size = 8;
    strncpy(fontname, cfgfont, sizeof(fontname) - 6);
    fontname[sizeof(fontname) - 6] = 0;
    font = NULL;
    if ((sl = strchr(fontname, '/')) != NULL) {
        *sl = 0;
        size = atoi(sl + 1);
        if (size < 6) size = 8;
        if (!strstr(fontname, ".font")) strcat(fontname, ".font");
        tattr.ta_Name = (STRPTR)fontname;
        tattr.ta_YSize = size;
        tattr.ta_Style = 0;
        tattr.ta_Flags = 0;
        if (nocase(fontname, "topaz.font") == 0) font = OpenFont(&tattr);
        else font = OpenDiskFont(&tattr);
        if (font && (font->tf_Flags & FPF_PROPORTIONAL)) {
            CloseFont(font);    /* the grid needs fixed-width glyphs */
            font = NULL;
        }
    }
    if (!font) {
        strcpy(fontname, "topaz.font");
        tattr.ta_Name = (STRPTR)fontname;
        tattr.ta_YSize = 8;
        tattr.ta_Style = 0;
        tattr.ta_Flags = 0;
        font = OpenFont(&tattr);
    }
    cw = font->tf_XSize;
    ch = font->tf_YSize;
    baseline = font->tf_Baseline;
}

void ui_layout(void)
{
    int avail;
    if (uimode != UI_TERM) {
        int w = gzz ? win->GZZWidth : win->Width, h = gzz ? win->GZZHeight : win->Height;
        ncols = w / cw;
        if (ncols > MAXCOLS) ncols = MAXCOLS;
        nrows = h / ch;
        x0 = (w - ncols * cw) / 2;
        y0 = (h - nrows * ch) / 2;
    }
    listh = nrows - 2;
    avail = ncols - 2;          /* a gap column after the first two */
    if (showpv) {
        c0w = avail / 8;        /* Ranger's 1:3:4 */
        c1w = avail * 3 / 8;
        c2w = avail - c0w - c1w;
    } else {
        c0w = avail / 4;
        c1w = avail - c0w;
        c2w = 0;
    }
    c0x = 0;
    c1x = c0w + 1;
    c2x = c1x + c1w + 1;
}

/* no window after all: the font openfont gave goes too */
static int nowin(void)
{
    if (font) CloseFont(font);
    font = NULL;
    return 0;
}

/* AUTO made definite: inside the terminal when started from a Shell in
 * CTerm (its console says 'CTRM'); else the screen. TERMINAL with no
 * console to draw in (started from Workbench) is the screen too. */
int ui_choose(void)
{
    int ctrm = 0, can = !fromwb && term_possible(&ctrm);
    if (cfgmode == UI_TERM) return can ? UI_TERM : UI_SCREEN;
    if (cfgmode == UI_AUTO) return can && ctrm ? UI_TERM : UI_SCREEN;
    return cfgmode;
}

/* a window on Workbench of its own: moved, sized and closed as any */
static int openwbwin(void)
{
    struct Screen *wb;
    int l, t, w, h;
    if (!(wb = LockPubScreen(NULL))) return 0;
    w = winbox[2] > 0 ? winbox[2] : 82 * cw + 24;
    h = winbox[3] > 0 ? winbox[3] : 30 * ch + wb->BarHeight + 12;
    if (w > wb->Width) w = wb->Width;
    if (h > wb->Height - wb->BarHeight - 1) h = wb->Height - wb->BarHeight - 1;
    l = winbox[0] >= 0 ? winbox[0] : (wb->Width - w) / 2;
    t = winbox[1] >= 0 ? winbox[1] : wb->BarHeight + 1 + (wb->Height - wb->BarHeight - 1 - h) / 2;
    if (l + w > wb->Width) l = wb->Width - w;
    if (t + h > wb->Height) t = wb->Height - h;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    win = OpenWindowTags(NULL, WA_Left, l, WA_Top, t, WA_Width, w, WA_Height, h,
        WA_PubScreen, (ULONG)wb, WA_Title, (ULONG)"Ahiru", WA_ScreenTitle, (ULONG)"Ahiru - file manager",
        WA_DragBar, TRUE, WA_DepthGadget, TRUE, WA_CloseGadget, TRUE, WA_SizeGadget, TRUE,
        WA_SizeBBottom, TRUE, WA_GimmeZeroZero, TRUE, WA_MinWidth, 40 * cw, WA_MinHeight, 10 * ch + 24,
        WA_MaxWidth, (ULONG)~0, WA_MaxHeight, (ULONG)~0, WA_Activate, TRUE, WA_RMBTrap, TRUE,
        WA_SimpleRefresh, FALSE,
        WA_IDCMP, IDCMP_RAWKEY | IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS | IDCMP_NEWSIZE |
                  IDCMP_CLOSEWINDOW, TAG_DONE);
    UnlockPubScreen(NULL, wb);
    return win != NULL;
}

int ui_open(void)
{
    struct Screen *wb;
    ULONG modeid = INVALID_ID;
    int w = 640, h = 256, depth;
    UWORD pens[] = { (UWORD)~0 };

    gzz = 0;
    uimode = ui_choose();
    if (uimode == UI_TERM) {
        if (term_open()) {
            dbg("inside the terminal");
            ownscr = 0;
            /* CTerm: the ANSI sixteen, its theme's colours; any other
             * console draws in its screen's own pens - Workbench's, as a
             * Workbench window does */
            colors = termcterm ? 16 : 4;
            rolepen = termcterm ? roles16 : rolesWB;
            ansipen = termcterm ? ansi16 : ansiWB;
            rolepen = filecol_apply(rolepen, ansipen, !termcterm, colors);
            ui_layout();
            return 1;
        }
        uimode = UI_SCREEN;         /* no console after all */
    }
    openfont();
    if (uimode == UI_WINDOW) {
        ownscr = 0;
        if (openwbwin()) {
            gzz = 1;
            dbg("window on Workbench (its own)");
            goto opened;
        }
        uimode = UI_SCREEN;
    }
    dbg("font open");
    depth = colors == 4 ? 2 : colors == 16 ? 4 : 3;
    if ((wb = LockPubScreen(NULL)) != NULL) {
        modeid = GetVPModeID(&wb->ViewPort);
        w = wb->Width;
        h = wb->Height;
        UnlockPubScreen(NULL, wb);
    }
    scr = OpenScreenTags(NULL,
        modeid != INVALID_ID ? SA_DisplayID : TAG_IGNORE, modeid,
        SA_Width, w, SA_Height, h, SA_Depth, depth,
        SA_Quiet, TRUE, SA_ShowTitle, FALSE, SA_Title, (ULONG)"Ahiru - file manager",
        SA_PubName, (ULONG)PUBNAME, SA_Pens, (ULONG)pens,
        SA_Font, (ULONG)&tattr, SA_Type, CUSTOMSCREEN, TAG_DONE);
    if (!scr)                   /* the mode would not take this depth */
        scr = OpenScreenTags(NULL, SA_DisplayID, HIRES_KEY, SA_Depth, depth,
            SA_Quiet, TRUE, SA_ShowTitle, FALSE, SA_Title, (ULONG)"Ahiru - file manager",
            SA_PubName, (ULONG)PUBNAME, SA_Pens, (ULONG)pens,
            SA_Font, (ULONG)&tattr, TAG_DONE);
    dbg(scr ? "screen open" : "no screen");
    if (scr) {
        ownscr = 1;
        win = OpenWindowTags(NULL, WA_Left, 0, WA_Top, 0,
            WA_Width, scr->Width, WA_Height, scr->Height,
            WA_CustomScreen, (ULONG)scr, WA_Backdrop, TRUE,
            WA_Borderless, TRUE, WA_Activate, TRUE, WA_RMBTrap, TRUE,
            WA_IDCMP, IDCMP_RAWKEY | IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS, TAG_DONE);
        if (!win) {
            CloseScreen(scr);
            scr = NULL;
        }
    }
    if (!win) {                 /* last resort: a Workbench window */
        ownscr = 0;
        if (!(wb = LockPubScreen(NULL))) return nowin();
        win = OpenWindowTags(NULL, WA_Left, 0, WA_Top, wb->BarHeight + 1,
            WA_Width, wb->Width, WA_Height, wb->Height - wb->BarHeight - 1,
            WA_PubScreen, (ULONG)wb, WA_Activate, TRUE, WA_RMBTrap, TRUE,
            WA_Borderless, TRUE,
            WA_IDCMP, IDCMP_RAWKEY | IDCMP_VANILLAKEY | IDCMP_MOUSEBUTTONS, TAG_DONE);
        UnlockPubScreen(NULL, wb);
        if (!win) return nowin();
    }
    dbg(ownscr ? "window on own screen" : "window on Workbench");
opened:
    rp = win->RPort;
    SetFont(rp, font);
    SetDrMd(rp, JAM2);
    if (ownscr) {
        PubScreenStatus(scr, 0);    /* consoles and MultiView may visit */
        if (colors == 4) {
            LoadRGB4(&scr->ViewPort, (UWORD *)pal4, 4);
            rolepen = roles4;
            ansipen = ansi4;
        } else if (colors == 16) {
            LoadRGB4(&scr->ViewPort, (UWORD *)pal16, 16);
            rolepen = roles16;
            ansipen = ansi16;
        } else {
            LoadRGB4(&scr->ViewPort, (UWORD *)pal8, 8);
            rolepen = roles8;
            ansipen = ansi8;
        }
    } else {
        rolepen = rolesWB;
        ansipen = ansiWB;
    }
    rolepen = filecol_apply(rolepen, ansipen, !ownscr, ownscr ? colors : 4);
    {   /* requesters (insert volume, errors) come up on our screen */
        struct Process *pr = (struct Process *)FindTask(NULL);
        oldwinptr = pr->pr_WindowPtr;
        pr->pr_WindowPtr = win;
    }
    ui_layout();
    return 1;
}

/* other windows on our screen (a shell, MultiView) keep it open */
int ui_otherwindows(void)
{
    struct Window *w;
    int n = 0;
    if (!ownscr || !scr) return 0;
    for (w = scr->FirstWindow; w; w = w->NextWindow)
        if (w != win) n++;
    return n;
}

/* the window's place and size, for next time (the config keeps it) */
void ui_keepbox(void)
{
    if (win && gzz) {
        winbox[0] = win->LeftEdge;
        winbox[1] = win->TopEdge;
        winbox[2] = win->Width;
        winbox[3] = win->Height;
    }
}

void ui_close(void)
{
    struct Process *pr = (struct Process *)FindTask(NULL);
    if (uimode == UI_TERM) {
        term_close();
        uimode = UI_SCREEN;
        return;
    }
    ui_keepbox();
    if (win) pr->pr_WindowPtr = oldwinptr;
    if (win) CloseWindow(win);
    win = NULL;
    if (scr && ownscr) {
        /* a visitor (a shell, a viewer) keeps it open: no new ones, and
         * wait until they are gone - an orphaned public screen keeps its
         * name, and the next Ahiru could not have it */
        PubScreenStatus(scr, PSNF_PRIVATE);
        while (!CloseScreen(scr)) Delay(10);
    }
    scr = NULL;
    if (font) CloseFont(font);
    font = NULL;
}

/* ---- drawing ---------------------------------------------------- */

/* LOG mode keeps a text copy of the grid (char + fg/bg pen per cell)
 * for ui_dumpgrid - the test road for exact text, since a planar
 * screen grab drops pixel columns */
#define GRIDROWS 128
static unsigned char *gridc, *gridf, *gridb;

static void gridput(int col, int row, const char *s, int len, int fg, int bg)
{
    int i, o;
    if (!gridc || row >= GRIDROWS) return;
    o = row * MAXCOLS + col;
    for (i = 0; i < len; i++) {
        gridc[o + i] = (unsigned char)s[i];
        gridf[o + i] = (unsigned char)fg;
        gridb[o + i] = (unsigned char)bg;
    }
}

void ui_gridon(void)
{
    gridc = calloc(GRIDROWS, MAXCOLS);
    gridf = calloc(GRIDROWS, MAXCOLS);
    gridb = calloc(GRIDROWS, MAXCOLS);
    if (!gridc || !gridf || !gridb) gridc = NULL;
}

/* rows of text, then per row the fg pens and bg pens as hex digits */
void ui_dumpgrid(const char *path)
{
    BPTR fh;
    char line[MAXCOLS + 2];
    int r, c, pass;
    if (!gridc || !(fh = Open((STRPTR)path, MODE_NEWFILE))) return;
    for (pass = 0; pass < 3; pass++) {
        for (r = 0; r < nrows && r < GRIDROWS; r++) {
            for (c = 0; c < ncols; c++) {
                int o = r * MAXCOLS + c, v;
                if (pass == 0) v = gridc[o] ? gridc[o] : ' ';
                else v = "0123456789ABCDEF"[(pass == 1 ? gridf[o] : gridb[o]) & 15];
                line[c] = (char)v;
            }
            line[c] = '\n';
            Write(fh, line, c + 1);
        }
        Write(fh, "=\n", 2);
    }
    Close(fh);
}

void put(int col, int row, const char *s, int len, int fg, int bg)
{
    if (len <= 0 || row < 0 || row >= nrows) return;
    if (col + len > ncols) len = ncols - col;
    if (len <= 0) return;
    gridput(col, row, s, len, fg, bg);
    if (uimode == UI_TERM) {
        term_put(col, row, s, len, fg, bg);
        return;
    }
    SetAPen(rp, fg);
    SetBPen(rp, bg);
    Move(rp, x0 + col * cw, y0 + row * ch + baseline);
    Text(rp, (STRPTR)s, len);
}

/* ---- UTF-8 text files ------------------------------------------------ */

/* is buf UTF-8 with something beyond ASCII (a sequence cut at the very
 * end is fine: the buffer may hold only the file's start)? */
int u8_is(const unsigned char *buf, long len)
{
    long i = 0;
    int high = 0;
    while (i < len) {
        int c = buf[i], k, j;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c < 0xC2 || c > 0xF4) return 0;
        k = c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        for (j = 1; j <= k; j++) {
            if (i + j >= len) return high || j > 1;
            if ((buf[i + j] & 0xC0) != 0x80) return 0;
        }
        high = 1;
        i += k + 1;
    }
    return high;
}

/* a row of UTF-8 text from off, at most w columns (tabs every 8, controls
 * as '.', wide characters two columns): in u8 as UTF-8 (*u8len bytes,
 * room for 4 * w + 8) and in lat as Latin-1, one byte a column ('?' and
 * a space for a wide one). With wrap a long line goes on in the next row;
 * without, the rest of it is skipped. Returns where the next row starts. */
long u8_row(const unsigned char *buf, long len, long off, int w, int wrap,
            char *u8, int *u8len, unsigned char *lat)
{
    long i = off;
    int col = 0, n = 0, k;
    while (i < len) {
        unsigned long c = buf[i];
        int bytes = 1, cw;
        if (c == 10) {
            i++;
            break;
        }
        if (c == 13) {
            i++;
            continue;
        }
        if (c == 9) {
            if (wrap && col >= w) break;
            i++;
            do {
                if (col < w) {
                    lat[col] = ' ';
                    u8[n++] = ' ';
                }
                col++;
            } while (col & 7);
            continue;
        }
        if (c >= 0xC2 && c <= 0xF4) {
            int need = c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3, j;
            unsigned long v = c & (need == 1 ? 0x1F : need == 2 ? 0x0F : 0x07);
            for (j = 1; j <= need && i + j < len && (buf[i + j] & 0xC0) == 0x80; j++)
                v = v << 6 | (buf[i + j] & 0x3F);
            if (j > need) {
                c = v;
                bytes = need + 1;
            }
        }
        /* controls show as '.'; a byte that is not UTF-8 is its Latin-1
         * letter (0xA0 and up) */
        if (c < 32 || c == 127 || (c >= 0x80 && c < 0xA0)) c = '.';
        cw = term_cw(c);
        if (wrap && col + cw > w) break;
        if (col + cw <= w && (cw > 0 || col > 0) && n + bytes + 2 <= 4 * w + 8) {
            if (bytes > 1) {
                for (k = 0; k < bytes; k++) u8[n++] = (char)buf[i + k];
            } else if (c >= 0x80) {                     /* Latin-1: as UTF-8 */
                u8[n++] = (char)(0xC0 | (c >> 6));
                u8[n++] = (char)(0x80 | (c & 0x3F));
            } else
                u8[n++] = (char)c;
            if (cw > 0) lat[col] = (unsigned char)(c < 0x100 ? c : '?');
            if (cw == 2) lat[col + 1] = ' ';
        }
        col += cw;
        i += bytes;
    }
    for (k = col; k < w; k++) lat[k] = ' ';
    *u8len = n;
    return i;
}

/* a row made by u8_row: UTF-8 to a terminal, Latin-1 on a screen */
void put8(int col, int row, const char *u8, int u8len, const unsigned char *lat, int w, int fg, int bg)
{
    if (row < 0 || row >= nrows || w <= 0) return;
    if (col + w > ncols) w = ncols - col;
    if (w <= 0) return;
    if (uimode != UI_TERM) {
        put(col, row, (const char *)lat, w, fg, bg);
        return;
    }
    gridput(col, row, (const char *)lat, w, fg, bg);
    {
        int used = term_pututf8(col, row, u8, u8len, fg, bg);
        if (used < w) term_put(col + used, row, (const char *)lat + used, w - used, fg, bg);
    }
}

void putrole(int col, int row, const char *s, int len, int role)
{
    put(col, row, s, len, rolepen[role], 0);
}

void clearrow(int row, int col, int w)
{
    if (w <= 0) return;
    if (gridc && row < GRIDROWS) {
        if (col + w > ncols) w = ncols - col;
        if (w <= 0) return;
        memset(gridc + row * MAXCOLS + col, ' ', w);
        memset(gridf + row * MAXCOLS + col, 0, w);
        memset(gridb + row * MAXCOLS + col, 0, w);
    }
    if (uimode == UI_TERM) {
        term_clear(row, col, w);
        return;
    }
    SetAPen(rp, 0);
    RectFill(rp, x0 + col * cw, y0 + row * ch,
             x0 + (col + w) * cw - 1, y0 + (row + 1) * ch - 1);
}

/* the whole grid's ground, before drawing everything again */
void ui_clearall(void)
{
    if (uimode == UI_TERM) {
        term_clearall();
        return;
    }
    SetAPen(rp, 0);
    if (gzz) RectFill(rp, 0, 0, win->GZZWidth - 1, win->GZZHeight - 1);
    else RectFill(rp, 0, 0, win->Width - 1, win->Height - 1);
}

void ui_flush(void)
{
    if (uimode == UI_TERM) term_flush();
}

void ui_suspend(void)
{
    if (uimode == UI_TERM) term_suspend();
}

void ui_resume(int wait)
{
    if (uimode == UI_TERM) {
        term_resume(wait);
        ui_layout();
    }
}

/* ---- what the person did ------------------------------------------- */

ULONG ui_sig(void)
{
    if (uimode == UI_TERM) return term_sig();
    return 1UL << win->UserPort->mp_SigBit;
}

/* one IntuiMessage as an event; 0: nothing that matters */
static int intui_event(UiEv *e)
{
    struct IntuiMessage *m;
    while ((m = (struct IntuiMessage *)GetMsg(win->UserPort)) != NULL) {
        ULONG cl = m->Class;
        UWORD code = m->Code, qual = m->Qualifier;
        WORD mx = m->MouseX, my = m->MouseY;
        memset(e, 0, sizeof(*e));
        e->secs = m->Seconds;
        e->mics = m->Micros;
        e->qual = qual;
        ReplyMsg((struct Message *)m);
        if (gzz) {
            mx -= win->BorderLeft;
            my -= win->BorderTop;
        }
        switch (cl) {
        case IDCMP_VANILLAKEY:
            e->type = EV_KEY;
            e->key = code;
            return 1;
        case IDCMP_MOUSEBUTTONS:
            if (code == SELECTDOWN && cellat(mx, my, &e->col, &e->row)) {
                e->type = EV_CLICK;
                return 1;
            }
            break;
        case IDCMP_NEWSIZE:
            e->type = EV_RESIZE;
            return 1;
        case IDCMP_CLOSEWINDOW:
            e->type = EV_CLOSE;
            return 1;
        case IDCMP_RAWKEY:
            if (code == 0x7A || code == 0x7B) {     /* the wheel (NewMouse codes) */
                e->type = EV_WHEEL;
                e->key = code == 0x7A ? -1 : 1;
                return 1;
            }
            if (code < 0x80) {
                int sh = qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT), k = 0;
                switch (code) {
                case 0x4C: k = sh ? KEY_PGUP : KEY_UP; break;
                case 0x4D: k = sh ? KEY_PGDN : KEY_DOWN; break;
                case 0x4F: k = sh ? KEY_TOP : KEY_LEFT; break;
                case 0x4E: k = sh ? KEY_END : KEY_RIGHT; break;
                case 0x5F: k = KEY_HELP; break;
                default:
                    if (code >= 0x50 && code <= 0x59) k = KEY_F1 + code - 0x50;
                }
                if (k) {
                    e->type = EV_KEY;
                    e->key = k;
                    return 1;
                }
            }
            break;
        }
    }
    return 0;
}

int ui_event(UiEv *e)
{
    if (uimode == UI_TERM) return term_event(e);
    return intui_event(e);
}

int ui_keywaiting(void)
{
    if (uimode == UI_TERM) return term_keywaiting();
    return win->UserPort->mp_MsgList.lh_Head->ln_Succ != NULL;
}

/* a row of ANSI cells: runs of one attribute go out as one Text */
void ui_attrrow(int col, int row, const unsigned char *cells,
                const unsigned char *attrs, int w)
{
    int s = 0, i;
    for (i = 1; i <= w; i++) {
        if (i == w || attrs[i] != attrs[s]) {
            int a = attrs[s];
            int fg = ansipen[(a & 7) + ((a & 64) ? 8 : 0)];
            int bg = ansipen[(a >> 3) & 7];
            put(col + s, row, (const char *)cells + s, i - s, fg, bg);
            s = i;
        }
    }
}

void say(const char *s, int err)
{
    int l = (int)strlen(s);
    if (l > ncols - 1) l = ncols - 1;
    clearrow(nrows - 1, 0, ncols);
    put(1, nrows - 1, s, l, rolepen[err ? RO_ERR : RO_ACC], 0);
    msgup = 1;
}

void showfault(const char *what)
{
    char e[100], m[200];
    LONG err = IoErr();
    if (err == 0) {
        say(what, 1);
        return;
    }
    Fault(err, NULL, (STRPTR)e, sizeof(e));
    fmt(m, "%s: %s", what, e);
    say(m, 1);
}

/* KEYS=... on the command line plays keys at start (the test road:
 * wasabi can grab the screen but cannot type). '|' is Return, '~'
 * is Esc, '{' and '}' are Left and Right, '_' is Space. 0 = none left. */
const char *inject;

int injected(void)
{
    int k;
    if (!inject || !*inject) return 0;
    k = (unsigned char)*inject++;
    if (k == '|') k = 13;
    else if (k == '~') k = 27;
    else if (k == '{') k = KEY_LEFT;
    else if (k == '}') k = KEY_RIGHT;
    else if (k == '_') k = ' ';
    return k;
}

/* the next key: vanilla codes, or a KEY_* for the raw ones */
int waitkey(int *rawqual)
{
    UiEv e;
    int k;
    if ((k = injected()) != 0) {
        if (rawqual) *rawqual = 0;
        return k;
    }
    if (inject) {                   /* the played keys ran out here */
        ui_dumpgrid("RAM:Ahiru.grid");
        inject = NULL;
    }
    ui_flush();
    for (;;) {
        while (!ui_event(&e)) {
            ULONG got;
            ui_flush();
            got = Wait(ui_sig() | SIGBREAKF_CTRL_C | rexx_sig());
            if (got & rexx_sig()) rexx_busy(0);     /* ARexx: GET only, the rest busy */
            if (got & SIGBREAKF_CTRL_C) {
                breakseen = 1;      /* every loop backs out, then quits */
                return KEY_BREAK;
            }
        }
        if (e.type == EV_RESIZE) {  /* a question stays up; drawn again after */
            ui_layout();
            uiresized = 1;
            continue;
        }
        if (e.type == EV_CLOSE) return 27;
        if (e.type == EV_KEY) {
            if (rawqual) *rawqual = e.qual;
            return e.key;
        }
    }
}

/* Esc pressed while a long job runs? Drains the queue either way. */
int ui_abort(void)
{
    UiEv e;
    int ab = 0;
    while (ui_event(&e))
        if ((e.type == EV_KEY && e.key == 27) || e.type == EV_CLOSE) ab = 1;
    return ab;
}

int ask(const char *prompt)
{
    int k;
    say(prompt, 0);
    k = waitkey(NULL);
    msgup = 0;
    drawstatus();
    if (k == KEY_BREAK) k = 27;
    if (k >= 'A' && k <= 'Z') k += 32;
    return k;
}

static void drawinput(const char *prompt, const char *buf, int cpos)
{
    int pl = (int)strlen(prompt), l = (int)strlen(buf), field, start;
    char c;
    clearrow(nrows - 1, 0, ncols);
    put(0, nrows - 1, prompt, pl, rolepen[RO_ACC], 0);
    field = ncols - pl - 1;
    start = cpos >= field ? cpos - field + 1 : 0;
    if (l - start > 0)
        put(pl, nrows - 1, buf + start, l - start > field ? field : l - start,
            rolepen[RO_HI], 0);
    c = cpos < l ? buf[cpos] : ' ';
    put(pl + cpos - start, nrows - 1, &c, 1, 0, rolepen[RO_HI]);
}

/* edit buf in place on the status row; TRUE on Return */
static void (*onchange)(const char *buf);

/* editline, telling fn after every change (the live filter) */
int editlive(const char *prompt, char *buf, int max, void (*fn)(const char *buf))
{
    int r;
    onchange = fn;
    r = editline(prompt, buf, max, -1);
    onchange = NULL;
    return r;
}

int editline(const char *prompt, char *buf, int max, int cpos)
{
    int l, qual, i;
    char was[PATHMAX];
    if ((int)strlen(buf) > max) buf[max] = 0;
    if (cpos < 0 || cpos > (int)strlen(buf)) cpos = (int)strlen(buf);
    for (;;) {
        int k;
        drawinput(prompt, buf, cpos);
        strcpy(was, buf);
        k = waitkey(&qual);
        l = (int)strlen(buf);
        if (k == 13) break;
        if (k == 27 || k == KEY_BREAK) {
            msgup = 0;
            drawstatus();
            return 0;
        }
        if (k == 8) {
            int j = cpos - 1;
            if (cpos == 0) continue;
            if (qual & (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) j = 0;
            else if (qual & IEQUALIFIER_CONTROL) {
                j = cpos;
                while (j > 0 && (buf[j - 1] == '/' || buf[j - 1] == ':')) j--;
                while (j > 0 && buf[j - 1] != '/' && buf[j - 1] != ':') j--;
            }
            memmove(buf + j, buf + cpos, l - cpos + 1);
            cpos = j;
        } else if (k == 127) {
            if (cpos < l) memmove(buf + cpos, buf + cpos + 1, l - cpos);
        } else if (k == KEY_LEFT) {
            if (cpos > 0) cpos--;
        } else if (k == KEY_RIGHT) {
            if (cpos < l) cpos++;
        } else if (k == KEY_TOP) {
            cpos = 0;
        } else if (k == KEY_END) {
            cpos = l;
        } else if (k == 21) {           /* Ctrl-U: clear */
            buf[0] = 0;
            cpos = 0;
        } else if (k >= 32 && k <= 255 && k != 127 && l < max) {
            for (i = l; i >= cpos; i--) buf[i + 1] = buf[i];
            buf[cpos++] = (char)k;
        }
        if (onchange && strcmp(was, buf) != 0) onchange(buf);
    }
    msgup = 0;
    drawstatus();
    return 1;
}

/* Ranger's which-key panel: what the pending prefix can do next.
 * items are pairs: key, description. */
void drawhints(const char *const *items, int n, int colw)
{
    int per, rows, r, i, top;
    char line[MAXCOLS];
    if (colw > ncols) colw = ncols;
    per = ncols / colw;
    if (per < 1) per = 1;
    rows = (n + per - 1) / per;
    top = nrows - 1 - rows;
    if (top < 1) top = 1;
    memset(line, '-', sizeof(line));
    put(0, top - 1, line, ncols, rolepen[RO_DIM], 0);
    for (r = top; r < nrows - 1; r++) clearrow(r, 0, ncols);
    for (i = 0; i < n; i++) {
        int row = top + i / per, col = (i % per) * colw;
        const char *k = items[2 * i], *d = items[2 * i + 1];
        int dl = (int)strlen(d);
        if (row >= nrows - 1) break;
        put(col + 1, row, k, (int)strlen(k), rolepen[RO_ACC], 0);
        if (dl > colw - 6) dl = colw - 6;
        put(col + 5, row, d, dl, rolepen[RO_TEXT], 0);
    }
}

/* ---- the pager ---------------------------------------------------
 * Text (wrapped), ANSI art (80 columns, colours) or hex, over a
 * buffer in memory. 'x' flips between hex and the readable form. */

static long *loff;
static int *lattr;
static long lnum, lcap;

static void addline(void *ctx, long off, int attr)
{
    (void)ctx;
    if (lnum >= lcap) {
        long nc = lcap ? lcap * 2 : 1024;
        long *a = realloc(loff, nc * sizeof(long));
        int *b;
        if (!a) return;
        loff = a;
        if (!(b = realloc(lattr, nc * sizeof(int)))) return;
        lattr = b;
        lcap = nc;
    }
    loff[lnum] = off;
    lattr[lnum] = attr;
    lnum++;
}

static int pgu8;                 /* the pager's text is UTF-8 */
static char u8row[4 * MAXCOLS + 8];

static void indexlines(unsigned char *buf, long len, int ty)
{
    lnum = 0;
    pgu8 = ty == TY_TEXT && u8_is(buf, len);
    if (ty == TY_ANSI) {
        ansi_index(buf, len, 80, addline, NULL);
    } else if (ty == TY_OTHER) {
        long o, bpr = hex_bpr(ncols);
        for (o = 0; o < len || o == 0; o += bpr) addline(NULL, o, 0);
    } else {
        long o = 0;
        unsigned char tmp[MAXCOLS];
        addline(NULL, 0, 0);
        while (o < len) {
            int n;
            o = pgu8 ? u8_row(buf, len, o, ncols, 1, u8row, &n, tmp) : text_row(buf, len, o, tmp, ncols, 1);
            if (o < len) addline(NULL, o, 0);
        }
    }
}

void pager(const char *title, unsigned char *buf, long len, int ty)
{
    int act;
    long top = 0, page = nrows - 2;
    int orig = ty, r;
    unsigned char *cells = NULL, *attrs = NULL;
    char info[80];
    if (ty != TY_TEXT && ty != TY_ANSI) ty = TY_OTHER;
    indexlines(buf, len, ty);
    for (;;) {
        long maxtop = lnum - page;
        int k, tl;
        if (maxtop < 0) maxtop = 0;
        if (top > maxtop) top = maxtop;
        if (top < 0) top = 0;
        /* title */
        clearrow(0, 0, ncols);
        tl = (int)strlen(title);
        if (tl > ncols - 30) tl = ncols - 30;
        put(1, 0, title, tl, rolepen[RO_HI], 0);
        fmt(info, "%s  %ld-%ld/%ld  %ld%%",
                ty == TY_ANSI ? "ansi" : ty == TY_TEXT ? "text" : "hex",
                top + 1, top + page < lnum ? top + page : lnum, lnum,
                lnum > page ? (top * 100) / (maxtop ? maxtop : 1) : 100L);
        put(ncols - (int)strlen(info) - 1, 0, info, (int)strlen(info), rolepen[RO_ACC], 0);
        /* body */
        if (ty == TY_ANSI) {
            if (!cells) {
                cells = malloc(ncols * page);
                attrs = malloc(ncols * page);
            }
            if (cells && attrs) {
                int rows = lnum ? ansi_render(buf, len, loff[top], lattr[top],
                                              cells, attrs, ncols, (int)page, 80) : 0;
                (void)rows;
                for (r = 0; r < page; r++)
                    ui_attrrow(0, r + 1, cells + r * ncols, attrs + r * ncols, ncols);
            }
        } else {
            for (r = 0; r < page; r++) {
                long ln = top + r;
                if (ln >= lnum) {
                    clearrow(r + 1, 0, ncols);
                    continue;
                }
                if (ty == TY_TEXT && pgu8) {
                    int n;
                    u8_row(buf, len, loff[ln], ncols, 1, u8row, &n, rowbuf);
                    put8(0, r + 1, u8row, n, rowbuf, ncols, rolepen[RO_TEXT], 0);
                    continue;
                } else if (ty == TY_TEXT)
                    text_row(buf, len, loff[ln], rowbuf, ncols, 1);
                else
                    hex_row(buf, len, loff[ln], hex_bpr(ncols), rowbuf, ncols);
                put(0, r + 1, (char *)rowbuf, ncols, rolepen[RO_TEXT], 0);
            }
        }
        clearrow(nrows - 1, 0, ncols);
        {
            /* the bottom line names the keys as they are bound now */
            char help[MAXCOLS], a[16], b[16], c[16], d[16], e[16], f[16];
            keys_first(CTX_VIEW, V_CLOSE, a);
            keys_first(CTX_VIEW, V_DOWN, b);
            keys_first(CTX_VIEW, V_PAGEDOWN, c);
            keys_first(CTX_VIEW, V_TOP, d);
            keys_first(CTX_VIEW, V_BOTTOM, e);
            keys_first(CTX_VIEW, V_HEX, f);
            fmt(help, "%s back   %s line   %s page   %s/%s top/end   %s hex", a, b, c, d, e, f);
            put(1, nrows - 1, help, (int)strlen(help), rolepen[RO_DIM], 0);
        }
        k = waitkey(NULL);
        if (k == KEY_BREAK) break;
        act = keys_find(CTX_VIEW, k, 0, NULL);
        if (act == V_CLOSE) break;
        else if (act == V_DOWN) top++;
        else if (act == V_UP) top--;
        else if (act == V_PAGEDOWN) top += page;
        else if (act == V_PAGEUP) top -= page;
        else if (act == V_HALFDOWN) top += page / 2;
        else if (act == V_HALFUP) top -= page / 2;
        else if (act == V_TOP) top = 0;
        else if (act == V_BOTTOM) top = maxtop;
        else if (act == V_HEX) {
            ty = ty == TY_OTHER ? (orig == TY_ANSI ? TY_ANSI : TY_TEXT) : TY_OTHER;
            indexlines(buf, len, ty);
            top = 0;
        }
    }
    free(cells);
    free(attrs);
    free(loff);
    free(lattr);
    loff = NULL;
    lattr = NULL;
    lcap = lnum = 0;
}

void cellxy(int col, int row, int *x, int *y)
{
    *x = x0 + col * cw;
    *y = y0 + row * ch;
}

/* the cell under a pixel (the mouse); FALSE outside the grid */
int cellat(int px, int py, int *col, int *row)
{
    if (px < x0 || py < y0) return 0;
    *col = (px - x0) / cw;
    *row = (py - y0) / ch;
    return *col < ncols && *row < nrows;
}
