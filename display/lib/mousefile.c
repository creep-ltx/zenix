/* SPDX-License-Identifier: GPL-2.0-only */
/* The desktop's mouse settings and pointer files (display/mousefile.h; DESIGN D61, docs/d61-mouse.md 6). Linked into
 * zdesk and mouseprefs. Cold code: -Os. */
#include "libz.h"
#include "../mousefile.h"

#define MF_MAX 2048

const unsigned char mf_acc_thresh[4] = { 0, 4, 3, 2 };
const unsigned short mf_acc_mult[4] = { 100, 200, 250, 300 };
const char *const mf_acc_name[4] = { "none", "low", "medium", "high" };

/* a file into a buffer of MF_MAX (malloc'ed, 0-ended) -> it, or 0 */
static char *slurp(const char *path)
{
    int fd = sys_open(path, O_RDONLY), n = 0, r;
    if (fd < 0)
        return 0;
    char *b = malloc(MF_MAX);
    if (b)
        while (n < MF_MAX - 1 && (r = sys_read(fd, b + n, (u32)(MF_MAX - 1 - n))) > 0)
            n += r;
    sys_close(fd);
    if (b)
        b[n] = 0;
    return b;
}
/* the next line's words (at most 4) -> how many; *p moves past the line */
static int words(char **p, char **w)
{
    char *l = *p, *e;
    int k = 0;
    for (e = l; *e && *e != '\n'; e++)
        if (*e == ' ' || *e == '\t' || *e == '\r')
            *e = 0;
        else if (e == l || !e[-1])
            w[k < 4 ? k++ : 3] = e;
    if (*e)
        *e++ = 0;
    *p = e;
    return k && *w[0] != '#' ? k : 0;
}
static int num(const char *s, int lo, int hi)  /* a decimal number in lo..hi, else -1 */
{
    int v = 0;
    if (!*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000)
            return -1;
        v = v * 10 + *s - '0';
    }
    return v < lo || v > hi ? -1 : v;
}

int dpy_mousefile(const char *path, struct dpymouse *m, char *pointer, const char *who)
{
    m->size = DPYMOUSE_FIRST;
    m->speed = 100;
    m->thresh = mf_acc_thresh[1];
    m->mult = mf_acc_mult[1];
    m->flags = 0;
    m->dclick = 50;
    pointer[0] = 0;
    char *b = slurp(path), *p = b, *w[4];
    if (!b)
        return -ENOENT;                 /* (or no memory: the defaults all the same) */
    int ln = 0, bad = 0, bln = 0;
    const char *bw = "";
    while (*p) {
        ln++;
        int k = words(&p, w), v = k == 2 ? num(w[1], 0, 1000) : -1, ok = k == 2;
        if (!k)
            continue;
        if (ok && !strcmp(w[0], "speed") && v >= 10 && v <= 800)
            m->speed = (unsigned short)v;
        else if (ok && !strcmp(w[0], "threshold") && v >= 0 && v <= 127)
            m->thresh = (unsigned char)v;
        else if (ok && !strcmp(w[0], "multiplier") && v >= 100 && v <= 800)
            m->mult = (unsigned short)v;
        else if (ok && !strcmp(w[0], "doubleclick") && v >= 10 && v <= 300)
            m->dclick = (unsigned short)v;
        else if (ok && !strcmp(w[0], "buttons") && (!strcmp(w[1], "normal") || !strcmp(w[1], "swapped")))
            m->flags = (unsigned char)((m->flags & ~DPYMF_SWAP) | (w[1][0] == 's' ? DPYMF_SWAP : 0));
        else if (ok && !strcmp(w[0], "pointer") && strlen(w[1]) < MF_NAME && !strchr(w[1], '/'))
            strcpy(pointer, w[1]);
        else if (ok && !strcmp(w[0], "acceleration")) {
            int a = 0;
            while (a < 4 && strcmp(w[1], mf_acc_name[a]))
                a++;
            if (!strcmp(w[1], "custom"))
                ;                       /* the threshold and multiplier lines say */
            else if (a < 4) {
                m->thresh = mf_acc_thresh[a];
                m->mult = mf_acc_mult[a];
            } else
                goto no;
        } else {
        no:
            if (!bad++) {
                bw = w[0];
                bln = ln;
            }
        }
    }
    if (bad)
        dprintf(2, "%s: %s line %d: not understood (speed, acceleration, threshold, multiplier, buttons, doubleclick, "
                "pointer): %s%s\n", who, path, bln, bw, bad > 1 ? " (and more lines)" : "");
    free(b);
    return 0;
}

const char *dpy_mousepath(const char *name, char *buf, int user)
{
    const char *h = getenv("HOME");
    if (!user) {
        strcpy(buf, "/etc/zdesk/");
    } else {
        if (!h || !*h || strlen(h) > 160)
            return 0;
        strcpy(buf, h);
        strcat(buf, "/.config");
        sys_mkdir(buf);
        strcat(buf, "/zdesk/");
    }
    sys_mkdir(buf);
    if (!strncmp(name, "pointers/", 9)) {
        strcat(buf, "pointers");
        sys_mkdir(buf);
        strcat(buf, "/");
        name += 9;
    }
    if (strlen(name) > 40)
        return 0;
    strcat(buf, name);
    return buf;
}

static const char px[4] = { '.', 'o', 'w', 'x' };
int dpy_pointerfile(const char *name, struct dpymouse *m, const char *who)
{
    char path[ZPATH_MAX], f[48];
    if (strlen(name) >= MF_NAME || strchr(name, '/'))
        return -EINVAL;
    strcpy(f, "pointers/");
    strcat(f, name);
    strcat(f, ".pointer");
    char *b = 0;
    if (sys_getuid() && dpy_mousepath(f, path, 1))
        b = slurp(path);
    if (!b)
        b = slurp(dpy_mousepath(f, path, 0));
    if (!b)
        return -ENOENT;
    unsigned short img[32], col[3];
    int hx = 0, hy = 0, rows = 0, ok = 1;
    memcpy(col, m->colour, sizeof col);
    memset(img, 0, sizeof img);
    char *p = b, *w[4];
    while (*p && ok) {
        int k = words(&p, w);
        if (!k)
            continue;
        if (k == 3 && !strcmp(w[0], "hot")) {
            hx = num(w[1], 0, 15);
            hy = num(w[2], 0, 15);
            ok = hx >= 0 && hy >= 0;
        } else if (k == 4 && !strcmp(w[0], "colours")) {
            for (int i = 0; i < 3; i++) {
                char *e;
                unsigned long v = strtoul(w[i + 1], &e, 16);
                ok &= !*e && v <= 0xFFF && strlen(w[i + 1]) == 3;
                col[i] = (unsigned short)v;
            }
        } else if (k == 1 && strlen(w[0]) == 16 && rows < 16) {
            for (int x = 0; x < 16; x++) {
                int v = 0;
                while (v < 4 && px[v] != w[0][x])
                    v++;
                ok &= v < 4;
                v &= 3;
                if (v & 1) img[2 * rows] |= (unsigned short)(0x8000 >> x);
                if (v & 2) img[2 * rows + 1] |= (unsigned short)(0x8000 >> x);
            }
            rows++;
        } else
            ok = 0;
    }
    free(b);
    if (!ok || rows != 16) {
        dprintf(2, "%s: %s: not a pointer (hot X Y, colours RGB RGB RGB, 16 rows of 16 of . o w x)\n", who, path);
        return -EINVAL;
    }
    memcpy(m->image, img, sizeof img);
    memcpy(m->colour, col, sizeof col);
    m->hotx = (signed char)hx;
    m->hoty = (signed char)hy;
    return 0;
}

int dpy_pointertext(const struct dpymouse *m, char *b)
{
    int n = sprintf(b, "# a pointer (docs/man/mouseprefs.txt): its hot spot, colours 1-3, 16 rows (. o w x)\n"
                       "hot %d %d\ncolours %03x %03x %03x\n", m->hotx, m->hoty, m->colour[0], m->colour[1], m->colour[2]);
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++)
            b[n++] = px[(m->image[2 * y] << x & 0x8000 ? 1 : 0) | (m->image[2 * y + 1] << x & 0x8000 ? 2 : 0)];
        b[n++] = '\n';
    }
    b[n] = 0;
    return n;
}
