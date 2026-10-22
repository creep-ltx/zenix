#include "libz.h"
#include "regex.h"
/* grep [-ivncloqhHrEFwxs] [-A N] [-B N] [-C N] [-e PAT] PATTERN [file...]
 * Patterns are POSIX extended regular expressions (-F: fixed strings). */
static int o_i, o_v, o_n, o_c, o_l, o_L, o_o, o_q, o_h, o_H, o_r, o_F, o_w, o_x, o_s, ctx_a, ctx_b;
static void *re; static const char *fixed; static int rc_found;
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
    char *before[16]; int nb = 0, bhead = 0, bln[16];
    for (int k = 0; k < 16; k++) before[k] = 0;
    int len;
    while ((len = rd_getline(&r, &line, &cap)) >= 0) {
        ln++;
        int ms = 0, me = 0, m = matches(line, len, &ms, &me);
        if (o_v) m = !m;
        if (m) {
            count++; rc_found = 1;
            if (o_q) return 1;
            if (o_l || o_L) break;
            if (!o_c) {
                if (nb) { for (int k = 0; k < nb; k++) { int idx = (bhead - nb + k + 32) % 16; emit(name, bln[idx], '-', before[idx]); } nb = 0; }
                if (o_o && !o_v) {
                    int from = ms; const char *p = line;
                    for (;;) { if (multi && !o_h) { oputs(name); oputc(':'); } if (o_n) oprintf("%d:", ln); oputn(p + from, (u32)(me - from)); oputc('\n');
                        int s2, e2; if (me >= len || !matches(line + me, len - me, &s2, &e2) ) break; from = me + s2; me = me + e2; if (me == from) break; }
                } else emit(name, ln, ':', line);
                after = ctx_a;
            }
        } else if (!o_c) {
            if (after > 0) { emit(name, ln, '-', line); after--; }
            else if (ctx_b) { int idx = bhead; if (before[idx]) free(before[idx]); before[idx] = strdup(line); bln[idx] = ln; bhead = (bhead + 1) % 16; if (nb < ctx_b && nb < 16) nb++; }
        }
    }
    if (o_c) { if (multi && !o_h) { oputs(name); oputc(':'); } oprintf("%d\n", count); }
    if (o_l && count) { oputs(name); oputc('\n'); }
    if (o_L && !count) { oputs(name); oputc('\n'); }
    free(line);
    return count != 0;
}

static void grep_path(const char *path, int top)
{
    struct ustat st;
    if (sys_stat(path, &st) < 0) { if (!o_s) fprintf(2, "grep: %s: not found\n", path); return; }
    if ((st.mode & S_IFMT) == S_IFDIR) {
        if (!o_r) { if (!o_s) fprintf(2, "grep: %s: is a directory\n", path); return; }
        int fd = sys_open(path, O_RDONLY); if (fd < 0) return;
        char *names[256]; int n = 0; struct udirent d;
        while (sys_readdir(fd, &d) > 0 && n < 256) if (strcmp(d.name, ".") && strcmp(d.name, "..")) names[n++] = strdup(d.name);
        sys_close(fd);
        for (int i = 0; i < n; i++) { char c[256]; strcpy(c, path); u32 l = strlen(c); if (l && c[l - 1] != '/') strcat(c, "/"); strncpy(c + strlen(c), names[i], 100); grep_path(c, 0); free(names[i]); }
        return;
    }
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) { if (!o_s) fprintf(2, "grep: %s: cannot open\n", path); return; }
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
    if (ctx_b > 16) ctx_b = 16;
    if (o_F) fixed = pat;
    else { re = re_comp(pat, (o_i ? RE_ICASE : 0) | RE_LONGEST); if (!re) { fprintf(2, "grep: bad pattern %s\n", pat); return 2; } }
    int nfiles = argc - i;
    multi = nfiles > 1 || o_r || o_H;
    if (!nfiles) { if (o_r) grep_path(".", 1); else grep_fd(0, "(standard input)"); }
    else for (; i < argc; i++) grep_path(argv[i], 1);
    return rc_found ? 0 : 1;
}
