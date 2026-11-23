/* SPDX-License-Identifier: GPL-2.0-only */
/* mouseprefs: the mouse's preferences (DESIGN D61, docs/d61-mouse.md 7, docs/man/mouseprefs.txt): a window with the
 * pointer's speed, the acceleration (AmigaOS's kind: a threshold, then a multiplier), the buttons (swapped for the
 * left hand), the double-click time with a place to try it, and the pointer's look (the pointer files, and a 16 x 16
 * editor with its three colours), and AmigaOS's four buttons:
 *   Test    the choice applied now (DPY_MOUSE); the window stays, so Cancel can still go back
 *   Use     applied for this session (zdesk puts back what it found when the session ends); nothing saved
 *   Save    applied and saved: ~/.config/zdesk/mouse (root: /etc/zdesk/mouse, the system's, for users without their
 *           own), and an edited pointer as pointers/custom.pointer beside it
 *   Cancel  everything back as it was when the window opened
 * Keys: Up and Down choose a line, Left and Right (or a click, Shift-click backwards) change it, t u s c the buttons,
 * Return Use, Esc Cancel. -v says where its parts are. Cold code: -Os. */
#include "libz.h"
#include "../display.h"
#include "../mousefile.h"
#include "../prefswin.h"
#include "cfgfile.h"
typedef unsigned char u8;
typedef unsigned short u16;

#define NP 16                           /* pointers listed */
static char ptrs[NP][MF_NAME];          /* the pointer files' names; the last may be "custom" */
static int nptrs, verbose;
static struct dpymouse was, sel;        /* in effect when the window opened (Cancel's), and chosen */
static int sel_ptr = -1;                /* the pointer chosen (-1: the one in effect, unnamed) */
static int edited;                      /* the picture was changed in the editor: it is "custom" */
static int paint = 1;                   /* the editor's colour: 0 clear, 1-3 */
static u32 dc_t;                        /* the double-click test: the last press's tick */
static int dc_state;                    /* 0 waiting, 1 a click, 2 a double click seen */

static const u16 speeds[] = { 25, 33, 50, 67, 75, 100, 125, 150, 200, 250, 300, 400, 600, 800 };
#define NSPEED (int)(sizeof speeds / sizeof speeds[0])
/* 16 colours a pointer's colour cycles through (12-bit) */
static const u16 cols[16] = { 0x000, 0xFFF, 0x888, 0x444, 0xBBB, 0xC22, 0xF80, 0xEEC,
                              0xFF0, 0x2A2, 0x0C8, 0x28F, 0x00A, 0x82C, 0xF6A, 0x630 };

static void value(int i, char *b, int n);
static const char *const label[7] = { "Speed", "Acceleration", "Threshold", "Multiplier", "Buttons", "Double click",
                                      "Pointer" };
static struct prefswin pw = { .nrows = 7, .gx = 112, .label = label, .value = value };

/* ---- what there is to choose ---- */
static void ptr_add(const char *n)
{
    for (int i = 0; i < nptrs; i++)
        if (!strcmp(ptrs[i], n))
            return;
    if (nptrs < NP && strlen(n) < MF_NAME)
        strcpy(ptrs[nptrs++], n);
}
static void ptrs_scan(int user)
{
    char path[ZPATH_MAX], **names;
    if (!dpy_mousepath("pointers/x", path, user))
        return;
    path[strlen(path) - 2] = 0;         /* the folder */
    int n = dir_list(path, &names);
    for (int i = 0; i < n; i++) {
        int l = (int)strlen(names[i]);
        if (l > 8 && l - 8 < MF_NAME && !strcmp(names[i] + l - 8, ".pointer")) {
            names[i][l - 8] = 0;
            ptr_add(names[i]);
        }
    }
    if (n > 0)
        dir_free(names, n);
}
static int acc_of(const struct dpymouse *m)     /* the acceleration's word (0-3), or 4: custom */
{
    for (int a = 0; a < 4; a++)
        if (m->thresh == mf_acc_thresh[a] && m->mult == mf_acc_mult[a])
            return a;
    return 4;
}

/* ---- the window ---- */
static void value(int i, char *b, int n)
{
    static const char *const accs[5] = { "off", "low", "medium", "high", "custom" };
    switch (i) {
    case 0: snprintf(b, (u32)n, "%u%%%s", sel.speed, sel.speed == 100 ? " (a pixel a count)" : ""); break;
    case 1: snprintf(b, (u32)n, "%s", accs[acc_of(&sel)]); break;
    case 2: snprintf(b, (u32)n, "%u counts a frame", sel.thresh); break;
    case 3: snprintf(b, (u32)n, "%u.%02u times", sel.mult / 100, sel.mult % 100); break;
    case 4: snprintf(b, (u32)n, "%s", sel.flags & DPYMF_SWAP ? "swapped (left hand)" : "normal"); break;
    case 5: snprintf(b, (u32)n, "%u.%02u s", sel.dclick / 100, sel.dclick % 100); break;
    default: snprintf(b, (u32)n, "%s", edited ? "custom (edited)" : sel_ptr >= 0 ? ptrs[sel_ptr] : "(as it is)");
    }
}
/* the program's own part, below the rows: the editor at the left (16 x 16 cells of EW x EH), the colours and the
 * double-click test at the right */
#define EW 6
#define EH 4
static int ex(void) { return 8; }
static int ey(void) { return pw_ey(&pw) + 2; }
static int cx(int i) { return 8 + 16 * EW + 12 + i * 22; }     /* colour i (0 clear, 1-3) */
static int cy(void) { return ey(); }
static int tx(void) { return cx(0); }                          /* the double-click test */
static int ty(void) { return ey() + 26; }
static int tw(void) { return pw.ww - 8 - tx(); }
static int th(void) { return 16 * EH - 26; }
static int pix(int x, int y)
{
    return (sel.image[2 * y] << x & 0x8000 ? 1 : 0) | (sel.image[2 * y + 1] << x & 0x8000 ? 2 : 0);
}
static u16 pal[16];                     /* the screen's pens (DPY_PALETTE): the editor shows the nearest */
static int npal;
static int pen_of(u16 c)
{
    int best = 1, bd = 1 << 30;
    for (int i = 0; i < npal; i++) {
        int dr = (c >> 8) - (pal[i] >> 8), dg = (c >> 4 & 15) - (pal[i] >> 4 & 15), db = (c & 15) - (pal[i] & 15),
            d = dr * dr + dg * dg + db * db;
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}
static void cell(int x, int y)
{
    int v = pix(x, y);
    dpy_fill(pw.win, ex() + x * EW, ey() + y * EH, EW, EH, v ? pen_of(sel.colour[v - 1]) : 2);
    if (!v && !((x ^ y) & 1))
        dpy_fill(pw.win, ex() + x * EW + EW / 2, ey() + y * EH + EH / 2, 1, 1, 5);
}
static void own_draw(void)
{
    pw_box(&pw, ex() - 1, ey() - 1, 16 * EW + 2, 16 * EH + 2, 1, 7);
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            cell(x, y);
    dpy_fill(pw.win, ex() + sel.hotx * EW, ey() + sel.hoty * EH, 1, EH, 9);   /* the hot spot's mark */
    for (int i = 0; i < 4; i++) {
        dpy_fill(pw.win, cx(i), cy(), 18, 18, i ? pen_of(sel.colour[i - 1]) : 2);
        pw_box(&pw, cx(i), cy(), 18, 18, paint == i ? 9 : 7, paint == i ? 9 : 1);
        if (!i)
            pw_text(&pw, cx(i) + 5, cy() + 5, 1, 2, "-");
    }
    dpy_fill(pw.win, tx(), ty(), tw(), th(), dc_state == 2 ? 4 : 5);
    pw_box(&pw, tx(), ty(), tw(), th(), 7, 1);
    const char *t = dc_state == 2 ? "Double click!" : "Double-click here";
    pw_text(&pw, tx() + 6, ty() + (th() - pw.lh) / 2, dc_state == 2 ? 7 : 1, dc_state == 2 ? 4 : 5, t);
}
static void draw(void)
{
    pw_draw(&pw);
    own_draw();
    dpy_flush();
}
static void layout(void)
{
    pw.extra = 16 * EH + 8;
    pw_layout(&pw, 400);
}
static void where(void)
{
    struct dpyinfo in;
    if (!verbose || dpy_info(pw.win, &in) < 0)
        return;
    pw_where(&pw, "mouseprefs");
    dprintf(1, "mouseprefs: editor %d %d %d %d colours %d %d %d test %d %d\n", in.ix + ex(), in.iy + ey(), EW, EH,
            in.ix + cx(0) + 9, in.ix + 22, in.iy + cy() + 9, in.ix + tx() + tw() / 2, in.iy + ty() + th() / 2);
}

/* ---- applying ---- */
static int apply(const struct dpymouse *m)
{
    int r = dpy_call(DPY_MOUSE, (u32)m, DPYMO_SET, 0, 0);
    if (r < 0)
        pw_say(&pw, "The desktop refused the setting");
    return r;
}
static void report(const char *what)
{
    char v[7][48];
    for (int i = 0; i < 7; i++)
        value(i, v[i], sizeof v[i]);
    dprintf(1, "mouseprefs: %s: speed %s, acceleration %s, threshold %u, multiplier %u, buttons %s, double click %s, "
            "pointer %s%s%s\n", what, v[0], v[1], sel.thresh, sel.mult, v[4], v[5], v[6], pw.msg[0] ? ": " : "",
            pw.msg);
}
static int save(void)
{
    char path[ZPATH_MAX], l[7][40];
    int user = sys_getuid() != 0;
    int a = acc_of(&sel);
    const char *name = edited ? "custom" : sel_ptr >= 0 ? ptrs[sel_ptr] : 0;
    if (edited) {
        char t[400];
        dpy_pointertext(&sel, t);
        if (!dpy_mousepath("pointers/custom.pointer", path, user) || cfg_write(path, t) < 0)
            return -EACCES;
    }
    snprintf(l[0], 40, "speed %u", sel.speed);
    snprintf(l[1], 40, "acceleration %s", a < 4 ? mf_acc_name[a] : "custom");
    snprintf(l[2], 40, "threshold %u", sel.thresh);
    snprintf(l[3], 40, "multiplier %u", sel.mult);
    snprintf(l[4], 40, "buttons %s", sel.flags & DPYMF_SWAP ? "swapped" : "normal");
    snprintf(l[5], 40, "doubleclick %u", sel.dclick);
    snprintf(l[6], 40, "pointer %s", name ? name : "arrow");
    static const char *const keys[7] = { "speed", "acceleration", "threshold", "multiplier", "buttons", "doubleclick",
                                         "pointer" };
    const char *lines[7];
    for (int i = 0; i < 7; i++)
        lines[i] = l[i];
    if (!dpy_mousepath("mouse", path, user))
        return -ENOENT;
    return cfg_set(path, keys, lines, name ? 7 : 6);
}
/* a button -> 1: the window closes */
static int press(int b)
{
    pw.msg[0] = 0;
    if (b == PW_CANCEL) {
        apply(&was);
        sel = was;
        edited = 0;
        report("cancel");
        return 1;
    }
    if (apply(&sel) < 0) {
        report(b == PW_TEST ? "test" : b == PW_USE ? "use" : "save");
        draw();
        return 0;
    }
    if (b == PW_SAVE) {
        int r = save();
        if (r < 0) {
            snprintf(pw.msg, sizeof pw.msg, "Not saved: %s", zstrerror(r) ? zstrerror(r) : "cannot write");
            report("save");
            draw();
            return 0;
        }
    }
    report(b == PW_TEST ? "test" : b == PW_USE ? "use" : "save");
    if (b == PW_TEST) {
        pw_say(&pw, "Applied: Use or Save to keep it, Cancel to go back");
        draw();
        return 0;
    }
    return 1;
}
static void cycle(int i, int d)
{
    pw.msg[0] = 0;
    pw.row = i;
    if (i == 0) {
        int k = 0;
        while (k < NSPEED - 1 && speeds[k] < sel.speed)
            k++;
        if (speeds[k] != sel.speed && d < 0)
            k++;                        /* a value between two: the one below is the next backwards */
        k = (k + d + NSPEED) % NSPEED;
        sel.speed = speeds[k];
    } else if (i == 1) {
        int a = acc_of(&sel);
        a = a == 4 ? (d > 0 ? 0 : 3) : (a + d + 4) % 4;
        sel.thresh = mf_acc_thresh[a];
        sel.mult = mf_acc_mult[a];
    } else if (i == 2)
        sel.thresh = (u8)((sel.thresh + d + 17) % 17);          /* 0..16 */
    else if (i == 3)
        sel.mult = (u16)(sel.mult + 25 * d > 400 ? 100 : sel.mult + 25 * d < 100 ? 400 : sel.mult + 25 * d);
    else if (i == 4)
        sel.flags ^= DPYMF_SWAP;
    else if (i == 5)
        sel.dclick = (u16)(sel.dclick + 10 * d > 150 ? 20 : sel.dclick + 10 * d < 20 ? 150 : sel.dclick + 10 * d);
    else if (nptrs) {
        if (edited || sel_ptr < 0)
            sel_ptr = d > 0 ? -1 : 0;
        sel_ptr = (sel_ptr + d + nptrs) % nptrs;
        edited = 0;
        struct dpymouse m = sel;
        if (dpy_pointerfile(ptrs[sel_ptr], &m, "mouseprefs") == 0)
            sel = m;
        else
            pw_say(&pw, "That pointer file cannot be read");
    }
}
static int own_click(const struct dpyev *e)
{
    int x = e->x, y = e->y, sh = e->qual & (GQ_LSHIFT | GQ_RSHIFT);
    if (x >= ex() && x < ex() + 16 * EW && y >= ey() && y < ey() + 16 * EH) {
        int px = (x - ex()) / EW, py = (y - ey()) / EH;
        if (sh) {                       /* Shift: the hot spot here */
            sel.hotx = (signed char)px;
            sel.hoty = (signed char)py;
        } else {
            u16 m = (u16)(0x8000 >> px);
            sel.image[2 * py] = (u16)((sel.image[2 * py] & ~m) | (paint & 1 ? m : 0));
            sel.image[2 * py + 1] = (u16)((sel.image[2 * py + 1] & ~m) | (paint & 2 ? m : 0));
        }
        edited = 1;
        return 1;
    }
    for (int i = 0; i < 4; i++)
        if (x >= cx(i) && x < cx(i) + 18 && y >= cy() && y < cy() + 18) {
            if (paint == i && i) {      /* the chosen colour again: its next colour (Shift: the one before) */
                int k = 0;
                while (k < 15 && cols[k] != sel.colour[i - 1])
                    k++;
                sel.colour[i - 1] = cols[(k + (sh ? 15 : 1)) % 16];
                edited = 1;
            }
            paint = i;
            return 1;
        }
    if (x >= tx() && x < tx() + tw() && y >= ty() && y < ty() + th()) {
        u32 now = sys_ticks();          /* the chosen time, before it is applied */
        dc_state = dc_state == 1 && now - dc_t < sel.dclick ? 2 : 1;
        dc_t = now;
        return 1;
    }
    return 0;
}

static const char USAGE[] = "usage: mouseprefs [-v]\n";

int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-v"))
            verbose = 1;
        else if (!strcmp(argv[i], "--") && i + 1 == argc)
            break;
        else
            zbadopt(argv[i], USAGE);
    int r = dpy_open("mouseprefs");
    if (r < 0) {
        dprintf(2, "mouseprefs: the desktop: %s\n", zstrerror(r) ? zstrerror(r) : "not reachable");
        return 1;
    }
    if ((r = dpy_call(DPY_MOUSE, (u32)&was, DPYMO_GET, 0, 0)) < 0) {
        dprintf(2, "mouseprefs: the desktop's mouse: %s\n", zstrerror(r) ? zstrerror(r) : "not answered");
        dpy_close();
        return 1;
    }
    if (sys_getuid())
        ptrs_scan(1);
    ptrs_scan(0);
    for (int i = 1; i < nptrs; i++)     /* by name (a directory's order is the disk's) */
        for (int j = i; j > 0 && strcmp(ptrs[j - 1], ptrs[j]) > 0; j--) {
            char t[MF_NAME];
            strcpy(t, ptrs[j]);
            strcpy(ptrs[j], ptrs[j - 1]);
            strcpy(ptrs[j - 1], t);
        }
    sel = was;
    char path[ZPATH_MAX], name[MF_NAME];  /* the pointer in effect by its name, as the saved file says */
    struct dpymouse f;
    if (dpy_mousepath("mouse", path, sys_getuid() != 0) && dpy_mousefile(path, &f, name, "mouseprefs") < 0 &&
        sys_getuid())
        dpy_mousefile("/etc/zdesk/mouse", &f, name, "mouseprefs");
    for (int i = 0; i < nptrs; i++)
        if (!strcmp(ptrs[i], name[0] ? name : "arrow"))
            sel_ptr = i;
    int depth = dpy_palette(pal, 16);
    npal = depth > 0 && depth <= 4 ? 1 << depth : 16;
    layout();
    pw.win = dpy_window(WK_NORMAL, "Mouse preferences", -1, -1, pw.ww, pw.wh, 0);
    if (pw.win < 0) {
        dprintf(2, "mouseprefs: no window: %s\n", zstrerror(pw.win) ? zstrerror(pw.win) : "refused");
        dpy_close();
        return 1;
    }
    draw();
    int done = 0, placed = 0;
    while (!done) {
        struct dpyev e;
        int got = dpy_next(&e, -1);
        if (got < 0)
            break;
        if (!got)
            continue;
        switch (e.type) {
        case EV_EXPOSE:
        case EV_RESIZE:
            draw();
            if (!placed++)
                where();
            break;
        case EV_SCREEN:
            if (e.code & DPYS_FONT) {
                layout();
                dpy_resize(pw.win, pw.ww, pw.wh);
            }
            draw();
            where();
            break;
        case EV_BUTTON:
            if (verbose && !(e.code & 0x80))
                dprintf(1, "mouseprefs: button %d down%s\n", e.code & 0x7F, e.arg == 2 ? " (double)" : "");
            if ((e.code & 0x7F) == 1 && !(e.code & 0x80)) {
                int h = pw_hit(&pw, e.x, e.y);
                if (h >= 100)
                    done = press(h - 100);
                else if (h >= 0) {
                    cycle(h, e.qual & (GQ_LSHIFT | GQ_RSHIFT) ? -1 : 1);
                    draw();
                } else if (own_click(&e))
                    draw();
            }
            break;
        case EV_KEY: {
            int k = pw_key(&pw, &e);
            if (k >= 100)
                done = press(k - 100);
            else {
                if (k)
                    cycle(k > 0 ? k - 1 : -k - 1, k > 0 ? 1 : -1);
                draw();
            }
            break;
        }
        case EV_CLOSE:
            press(PW_CANCEL);
            done = 1;
            break;
        }
    }
    dpy_close();
    return 0;
}
