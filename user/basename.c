#include "libz.h"
/* basename path [suffix] */
int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(2, "usage: basename path [suffix]\n"); return 2; }
    char *p = strdup(argv[1]); u32 l = strlen(p);
    while (l > 1 && p[l - 1] == '/') p[--l] = 0;
    char *b = strrchr(p, '/'); b = (b && l > 1) ? b + 1 : p;
    if (argc > 2) { u32 sl = strlen(argv[2]), bl = strlen(b); if (bl > sl && !strcmp(b + bl - sl, argv[2])) b[bl - sl] = 0; }
    printf("%s\n", b);
    return 0;
}
