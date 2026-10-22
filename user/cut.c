#include "libz.h"
/* cut -f LIST [-d C] [-s] | -c LIST | -b LIST [file...]; LIST: N, N-M, N-, -M, comma separated */
static struct { int lo, hi; } rng[32]; static int nr;
static void parse_list(const char *s)
{
    while (*s && nr < 32) {
        int lo = 1, hi = 100000;
        if (*s == '-') { s++; hi = (int)strtol(s, (char **)&s, 10); }
        else { lo = (int)strtol(s, (char **)&s, 10); if (*s == '-') { s++; hi = isdigit((unsigned char)*s) ? (int)strtol(s, (char **)&s, 10) : 100000; } else hi = lo; }
        rng[nr].lo = lo; rng[nr].hi = hi; nr++;
        if (*s == ',') s++; else break;
    }
}
static int wanted(int k) { for (int i = 0; i < nr; i++) if (k >= rng[i].lo && k <= rng[i].hi) return 1; return 0; }
int main(int argc, char **argv)
{
    int mode = 0, delim = '\t', only = 0, i = 1;                  /* mode 1 fields, 2 chars */
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        char o = argv[i][1]; const char *val = argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "");
        if (o == 'f') { mode = 1; parse_list(val); }
        else if (o == 'c' || o == 'b') { mode = 2; parse_list(val); }
        else if (o == 'd') delim = (unsigned char)val[0];
        else if (o == 's') only = 1;
        else { fprintf(2, "cut: unknown option -%c\n", o); return 2; }
    }
    if (!mode) { fprintf(2, "usage: cut -f LIST [-d C] [-s] | -c LIST [file...]\n"); return 2; }
    int rc = 0, nfiles = argc - i;
    for (int f = 0; f < (nfiles ? nfiles : 1); f++) {
        int fd = 0;
        if (nfiles) { fd = sys_open(argv[i + f], O_RDONLY); if (fd < 0) { fprintf(2, "cut: %s: cannot open\n", argv[i + f]); rc = 1; continue; } }
        struct rd r; rd_init(&r, fd);
        char *line = 0; u32 cap = 0;
        while (rd_getline(&r, &line, &cap) >= 0) {
            if (mode == 2) { for (int k = 0; line[k]; k++) if (wanted(k + 1)) oputc(line[k]); oputc('\n'); continue; }
            if (!strchr(line, delim)) { if (!only) { oputs(line); oputc('\n'); } continue; }
            int field = 1, first = 1; const char *p = line;
            for (;;) {
                const char *e = p; while (*e && *e != delim) e++;
                if (wanted(field)) { if (!first) oputc(delim); oputn(p, (u32)(e - p)); first = 0; }
                if (!*e) break;
                p = e + 1; field++;
            }
            oputc('\n');
        }
        free(line);
        if (nfiles) sys_close(fd);
    }
    return rc;
}
