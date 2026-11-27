/* SPDX-License-Identifier: GPL-2.0-only */
/* The screen file's reader (display/screenfile.h; the format in docs/d62-screen-modes.md 6, before it
 * docs/d38-desktop.md "Screen modes" and "Fonts"; frozen in docs/frozen-interfaces.md): "video auto|pal|ntsc",
 * "console hires|hires-lace|auto", "console-colours auto|2|4|8|16", "console-font auto|none|NAME",
 * "desktop hires|hires-lace|superhires|superhires-lace|auto", "desktop-depth auto|3|4", "desktop-font NAME SIZE|builtin",
 * "rtg front|WxH|WxHxBITS"; the older desktop file's "mode ..." and "font ..."; "# comments". Linked into displayd and
 * screenprefs. Cold code: -Os. */
#include "libz.h"
#include "../proto.h"
#include "../screenfile.h"

#define SF_MAX 4096                     /* the shipped /etc/screen with its comments is about 2 KB */

static int num(const char *q)           /* a whole number, or -1 */
{
    int v = 0;
    if (!*q)
        return -1;
    for (; *q; q++) {
        if (*q < '0' || *q > '9' || v > 100000)
            return -1;
        v = v * 10 + (*q - '0');
    }
    return v;
}
static int dmode(const char *w)         /* a desktop mode's name -> DPYM_*, or -1 */
{
    static const char *const n[5] = { "hires", "hires-lace", "", "superhires", "superhires-lace" };
    if (!strcmp(w, "auto"))
        return DPYM_HIRES;
    for (int i = 0; i < 5; i++)
        if (i != DPYM_RTG && !strcmp(w, n[i]))
            return i;
    return -1;
}
/* "WxH" or "WxHxBITS" -> 0 */
static int rtgsize(const char *w, int *rw, int *rh, int *rb)
{
    char t[24];
    if (strlen(w) >= sizeof t)
        return -1;
    strcpy(t, w);
    char *x = strchr(t, 'x'), *y = x ? strchr(x + 1, 'x') : 0;
    if (!x)
        return -1;
    *x = 0;
    if (y)
        *y = 0;
    int a = num(t), b = num(x + 1), c = y ? num(y + 1) : 0;
    if (a < 320 || a > 4096 || b < 200 || b > 4096 || (y && c != 16 && c != 24 && c != 32))
        return -1;
    *rw = a, *rh = b, *rb = c;
    return 0;
}
int dpy_screenfile(const char *path, struct dpyscreen *s, const char *who)
{
    memset(s, 0, sizeof *s);
    s->mode = DPYM_HIRES;
    strcpy(s->cfont, "auto");
    int fd = sys_open(path, O_RDONLY), n = 0, r, ln = 0, bad = 0, bln = 0;
    const char *bw = "";
    if (fd < 0)
        return -ENOENT;
    char *b = malloc(SF_MAX);
    if (b)
        while (n < SF_MAX - 1 && (r = sys_read(fd, b + n, (u32)(SF_MAX - 1 - n))) > 0)
            n += r;
    sys_close(fd);
    if (!b)
        return 0;                       /* no memory: the defaults */
    b[n] = 0;
    for (char *l = b, *e; *l; l = e) {
        ln++;
        char *w[4];
        int k = 0;
        for (e = l; *e && *e != '\n'; e++)
            if (*e == ' ' || *e == '\t' || *e == '\r')
                *e = 0;
            else if (e == l || !e[-1])
                w[k < 4 ? k++ : 3] = e;
        if (*e)
            *e++ = 0;
        if (!k || *w[0] == '#')
            continue;
        int v = k == 3 ? num(w[2]) : -1, m = k == 2 ? dmode(w[1]) : -1, ok = 1;
        const char *key = w[0];
        int font = !strcmp(key, "font") || !strcmp(key, "desktop-font");
        if (k == 2 && (!strcmp(key, "mode") || !strcmp(key, "desktop")) && m >= 0 &&
            (m < DPYM_SHRES || key[0] == 'd'))
            s->mode = m, s->found |= SF_DESKTOP;
        else if (font && ((k == 2 && !strcmp(w[1], "builtin")) || (k == 3 && !strcmp(w[1], "topaz") && v == 8)))
            s->font[0] = 0, s->size = 0, s->found |= SF_DESKTOP;     /* the 8 x 8: Topaz 8 is the ROM's */
        else if (font && k == 3 && v > 0 && v < 1000 && strlen(w[1]) < sizeof s->font) {
            strcpy(s->font, w[1]);
            s->size = v;
            s->found |= SF_DESKTOP;
        } else if (k == 2 && !strcmp(key, "desktop-depth") && (!strcmp(w[1], "auto") || !strcmp(w[1], "3") ||
                                                               !strcmp(w[1], "4")))
            s->depth = w[1][0] == 'a' ? 0 : w[1][0] - '0', s->found |= SF_DESKTOP;
        else if (k == 2 && !strcmp(key, "video") && (!strcmp(w[1], "auto") || !strcmp(w[1], "pal") ||
                                                    !strcmp(w[1], "ntsc")))
            s->video = w[1][0] == 'a' ? 0 : w[1][0] == 'p' ? 1 : 2, s->found |= SF_VIDEO;
        else if (k == 2 && !strcmp(key, "console") && (m == DPYM_HIRES || m == DPYM_LACE))
            s->cmode = m, s->found |= SF_CONSOLE;
        else if (k == 2 && !strcmp(key, "console-colours") && (!strcmp(w[1], "auto") || num(w[1]) == 2 ||
                                                              num(w[1]) == 4 || num(w[1]) == 8 || num(w[1]) == 16))
            s->colours = w[1][0] == 'a' ? 0 : num(w[1]), s->found |= SF_CONSOLE;
        else if (k == 2 && !strcmp(key, "console-font") && strlen(w[1]) < sizeof s->cfont && !strchr(w[1], '/')) {
            strcpy(s->cfont, w[1]);
            s->found |= SF_CONSOLE;
        } else if (k == 2 && !strcmp(key, "rtg") && (!strcmp(w[1], "front") ||
                                                    rtgsize(w[1], &s->rw, &s->rh, &s->rbits) == 0)) {
            if (w[1][0] == 'f')
                s->rw = s->rh = s->rbits = 0;
            s->found |= SF_RTG;
        } else
            ok = 0;
        if (!ok && !bad++) {
            bw = w[0];
            bln = ln;
        }
    }
    if (bad)
        dprintf(2, "%s: %s line %d: not understood (man screenprefs \"FILES\"): %s%s\n", who, path, bln, bw,
                bad > 1 ? " (and more lines)" : "");
    free(b);
    return 0;
}
