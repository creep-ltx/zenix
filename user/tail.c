/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* tail [-n N | -N | -n +N] [-c N] [file...]: the last lines (default 10) or bytes */
int main(int argc, char **argv)
{
    long nl = 10, nc = -1; int from_start = 0, i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) { i++; if (argv[i][0] == '+') { from_start = 1; nl = atoi(argv[i] + 1); } else nl = atoi(argv[i]); }
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) nc = atoi(argv[++i]);
        else if (isdigit((unsigned char)argv[i][1])) nl = atoi(argv[i] + 1);
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { fprintf(2, "tail: unknown option %s\n", argv[i]); return 2; }
    }
    int nfiles = argc - i;
    for (int f = 0; f < (nfiles ? nfiles : 1); f++) {
        int fd = 0; const char *name = "standard input";
        if (nfiles) { name = argv[i + f]; fd = sys_open(name, O_RDONLY); if (fd < 0) { fprintf(2, "tail: %s: %s\n", name, zwhy("cannot open")); rc = 1; continue; } }
        if (nfiles > 1) oprintf("%s==> %s <==\n", f ? "\n" : "", name);
        struct rd r; rd_init(&r, fd);
        if (nc >= 0) {                                    /* the last nc bytes */
            char *ring = xmalloc((u32)nc + 1); long n = 0, head = 0; int c;
            while ((c = rd_getc(&r)) >= 0) { if (nc) { ring[head] = (char)c; head = (head + 1) % nc; if (n < nc) n++; } }
            for (long k = 0; k < n; k++) oputc(ring[(head - n + k + nc * 2) % nc]);
            free(ring);
        } else if (from_start) {
            char *line = 0; u32 cap = 0; long k = 1;
            while (rd_getline(&r, &line, &cap) >= 0) { if (k++ >= nl) { oputs(line); oputc('\n'); } }
            free(line);
        } else {
            char **ring = xmalloc((u32)(nl ? nl : 1) * sizeof(char *)); long n = 0, head = 0;
            for (long k = 0; k < nl; k++) ring[k] = 0;
            char *line = 0; u32 cap = 0; int len;
            while ((len = rd_getline(&r, &line, &cap)) >= 0) {
                if (!nl) continue;
                if (ring[head]) free(ring[head]);
                if (!(ring[head] = strdup(line))) { fprintf(2, "tail: out of memory\n"); _zexit(1); }
                head = (head + 1) % nl; if (n < nl) n++;
            }
            for (long k = 0; k < n; k++) { char *s = ring[(head - n + k + nl * 2) % nl]; oputs(s); oputc('\n'); }
            for (long k = 0; k < nl; k++) free(ring[k]);
            free(ring); free(line);
        }
        if (nfiles) sys_close(fd);
    }
    return rc;
}
