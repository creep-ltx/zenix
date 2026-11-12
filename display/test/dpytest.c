// SPDX-License-Identifier: GPL-2.0-only
/* dpytest: a test client of the display server (DESIGN D38 steps 3-4, docs/man/dpytest.txt), driven by commands on
 * its standard input (or, with -f FILE, from FILE as it grows), one a line, so a test script types them
 * (tools/dpy-test.sh, tools/zdesk-test.sh); each answers one line
 * "dpytest: ..." on its standard output:
 *   win NAME W H PEN [X Y]  a window of W x H pixels, filled with PEN, its name in the top left in pen 7 on PEN;
 *                      X Y: the place it asks for (the server keeps it on the screen, D38 "Windows stay on the screen")
 *   text NAME X Y S    the text S at X, Y (pen 1 on 2)
 *   utext NAME X Y S   the same in the desktop's font (DPYF_UI)
 *   font               the desktop's font (DPY_FONT_INFO): "dpytest: font ID HEIGHT BASELINE XSIZE FLAGS NAME|-
 *                      widths of A D W . space"
 *   hole NAME X Y W H  a rectangle of pen 0 inside the window, drawn with it from now on (the chips' drag test)
 *   pens NAME          a patch of all 16 pens in its bottom right, from now on (the chips' drag cannot lift it)
 *   move NAME X Y      (the window manager's) the frame's top left at X, Y
 *   size NAME W H      (the window manager's) the inside W x H
 *   raise NAME | lower NAME | focus NAME | hide NAME | show NAME | close NAME
 *   scroll NAME N      the inside's contents up N lines (O_COPY), the freed lines filled with pen 2
 *   wm                 become the window manager (DPY_WM_HELLO)
 *   info NAME          where the window is: "dpytest: info NAME x y w h ix iy state"
 *   bad NAME           a batch with a broken order: refused, with its offset
 *   time NAME N        N raises of NAME (and of the window under it in turn), then N moves: the mean in microseconds
 *                      (E-clock), the server's whole round trip
 *   events             the events received so far, a line each
 *   stat [LABEL]       the server's counters since the last stat (DPY_STAT), then from 0: "dpytest: stat LABEL blits
 *                      B pixels P glyphs G draws D paints N copies C (0)"
 *   mem                chip and fast RAM free, and the memory of displayd and dpytest (procinfo)
 *   peek X Y           (with -fb) the pretend card's pixel at X, Y: "dpytest: peek X Y HEX", its bytes in memory order
 *   mouse DX DY        the mouse moved DX, DY counts (CON_INPUT's CONI_MOUSE, root's), no button
 *   quit               the end (DPY_BYE)
 * -fb WxHxB (root, before the connection; DESIGN D43, docs/d43-rtg.md 10): a pretend graphics card of W x H pixels of
 * B bytes (2: R5G6B5, 3: R8G8B8, 4: A8R8G8B8) in fast RAM, its record written into the boot's hardware table as
 * zenixboot RTG writes one, so the display server it starts draws there (it allocates the card itself, so the card
 * lives as long as it draws, and clears the record when it ends).
 * EV_EXPOSE is answered by drawing the window again (its fill and name); every event is kept for "events". */
#include "libz.h"
#include "../display.h"
#include "../../kernel/hwinfo.h"
typedef unsigned short u16;
typedef unsigned char u8;

#define NW 8
static struct { char name[16]; int id, pen, w, h, hx, hy, hw, hh, pens; } ws[NW];
static char evlog[2048];
static u32 nev;

static void say(const char *fmt, ...)
{
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    sys_write(1, b, (u32)n);
}
static int find(const char *n)
{
    for (int i = 0; i < NW; i++)
        if (ws[i].id && !strcmp(ws[i].name, n))
            return i;
    return -1;
}
static void paint(int i, int x, int y, int w, int h)
{
    dpy_clip(ws[i].id, x, y, w, h);
    dpy_fill(ws[i].id, 0, 0, ws[i].w, ws[i].h, ws[i].pen);
    dpy_text(ws[i].id, 0, 0, 7, ws[i].pen, 0, ws[i].name, (int)strlen(ws[i].name));
    if (ws[i].hw)
        dpy_fill(ws[i].id, ws[i].hx, ws[i].hy, ws[i].hw, ws[i].hh, 0);
    for (int p = 0; ws[i].pens && p < 16; p++)
        dpy_fill(ws[i].id, ws[i].w - 64 + p * 4, ws[i].h - 4, 4, 4, p);
    dpy_clip(ws[i].id, 0, 0, 0, 0);
    dpy_flush();
}
/* D43: the pretend card (-fb): the record written here, the card itself allocated by the display server it starts
 * (so the card lives exactly as long as the server draws on it); its address read back from the record */
static struct hwfb *fbrec;
static void fb_end(void)
{
    if (fbrec && !fbrec->base)
        fbrec->magic = 0;               /* never taken: the server clears a record it took when it ends */
    fbrec = 0;
}
static int fb_make(const char *spec)
{
    char *e;
    u32 w = (u32)strtol(spec, &e, 10), h = *e == 'x' ? (u32)strtol(e + 1, &e, 10) : 0,
        b = *e == 'x' ? (u32)strtol(e + 1, &e, 10) : 0;
    struct hwinfo *hw = (struct hwinfo *)HWINFO_ADDR;
    if (*e || w < 320 || w > 2048 || h < 200 || h > 2048 || b < 2 || b > 4) {
        dprintf(2, "dpytest: -fb %s: WxHxB, 320-2048 x 200-2048 x 2, 3 or 4 bytes\n", spec);
        return -1;
    }
    if (sys_getuid() != 0 || hw->magic != HWINFO_MAGIC) {
        dprintf(2, "dpytest: -fb: root's, on a machine whose boot left its hardware table\n");
        return -1;
    }
    fbrec = &hw->fb;
    fbrec->magic = 0;
    fbrec->base = 0;                    /* the server allocates the card and writes its address here */
    fbrec->width = (u16)w;
    fbrec->height = (u16)h;
    fbrec->bpr = w * b;
    fbrec->bpp = (u8)b;
    fbrec->format = b == 2 ? 10 : b == 3 ? 2 : 6;  /* Picasso96's R5G6B5, R8G8B8, A8R8G8B8 */
    fbrec->flags = HWFB_TEST;
    fbrec->hvs = fbrec->dl_rtg = fbrec->dl_chip = 0;
    fbrec->magic = HWFB_MAGIC;
    return 0;
}

static void events(void)
{
    struct dpyev e;
    while (dpy_next(&e, 0) > 0) {
        int i = -1;
        for (int k = 0; k < NW; k++)
            if (ws[k].id == e.win)
                i = k;
        if (e.type == EV_EXPOSE && i >= 0)
            paint(i, e.x, e.y, e.w, e.h);
        if (e.type == EV_RESIZE && i >= 0) {
            ws[i].w = e.w;
            ws[i].h = e.h;
        }
        char l[64];
        int n = snprintf(l, sizeof l, "ev %d %s %d %d %d %d %d\n", e.type, i >= 0 ? ws[i].name : "?", e.code, e.x, e.y,
                         e.w, e.h);
        if (nev + (u32)n < sizeof evlog) {
            memcpy(evlog + nev, l, (u32)n);
            nev += (u32)n;
        }
    }
}

static int words(char *l, char **w, int max)
{
    int n = 0;
    while (*l && n < max) {
        while (*l == ' ') *l++ = 0;
        if (!*l) break;
        w[n++] = l;
        while (*l && *l != ' ') l++;
    }
    return n;
}

static void command(char *line)
{
    char *w[8];
    int n = words(line, w, 8), i = n > 1 ? find(w[1]) : -1, r = 0;
    if (!n)
        return;
    if (!strcmp(w[0], "win") && n >= 5) {
        int k = -1;
        for (int j = 0; j < NW; j++)
            if (!ws[j].id) { k = j; break; }
        if (k < 0) { say("dpytest: no room\n"); return; }
        int id = dpy_window(WK_NORMAL, w[1], n >= 7 ? atoi(w[5]) : -1, n >= 7 ? atoi(w[6]) : -1, atoi(w[2]), atoi(w[3]), 0);
        if (id < 0) { say("dpytest: win %s: %d\n", w[1], id); return; }
        strncpy(ws[k].name, w[1], 15);
        ws[k].id = id;
        ws[k].pen = atoi(w[4]);
        ws[k].w = atoi(w[2]);
        ws[k].h = atoi(w[3]);
        paint(k, 0, 0, ws[k].w, ws[k].h);
        say("dpytest: win %s id %d\n", w[1], id);
        return;
    }
    if (!strcmp(w[0], "stat")) {       /* the server's counters since the last stat, then from 0 */
        u32 st[12];
        memset(st, 0, sizeof st);
        int r = dpy_call(DPY_STAT, (u32)st, 3, 0, 0);
        say("dpytest: stat %s blits %u pixels %u glyphs %u draws %u paints %u copies %u moves %u movet %u shows %u "
            "showt %u waitt %u late %u (%d)\n", n > 1 ? w[1] : "-", st[0], st[1], st[2], st[3], st[4], st[5], st[6],
            st[7], st[8], st[9], st[10], st[11], r);
        return;
    }
    if (!strcmp(w[0], "wm")) { say("dpytest: wm %d\n", dpy_call(DPY_WM_HELLO, 0, 0, 0, 0)); return; }
    if (!strcmp(w[0], "mode")) {       /* mode [hires|lace]: DPY_MODE (D38 "Screen modes"); none: only asked */
        int want = n < 2 ? DPYM_ASK : !strcmp(w[1], "lace") ? DPYM_LACE : DPYM_HIRES;
        int r = dpy_mode(want);
        events();                       /* EV_SCREEN: dpy_w, dpy_h follow; the windows' exposes drawn */
        say("dpytest: mode %d screen %ux%u\n", r, dpy_w, dpy_h);
        return;
    }
    if (!strcmp(w[0], "events")) { events(); sys_write(1, evlog, nev); nev = 0; say("dpytest: events end\n"); return; }
    if (!strcmp(w[0], "mem")) {
        u32 mi[4];
        sys_meminfo(mi);
        say("dpytest: mem chip %u KB fast %u KB free\n", mi[0] >> 10, mi[1] >> 10);
        struct uproc pr;
        for (u32 k = 0; k < 64; k++)
            if (sys_procinfo(k, &pr) == 0 && pr.state && pr.state != 5 &&
                (!strcmp(pr.name, "displayd") || !strcmp(pr.name, "dpytest")))
                say("dpytest: mem %s %u KB\n", pr.name, pr.mem >> 10);
        return;
    }
    if (!strcmp(w[0], "font")) {       /* the desktop's font's metrics (DPY_FONT_INFO) */
        static struct dpyfont f;
        int r = dpy_font(DPYF_UI, &f);
        say("dpytest: font %d %u %u %u %u %u %s widths %u %u %u %u %u\n", r, f.id, f.height, f.baseline, f.xsize,
            f.flags, f.name[0] ? f.name : "-", f.adv['A'], f.adv['D'], f.adv['W'], f.adv['.'], f.adv[' ']);
        return;
    }
    if (!strcmp(w[0], "quit")) { dpy_close(); fb_end(); say("dpytest: quit\n"); sys_exit(0); }
    if (!strcmp(w[0], "peek") && n >= 3) {
        u32 x = (u32)atoi(w[1]), y = (u32)atoi(w[2]);
        if (!fbrec || !fbrec->base || x >= fbrec->width || y >= fbrec->height) {
            say("dpytest: peek: no card, or outside it\n");
            return;
        }
        const u8 *p = (const u8 *)fbrec->base + y * fbrec->bpr + x * fbrec->bpp;
        char hx[9];
        for (u32 k = 0; k < fbrec->bpp; k++)
            snprintf(hx + 2 * k, 3, "%02X", p[k]);
        say("dpytest: peek %u %u %s\n", x, y, hx);
        return;
    }
    if (!strcmp(w[0], "mouse") && n >= 3) {
        u8 r[3] = { (u8)atoi(w[1]), (u8)atoi(w[2]), 0 };
        say("dpytest: mouse %d\n", sys_coninput(0, CONI_MOUSE, r, 3));
        sys_sleep(3);
        return;
    }
    if (i < 0) { say("dpytest: no window %s\n", n > 1 ? w[1] : ""); return; }
    int id = ws[i].id;
    if (!strcmp(w[0], "text") && n >= 5) {
        dpy_text(id, atoi(w[2]), atoi(w[3]), 1, 2, 0, w[4], (int)strlen(w[4]));
        r = dpy_flush();
    } else if (!strcmp(w[0], "utext") && n >= 5) {
        static struct dpyfont f;
        dpy_font(DPYF_UI, &f);
        dpy_textf(id, &f, atoi(w[2]), atoi(w[3]), 1, 2, 0, w[4], (int)strlen(w[4]));
        r = dpy_flush();
    } else if (!strcmp(w[0], "hole") && n >= 6) {
        ws[i].hx = atoi(w[2]); ws[i].hy = atoi(w[3]); ws[i].hw = atoi(w[4]); ws[i].hh = atoi(w[5]);
        paint(i, 0, 0, ws[i].w, ws[i].h);
    } else if (!strcmp(w[0], "pens")) {
        ws[i].pens = 1;
        paint(i, 0, 0, ws[i].w, ws[i].h);
    } else if (!strcmp(w[0], "move") && n >= 4) {
        struct dpyinfo in;
        dpy_info(id, &in);
        r = dpy_call(DPY_WM_PLACE, (u32)id, (u32)atoi(w[2]) << 16 | (u16)atoi(w[3]), (u32)in.w << 16 | in.h, 0);
    } else if (!strcmp(w[0], "size") && n >= 4) {
        struct dpyinfo in;
        dpy_info(id, &in);
        r = dpy_call(DPY_WM_PLACE, (u32)id, (u32)(u16)in.x << 16 | (u16)in.y, (u32)atoi(w[2]) << 16 | (u16)atoi(w[3]), 0);
    } else if (!strcmp(w[0], "raise")) r = dpy_call(DPY_WM_STACK, (u32)id, 0, 0, 0);
    else if (!strcmp(w[0], "lower")) r = dpy_call(DPY_WM_STACK, (u32)id, 1, 0, 0);
    else if (!strcmp(w[0], "focus")) r = dpy_call(DPY_WM_FOCUS, (u32)id, 0, 0, 0);
    else if (!strcmp(w[0], "hide")) r = dpy_call(DPY_WM_HIDE, (u32)id, HIDE_MINIMISE, 0, 0);
    else if (!strcmp(w[0], "show")) r = dpy_call(DPY_WM_SHOW, (u32)id, 0, 0, 0);
    else if (!strcmp(w[0], "close")) { r = dpy_destroy(id); ws[i].id = 0; }
    else if (!strcmp(w[0], "scroll") && n >= 3) {
        int k = atoi(w[2]);
        dpy_copy(id, 0, k, ws[i].w, ws[i].h - k, 0, 0);
        dpy_fill(id, 0, ws[i].h - k, ws[i].w, k, 2);
        r = dpy_flush();
    } else if (!strcmp(w[0], "info")) {
        struct dpyinfo in;
        r = dpy_info(id, &in);
        if (!r) {
            say("dpytest: info %s %d %d %d %d %d %d %d\n", w[1], in.x, in.y, in.w, in.h, in.ix, in.iy, in.state);
            return;
        }
    } else if (!strcmp(w[0], "bad")) {
        static unsigned short o[] = { O_FILL << 8 | 6, 0, 0, 8, 8, 1, O_TEXT << 8 | 7, 0, 0, 0, 7, 0, 300 };
        struct umsg m;
        memset(&m, 0, sizeof m);
        m.type = DPY_DRAW; m.a[0] = (u32)id; m.a[1] = (u32)o; m.a[2] = sizeof o;
        int sp = sys_port_find(DPY_PORTNAME);
        r = sys_call(sp, &m);
        say("dpytest: bad %d at %d\n", r, (int)m.a[3]);
        return;
    } else if (!strcmp(w[0], "time") && n >= 3) {
        int k = atoi(w[2]), other = -1;
        for (int j = 0; j < NW; j++)
            if (ws[j].id && j != i) other = j;
        u32 t0 = sys_hrtime();
        for (int j = 0; j < k; j++) {
            dpy_call(DPY_WM_STACK, (u32)(j & 1 && other >= 0 ? ws[other].id : id), 0, 0, 0);
            events();
        }
        u32 t1 = sys_hrtime();
        struct dpyinfo in;
        dpy_info(id, &in);
        for (int j = 0; j < k; j++) {
            dpy_call(DPY_WM_PLACE, (u32)id, (u32)(u16)(in.x + (j & 1 ? 0 : 16)) << 16 | (u16)in.y, (u32)in.w << 16 | in.h, 0);
            events();
        }
        u32 t2 = sys_hrtime();
        /* E-clock ticks (709,379 Hz on PAL) to microseconds */
        say("dpytest: time %s raise %u us move %u us (%d each, an emulator's figure)\n", w[1],
            (u32)((u32)(t1 - t0) / (u32)k * 1000u / 709u), (u32)((u32)(t2 - t1) / (u32)k * 1000u / 709u), k);
        return;
    } else { say("dpytest: what is %s?\n", w[0]); return; }
    say("dpytest: %s %s %d\n", w[0], w[1], r);
}

int main(int argc, char **argv)
{
    /* -f FILE: the commands from FILE, read as it grows (the shell stays free for the test's other commands) */
    int a = 1;
    if (argc > a + 1 && !strcmp(argv[a], "-fb")) {
        if (fb_make(argv[a + 1]) < 0)
            return 2;
        a += 2;
    }
    int in = 0, file = argc > a + 1 && !strcmp(argv[a], "-f");
    if (file && (in = sys_open(argv[a + 1], O_RDONLY)) < 0) {
        dprintf(2, "dpytest: %s: %s\n", argv[a + 1], zwhy("cannot open"));
        fb_end();
        return 1;
    }
    int r = dpy_open("dpytest");
    say("dpytest: open %d screen %ux%u depth %u\n", r, dpy_w, dpy_h, dpy_depth);
    if (fbrec)
        say("dpytest: card %ux%ux%u at 0x%x\n", fbrec->width, fbrec->height, fbrec->bpp, fbrec->base);
    if (r < 0) {
        fb_end();
        return 1;
    }
    char line[128];
    u32 len = 0;
    for (;;) {
        struct pollfd p[2] = { { in, POLLIN, 0 }, { dpy_port(), POLLPORT | POLLRDBAND, 0 } };
        if (file)
            sys_evwait(&p[1], 1, 20);
        else
            sys_evwait(p, 2, -1);
        events();
        if (file || (p[0].revents & POLLIN)) {
            char c[64];
            int k = sys_read(in, c, sizeof c);
            if (k < 0 || (k == 0 && !file)) { dpy_close(); fb_end(); return 0; }
            for (int j = 0; j < k; j++) {
                if (c[j] == '\n' || c[j] == '\r') {
                    line[len] = 0;
                    command(line);
                    len = 0;
                } else if (len < sizeof line - 1)
                    line[len++] = c[j];
            }
        }
    }
}
