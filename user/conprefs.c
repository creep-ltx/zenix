/* SPDX-License-Identifier: GPL-2.0-only */
/* conprefs: the text console's preferences (DESIGN D28 section 22, docs/man/conprefs.txt): its colours (2, 4, 8, 16
 * or the machine's choice) and its Unicode font (/usr/share/consolefonts/NAME.zcf, auto or none), on a text screen,
 * with AmigaOS's four buttons:
 *   Test    applied now (SYS_console CON_COLOURS, CON_UFONT); the screen stays, so Cancel can still go back
 *   Use     applied until the next boot; nothing saved (the session's choice is /var/run/console, which init removes
 *           at boot: AmigaOS's ENV: beside its ENVARC:)
 *   Save    applied and written to /etc/console (its other lines and comments kept), which init reads at boot
 *   Cancel  everything back as it was when the program started
 * The console is every console's (all ten share the screen): applying is root's, as /etc/console is; another user
 * sees the settings and is told so. -q prints the settings in effect, one line. */
#include "libz.h"
#include "cfgfile.h"

#define FONTDIR "/usr/share/consolefonts"
#define RUNFILE "/var/run/console"
#define NF 16
static char fonts[NF][24] = { "auto", "none" };
static int nfonts = 2;
static const int colours[5] = { 0, 2, 4, 8, 16 };     /* 0: auto */
struct choice { int col, font; };       /* indices */
static struct choice was, now, sel;
static char run_was[128];               /* /var/run/console at the start ("": none) */
static int row;
static char msg[80];

static void fonts_scan(void)
{
    char **names;
    int n = dir_list(FONTDIR, &names);
    for (int i = 0; i < n && nfonts < NF; i++) {
        int l = (int)strlen(names[i]);
        if (l > 4 && l - 4 < 24 && !strcmp(names[i] + l - 4, ".zcf")) {
            memcpy(fonts[nfonts], names[i], (u32)(l - 4));
            fonts[nfonts++][l - 4] = 0;
        }
    }
    if (n > 0)
        dir_free(names, n);
}
static int font_index(const char *f)
{
    for (int i = 0; i < nfonts; i++)
        if (!strcmp(fonts[i], f))
            return i;
    if (nfonts < NF && strlen(f) < 24 && !strchr(f, '/')) {
        strcpy(fonts[nfonts], f);
        return nfonts++;
    }
    return 0;
}
/* a file into b (at most n - 1 bytes, ended by a 0) -> its length, or -1 */
static int slurp(const char *path, char *b, int n)
{
    int fd = sys_open(path, O_RDONLY), got = 0, r;
    if (fd < 0)
        return -1;
    while (got < n - 1 && (r = sys_read(fd, b + got, (u32)(n - 1 - got))) > 0)
        got += r;
    sys_close(fd);
    b[got] = 0;
    return got;
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

/* ---- applying ---- */
static int auto_colours(void)           /* the kernel's own choice at boot: 16 with fast RAM (it shows 8 on OCS/ECS) */
{
    u32 mi[4];
    return sys_meminfo(mi) == 0 && mi[1] ? 16 : 8;
}
static int set_colours(int i)
{
    int c = colours[i] ? colours[i] : auto_colours(), r = sys_console(CON_COLOURS, c);
    if (r == -EBUSY)
        snprintf(msg, sizeof msg, "The desktop has the screen: colours not changed");
    else if (r == -ENOMEM)
        snprintf(msg, sizeof msg, "Not enough chip RAM for %d colours", c);
    else if (r < 0)
        snprintf(msg, sizeof msg, "Colours %d: %s", c, zstrerror(r) ? zstrerror(r) : "refused");
    else if (r != c)
        snprintf(msg, sizeof msg, "This machine shows %d colours", r);
    return r < 0 ? -1 : 0;
}
static int set_font(int i)
{
    if (!strcmp(fonts[i], "none")) {
        int r = sys_conufont(0, 0);
        if (r < 0)
            snprintf(msg, sizeof msg, "The font: %s", zstrerror(r) ? zstrerror(r) : "refused");
        return r < 0 ? -1 : 0;
    }
    char *p = path_join(FONTDIR, i == 0 ? "zenix-8x8.zcf" : fonts[i]);
    char *q = p && i ? malloc(strlen(p) + 5) : 0;
    if (q) {
        strcpy(q, p);
        strcat(q, ".zcf");
        free(p);
        p = q;
    }
    struct ustat st;
    int fd = p ? sys_open(p, O_RDONLY) : -ENOMEM, r = fd;
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
        snprintf(msg, sizeof msg, "%s: %s", p ? p : "the font", r == -ENOENT ? "not there" : r == -EINVAL ?
                 "not a console font (ZCF1)" : zstrerror(r) ? zstrerror(r) : "refused");
    free(b);
    free(p);
    return r < 0 ? -1 : 0;
}
static int apply(const struct choice *c)
{
    if (c->col != now.col) {
        if (set_colours(c->col) < 0)
            return -1;
        now.col = c->col;
    }
    if (c->font != now.font) {
        if (set_font(c->font) < 0)
            return -1;
        now.font = c->font;
    }
    return 0;
}
static void lines(const struct choice *c, char *col, char *font)
{
    if (colours[c->col])
        snprintf(col, 24, "colours %d", colours[c->col]);
    else
        strcpy(col, "colours auto");
    snprintf(font, 40, "font %s", fonts[c->font]);
}
static int write_run(const struct choice *c)
{
    char col[24], font[40], t[80];
    lines(c, col, font);
    snprintf(t, sizeof t, "%s\n%s\n", col, font);
    sys_mkdir("/var/run");
    return cfg_write(RUNFILE, t);
}

/* ---- the screen ---- */
static const char *const label[2] = { "Colours", "Unicode font" };
static const char *const button[4] = { "Test", "Use", "Save", "Cancel" };
static void value(int i, char *b)
{
    if (i == 0) {
        if (colours[sel.col])
            snprintf(b, 32, "%d", colours[sel.col]);
        else
            snprintf(b, 32, "auto (%d)", auto_colours());
    } else
        snprintf(b, 32, "%s", fonts[sel.font]);
}
static void draw(void)
{
    oputs("\033[H\033[2J\033[1m Console preferences\033[m");
    if (sys_getuid())
        oputs("   (root's: sudo conprefs to change)");
    for (int i = 0; i < 2; i++) {
        char v[32];
        value(i, v);
        oprintf("\033[%d;4H%-15s%s< %-24s >\033[m", 4 + 2 * i, label[i], i == row ? "\033[7m" : "", v);
    }
    oputs("\033[9;4H");
    for (int i = 0; i < 4; i++)
        oprintf("[ %s ]  ", button[i]);
    oputs("\033[11;4HUp/Down: a setting   Left/Right: change it   T, U, S, C: the buttons   Esc: Cancel");
    oprintf("\033[13;4H%s\033[K", msg);
    oflush();
}
static void report(const char *what)
{
    char col[24], font[40];
    lines(&sel, col, font);
    dprintf(2, "conprefs: %s: %s, %s%s%s\n", what, col, font, msg[0] ? ": " : "", msg);
}
/* a button -> 1: the program ends */
static int press(int b)
{
    msg[0] = 0;
    if (sys_getuid()) {
        if (b == 3)
            return 1;
        snprintf(msg, sizeof msg, "Only root can change the console: sudo conprefs");
        report(button[b]);
        draw();
        return 0;
    }
    if (b == 3) {
        apply(&was);
        if (run_was[0])
            cfg_write(RUNFILE, run_was);
        else
            sys_unlink(RUNFILE);
        report("cancel");
        return 1;
    }
    if (apply(&sel) < 0) {
        report(button[b]);
        draw();
        return 0;
    }
    int r = 0;
    if (b == 2) {
        static const char *const keys[2] = { "colours", "font" };
        char col[24], font[40];
        lines(&sel, col, font);
        const char *ls[2] = { col, font };
        if ((r = cfg_set("/etc/console", keys, ls, 2)) == 0)
            sys_unlink(RUNFILE);        /* the saved settings are the session's now */
        else
            snprintf(msg, sizeof msg, "Not saved: %s", zstrerror(r) ? zstrerror(r) : "cannot write /etc/console");
    } else
        write_run(&sel);
    report(b == 0 ? "test" : b == 1 ? "use" : "save");
    if (b == 0 || r < 0) {
        draw();
        return 0;
    }
    return 1;
}
static void cycle(int d)
{
    msg[0] = 0;
    if (row == 0)
        sel.col = (sel.col + d + 5) % 5;
    else
        sel.font = (sel.font + d + nfonts) % nfonts;
}
static int getkey(int ms)               /* a byte, or -1 after ms */
{
    struct pollfd p = { 0, POLLIN, 0 };
    char c;
    if (ms >= 0 && poll(&p, 1, ms) <= 0)
        return -1;
    return sys_read(0, &c, 1) == 1 ? (unsigned char)c : -2;
}

int main(int argc, char **argv)
{
    int quiet = argc > 1 && !strcmp(argv[1], "-q");
    if (argc > 1 && !quiet) {
        dprintf(2, "usage: conprefs [-q]\n");
        return 2;
    }
    fonts_scan();
    /* in effect: the colours the kernel shows; the font of this session's Use, else /etc/console's (no file, no line:
     * auto) - the kernel does not say which font it has */
    char *b = malloc(4096), v[24];
    if (!b)
        return 1;
    int shown = sys_console(CON_COLOURS, 0);
    was.col = 0;
    for (int i = 1; i < 5; i++)
        if (colours[i] == shown)
            was.col = i;
    run_was[0] = 0;
    if (slurp(RUNFILE, b, 4096) > 0 && strlen(b) < sizeof run_was)
        strcpy(run_was, b);
    if (!(run_was[0] && setting(run_was, "font", v)) && !(slurp("/etc/console", b, 4096) > 0 && setting(b, "font", v)))
        strcpy(v, "auto");
    was.font = font_index(v);
    free(b);
    now = sel = was;
    if (quiet) {
        char col[24], font[40];
        lines(&was, col, font);
        oprintf("conprefs: %s, %s\n", col, font);
        oflush();
        return 0;
    }
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    draw();
    for (int done = 0; !done;) {
        int c = getkey(-1);
        if (c == -2)
            break;
        if (c == 27) {
            int c2 = getkey(150);
            if (c2 < 0) {
                done = press(3);
                continue;
            }
            int c3 = c2 == '[' || c2 == 'O' ? getkey(150) : -1;
            if (c3 == 'A' || c3 == 'B')
                row ^= 1;
            else if (c3 == 'C')
                cycle(1);
            else if (c3 == 'D')
                cycle(-1);
            draw();
            continue;
        }
        if (c == '\r' || c == '\n')
            c = 'u';                    /* Return: Use */
        switch (c | 0x20) {
        case 't': done = press(0); break;
        case 'u': done = press(1); break;
        case 's': done = press(2); break;
        case 'c': done = press(3); break;
        }
        if (!done)
            draw();
    }
    sys_ttymode(0);
    oputs("\033[H\033[2J");
    oflush();
    return 0;
}
