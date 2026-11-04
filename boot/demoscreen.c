/* SPDX-License-Identifier: GPL-2.0-only */
/* The demo floppy's screen while it loads (DESIGN D25 revision 3, 6.2 step 4): one bitplane in hires, the Zenix
 * name, a bar the loader fills (demo_bar, boot/demoload.c), and when something fails the screen turns red with the
 * reason. Built for the 68000 (-m68000, no multiply or divide): it also says "needs a 68020" on a 68000 Amiga, where
 * the rest of the stage 2 cannot run. Through intuition.library's OpenScreen and graphics.library's text, which
 * Kickstart has ready when the bootblock runs. Without a screen (no memory) the whole display turns red instead. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

long zb_os(void *base, long lvo, long d0, long d1, long d2, long d3, long a0, long a1);
extern void *SysBase;
void *demo_rp;                          /* the screen's RastPort (0: no screen), for demo_bar */
static void *ibase, *gbase, *vp;

#define OpenLibrary(n, v)   ((void *)zb_os(SysBase, -552, (long)(v), 0, 0, 0, 0, (long)(n)))
#define OpenScreen(ns)      ((u8 *)zb_os(ibase, -198, 0, 0, 0, 0, (long)(ns), 0))
#define Text(rp, s, n)      zb_os(gbase, -60, (long)(n), 0, 0, 0, (long)(s), (long)(rp))
#define Move(rp, x, y)      zb_os(gbase, -240, (long)(x), (long)(y), 0, 0, 0, (long)(rp))
#define RectFill(rp, a, b, c, d) zb_os(gbase, -306, (long)(a), (long)(b), (long)(c), (long)(d), 0, (long)(rp))
#define SetAPen(rp, p)      zb_os(gbase, -342, (long)(p), 0, 0, 0, 0, (long)(rp))
#define SetRGB4(v, i, r, g, b) zb_os(gbase, -288, (long)(i), (long)(r), (long)(g), (long)(b), (long)(v), 0)

struct textattr { const char *name; u16 ysize; u8 style, flags; };
struct newscreen {                      /* intuition's NewScreen (32 bytes) */
    short left, top, width, height, depth;
    u8 detailpen, blockpen;
    u16 viewmodes, type;
    const struct textattr *font;
    const char *title;
    void *gadgets, *bitmap;
};
static const struct textattr topaz = { "topaz.font", 8, 0, 0 };
static const struct newscreen ns = { 0, 0, 640, 200, 1, 0, 1, 0x8000 /* HIRES */, 0x010F /* CUSTOMSCREEN|SCREENQUIET */,
                                     &topaz, 0, 0, 0 };

static void centre(const char *s, int y)
{
    int n = 0;
    while (s[n]) n++;
    if (n > 80) n = 80;
    Move(demo_rp, (640 - (n << 3)) >> 1, y);
    Text(demo_rp, s, n);
}

static void open_screen(void)
{
    if (demo_rp) return;
    ibase = OpenLibrary("intuition.library", 0);
    gbase = OpenLibrary("graphics.library", 0);
    u8 *scr = ibase && gbase ? OpenScreen(&ns) : 0;
    if (!scr) return;
    *(volatile u16 *)0xDFF096 = 0x8180; /* DMACON: copper and bitplanes on again (the bootblock's colour screen had them off) */
    vp = scr + 44;                      /* Screen.ViewPort */
    demo_rp = scr + 84;                 /* Screen.RastPort */
    for (int i = 0; i < 4; i++)         /* 0 and Intuition's pointer (17-19): the background, so no pointer shows */
        SetRGB4(vp, i ? 16 + i : 0, 0, 0, 2);
    SetRGB4(vp, 1, 12, 12, 13);
    SetAPen(demo_rp, 1);
    centre("Z E N I X", 64);
    centre("a Unix-like system for the Amiga: the demo disk", 84);
    centre("loading the whole system into memory", 96);
}

/* failed = 0: the loading screen with an empty bar; 1: the red screen with lines (0-ended) under the name */
void demo_screen(int failed, const char *const *lines)
{
    open_screen();
    if (!demo_rp) {                     /* no screen: COLOR00 fills the display (DMA off), red = failed */
        if (failed) {
            *(volatile u16 *)0xDFF096 = 0x0180;
            *(volatile u16 *)0xDFF100 = 0x0200;
            *(volatile u16 *)0xDFF180 = 0xF00;
        }
        return;
    }
    if (!failed) {
        RectFill(demo_rp, 118, 110, 521, 110);
        RectFill(demo_rp, 118, 123, 521, 123);
        RectFill(demo_rp, 118, 110, 119, 123);
        RectFill(demo_rp, 520, 110, 521, 123);
        return;
    }
    for (int i = 0; i < 4; i++)
        SetRGB4(vp, i ? 16 + i : 0, 8, 0, 0);
    SetRGB4(vp, 1, 15, 15, 15);
    SetAPen(demo_rp, 0);
    RectFill(demo_rp, 0, 72, 639, 199);
    SetAPen(demo_rp, 1);
    for (int i = 0, y = 92; lines[i] && y < 196; i++, y += 12)
        centre(lines[i], y);
}
