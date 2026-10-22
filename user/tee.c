#include "libz.h"
/* tee [-a] [file...]: copy standard input to standard output and the files */
int main(int argc, char **argv)
{
    int app = 0, i = 1, fds[8], nf = 0;
    if (i < argc && !strcmp(argv[i], "-a")) { app = 1; i++; }
    for (; i < argc && nf < 8; i++) { int fd = sys_open(argv[i], O_WRONLY | O_CREAT | (app ? O_APPEND : O_TRUNC)); if (fd < 0) fprintf(2, "tee: %s: cannot write\n", argv[i]); else fds[nf++] = fd; }
    char b[512]; int n;
    while ((n = sys_read(0, b, sizeof b)) > 0) { sys_write(1, b, (u32)n); for (int k = 0; k < nf; k++) sys_write(fds[k], b, (u32)n); }
    for (int k = 0; k < nf; k++) sys_close(fds[k]);
    return 0;
}
