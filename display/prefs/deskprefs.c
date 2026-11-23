/* SPDX-License-Identifier: GPL-2.0-only */
/* deskprefs: the desktop's preferences (docs/d38-desktop.md "Preferences", docs/man/deskprefs.txt): a window with the
 * screen's mode, the desktop's font and the colours (zdesk's theme), and AmigaOS's four buttons:
 *   Test    the choice applied now (DPY_MODE, DPY_FONT_SET; the theme through ~/.config/zdesk/theme.use and SIGHUP
 *           to the user's zdesk); the window stays, so Cancel can still go back
 *   Use     applied for this session (until Log out); nothing saved
 *   Save    applied and saved: ~/.config/zdesk/screen and ~/.config/zdesk/theme (root: /etc/zdesk/screen and
 *           /etc/zdesk/theme, the system's own, which dlogin's greeter and users without their own file use)
 *   Cancel  everything back as it was when the window opened
 * A request the server refuses (no chip RAM for interlace, a font it cannot read) is said in the window, the
 * setting stays as it was, and nothing is written for it. Keys: Up and Down choose a line, Left and Right (or a click,
 * Shift-click backwards) change it, t u s c the buttons, Return Use, Esc Cancel. -v says where its parts are. */
#include "libz.h"
#include "../display.h"
#include "../screenfile.h"
#include "../server/amifont.h"
#include "cfgfile.h"
typedef unsigned char u8;

#define FONTDIR "/usr/share/fonts/amiga"
#define TTFDIR "/usr/share/fonts/truetype"
#define NF 64
#define NT 16
static struct { char name[DPY_FONTNAME]; int size; } fonts[NF];  /* 0: the 8 x 8 */
static int nfonts = 1;
static char themes[NT][16];
static int nthemes;
static int verbose;

/* what is in effect when the window opened (Cancel's), what is applied now, and what the gadgets show */
struct choice { int mode, font, theme; };
static struct choice was, now, sel;
static char use_was[16];                /* the session's theme file at the start ("": none) */
static int win, row, ww, wh, lh;
static struct dpyfont uf;
static char msg[64];

/* ---- what there is to choose ---- */
static int font_find(const char *name, int size)
{
    for (int i = 1; i < nfonts; i++)
        if (fonts[i].size == size && !strcmp(fonts[i].name, name))
            return i;
    return -1;
}
static int fdropped;                    /* past the list's room: said, not dropped unseen */
static void font_add(const char *name, int size)
{
    if (nfonts >= NF)
        fdropped++;
    else if (nfonts < NF && strlen(name) < DPY_FONTNAME && size > 0 && size <= AF_MAXH && font_find(name, size) < 0) {
        strcpy(fonts[nfonts].name, name);
        fonts[nfonts++].size = size;
    }
}
static void fonts_scan(void)
{
    char **names;
    int n = dir_list(FONTDIR, &names);
    for (int i = 0; i < n; i++) {
        int l = (int)strlen(names[i]);
        if (l < 6 || l - 5 >= DPY_FONTNAME || strcmp(names[i] + l - 5, ".font"))
            continue;
        char *p = path_join(FONTDIR, names[i]);
        int fd = p ? sys_open(p, O_RDONLY) : -1;
        free(p);
        if (fd < 0)
            continue;
        u8 *d = malloc(AF_MAXCONTENTS);
        int got = d ? sys_read(fd, d, AF_MAXCONTENTS) : -1;
        sys_close(fd);
        char sizes[160], file[8];
        if (got >= 4 && !(d[0] == 0x0f && d[1] == 0x03)) {    /* not an outline font's */
            af_contents(d, (unsigned)got, 0, file, sizeof file, sizes, sizeof sizes);
            names[i][l - 5] = 0;
            for (char *s = sizes; *s;) {
                int v = (int)strtoul(s, &s, 10);
                font_add(names[i], v);
                while (*s == ' ')
                    s++;
            }
        }
        free(d);
    }
    if (n > 0)
        dir_free(names, n);
    /* TrueType fonts (D51): any size, offered in the sizes a desktop uses; displayd draws one the first time it is
     * chosen. A name an Amiga font has is the Amiga font's (displayd reads that first). */
    static const u8 tsz[] = { 8, 9, 10, 11, 12, 13, 14, 16 };
    int namiga = nfonts;
    n = dir_list(TTFDIR, &names);
    for (int i = 0; i < n; i++) {
        int l = (int)strlen(names[i]), dup = 0;
        if (l < 5 || l - 4 > 25 || strcmp(names[i] + l - 4, ".ttf"))
            continue;
        names[i][l - 4] = 0;
        for (int k = 1; k < namiga; k++)
            dup |= !strcmp(fonts[k].name, names[i]);
        for (u32 k = 0; k < sizeof tsz && !dup; k++)
            font_add(names[i], tsz[k]);
    }
    if (n > 0)
        dir_free(names, n);
    if (fdropped)
        dprintf(2, "deskprefs: %d font sizes past the list's %d are not shown\n", fdropped, NF - 1);
    for (int i = 2; i < nfonts; i++)    /* by name and size (a directory's order is the disk's) */
        for (int j = i; j > 1 && (strcmp(fonts[j - 1].name, fonts[j].name) > 0 ||
                                  (!strcmp(fonts[j - 1].name, fonts[j].name) && fonts[j - 1].size > fonts[j].size)); j--) {
            typeof(fonts[0]) t = fonts[j];
            fonts[j] = fonts[j - 1];
            fonts[j - 1] = t;
        }
}
static void themes_scan(void)
{
    char **names;
    int n = dir_list("/etc/zdesk/themes", &names);
    for (int i = 0; i < n && nthemes < NT; i++) {
        int l = (int)strlen(names[i]);
        if (l > 6 && l - 6 < 16 && !strcmp(names[i] + l - 6, ".theme")) {
            memcpy(themes[nthemes], names[i], (u32)(l - 6));
            themes[nthemes++][l - 6] = 0;
        }
    }
    if (n > 0)
        dir_free(names, n);
    for (int i = 1; i < nthemes; i++)
        for (int j = i; j > 0 && strcmp(themes[j - 1], themes[j]) > 0; j--) {
            char t[16];
            strcpy(t, themes[j]);
            strcpy(themes[j], themes[j - 1]);
            strcpy(themes[j - 1], t);
        }
}
static int theme_find(const char *t)
{
    for (int i = 0; i < nthemes; i++)
        if (!strcmp(themes[i], t))
            return i;
    if (nthemes < NT && strlen(t) < 16) {
        strcpy(themes[nthemes], t);
        return nthemes++;
    }
    return 0;
}

/* ---- the files ---- */
static int root(void) { return sys_getuid() == 0; }
static const char *home(void) { const char *h = getenv("HOME"); return h && *h && strlen(h) < 160 ? h : 0; }
/* the user's ~/.config/zdesk/NAME (made if need be), or root's /etc/zdesk/NAME, into buf; 0: no home */
static const char *save_path(const char *name, char *buf)
{
    if (root() && strcmp(name, "theme.use")) {
        strcpy(buf, "/etc/zdesk/");
    } else {
        if (!home())
            return 0;
        strcpy(buf, home());
        strcat(buf, "/.config");
        sys_mkdir(buf);
        strcat(buf, "/zdesk");
        sys_mkdir(buf);
        strcat(buf, "/");
    }
    strcat(buf, name);
    return buf;
}
/* a file's first word (a theme's name) into w (16 bytes) -> 0, or -1 */
static int first_word(const char *path, char *w)
{
    char b[64];
    int fd = sys_open(path, O_RDONLY), n = fd < 0 ? -1 : sys_read(fd, b, sizeof b - 1);
    if (fd >= 0)
        sys_close(fd);
    if (n <= 0)
        return -1;
    b[n] = 0;
    char *s = b;
    while (*s == ' ' || *s == '\t' || *s == '\n')
        s++;
    int k = 0;
    while (s[k] && s[k] != ' ' && s[k] != '\t' && s[k] != '\n' && s[k] != '\r' && k < 15)
        k++;
    if (!k)
        return -1;
    memcpy(w, s, (u32)k);
    w[k] = 0;
    return 0;
}
/* the screen file: its mode and font lines replaced, every other line (comments) kept */
static int save_screen(const char *path, int mode, int font)
{
    static const char *const keys[2] = { "mode", "font" };
    char fl[48];
    const char *lines[2] = { mode ? "mode hires-lace" : "mode hires", fl };
    if (font)
        snprintf(fl, sizeof fl, "font %s %d", fonts[font].name, fonts[font].size);
    else
        strcpy(fl, "font builtin");
    return cfg_set(path, keys, lines, 2);
}

/* ---- applying ---- */
static void say(const char *s) { strncpy(msg, s, sizeof msg - 1); msg[sizeof msg - 1] = 0; }
/* zdesk reads its theme again on SIGHUP: every process called zdesk that this user may signal (the kernel refuses
 * another user's) */
static void tell_zdesk(void)
{
    struct uproc p;
    for (u32 k = 0; k < 256; k++)
        if (sys_procinfo(k, &p) == 0 && p.state && p.state != 5 && !strcmp(p.name, "zdesk"))
            sys_killsig((int)p.pid, SIGHUP);
}
/* the session's theme (theme.use) set to t, or removed ("") */
static void theme_use(const char *t)
{
    char path[ZPATH_MAX];
    if (!save_path("theme.use", path)) {
        say("No HOME: the colours cannot be tried");
        return;
    }
    if (*t) {
        char l[20];
        strcpy(l, t);
        strcat(l, "\n");
        cfg_write(path, l);
    } else
        sys_unlink(path);
}
/* c in effect now -> 0, or -1 with the reason in msg (what did work stays: now follows each part) */
static int apply(const struct choice *c)
{
    int r;
    if (c->mode != now.mode) {
        if ((r = dpy_mode(c->mode)) < 0) {
            say(r == -ENOMEM ? "Not enough chip RAM for interlace" : r == -EBUSY ? "The screen is busy: try again"
                : "The mode was refused");
            return -1;
        }
        now.mode = c->mode;
    }
    if (c->font != now.font) {
        if ((r = dpy_fontset(c->font ? fonts[c->font].name : 0, fonts[c->font].size)) < 0) {
            say(r == -ENOENT ? "That font is not there" : r == -ENOMEM ? "No memory for that font"
                : r == -EBUSY ? "The screen is busy: try again" : "That font cannot be read");
            return -1;
        }
        now.font = c->font;
    }
    if (c->theme != now.theme) {
        theme_use(themes[c->theme]);
        now.theme = c->theme;
        tell_zdesk();
    }
    return 0;
}

/* ---- the window ---- */
static const char *const label[3] = { "Screen mode", "Font", "Colours" };
static const char *const button[4] = { "Test", "Use", "Save", "Cancel" };
#define GX 104                          /* the gadgets' left */
static int gy(int i) { return 6 + i * (lh + 6); }
static int by(void) { return gy(3) + 4; }
static int bx(int i) { return 8 + i * ((ww - 16 + 6) / 4); }
static int bw(void) { return (ww - 16 + 6) / 4 - 6; }
static void value(int i, char *b, int n)
{
    if (i == 0)
        snprintf(b, (u32)n, "%s", sel.mode ? "Hires interlaced" : "Hires");
    else if (i == 1) {
        if (sel.font)
            snprintf(b, (u32)n, "%s %d", fonts[sel.font].name, fonts[sel.font].size);
        else
            snprintf(b, (u32)n, "topaz 8 (built in)");
    } else
        snprintf(b, (u32)n, "%s", nthemes ? themes[sel.theme] : "flat");
}
static void text(int x, int y, int fg, int bg, const char *s)
{
    dpy_textf(win, &uf, x, y, fg, bg, 0, s, (int)strlen(s));
}
static void box(int x, int y, int w, int h, int light, int dark)
{
    dpy_fill(win, x, y, w, 1, light);
    dpy_fill(win, x, y, 1, h, light);
    dpy_fill(win, x, y + h - 1, w, 1, dark);
    dpy_fill(win, x + w - 1, y, 1, h, dark);
}
static void draw(void)
{
    dpy_fill(win, 0, 0, ww, wh, 2);
    for (int i = 0; i < 3; i++) {
        char v[48];
        text(8, gy(i) + 3, 1, 2, label[i]);
        int on = i == row;
        dpy_fill(win, GX, gy(i), ww - GX - 8, lh + 4, on ? 4 : 5);
        box(GX, gy(i), ww - GX - 8, lh + 4, 7, 1);
        value(i, v, sizeof v);
        text(GX + 6, gy(i) + 2, on ? 7 : 1, on ? 4 : 5, "\xc2\xab");    /* the cycle mark */
        text(GX + 18, gy(i) + 2, on ? 7 : 1, on ? 4 : 5, v);
    }
    for (int i = 0; i < 4; i++) {
        dpy_fill(win, bx(i), by(), bw(), lh + 6, 5);
        box(bx(i), by(), bw(), lh + 6, 7, 1);
        int tw = dpy_textw(&uf, button[i], (int)strlen(button[i]));
        text(bx(i) + (bw() - tw) / 2, by() + 3, 1, 5, button[i]);
    }
    dpy_fill(win, 8, by() + lh + 10, ww - 16, lh, 2);
    if (msg[0])
        text(8, by() + lh + 10, 9, 2, msg);
    dpy_flush();
}
static void layout(void)
{
    dpy_font(DPYF_UI, &uf);
    lh = uf.height > 8 ? uf.height : 8;
    ww = 300;
    wh = by() + lh + 6 + 4 + lh + 4;
}
static void where(void)
{
    struct dpyinfo in;
    if (verbose && dpy_info(win, &in) == 0) {
        dprintf(1, "deskprefs: window %d %d %d %d", in.ix, in.iy, ww, wh);
        for (int i = 0; i < 3; i++)
            dprintf(1, " row%d %d %d", i, in.ix + GX + 20, in.iy + gy(i) + 3);
        for (int i = 0; i < 4; i++)
            dprintf(1, " %s %d %d", button[i], in.ix + bx(i) + bw() / 2, in.iy + by() + 4);
        dprintf(1, "\n");
    }
}
static void cycle(int i, int d)
{
    msg[0] = 0;
    if (i == 0)
        sel.mode ^= 1;
    else if (i == 1)
        sel.font = (sel.font + d + nfonts) % nfonts;
    else if (nthemes)
        sel.theme = (sel.theme + d + nthemes) % nthemes;
    row = i;
}
static void report(const char *what)
{
    char v[3][48];
    for (int i = 0; i < 3; i++)
        value(i, v[i], sizeof v[i]);
    dprintf(1, "deskprefs: %s: mode %s, font %s, colours %s%s%s\n", what, v[0], v[1], v[2], msg[0] ? ": " : "", msg);
}
/* a button -> 1: the window closes */
static int press(int b)
{
    if (b == 3) {                       /* Cancel: back to the start */
        struct choice c = was;
        msg[0] = 0;
        c.theme = now.theme;            /* the theme by its own file, as it was */
        apply(&c);
        if (now.theme != was.theme) {
            theme_use(use_was);
            tell_zdesk();
        }
        report("cancel");
        return 1;
    }
    if (apply(&sel) < 0) {
        report(button[b]);
        draw();
        return 0;
    }
    if (b == 2) {
        char path[ZPATH_MAX];
        int r = save_path("screen", path) ? save_screen(path, sel.mode, sel.font) : -ENOENT;
        if (r >= 0 && save_path("theme", path)) {
            char l[20];
            strcpy(l, themes[sel.theme]);
            strcat(l, "\n");
            r = cfg_write(path, l);
        }
        if (r < 0) {
            snprintf(msg, sizeof msg, "Not saved: %s", zstrerror(r) ? zstrerror(r) : "cannot write");
            report("save");
            draw();
            return 0;
        }
        theme_use("");                  /* the saved theme is the session's now */
        tell_zdesk();
    }
    report(b == 0 ? "test" : b == 1 ? "use" : "save");
    if (b == 0) {
        draw();
        return 0;
    }
    return 1;
}
static int click(int x, int y, int shift)
{
    for (int i = 0; i < 3; i++)
        if (x >= GX && x < ww - 8 && y >= gy(i) && y < gy(i) + lh + 4) {
            cycle(i, shift ? -1 : 1);
            draw();
            return 0;
        }
    for (int i = 0; i < 4; i++)
        if (x >= bx(i) && x < bx(i) + bw() && y >= by() && y < by() + lh + 6)
            return press(i);
    return 0;
}
static int key(const struct dpyev *e)
{
    if (e->code & 0x80)
        return 0;
    switch (e->code) {
    case 0x4C: row = (row + 2) % 3; draw(); return 0;
    case 0x4D: row = (row + 1) % 3; draw(); return 0;
    case 0x4E: cycle(row, 1); draw(); return 0;
    case 0x4F: cycle(row, -1); draw(); return 0;
    case 0x44: return press(1);
    case 0x45: return press(3);
    }
    switch (e->ch[0] | 0x20) {
    case 't': return press(0);
    case 'u': return press(1);
    case 's': return press(2);
    case 'c': return press(3);
    }
    return 0;
}

static const char USAGE[] = "usage: deskprefs [-v]\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (argv[i][0] == '-') zbadopt(argv[i], USAGE);
        else zusage(USAGE);
    int r = dpy_open("deskprefs");
    if (r < 0) {
        dprintf(2, "deskprefs: the desktop: %s\n", zstrerror(r) ? zstrerror(r) : "not reachable");
        return 1;
    }
    fonts_scan();
    themes_scan();
    /* what is in effect: the server's mode and font, zdesk's theme (the session's file, else the saved one) */
    was.mode = dpy_mode(DPYM_ASK) == DPYM_LACE;
    dpy_font(DPYF_UI, &uf);
    was.font = 0;
    if (!(uf.flags & DPYFF_BUILTIN) && uf.name[0]) {
        char nm[DPY_FONTNAME];
        strncpy(nm, uf.name, sizeof nm - 1);
        nm[sizeof nm - 1] = 0;
        char *sp = strrchr(nm, ' ');
        if (sp) {
            *sp = 0;
            font_add(nm, atoi(sp + 1));
            was.font = font_find(nm, atoi(sp + 1));
            if (was.font < 0)
                was.font = 0;
        }
    }
    char path[ZPATH_MAX], t[16] = "flat";
    if (save_path("theme.use", path) && first_word(path, use_was) == 0)
        strcpy(t, use_was);
    else {
        use_was[0] = 0;
        if (!(home() && strlen(home()) < 160 && (strcpy(path, home()), strcat(path, "/.config/zdesk/theme"),
                                                 first_word(path, t) == 0)))
            first_word("/etc/zdesk/theme", t);
    }
    was.theme = theme_find(t);
    now = sel = was;
    layout();
    win = dpy_window(WK_NORMAL, "Desktop preferences", -1, -1, ww, wh, 0);
    if (win < 0) {
        dprintf(2, "deskprefs: no window: %s\n", zstrerror(win) ? zstrerror(win) : "refused");
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
                dpy_resize(win, ww, wh);
            }
            draw();
            where();
            break;
        case EV_BUTTON:
            if ((e.code & 0x7F) == 1 && !(e.code & 0x80))
                done = click(e.x, e.y, e.qual & (GQ_LSHIFT | GQ_RSHIFT));
            break;
        case EV_KEY:
            done = key(&e);
            break;
        case EV_CLOSE:
            press(3);
            done = 1;
            break;
        }
    }
    dpy_close();
    return 0;
}
