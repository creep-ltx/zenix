/* SPDX-License-Identifier: GPL-2.0-only */
/* files: the file view of Zenix Desktop (DESIGN D48, docs/d48-fileview.md, docs/man/files.txt). A window that lists a
 * folder (names, sizes, dates; folders first), Ahiru's model: its vi keys, read from a keys file of Ahiru.keys's
 * format (~/.config/files/keys, written with the built-in keys when missing), and the mouse (a click picks a row, a
 * double click opens it). Lean: a folder is its readdir records (names and types) sorted, and only the rows on view
 * are lstat-ed. Opening runs a program in a terminal window (term -e): text in less, E the editor, a program after a
 * question, a picture in show (D50: its own window, no terminal), anything else what the user types. Copy, move and
 * delete are the commands themselves (cp -r, mv, rm -r), rename and a new folder the system calls; everything that
 * could lose a file asks first.
 * -v writes every row it draws, the window's place and what it did to its standard output (the tests read it). */
#include "libz.h"
#include "../display.h"
#include "../keys.h"
typedef unsigned char u8;
typedef unsigned short u16;

#define MAXENT  4096                    /* entries a folder (d48 3: past it the listing says so) */
#define MAXPOOL (64 * 1024)             /* bytes of names a folder */
#define MAXCOLS 200
#define KEYFILE "/.config/files/keys"   /* under $HOME */

static int verbose, win = -1, cols, rows, focus = 1, hidden;
static struct dpyinfo where;

/* ================================================================ the listing */
struct ent {
    u32 name;                           /* offset into the pool */
    u32 size, mtime;
    u16 mode, uid;
    u8 type;                            /* readdir's: 1 file, 2 folder, 7 link (0 unknown: lstat-ed while reading) */
    u8 fl;
};
#define F_STAT 1                        /* size, mtime, mode, uid read */
#define F_MARK 2
struct list {
    char *dir;                          /* absolute, malloc'd; 0: no list */
    struct ent *e;
    int n, cap, cur, top, cut;          /* cut: the folder had more than MAXENT entries or MAXPOOL of names */
    char *pool;
    u32 used, pcap;
};
static struct list L, P;                /* the folder, its parent (only while the parent column is shown) */
static int sortby, sortrev;             /* 0 name, 1 size, 2 time, 3 extension */
#define NM(l, i) ((l)->pool + (l)->e[i].name)

static char pb[ZPATH_MAX * 2];          /* a path being made (never recursive) */
static const char *pjoin(const char *dir, const char *name)
{
    int dl = (int)strlen(dir);
    if (dl + 1 + (int)strlen(name) >= ZPATH_MAX)
        return 0;
    strcpy(pb, dir);
    if (dl > 1)
        strcat(pb, "/");
    strcat(pb, name);
    return pb;
}
static char *xjoin(const char *dir, const char *name)
{
    const char *p = pjoin(dir, name);
    return p ? strdup(p) : 0;
}
/* the folder above dir ("/a/b" -> "/a", "/a" -> "/"), malloc'd */
static char *parent_of(const char *dir)
{
    char *p = strdup(dir);
    if (!p)
        return 0;
    char *s = strrchr(p, '/');
    if (s == p)
        p[1] = 0;
    else if (s)
        *s = 0;
    return p;
}
static const char *base_of(const char *dir)
{
    const char *s = strrchr(dir, '/');
    return s && s[1] ? s + 1 : dir;
}

static void list_free(struct list *l)
{
    free(l->dir);
    free(l->e);
    free(l->pool);
    memset(l, 0, sizeof *l);
}
static void ent_stat(struct list *l, int i)
{
    struct ent *e = &l->e[i];
    if (e->fl & F_STAT)
        return;
    e->fl |= F_STAT;
    struct ustat st;
    const char *p = pjoin(l->dir, NM(l, i));
    if (!p || sys_lstat(p, &st) < 0)
        return;
    e->size = st.size;
    e->mtime = st.mtime;
    e->mode = (u16)st.mode;
    e->uid = (u16)st.uid;
    if (!e->type)
        e->type = (st.mode & S_IFMT) == S_IFDIR ? 2 : (st.mode & S_IFMT) == S_IFLNK ? 7 : 1;
}
static int ci_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = tolower((u8)*a), y = tolower((u8)*b);
        if (x != y || !x)
            return x - y;
    }
}
static const char *ext_of(const char *n)
{
    const char *d = strrchr(n, '.');
    return d && d != n ? d + 1 : "";
}
static struct list *sl;                 /* the list qsort is sorting */
static int ent_cmp(const void *pa, const void *pb2)
{
    const struct ent *a = pa, *b = pb2;
    int da = a->type == 2, db = b->type == 2;
    if (da != db)
        return db - da;                 /* folders first, whichever order */
    const char *na = sl->pool + a->name, *nb = sl->pool + b->name;
    int c = 0;
    if (sortby == 1)
        c = a->size < b->size ? -1 : a->size > b->size;
    else if (sortby == 2)
        c = a->mtime > b->mtime ? -1 : a->mtime < b->mtime;     /* the newest first, as ls -t */
    else if (sortby == 3)
        c = ci_cmp(ext_of(na), ext_of(nb));
    if (!c)
        c = ci_cmp(na, nb);
    if (!c)
        c = strcmp(na, nb);
    return sortrev ? -c : c;
}
static void list_sort(struct list *l)
{
    if (sortby == 1 || sortby == 2)
        for (int i = 0; i < l->n; i++)
            ent_stat(l, i);
    sl = l;
    qsort(l->e, (size_t)l->n, sizeof *l->e, ent_cmp);
}
static int list_find(const struct list *l, const char *name)
{
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->pool + l->e[i].name, name))
            return i;
    return -1;
}
/* dir read into l (names and types, sorted) -> 0, or -errno */
static int list_read(struct list *l, const char *dir)
{
    memset(l, 0, sizeof *l);
    struct zdir *z = zdir_open(dir);    /* the records in batches, each with its lstat where given (DESIGN D49) */
    if (!z)
        return -zerrno;
    l->dir = strdup(dir);
    l->cap = 64;
    l->pcap = 2048;
    l->e = malloc((u32)l->cap * sizeof *l->e);
    l->pool = malloc(l->pcap);
    if (!l->dir || !l->e || !l->pool) {
        zdir_close(z);
        list_free(l);
        return -ENOMEM;
    }
    const struct udirx *d;
    while ((d = zdir_next(z))) {
        if (d->name[0] == '.' && (!hidden || !d->name[1] || (d->name[1] == '.' && !d->name[2])))
            continue;
        u32 nl = (u32)strlen(d->name) + 1;
        if (l->n == MAXENT || l->used + nl > MAXPOOL) {
            l->cut = 1;
            break;
        }
        if (l->n == l->cap) {
            struct ent *ne = realloc(l->e, (u32)l->cap * 2 * sizeof *l->e);
            if (!ne) { l->cut = 1; break; }
            l->e = ne;
            l->cap *= 2;
        }
        if (l->used + nl > l->pcap) {
            u32 nc = l->pcap * 2 > MAXPOOL ? MAXPOOL : l->pcap * 2;
            char *np = realloc(l->pool, nc);
            if (!np) { l->cut = 1; break; }
            l->pool = np;
            l->pcap = nc;
        }
        struct ent *e = &l->e[l->n];
        memset(e, 0, sizeof *e);
        e->name = l->used;
        memcpy(l->pool + l->used, d->name, nl);
        l->used += nl;
        e->type = d->type == 1 || d->type == 2 || d->type == 7 ? (u8)d->type : d->type ? 1 : 0;
        if (d->flags & UDX_STAT) {      /* what ent_stat would ask: no lstat for this row */
            e->size = d->st.size;
            e->mtime = d->st.mtime;
            e->mode = (u16)d->st.mode;
            e->uid = (u16)d->st.uid;
            e->fl = F_STAT;
            if (!e->type)
                e->type = (d->st.mode & S_IFMT) == S_IFDIR ? 2 : (d->st.mode & S_IFMT) == S_IFLNK ? 7 : 1;
        } else if (!e->type)
            ent_stat(l, l->n);          /* a filesystem that does not say (ext2 without the file-type feature) */
        l->n++;
    }
    zdir_close(z);
    list_sort(l);
    return 0;
}

/* ================================================================ the window's text */
static char msg[160];                   /* the status line's message (0: the entry's details) */
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (verbose)
        dprintf(1, "files: say: %s\n", msg);
}
/* n bytes of UTF-8 -> their cells (every byte that is not a continuation) */
static int ncells(const char *s, int n)
{
    int c = 0;
    for (int i = 0; i < n; i++)
        if (((u8)s[i] & 0xC0) != 0x80)
            c++;
    return c;
}
/* s put into b at *bl in exactly w cells: cut with '~' at its end (left: at its start), else padded with spaces */
/* s with every broken UTF-8 sequence's bytes made '?' (displayd draws such a byte as one Latin-1 cell, D38: the
 * cells counted here must be the ones drawn); at most sizeof clean_b - 1 bytes */
static char clean_b[ZPATH_MAX + 96];
static const char *clean(const char *s)
{
    int o = 0;
    for (int i = 0; s[i] && o < (int)sizeof clean_b - 5; ) {
        u8 c = (u8)s[i];
        int k = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
        for (int j = 1; j < k; j++)
            if (((u8)s[i + j] & 0xC0) != 0x80)
                k = 0;
        if (!k) {
            clean_b[o++] = '?';
            i++;
            continue;
        }
        memcpy(clean_b + o, s + i, (u32)k);
        o += k;
        i += k;
    }
    clean_b[o] = 0;
    return clean_b;
}
static void fit(char *b, int *bl, const char *s0, int w, int left)
{
    const char *s = clean(s0);
    int n = (int)strlen(s), c = ncells(s, n);
    if (w <= 0)
        return;
    if (c > w) {
        int keep = w - 1, i;
        if (left) {
            b[(*bl)++] = '~';
            for (i = n; i > 0 && keep > 0; ) {      /* the last keep cells */
                i--;
                if (((u8)s[i] & 0xC0) != 0x80)
                    keep--;
            }
            while (i > 0 && ((u8)s[i] & 0xC0) == 0x80)
                i--;
            memcpy(b + *bl, s + i, (u32)(n - i));
            *bl += n - i;
        } else {
            for (i = 0; i < n && keep > 0; keep--)
                for (i++; i < n && ((u8)s[i] & 0xC0) == 0x80; i++)
                    ;
            memcpy(b + *bl, s, (u32)i);
            *bl += i;
            b[(*bl)++] = '~';
        }
        return;
    }
    memcpy(b + *bl, s, (u32)n);
    *bl += n;
    for (; c < w; c++)
        b[(*bl)++] = ' ';
}
static void sizestr(u32 s, char *o)
{
    static const char u[] = "KMG";
    if (s < 1024) {
        snprintf(o, 8, "%u", s);
        return;
    }
    u32 v = s, k = 0, r = 0;
    while (v >= 1024 && k < 3) {
        r = v % 1024;
        v /= 1024;
        k++;
    }
    if (v < 10)
        snprintf(o, 8, "%u.%u%c", v, r * 10 / 1024, u[k - 1]);
    else
        snprintf(o, 8, "%u%c", v, u[k - 1]);
}
static int leap(int y) { return (y % 4 == 0 && y % 100) || y % 400 == 0; }
/* as ls -l: "Nov 13 02:47", or "Nov 13  2025" more than half a year from now */
static void datestr(u32 t, char *o)
{
    static const char mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    static const u8 ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    u32 now = sys_time(), days = t / 86400, rem = t % 86400;
    int y = 1970, m = 0;
    for (;;) {
        u32 yl = leap(y) ? 366 : 365;
        if (days < yl) break;
        days -= yl;
        y++;
    }
    for (; m < 11; m++) {
        u32 l = ml[m] + (m == 1 && leap(y));
        if (days < l) break;
        days -= l;
    }
    int old = t + 183u * 86400 < now || t > now + 86400;
    if (old)
        snprintf(o, 16, "%.3s %2u  %d", mon + m * 3, days + 1, y);
    else
        snprintf(o, 16, "%.3s %2u %02u:%02u", mon + m * 3, days + 1, rem / 3600, rem / 60 % 60);
}

/* ================================================================ the layout */
/* rows: 0 the path, 1 .. rows - 2 the list (lrows), rows - 1 the status. The parent column (pw cells and a gap) when
 * the window is at least 72 cells wide */
static int lrows(void) { return rows > 2 ? rows - 2 : 0; }
static int pw(void) { return cols >= 72 ? cols / 4 : 0; }
static int lx(void) { return pw() ? pw() + 1 : 0; }    /* the list's first cell */
static int lw(void) { return cols - lx(); }
#define PEN_BG   2
#define PEN_FG   1
#define PEN_SEL  4
#define PEN_SELF 7
#define PEN_DIM  5
#define PEN_MARK 9

static char rb[MAXCOLS * 4 + 8];
static void text_at(int cx, int cy, int fg, int bg, int attr, const char *s, int n)
{
    dpy_text(win, cx * 8, cy * 8, fg, bg, attr, s, n);
}
/* the list's row for entry i (its screen row from L.top) */
static void draw_ent(int i)
{
    int y = 1 + i - L.top;
    if (y < 1 || y > lrows())
        return;
    int w = lw(), bl = 0;
    if (i >= L.n) {
        dpy_fill(win, lx() * 8, y * 8, w * 8, 8, PEN_BG);
        return;
    }
    struct ent *e = &L.e[i];
    ent_stat(&L, i);
    int ss = w >= 30, ds = w >= 40, nw = w - 1 - (ss ? 6 : 0) - (ds ? 13 : 0);
    rb[bl++] = e->fl & F_MARK ? '*' : ' ';
    char nm[72];
    snprintf(nm, sizeof nm, "%s%s", NM(&L, i), e->type == 2 ? "/" : e->type == 7 ? "@" : "");
    fit(rb, &bl, nm, nw, 0);
    if (ss) {
        char s[8] = "";
        if (e->type != 2)
            sizestr(e->size, s);
        bl += snprintf(rb + bl, 8, " %5s", s);
    }
    if (ds) {
        char d[16];
        datestr(e->mtime, d);
        bl += snprintf(rb + bl, 16, " %-12s", d);
    }
    int cur = i == L.cur, fg = cur ? (focus ? PEN_SELF : PEN_FG) : e->fl & F_MARK ? PEN_MARK : PEN_FG;
    int bg = cur ? (focus ? PEN_SEL : PEN_DIM) : PEN_BG;
    text_at(lx(), y, fg, bg, e->type == 2 ? TA_BOLD : 0, rb, bl);
    if (verbose)
        dprintf(1, "files: row %d%s: %.*s\n", y, cur ? "*" : "", bl, rb);
}
static void draw_parent(void)
{
    int w = pw();
    if (!w)
        return;
    int n = lrows();
    if (P.dir) {                        /* the folder in its parent kept on view */
        if (P.cur < P.top) P.top = P.cur;
        if (P.cur >= P.top + n) P.top = P.cur - n + 1;
    }
    for (int r = 0; r < n; r++) {
        int i = P.top + r, bl = 0;
        if (!P.dir || i >= P.n || i < 0) {
            dpy_fill(win, 0, (r + 1) * 8, (w + 1) * 8, 8, PEN_BG);
            continue;
        }
        rb[bl++] = ' ';
        char nm[72];
        snprintf(nm, sizeof nm, "%s%s", NM(&P, i), P.e[i].type == 2 ? "/" : "");
        fit(rb, &bl, nm, w - 1, 0);
        rb[bl++] = ' ';
        text_at(0, r + 1, PEN_FG, i == P.cur ? PEN_DIM : PEN_BG, P.e[i].type == 2 ? TA_BOLD : 0, rb, bl);
    }
}
static void draw_head(void)
{
    int bl = 0;
    char t[ZPATH_MAX + 16];
    snprintf(t, sizeof t, " %s", L.dir ? L.dir : "?");
    fit(rb, &bl, t, cols, 1);
    text_at(0, 0, PEN_FG, PEN_DIM, TA_BOLD, rb, bl);
}

/* the status line: a message, a question or the line being typed, else the entry's details */
enum { M_LIST, M_ASK, M_LINE, M_HELP };
static int mode, askwhat;               /* askwhat: what a yes is for ('p' paste, 'd' delete, 'r' run) */
static char ed[ZPATH_MAX], edp[48];     /* the line typed and its prompt */
static int edlen, edpos;
static void draw_status(void)
{
    int bl = 0, y = rows - 1, curc = -1;
    char t[ZPATH_MAX + 64];
    if (mode == M_LINE) {
        snprintf(t, sizeof t, "%s%.*s", edp, edlen, ed);
        curc = ncells(edp, (int)strlen(edp)) + ncells(ed, edpos);
    } else if (msg[0])
        snprintf(t, sizeof t, "%s", msg);
    else if (L.dir && L.cur < L.n) {
        struct ent *e = &L.e[L.cur];
        ent_stat(&L, L.cur);
        char m[11] = "----------";
        static const char rwx[] = "rwxrwxrwx";
        m[0] = e->type == 2 ? 'd' : e->type == 7 ? 'l' : '-';
        for (int b = 0; b < 9; b++)
            if (e->mode & (0400 >> b))
                m[b + 1] = rwx[b];
        int nm = 0;
        for (int i = 0; i < L.n; i++)
            nm += !!(L.e[i].fl & F_MARK);
        snprintf(t, sizeof t, "%s %u %u bytes  %d/%d%s", m, e->uid, e->size, L.cur + 1, L.n,
                 nm ? "" : L.cut ? " (only the first shown)" : "");
        if (nm) {
            int k = (int)strlen(t);
            snprintf(t + k, sizeof t - (u32)k, ", %d marked", nm);
        }
    } else
        snprintf(t, sizeof t, "%s", L.dir ? "empty" : "");
    char line[ZPATH_MAX + 72];
    snprintf(line, sizeof line, " %s", t);
    fit(rb, &bl, line, cols, curc >= 0 && curc + 2 > cols);
    text_at(0, y, PEN_FG, PEN_DIM, 0, rb, bl);
    if (curc >= 0 && curc + 1 < cols)
        dpy_xor(win, (curc + 1) * 8, y * 8, 8, 8);
    if (verbose)
        dprintf(1, "files: status: %.*s\n", bl, rb);
}
static void keep_visible(void)
{
    int n = lrows();
    if (L.cur >= L.n) L.cur = L.n - 1;
    if (L.cur < 0) L.cur = 0;
    if (L.top > L.cur) L.top = L.cur;
    if (n > 0 && L.cur >= L.top + n) L.top = L.cur - n + 1;
    if (L.top > L.n - n) L.top = L.n - n;
    if (L.top < 0) L.top = 0;
}
static void draw_help(void);
static void draw_all(void)
{
    if (win < 0 || rows < 3)
        return;
    if (mode == M_HELP) {
        draw_help();
        return;
    }
    keep_visible();
    draw_head();
    draw_parent();
    if (pw())
        dpy_fill(win, pw() * 8, 8, 8, lrows() * 8, PEN_BG);
    for (int r = 0; r < lrows(); r++)
        draw_ent(L.top + r);
    draw_status();
    dpy_flush();
}
/* rows r0..r1 of the window again (an expose): the path, the list's rows, the parent column when it is named, the
 * status */
static void draw_rows(int r0, int r1, int parent)
{
    if (win < 0 || rows < 3)
        return;
    if (mode == M_HELP) {
        draw_help();
        return;
    }
    keep_visible();
    if (r0 <= 0)
        draw_head();
    if (parent && pw()) {
        draw_parent();
        dpy_fill(win, pw() * 8, 8, 8, lrows() * 8, PEN_BG);
    }
    for (int r = r0 < 1 ? 1 : r0; r <= r1 && r <= lrows(); r++)
        draw_ent(L.top + r - 1);
    if (r1 >= rows - 1)
        draw_status();
    dpy_flush();
}
/* the cursor moved from old: the rows that changed, or a scroll */
static int mtop = -1;                  /* the top before an action moved it (act sets it), else -1 */
static void moved(int old)
{
    int otop = mtop >= 0 ? mtop : L.top;
    mtop = -1;
    keep_visible();
    msg[0] = 0;
    int d = L.top - otop, n = lrows();
    if (d && (d >= n || -d >= n)) {
        draw_all();
        return;
    }
    if (d > 0) {                        /* down: the rows move up */
        dpy_copy(win, lx() * 8, (1 + d) * 8, lw() * 8, (n - d) * 8, lx() * 8, 8);
        for (int r = n - d; r < n; r++) draw_ent(L.top + r);
    } else if (d < 0) {
        dpy_copy(win, lx() * 8, 8, lw() * 8, (n + d) * 8, lx() * 8, (1 - d) * 8);
        for (int r = 0; r < -d; r++) draw_ent(L.top + r);
    }
    if (old != L.cur)
        draw_ent(old);
    draw_ent(L.cur);
    draw_status();
    dpy_flush();
}
static void set_title(void)
{
    char t[DPY_TITLE];
    snprintf(t, sizeof t, "Files: %s", L.dir ? (strcmp(L.dir, "/") ? base_of(L.dir) : "/") : "");
    if (win >= 0)
        dpy_title(win, t);
}

/* ================================================================ going places */
/* the message the next folder is drawn with (go takes it: the status row drawn once, with it) */
static char next_msg[sizeof msg];
static void say_next(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(next_msg, sizeof next_msg, fmt, ap);
    va_end(ap);
    if (verbose)
        dprintf(1, "files: say: %s\n", next_msg);
}
/* dir becomes the folder (as the kernel resolves it: a folder reached through a link is the folder itself, so the
 * paste's checks compare one name for it), the cursor on sel, else on row at (-1: the first) -> 0, or -errno (said;
 * the folder as it was) */
static int go_at(const char *dir0, const char *sel, int at)
{
    struct list nl;
    u32 t0 = sys_ticks();
    char dir[ZPATH_MAX];
    if (zrealpath(dir0, dir, sizeof dir) < 0)
        snprintf(dir, sizeof dir, "%s", dir0);
    if (P.dir && !strcmp(P.dir, dir)) {
        nl = P;                         /* up: the parent's list is the folder's */
        memset(&P, 0, sizeof P);
    } else {
        snprintf(msg, sizeof msg, "reading %s...", dir);       /* at once: a big folder or a slow disk takes a while */
        draw_status();
        dpy_flush();
        int r = list_read(&nl, dir);
        if (r < 0) {
            say("%s: %s", dir, zstrerror(r) ? zstrerror(r) : "cannot read");
            next_msg[0] = 0;
            draw_status();
            dpy_flush();
            return r;
        }
    }
    u32 t1 = sys_ticks();
    struct list old = L;
    L = nl;
    L.cur = at > 0 ? at : 0;
    L.top = L.cur - lrows() / 2;
    if (sel) {
        int k = list_find(&L, sel);
        if (k >= 0) {
            L.cur = k;
            L.top = k - lrows() / 2;
        }
    }
    list_free(&P);
    if (pw() && strcmp(dir, "/")) {
        char *pd = parent_of(dir);
        if (pd && old.dir && !strcmp(old.dir, pd)) {    /* down: the folder's list is the parent's */
            P = old;
            memset(&old, 0, sizeof old);
        } else if (pd && list_read(&P, pd) < 0)
            memset(&P, 0, sizeof P);
        if (P.dir) {
            P.cur = list_find(&P, base_of(dir));
            if (P.cur < 0) P.cur = 0;
        }
        free(pd);
    }
    list_free(&old);
    strcpy(msg, next_msg);
    next_msg[0] = 0;
    if (L.cut && !msg[0])
        snprintf(msg, sizeof msg, "only the first %d entries shown (at most %d, %d KB of names)", L.n, MAXENT, MAXPOOL / 1024);
    set_title();
    draw_all();
    if (verbose)
        dprintf(1, "files: folder %s: %d entries in %u ticks (read and sorted %u)\n", dir, L.n, sys_ticks() - t0, t1 - t0);
    return 0;
}
static int go(const char *dir, const char *sel) { return go_at(dir, sel, -1); }
static void reload(void)
{
    if (!L.dir)
        return;
    char *d = strdup(L.dir), sel[64] = "";
    if (!d)
        return;
    if (L.cur < L.n)
        strncpy(sel, NM(&L, L.cur), sizeof sel - 1);
    int cur = L.cur;
    list_free(&P);
    struct list keep = L;               /* read afresh (go reuses neither) */
    memset(&L, 0, sizeof L);
    if (go_at(d, sel[0] ? sel : 0, cur) < 0)  /* the name gone: the row it was on */
        L = keep;
    else
        list_free(&keep);
    free(d);
}

/* ================================================================ running programs */
static void reap(void)
{
    while (sys_waitnb(-1) >= 0)
        ;
}
/* av in a terminal window (term -e), in the folder shown; hold: the window stays after it ends (term -H: a short
 * text that less just prints, a program's last words) */
static void in_term(char **av, int hold)
{
    char *v[16];
    int n = 0;
    v[n++] = "/bin/term";
    if (hold)
        v[n++] = "-H";
    if (av) {
        v[n++] = "-e";
        for (int i = 0; av[i] && n < 15; i++)
            v[n++] = av[i];
    }
    v[n] = 0;
    char cwd[ZPATH_MAX];
    int back = sys_getcwd(cwd, sizeof cwd) >= 0 && L.dir && sys_chdir(L.dir) == 0;
    int pid = sys_spawn("/bin/term", v, 0);
    if (back)
        sys_chdir(cwd);
    if (verbose) {
        dprintf(1, "files: start");
        for (int i = 0; v[i]; i++)
            dprintf(1, " %s", v[i]);
        dprintf(1, "%s\n", pid < 0 ? " (failed)" : "");
    }
    if (pid < 0)
        say("term: %s", zwhy("cannot start"));
}
static int handle(struct dpyev *e);
static int quit;
/* a command (cp, mv, rm) run to its end: the window still answers, Esc stops it; its first line of standard error
 * said when it fails -> its exit status */
static int run_cmd(char **av, const char *doing)
{
    int fd[2];
    if (sys_pipe(fd) < 0) {
        say("%s: %s", av[0], zwhy("no pipe"));
        return 1;
    }
    int io[3] = { 0, fd[1], fd[1] };
    int pid = sys_spawn(av[0], av, io);
    sys_close(fd[1]);
    if (pid < 0) {
        sys_close(fd[0]);
        say("%s: %s", av[0], zwhy("cannot start"));
        return 1;
    }
    say("%s...", doing);
    draw_status();
    dpy_flush();
    char err[120];
    int ne = 0, eol = 0, stopped = 0, closed = 0;
    for (;;) {
        struct pollfd p[2] = { { fd[0], POLLIN, 0 }, { dpy_port(), POLLPORT | POLLRDBAND, 0 } };
        sys_evwait(p, 2, 50);
        if (p[0].revents & (POLLIN | POLLHUP)) {
            char b[128];
            int n = sys_read(fd[0], b, sizeof b);
            if (n <= 0)
                break;                  /* every writer gone: the command ended */
            for (int i = 0; i < n && !eol && ne < (int)sizeof err - 1; i++)
                if (b[i] == '\n')
                    eol = 1;                    /* the first line only */
                else
                    err[ne++] = b[i];
        }
        struct dpyev e;
        while (dpy_next(&e, 0) > 0) {
            if (e.type == EV_KEY && e.code == 0x45 && stopped < 2) {
                sys_killsig(pid, stopped ? SIGKILL : SIGTERM);    /* Esc: asked to stop; again: stopped */
                stopped++;
                say(stopped == 1 ? "stopping (Esc again: at once)..." : "stopped at once");
                draw_status();
                dpy_flush();
            } else if (e.type == EV_CLOSE)
                closed = 1;             /* quit when the command has ended */
            else if (e.type == EV_EXPOSE || e.type == EV_RESIZE || e.type == EV_FOCUS)
                handle(&e);             /* drawn again; other keys and clicks are not taken meanwhile */
        }
    }
    sys_close(fd[0]);
    int st = sys_wait(pid);
    err[ne] = 0;
    if (stopped)
        say("stopped");
    else if (st) {
        say("%s", err[0] ? err : "failed");
    } else
        msg[0] = 0;
    if (verbose)
        dprintf(1, "files: %s: status %d\n", av[0], st);
    if (closed)
        quit = 1;
    return st;
}

/* ================================================================ the set, yank, paste, delete */
static char **yk;                       /* the yank: absolute paths */
static int nyk, ykcut;
static void yank_free(void)
{
    for (int i = 0; i < nyk; i++)
        free(yk[i]);
    free(yk);
    yk = 0;
    nyk = 0;
}
/* the marked entries, else the one under the cursor -> malloc'd absolute paths, their count (0: none) */
static int the_set(char ***out)
{
    int n = 0;
    for (int i = 0; i < L.n; i++)
        n += !!(L.e[i].fl & F_MARK);
    int any = n;
    if (!n)
        n = L.cur < L.n ? 1 : 0;
    if (!n)
        return 0;
    char **v = calloc((size_t)n, sizeof *v);
    if (!v)
        return 0;
    int k = 0, lost = 0;
    for (int i = 0; i < L.n && k < n; i++)
        if (any ? L.e[i].fl & F_MARK : i == L.cur) {
            v[k] = xjoin(L.dir, NM(&L, i));
            if (v[k]) k++;
            else lost++;
        }
    *out = v;
    if (lost) {                         /* a path over 255 bytes, or no memory: said, never dropped quietly */
        for (int i = 0; i < k; i++)
            free(v[i]);
        free(v);
        *out = 0;
        say("%d %s too long or out of memory: nothing done", lost, lost == 1 ? "path is" : "paths are");
        return 0;
    }
    return k;
}
static void yank(int cut)
{
    yank_free();
    nyk = the_set(&yk);
    ykcut = cut;
    for (int i = 0; i < L.n; i++)
        L.e[i].fl &= (u8)~F_MARK;
    if (nyk)
        say("%d %s %s: pp pastes", nyk, nyk == 1 ? "entry" : "entries", cut ? "cut" : "yanked");
    else
        say("nothing to yank");
    draw_all();
}
/* the runs of paths for one command: at most 200 paths and 7 KB of words (the kernel's 256 / 8 KB) */
static int run_paths(const char *cmd, const char *opt, char **paths, int n, const char *last, const char *doing)
{
    int rc = 0;
    for (int i = 0; i < n; ) {
        char *av[206];
        int k = 0, bytes = 0;
        av[k++] = (char *)cmd;
        if (opt) av[k++] = (char *)opt;
        while (i < n && k < 202 && bytes + (int)strlen(paths[i]) < 7000) {
            bytes += (int)strlen(paths[i]) + 1;
            av[k++] = paths[i++];
        }
        if (k == (opt ? 2 : 1)) {        /* one path over 7 KB cannot be: ZPATH_MAX; but never loop */
            i++;
            continue;
        }
        if (last) av[k++] = (char *)last;
        av[k] = 0;
        if (run_cmd(av, doing)) {
            rc = 1;
            break;
        }
    }
    return rc;
}
static void paste(int over)
{
    if (!nyk) {
        say("nothing yanked (yy) or cut (dd)");
        draw_status();
        dpy_flush();
        return;
    }
    /* compared as the kernel resolves them (links, ..): a folder reached by a link is the same folder, and cp onto
     * the file itself would empty it */
    char rdir[ZPATH_MAX], rs[ZPATH_MAX];
    if (zrealpath(L.dir, rdir, sizeof rdir) < 0)
        strcpy(rdir, L.dir);
    int exist = 0, edirs = 0, rl = (int)strlen(rdir);
    for (int i = 0; i < nyk; i++) {
        char *pd = parent_of(yk[i]);
        int same = pd && (zrealpath(pd, rs, sizeof rs) == 0 ? !strcmp(rs, rdir) : !strcmp(pd, L.dir));
        free(pd);
        const char *why = same ? "%s is already here" : 0;
        if (!why && zrealpath(yk[i], rs, sizeof rs) == 0) {
            int sl2 = (int)strlen(rs);
            if (!strncmp(rdir, rs, (u32)sl2) && (rdir[sl2] == '/' || !rdir[sl2] || (sl2 == 1 && rl > 1)))
                why = "cannot put %s into itself";
        }
        if (why) {
            say(why, base_of(yk[i]));
            draw_status();
            dpy_flush();
            return;
        }
        struct ustat st;
        const char *t = pjoin(L.dir, base_of(yk[i]));
        if (t && sys_lstat(t, &st) == 0) {
            exist++;
            if ((st.mode & S_IFMT) == S_IFDIR)
                edirs++;
        }
    }
    if (exist && !over) {
        say("%d %s here: replace %s%s? (y/n)", exist, exist == 1 ? "name exists" : "names exist", exist == 1 ? "it" : "them",
            edirs ? " (a folder: merged into)" : "");
        mode = M_ASK;
        askwhat = 'p';
        draw_status();
        dpy_flush();
        return;
    }
    int r = ykcut ? run_paths("/bin/mv", 0, yk, nyk, L.dir, "moving")
                  : run_paths("/bin/cp", "-r", yk, nyk, L.dir, "copying");
    if (r)
        say_next("%s", msg);
    else
        say_next("%d %s %s", nyk, nyk == 1 ? "entry" : "entries", ykcut ? "moved" : "copied");
    if (ykcut && !r)
        yank_free();
    reload();
}
static char **dset;
static int ndset;
static void delete_ask(void)
{
    ndset = the_set(&dset);
    if (!ndset)
        return;
    if (ndset == 1) {
        int k = list_find(&L, base_of(dset[0]));
        say("Delete %s%s? (y/n)", base_of(dset[0]), k >= 0 && L.e[k].type == 2 ? " and all in it" : "");
    }
    else
        say("Delete %d entries? (y/n)", ndset);
    mode = M_ASK;
    askwhat = 'd';
    draw_status();
    dpy_flush();
}
static void delete_do(void)
{
    int r = run_paths("/bin/rm", "-r", dset, ndset, 0, "deleting");
    if (r)
        say_next("%s", msg);
    else
        say_next("%d %s deleted", ndset, ndset == 1 ? "entry" : "entries");
    for (int i = 0; i < ndset; i++)
        free(dset[i]);
    free(dset);
    dset = 0;
    ndset = 0;
    reload();
}

/* ================================================================ opening */
static char opath[ZPATH_MAX];           /* the file a question is about */
static int kind_of(const char *path, const struct ustat *st)
{
    u8 b[512];
    int fd = sys_open(path, O_RDONLY), n = fd < 0 ? -1 : sys_read(fd, b, sizeof b);
    if (fd >= 0)
        sys_close(fd);
    if (n < 0)
        return -1;
    int x = st->mode & 0111;
    if (x && ((n >= 8 && (!memcmp(b + 4, "ZEXE", 4) || !memcmp(b + 4, "ZEX2", 4))) ||
              (n >= 4 && b[0] == 0 && b[1] == 0 && b[2] == 3 && b[3] == 0xF3) || (n >= 2 && b[0] == '#' && b[1] == '!')))
        return 2;                       /* a program */
    struct ustat sv;
    if (((n >= 12 && !memcmp(b, "FORM", 4) && !memcmp(b + 8, "ILBM", 4)) ||
         (n >= 6 && !memcmp(b, "GIF8", 4) && (b[4] == '7' || b[4] == '9') && b[5] == 'a')) &&
        sys_stat("/bin/show", &sv) == 0)
        return 4;                       /* a picture show reads (D50): IFF ILBM, GIF */
    int ctl = 0;
    for (int i = 0; i < n; i++) {
        if (!b[i])
            return 3;                   /* data */
        if (b[i] < 32 && b[i] != '\n' && b[i] != '\r' && b[i] != '\t' && b[i] != '\f' && b[i] != 27 && b[i] != 8)
            ctl++;
    }
    return ctl * 32 > n ? 3 : 1;        /* text */
}
static void ask_line(const char *prompt, const char *init, int pos, int what);
static void open_ent(int viewonly)
{
    if (L.cur >= L.n)
        return;
    const char *p = pjoin(L.dir, NM(&L, L.cur));
    if (!p) {
        say("the path is too long");
        draw_status();
        dpy_flush();
        return;
    }
    struct ustat st;
    if (sys_stat(p, &st) < 0) {
        say("%s: %s", NM(&L, L.cur), zwhy("cannot open"));
        draw_status();
        dpy_flush();
        return;
    }
    strcpy(opath, p);
    if ((st.mode & S_IFMT) == S_IFDIR) {
        if (!viewonly) {
            char *d = strdup(opath);
            if (d) go(d, 0);
            free(d);
        }
        return;
    }
    int k = (st.mode & S_IFMT) != S_IFREG ? 3 : viewonly ? 1 : kind_of(opath, &st);   /* a device or a FIFO is never
                                                                                        * read: it could wait for ever */
    if (k < 0) {
        say("%s: %s", base_of(opath), zwhy("cannot read"));
    } else if (k == 1) {
        char *av[] = { "/bin/less", opath, 0 };
        in_term(av, 1);
    } else if (k == 4) {
        char *av[] = { "/bin/show", opath, 0 };    /* a desktop program: no terminal; show opens a screen of its own by default (D55) */
        char cwd[ZPATH_MAX];
        int back = sys_getcwd(cwd, sizeof cwd) >= 0 && L.dir && sys_chdir(L.dir) == 0;
        int pid = sys_spawn(av[0], av, 0);
        if (back)
            sys_chdir(cwd);
        if (verbose)
            dprintf(1, "files: start /bin/show %s%s\n", opath, pid < 0 ? " (failed)" : "");
        if (pid < 0)
            say("show: %s", zwhy("cannot start"));
    } else if (k == 2) {
        say("Run %s? (y/n)", base_of(opath));
        mode = M_ASK;
        askwhat = 'r';
    } else {
        char pr[48];
        snprintf(pr, sizeof pr, "Open %.20s with: ", base_of(opath));
        ask_line(pr, "", 0, 'o');
        return;
    }
    draw_status();
    dpy_flush();
}
static void edit_ent(void)
{
    if (L.cur >= L.n || L.e[L.cur].type == 2)
        return;
    const char *p = pjoin(L.dir, NM(&L, L.cur));
    if (!p)
        return;
    strcpy(opath, p);
    const char *e = getenv("EDITOR");
    struct ustat st;
    char line[ZPATH_MAX], *av[14];
    int n = 0;
    snprintf(line, sizeof line, "%s", e && *e ? e : sys_stat("/bin/sumi", &st) == 0 ? "/bin/sumi" : "/bin/edit");
    for (char *s = line; *s && n < 12; ) {     /* $EDITOR may carry words ("sumi -x") */
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        av[n++] = s;
        while (*s && *s != ' ') s++;
    }
    if (!n)
        return;
    av[n++] = opath;
    av[n] = 0;
    in_term(av, 0);
    draw_status();
    dpy_flush();
}
static void open_with(const char *cmd)
{
    char line[ZPATH_MAX], *av[14];
    int n = 0;
    strncpy(line, cmd, sizeof line - 1);
    line[sizeof line - 1] = 0;
    for (char *s = line; *s; ) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        if (n == 12) {
            say("at most 12 words");
            return;
        }
        av[n++] = s;
        while (*s && *s != ' ') s++;
    }
    if (!n)
        return;
    av[n++] = opath;
    av[n] = 0;
    in_term(av, 1);
}

/* ================================================================ the typed line and the questions */
static int edwhat;                      /* 'r' rename, 'm' a new folder, '/' search, 'o' open with */
static char last_search[64];
static void ask_line(const char *prompt, const char *init, int pos, int what)
{
    snprintf(edp, sizeof edp, "%s", prompt);
    strncpy(ed, init, sizeof ed - 1);
    ed[sizeof ed - 1] = 0;
    edlen = (int)strlen(ed);
    edpos = pos < 0 || pos > edlen ? edlen : pos;
    edwhat = what;
    mode = M_LINE;
    msg[0] = 0;
    draw_status();
    dpy_flush();
}
static void search(int from, int dir)
{
    if (!last_search[0] || !L.n)
        return;
    for (int k = 1; k <= L.n; k++) {
        int i = ((from + dir * k) % L.n + L.n) % L.n;
        const char *n = NM(&L, i);
        for (const char *s = n; *s; s++) {
            int j = 0;
            while (last_search[j] && tolower((u8)s[j]) == tolower((u8)last_search[j])) j++;
            if (!last_search[j]) {
                int old = L.cur;
                L.cur = i;
                moved(old);
                return;
            }
        }
    }
    say("not found: %s", last_search);
    draw_status();
    dpy_flush();
}
static int name_ok(const char *n)
{
    if (!*n || strchr(n, '/') || !strcmp(n, ".") || !strcmp(n, "..")) {
        say("not a name: \"%s\"", n);
        return 0;
    }
    if (strlen(n) > 63) {
        say("a name is at most 63 bytes");
        return 0;
    }
    return 1;
}
static void line_done(void)
{
    ed[edlen] = 0;
    mode = M_LIST;
    msg[0] = 0;
    if (edwhat == '/') {
        snprintf(last_search, sizeof last_search, "%s", ed);
        search(L.cur - 1, 1);
        draw_status();
        dpy_flush();
        return;
    }
    if (edwhat == 'o') {
        if (ed[0]) open_with(ed);
        draw_status();
        dpy_flush();
        return;
    }
    if (!name_ok(ed)) {
        draw_status();
        dpy_flush();
        return;
    }
    char *to = xjoin(L.dir, ed);
    struct ustat st;
    int r;
    if (!to)
        r = -ENAMETOOLONG;
    else if (sys_lstat(to, &st) == 0)
        r = -EEXIST;
    else if (edwhat == 'm')
        r = sys_mkdir(to);
    else {
        const char *from = pjoin(L.dir, NM(&L, L.cur));
        r = from ? sys_rename(from, to) : -ENAMETOOLONG;
    }
    free(to);
    if (r < 0) {
        say("%s: %s", ed, zstrerror(r) ? zstrerror(r) : "failed");
        draw_status();
        dpy_flush();
        return;
    }
    char nm[64];
    snprintf(nm, sizeof nm, "%s", ed);
    list_free(&P);
    struct list keep = L;
    memset(&L, 0, sizeof L);
    say_next("%s %s", edwhat == 'm' ? "made" : "renamed to", nm);
    if (go(keep.dir, nm) < 0)
        L = keep;
    else
        list_free(&keep);
}
static void line_key(const struct dpyev *e)
{
    int c = e->code;
    if (c == 0x44 || c == 0x43) { line_done(); return; }
    if (c == 0x45) { mode = M_LIST; msg[0] = 0; }
    else if (c == 0x4F) {               /* Left (Shift: the start) */
        if (e->qual & (GQ_LSHIFT | GQ_RSHIFT)) edpos = 0;
        else while (edpos > 0 && (((u8)ed[--edpos]) & 0xC0) == 0x80);
    } else if (c == 0x4E) {
        if (e->qual & (GQ_LSHIFT | GQ_RSHIFT)) edpos = edlen;
        else if (edpos < edlen) for (edpos++; edpos < edlen && (((u8)ed[edpos]) & 0xC0) == 0x80; edpos++);
    } else if (c == 0x41) {             /* Backspace */
        int s = edpos;
        while (s > 0 && (((u8)ed[--s]) & 0xC0) == 0x80);
        memmove(ed + s, ed + edpos, (u32)(edlen - edpos));
        edlen -= edpos - s;
        edpos = s;
    } else if (c == 0x46) {             /* Del */
        int t = edpos;
        if (t < edlen) for (t++; t < edlen && (((u8)ed[t]) & 0xC0) == 0x80; t++);
        memmove(ed + edpos, ed + t, (u32)(edlen - t));
        edlen -= t - edpos;
    } else if ((u8)e->ch[0] >= 32 && e->ch[0] != 127) {
        int n = 1;
        while (n < 4 && e->ch[n] && ((u8)e->ch[n] & 0xC0) == 0x80) n++;
        if (edlen + n < (int)sizeof ed - 1) {
            memmove(ed + edpos + n, ed + edpos, (u32)(edlen - edpos));
            memcpy(ed + edpos, e->ch, (u32)n);
            edlen += n;
            edpos += n;
        }
    }
    draw_status();
    dpy_flush();
}
static void answer(int yes)
{
    int what = askwhat;
    mode = M_LIST;
    msg[0] = 0;
    if (yes && what == 'p')
        paste(1);
    else if (yes && what == 'd')
        delete_do();
    else if (yes && what == 'r') {
        char *av[] = { opath, 0 };
        in_term(av, 1);
    } else if (what == 'd') {
        for (int i = 0; i < ndset; i++)
            free(dset[i]);
        free(dset);
        dset = 0;
        ndset = 0;
    }
    draw_status();
    dpy_flush();
}

/* ================================================================ keys (Ahiru.keys's format) */
enum { A_NONE, A_DOWN, A_UP, A_HALFDOWN, A_HALFUP, A_PAGEDOWN, A_PAGEUP, A_TOP, A_BOTTOM, A_PARENT, A_ENTER, A_VIEW,
       A_EDIT, A_MARK, A_INVERT, A_UNMARK, A_MARKALL, A_YANK, A_CUT, A_PASTE, A_PASTEOVER, A_UNYANK, A_DELETE,
       A_RENAME, A_RENAMEEXT, A_RENAMEEND, A_RENAMESTART, A_MKDIR, A_SEARCH, A_NEXT, A_PREV, A_SORT, A_HIDDEN, A_GO,
       A_SHELL, A_RELOAD, A_HELP, A_QUIT, NACT };
static const char *const actname[NACT] = {
    "none", "down", "up", "halfdown", "halfup", "pagedown", "pageup", "top", "bottom", "parent", "enter", "view",
    "edit", "mark", "invertmarks", "unmarkall", "markall", "yank", "cut", "paste", "pasteover", "unyank", "delete",
    "rename", "renameext", "renameend", "renamestart", "mkdir", "search", "next", "prev", "sort", "togglehidden", "go",
    "newshell", "reload", "help", "quit" };
static const char *const actdesc[NACT] = {
    "", "move down", "move up", "half a page down", "half a page up", "a page down", "a page up", "to the top",
    "to the bottom", "the parent folder", "enter, or open the file by type", "view the file (less)", "edit it",
    "mark and move on", "invert the marks", "unmark all", "mark all", "yank (copy)", "cut", "paste here",
    "paste, replacing", "forget the yank", "delete (asks)", "rename (empty line)", "rename, at the extension",
    "rename, at the end", "rename, at the start", "make a folder", "search names", "next match", "previous match",
    "sort by", "show/hide hidden names", "go to", "a shell here (a terminal)", "read again", "these keys", "quit" };
static const char defaults[] =
    "; files - the file view of Zenix Desktop: the keys (man files). Edit freely; files reads this at start.\n"
    ";\n"
    "; One key a line: KEYS ACTION [ARGUMENT]. KEYS are typed characters, or\n"
    "; names in <>: <Up> <Down> <Left> <Right>, <S-Up> (Shift) ..., <C-d> (Ctrl),\n"
    "; <Return> <Esc> <Tab> <Space> <Del> <BS> <Help> <F1> .. <F10>.\n"
    "; Two keys in a row make a prefix: the first shows what can follow.\n"
    "; ACTION none takes a key away. The format is Ahiru.keys's.\n"
    ";\n"
    "; sort takes name, size, time or ext, and \"reverse\" after it, or flip;\n"
    "; go takes a path, or root or home.\n"
    "\n[main]\n"
    "j down\n<Down> down\nk up\n<Up> up\n"
    "J halfdown\n<C-d> halfdown\nK halfup\n<C-u> halfup\n"
    "<S-Down> pagedown\n<C-f> pagedown\n<S-Up> pageup\n<C-b> pageup\n"
    "gg top\n<S-Left> top\nG bottom\n<S-Right> bottom\n"
    "h parent\n<Left> parent\n<BS> parent\n"
    "l enter\n<Right> enter\n<Return> enter\n"
    "i view\nE edit\n"
    "<Space> mark\nv invertmarks\nuv unmarkall\n"
    "yy yank\ndd cut\npp paste\npo pasteover\nuy unyank\n"
    "dD delete\n<Del> delete\n"
    "cw rename\na renameext\nA renameend\nI renamestart\nM mkdir\n"
    "/ search\nn next\nN prev\n"
    "on sort name\nos sort size\not sort time\noe sort ext\n"
    "oN sort name reverse\noS sort size reverse\noT sort time reverse\noE sort ext reverse\nor sort flip\n"
    "zh togglehidden\n"
    "g/ go root\ngh go home\ngt go /tmp\n"
    "S newshell\nR reload\n<F5> reload\n"
    "? help\n<Help> help\nq quit\n";

#define MAXB 160
static struct kbind kb[MAXB];
static struct keyset K = { actname, NACT, kb, 0, MAXB, "" };
#define binds K.b
#define nbinds K.n
#define keybad K.bad
#define key_name keys_name
#define key_code keys_code
#define bind_find(a, b) keys_find(&K, a, b)
#define is_prefix(k) keys_prefix(&K, k)
static void keys_init(void)
{
    if (keys_load(&K, defaults, sizeof defaults - 1, "files") && verbose)
        dprintf(1, "files: wrote ~%s\n", KEYFILE);
}
static void hints(int k1)
{
    int bl = 0;
    char kn[12];
    key_name(k1, kn);
    bl = snprintf(msg, sizeof msg, "%s:", kn);
    for (int i = 0; i < nbinds && bl < (int)sizeof msg - 40; i++)
        if (binds[i].act && binds[i].k1 == k1 && binds[i].k2) {
            key_name(binds[i].k2, kn);
            bl += snprintf(msg + bl, sizeof msg - (u32)bl, "  %s %s%s%s", kn, binds[i].arg[0] ? actname[binds[i].act] : actdesc[binds[i].act],
                           binds[i].arg[0] ? " " : "", binds[i].arg);
        }
    if (verbose)
        dprintf(1, "files: say: %s\n", msg);
}

/* ================================================================ help */
static int help_top;
static void draw_help(void)
{
    int n = rows - 1;
    for (int r = 0; r < n; r++) {
        int i = help_top + r - 1, bl = 0;
        char t[128];
        if (r == 0)
            snprintf(t, sizeof t, " The keys (%s%s): j k move, q or Esc closes", getenv("HOME") ? "~" : "", KEYFILE);
        else if (i < nbinds) {
            char a[12], b[12] = "";
            key_name(binds[i].k1, a);
            if (binds[i].k2) key_name(binds[i].k2, b);
            snprintf(t, sizeof t, " %s%-*s %s%s%s", a, (int)(10 - strlen(a)), b, actdesc[binds[i].act],
                     binds[i].arg[0] ? ": " : "", binds[i].arg);
        } else
            t[0] = 0;
        fit(rb, &bl, t, cols, 0);
        text_at(0, r, PEN_FG, r ? PEN_BG : PEN_DIM, r ? 0 : TA_BOLD, rb, bl);
    }
    msg[0] = 0;
    draw_status();
    dpy_flush();
}
static void help_key(int k)
{
    int n = rows - 2;
    if (k == 'j' || k == KM_DOWN) help_top++;
    else if (k == 'k' || k == KM_UP) help_top--;
    else if (k == ' ' || k == KM_SDOWN) help_top += n;
    else if (k == KM_SUP) help_top -= n;
    else { mode = M_LIST; draw_all(); return; }
    if (help_top > nbinds - n) help_top = nbinds - n;
    if (help_top < 0) help_top = 0;
    draw_help();
}

/* ================================================================ actions */
static void mark_all(int how)          /* 0 clear, 1 set, 2 invert */
{
    for (int i = 0; i < L.n; i++)
        L.e[i].fl = (u8)(how == 0 ? L.e[i].fl & ~F_MARK : how == 1 ? L.e[i].fl | F_MARK : L.e[i].fl ^ F_MARK);
    msg[0] = 0;
    draw_all();
}
static void go_parent(void)
{
    if (!L.dir || !strcmp(L.dir, "/"))
        return;
    char *pd = parent_of(L.dir), *me = strdup(base_of(L.dir));
    if (pd && me)
        go(pd, me);
    free(pd);
    free(me);
}
static void act(int a, const char *arg)
{
    int old = L.cur, half = lrows() / 2 > 0 ? lrows() / 2 : 1, page = lrows() > 1 ? lrows() - 1 : 1;
    mtop = L.top;
    switch (a) {
    case A_DOWN: L.cur++; moved(old); break;
    case A_UP: L.cur--; moved(old); break;
    case A_HALFDOWN: L.cur += half; L.top += half; moved(old); break;
    case A_HALFUP: L.cur -= half; L.top -= half; if (L.top < 0) L.top = 0; moved(old); break;
    case A_PAGEDOWN: L.cur += page; moved(old); break;
    case A_PAGEUP: L.cur -= page; moved(old); break;
    case A_TOP: L.cur = 0; moved(old); break;
    case A_BOTTOM: L.cur = L.n - 1; moved(old); break;
    case A_PARENT: go_parent(); break;
    case A_ENTER: open_ent(0); break;
    case A_VIEW: open_ent(1); break;
    case A_EDIT: edit_ent(); break;
    case A_MARK:
        if (L.cur < L.n) {
            L.e[L.cur].fl ^= F_MARK;
            L.cur++;
            moved(old);
        }
        break;
    case A_INVERT: mark_all(2); break;
    case A_UNMARK: mark_all(0); break;
    case A_MARKALL: mark_all(1); break;
    case A_YANK: yank(0); break;
    case A_CUT: yank(1); break;
    case A_PASTE: paste(0); break;
    case A_PASTEOVER: paste(1); break;
    case A_UNYANK: yank_free(); say("the yank is forgotten"); draw_status(); dpy_flush(); break;
    case A_DELETE: delete_ask(); break;
    case A_RENAME: case A_RENAMEEXT: case A_RENAMEEND: case A_RENAMESTART:
        if (L.cur < L.n) {
            const char *n = NM(&L, L.cur), *x = strrchr(n, '.');
            int pos = a == A_RENAMESTART ? 0 : a == A_RENAMEEXT && x && x != n ? (int)(x - n) : -1;
            ask_line("Rename to: ", a == A_RENAME ? "" : n, pos, 'r');
        }
        break;
    case A_MKDIR: ask_line("New folder: ", "", -1, 'm'); break;
    case A_SEARCH: ask_line("/", "", -1, '/'); break;
    case A_NEXT: search(L.cur, 1); break;
    case A_PREV: search(L.cur, -1); break;
    case A_SORT: {
        char nm[64] = "";
        if (L.cur < L.n) snprintf(nm, sizeof nm, "%s", NM(&L, L.cur));
        if (!strncmp(arg, "flip", 4)) sortrev ^= 1;
        else {
            sortby = !strncmp(arg, "size", 4) ? 1 : !strncmp(arg, "time", 4) ? 2 : !strncmp(arg, "ext", 3) ? 3 : 0;
            sortrev = strstr(arg, "reverse") != 0;
        }
        list_sort(&L);
        int k = list_find(&L, nm);
        L.cur = k >= 0 ? k : 0;
        say("sorted by %s%s", sortby == 1 ? "size" : sortby == 2 ? "time" : sortby == 3 ? "extension" : "name",
            sortrev ? ", reversed" : "");
        draw_all();
        break;
    }
    case A_HIDDEN:
        hidden ^= 1;
        say_next("hidden names %s", hidden ? "shown" : "hidden");
        reload();
        break;
    case A_GO: {
        const char *h = getenv("HOME");
        const char *to = !strcmp(arg, "root") ? "/" : !strcmp(arg, "home") ? (h && *h ? h : "/") : arg;
        if (*to == '/') {
            char d[ZPATH_MAX];
            if (zrealpath(to, d, sizeof d) == 0) go(d, 0);
            else { say("%s: %s", to, zwhy("not found")); draw_status(); dpy_flush(); }
        }
        break;
    }
    case A_SHELL: in_term(0, 0); draw_status(); dpy_flush(); break;
    case A_RELOAD: reload(); break;
    case A_HELP: mode = M_HELP; help_top = 0; draw_help(); break;
    case A_QUIT: quit = 1; break;
    }
}
static int pending;                     /* a prefix key waiting for its second */
static void key(const struct dpyev *e)
{
    if (e->code & 0x80)
        return;
    if (mode == M_LINE) { line_key(e); return; }
    int k = key_code(e);
    if (!k)
        return;
    if (mode == M_ASK) { answer(k == 'y' || k == 'Y'); return; }
    if (mode == M_HELP) { help_key(k); return; }
    const struct kbind *b;
    if (pending) {
        int k1 = pending;
        pending = 0;
        msg[0] = 0;
        if ((b = bind_find(k1, k))) act(b->act, b->arg);
        else { draw_status(); dpy_flush(); }
        return;
    }
    if (is_prefix(k)) {
        pending = k;
        hints(k);
        draw_status();
        dpy_flush();
        return;
    }
    if ((b = bind_find(k, 0)))
        act(b->act, b->arg);
}

/* ================================================================ the mouse and the menus */
static int last_row = -1;
static void click(const struct dpyev *e)
{
    int r = e->y / 8, c = e->x / 8;
    if (mode != M_LIST || r < 1 || r > lrows())
        return;
    if (pw() && c < pw()) {             /* the parent column: that folder */
        int i = P.top + r - 1;
        if (P.dir && i < P.n && P.e[i].type == 2 && i != P.cur) {
            char *d = xjoin(P.dir, NM(&P, i));
            if (d) go(d, 0);
            free(d);
        }
        return;
    }
    int i = L.top + r - 1;
    if (i >= L.n)
        return;
    int old = L.cur;
    if (e->qual & (GQ_LSHIFT | GQ_RSHIFT)) {
        L.e[i].fl ^= F_MARK;
        L.cur = i;
        moved(old);
        last_row = -1;
        return;
    }
    if (i == last_row && e->arg == 2) {     /* a double click (D61: displayd's, the mouse setting's time): open */
        last_row = -1;
        L.cur = i;
        if (old != i) moved(old);
        open_ent(0);
        return;
    }
    last_row = i;
    L.cur = i;
    moved(old);
}
static const char menus[] = "Files\tOpen\tParent\tReload\tShow hidden\tShell here\tQuit\n"
                            "Edit\tCopy\tCut\tPaste\tDelete\tRename\tNew folder\tMark all\tUnmark all";
static void menu(const struct dpyev *e)
{
    static const u8 m0[] = { A_ENTER, A_PARENT, A_RELOAD, A_HIDDEN, A_SHELL, A_QUIT };
    static const u8 m1[] = { A_YANK, A_CUT, A_PASTE, A_DELETE, A_RENAME, A_MKDIR, A_MARKALL, A_UNMARK };
    if (mode != M_LIST)
        return;
    if (e->code == 0 && e->arg < sizeof m0)
        act(m0[e->arg], "");
    else if (e->code == 1 && e->arg < sizeof m1)
        act(m1[e->arg], "");
}

static int handle(struct dpyev *e)
{
    switch (e->type) {
    case EV_RESIZE: {
        int opw = pw();
        cols = e->w / 8 > MAXCOLS ? MAXCOLS : e->w / 8;
        rows = e->h / 8;
        if (pw() && !opw && L.dir && strcmp(L.dir, "/")) {     /* the parent column appears */
            char *pd = parent_of(L.dir);
            if (pd && list_read(&P, pd) == 0) {
                P.cur = list_find(&P, base_of(L.dir));
                if (P.cur < 0) P.cur = 0;
            }
            free(pd);
        } else if (!pw())
            list_free(&P);
        break;
    }
    case EV_EXPOSE:
        draw_rows(e->y / 8, (e->y + e->h - 1) / 8, e->x < (pw() + 1) * 8);
        if (verbose && dpy_info(win, &where) == 0)
            dprintf(1, "files: window %d %d %d %d\n", where.ix, where.iy, cols, rows);
        break;
    case EV_FOCUS:
        focus = e->arg;
        if (mode == M_LIST && L.cur < L.n) { draw_ent(L.cur); dpy_flush(); }
        break;
    case EV_KEY: key(e); break;
    case EV_BUTTON:
        if ((e->code & 0x7F) == 1 && !(e->code & 0x80)) click(e);
        break;
    case EV_MENU: menu(e); break;
    case EV_CLOSE: quit = 1; break;
    }
    return quit;
}

int main(int argc, char **argv)
{
    const char *start = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (argv[i][0] != '-' && !start) start = argv[i];
        else { dprintf(2, "usage: files [-v] [FOLDER]\n"); return 2; }
    char dir[ZPATH_MAX];
    if (zrealpath(start ? start : ".", dir, sizeof dir) < 0) {
        dprintf(2, "files: %s: %s\n", start ? start : ".", zwhy("cannot resolve"));
        return 1;
    }
    struct ustat st;
    if (sys_stat(dir, &st) < 0 || (st.mode & S_IFMT) != S_IFDIR) {
        dprintf(2, "files: %s: not a folder\n", dir);
        return 1;
    }
    int r = dpy_open("files");
    if (r < 0) {
        dprintf(2, "files: the desktop: %s\n", zstrerror(r) ? zstrerror(r) : "not reachable");
        return 1;
    }
    keys_init();
    int maxr = (dpy_h - 32) / 8;
    rows = maxr < 22 ? maxr : 22;
    if (rows < 8) rows = 8;
    cols = 60;
    struct dpywin d;
    memset(&d, 0, sizeof d);
    d.kind = WK_NORMAL;
    d.x = d.y = -1;
    d.w = (u16)(cols * 8);
    d.h = (u16)(rows * 8);
    d.minw = 24 * 8;
    d.minh = 6 * 8;
    d.maxw = (u16)(dpy_w < MAXCOLS * 8 ? dpy_w : MAXCOLS * 8);
    d.stepw = d.steph = 8;
    strcpy(d.title, "Files");
    win = dpy_newwin(&d);
    if (win < 0) {
        dprintf(2, "files: no window: %s\n", zstrerror(win) ? zstrerror(win) : "refused");
        dpy_close();
        return 1;
    }
    cols = d.w / 8;
    rows = d.h / 8;
    dpy_menu(win, menus, sizeof menus - 1);
    if (go(dir, 0) < 0) {
        dpy_close();
        return 1;
    }
    if (keybad[0]) {
        snprintf(msg, sizeof msg, "%s", keybad);
        if (verbose) dprintf(1, "files: say: %s\n", msg);
        draw_status();
        dpy_flush();
    }
    while (!quit) {
        struct dpyev e;
        int got = dpy_next(&e, 200);
        reap();
        if (got < 0)
            break;
        if (got)
            handle(&e);
        if (dpy_lost())
            draw_all();
    }
    dpy_close();
    return 0;
}
