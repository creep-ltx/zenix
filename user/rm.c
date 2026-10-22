#include "libz.h"
int main(int argc, char **argv)
{
    int rc = 0;
    if (argc < 2) { fprintf(2, "usage: rm file...\n"); return 2; }
    for (int i = 1; i < argc; i++)
        if (sys_unlink(argv[i]) < 0) { fprintf(2, "rm: %s: cannot remove\n", argv[i]); rc = 1; }
    return rc;
}
