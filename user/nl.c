// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
static const char USAGE[] = "usage: nl [-b a|t|n] [-n ln|rn|rz] [-w width] [-s separator]\n       [-v first] [-i step] [file]\n";
/* nl [-b a|t|n] [-n ln|rn|rz] [-w width] [-s separator] [-v first] [-i step] [file]: number the lines */
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    char body = 't'; const char *fmt = "rn", *sep = "\t"; int width = 6, i = 1; long n = 1, step = 1;
    for (; zisopt(argc, argv, &i, USAGE); i++) {
        const char *o = argv[i];
        const char *val = o[2] ? o + 2 : (i + 1 < argc ? argv[++i] : "");
        if (o[1] == 'b') body = val[0]; else if (o[1] == 'n') fmt = val; else if (o[1] == 'w') width = atoi(val);
        else if (o[1] == 's') sep = val; else if (o[1] == 'v') n = atoi(val); else if (o[1] == 'i') step = atoi(val);
        else zbadopt(o, USAGE);
    }
    int fd = 0;
    if (i < argc) { fd = sys_open(argv[i], O_RDONLY); if (fd < 0) { dprintf(2, "nl: %s: %s\n", argv[i], zwhy("cannot open")); return 1; } }
    struct rd r; rd_init(&r, fd);
    char *line = 0; u32 cap = 0; int len;
    while ((len = rd_getline(&r, &line, &cap)) >= 0) {
        int number = body == 'a' || (body == 't' && len > 0);
        if (number) {
            if (!strcmp(fmt, "ln")) oprintf("%-*ld", width, n); else if (!strcmp(fmt, "rz")) oprintf("%0*ld", width, n); else oprintf("%*ld", width, n);
            oputs(sep); n += step;
        } else { for (int k = 0; k < width; k++) oputc(' '); for (const char *s = sep; *s; s++) oputc(' '); }
        oputs(line); oputc('\n');
    }
    return 0;
}
