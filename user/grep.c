#include "libz.h"
#include "regex.h"
/* grep [-ivncloqhHrEFwxs] [-A N] [-B N] [-C N] [-e PAT] PATTERN [file...]
 * Patterns are POSIX extended regular expressions (-F: fixed strings). */
static int o_i, o_v, o_n, o_c, o_l, o_L, o_o, o_q, o_h, o_H, o_r, o_F, o_w, o_x, o_s, ctx_a, ctx_b;
static void *re; static const char *fixed; static int rc_found, rc_err;
static int multi;

static int word_edge(const char *s, int a, int b) { return (a == 0 || !isalnum((unsigned char)s[a - 1])) && !isalnum((unsigned char)s[b]) ; }

/* does the line match? start/end of the first match go to *ms, *me */
static int matches(const char *line, int len, int *ms, int *me)
{
    if (o_F) {
        u32 fl = strlen(fixed);
        for (int p = 0; p + (int)fl <= len; p++) {
            int ok = 1; for (u32 k = 0; k < fl; k++) { int a = (unsigned char)line[p + k], b = (unsigned char)fixed[k]; if (o_i) { a = tolower(a); b = tolower(b); } if (a != b) { ok = 0; break; } }
            if (ok && (!o_w || word_edge(line, p, p + (int)fl)) && (!o_x || (p == 0 && (int)fl == len))) { *ms = p; *me = p + (int)fl; return 1; }
        }
        return 0;
    }
    int caps[2 * RE_GROUPS], from = 0;
    while (from <= len) {
        if (!re_exec(re, line, len, from, caps)) return 0;
        int s = caps[0], e = caps[1];
        if ((!o_w || word_edge(line, s, e)) && (!o_x || (s == 0 && e == len))) { *ms = s; *me = e; return 1; }
        from = s + 1;
    }
    return 0;
}

static void emit(const char *name, int ln, int sep, const char *line)
{
    if (multi && !o_h) { oputs(name); oputc(sep); }
    if (o_n) { oprintf("%d", ln); oputc(sep); }
    oputs(line); oputc('\n');
}

static int grep_fd(int fd, const char *name)
{
    struct rd r; rd_init(&r, fd);
    char *line = 0; u32 cap = 0; int ln = 0, count = 0, after = 0;
    int bsz = ctx_b ? ctx_b : 1;                       /* the -B lines: a ring as long as asked for */
    char **before = xmalloc((u32)bsz * sizeof(char *)); int nb = 0, bhead = 0, *bln = xmalloc((u32)bsz * sizeof(int));
    for (int k = 0; k < bsz; k++) before[k] = 0;
    int len;
    while ((len = rd_getline(&r, &line, &cap)) >= 0) {
        ln++;
        int ms = 0, me = 0, m = matches(line, len, &ms, &me);
        if (o_v) m = !m;
        if (m) {
            count++; rc_found = 1;
            if (o_q) { for (int k = 0; k < bsz; k++) free(before[k]); free(before); free(bln); free(line); return 1; }
            if (o_l || o_L) break;
            if (!o_c) {
                if (nb) { for (int k = 0; k < nb; k++) { int idx = (bhead - nb + k + bsz) % bsz; emit(name, bln[idx], '-', before[idx]); } nb = 0; }
                if (o_o && !o_v) {
                    int from = ms; const char *p = line;
                    for (;;) { if (multi && !o_h) { oputs(name); oputc(':'); } if (o_n) oprintf("%d:", ln); oputn(p + from, (u32)(me - from)); oputc('\n');
                        int s2, e2; if (me >= len || !matches(line + me, len - me, &s2, &e2) ) break; from = me + s2; me = me + e2; if (me == from) break; }
                } else emit(name, ln, ':', line);
                after = ctx_a;
            }
        } else if (!o_c) {
            if (after > 0) { emit(name, ln, '-', line); after--; }
            else if (ctx_b) { int idx = bhead; if (before[idx]) free(before[idx]); if (!(before[idx] = strdup(line))) { fprintf(2, "grep: out of memory\n"); _zexit(2); } bln[idx] = ln; bhead = (bhead + 1) % bsz; if (nb < ctx_b) nb++; }
        }
    }
    if (o_c) { if (multi && !o_h) { oputs(name); oputc(':'); } oprintf("%d\n", count); }
    if (o_l && count) { oputs(name); oputc('\n'); }
    if (o_L && !count) { oputs(name); oputc('\n'); }
    for (int k = 0; k < bsz; k++) free(before[k]);
    free(before); free(bln);
    free(line);
    return count != 0;
}

static void grep_path(const char *path, int top)
{
    struct ustat st;
    if (path[0] == '-' && !path[1] && top) { grep_fd(0, "(standard input)"); return; }     /* "-": standard input */
    if (sys_stat(path, &st) < 0) { if (!o_s) fprintf(2, "grep: %s: %s\n", path, zwhy("no such file or directory")); rc_err = 1; return; }
    if ((st.mode & S_IFMT) == S_IFDIR) {
        if (!o_r) { if (!o_s) fprintf(2, "grep: %s: is a directory\n", path); rc_err = 1; return; }
        char **names; int n = dir_list(path, &names);         /* heap, not stack: one level per directory */
        if (n < 0) { if (!o_s) fprintf(2, "grep: %s: %s\n", path, zwhy("cannot open")); rc_err = 1; return; }
        for (int i = 0; i < n; i++) {
            char *c = path_join(path, names[i]);
            if (c) grep_path(c, 0); else { if (!o_s) fprintf(2, "grep: %s/%s: %s\n", path, names[i], zwhy("cannot open")); rc_err = 1; }
            free(c);
        }
        dir_free(names, n);
        return;
    }
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) { if (!o_s) fprintf(2, "grep: %s: %s\n", path, zwhy("cannot open")); rc_err = 1; return; }
    grep_fd(fd, path); sys_close(fd);
    (void)top;
}

int main(int argc, char **argv)
{
    int i = 1; const char *pat = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-e") && i + 1 < argc) { pat = argv[++i]; continue; }
        if (argv[i][1] == 'A' || argv[i][1] == 'B' || argv[i][1] == 'C') {     /* -A2 or -A 2 */
            char opt = argv[i][1];
            const char *v = argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "0");
            int k = atoi(v);
            if (opt == 'A' || opt == 'C') ctx_a = k;
            if (opt == 'B' || opt == 'C') ctx_b = k;
            continue;
        }
        for (const char *p = argv[i] + 1; *p; p++) {
            switch (*p) {
            case 'i': o_i = 1; break; case 'v': o_v = 1; break; case 'n': o_n = 1; break; case 'c': o_c = 1; break;
            case 'l': o_l = 1; break; case 'L': o_L = 1; break; case 'o': o_o = 1; break; case 'q': o_q = 1; break;
            case 'h': o_h = 1; break; case 'H': o_H = 1; break; case 'r': case 'R': o_r = 1; break; case 'E': break;
            case 'F': o_F = 1; break; case 'w': o_w = 1; break; case 'x': o_x = 1; break; case 's': o_s = 1; break;
            default: fprintf(2, "grep: unknown option -%c\n", *p); return 2;
            }
        }
    }
    if (!pat) { if (i >= argc) { fprintf(2, "usage: grep [options] PATTERN [file...]\n"); return 2; } pat = argv[i++]; }
    if (o_F) fixed = pat;
    else { re = re_comp(pat, (o_i ? RE_ICASE : 0) | RE_LONGEST); if (!re) { fprintf(2, "grep: bad pattern %s\n", pat); return 2; } }
    int nfiles = argc - i;
    multi = nfiles > 1 || o_r || o_H;
    if (!nfiles) { if (o_r) grep_path(".", 1); else grep_fd(0, "(standard input)"); }
    else for (; i < argc; i++) grep_path(argv[i], 1);
    return rc_err && !(o_q && rc_found) ? 2 : rc_found ? 0 : 1;     /* as GNU grep: trouble is 2, unless -q found a match */
}
