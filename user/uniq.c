#include "libz.h"
/* uniq [-c] [-d] [-u] [-i] [input [output]]: collapse adjacent duplicate lines */
static int o_c, o_d, o_u, o_i;
static int same(const char *a, const char *b)
{
    if (!o_i) return !strcmp(a, b);
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
    return *a == *b;
}
int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'c') o_c = 1; else if (*p == 'd') o_d = 1; else if (*p == 'u') o_u = 1; else if (*p == 'i') o_i = 1; else { fprintf(2, "uniq: unknown option -%c\n", *p); return 2; } }
    int fd = 0;
    if (i < argc) { fd = sys_open(argv[i], O_RDONLY); if (fd < 0) { fprintf(2, "uniq: %s: cannot open\n", argv[i]); return 1; } }
    struct rd r; rd_init(&r, fd);
    char *line = 0, *prev = 0; u32 cap = 0; long count = 0;
#define FLUSH() do { if (prev && (!o_d || count > 1) && (!o_u || count == 1)) { if (o_c) oprintf("%7d ", (int)count); oputs(prev); oputc('\n'); } } while (0)
    while (rd_getline(&r, &line, &cap) >= 0) {
        if (prev && same(prev, line)) { count++; continue; }
        FLUSH();
        free(prev); prev = strdup(line); count = 1;
    }
    FLUSH();
    return 0;
}
