/* SPDX-License-Identifier: GPL-2.0-only */
/* zdesk's settings (docs/man/zdesk.txt "FILES"): the theme /etc/zdesk/themes/NAME.theme (NAME from -theme, else the
 * file theme, else flat), the keys, the bar, the Zenix menu and the desktop's icons; for each, the user's
 * ~/.config/zdesk/NAME wins over /etc/zdesk/NAME. A bad line is said on standard error with its file and number and
 * skipped; a missing file leaves the theme's or the built-in default. */
#include "zdesk.h"

struct theme th;
struct key keys[NKEY];
int nkeys;
struct item menu_items[NITEM], icons[NITEM];
int nmenu_items, nicons;
struct block blocks[NBLOCK];
int nblocks;

int pen(int role)
{
    int p = th.role[role];
    return p == 255 ? 255 : depth < 4 ? th.map8[p & 15] : p;
}

const char *conf_file(const char *name, char *buf)
{
    const char *h = getenv("HOME");
    struct ustat st;
    if (h && strlen(h) + strlen(name) + 16 < ZPATH_MAX) {
        strcpy(buf, h);
        strcat(buf, "/.config/zdesk/");
        strcat(buf, name);
        if (sys_stat(buf, &st) == 0)
            return buf;
    }
    strcpy(buf, "/etc/zdesk/");
    strcat(buf, name);
    return buf;
}

/* the whole file into memory (at most 16 KB), or 0 */
static char *slurp(const char *path)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    char *b = malloc(16384);
    int n = 0, r;
    if (b)
        while (n < 16383 && (r = sys_read(fd, b + n, (u32)(16383 - n))) > 0)
            n += r;
    sys_close(fd);
    if (b)
        b[n] = 0;
    return b;
}
static char *next_line(char **p)
{
    char *s = *p;
    if (!*s)
        return 0;
    char *e = strchr(s, '\n');
    if (e) { *e = 0; *p = e + 1; } else *p = s + strlen(s);
    return s;
}
static int words(char *l, char **w, int max)
{
    int n = 0;
    while (*l && n < max) {
        while (*l == ' ' || *l == '\t') *l++ = 0;
        if (!*l || *l == '#') break;
        w[n++] = l;
        while (*l && *l != ' ' && *l != '\t') l++;
    }
    return n;
}
static void bad(const char *f, int ln, const char *what)
{
    dprintf(2, "zdesk: %s line %d: %s: not used\n", f, ln, what);
}
static char *dup_rest(const char *s)   /* the rest of a line after "=", trimmed */
{
    while (*s == ' ' || *s == '\t') s++;
    char *d = strdup(s);
    return d;
}

/* ---- the theme ---- */
static const char *const rolenames[R_N] = { "desk", "bar", "barline", "barfg", "logo", "barcur", "barcurbg", "bardim",
    "clock", "iconfg", "labelbg", "ik", "if", "ia", "iw", "id", "winbg", "winfg", "btn", "btnfg", "menubg", "menufg",
    "selbg", "selfg" };
static int hex12(const char *s) { return (int)strtoul(s, 0, 16) & 0xFFF; }
static int theme_load(const char *name)
{
    char path[ZPATH_MAX];
    if (strlen(name) > 15 || strchr(name, '/'))
        return -1;
    strcpy(path, "/etc/zdesk/themes/");
    strcat(path, name);
    strcat(path, ".theme");
    char *b = slurp(path), *p = b, *l;
    if (!b) {
        dprintf(2, "zdesk: the theme %s: %s\n", path, zwhy("not there"));
        return -1;
    }
    strcpy(th.name, name);
    int ln = 0, g = -1, row = 0;
    while ((l = next_line(&p))) {
        ln++;
        if (l[0] == '|' && g >= 0) {    /* a gadget's picture, a row of pens: . none, 0-9 a-f */
            if (row < 12) {
                strncpy(th.gad[g][row], l + 1, 15);
                if ((int)strlen(l + 1) > th.gadw[g]) th.gadw[g] = (u8)(strlen(l + 1) > 15 ? 15 : strlen(l + 1));
                th.gadh[g] = (u8)++row;
            }
            continue;
        }
        g = -1;
        char *w[20];
        int n = words(l, w, 20);
        if (!n)
            continue;
        if (!strcmp(w[0], "palette") && n == 17) {
            for (int i = 0; i < 16; i++) th.st.palette[i] = (u16)hex12(w[i + 1]);
        } else if (!strcmp(w[0], "map8") && n == 17) {
            for (int i = 0; i < 16; i++) th.map8[i] = (u8)(atoi(w[i + 1]) & 7);
        } else if (!strcmp(w[0], "frame") && n == 9) {
            th.st.left = (u8)atoi(w[1]); th.st.right = (u8)atoi(w[2]); th.st.top = (u8)atoi(w[3]);
            th.st.bottom = (u8)atoi(w[4]); th.st.title = (u8)atoi(w[5]); th.st.bevel = (u8)atoi(w[6]);
            th.st.shadow = (u8)atoi(w[7]); th.st.centre = (u8)atoi(w[8]);
        } else if (!strcmp(w[0], "pens") && n == 11) {
            th.st.border[0] = (u8)atoi(w[1]); th.st.border[1] = (u8)atoi(w[2]);
            th.st.bar[0] = (u8)atoi(w[3]); th.st.bar[1] = (u8)atoi(w[4]);
            th.st.text[0] = (u8)atoi(w[5]); th.st.text[1] = (u8)atoi(w[6]);
            th.st.light = (u8)atoi(w[7]); th.st.dark = (u8)atoi(w[8]); th.st.shadowc = (u8)atoi(w[9]);
            th.st.stripes = (u8)atoi(w[10]);
        } else if (!strcmp(w[0], "role") && n == 3) {
            int k = -1;
            for (int i = 0; i < R_N; i++) if (!strcmp(w[1], rolenames[i])) k = i;
            if (k < 0) bad(path, ln, w[1]);
            else th.role[k] = (u8)(!strcmp(w[2], "none") ? 255 : atoi(w[2]) & 15);
        } else if (!strcmp(w[0], "gadget") && n == 4) {
            int k = !strcmp(w[1], "close") ? 0 : !strcmp(w[1], "min") ? 1 : !strcmp(w[1], "zoom") ? 2 : -1;
            int off = atoi(w[3]);
            if (k < 0 || off < 0 || off > 120) { bad(path, ln, "gadget"); continue; }
            th.st.gadx[k] = (u8)(!strcmp(w[2], "right") ? 128 + off : off ? off : 1);
            g = k; row = 0; th.gadw[k] = th.gadh[k] = 0;
        } else if (!strcmp(w[0], "menus") && n == 2) {
            th.menus = (u8)(!strcmp(w[1], "click") ? M_CLICK : !strcmp(w[1], "hold") ? M_HOLD : M_CLICK | M_HOLD);
        } else if (!strcmp(w[0], "drag") && n == 2) {
            th.drag = !strcmp(w[1], "outline") ? DRAG_OUTLINE : !strcmp(w[1], "whole") ? 0 : -1;
        } else if (!strcmp(w[0], "bar") && n >= 2) {
            int z = !strcmp(w[1], "left") ? 0 : !strcmp(w[1], "middle") ? 1 : !strcmp(w[1], "right") ? 2 : -1;
            if (z < 0) { bad(path, ln, "bar"); continue; }
            th.bar[z][0] = 0;
            for (int i = 2; i < n; i++) {
                if (strlen(th.bar[z]) + strlen(w[i]) + 2 >= sizeof th.bar[z]) break;
                if (i > 2) strcat(th.bar[z], " ");
                strcat(th.bar[z], w[i]);
            }
        } else
            bad(path, ln, w[0]);
    }
    free(b);
    return 0;
}

/* ---- keys: "lamiga+shift+left resize -8 0" ---- */
static const struct { const char *n; u8 c; } keynames[] = {
    { "return", 0x44 }, { "enter", 0x43 }, { "tab", 0x42 }, { "space", 0x40 }, { "esc", 0x45 }, { "del", 0x46 },
    { "backspace", 0x41 }, { "help", 0x5F }, { "up", 0x4C }, { "down", 0x4D }, { "right", 0x4E }, { "left", 0x4F },
    { "1", 0x01 }, { "2", 0x02 }, { "3", 0x03 }, { "4", 0x04 }, { "5", 0x05 }, { "6", 0x06 }, { "7", 0x07 },
    { "8", 0x08 }, { "9", 0x09 }, { "0", 0x0A }, { "q", 0x10 }, { "w", 0x11 }, { "e", 0x12 }, { "r", 0x13 },
    { "t", 0x14 }, { "y", 0x15 }, { "u", 0x16 }, { "i", 0x17 }, { "o", 0x18 }, { "p", 0x19 }, { "a", 0x20 },
    { "s", 0x21 }, { "d", 0x22 }, { "f", 0x23 }, { "g", 0x24 }, { "h", 0x25 }, { "j", 0x26 }, { "k", 0x27 },
    { "l", 0x28 }, { "z", 0x31 }, { "x", 0x32 }, { "c", 0x33 }, { "v", 0x34 }, { "b", 0x35 }, { "n", 0x36 },
    { "m", 0x37 },
};
static const struct { const char *n; u8 a; } actions[] = {
    { "terminal", A_TERMINAL }, { "run", A_RUN }, { "close", A_CLOSE }, { "minimise", A_MIN }, { "next", A_NEXT },
    { "menu", A_MENU }, { "move", A_MOVE }, { "resize", A_RESIZE }, { "workspace", A_WORKSPACE },
    { "sendto", A_SENDTO }, { "logout", A_LOGOUT }, { "about", A_ABOUT }, { "zoom", A_ZOOM },
};
static void add_key(u8 code, u16 mask, u16 qual, u8 action, short a, short b, char *cmd)
{
    if (nkeys >= NKEY)
        return;
    keys[nkeys++] = (struct key){ code, mask, qual, action, a, b, cmd };
}
static int key_line(char **w, int n)
{
    if (n < 2)
        return -1;
    u16 mask = GQ_LSHIFT | GQ_RSHIFT | GQ_CTRL | GQ_LALT | GQ_RALT | GQ_LAMIGA | GQ_RAMIGA, qual = 0;
    int shift = 0, code = -1;
    for (char *s = w[0]; *s;) {
        char *e = strchr(s, '+');
        if (e) *e = 0;
        if (!strcmp(s, "lamiga") || !strcmp(s, "amiga")) qual |= GQ_LAMIGA;
        else if (!strcmp(s, "ramiga")) qual |= GQ_RAMIGA;
        else if (!strcmp(s, "ctrl")) qual |= GQ_CTRL;
        else if (!strcmp(s, "alt")) qual |= GQ_LALT;
        else if (!strcmp(s, "shift")) shift = 1;
        else {
            for (u32 i = 0; i < sizeof keynames / sizeof keynames[0]; i++)
                if (!strcmp(s, keynames[i].n)) code = keynames[i].c;
            if (code < 0) return -1;
        }
        if (!e) break;
        s = e + 1;
    }
    int act = 0;
    for (u32 i = 0; i < sizeof actions / sizeof actions[0]; i++)
        if (!strcmp(w[1], actions[i].n)) act = actions[i].a;
    if (code < 0 || !act || (code >= 0x50 && code <= 0x59))
        return -1;                      /* Amiga+F1..F10 are the kernel's (the consoles) */
    short a = n > 2 ? (short)atoi(w[2]) : 0, b = n > 3 ? (short)atoi(w[3]) : 0;
    char *cmd = 0;
    if (act == A_RUN) {
        if (n < 3) return -1;
        char l[160] = "";
        for (int i = 2; i < n && strlen(l) + strlen(w[i]) + 2 < sizeof l; i++) {
            if (i > 2) strcat(l, " ");
            strcat(l, w[i]);
        }
        cmd = strdup(l);
    }
    if (shift) {                        /* either shift: one grab for each */
        add_key((u8)code, mask, qual | GQ_LSHIFT, (u8)act, a, b, cmd);
        add_key((u8)code, mask, qual | GQ_RSHIFT, (u8)act, a, b, cmd);
    } else
        add_key((u8)code, mask, qual, (u8)act, a, b, cmd);
    return 0;
}

/* "LABEL = COMMAND" (the menu), "ART LABEL = COMMAND" (the icons) */
static void item_file(const char *name, struct item *it, int *n, int art)
{
    char path[ZPATH_MAX];
    conf_file(name, path);
    char *b = slurp(path), *p = b, *l;
    if (!b)
        return;
    int ln = 0;
    while ((l = next_line(&p)) && *n < NITEM) {
        ln++;
        while (*l == ' ') l++;
        if (!*l || *l == '#')
            continue;
        char *eq = strstr(l, " = ");
        if (!eq) { bad(path, ln, "no \" = \""); continue; }
        *eq = 0;
        struct item *t = &it[*n];
        memset(t, 0, sizeof *t);
        if (art) {
            char *sp = strchr(l, ' ');
            if (!sp || sp - l >= 8) { bad(path, ln, "no picture"); continue; }
            *sp = 0;
            strcpy(t->art, l);
            l = sp + 1;
        }
        t->label = strdup(l);
        t->cmd = dup_rest(eq + 3);
        (*n)++;
    }
    free(b);
}

static void bar_file(void)
{
    char path[ZPATH_MAX], *b = slurp(conf_file("bar", path)), *p, *l;
    char zone[3][160];
    for (int z = 0; z < 3; z++) strcpy(zone[z], th.bar[z]);
    if (b) {                            /* the file's zones replace the theme's (a zone it names) */
        int ln = 0, seen[3] = { 0, 0, 0 };
        p = b;
        while ((l = next_line(&p))) {
            ln++;
            while (*l == ' ') l++;
            if (!*l || *l == '#') continue;
            char *sp = strchr(l, ' ');
            if (!sp) { bad(path, ln, l); continue; }
            *sp++ = 0;
            int z = !strcmp(l, "left") ? 0 : !strcmp(l, "middle") ? 1 : !strcmp(l, "right") ? 2 : -1;
            if (z < 0) { bad(path, ln, l); continue; }
            if (!seen[z]++) zone[z][0] = 0;
            if (strlen(zone[z]) + strlen(sp) + 3 < sizeof zone[z]) {
                if (zone[z][0]) strcat(zone[z], " ; ");
                strcat(zone[z], sp);
            }
        }
        free(b);
    }
    /* each zone: modules, a command block being "cmd N COMMAND..." up to a ";" or the line's end */
    for (int z = 0; z < 3; z++) {
        char *s = zone[z], *w[24];
        int n = words(s, w, 24);
        for (int i = 0; i < n && nblocks < NBLOCK; i++) {
            struct block *k = &blocks[nblocks];
            memset(k, 0, sizeof *k);
            k->zone = (u8)z;
            k->fd = -1;
            if (!strcmp(w[i], ";")) continue;
            if (!strcmp(w[i], "zenix")) k->kind = B_ZENIX;
            else if (!strcmp(w[i], "menus")) k->kind = B_MENUS;
            else if (!strcmp(w[i], "windows")) k->kind = B_WINDOWS;
            else if (!strcmp(w[i], "workspaces")) k->kind = B_WORKSPACES;
            else if (!strcmp(w[i], "clock")) { k->kind = B_CLOCK; k->every = 1; }
            else if (!strcmp(w[i], "memory")) { k->kind = B_MEMORY; k->every = 10; }
            else if (!strcmp(w[i], "network")) { k->kind = B_NETWORK; k->every = 10; }
            else if (!strcmp(w[i], "cpu")) { k->kind = B_CPU; k->every = 3600; }
            else if (!strcmp(w[i], "cmd") && i + 2 < n) {
                k->kind = B_CMD;
                int e = atoi(w[i + 1]);
                if (e < 1) {
                    dprintf(2, "zdesk: the bar: a command every %d s: every 1 s, the least\n", e);
                    e = 1;              /* D38 revision 2: never under a second, so a 68020 is not kept busy */
                }
                k->every = (u16)e;
                char l[128] = "";
                i += 2;
                for (; i < n && strcmp(w[i], ";"); i++) {
                    if (strlen(l) + strlen(w[i]) + 2 >= sizeof l) break;
                    if (l[0]) strcat(l, " ");
                    strcat(l, w[i]);
                }
                k->cmd = strdup(l);
            } else {
                dprintf(2, "zdesk: the bar: %s: no such module\n", w[i]);
                continue;
            }
            nblocks++;
        }
    }
}

int conf_load(const char *theme)
{
    char path[ZPATH_MAX], name[16] = "flat";
    if (theme)
        strncpy(name, theme, 15);
    else {
        char *b = slurp(conf_file("theme", path));
        if (b) {
            char *w[2];
            if (words(b, w, 2) >= 1 && strlen(w[0]) < 16)
                strcpy(name, w[0]);
            free(b);
        }
    }
    memset(&th, 0, sizeof th);
    th.st.size = sizeof th.st;
    th.role[R_LABELBG] = 255;
    th.menus = M_CLICK | M_HOLD;
    th.drag = -1;
    for (int i = 0; i < 16; i++) th.map8[i] = (u8)(i & 7);
    strcpy(th.bar[0], "zenix menus");
    strcpy(th.bar[1], "windows");
    strcpy(th.bar[2], "memory clock");
    if (theme_load(name) < 0 && (!strcmp(name, "flat") || theme_load("flat") < 0))
        return -1;
    /* keys */
    char *b = slurp(conf_file("keys", path)), *p = b, *l;
    int ln = 0;
    while (b && (l = next_line(&p))) {
        ln++;
        char *w[8];
        int n = words(l, w, 8);
        if (n && key_line(w, n) < 0)
            bad(path, ln, w[0]);
    }
    free(b);
    item_file("menu", menu_items, &nmenu_items, 0);
    item_file("icons", icons, &nicons, 1);
    bar_file();
    return 0;
}
