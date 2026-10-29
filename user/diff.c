/* SPDX-License-Identifier: GPL-2.0-only */
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
        char *d = 0;
        if ((u32)l->n == max) { u32 nm = max ? max * 2 : 128; char **nv = realloc(l->v, nm * sizeof(char *)); if (!nv) goto oom; l->v = nv; max = nm; }
        if (!(d = strdup(line))) goto oom;
        l->v[l->n++] = d;
    }
    free(line);
    if (fd) sys_close(fd);
    return 0;
oom:                                /* what was read is freed again; n = -1 tells the caller it was memory, not the file */
    free(line);
    if (fd) sys_close(fd);
    for (int k = 0; k < l->n; k++) free(l->v[k]);
    free(l->v); l->v = 0; l->n = -1;
    return -1;
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
    if (!o) return -1;
    for (int i = 0; i < pre; i++) o[k++] = ' ';
    if ((long)an * bn > 3000000L) {                               /* too big for the table: treat the middle as replaced */
        for (int i = 0; i < an; i++) o[k++] = '-';
        for (int j = 0; j < bn; j++) o[k++] = '+';
    } else {
        unsigned short *L = malloc((u32)(an + 1) * (u32)(bn + 1) * 2);
        if (!L) { free(o); return -1; }
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

/* hdr: the "diff -r A B" line of a directory comparison, printed only when the two files differ (as GNU diff) */
static int diff_files(const char *p1, const char *p2, const char *l1, const char *l2, const char *hdr)
{
    struct lines a = { 0, 0 }, b = { 0, 0 };
    char *ops = 0; struct hunk { int a0, ac, b0, bc, k0, k1; } *h = 0;
    int rc, total;
    if (read_lines(p1, &a) < 0) { dprintf(2, "diff: %s: %s\n", p1, a.n < 0 ? "out of memory" : zwhy("cannot open")); rc = 2; goto out; }
    if (read_lines(p2, &b) < 0) { dprintf(2, "diff: %s: %s\n", p2, b.n < 0 ? "out of memory" : zwhy("cannot open")); rc = 2; goto out; }
    total = script(&a, &b, &ops);
    if (total < 0) { dprintf(2, "diff: out of memory\n"); rc = 2; goto out; }
    int differ = 0;
    for (int k = 0; k < total; k++) if (ops[k] != ' ') { differ = 1; break; }
    if (!differ) { rc = 0; goto out; }
    if (o_q) { oprintf("Files %s and %s differ\n", l1, l2); rc = 1; goto out; }
    if (hdr) oputs(hdr);

    /* hunks: runs of non-common ops, with their positions in a and b */
    int ai = 0, bi = 0;
    h = malloc(sizeof *h * (u32)(total + 1)); int nh = 0;
    if (!h) { dprintf(2, "diff: out of memory\n"); rc = 2; goto out; }
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
        rc = 1; goto out;
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
    rc = 1;
out:
    for (int k = 0; k < a.n; k++) free(a.v[k]);
    for (int k = 0; k < b.n; k++) free(b.v[k]);
    free(a.v); free(b.v); free(ops); free(h);
    return rc;
}

static int diff_dirs(const char *d1, const char *d2)
{
    int rc = 0;
    /* the names of both sides, each once, sorted; lists and paths on the heap (one level per directory) */
    char **l1, **l2; int n1 = dir_list(d1, &l1), n2 = dir_list(d2, &l2);
    if (n1 < 0 || n2 < 0) { dprintf(2, "diff: %s: %s\n", n1 < 0 ? d1 : d2, zwhy("cannot read")); dir_free(l1, n1); dir_free(l2, n2); return 2; }
    char **names = malloc((u32)(n1 + n2 + 1) * sizeof *names); int nn = 0;
    if (!names) { dprintf(2, "diff: out of memory\n"); dir_free(l1, n1); dir_free(l2, n2); return 2; }
    for (int i = 0; i < n1; i++) names[nn++] = l1[i];
    for (int i = 0; i < n2; i++) { int dup = 0; for (int k = 0; k < n1; k++) if (!strcmp(l1[k], l2[i])) dup = 1; if (!dup) names[nn++] = l2[i]; }
    for (int i = 1; i < nn; i++) { char *t = names[i]; int j = i; while (j > 0 && strcmp(names[j - 1], t) > 0) { names[j] = names[j - 1]; j--; } names[j] = t; }
    for (int i = 0; i < nn; i++) {
        struct ustat s1, s2;
        char *p1 = path_join(d1, names[i]), *p2 = p1 ? path_join(d2, names[i]) : 0;
        if (!p2) { dprintf(2, "diff: %s/%s: %s\n", p1 ? d2 : d1, names[i], zwhy("cannot compare")); free(p1); rc = 2; continue; }
        int e1 = sys_stat(p1, &s1) == 0, e2 = sys_stat(p2, &s2) == 0;
        if (!e1) { oprintf("Only in %s: %s\n", d2, names[i]); if (rc < 1) rc = 1; }
        else if (!e2) { oprintf("Only in %s: %s\n", d1, names[i]); if (rc < 1) rc = 1; }
        else {
            int dir1 = (s1.mode & S_IFMT) == S_IFDIR, dir2 = (s2.mode & S_IFMT) == S_IFDIR;
            if (dir1 && dir2) { if (o_r) { int r = diff_dirs(p1, p2); if (r > rc) rc = r; } else oprintf("Common subdirectories: %s and %s\n", p1, p2); }
            else {
                char *hdr = 0;
                if (!o_q) {
                    u32 hl = strlen(p1) + strlen(p2) + 16;
                    if ((hdr = malloc(hl))) snprintf(hdr, hl, "diff %s%s %s\n", o_u ? "-u " : "", p1, p2);
                }
                int r = diff_files(p1, p2, p1, p2, hdr); if (r > rc) rc = r;
                free(hdr);
            }
        }
        free(p1); free(p2);
    }
    free(names); dir_free(l1, n1); dir_free(l2, n2);
    return rc;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (argv[i][1] == 'U') { o_u = 1; o_ctx = atoi(argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "3")); continue; }
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'u') o_u = 1; else if (*p == 'q') o_q = 1; else if (*p == 'i') o_i = 1; else if (*p == 'w') o_w = 1; else if (*p == 'r') o_r = 1; else { dprintf(2, "diff: unknown option -%c\n", *p); return 2; } }
    }
    if (argc - i != 2) { dprintf(2, "usage: diff [-uqiwr] [-U N] file1 file2\n"); return 2; }
    struct ustat s1, s2;
    int e1 = sys_stat(argv[i], &s1), e2 = sys_stat(argv[i + 1], &s2);
    if (e1 == 0 && e2 == 0 && (s1.mode & S_IFMT) == S_IFDIR && (s2.mode & S_IFMT) == S_IFDIR) { int rc = diff_dirs(argv[i], argv[i + 1]); oflush(); return rc; }
    int rc = diff_files(argv[i], argv[i + 1], argv[i], argv[i + 1], 0);
    oflush();
    return rc;
}
