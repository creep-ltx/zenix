#include "libz.h"
/* wc [-l] [-w] [-c] [file...]: lines, words, bytes (all three by default) */
static u32 tl, tw, tc;
static void count(int fd, const char *name, int fl, int fw, int fc, int show)
{
    char b[512]; int n, inw = 0;
    u32 lines = 0, words = 0, bytes = 0;
    while ((n = sys_read(fd, b, sizeof b)) > 0)
        for (int i = 0; i < n; i++) {
            bytes++;
            if (b[i] == '\n') lines++;
            int sp = b[i] == ' ' || b[i] == '\n' || b[i] == '\t' || b[i] == '\r';
            if (!sp && !inw) words++;
            inw = !sp;
        }
    if (fl) printf("%7u", lines);
    if (fw) printf("%7u", words);
    if (fc) printf("%7u", bytes);
    if (show) printf(" %s", name);
    printf("\n");
    tl += lines; tw += words; tc += bytes;
}
int main(int argc, char **argv)
{
    int fl = 0, fw = 0, fc = 0, files = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'l') fl = 1; else if (*p == 'w') fw = 1; else if (*p == 'c') fc = 1; }
    if (!fl && !fw && !fc) fl = fw = fc = 1;
    int rc = 0;
    if (i >= argc) { count(0, "", fl, fw, fc, 0); return 0; }
    for (; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(2, "wc: %s: cannot open\n", argv[i]); rc = 1; continue; }
        count(fd, argv[i], fl, fw, fc, 1); sys_close(fd); files++;
    }
    if (files > 1) { if (fl) printf("%7u", tl); if (fw) printf("%7u", tw); if (fc) printf("%7u", tc); printf(" total\n"); }
    return rc;
}
