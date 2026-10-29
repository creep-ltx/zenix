/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* sort [-r] [-n] [-u] [-f] [-b] [-k N] [-t C] [-o FILE] [file...]: stable merge sort of lines */
static int o_r, o_n, o_u, o_f, o_b, o_key; static int o_sep = -1;
static const char *keyof(const char *s)
{
    if (o_key) {
        for (int k = 1; k < o_key; k++) {
            if (o_sep >= 0) { while (*s && *s != o_sep) s++; if (*s) s++; }
            else { while (*s == ' ' || *s == '\t') s++; while (*s && *s != ' ' && *s != '\t') s++; }
        }
        if (o_sep < 0) while (*s == ' ' || *s == '\t') s++;
    } else if (o_b) while (*s == ' ' || *s == '\t') s++;
    return s;
}
static int cmp(const char *a, const char *b)
{
    a = keyof(a); b = keyof(b);
    int c;
    if (o_n) { long x = strtol(a, 0, 10), y = strtol(b, 0, 10); c = x < y ? -1 : x > y; }
    else if (o_f) { while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; } c = tolower((unsigned char)*a) - tolower((unsigned char)*b); }
    else c = strcmp(a, b);
    return o_r ? -c : c;
}
static void msort(char **v, char **tmp, u32 n)
{
    if (n < 2) return;
    u32 m = n / 2;
    msort(v, tmp, m); msort(v + m, tmp, n - m);
    u32 i = 0, j = m, k = 0;
    while (i < m && j < n) tmp[k++] = cmp(v[j], v[i]) < 0 ? v[j++] : v[i++];   /* stable: equal keys keep the left one first */
    while (i < m) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, n * sizeof(char *));
}
int main(int argc, char **argv)
{
    const char *outfile = 0; int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (argv[i][1] == 'k' || argv[i][1] == 't' || argv[i][1] == 'o') {      /* -k2 or -k 2 */
            char opt = argv[i][1];
            const char *v = argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[++i] : "");
            if (opt == 'k') o_key = atoi(v); else if (opt == 't') o_sep = (unsigned char)v[0]; else outfile = v;
            continue;
        }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'r') o_r = 1; else if (*p == 'n') o_n = 1; else if (*p == 'u') o_u = 1; else if (*p == 'f') o_f = 1; else if (*p == 'b') o_b = 1;
            else { dprintf(2, "sort: unknown option -%c\n", *p); return 2; }
        }
    }
    char **v = 0; u32 n = 0, cap = 0;
    int nfiles = argc - i;
    for (int f = 0; f < (nfiles ? nfiles : 1); f++) {
        int fd = 0;
        if (nfiles && strcmp(argv[i + f], "-")) { fd = sys_open(argv[i + f], O_RDONLY); if (fd < 0) { dprintf(2, "sort: %s: %s\n", argv[i + f], zwhy("cannot open")); rc = 2; continue; } }
        struct rd r; rd_init(&r, fd);
        char *line = 0; u32 lc = 0;
        while (rd_getline(&r, &line, &lc) >= 0) {
            if (n == cap) { u32 nc = cap ? cap * 2 : 256; char **nv = realloc(v, nc * sizeof(char *)); if (!nv) { dprintf(2, "sort: out of memory\n"); return 2; } v = nv; cap = nc; }
            if (!(v[n] = strdup(line))) { dprintf(2, "sort: out of memory\n"); return 2; }
            n++;
        }
        free(line);
        if (fd) sys_close(fd);
    }
    char **tmp = malloc((n ? n : 1) * sizeof(char *));
    if (!tmp) { dprintf(2, "sort: out of memory\n"); return 2; }
    msort(v, tmp, n);
    int ofd = 1;
    if (outfile) { ofd = sys_open(outfile, O_WRONLY | O_CREAT | O_TRUNC); if (ofd < 0) { dprintf(2, "sort: %s: %s\n", outfile, zwhy("cannot write")); return 2; } }
    for (u32 k = 0; k < n; k++) {
        if (o_u && k && cmp(v[k - 1], v[k]) == 0) continue;
        if (outfile) { sys_write(ofd, v[k], strlen(v[k])); sys_write(ofd, "\n", 1); } else { oputs(v[k]); oputc('\n'); }
    }
    if (outfile) sys_close(ofd);
    return rc;
}
