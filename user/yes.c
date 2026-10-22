#include "libz.h"
int main(int argc, char **argv)
{
    char line[256]; u32 n = 0;
    const char *s = argc > 1 ? argv[1] : "y";
    while (*s && n < 250) line[n++] = *s++;
    line[n++] = '\n';
    for (;;) if (sys_write(1, line, n) < 0) return 141;       /* the reader is gone */
}
