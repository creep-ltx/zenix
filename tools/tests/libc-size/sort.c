/* SPDX-License-Identifier: GPL-2.0-only */
/* the size table: a small sort tool (stdio, malloc, realloc, strdup, qsort, strtol, getenv, printf) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
int main(int argc, char **argv)
{
    char line[256], **v = 0;
    size_t n = 0, cap = 0;
    long limit = argc > 1 ? strtol(argv[1], 0, 10) : 0;
    const char *sep = getenv("SEP");
    FILE *f = argc > 2 ? fopen(argv[2], "r") : stdin;
    while (f && fgets(line, sizeof line, f)) {
        if (n == cap && !(v = realloc(v, (cap = cap ? cap * 2 : 64) * sizeof *v))) return 1;
        v[n++] = strdup(line);
    }
    qsort(v, n, sizeof *v, cmp);
    for (size_t i = 0; i < n && (!limit || (long)i < limit); i++) printf("%s%s", v[i], sep ? sep : "");
    return 0;
}
