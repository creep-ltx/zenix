/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's assigns and Execute's Assign command (zpath.h; docs/d29-zine.md 16.8) */
#include "zpath.h"

static zu8 up(zu8 c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
        return c - 0x20;
    return c;
}

/* a's n bytes and b (0-ended) the same but for case */
static int same(const char *a, zu32 n, const char *b)
{
    zu32 i;
    for (i = 0; i < n && b[i]; i++)
        if (up((zu8)a[i]) != up((zu8)b[i]))
            return 0;
    return i == n && !b[i];
}

static zu32 len(const char *s)
{
    zu32 n = 0;
    while (s[n])
        n++;
    return n;
}

static void cpy(char *d, const char *s, zu32 n)
{
    for (zu32 i = 0; i < n; i++)
        d[i] = s[i];
    d[n] = 0;
}

void za_init(struct za_table *t)
{
    zu8 *p = (zu8 *)t;
    for (zu32 i = 0; i < sizeof *t; i++)
        p[i] = 0;
    static const char *const dirs[3] = { "SYS:LIBS", "PROGDIR:", "RAM:" };
    za_set(t, "LIBS", 4, dirs, 3, 0);
}

static struct za_assign *find(const struct za_table *t, const char *name, zu32 n)
{
    for (zu32 i = 0; i < ZA_MAX; i++)
        if (t->a[i].name[0] && same(name, n, t->a[i].name))
            return (struct za_assign *)&t->a[i];
    return 0;
}

int za_set(struct za_table *t, const char *name, zu32 n, const char *const *dirs, zu32 k, int add)
{
    if (!n)
        return -ZE_INVAL;
    if (n > ZA_NAMEMAX)
        return -ZE_NAMETOOLONG;
    struct za_assign *a = find(t, name, n);
    if (!k && !add) {
        if (a)
            a->name[0] = 0, a->n = 0;
        return 0;
    }
    if (!a) {
        for (zu32 i = 0; i < ZA_MAX && !a; i++)
            if (!t->a[i].name[0])
                a = &t->a[i];
        if (!a)
            return -ZE_NOSPC;
        cpy(a->name, name, n);
        a->n = 0;
    } else if (!add)
        a->n = 0;
    if (a->n + k > ZA_TGT) {
        if (!a->n)
            a->name[0] = 0;
        return -ZE_NOSPC;
    }
    for (zu32 j = 0; j < k; j++) {
        zu32 l = len(dirs[j]);
        if (l > ZA_PATHMAX)
            l = ZA_PATHMAX;
        cpy(a->tgt[a->n++], dirs[j], l);
    }
    return 0;
}

/* dir and rest joined into out: a '/' between them unless dir ends in ':' or '/' (or rest is empty) */
static int join(const char *dir, const char *rest, char *out, zu32 max)
{
    zu32 a = len(dir), b = len(rest), sep = a && b && dir[a - 1] != ':' && dir[a - 1] != '/';
    if (a + sep + b + 1 > max)
        return 0;
    cpy(out, dir, a);
    if (sep)
        out[a] = '/';
    cpy(out + a + sep, rest, b);
    return 1;
}

int za_expand(const struct za_table *t, const char *path, zu32 i, char *out, zu32 max)
{
    char tmp[2 * (ZA_PATHMAX + 1)];
    const char *p = path;
    for (zu32 d = 0;; d++) {
        const char *c = p;
        while (*c && *c != ':')
            c++;
        const struct za_assign *a = *c ? find(t, p, (zu32)(c - p)) : 0;
        zu32 k = d ? 0 : i;             /* the i-th directory of the path's own assign, the first of any after it */
        if (!a || d == ZA_DEPTH) {
            if (!d && i)
                return 0;               /* an unassigned path stands only for itself */
            zu32 l = len(p);
            if (l + 1 > max)
                return 0;
            cpy(out, p, l);
            return 1;
        }
        if (k >= a->n)
            return 0;
        if (!join(a->tgt[k], c + 1, tmp, sizeof tmp))
            return 0;
        zu32 l = len(tmp);              /* into out: the next round's join reads out and writes tmp again */
        if (l + 1 > max)
            return 0;
        cpy(out, tmp, l);
        p = out;
    }
}

const char *za_ram(const char *path)
{
    const char *c = path;
    while (*c && *c != ':')
        c++;
    if (!*c)
        return 0;
    zu32 n = (zu32)(c - path);
    return same(path, n, "RAM") || same(path, n, "Ram Disk") ? c + 1 : 0;
}

/* part s (n bytes) after the key's top (out[0..*o), top its first byte past "DEV:"): 1, or 0 when it does not fit */
static int key_part(char *out, zu32 *o, zu32 top, const char *s, zu32 n, zu32 max)
{
    if (!n) {                           /* "/" alone: up one */
        if (*o == top)
            return 0;                   /* above the device's top */
        while (*o > top && out[*o - 1] != '/')
            (*o)--;
        if (*o > top)
            (*o)--;                     /* the '/' before the part that went */
        return 1;
    }
    zu32 sep = *o > top;
    if (*o + sep + n + 1 > max)
        return 0;
    if (sep)
        out[(*o)++] = '/';
    for (zu32 i = 0; i < n; i++)
        out[(*o)++] = s[i];
    return 1;
}

int za_key(const char *path, const char *vol, char *out, zu32 max)
{
    static const char *const sub[] = { "C", "S", "L", "LIBS", "DEVS", "FONTS" };
    const char *c = path, *rest = path, *first = 0;
    while (*c && *c != ':')
        c++;
    zu32 n = (zu32)(c - path), o;
    if (max < 5)
        return 0;
    if (!*c || !n || same(path, n, "SYS") || same(path, n, "PROGDIR") || (vol && vol[0] && same(path, n, vol)))
        cpy(out, "SYS:", 4);            /* the program's volume (no device: its own directory, the volume's top) */
    else if (same(path, n, "RAM") || same(path, n, "Ram Disk"))
        cpy(out, "RAM:", 4);
    else {
        for (zu32 k = 0; k < sizeof sub / sizeof sub[0]; k++)
            if (same(path, n, sub[k]))
                first = sub[k];         /* the volume's own directory of that name */
        if (first)
            cpy(out, "SYS:", 4);
        else {
            if (n + 2 > max)
                return 0;
            for (zu32 i = 0; i < n; i++)
                out[i] = (char)up((zu8)path[i]);
            out[n] = ':';
            out[n + 1] = 0;
        }
    }
    if (*c)
        rest = c + 1;
    zu32 top = len(out);
    o = top;
    if (first && !key_part(out, &o, top, first, len(first), max))
        return 0;
    for (const char *p = rest; *p;) {
        const char *s = p;
        while (*p && *p != '/')
            p++;
        if (*p == '/' && !p[1] && p > s) {      /* a '/' at the end: the part, no going up */
            if (!key_part(out, &o, top, s, (zu32)(p - s), max))
                return 0;
            break;
        }
        if (!key_part(out, &o, top, s, (zu32)(p - s), max))
            return 0;
        if (*p == '/')
            p++;
    }
    out[o] = 0;
    return 1;
}

/* the next word of the command (quoted or not) into w (max bytes with its 0): its length, or -1 too long; *pp moved on.
 * 0 at the end of the command */
static int word(const char **pp, const char *end, char *w, zu32 max)
{
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    zu32 n = 0;
    int q = p < end && *p == '"';
    if (q)
        p++;
    while (p < end && (q ? *p != '"' : *p != ' ' && *p != '\t')) {
        if (n + 1 >= max)
            return -1;
        w[n++] = *p++;
    }
    if (q && p < end)
        p++;
    w[n] = 0;
    *pp = p;
    return (int)n;
}

int za_parse(const char *cmd, zu32 len_, struct za_cmd *c)
{
    const char *end = cmd;
    while ((zu32)(end - cmd) < len_ && *end && *end != '\n' && *end != ';')
        end++;
    const char *p = cmd;
    char w[ZA_PATHMAX + 1];
    int n = word(&p, end, w, sizeof w);
    if (n <= 0)
        return 0;
    const char *b = w;                  /* the command's own name: after its path ("Ram:Assign", "C:Assign") */
    for (const char *s = w; *s; s++)
        if (*s == ':' || *s == '/')
            b = s + 1;
    if (!same(b, len(b), "ASSIGN"))
        return 0;
    zu8 *z = (zu8 *)c;
    for (zu32 i = 0; i < sizeof *c; i++)
        z[i] = 0;
    n = word(&p, end, w, sizeof w);
    if (n <= 1 || w[n - 1] != ':')
        return -1;                      /* a listing, an option first, or no "NAME:" */
    for (int i = 0; i < n - 1; i++)
        if (w[i] == ':' || w[i] == '/')
            return -1;
    if (n - 1 > ZA_NAMEMAX)
        return -1;
    cpy(c->name, w, (zu32)n - 1);
    while ((n = word(&p, end, w, sizeof w)) != 0) {
        if (n < 0)
            return -1;
        if (same(w, (zu32)n, "ADD"))
            c->add = 1;
        else if (same(w, (zu32)n, "REMOVE"))
            c->remove = 1;
        else if (same(w, (zu32)n, "DEFER") || same(w, (zu32)n, "PATH"))
            ;                           /* a plain assign is the same while nothing changes under it */
        else if (same(w, (zu32)n, "DISMOUNT") || same(w, (zu32)n, "LIST") || same(w, (zu32)n, "EXISTS") ||
                 c->n == ZA_TGT)
            return -1;
        else
            cpy(c->dir[c->n++], w, (zu32)n);
    }
    if (c->remove && c->n)
        return -1;                      /* removing one directory of several: ZINE removes whole assigns only */
    if (c->add && !c->n)
        return -1;
    return 1;
}

int za_apply(struct za_table *t, const struct za_cmd *c)
{
    const char *d[ZA_TGT];
    for (zu32 i = 0; i < ZA_TGT; i++)
        d[i] = c->dir[i];
    return za_set(t, c->name, len(c->name), d, c->remove ? 0 : c->n, c->add);
}

/* ---- Execute's command lines (zpath.h; docs/d29-zine.md 16.11) ---- */
static int blank(char ch) { return ch == ' ' || ch == '\t'; }

int zx_parse(const char *cmd, zu32 len_, struct zx_cmd *c)
{
    zu8 *z = (zu8 *)c;
    for (zu32 i = 0; i < sizeof *c; i++)
        z[i] = 0;
    const char *end = cmd, *stop = cmd + len_;
    while (end < stop && *end && *end != '\n')
        end++;
    c->next = (zu32)(end - cmd) + (end < stop && *end == '\n');
    const char *e = cmd;                /* the command proper ends at a ';' outside quotes */
    for (int q = 0; e < end && (q || *e != ';'); e++)
        if (*e == '"')
            q = !q;
    end = e;
    const char *p = cmd;
    char w[ZA_PATHMAX + 1];
    int named = 0;
    for (;;) {
        while (p < end && blank(*p))
            p++;
        if (p >= end)
            break;
        const char *s = p;
        if (*p == '<' || *p == '>') {   /* a redirection: before the name, or right after it */
            int out = *p == '>', app = out && p + 1 < end && p[1] == '>';
            p += 1 + app;
            int n = word(&p, end, w, sizeof w);
            if (n < 0)
                return -1;
            if (!n)
                return -1;              /* '>' with no file */
            cpy(out ? c->out : c->in, w, (zu32)n);
            if (out)
                c->append = app;
            continue;
        }
        if (named)
            break;                      /* the arguments begin */
        int n = word(&p, end, w, sizeof w);
        if (n < 0)
            return -1;
        if (!c->run && same(w, (zu32)n, "RUN") && *s != '"') {
            c->run = 1;
            continue;
        }
        cpy(c->name, w, (zu32)n);
        c->at = (zu32)(s - cmd);
        named = 1;
    }
    if (!named)
        return c->in[0] || c->out[0] ? -1 : 0;
    const char *a = p, *b = end;
    while (b > a && blank(b[-1]))
        b--;
    c->args = (zu32)(a - cmd), c->arglen = (zu32)(b - a);
    return 1;
}

int zx_known(const char *name)
{
    /* AmigaOS 3.1's Shell commands and its C: directory (the AmigaDOS Manual's command list), and the common
     * additions; Assign is ZINE's own (za_parse), Run is taken off before (zx_parse) */
    static const char list[] =
        "ADDBUFFERS\0ADDDATATYPES\0ALIAS\0ASK\0AVAIL\0BINDDRIVERS\0BREAK\0CD\0CHANGETASKPRI\0CONCLIP\0COPY\0CPU\0"
        "DATE\0DELETE\0DIR\0DISKCHANGE\0ECHO\0ED\0EDIT\0ELSE\0ENDCLI\0ENDIF\0ENDSHELL\0ENDSKIP\0EVAL\0EXECUTE\0FAILAT\0"
        "FAULT\0FILENOTE\0FORMAT\0GET\0GETENV\0ICONX\0IF\0INFO\0INSTALL\0IPREFS\0JOIN\0LAB\0LIST\0LOADRESOURCE\0LOADWB\0"
        "LOCK\0MAKEDIR\0MAKELINK\0MOUNT\0NEWCLI\0NEWSHELL\0PATH\0PROMPT\0PROTECT\0QUIT\0RELABEL\0REMRAD\0RENAME\0"
        "REQUESTCHOICE\0REQUESTFILE\0RESIDENT\0SEARCH\0SET\0SETCLOCK\0SETDATE\0SETENV\0SETFONT\0SETKEYBOARD\0SETPATCH\0"
        "SKIP\0SORT\0STACK\0STATUS\0TYPE\0UNALIAS\0UNSET\0UNSETENV\0VERSION\0WAIT\0WHICH\0WHY\0";
    const char *b = name;
    for (const char *s = name; *s; s++)
        if (*s == ':' || *s == '/')
            b = s + 1;
    zu32 n = len(b);
    for (const char *l = list; *l; l += len(l) + 1)
        if (same(b, n, l))
            return 1;
    return 0;
}
