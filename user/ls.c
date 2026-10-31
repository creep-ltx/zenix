/* SPDX-License-Identifier: GPL-2.0-only */
#include "pw.h"
/* ls [-aAldRFht rSs1] [--color[=WHEN]] [file...]: list files. In a terminal the names come in columns; -l is the long
 * form. Names are coloured as GNU ls colours them with LS_COLORS unset (the same bytes), by default when the output
 * is a terminal (libz color_auto; GNU's own default is never). */
static int o_a, o_A, o_l, o_d, o_R, o_F, o_h, o_t, o_r, o_S, o_1, multi, tty, printed_any, color, color_lead;
struct ent { char name[64]; struct ustat st; char link[64]; int islink; };
static const char *const mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static int leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static void when(u32 t, char *out)
{
    u32 days = t / 86400, rem = t % 86400; int y = 1970;
    for (;;) { u32 yl = leap(y) ? 366 : 365; if (days < yl) break; days -= yl; y++; }
    static const int ml[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int m = 0; for (; m < 11; m++) { int l = ml[m] + (m == 1 && leap(y)); if ((int)days < l) break; days -= (u32)l; }
    u32 now = sys_time(); int old = now > t ? now - t > 15552000u : t - now > 3600;
    if (old) snprintf(out, 20, "%s %2d  %d", mon[m], (int)days + 1, y);
    else snprintf(out, 20, "%s %2d %02d:%02d", mon[m], (int)days + 1, (int)(rem / 3600), (int)(rem / 60 % 60));
}
static void modestr(u32 m, char *s)
{
    u32 t = m & S_IFMT;
    s[0] = t == S_IFDIR ? 'd' : t == S_IFLNK ? 'l' : '-';
    for (int i = 0; i < 9; i++) s[1 + i] = (m & (0400u >> i)) ? "rwxrwxrwx"[i] : '-';
    if (m & 04000) s[3] = s[3] == 'x' ? 's' : 'S';              /* set-user-id */
    if (m & 02000) s[6] = s[6] == 'x' ? 's' : 'S';              /* set-group-id */
    if (m & 01000) s[9] = s[9] == 'x' ? 't' : 'T';              /* sticky */
    s[10] = 0;
}
static void sizestr(u32 sz, char *out)
{
    if (!o_h || sz < 1024) { snprintf(out, 16, "%u", sz); return; }
    u32 v = sz, u = 0; const char *un = "KMG";
    while (v >= 1024 * 10 && u < 2) { v /= 1024; u++; }
    if (v < 1024 * 10) { u32 whole = sz >> (10 * (u + 1)); u32 tenth = ((sz >> (10 * u)) % 1024) * 10 / 1024; if (whole < 10) { snprintf(out, 16, "%u.%u%c", whole, tenth, un[u]); return; } }
    snprintf(out, 16, "%u%c", sz >> (10 * (u + 1)), un[u]);
}
static int cmp(const void *a, const void *b)
{
    const struct ent *x = a, *y = b; int c;
    if (o_S) c = x->st.size < y->st.size ? 1 : x->st.size > y->st.size ? -1 : strcmp(x->name, y->name);
    else if (o_t) c = x->st.mtime < y->st.mtime ? 1 : x->st.mtime > y->st.mtime ? -1 : strcmp(x->name, y->name);
    else c = strcmp(x->name, y->name);
    return o_r ? -c : c;
}
static char suffix(const struct ent *e)
{
    if (!o_F) return 0;
    if (e->islink) return '@';
    u32 t = e->st.mode & S_IFMT;
    return t == S_IFDIR ? '/' : (e->st.mode & 0111) ? '*' : 0;
}
/* GNU's colours for a name (dircolors' defaults): directories (sticky, other-writable), links, set-user/group-id and
 * executable files; 0 for none */
static const char *lscolor(const struct ent *e)
{
    u32 m = e->st.mode, t = m & S_IFMT;
    if (e->islink) return "01;36";
    if (t == S_IFDIR) return (m & 01002) == 01002 ? "30;42" : (m & 2) ? "34;42" : (m & 01000) ? "37;44" : "01;34";
    if (t == S_IFREG) return (m & 04000) ? "37;41" : (m & 02000) ? "30;43" : (m & 0111) ? "01;32" : 0;
    return 0;
}
static void putname(const struct ent *e)
{
    const char *c = color ? lscolor(e) : 0;
    if (!c) { oputs(e->name); return; }
    if (!color_lead) { oputs("\033[0m"); color_lead = 1; }    /* GNU: a reset once, before the first coloured name */
    oputs("\033["); oputs(c); oputc('m'); oputs(e->name); oputs("\033[0m");
}
static void show(struct ent *v, int n)
{
    qsort(v, (u32)n, sizeof(struct ent), cmp);
    if (o_l) {
        u32 wl = 1, ws = 1, wu = 1, wg = 1;
        for (int i = 0; i < n; i++) {
            char b[16], nb[1];
            if (strlen(pw_name(v[i].st.uid, nb)) > wu) wu = strlen(pw_name(v[i].st.uid, nb));
            if (strlen(gr_name(v[i].st.gid, nb)) > wg) wg = strlen(gr_name(v[i].st.gid, nb));
            snprintf(b, 16, "%u", v[i].st.links); if (strlen(b) > wl) wl = strlen(b);
            sizestr(v[i].st.size, b); if (strlen(b) > ws) ws = strlen(b);
        }
        for (int i = 0; i < n; i++) {
            char ms[11], dt[20], sz[16], ln[16];
            modestr(v[i].st.mode, ms); when(v[i].st.mtime, dt); sizestr(v[i].st.size, sz); snprintf(ln, 16, "%u", v[i].st.links);
            oprintf("%s ", ms); for (u32 k = strlen(ln); k < wl; k++) oputc(' '); oprintf("%s ", ln);
            { char nb[1], un[32], gn[32]; strcpy(un, pw_name(v[i].st.uid, nb)); strcpy(gn, gr_name(v[i].st.gid, nb));
              oprintf("%s", un); for (u32 k = strlen(un); k < wu; k++) oputc(' '); oputc(' ');
              oprintf("%s", gn); for (u32 k = strlen(gn); k < wg; k++) oputc(' '); oputc(' '); }
            for (u32 k = strlen(sz); k < ws; k++) oputc(' ');
            oprintf("%s %s ", sz, dt); putname(&v[i]);
            char sf = suffix(&v[i]); if (sf && !v[i].islink) oputc(sf);
            if (v[i].islink) { oputs(" -> "); oputs(v[i].link); }
            oputc('\n');
        }
        return;
    }
    if (!tty || o_1) { for (int i = 0; i < n; i++) { putname(&v[i]); char sf = suffix(&v[i]); if (sf) oputc(sf); oputc('\n'); } return; }
    /* the columns by the names' widths on the console (DESIGN D28: a UTF-8 letter one column, a wide one two) */
    int w = 0; for (int i = 0; i < n; i++) { int l = zu8cols(v[i].name, strlen(v[i].name)) + (suffix(&v[i]) ? 1 : 0); if (l > w) w = l; }
    w += 2; int cols = 80 / w; if (cols < 1) cols = 1;
    int rows = (n + cols - 1) / cols;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int i = c * rows + r; if (i >= n) break;
            putname(&v[i]); char sf = suffix(&v[i]); int l = zu8cols(v[i].name, strlen(v[i].name)) + (sf ? 1 : 0); if (sf) oputc(sf);
            if (c * rows + r + rows < n) for (int k = l; k < w; k++) oputc(' ');
        }
        oputc('\n');
    }
}
static void fill(struct ent *e, const char *path)
{
    if (sys_lstat(path, &e->st) < 0) memset(&e->st, 0, sizeof e->st);
    e->islink = (e->st.mode & S_IFMT) == S_IFLNK;
    e->link[0] = 0;
    if (e->islink) { int n = sys_readlink(path, e->link, 63); if (n < 0) n = 0; e->link[n] = 0;
                     if (n == 63 && (u32)e->st.size > 63) memcpy(e->link + 60, "...", 3); }   /* a longer target: shown cut */
}
static int list(const char *path, int top)
{
    struct ustat st;
    if (sys_lstat(path, &st) < 0) { dprintf(2, "ls: %s: not found\n", path); return 1; }
    int isdir = (st.mode & S_IFMT) == S_IFDIR;
    if (!isdir && (st.mode & S_IFMT) == S_IFLNK && !o_l && !o_d) { struct ustat t; if (sys_stat(path, &t) == 0 && (t.mode & S_IFMT) == S_IFDIR) { isdir = 1; st = t; } }
    if (!isdir || o_d) {
        struct ent *e = xmalloc(sizeof *e); strncpy(e->name, path, 63); e->name[63] = 0; fill(e, path); show(e, 1); free(e); return 0;
    }
    if (multi || o_R) { if (printed_any) oputc('\n'); oprintf("%s:\n", path); }
    printed_any = 1;
    (void)top;
    /* the names come from the heap and grow as needed (libz dir_list): no limit but memory, and running out says so */
    char **names; int rerr, nn = dir_list_some(path, &names, &rerr);
    if (nn < 0) { dprintf(2, "ls: %s: %s\n", path, zwhy("cannot open")); return 1; }
    struct ent *v = malloc(((u32)nn + 2) * sizeof(struct ent));
    if (!v) { dprintf(2, "ls: %s: out of memory (%d names)\n", path, nn); dir_free(names, nn); return 1; }
    int n = 0;
    for (int k = o_a ? -2 : 0; k < nn; k++) {          /* -a: . and .. too (dir_list leaves them out) */
        const char *nm = k == -2 ? "." : k == -1 ? ".." : names[k];
        if (k >= 0 && nm[0] == '.' && !o_a && !o_A) continue;
        struct ent *e = &v[n++]; strncpy(e->name, nm, 63); e->name[63] = 0;
        char *full = path_join(path, nm);
        if (full) fill(e, full); else { memset(&e->st, 0, sizeof e->st); e->islink = 0; e->link[0] = 0; dprintf(2, "ls: %s/%s: %s\n", path, nm, zwhy("cannot read")); }
        free(full);
    }
    dir_free(names, nn);
    show(v, n);
    int rc = 0;
    if (rerr) {                                        /* what could be read is shown; the rest is said to be missing */
        zerrno = rerr;
        dprintf(2, "ls: %s: %s\n", path, zwhy("cannot read all of it"));
        rc = 1;
    }
    if (o_R)
        for (int i = 0; i < n; i++)
            if ((v[i].st.mode & S_IFMT) == S_IFDIR && strcmp(v[i].name, ".") && strcmp(v[i].name, "..")) {
                char *full = path_join(path, v[i].name);
                if (full) rc |= list(full, 0); else { dprintf(2, "ls: %s/%s: %s\n", path, v[i].name, zwhy("cannot read")); rc = 1; }
                free(full);
            }
    free(v);
    return rc;
}
int main(int argc, char **argv)
{
    int i = 1, rc = 0, cmode = 2;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (argv[i][1] == '-') {
            int m = color_arg(argv[i], 1);
            if (m == -2) { dprintf(2, "ls: invalid argument for --color: %s (always, never or auto)\n", argv[i]); return 2; }
            if (m < 0) { dprintf(2, "ls: unknown option %s\n", argv[i]); return 2; }
            cmode = m;
            continue;
        }
        for (const char *p = argv[i] + 1; *p; p++) {
            switch (*p) { case 'a': o_a = 1; break; case 'A': o_A = 1; break; case 'l': o_l = 1; break; case 'd': o_d = 1; break; case 'R': o_R = 1; break;
                case 'F': o_F = 1; break; case 'h': o_h = 1; break; case 't': o_t = 1; break; case 'r': o_r = 1; break; case 'S': o_S = 1; break; case '1': o_1 = 1; break; case 's': break;
                default: dprintf(2, "ls: unknown option -%c\n", *p); return 2; }
        }
    }
    tty = sys_isatty(1);
    color = cmode == 2 ? color_auto(1) : cmode;
    int n = argc - i; multi = n > 1;
    if (!n) { rc = list(".", 1); oflush(); return rc; }
    /* operands: everything that is not a directory is listed together first, sorted; then each directory */
    struct ent *files = xmalloc((u32)n * sizeof(struct ent)); int nf = 0;
    struct ent *dirs = xmalloc((u32)n * sizeof(struct ent)); int nd = 0;
    for (int k = 0; k < n; k++) {
        const char *a = argv[i + k];
        struct ustat st;
        if (sys_lstat(a, &st) < 0) { dprintf(2, "ls: %s: not found\n", a); rc = 1; continue; }
        int isdir = (st.mode & S_IFMT) == S_IFDIR;
        if ((st.mode & S_IFMT) == S_IFLNK && !o_l && !o_d) { struct ustat t; if (sys_stat(a, &t) == 0 && (t.mode & S_IFMT) == S_IFDIR) isdir = 1; }
        struct ent *e = (isdir && !o_d) ? &dirs[nd++] : &files[nf++];
        strncpy(e->name, a, 63); e->name[63] = 0; fill(e, a);
    }
    if (nf) { show(files, nf); printed_any = 1; }
    qsort(dirs, (u32)nd, sizeof(struct ent), cmp);
    for (int k = 0; k < nd; k++) rc |= list(dirs[k].name, k == 0 && !nf);
    oflush();
    return rc;
}
