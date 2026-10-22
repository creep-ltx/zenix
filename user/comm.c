// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* comm [-123] file1 file2: compare two sorted files line by line; columns: only in 1, only in 2, in both */
int main(int argc, char **argv)
{
    int hide[4] = {0}, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *p = argv[i] + 1; *p; p++) { if (*p >= '1' && *p <= '3') hide[*p - '0'] = 1; else { fprintf(2, "comm: unknown option -%c\n", *p); return 2; } }
    if (argc - i != 2) { fprintf(2, "usage: comm [-123] file1 file2\n"); return 2; }
    int fa = strcmp(argv[i], "-") ? sys_open(argv[i], O_RDONLY) : 0, fb = strcmp(argv[i + 1], "-") ? sys_open(argv[i + 1], O_RDONLY) : 0;
    if (fa < 0 || fb < 0) { fprintf(2, "comm: %s: %s\n", fa < 0 ? argv[i] : argv[i + 1], zwhy("cannot open")); return 1; }
    struct rd ra, rb; rd_init(&ra, fa); rd_init(&rb, fb);
    char *la = 0, *lb = 0; u32 ca = 0, cb = 0;
    int ga = rd_getline(&ra, &la, &ca) >= 0, gb = rd_getline(&rb, &lb, &cb) >= 0;
    const char *t2 = hide[1] ? "" : "\t";
    const char *t3 = hide[1] ? (hide[2] ? "" : "\t") : (hide[2] ? "\t" : "\t\t");
    while (ga || gb) {
        int c = !ga ? 1 : !gb ? -1 : strcmp(la, lb);
        if (c < 0) { if (!hide[1]) { oputs(la); oputc('\n'); } ga = rd_getline(&ra, &la, &ca) >= 0; }
        else if (c > 0) { if (!hide[2]) { oputs(t2); oputs(lb); oputc('\n'); } gb = rd_getline(&rb, &lb, &cb) >= 0; }
        else { if (!hide[3]) { oputs(t3); oputs(la); oputc('\n'); } ga = rd_getline(&ra, &la, &ca) >= 0; gb = rd_getline(&rb, &lb, &cb) >= 0; }
    }
    return 0;
}
