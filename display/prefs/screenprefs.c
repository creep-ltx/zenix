/* SPDX-License-Identifier: GPL-2.0-only */
/* screenprefs: the screen's settings, the console's and the desktop's (DESIGN D62, docs/d62-screen-modes.md,
 * docs/man/screenprefs.txt). One engine, three faces, as Linux (the kernel's modes under xrandr and a Displays panel)
 * and AmigaOS (one ScreenMode editor) do it:
 *   - a window on the desktop (display/lib/prefswin.c, as mouseprefs), when it runs under the desktop;
 *   - a text form on the screen of a console (user/zscr.c), when it runs on one (or with -T);
 *   - a command line for scripts: --list, video, console, desktop, rtg (as xrandr).
 * The settings: the video standard (PAL, NTSC: one beam for both), the console's mode, colours and Unicode font
 * (every console's, root's), the desktop's mode, depth, font and colours (its theme; a user's own, root's the
 * system's default), and a graphics card's size (zenixboot RTGMODE, at the next start). AmigaOS's four buttons:
 *   Test    applied now where this face's screen is (the form: the console; the window: the desktop); the rest is
 *           kept for Save. A new mode or video standard asks to be kept, and goes back by itself after 10 s
 *   Use     the same, then the program ends (the console until the next boot, the desktop until Log out)
 *   Save    applied, and written: /etc/screen (root: the system's and the desktop's default), a user's
 *           ~/.config/screen/screen; the theme as zdesk's theme file
 *   Cancel  everything back as it was
 * Before D62, conprefs (/etc/console's colours, font, mode) and deskprefs (/etc/zdesk/screen, ~/.config/zdesk/screen):
 * both are other names of this program now; their files are read when the new ones lack those lines (said once), and
 * Save moves the lines over. Cold code: -Os. */
#include "libz.h"
#include "../display.h"
#include "../screenfile.h"
#include "../prefswin.h"
#include "../keys.h"
#include "../server/amifont.h"
#include "cfgfile.h"
typedef unsigned char u8;

#define FONTDIR "/usr/share/fonts/amiga"
#define TTFDIR "/usr/share/fonts/truetype"
#define CFONTDIR "/usr/share/consolefonts"
#define RUNFILE "/var/run/console"
#define KEEP_S 10                       /* the fall-back: seconds to keep a new mode */

/* ---- the rows ---- */
enum { R_VIDEO, R_CMODE, R_CCOL, R_CFONT, R_DMODE, R_DDEPTH, R_DFONT, R_THEME, R_RTG, NR };
static const char *const rlabel[NR] = { "Video", "Console mode", "Console colours", "Console font", "Desktop mode",
                                        "Desktop depth", "Desktop font", "Desktop colours", "Card size" };
static const char *const ckey[NR] = { "video", "console", "console-colours", "console-font", "desktop",
                                      "desktop-depth", "desktop-font", 0, "rtg" };
static int rows[NR], nrows;             /* the rows shown, in order */
static int verbose, text_face, quiet_old;

/* the lists */
static const char *const mname[5] = { "hires", "hires-lace", "rtg", "superhires", "superhires-lace" };
static const int ccols[5] = { 0, 2, 4, 8, 16 };
#define NCF 16
static char cfonts[NCF][24] = { "auto", "none" };
static int ncfonts = 2;
#define NF 128
static struct { char name[DPY_FONTNAME]; int size; } fonts[NF];  /* 0: the 8 x 8 */
static int nfonts = 1, fdropped;
#define NT 16
static char themes[NT][16];
static int nthemes;
static const short rtgs[][3] = { { 0, 0, 0 }, { 640, 480, 16 }, { 800, 600, 16 }, { 1024, 768, 16 },
    { 1280, 720, 16 }, { 1280, 960, 16 }, { 1280, 1024, 16 }, { 1920, 1080, 16 }, { 640, 480, 32 }, { 800, 600, 32 },
    { 1024, 768, 32 }, { 1280, 720, 32 }, { 1280, 960, 32 }, { 1280, 1024, 32 }, { 1920, 1080, 32 } };
#define NRTG (int)(sizeof rtgs / sizeof rtgs[0])

/* the machine (CON_MODE's CONM_INFO; 0 from an older kernel: hires and interlace only, the standard not known) */
static int info = -1, has_info;
static int aga(void) { return has_info && (info & CONMI_AGA); }
static int beam(void) { return has_info && (info & CONMI_BEAM); }
static int shown_ntsc(void) { return has_info ? !!(info & CONM_NTSC) : 0; }
static int crystal_ntsc(void) { return has_info && (info & CONMI_NTSCX); }

/* the desktop: reachable (displayd runs and took us as a client), and on a card */
static int dpy_ok, on_card;
static struct prefswin pw;

/* a choice: each row's value (an index into its list, or the value itself where noted) */
struct choice {
    int v[NR];                          /* video 0 auto, 1 PAL, 2 NTSC; console 0/1; colours an index of ccols;
                                         * fonts, themes, rtg an index; desktop a DPYM_*; depth 0 auto, 3, 4 */
};
static struct choice was, now, sel;     /* in effect at the start (Cancel's), applied now, shown */
static char run_was[160];               /* /var/run/console at the start ("": none) */
static char use_was[16];                /* the session's theme file at the start ("": none) */
static char msg[80];
static int row;                         /* the row chosen (an index into rows[]) */
static char moved[2][ZPATH_MAX];        /* old files read for lack of the new ones (said once; Save moves them) */
static int nmoved;

static int root(void) { return sys_getuid() == 0; }
static const char *home(void) { const char *h = getenv("HOME"); return h && *h && strlen(h) < 160 ? h : 0; }
static void say(const char *s) { strncpy(msg, s, sizeof msg - 1); msg[sizeof msg - 1] = 0; }
static int is_console(int r) { return r <= R_CFONT || r == R_RTG; }    /* the system's (root's) rows */

/* ---- what there is to choose ---- */
static int font_find(const char *name, int size)
{
    for (int i = 1; i < nfonts; i++)
        if (fonts[i].size == size && !strcmp(fonts[i].name, name))
            return i;
    return -1;
}
static void font_add(const char *name, int size)
{
    if (nfonts >= NF)
        fdropped++;
    else if (strlen(name) < DPY_FONTNAME && size > 0 && size <= AF_MAXH && font_find(name, size) < 0) {
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
    /* TrueType fonts (D51): any size, offered in the sizes a desktop uses; a name an Amiga font has is the Amiga
     * font's (displayd reads that first) */
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
        dprintf(2, "screenprefs: %d font sizes past the list's %d are not shown\n", fdropped, NF - 1);
    for (int i = 2; i < nfonts; i++)    /* by name and size (a directory's order is the disk's) */
        for (int j = i; j > 1 && (strcmp(fonts[j - 1].name, fonts[j].name) > 0 ||
                                  (!strcmp(fonts[j - 1].name, fonts[j].name) && fonts[j - 1].size > fonts[j].size)); j--) {
            typeof(fonts[0]) t = fonts[j];
            fonts[j] = fonts[j - 1];
            fonts[j - 1] = t;
        }
}
static int cfont_find(const char *f)
{
    for (int i = 0; i < ncfonts; i++)
        if (!strcmp(cfonts[i], f))
            return i;
    if (ncfonts < NCF && strlen(f) < 24 && !strchr(f, '/')) {
        strcpy(cfonts[ncfonts], f);
        return ncfonts++;
    }
    return 0;
}
static void cfonts_scan(void)
{
    char **names;
    int n = dir_list(CFONTDIR, &names);
    for (int i = 0; i < n && ncfonts < NCF; i++) {
        int l = (int)strlen(names[i]);
        if (l > 4 && l - 4 < 24 && !strcmp(names[i] + l - 4, ".zcf")) {
            names[i][l - 4] = 0;
            cfont_find(names[i]);
        }
    }
    if (n > 0)
        dir_free(names, n);
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
static void themes_scan(void)
{
    char **names;
    int n = dir_list("/etc/zdesk/themes", &names);
    for (int i = 0; i < n && nthemes < NT; i++) {
        int l = (int)strlen(names[i]);
        if (l > 6 && l - 6 < 16 && !strcmp(names[i] + l - 6, ".theme")) {
            names[i][l - 6] = 0;
            theme_find(names[i]);
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
    if (!nthemes)
        theme_find("flat");
}
static int rtg_find(int w, int h, int b)
{
    for (int i = 0; i < NRTG; i++)
        if (rtgs[i][0] == w && rtgs[i][1] == h && rtgs[i][2] == (b ? b : 16))
            return i;
    return 0;                           /* a size not in the list: "front" (said by --list) */
}
/* ---- the files ---- */
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
/* the value of a setting's line ("font auto") in a file's text, into v (24 bytes) -> 1 found */
static int setting(const char *text, const char *key, char *v)
{
    u32 kl = strlen(key);
    for (const char *l = text; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : 0) {
        while (*l == ' ' || *l == '\t')
            l++;
        if (strncmp(l, key, kl) || (l[kl] != ' ' && l[kl] != '\t'))
            continue;
        l += kl;
        while (*l == ' ' || *l == '\t')
            l++;
        int k = 0;
        while (l[k] && l[k] != ' ' && l[k] != '\t' && l[k] != '\n' && l[k] != '\r' && k < 23)
            k++;
        memcpy(v, l, (u32)k);
        v[k] = 0;
        return k > 0;
    }
    return 0;
}
/* a user's file under $HOME (its folder made when asked), into buf; 0: no home */
static const char *user_path(const char *rel, char *buf, int mk)
{
    if (!home())
        return 0;
    strcpy(buf, home());
    strcat(buf, "/");
    strcat(buf, rel);
    if (mk) {                           /* each folder on the way */
        for (char *p = buf + strlen(home()) + 1; (p = strchr(p, '/')); *p++ = '/') {
            *p = 0;
            sys_mkdir(buf);
        }
    }
    return buf;
}
static void note_moved(const char *path)
{
    if (nmoved < 2)
        strcpy(moved[nmoved++], path);
}
/* the saved settings: the system's /etc/screen (else /etc/console's and /etc/zdesk/screen's old lines), the user's
 * ~/.config/screen/screen (else ~/.config/zdesk/screen) over its desktop lines */
static struct dpyscreen sys_f, user_f;
static int user_has;
static void read_files(void)
{
    char path[ZPATH_MAX];
    dpy_screenfile(SCREEN_SYS, &sys_f, "screenprefs");
    if (!(sys_f.found & SF_CONSOLE)) {  /* conprefs' lines (D28 22) */
        char *t = zslurp("/etc/console", 4096, 0), v[24];
        int got = 0;
        if (t) {
            if (setting(t, "colours", v) || setting(t, "colors", v))
                sys_f.colours = !strcmp(v, "auto") ? 0 : atoi(v), got = 1;
            if (setting(t, "font", v))
                strcpy(sys_f.cfont, v), got = 1;
            if (setting(t, "mode", v))
                sys_f.cmode = !strcmp(v, "hires-lace"), got = 1;
            free(t);
        }
        if (got)
            note_moved("/etc/console");
    }
    if (!(sys_f.found & SF_DESKTOP)) {  /* deskprefs' (D38 "Preferences") */
        struct dpyscreen o;
        if (dpy_screenfile(SCREEN_OLD_SYS, &o, "screenprefs") == 0 && (o.found & SF_DESKTOP)) {
            sys_f.mode = o.mode, sys_f.depth = o.depth, sys_f.size = o.size;
            strcpy(sys_f.font, o.font);
            sys_f.found |= SF_DESKTOP;
            if (root())
                note_moved(SCREEN_OLD_SYS);
        }
    }
    user_f = sys_f;
    if (!root() && user_path(SCREEN_USER, path, 0) && dpy_screenfile(path, &user_f, "screenprefs") == 0 &&
        (user_f.found & SF_DESKTOP))
        user_has = 1;
    else if (!root() && user_path(SCREEN_OLD_USER, path, 0) && dpy_screenfile(path, &user_f, "screenprefs") == 0 &&
             (user_f.found & SF_DESKTOP)) {
        user_has = 1;
        note_moved(path);
    }
    if (!user_has)
        user_f = sys_f;
}
/* the theme's file: the user's ~/.config/zdesk/NAME (made if need be), or root's /etc/zdesk/NAME (theme.use always
 * the user's) */
static const char *theme_path(const char *name, char *buf)
{
    if (root() && strcmp(name, "theme.use")) {
        strcpy(buf, "/etc/zdesk/");
        strcat(buf, name);
        return buf;
    }
    char rel[40] = ".config/zdesk/";
    strcat(rel, name);
    return user_path(rel, buf, 1);
}
/* the choice's lines as the files hold them */
static void line_of(int r, const struct choice *c, char *b, int n)
{
    int v = c->v[r];
    switch (r) {
    case R_VIDEO: snprintf(b, (u32)n, "video %s", v == 1 ? "pal" : v == 2 ? "ntsc" : "auto"); break;
    case R_CMODE: snprintf(b, (u32)n, "console %s", v ? "hires-lace" : "hires"); break;
    case R_CCOL:
        if (ccols[v]) snprintf(b, (u32)n, "console-colours %d", ccols[v]);
        else snprintf(b, (u32)n, "console-colours auto");
        break;
    case R_CFONT: snprintf(b, (u32)n, "console-font %s", cfonts[v]); break;
    case R_DMODE: snprintf(b, (u32)n, "desktop %s", v == DPYM_RTG ? "hires" : mname[v]); break;
    case R_DDEPTH:
        if (v) snprintf(b, (u32)n, "desktop-depth %d", v);
        else snprintf(b, (u32)n, "desktop-depth auto");
        break;
    case R_DFONT:
        if (v) snprintf(b, (u32)n, "desktop-font %s %d", fonts[v].name, fonts[v].size);
        else snprintf(b, (u32)n, "desktop-font builtin");
        break;
    case R_RTG:
        if (v) snprintf(b, (u32)n, "rtg %dx%dx%d", rtgs[v][0], rtgs[v][1], rtgs[v][2]);
        else snprintf(b, (u32)n, "rtg front");
        break;
    default: b[0] = 0;
    }
}
/* the moved file: its lines that now live in /etc/screen dropped (a comment and the rest kept); a user's old
 * desktop file, and root's /etc/zdesk/screen, removed */
static void move_done(const char *path)
{
    if (strcmp(path, "/etc/console")) {
        sys_unlink(path);
        return;
    }
    char *t = zslurp(path, 4096, 0), *o = t ? malloc(strlen(t) + 80) : 0;
    if (!o) {
        free(t);
        return;
    }
    o[0] = 0;
    for (char *l = t, *e; *l; l = e) {
        e = strchr(l, '\n');
        e = e ? e + 1 : l + strlen(l);
        char *w = l;
        while (*w == ' ' || *w == '\t')
            w++;
        int wl = 0;
        while (w[wl] && w[wl] != ' ' && w[wl] != '\t' && w[wl] != '\n')
            wl++;
        int k = (wl == 7 && (!strncmp(w, "colours", 7) || !strncmp(w, "colors ", 7))) || (wl == 6 && !strncmp(w, "colors", 6)) ||
                (wl == 4 && (!strncmp(w, "font", 4) || !strncmp(w, "mode", 4)));
        if (!k) {
            u32 ol = strlen(o);
            memcpy(o + ol, l, (u32)(e - l));
            o[ol + (u32)(e - l)] = 0;
        }
    }
    strcat(o, "# colours, font and mode: in /etc/screen since screenprefs (man screenprefs)\n");
    cfg_write(path, o);
    free(o);
    free(t);
}
/* Save's files -> 0, or -errno */
static int save(const struct choice *c)
{
    char path[ZPATH_MAX], l[NR][48];
    const char *keys[NR], *lines[NR];
    int n = 0, r, con = 0, desk = 0, theme = 0;
    for (int i = 0; i < nrows; i++) {
        int k = rows[i];
        theme |= k == R_THEME;
        if (!ckey[k] || (!root() && is_console(k)) || (on_card && (k == R_DMODE || k == R_DDEPTH)))
            continue;                   /* (a card's desktop: the file's chipset mode stays) */
        con += k == R_CMODE || k == R_CCOL || k == R_CFONT;
        desk += k == R_DMODE || k == R_DFONT;
        line_of(k, c, l[n], sizeof l[n]);
        keys[n] = ckey[k];
        lines[n] = l[n];
        n++;
    }
    if (root())
        r = cfg_set(SCREEN_SYS, keys, lines, n);
    else
        r = user_path(SCREEN_USER, path, 1) ? cfg_set(path, keys, lines, n) : -ENOENT;
    if (r < 0)
        return r;
    for (int i = 0; i < nmoved; i++)    /* an older file only once the lines that replace it are written */
        if (strcmp(moved[i], "/etc/console") ? desk == 2 : con == 3) {
            move_done(moved[i]);
            moved[i][0] = 0;
        }
    int k = 0;
    for (int i = 0; i < nmoved; i++)
        if (moved[i][0])
            memmove(moved[k++], moved[i], sizeof moved[0]);
    nmoved = k;
    if (theme && theme_path("theme", path)) {  /* (the command line's saves leave the theme alone) */
        char t[20];
        strcpy(t, themes[c->v[R_THEME]]);
        strcat(t, "\n");
        r = cfg_write(path, t);
    }
    return r;
}

/* ---- applying ---- */
static int auto_colours(void)           /* the kernel's own choice at boot: 16 with fast RAM (8 on OCS/ECS) */
{
    u32 mi[4];
    return sys_meminfo(mi) == 0 && mi[1] ? 16 : 8;
}
static const char *why(int r) { return zstrerror(r) ? zstrerror(r) : "refused"; }
static int want_ntsc(int v) { return v == 2 || (v == 0 && crystal_ntsc()); }
static int set_cmode(int video, int lace)
{
    int m = lace | (has_info ? (want_ntsc(video) ? CONM_NTSC : CONM_PAL) : 0), r = sys_console(CON_MODE, m);
    if (r == -EBUSY)
        say("The desktop has the screen: the console's mode is saved only");
    else if (r == -ENODEV)
        say("This Agnus shows only its own video standard");
    else if (r == -ENOMEM)
        say("Not enough memory for that mode");
    else if (r < 0)
        snprintf(msg, sizeof msg, "The console's mode: %s", why(r));
    else if (has_info)
        info = sys_console(CON_MODE, CONM_INFO);
    return r == -EBUSY ? 1 : r < 0 ? -1 : 0;    /* 1: not applied, kept for Save (the desktop holds the beam) */
}
static int set_colours(int i)
{
    int c = ccols[i] ? ccols[i] : auto_colours(), r = sys_console(CON_COLOURS, c);
    if (r == -EBUSY)
        say("The desktop has the screen: the colours are saved only");
    else if (r == -ENOMEM)
        snprintf(msg, sizeof msg, "Not enough chip RAM for %d colours", c);
    else if (r < 0)
        snprintf(msg, sizeof msg, "Colours %d: %s", c, why(r));
    else if (r != c)
        snprintf(msg, sizeof msg, "This machine shows %d colours", r);
    return r == -EBUSY ? 1 : r < 0 ? -1 : 0;
}
static int set_cfont(int i)
{
    if (!strcmp(cfonts[i], "none")) {
        int r = sys_conufont(0, 0);
        if (r < 0)
            snprintf(msg, sizeof msg, "The font: %s", why(r));
        return r < 0 ? -1 : 0;
    }
    char p[80];
    snprintf(p, sizeof p, "%s/%s.zcf", CFONTDIR, i == 0 ? "zenix-8x8" : cfonts[i]);
    struct ustat st;
    int fd = sys_open(p, O_RDONLY), r = fd;
    char *b = 0;
    if (fd >= 0) {
        if (sys_fstat(fd, &st) < 0 || st.size < 544 || st.size > 98848)
            r = -EINVAL;
        else if (!(b = malloc(st.size)))
            r = -ENOMEM;
        else if (sys_read(fd, b, st.size) != (int)st.size)
            r = -EIO;
        else
            r = sys_conufont(b, st.size);
        sys_close(fd);
    }
    if (r < 0)
        snprintf(msg, sizeof msg, "%s: %s", p, r == -ENOENT ? "not there" : r == -EINVAL ?
                 "not a console font (ZCF1)" : why(r));
    free(b);
    return r < 0 ? -1 : 0;
}
/* zdesk reads its theme again on SIGHUP: every process called zdesk that this user may signal */
static void tell_zdesk(void)
{
    struct uproc p;
    for (u32 k = 0; k < 256; k++)
        if (sys_procinfo(k, &p) == 0 && p.state && p.state != 5 && !strcmp(p.name, "zdesk"))
            sys_killsig((int)p.pid, SIGHUP);
}
static void theme_use(const char *t)
{
    char path[ZPATH_MAX];
    if (!theme_path("theme.use", path)) {
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
static int live_console(void) { return text_face; }    /* each face applies its own screen's rows */
static int live_desktop(void) { return !text_face && dpy_ok; }
/* the rows of c that this face applies, in effect -> 0, or -1 with the reason in msg (what did work stays) */
static int apply(const struct choice *c)
{
    int r;
    if (live_console() && root()) {
        if (c->v[R_VIDEO] != now.v[R_VIDEO] || c->v[R_CMODE] != now.v[R_CMODE]) {
            int shown = shown_ntsc();   /* the mode first: the colours' screen is then made for it once */
            int r2 = want_ntsc(c->v[R_VIDEO]) != shown || c->v[R_CMODE] != now.v[R_CMODE] ?
                     set_cmode(c->v[R_VIDEO], c->v[R_CMODE]) : 0;
            if (r2 < 0)
                return -1;
            if (r2 == 0) {
                now.v[R_VIDEO] = c->v[R_VIDEO];
                now.v[R_CMODE] = c->v[R_CMODE];
            }
        }
        if (c->v[R_CCOL] != now.v[R_CCOL]) {
            int r2 = set_colours(c->v[R_CCOL]);
            if (r2 < 0)
                return -1;
            if (r2 == 0)
                now.v[R_CCOL] = c->v[R_CCOL];
        }
        if (c->v[R_CFONT] != now.v[R_CFONT]) {
            if (set_cfont(c->v[R_CFONT]) < 0)
                return -1;
            now.v[R_CFONT] = c->v[R_CFONT];
        }
    }
    if (live_desktop()) {
        if (!on_card && (c->v[R_DMODE] != now.v[R_DMODE] || c->v[R_DDEPTH] != now.v[R_DDEPTH])) {
            int d = c->v[R_DDEPTH] ? c->v[R_DDEPTH] : aga() ? 4 : 3;
            if ((r = dpy_mode2(c->v[R_DMODE], d)) < 0) {
                say(r == -ENOMEM ? "Not enough chip RAM for that mode" : r == -EBUSY ? "The screen is busy: try again"
                    : "The mode was refused");
                return -1;
            }
            now.v[R_DMODE] = c->v[R_DMODE];
            now.v[R_DDEPTH] = c->v[R_DDEPTH];
        }
        int f = c->v[R_DFONT];
        if (f != now.v[R_DFONT]) {
            if ((r = dpy_fontset(f ? fonts[f].name : 0, fonts[f].size)) < 0) {
                say(r == -ENOENT ? "That font is not there" : r == -ENOMEM ? "No memory for that font"
                    : r == -EBUSY ? "The screen is busy: try again" : "That font cannot be read");
                return -1;
            }
            now.v[R_DFONT] = f;
        }
        if (c->v[R_THEME] != now.v[R_THEME]) {
            theme_use(themes[c->v[R_THEME]]);
            now.v[R_THEME] = c->v[R_THEME];
            tell_zdesk();
        }
    }
    return 0;
}
/* a mode (or the video standard) changed by this face: it must be kept */
static int mode_changed(const struct choice *a, const struct choice *b)
{
    return live_console() ? a->v[R_VIDEO] != b->v[R_VIDEO] || a->v[R_CMODE] != b->v[R_CMODE]
                          : a->v[R_DMODE] != b->v[R_DMODE] || a->v[R_DDEPTH] != b->v[R_DDEPTH];
}

/* ---- mode providers (docs/d62-screen-modes.md 8.1) ----
 * Each display's modes come from one provider: what the hardware behind it shows, as entries the faces and --list
 * use without knowing the hardware. Built in: the chipset (the console's and the desktop's modes, from CON_MODE's
 * CONM_INFO), and a Picasso96 card (zenixboot RTG: the card's size now, the sizes asked of it at the next start). A
 * driver of its own (the Pi's HVS, to come) is one more entry in providers[]: its modes, live or at the next start,
 * written as the same settings lines, so neither the faces nor the files change. */
struct smode {
    int id;                             /* the provider's own number (DPYM_* for the chipset, an rtgs[] index) */
    const char *name;                   /* the settings word ("hires-lace") or 0: WxHxBITS */
    short w, h, bits;                   /* pixels; bits a pixel (a card) or 0 (planes: dmin..dmax) */
    unsigned char dmin, dmax;           /* planes the desktop may have in it (0: the card's own) */
    unsigned char next;                 /* 1: counts from the next start (not live) */
};
struct provider {
    const char *display;                /* the --list word and the setting it writes: "desktop" or "rtg" */
    int (*here)(void);                  /* the machine has it now */
    int (*modes)(struct smode *m, int max);     /* its modes -> how many */
};
static int lines_now(void) { return shown_ntsc() ? 200 : 256; }    /* (the standard shown) */
static int chip_here(void) { return !on_card; }
static int chip_modes(struct smode *m, int max)
{
    int n = 0;
    for (int k = 0; k < 5 && n < max; k++) {
        if (k == DPYM_RTG || (k >= DPYM_SHRES && !aga()))
            continue;                   /* super-hires: AGA (d62 4) */
        m[n].id = k;
        m[n].name = mname[k];
        m[n].w = (short)(k >= DPYM_SHRES ? 1280 : 640);
        m[n].h = (short)(lines_now() << (k == DPYM_LACE || k == DPYM_SHRES_LACE));
        m[n].bits = 0;
        m[n].dmin = 3;
        m[n].dmax = (unsigned char)(aga() ? 4 : 3);
        m[n++].next = 0;
    }
    return n;
}
static int card_here(void) { return on_card || (sys_f.found & SF_RTG); }
static int card_modes(struct smode *m, int max)
{
    int n = 0;
    for (int i = 1; i < NRTG && n < max; i++, n++) {
        m[n].id = i;
        m[n].name = 0;
        m[n].w = rtgs[i][0], m[n].h = rtgs[i][1], m[n].bits = rtgs[i][2];
        m[n].dmin = m[n].dmax = 0;
        m[n].next = 1;
    }
    return n;
}
static const struct provider providers[] = {
    { "desktop", chip_here, chip_modes },
    { "rtg", card_here, card_modes },
};
#define NPROV (int)(sizeof providers / sizeof providers[0])
/* the desktop modes this machine shows, in cycle order */
static int dmodes(int *m)
{
    struct smode t[8];
    if (on_card) {
        m[0] = DPYM_RTG;
        return 1;
    }
    int n = chip_modes(t, 8);
    for (int i = 0; i < n; i++)
        m[i] = t[i].id;
    return n;
}

/* ---- values as text ---- */
static void value(int i, char *b, int n)
{
    int r = rows[i], v = sel.v[r];
    switch (r) {
    case R_VIDEO:
        if (!beam())
            snprintf(b, (u32)n, "%s (this machine's own)", shown_ntsc() ? "NTSC" : "PAL");
        else
            snprintf(b, (u32)n, "%s", v == 1 ? "PAL" : v == 2 ? "NTSC" : crystal_ntsc() ? "auto (NTSC)" : "auto (PAL)");
        break;
    case R_CMODE: {
        int rr = (want_ntsc(sel.v[R_VIDEO]) ? 25 : 32) << v;
        snprintf(b, (u32)n, "%s, 80 x %d", v ? "Hires interlaced" : "Hires", rr);
        break;
    }
    case R_CCOL:
        if (ccols[v]) snprintf(b, (u32)n, "%d", ccols[v]);
        else snprintf(b, (u32)n, "auto (%d)", auto_colours() == 16 && !aga() && has_info ? 8 : auto_colours());
        break;
    case R_CFONT: snprintf(b, (u32)n, "%s", cfonts[v]); break;
    case R_DMODE:
        if (v == DPYM_RTG)
            snprintf(b, (u32)n, "the card's, %u x %u", dpy_w, dpy_h);
        else {
            int h = want_ntsc(sel.v[R_VIDEO]) ? 200 : 256;
            snprintf(b, (u32)n, "%s, %d x %d", v == DPYM_HIRES ? "Hires" : v == DPYM_LACE ? "Hires interlaced" :
                     v == DPYM_SHRES ? "Super-hires" : "Super-hires laced", v >= DPYM_SHRES ? 1280 : 640,
                     h << (v == DPYM_LACE || v == DPYM_SHRES_LACE));
        }
        break;
    case R_DDEPTH:
        if (v) snprintf(b, (u32)n, "%d colours", 1 << v);
        else snprintf(b, (u32)n, "auto (%d colours)", aga() ? 16 : 8);
        break;
    case R_DFONT:
        if (v) snprintf(b, (u32)n, "%s %d", fonts[v].name, fonts[v].size);
        else snprintf(b, (u32)n, "topaz 8 (built in)");
        break;
    case R_THEME: snprintf(b, (u32)n, "%s", themes[v]); break;
    case R_RTG:
        if (v) snprintf(b, (u32)n, "%d x %d, %d bits (next start)", rtgs[v][0], rtgs[v][1], rtgs[v][2]);
        else snprintf(b, (u32)n, "as zenixboot finds it");
        break;
    }
}
static void cycle(int i, int d)
{
    int r = rows[i], *v = &sel.v[r];
    msg[0] = 0;
    if (is_console(r) && !root()) {
        say("The console's settings are root's: sudo screenprefs");
        return;
    }
    switch (r) {
    case R_VIDEO:
        if (!beam()) say("This machine shows only its own video standard");
        else *v = (*v + d + 3) % 3;
        break;
    case R_CMODE: *v ^= 1; break;
    case R_CCOL: *v = (*v + d + 5) % 5; break;
    case R_CFONT: *v = (*v + d + ncfonts) % ncfonts; break;
    case R_DMODE: {
        int m[5], n = dmodes(m), k = 0;
        for (int j = 0; j < n; j++)
            if (m[j] == *v)
                k = j;
        *v = m[(k + d + n) % n];
        break;
    }
    case R_DDEPTH: {
        static const int ds[3] = { 0, 3, 4 };
        int n = aga() ? 3 : 2, k = *v == 3 ? 1 : *v == 4 ? 2 : 0;
        *v = ds[(k + d + n) % n];
        break;
    }
    case R_DFONT: *v = (*v + d + nfonts) % nfonts; break;
    case R_THEME: *v = (*v + d + nthemes) % nthemes; break;
    case R_RTG: *v = (*v + d + NRTG) % NRTG; break;
    }
    row = i;
}
static void report(const char *what)
{
    char l[56];
    int fd = text_face ? 2 : 1;         /* the form's screen is the terminal: its report on the error stream */
    dprintf(fd, "%s: %s:", zprogname(), what);
    for (int i = 0; i < nrows; i++) {
        if (!ckey[rows[i]]) {
            dprintf(fd, "%s theme %s", i ? "," : "", themes[sel.v[R_THEME]]);
            continue;
        }
        line_of(rows[i], &sel, l, sizeof l);
        dprintf(fd, "%s %s", i ? "," : "", l);
    }
    dprintf(fd, "%s%s\n", msg[0] ? ": " : "", msg);
}

/* ---- keys (display/keys.h; ~/.config/screenprefs/keys) ---- */
enum { A_NONE, A_UP, A_DOWN, A_NEXT, A_PREV, A_TEST, A_USE, A_SAVE, A_CANCEL, A_HELP, A_KEEP, A_REVERT, A_REDRAW,
       A_N };
static const char *const aname[A_N] = { "none", "up", "down", "next", "previous", "test", "use", "save", "cancel",
                                        "help", "keep", "revert", "redraw" };
static const char defkeys[] =
    "# screenprefs' keys (docs/man/screenprefs.txt): KEY ACTION, a key a line; <Name> for <Up> <Down> <Left> <Right>\n"
    "# <Return> <Esc> <Tab> <Space> <F1>..<F10> <Help> <C-x>; \"KEY none\" takes a key away. keep and revert answer\n"
    "# the question after a new mode (it goes back by itself after 10 seconds).\n"
    "<Up> up\n<Down> down\n<Tab> down\n<Right> next\n<Space> next\n<Left> previous\nt test\nu use\n<Return> use\n"
    "s save\nc cancel\n<Esc> cancel\nq cancel\n? help\n<F1> help\n<Help> help\ny keep\nk keep\nn revert\nr revert\n"
    "<C-l> redraw\n";
static struct kbind kb[48];
static struct keyset ks = { aname, A_N, kb, 0, 48, "" };
static int act_of(int k)
{
    const struct kbind *b = k > 0 ? keys_find(&ks, k, 0) : 0;
    return b ? b->act : A_NONE;
}

/* ---- the text form (on a console: zscr) ---- */
static int asking;                      /* the fall-back's question is up: its seconds left */
static void t_draw(void)
{
    char v[48], b[96];
    oputs("\033[H\033[2J");
    zscr_bar(1, " Screen preferences");
    int y = 3;
    for (int i = 0; i < nrows; i++) {
        int r = rows[i];
        if (r == R_VIDEO || r == R_CMODE || r == R_DMODE) {
            const char *h = r == R_VIDEO ? "The video standard (the console's and the desktop's)" : r == R_CMODE ?
                            "The console (every console's: root's)" : root() ? "The desktop (the system's default)" :
                            "The desktop (yours: from its next start)";
            if (r != R_VIDEO)
                y++;
            zscr_at(y++, 2);
            oprintf("\033[1m%s\033[m", h);
        }
        value(i, v, sizeof v);
        zscr_at(y++, 4);
        oprintf("%-16s%s< ", rlabel[r], i == row ? "\033[7m" : "");
        zscr_putw(v, 34);
        oputs(" >\033[m");
    }
    y++;
    zscr_at(y, 4);
    if (asking)
        oprintf("\033[7m Keep this mode? y keeps it, n goes back (%d s) \033[m", asking);
    else
        oputs("[ Test ]  [ Use ]  [ Save ]  [ Cancel ]");
    zscr_at(y + 2, 4);
    zscr_putw(msg, zscr_cols - 5);
    snprintf(b, sizeof b, " Up/Down a setting  Left/Right change it  t u s c the buttons  ? keys");
    zscr_status(b);
    oflush();
}
static void t_help(void)
{
    oputs("\033[H\033[2J");
    zscr_bar(1, " screenprefs: the keys (~/.config/screenprefs/keys)");
    int y = 3;
    for (int i = 0; i < ks.n && y < zscr_rows - 1; i++) {
        char k[16];
        if (!ks.b[i].act)
            continue;
        keys_name(ks.b[i].k1, k);
        zscr_at(y++, 4);
        oprintf("%-10s %s", k, aname[ks.b[i].act]);
    }
    zscr_status(" any key: back");
    oflush();
    keys_tty(0);
}
static int t_key(int ms)                /* an action, A_NONE after ms (-1: wait), -1 the end of the input */
{
    if (ms >= 0) {
        struct pollfd p = { 0, POLLIN, 0 };
        if (poll(&p, 1, ms) <= 0)
            return A_NONE;
    }
    int k = keys_tty(0);
    return k < 0 ? -1 : act_of(k);
}

/* ---- the window (display/lib/prefswin.c) ---- */
static const char *wlabel[NR];
static void w_draw(void)
{
    pw.row = row;
    strncpy(pw.msg, msg, sizeof pw.msg - 1);
    pw.msg[sizeof pw.msg - 1] = 0;
    pw_draw(&pw);
    if (asking) {                       /* the question: Keep on the first button, Revert on the last */
        static const char *const q[4] = { "Keep", "", "", "Back" };
        for (int i = 0; i < 4; i++) {
            dpy_fill(pw.win, pw_bx(&pw, i) + 2, pw_by(&pw) + 2, pw_bw(&pw) - 4, pw.lh + 2, 5);
            int tw = dpy_textw(&pw.uf, q[i], (int)strlen(q[i]));
            pw_text(&pw, pw_bx(&pw, i) + (pw_bw(&pw) - tw) / 2, pw_by(&pw) + 3, 1, 5, q[i]);
        }
        char t[64];
        snprintf(t, sizeof t, "Keep this mode? Back by itself in %d s", asking);
        dpy_fill(pw.win, 8, pw_by(&pw) + pw.lh + 10, pw.ww - 16, pw.lh, 2);
        pw_text(&pw, 8, pw_by(&pw) + pw.lh + 10, 9, 2, t);
    }
    dpy_flush();
}
static void w_layout(void) { pw_layout(&pw, 400); }
/* the window's next action: a key, a click (on a row: cycled here; a button), the close gadget; A_NONE after ms */
static int w_click_row = -1, w_shift;
static int w_next(int ms, int *quit)
{
    struct dpyev e;
    int got = dpy_next(&e, ms < 0 ? -1 : (ms + 9) / 10);
    if (got < 0) {
        *quit = 1;
        return A_CANCEL;
    }
    if (!got)
        return A_NONE;
    switch (e.type) {
    case EV_EXPOSE:
    case EV_RESIZE:
        w_draw();
        return A_NONE;
    case EV_SCREEN:
        if (e.code & DPYS_FONT) {
            w_layout();
            dpy_resize(pw.win, pw.ww, pw.wh);
        }
        w_draw();
        if (verbose)
            pw_where(&pw, zprogname());
        return A_NONE;
    case EV_CLOSE:
        return asking ? A_REVERT : A_CANCEL;
    case EV_KEY:
        if (e.code & 0x80)
            return A_NONE;
        return act_of(keys_code(&e));
    case EV_BUTTON: {
        if ((e.code & 0x7F) != 1 || (e.code & 0x80))
            return A_NONE;
        int h = pw_hit(&pw, e.x, e.y);
        if (h >= 100)
            return asking ? (h == 100 ? A_KEEP : h == 103 ? A_REVERT : A_NONE) : A_TEST + h - 100;
        if (h >= 0 && !asking) {
            w_click_row = h;
            w_shift = e.qual & (GQ_LSHIFT | GQ_RSHIFT);
            return A_NEXT;
        }
        return A_NONE;
    }
    }
    return A_NONE;
}

/* ---- the faces' loop ---- */
static void draw(void) { if (text_face) t_draw(); else w_draw(); }
static int next(int ms, int *quit)
{
    if (!text_face)
        return w_next(ms, quit);
    if (zscr_resized())
        t_draw();
    int a = t_key(ms);
    if (a < 0) {
        *quit = 1;
        return A_CANCEL;
    }
    return a;
}
/* after a new mode: kept (1) on an answer, or back (0) on "revert" or after KEEP_S seconds */
static int keep_q(void)
{
    int quit = 0;
    u32 t0 = sys_ticks();
    for (asking = KEEP_S; asking > 0;) {
        draw();
        int a = next(250, &quit);
        if (a == A_KEEP)
            break;
        if (a == A_REVERT || quit) {
            asking = 0;
            return 0;
        }
        int left = KEEP_S - (int)((sys_ticks() - t0) / 100);
        if (left <= 0) {
            asking = 0;
            say("Not kept: back to the mode before");
            return 0;
        }
        asking = left;
    }
    asking = 0;
    return 1;
}
static const char *const bname[4] = { "test", "use", "save", "cancel" };
/* a button -> 1: the program ends */
static int press(int b)
{
    msg[0] = 0;
    if (b == 3) {                       /* Cancel: everything back as it was at the start */
        struct choice c = was;
        c.v[R_THEME] = now.v[R_THEME];  /* the theme by its own file, as it was */
        apply(&c);
        if (live_desktop() && now.v[R_THEME] != was.v[R_THEME]) {
            theme_use(use_was);
            tell_zdesk();
        }
        if (live_console() && root()) {
            if (run_was[0])
                cfg_write(RUNFILE, run_was);
            else
                sys_unlink(RUNFILE);
        }
        report("cancel");
        return 1;
    }
    struct choice before = now;
    if (apply(&sel) < 0) {
        report(bname[b]);
        draw();
        return 0;
    }
    if (mode_changed(&before, &now) && !keep_q()) {
        struct choice back = before;    /* back as it was before this button, the rest kept */
        back.v[R_THEME] = now.v[R_THEME];
        back.v[R_DFONT] = now.v[R_DFONT];
        back.v[R_CCOL] = now.v[R_CCOL];
        back.v[R_CFONT] = now.v[R_CFONT];
        char m[80];
        strcpy(m, msg);
        apply(&back);
        sel.v[R_VIDEO] = now.v[R_VIDEO], sel.v[R_CMODE] = now.v[R_CMODE];
        sel.v[R_DMODE] = now.v[R_DMODE], sel.v[R_DDEPTH] = now.v[R_DDEPTH];
        say(m[0] ? m : "Not kept: back to the mode before");
        report("reverted");
        draw();
        return 0;
    }
    int r = 0;
    if (b == 2) {
        if ((r = save(&sel)) < 0)
            snprintf(msg, sizeof msg, "Not saved: %s", why(r));
        else if (live_console() && root())
            sys_unlink(RUNFILE);        /* the saved settings are the session's now */
        if (r >= 0 && live_desktop()) {
            theme_use("");
            tell_zdesk();
        }
    } else if (live_console() && root()) {  /* the session's console (conprefs' /var/run/console, D28 22) */
        char t[160], c[48], f[48], m[48];
        int v = sel.v[R_VIDEO];
        line_of(R_CCOL, &sel, c, sizeof c);
        line_of(R_CFONT, &sel, f, sizeof f);
        line_of(R_CMODE, &sel, m, sizeof m);
        snprintf(t, sizeof t, "colours %s\nfont %s\nmode %s\nvideo %s\n", c + 16, f + 13, m + 8,
                 v == 1 ? "pal" : v == 2 ? "ntsc" : "auto");
        sys_mkdir("/var/run");
        cfg_write(RUNFILE, t);
    }
    if (!msg[0]) {
        int other = 0;
        for (int i = 0; i < nrows; i++)
            other |= sel.v[rows[i]] != was.v[rows[i]] && (live_console() ? !is_console(rows[i]) || rows[i] == R_RTG
                                                                            : is_console(rows[i]));
        if (other)
            say(b == 2 ? "Saved; the other screen's settings count from its next start"
                       : "The other screen's settings: only Save keeps them");
    }
    report(b == 0 ? "test" : b == 1 ? "use" : "save");
    if (b == 0 || r < 0) {
        draw();
        return 0;
    }
    return 1;
}
static void run(void)
{
    draw();
    if (!text_face && verbose)
        pw_where(&pw, zprogname());
    for (int quit = 0, done = 0; !done && !quit;) {
        int a = next(-1, &quit);
        switch (a) {
        case A_UP: row = (row + nrows - 1) % nrows; msg[0] = 0; break;
        case A_DOWN: row = (row + 1) % nrows; msg[0] = 0; break;
        case A_NEXT:
        case A_PREV:
            if (w_click_row >= 0) {
                cycle(w_click_row, w_shift ? -1 : 1);
                w_click_row = -1;
            } else
                cycle(row, a == A_NEXT ? 1 : -1);
            break;
        case A_TEST: case A_USE: case A_SAVE: case A_CANCEL:
            done = press(a - A_TEST);
            break;
        case A_HELP:
            if (text_face)
                t_help();
            else
                say("Keys: Up/Down, Left/Right, t u s c; ~/.config/screenprefs/keys");
            break;
        case A_NONE:
            continue;
        }
        if (!done)
            draw();
    }
}

/* ---- what is in effect ---- */
static int displayd_runs(void)
{
    struct uproc p;
    for (u32 k = 0; k < 256; k++)
        if (sys_procinfo(k, &p) == 0 && p.state && p.state != 5 && !strcmp(p.name, "displayd"))
            return 1;
    return 0;
}
static void in_effect(void)
{
    char v[24];
    info = sys_console(CON_MODE, CONM_INFO);
    has_info = info >= 0;
    if (!has_info)
        info = 0;
    /* the console: the kernel's mode and colours; the font of this session's Use, else the saved one */
    int m = sys_console(CON_MODE, CONM_ASK);
    was.v[R_CMODE] = m == CONM_LACE;
    was.v[R_VIDEO] = sys_f.video;
    int shown = sys_console(CON_COLOURS, 0);
    was.v[R_CCOL] = 0;
    for (int i = 1; i < 5; i++)
        if (ccols[i] == shown && ccols[i] == sys_f.colours)
            was.v[R_CCOL] = i;
    if (sys_f.colours && !was.v[R_CCOL])
        for (int i = 1; i < 5; i++)
            if (ccols[i] == shown)
                was.v[R_CCOL] = i;
    char *t = zslurp(RUNFILE, sizeof run_was - 1, 0);
    run_was[0] = 0;
    if (t) {
        strcpy(run_was, t);
        free(t);
    }
    if (run_was[0] && setting(run_was, "font", v))
        was.v[R_CFONT] = cfont_find(v);
    else
        was.v[R_CFONT] = cfont_find(sys_f.cfont);
    if (run_was[0] && setting(run_was, "colours", v))  /* a Use's colours, as the font */
        for (int i = 0; i < 5; i++)
            if ((!i && !strcmp(v, "auto")) || (i && atoi(v) == ccols[i]))
                was.v[R_CCOL] = i;
    if (run_was[0] && setting(run_was, "video", v))
        was.v[R_VIDEO] = !strcmp(v, "pal") ? 1 : !strcmp(v, "ntsc") ? 2 : 0;
    /* the desktop: displayd's when it runs and takes us, else the saved file's */
    struct dpyfont f;
    was.v[R_DMODE] = user_f.mode;
    was.v[R_DDEPTH] = user_f.depth;
    was.v[R_DFONT] = user_f.font[0] ? font_find(user_f.font, user_f.size) : 0;
    if (user_f.font[0] && was.v[R_DFONT] < 0) {
        font_add(user_f.font, user_f.size);
        was.v[R_DFONT] = font_find(user_f.font, user_f.size);
        if (was.v[R_DFONT] < 0)
            was.v[R_DFONT] = 0;
    }
    if (dpy_ok) {
        int md = dpy_mode2(DPYM_ASK, DPYMD_ASK);
        if (md < 0)
            md = dpy_mode(DPYM_ASK) | dpy_depth << 8;
        on_card = (md & 0xFF) == DPYM_RTG;
        was.v[R_DMODE] = md & 0xFF;
        if (md >> 8 != (was.v[R_DDEPTH] ? was.v[R_DDEPTH] : aga() ? 4 : 3))
            was.v[R_DDEPTH] = md >> 8;  /* (a session's Use: the depth shown, not the file's) */
        dpy_font(DPYF_UI, &f);
        was.v[R_DFONT] = 0;
        if (!(f.flags & DPYFF_BUILTIN) && f.name[0]) {
            char nm[DPY_FONTNAME];
            strncpy(nm, f.name, sizeof nm - 1);
            nm[sizeof nm - 1] = 0;
            char *sp = strrchr(nm, ' ');
            if (sp) {
                *sp = 0;
                font_add(nm, atoi(sp + 1));
                was.v[R_DFONT] = font_find(nm, atoi(sp + 1));
                if (was.v[R_DFONT] < 0)
                    was.v[R_DFONT] = 0;
            }
        }
    }
    if (!aga() && (was.v[R_DMODE] == DPYM_SHRES || was.v[R_DMODE] == DPYM_SHRES_LACE))
        was.v[R_DMODE] -= DPYM_SHRES;   /* (displayd shows such a file's mode without its super-hires) */
    /* the theme: the session's file, else the saved one */
    char path[ZPATH_MAX], th[16] = "flat";
    if (theme_path("theme.use", path) && first_word(path, use_was) == 0)
        strcpy(th, use_was);
    else {
        use_was[0] = 0;
        if (!theme_path("theme", path) || first_word(path, th) < 0)
            first_word("/etc/zdesk/theme", th);
    }
    was.v[R_THEME] = theme_find(th);
    was.v[R_RTG] = rtg_find(sys_f.rw, sys_f.rh, sys_f.rbits);
    now = sel = was;
    /* the rows this machine has */
    nrows = 0;
    rows[nrows++] = R_VIDEO;
    rows[nrows++] = R_CMODE;
    rows[nrows++] = R_CCOL;
    rows[nrows++] = R_CFONT;
    rows[nrows++] = R_DMODE;
    if (!on_card)
        rows[nrows++] = R_DDEPTH;
    rows[nrows++] = R_DFONT;
    rows[nrows++] = R_THEME;
    if (on_card || (sys_f.found & SF_RTG))
        rows[nrows++] = R_RTG;
}

/* ---- the command line ---- */
static void list(void)
{
    int std = shown_ntsc(), lines = std ? 200 : 256;
    if (has_info && beam())
        oprintf("video %s (pal ntsc)\n", std ? "ntsc" : "pal");
    else
        oprintf("video %s (this machine's own)\n", has_info ? std ? "ntsc" : "pal" : "unknown");
    for (int m = 0; m < 2; m++)
        oprintf("console %s 640x%d 80x%d%s\n", mname[m], lines << m, (lines >> 3) << m, was.v[R_CMODE] == m ? " *" : "");
    if (on_card)
        oprintf("desktop rtg %ux%u (the card's, zenixboot RTG) *\n", dpy_w, dpy_h);
    for (int p = 0; p < NPROV; p++) {
        struct smode md[NRTG];
        const struct provider *pv = &providers[p];
        int cur = !strcmp(pv->display, "rtg") ? was.v[R_RTG] : was.v[R_DMODE];
        if (!pv->here()) {
            if (!strcmp(pv->display, "rtg"))      /* no card in use, none asked for: the sizes left out */
                oprintf("rtg front * (no graphics card in use)\n");
            continue;
        }
        if (!strcmp(pv->display, "rtg"))
            oprintf("rtg front%s\n", cur ? "" : " *");
        int n = pv->modes(md, NRTG);
        for (int i = 0; i < n; i++) {
            if (md[i].name)
                oprintf("%s %s %dx%d depth %d%s%s\n", pv->display, md[i].name, md[i].w, md[i].h, md[i].dmin,
                        md[i].dmax > md[i].dmin ? ",4" : "", cur == md[i].id ? " *" : "");
            else
                oprintf("%s %dx%dx%d (zenixboot RTGMODE=%dx%dx%d)%s\n", pv->display, md[i].w, md[i].h, md[i].bits,
                        md[i].w, md[i].h, md[i].bits, cur == md[i].id ? " *" : "");
        }
        if (!strcmp(pv->display, "rtg") && sys_f.rw && !cur)
            oprintf("rtg %dx%dx%d (zenixboot RTGMODE=%dx%dx%d) *\n", sys_f.rw, sys_f.rh,
                    sys_f.rbits ? sys_f.rbits : 16, sys_f.rw, sys_f.rh, sys_f.rbits ? sys_f.rbits : 16);
    }
    oflush();
}
static void quiet(void)
{
    char c[48], f[48], m[48];
    line_of(R_CCOL, &was, c, sizeof c);
    line_of(R_CFONT, &was, f, sizeof f);
    if (quiet_old) {                    /* conprefs -q's line (D28 22, frozen) */
        oprintf("conprefs: colours %s, font %s, mode %s\n", c + 16, f + 13, was.v[R_CMODE] ? "hires-lace" : "hires");
        oflush();
        return;
    }
    int v = was.v[R_VIDEO], d = was.v[R_DMODE];
    line_of(R_DFONT, &was, m, sizeof m);
    oprintf("screenprefs: video %s (%s shown), console %s, colours %s, font %s, desktop %s, depth %s%d, %s\n",
            v == 1 ? "pal" : v == 2 ? "ntsc" : "auto", shown_ntsc() ? "ntsc" : "pal",
            was.v[R_CMODE] ? "hires-lace" : "hires", c + 16, f + 13, mname[d], was.v[R_DDEPTH] ? "" : "auto ",
            was.v[R_DDEPTH] ? was.v[R_DDEPTH] : aga() ? 4 : 3, m);
    oflush();
}
static const char USAGE[] =
    "usage: screenprefs [-v] [-T | -W]\n"
    "       screenprefs -l | -q | -K\n"
    "       screenprefs [-s] [-y] video pal|ntsc|auto\n"
    "       screenprefs [-s] [-y] console [hires|hires-lace] [colours auto|2|4|8|16] [font NAME]\n"
    "       screenprefs [-s] [-y] desktop [MODE] [auto|3|4] [font NAME SIZE|builtin]\n"
    "       screenprefs -s rtg front|WIDTHxHEIGHT[xBITS]\n";
static int fail(const char *what, const char *why_)
{
    dprintf(2, "screenprefs: %s: %s\n", what, why_);
    return 1;
}
/* the command line's change: the words after video/console/desktop/rtg into sel -> 0, or a usage error (2) */
static int words(int which, int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        const char *w = argv[i];
        int ok = 0;
        if (which == R_VIDEO && i == 0) {
            ok = !strcmp(w, "pal") || !strcmp(w, "ntsc") || !strcmp(w, "auto");
            sel.v[R_VIDEO] = w[0] == 'p' ? 1 : w[0] == 'n' ? 2 : 0;
        } else if (which == R_CMODE && (!strcmp(w, "hires") || !strcmp(w, "hires-lace"))) {
            sel.v[R_CMODE] = w[5] == '-';
            ok = 1;
        } else if (which == R_CMODE && !strcmp(w, "colours") && i + 1 < argc) {
            w = argv[++i];
            for (int k = 0; k < 5; k++)
                if ((!k && !strcmp(w, "auto")) || (k && atoi(w) == ccols[k]))
                    sel.v[R_CCOL] = k, ok = 1;
        } else if ((which == R_CMODE || which == R_DMODE) && !strcmp(w, "font") && i + 1 < argc) {
            w = argv[++i];
            if (which == R_CMODE)
                ok = strlen(w) < 24 && !strchr(w, '/') && (sel.v[R_CFONT] = cfont_find(w), 1);
            else if (!strcmp(w, "builtin"))
                sel.v[R_DFONT] = 0, ok = 1;
            else if (i + 1 < argc && atoi(argv[i + 1]) > 0 && strlen(w) < DPY_FONTNAME) {
                font_add(w, atoi(argv[++i]));
                int f = font_find(w, atoi(argv[i]));
                ok = f > 0 && (sel.v[R_DFONT] = f, 1);
            }
        } else if (which == R_DMODE && (!strcmp(w, "auto") || !strcmp(w, "3") || !strcmp(w, "4"))) {
            sel.v[R_DDEPTH] = w[0] == 'a' ? 0 : w[0] - '0';
            ok = 1;
        } else if (which == R_DMODE) {
            for (int m = 0; m < 5; m++)
                if (m != DPYM_RTG && !strcmp(w, mname[m]))
                    sel.v[R_DMODE] = m, ok = 1;
        } else if (which == R_RTG && i == 0) {
            if (!strcmp(w, "front"))
                sel.v[R_RTG] = 0, ok = 1;
            else {
                int a = atoi(w);
                const char *x = strchr(w, 'x'), *y = x ? strchr(x + 1, 'x') : 0;
                int bb = y ? atoi(y + 1) : 16, h = x ? atoi(x + 1) : 0;
                int k = x ? rtg_find(a, h, bb) : 0;
                if (k) {
                    sel.v[R_RTG] = k;
                    ok = 1;
                } else if (a >= 320 && a <= 4096 && h >= 200 && h <= 4096 && (bb == 16 || bb == 24 || bb == 32)) {
                    sys_f.rw = a, sys_f.rh = h, sys_f.rbits = bb;
                    sel.v[R_RTG] = -1;  /* a size of one's own: written as given */
                    ok = 1;
                }
            }
        }
        if (!ok) {
            dprintf(2, "screenprefs: %s: not understood here\n", argv[i]);
            zusage(USAGE);
        }
    }
    return 0;
}
static int command(int savef, int yes, int argc, char **argv)
{
    const char *c = argv[0];
    int which = !strcmp(c, "video") ? R_VIDEO : !strcmp(c, "console") ? R_CMODE : !strcmp(c, "desktop") ? R_DMODE
              : !strcmp(c, "rtg") ? R_RTG : -1;
    if (which < 0 || (which != R_CMODE && which != R_DMODE && argc < 2) || (which == R_RTG && !savef)) {
        if (which == R_RTG && !savef)
            dprintf(2, "screenprefs: rtg: a card's size counts from the next start: -s saves it\n");
        zusage(USAGE);
    }
    words(which, argc - 1, argv + 1);
    if (which != R_DMODE && !root())
        return fail(c, "the console's and the system's settings are root's (sudo screenprefs)");
    if (which == R_DMODE && on_card && (sel.v[R_DMODE] != now.v[R_DMODE] || sel.v[R_DDEPTH] != now.v[R_DDEPTH]))
        return fail(c, "the desktop is on a graphics card: its size counts from the next start (screenprefs -s rtg)");
    if (which == R_DMODE && !aga() && sel.v[R_DMODE] >= DPYM_SHRES)
        return fail(mname[sel.v[R_DMODE]], "super-hires needs AGA");
    if (which == R_DMODE && sel.v[R_DDEPTH] == 4 && !aga())
        return fail("4", "a depth of 4 on the desktop needs AGA (OCS and ECS show 3)");
    if (which == R_VIDEO && sel.v[R_VIDEO] && want_ntsc(sel.v[R_VIDEO]) != shown_ntsc() && !beam())
        return fail(argv[1], "this machine shows only its own video standard");
    /* applied where the screen is ours: the console's rows by CON_MODE etc., the desktop's when displayd runs */
    text_face = which != R_DMODE;
    struct choice before = now;
    int applied = 0;
    if (which == R_DMODE ? live_desktop() : which != R_RTG) {
        if (apply(&sel) < 0)
            return fail(c, msg);
        applied = 1;
        if (mode_changed(&before, &now) && !yes && sys_ttyinfo() > 0) {
            sys_ttymode(TTY_RAW | TTY_NOSIG);
            dprintf(2, "screenprefs: keep this mode? y keeps it; back by itself in %d s\n", KEEP_S);
            int k = 0;
            u32 t0 = sys_ticks();
            while ((int)((sys_ticks() - t0) / 100) < KEEP_S) {
                struct pollfd p = { 0, POLLIN, 0 };
                if (poll(&p, 1, 250) > 0) {
                    unsigned char ch;
                    if (sys_read(0, &ch, 1) == 1 && (ch | 0x20) == 'y')
                        k = 1;
                    break;
                }
            }
            sys_ttymode(0);
            if (!k) {
                struct choice back = before;
                back.v[R_CCOL] = now.v[R_CCOL], back.v[R_CFONT] = now.v[R_CFONT], back.v[R_DFONT] = now.v[R_DFONT];
                apply(&back);
                return fail(c, "not kept: back to the mode before");
            }
        }
    } else if (!savef && which == R_DMODE)
        return fail(c, "the desktop is not running here: -s saves it for its next start");
    if (savef) {
        if (sel.v[R_RTG] < 0) {         /* a card size of one's own */
            char l[48];
            const char *k[1] = { "rtg" }, *ls[1] = { l };
            snprintf(l, sizeof l, "rtg %dx%dx%d", sys_f.rw, sys_f.rh, sys_f.rbits);
            int r = cfg_set(SCREEN_SYS, k, ls, 1);
            if (r < 0)
                return fail(SCREEN_SYS, why(r));
        } else {
            nrows = 0;                  /* only this command's lines */
            if (which == R_VIDEO) rows[nrows++] = R_VIDEO;
            if (which == R_CMODE) rows[nrows++] = R_CMODE, rows[nrows++] = R_CCOL, rows[nrows++] = R_CFONT;
            if (which == R_DMODE) rows[nrows++] = R_DMODE, rows[nrows++] = R_DDEPTH, rows[nrows++] = R_DFONT;
            if (which == R_RTG) rows[nrows++] = R_RTG;
            int r = save(&sel);
            if (r < 0)
                return fail(root() ? SCREEN_SYS : "~/" SCREEN_USER, why(r));
        }
    }
    if (which == R_RTG)
        dprintf(1, "screenprefs: the card's size counts from the next start, with zenixboot RTG RTGMODE=%dx%dx%d\n",
                sel.v[R_RTG] > 0 ? rtgs[sel.v[R_RTG]][0] : sys_f.rw, sel.v[R_RTG] > 0 ? rtgs[sel.v[R_RTG]][1] :
                sys_f.rh, sel.v[R_RTG] > 0 ? rtgs[sel.v[R_RTG]][2] : sys_f.rbits);
    else if (applied && msg[0])
        dprintf(1, "screenprefs: %s\n", msg);
    return 0;
}

int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    const char *me = zprogname();
    int listf = 0, quietf = 0, savef = 0, yes = 0, force = 0, i;
    quiet_old = !strcmp(me, "conprefs");
    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        const char *o = argv[i];
        if (!strcmp(o, "--")) { i++; break; }
        if (!strcmp(o, "-v")) verbose = 1;
        else if (!strcmp(o, "-l") || !strcmp(o, "--list")) listf = 1;
        else if (!strcmp(o, "-q")) quietf = 1;
        else if (!strcmp(o, "-s")) savef = 1;
        else if (!strcmp(o, "-y")) yes = 1;
        else if (!strcmp(o, "-T")) force = 1;
        else if (!strcmp(o, "-W")) force = 2;
        else if (!strcmp(o, "-K")) { sys_write(1, defkeys, sizeof defkeys - 1); return 0; }
        else zbadopt(o, USAGE);
    }
    keys_load(&ks, defkeys, sizeof defkeys - 1, "screenprefs");
    if (ks.bad[0])
        dprintf(2, "screenprefs: %s\n", ks.bad);
    /* the face: the old names keep theirs (conprefs the console's, deskprefs the desktop's); else the window when the
     * desktop runs and this is not one of the consoles' own screens, else the text form */
    struct ustat st;
    int on_con = sys_fstat(0, &st) == 0 && st.ino >= 1 && st.ino <= 10;
    if (!strcmp(me, "conprefs") && !force)
        force = 1;
    if (!strcmp(me, "deskprefs") && !force)
        force = 2;
    int want_win = force == 2 || (!force && !on_con && displayd_runs());
    if ((want_win || (i < argc && !strcmp(argv[i], "desktop")) || listf || quietf) && displayd_runs())
        dpy_ok = dpy_open("screenprefs") == 0;
    if (force == 2 && !dpy_ok) {
        dprintf(2, "%s: the desktop: not reachable\n", me);
        return 1;
    }
    text_face = !(want_win && dpy_ok);
    fonts_scan();
    cfonts_scan();
    themes_scan();
    read_files();
    in_effect();
    if (listf) { list(); return 0; }
    if (quietf) { quiet(); return 0; }
    if (i < argc)
        return command(savef, yes, argc - i, argv + i);
    if (savef || yes)
        zusage(USAGE);
    for (int k = 0; k < nmoved; k++)
        dprintf(2, "%s: the settings of %s were read (%s's); Save keeps them in %s\n", me, moved[k],
                strstr(moved[k], "console") ? "conprefs" : "deskprefs",
                strstr(moved[k], "zdesk/screen") && moved[k][0] != '/' ? "~/" SCREEN_USER :
                strncmp(moved[k], "/etc", 4) ? "~/" SCREEN_USER : SCREEN_SYS);
    row = text_face ? 1 : 4;            /* the first row of this face's own screen */
    for (int k = 0; k < nrows; k++)
        if (rows[k] == (text_face ? R_CMODE : R_DMODE))
            row = k;
    if (text_face) {
        zscr_init();
        sys_ttymode(TTY_RAW | TTY_NOSIG);
        run();
        sys_ttymode(0);
        oputs("\033[H\033[2J");
        oflush();
    } else {
        for (int k = 0; k < nrows; k++)
            wlabel[k] = rlabel[rows[k]];
        pw.nrows = nrows;
        pw.gx = 136;
        pw.label = wlabel;
        pw.value = value;
        w_layout();
        pw.win = dpy_window(WK_NORMAL, "Screen preferences", -1, -1, pw.ww, pw.wh, 0);
        if (pw.win < 0) {
            dprintf(2, "%s: no window: %s\n", me, why(pw.win));
            dpy_close();
            return 1;
        }
        run();
        dpy_close();
    }
    return 0;
}
