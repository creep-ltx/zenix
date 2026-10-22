#include "libz.h"
/* ls [-aAldRFht rSs1] [file...]: list files. In a terminal the names come in columns; -l is the long form. */
static int o_a, o_A, o_l, o_d, o_R, o_F, o_h, o_t, o_r, o_S, o_1, multi, tty, printed_any;
struct ent { char name[64]; struct ustat st; char link[64]; int islink; };
#define MAXE 400
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
static void show(struct ent *v, int n)
{
    qsort(v, (u32)n, sizeof(struct ent), cmp);
    if (o_l) {
        u32 wl = 1, ws = 1;
        for (int i = 0; i < n; i++) {
            char b[16];
            snprintf(b, 16, "%u", v[i].st.links); if (strlen(b) > wl) wl = strlen(b);
            sizestr(v[i].st.size, b); if (strlen(b) > ws) ws = strlen(b);
        }
        for (int i = 0; i < n; i++) {
            char ms[11], dt[20], sz[16], ln[16];
            modestr(v[i].st.mode, ms); when(v[i].st.mtime, dt); sizestr(v[i].st.size, sz); snprintf(ln, 16, "%u", v[i].st.links);
            oprintf("%s ", ms); for (u32 k = strlen(ln); k < wl; k++) oputc(' '); oprintf("%s root root ", ln);
            for (u32 k = strlen(sz); k < ws; k++) oputc(' '); oprintf("%s %s %s", sz, dt, v[i].name);
            char sf = suffix(&v[i]); if (sf && !v[i].islink) oputc(sf);
            if (v[i].islink) { oputs(" -> "); oputs(v[i].link); }
            oputc('\n');
        }
        return;
    }
    if (!tty || o_1) { for (int i = 0; i < n; i++) { oputs(v[i].name); char sf = suffix(&v[i]); if (sf) oputc(sf); oputc('\n'); } return; }
    int w = 0; for (int i = 0; i < n; i++) { int l = (int)strlen(v[i].name) + (suffix(&v[i]) ? 1 : 0); if (l > w) w = l; }
    w += 2; int cols = 80 / w; if (cols < 1) cols = 1;
    int rows = (n + cols - 1) / cols;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int i = c * rows + r; if (i >= n) break;
            oputs(v[i].name); char sf = suffix(&v[i]); int l = (int)strlen(v[i].name) + (sf ? 1 : 0); if (sf) oputc(sf);
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
    if (e->islink) { int n = sys_readlink(path, e->link, 63); if (n < 0) n = 0; e->link[n] = 0; }
}
static int list(const char *path, int top)
{
    struct ustat st;
    if (sys_lstat(path, &st) < 0) { fprintf(2, "ls: %s: not found\n", path); return 1; }
    int isdir = (st.mode & S_IFMT) == S_IFDIR;
    if (!isdir && (st.mode & S_IFMT) == S_IFLNK && !o_l && !o_d) { struct ustat t; if (sys_stat(path, &t) == 0 && (t.mode & S_IFMT) == S_IFDIR) { isdir = 1; st = t; } }
    if (!isdir || o_d) {
        struct ent *e = malloc(sizeof *e); strncpy(e->name, path, 63); e->name[63] = 0; fill(e, path); show(e, 1); free(e); return 0;
    }
    if (multi || o_R) { if (printed_any) oputc('\n'); oprintf("%s:\n", path); }
    printed_any = 1;
    (void)top;
    int fd = sys_open(path, O_RDONLY); if (fd < 0) { fprintf(2, "ls: %s: cannot open\n", path); return 1; }
    struct ent *v = malloc(MAXE * sizeof(struct ent)); int n = 0; struct udirent d;
    while (sys_readdir(fd, &d) > 0 && n < MAXE) {
        if (d.name[0] == '.' && !o_a && !o_A) continue;
        if (o_A && (!strcmp(d.name, ".") || !strcmp(d.name, ".."))) continue;
        struct ent *e = &v[n++]; strncpy(e->name, d.name, 63); e->name[63] = 0;
        char full[200]; strncpy(full, path, 130); full[130] = 0; if (full[strlen(full) - 1] != '/') strcat(full, "/"); strncpy(full + strlen(full), d.name, 60);
        fill(e, full);
    }
    sys_close(fd);
    show(v, n);
    int rc = 0;
    if (o_R)
        for (int i = 0; i < n; i++)
            if ((v[i].st.mode & S_IFMT) == S_IFDIR && strcmp(v[i].name, ".") && strcmp(v[i].name, "..")) {
                char full[200]; strncpy(full, path, 130); full[130] = 0; if (full[strlen(full) - 1] != '/') strcat(full, "/"); strncpy(full + strlen(full), v[i].name, 60);
                rc |= list(full, 0);
            }
    free(v);
    return rc;
}
int main(int argc, char **argv)
{
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            switch (*p) { case 'a': o_a = 1; break; case 'A': o_A = 1; break; case 'l': o_l = 1; break; case 'd': o_d = 1; break; case 'R': o_R = 1; break;
                case 'F': o_F = 1; break; case 'h': o_h = 1; break; case 't': o_t = 1; break; case 'r': o_r = 1; break; case 'S': o_S = 1; break; case '1': o_1 = 1; break; case 's': break;
                default: fprintf(2, "ls: unknown option -%c\n", *p); return 2; }
        }
    }
    tty = sys_isatty(1);
    int n = argc - i; multi = n > 1;
    if (!n) { rc = list(".", 1); oflush(); return rc; }
    /* operands: everything that is not a directory is listed together first, sorted; then each directory */
    struct ent *files = malloc((u32)n * sizeof(struct ent)); int nf = 0;
    struct ent *dirs = malloc((u32)n * sizeof(struct ent)); int nd = 0;
    for (int k = 0; k < n; k++) {
        const char *a = argv[i + k];
        struct ustat st;
        if (sys_lstat(a, &st) < 0) { fprintf(2, "ls: %s: not found\n", a); rc = 1; continue; }
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
