/* SPDX-License-Identifier: GPL-2.0-only */
/* The desktop's screen file's reader (display/screenfile.h; the format in docs/d38-desktop.md "Screen modes" and
 * "Fonts", frozen in docs/frozen-interfaces.md): "mode hires|hires-lace|auto", "font NAME SIZE", "font builtin",
 * "# comments". Linked into displayd, zdesk and deskprefs. Cold code: -Os. */
#include "libz.h"
#include "../proto.h"
#include "../screenfile.h"

#define SF_MAX 2048                     /* the shipped file with its comments is about 900 bytes */

int dpy_screenfile(const char *path, struct dpyscreen *s, const char *who)
{
    s->mode = DPYM_HIRES;
    s->font[0] = 0;
    s->size = 0;
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
        int v = k == 3 ? atoi(w[2]) : 0;
        for (const char *q = k == 3 ? w[2] : ""; *q; q++)
            if (*q < '0' || *q > '9')
                v = 0;
        if (k == 2 && !strcmp(w[0], "mode") && (!strcmp(w[1], "hires") || !strcmp(w[1], "auto")))
            s->mode = DPYM_HIRES;
        else if (k == 2 && !strcmp(w[0], "mode") && !strcmp(w[1], "hires-lace"))
            s->mode = DPYM_LACE;
        else if (!strcmp(w[0], "font") && ((k == 2 && !strcmp(w[1], "builtin")) ||
                                          (k == 3 && !strcmp(w[1], "topaz") && v == 8)))
            s->font[0] = 0, s->size = 0;    /* the 8 x 8: Topaz 8 is the ROM's */
        else if (k == 3 && !strcmp(w[0], "font") && v > 0 && v < 1000 && strlen(w[1]) < sizeof s->font) {
            strcpy(s->font, w[1]);
            s->size = v;
        } else if (!bad++) {
            bw = w[0];
            bln = ln;
        }
    }
    if (bad)
        dprintf(2, "%s: %s line %d: not understood (mode hires|hires-lace|auto, font NAME SIZE): %s%s\n", who, path,
                bln, bw, bad > 1 ? " (and more lines)" : "");
    free(b);
    return 0;
}
