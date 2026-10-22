#include "libz.h"
/* diff [-u] [-U N] [-q] [-i] [-w] [-r] file1 file2   (directories with -r)
 * Normal and unified output; exit status 0 same, 1 different, 2 trouble. */
static int o_u, o_ctx = 3, o_q, o_i, o_w, o_r;
struct lines { char **v; int n; };

static int read_lines(const char *path, struct lines *l)
{
    l->v = 0; l->n = 0;
    int fd = path[0] == '-' && !path[1] ? 0 : sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct rd r; rd_init(&r, fd);
    char *line = 0; u32 cap = 0, max = 0;
    while (rd_getline(&r, &line, &cap) >= 0) {
        if ((u32)l->n == max) { u32 nm = max ? max * 2 : 128; char **nv = malloc(nm * sizeof(char *)); if (!nv) return -1; if (l->n) memcpy(nv, l->v, (u32)l->n * sizeof(char *)); l->v = nv; max = nm; }
        l->v[l->n++] = strdup(line);
    }
    free(line);
    if (fd) sys_close(fd);
    return 0;
}

static int same(const char *a, const char *b)
{
    if (!o_i && !o_w) return !strcmp(a, b);
    for (;;) {
        if (o_w) { while (*a == ' ' || *a == '\t') a++; while (*b == ' ' || *b == '\t') b++; }
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (o_i) { x = tolower(x); y = tolower(y); }
        if (x != y) return 0;
        if (!x) return 1;
        a++; b++;
    }
}

/* the edit script: op[k] = ' ' common, '-' only in a, '+' only in b */
static int script(struct lines *a, struct lines *b, char **ops)
{
    int n = a->n, m = b->n, pre = 0, suf = 0;
    while (pre < n && pre < m && same(a->v[pre], b->v[pre])) pre++;
    while (suf < n - pre && suf < m - pre && same(a->v[n - 1 - suf], b->v[m - 1 - suf])) suf++;
    int an = n - pre - suf, bn = m - pre - suf;
    char *o = malloc((u32)(n + m + 2)); int k = 0;
    for (int i = 0; i < pre; i++) o[k++] = ' ';
    if ((long)an * bn > 3000000L) {                               /* too big for the table: treat the middle as replaced */
        for (int i = 0; i < an; i++) o[k++] = '-';
        for (int j = 0; j < bn; j++) o[k++] = '+';
    } else {
        unsigned short *L = malloc((u32)(an + 1) * (u32)(bn + 1) * 2);
        if (!L) return -1;
        #define LT(i, j) L[(u32)(i) * (u32)(bn + 1) + (u32)(j)]
        for (int i = an; i >= 0; i--)
            for (int j = bn; j >= 0; j--) {
                if (i == an || j == bn) LT(i, j) = 0;
                else if (same(a->v[pre + i], b->v[pre + j])) LT(i, j) = (unsigned short)(LT(i + 1, j + 1) + 1);
                else LT(i, j) = LT(i + 1, j) >= LT(i, j + 1) ? LT(i + 1, j) : LT(i, j + 1);
            }
        int i = 0, j = 0;
        while (i < an || j < bn) {
            if (i < an && j < bn && same(a->v[pre + i], b->v[pre + j])) { o[k++] = ' '; i++; j++; }
            else if (i < an && (j == bn || LT(i + 1, j) >= LT(i, j + 1))) { o[k++] = '-'; i++; }
            else { o[k++] = '+'; j++; }
        }
        free(L);
    }
    for (int i = 0; i < suf; i++) o[k++] = ' ';
    o[k] = 0; *ops = o;
    return k;
}

static void range(int start, int cnt) { if (cnt <= 1) oprintf("%d", cnt ? start : start); else oprintf("%d,%d", start, start + cnt - 1); }

static int diff_files(const char *p1, const char *p2, const char *l1, const char *l2)
{
    struct lines a, b;
    if (read_lines(p1, &a) < 0) { fprintf(2, "diff: %s: %s\n", p1, zwhy("cannot open")); return 2; }
    if (read_lines(p2, &b) < 0) { fprintf(2, "diff: %s: %s\n", p2, zwhy("cannot open")); return 2; }
    char *ops; int total = script(&a, &b, &ops);
    if (total < 0) { fprintf(2, "diff: out of memory\n"); return 2; }
    int differ = 0;
    for (int k = 0; k < total; k++) if (ops[k] != ' ') { differ = 1; break; }
    if (!differ) return 0;
    if (o_q) { oprintf("Files %s and %s differ\n", l1, l2); return 1; }

    /* hunks: runs of non-common ops, with their positions in a and b */
    int ai = 0, bi = 0;
    struct hunk { int a0, ac, b0, bc, k0, k1; } *h = malloc(sizeof(struct hunk) * (u32)(total / 1 + 1)); int nh = 0;
    for (int k = 0; k < total;) {
        if (ops[k] == ' ') { ai++; bi++; k++; continue; }
        struct hunk *x = &h[nh++]; x->a0 = ai; x->b0 = bi; x->k0 = k; x->ac = x->bc = 0;
        while (k < total && ops[k] != ' ') { if (ops[k] == '-') { x->ac++; ai++; } else { x->bc++; bi++; } k++; }
        x->k1 = k;
    }
    if (!o_u) {
        for (int i = 0; i < nh; i++) {
            struct hunk *x = &h[i];
            int ar = x->ac ? x->a0 + 1 : x->a0, br = x->bc ? x->b0 + 1 : x->b0;
            if (!x->ac) { oprintf("%da", x->a0); range(br, x->bc); oputc('\n'); }
            else if (!x->bc) { range(ar, x->ac); oprintf("d%d\n", x->b0); }
            else { range(ar, x->ac); oputc('c'); range(br, x->bc); oputc('\n'); }
            for (int k = 0; k < x->ac; k++) { oputs("< "); oputs(a.v[x->a0 + k]); oputc('\n'); }
            if (x->ac && x->bc) oputs("---\n");
            for (int k = 0; k < x->bc; k++) { oputs("> "); oputs(b.v[x->b0 + k]); oputc('\n'); }
        }
        return 1;
    }
    oprintf("--- %s\n+++ %s\n", l1, l2);
    for (int i = 0; i < nh;) {
        int j = i;
        while (j + 1 < nh && h[j + 1].a0 - (h[j].a0 + h[j].ac) <= 2 * o_ctx) j++;      /* hunks close together share one block */
        int a_start = h[i].a0 - o_ctx; if (a_start < 0) a_start = 0;
        int b_start = h[i].b0 - o_ctx; if (b_start < 0) b_start = 0;
        int a_end = h[j].a0 + h[j].ac + o_ctx; if (a_end > a.n) a_end = a.n;
        int b_end = h[j].b0 + h[j].bc + o_ctx; if (b_end > b.n) b_end = b.n;
        int acount = a_end - a_start, bcount = b_end - b_start;
        oprintf("@@ -%d", acount ? a_start + 1 : a_start); if (acount != 1) oprintf(",%d", acount);
        oprintf(" +%d", bcount ? b_start + 1 : b_start); if (bcount != 1) oprintf(",%d", bcount);
        oputs(" @@\n");
        int pa = a_start;
        for (int x = i; x <= j; x++) {
            for (; pa < h[x].a0; pa++) { oputc(' '); oputs(a.v[pa]); oputc('\n'); }
            for (int k = 0; k < h[x].ac; k++) { oputc('-'); oputs(a.v[h[x].a0 + k]); oputc('\n'); }
            for (int k = 0; k < h[x].bc; k++) { oputc('+'); oputs(b.v[h[x].b0 + k]); oputc('\n'); }
            pa = h[x].a0 + h[x].ac;
        }
        for (; pa < a_end; pa++) { oputc(' '); oputs(a.v[pa]); oputc('\n'); }
        i = j + 1;
    }
    return 1;
}

static int diff_dirs(const char *d1, const char *d2)
{
    int rc = 0;
    char *names[512]; int nn = 0;
    for (int side = 0; side < 2; side++) {
        int fd = sys_open(side ? d2 : d1, O_RDONLY); if (fd < 0) continue;
        struct udirent d;
        while (sys_readdir(fd, &d) > 0 && nn < 512) {
            if (!strcmp(d.name, ".") || !strcmp(d.name, "..")) continue;
            int dup = 0; for (int i = 0; i < nn; i++) if (!strcmp(names[i], d.name)) dup = 1;
            if (!dup) names[nn++] = strdup(d.name);
        }
        sys_close(fd);
    }
    for (int i = 1; i < nn; i++) { char *t = names[i]; int j = i; while (j > 0 && strcmp(names[j - 1], t) > 0) { names[j] = names[j - 1]; j--; } names[j] = t; }
    for (int i = 0; i < nn; i++) {
        char p1[200], p2[200]; struct ustat s1, s2;
        strcpy(p1, d1); strcat(p1, "/"); strcat(p1, names[i]); strcpy(p2, d2); strcat(p2, "/"); strcat(p2, names[i]);
        int e1 = sys_stat(p1, &s1) == 0, e2 = sys_stat(p2, &s2) == 0;
        if (!e1) { oprintf("Only in %s: %s\n", d2, names[i]); rc = 1; continue; }
        if (!e2) { oprintf("Only in %s: %s\n", d1, names[i]); rc = 1; continue; }
        int dir1 = (s1.mode & S_IFMT) == S_IFDIR, dir2 = (s2.mode & S_IFMT) == S_IFDIR;
        if (dir1 && dir2) { if (o_r) { int r = diff_dirs(p1, p2); if (r > rc) rc = r; } else oprintf("Common subdirectories: %s and %s\n", p1, p2); continue; }
        if (!o_q) oprintf("diff %s%s %s\n", o_u ? "-u " : "", p1, p2);
        int r = diff_files(p1, p2, p1, p2); if (r > rc) rc = r;
    }
    return rc;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (argv[i][1] == 'U') { o_u = 1; o_ctx = atoi(argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "3")); continue; }
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'u') o_u = 1; else if (*p == 'q') o_q = 1; else if (*p == 'i') o_i = 1; else if (*p == 'w') o_w = 1; else if (*p == 'r') o_r = 1; else { fprintf(2, "diff: unknown option -%c\n", *p); return 2; } }
    }
    if (argc - i != 2) { fprintf(2, "usage: diff [-uqiwr] [-U N] file1 file2\n"); return 2; }
    struct ustat s1, s2;
    int e1 = sys_stat(argv[i], &s1), e2 = sys_stat(argv[i + 1], &s2);
    if (e1 == 0 && e2 == 0 && (s1.mode & S_IFMT) == S_IFDIR && (s2.mode & S_IFMT) == S_IFDIR) { int rc = diff_dirs(argv[i], argv[i + 1]); oflush(); return rc; }
    int rc = diff_files(argv[i], argv[i + 1], argv[i], argv[i + 1]);
    oflush();
    return rc;
}
