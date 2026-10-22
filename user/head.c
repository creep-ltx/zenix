#include "libz.h"
/* head [-n N | -N] [-c N] [file...]: the first lines (default 10) or bytes */
int main(int argc, char **argv)
{
    long nl = 10, nc = -1; int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) nl = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) nc = atoi(argv[++i]);
        else if (isdigit((unsigned char)argv[i][1])) nl = atoi(argv[i] + 1);
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { fprintf(2, "head: unknown option %s\n", argv[i]); return 2; }
    }
    int nfiles = argc - i;
    for (int f = 0; f < (nfiles ? nfiles : 1); f++) {
        int fd = 0; const char *name = "standard input";
        if (nfiles) { name = argv[i + f]; fd = sys_open(name, O_RDONLY); if (fd < 0) { fprintf(2, "head: %s: cannot open\n", name); rc = 1; continue; } }
        if (nfiles > 1) oprintf("%s==> %s <==\n", f ? "\n" : "", name);
        struct rd r; rd_init(&r, fd);
        if (nc >= 0) { int c; for (long k = 0; k < nc && (c = rd_getc(&r)) >= 0; k++) oputc(c); }
        else {
            char *line = 0; u32 cap = 0;
            for (long k = 0; k < nl && rd_getline(&r, &line, &cap) >= 0; k++) { oputs(line); oputc('\n'); }
            free(line);
        }
        if (nfiles) sys_close(fd);
    }
    return rc;
}
