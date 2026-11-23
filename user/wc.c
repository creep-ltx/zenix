/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* wc [-l] [-w] [-c] [file...]: lines, words, bytes (all three by default) */
static u32 tl, tw, tc;
static void count(int fd, const char *name, int fl, int fw, int fc, int show)
{
    char b[4096]; int n, inw = 0;          /* 4 KB a call: one trip to the server per 4 KB (audit 2026-11-23 OS-15) */
    u32 lines = 0, words = 0, bytes = 0;
    while ((n = sys_read(fd, b, sizeof b)) > 0)
        for (int i = 0; i < n; i++) {
            bytes++;
            if (b[i] == '\n') lines++;
            int sp = b[i] == ' ' || b[i] == '\n' || b[i] == '\t' || b[i] == '\r';
            if (!sp && !inw) words++;
            inw = !sp;
        }
    int one = fl + fw + fc == 1;
    if (fl) dprintf(1, one ? "%u" : "%7u", lines);
    if (fw) dprintf(1, one ? "%u" : "%7u", words);
    if (fc) dprintf(1, one ? "%u" : "%7u", bytes);
    if (show) dprintf(1, " %s", name);
    dprintf(1, "\n");
    tl += lines; tw += words; tc += bytes;
}
int main(int argc, char **argv)
{
    int fl = 0, fw = 0, fc = 0, files = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'l') fl = 1; else if (*p == 'w') fw = 1; else if (*p == 'c') fc = 1;
            else { dprintf(2, "wc: unknown option -%c\nusage: wc [-lwc] [file...]\n", *p); return 2; } }
    if (!fl && !fw && !fc) fl = fw = fc = 1;
    int rc = 0;
    if (i >= argc) { count(0, "", fl, fw, fc, 0); return 0; }
    for (; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) { dprintf(2, "wc: %s: %s\n", argv[i], zwhy("cannot open")); rc = 1; continue; }
        count(fd, argv[i], fl, fw, fc, 1); sys_close(fd); files++;
    }
    if (files > 1) { if (fl) dprintf(1, "%7u", tl); if (fw) dprintf(1, "%7u", tw); if (fc) dprintf(1, "%7u", tc); dprintf(1, " total\n"); }
    return rc;
}
