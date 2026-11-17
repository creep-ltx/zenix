/* SPDX-License-Identifier: GPL-2.0-only */
/* Configurable keys for the desktop's programs (display/keys.h): the parser files had (DESIGN D48 4), shared with show
 * (D50). */
#include "libz.h"
#include "../keys.h"

typedef unsigned char u8;
typedef unsigned short u16;

static const struct { const char *n; u16 k; } knames[] = {
    { "Up", KM_UP }, { "Down", KM_DOWN }, { "Left", KM_LEFT }, { "Right", KM_RIGHT },
    { "S-Up", KM_SUP }, { "S-Down", KM_SDOWN }, { "S-Left", KM_SLEFT }, { "S-Right", KM_SRIGHT },
    { "C-Up", KM_CUP }, { "C-Down", KM_CDOWN }, { "C-Left", KM_CLEFT }, { "C-Right", KM_CRIGHT },
    { "Return", 13 }, { "Esc", 27 }, { "Tab", 9 }, { "Space", ' ' }, { "Del", 127 }, { "BS", 8 }, { "Help", KM_HELP },
};

/* one key from s (a character or a <Name>) -> its code, s moved past it; 0: not a key */
static int key_parse(const char **sp)
{
    const char *s = *sp;
    if (*s == '<' && s[1] && s[1] != ' ') {
        const char *e = strchr(s, '>');
        if (e && e > s + 1) {
            int n = (int)(e - s - 1);
            *sp = e + 1;
            for (u32 i = 0; i < sizeof knames / sizeof knames[0]; i++)
                if ((int)strlen(knames[i].n) == n && !strncmp(knames[i].n, s + 1, (u32)n))
                    return knames[i].k;
            if (n == 3 && s[1] == 'C' && s[2] == '-' && isalpha((u8)s[3]))
                return s[3] & 0x1F;
            if ((n == 2 || n == 3) && s[1] == 'F' && atoi(s + 2) >= 1 && atoi(s + 2) <= 10)
                return KM_F1 + atoi(s + 2) - 1;
            return 0;
        }
    }
    u8 c = (u8)*s;
    if (c <= ' ')
        return 0;
    if (c < 0x80) {
        *sp = s + 1;
        return c;
    }
    int cp = 0, n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;      /* a Latin-1 letter in UTF-8 (å, ö) */
    cp = c & (0x3F >> n);
    for (int i = 1; i <= n; i++) {
        if (((u8)s[i] & 0xC0) != 0x80) return 0;
        cp = cp << 6 | ((u8)s[i] & 0x3F);
    }
    *sp = s + n + 1;
    return cp < 300 && cp > 127 ? cp : 0;
}
void keys_name(int k, char *o)
{
    for (u32 i = 0; i < sizeof knames / sizeof knames[0]; i++)
        if (knames[i].k == k) { snprintf(o, 12, "<%s>", knames[i].n); return; }
    if (k >= KM_F1 && k < KM_F1 + 10) { snprintf(o, 12, "<F%d>", k - KM_F1 + 1); return; }
    if (k < 32) { snprintf(o, 12, "<C-%c>", k | 0x60); return; }
    if (k < 128) { o[0] = (char)k; o[1] = 0; return; }
    o[0] = (char)(0xC0 | k >> 6);       /* 128 .. 299: two bytes of UTF-8 */
    o[1] = (char)(0x80 | (k & 0x3F));
    o[2] = 0;
}
static void bad(struct keyset *ks, int line, const char *why)
{
    if (!ks->bad[0])
        snprintf(ks->bad, sizeof ks->bad, "keys: line %d: %s", line, why);
}
void keys_text(struct keyset *ks, const char *t, int file)
{
    int line = 0, other = 0;
    while (*t) {
        const char *e = strchr(t, '\n');
        int n = e ? (int)(e - t) : (int)strlen(t);
        char l[96];
        line++;
        const char *f = t;
        while (f < t + n && (*f == ' ' || *f == '\t'))
            f++;
        if (n >= (int)sizeof l) {
            if (file && *f != ';' && *f != '#') bad(ks, line, "too long");
            t += n + (e ? 1 : 0);
            continue;
        }
        memcpy(l, t, (u32)n);
        l[n] = 0;
        t += n + (e ? 1 : 0);
        char *s = l;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == ';' || *s == '#' || *s == '\r')
            continue;
        if (*s == '[') {
            other = strncmp(s, "[main]", 6) != 0;      /* Ahiru's [view] and others: not these keys */
            continue;
        }
        if (other)
            continue;
        const char *p = s;
        int k1 = key_parse(&p), k2 = 0;
        if (k1 && *p && *p != ' ' && *p != '\t')
            k2 = key_parse(&p);
        if (!k1 || (*p && *p != ' ' && *p != '\t')) {
            if (file) bad(ks, line, "not a key");
            continue;
        }
        while (*p == ' ' || *p == '\t') p++;
        char an[24];
        int k = 0;
        while (p[k] && p[k] != ' ' && p[k] != '\t' && p[k] != '\r' && k < 23) { an[k] = p[k]; k++; }
        an[k] = 0;
        p += k;
        while (*p == ' ' || *p == '\t') p++;
        int a = -1;
        for (int i = 0; i < ks->nact; i++)
            if (!strcmp(an, ks->names[i])) a = i;
        if (a < 0) {
            if (file) { char w[48]; snprintf(w, sizeof w, "unknown action %s", an); bad(ks, line, w); }
            continue;
        }
        int slot = ks->n;
        for (int i = 0; i < ks->n; i++)
            if (ks->b[i].k1 == k1 && ks->b[i].k2 == k2) slot = i;
        if (slot == ks->max) {
            if (file) bad(ks, line, "too many keys");
            continue;
        }
        if (slot == ks->n) ks->n++;
        struct kbind *b = &ks->b[slot];
        b->k1 = (u16)k1;
        b->k2 = (u16)k2;
        b->act = (u8)a;
        if (strlen(p) >= sizeof b->arg && file)
            bad(ks, line, "its argument is too long (26 bytes at most)");
        snprintf(b->arg, sizeof b->arg, "%s", p);
        char *r = strchr(b->arg, '\r');
        if (r) *r = 0;
    }
}
int keys_load(struct keyset *ks, const char *def, unsigned deflen, const char *prog)
{
    int wrote = 0;
    keys_text(ks, def, 0);
    const char *h = getenv("HOME");
    if (!h || !*h || strlen(h) > 200 || strlen(prog) > 20)
        return 0;
    char path[ZPATH_MAX];
    snprintf(path, sizeof path, "%s/.config/%s/keys", h, prog);
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) {                       /* none: written with every built-in key (Ahiru's way) */
        snprintf(path, sizeof path, "%s/.config", h);
        sys_mkdir(path);
        strcat(path, "/");
        strcat(path, prog);
        sys_mkdir(path);
        strcat(path, "/keys");
        fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd >= 0) {
            wrote = sys_write(fd, def, deflen) == (int)deflen;
            sys_close(fd);
        }
        return wrote;
    }
    struct ustat st;
    char *t = 0;
    if (sys_fstat(fd, &st) == 0 && st.size < 16384 && (t = malloc(st.size + 1))) {
        int n = sys_read(fd, t, st.size);
        t[n > 0 ? n : 0] = 0;
        keys_text(ks, t, 1);
    } else
        snprintf(ks->bad, sizeof ks->bad, "keys: %s is too big (16 KB at most)", path);
    free(t);
    sys_close(fd);
    for (int i = 0; i < ks->n; i++)     /* a key that also starts a prefix is never reached */
        if (ks->b[i].act && !ks->b[i].k2)
            for (int j = 0; j < ks->n; j++)
                if (ks->b[j].act && ks->b[j].k2 && ks->b[j].k1 == ks->b[i].k1) {
                    char kn[12];
                    keys_name(ks->b[i].k1, kn);
                    if (!ks->bad[0]) snprintf(ks->bad, sizeof ks->bad, "keys: %s starts a prefix: its own key is not reached", kn);
                }
    return 0;
}
int keys_prefix(const struct keyset *ks, int k)
{
    for (int i = 0; i < ks->n; i++)
        if (ks->b[i].act && ks->b[i].k1 == k && ks->b[i].k2)
            return 1;
    return 0;
}
const struct kbind *keys_find(const struct keyset *ks, int k1, int k2)
{
    for (int i = 0; i < ks->n; i++)
        if (ks->b[i].act && ks->b[i].k1 == k1 && ks->b[i].k2 == k2)
            return &ks->b[i];
    return 0;
}
int keys_code(const struct dpyev *e)
{
    int c = e->code, sh = e->qual & (GQ_LSHIFT | GQ_RSHIFT), ct = e->qual & GQ_CTRL;
    if (c >= 0x4C && c <= 0x4F) {
        static const u16 arrows[4] = { KM_UP, KM_DOWN, KM_RIGHT, KM_LEFT };
        int a = arrows[c - 0x4C];
        return a + (sh ? KM_SUP - KM_UP : ct ? KM_CUP - KM_UP : 0);
    }
    if (c >= 0x50 && c <= 0x59) return KM_F1 + c - 0x50;
    switch (c) {
    case 0x5F: return KM_HELP;
    case 0x44: case 0x43: return 13;
    case 0x45: return 27;
    case 0x42: return 9;
    case 0x40: return ' ';
    case 0x46: return 127;
    case 0x41: return 8;
    }
    u8 b = (u8)e->ch[0];
    if (!b) return 0;
    if (ct && isalpha(b)) return b & 0x1F;
    if (b < 0x80) return b;
    if (b >= 0xC2 && b < 0xE0 && ((u8)e->ch[1] & 0xC0) == 0x80) {
        int cp = (b & 0x1F) << 6 | ((u8)e->ch[1] & 0x3F);
        return cp < 300 ? cp : 0;
    }
    return 0;
}

/* a key from a terminal (DESIGN D59: one reader for irc and mail): a character (a byte of UTF-8 text: 0x10000 + the
 * byte), the code of a sequence (xterm's and Zenix's console's: arrows with Shift and Ctrl, F1-F10, Home and End as
 * <C-a> and <C-e>, Del as <C-d>, PageUp and PageDown as <S-Up> and <S-Down>, Help), 27 for a lone Esc, 0 for a
 * sequence not known, -1 at the end of the input */
static int tgetb(int fd, int ms)
{
    unsigned char c;
    struct pollfd pf = { fd, POLLIN, 0 };
    if (poll(&pf, 1, ms) <= 0 || sys_read(fd, &c, 1) != 1) return -1;
    return c;
}
int keys_tty(int fd)
{
    unsigned char c;
    if (sys_read(fd, &c, 1) != 1) return -1;
    if (c == 10) return 13;
    if (c >= 0x80) return 0x10000 | c;
    if (c != 27) return c;
    int s1 = tgetb(fd, 30), ch = 0, num = 0, mod = 0, semi = 0;
    if (s1 < 0) return 27;
    if (s1 == 'O') { ch = tgetb(fd, 30); return ch >= 'P' && ch <= 'S' ? KM_F1 + ch - 'P' : ch == 'A' ? KM_UP :
                     ch == 'B' ? KM_DOWN : ch == 'C' ? KM_RIGHT : ch == 'D' ? KM_LEFT : ch == 'H' ? 1 : ch == 'F' ? 5 : 0; }
    if (s1 != '[') return 0;
    while ((ch = tgetb(fd, 30)) >= 0 && ((ch >= '0' && ch <= '9') || ch == ';')) {
        if (ch == ';') semi = 1;
        else if (semi) mod = mod * 10 + ch - '0';
        else num = num * 10 + ch - '0';
    }
    if (ch >= 'A' && ch <= 'D') {       /* xterm's modified arrows: ESC [ 1 ; m A (2 Shift, 5 Ctrl) */
        int b = mod == 2 ? KM_SUP : mod >= 5 ? KM_CUP : KM_UP;
        static const unsigned char o[4] = { 0, 1, 3, 2 };   /* A up, B down, C right, D left */
        return b + o[ch - 'A'];
    }
    switch (ch) {
    case 'H': return 1;                 /* Home */
    case 'F': return 5;                 /* End */
    case 'Z': return 0;                 /* Shift-Tab */
    case 'P': case 'Q': case 'R': case 'S': return KM_F1 + ch - 'P';
    case '~':
        if (num == 1 || num == 7) return 1;
        if (num == 4 || num == 8) return 5;
        if (num == 3) return 4;         /* Del: as Ctrl-D */
        if (num == 5) return KM_SUP;
        if (num == 6) return KM_SDOWN;
        if (num == 2) return KM_HELP;   /* Help (Insert) */
        return num >= 11 && num <= 15 ? KM_F1 + num - 11 : num >= 17 && num <= 21 ? KM_F1 + num - 12 : 0;
    }
    return 0;
}
