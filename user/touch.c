#include "libz.h"
/* touch [-c] file... : create files, or set their modification time to now */
int main(int argc, char **argv)
{
    int no_create = 0, i = 1, rc = 0;
    if (i < argc && !strcmp(argv[i], "-c")) { no_create = 1; i++; }
    for (; i < argc; i++) {
        struct ustat st;
        if (sys_stat(argv[i], &st) == 0) { sys_utimes(argv[i], sys_time()); continue; }
        if (no_create) continue;
        int fd = sys_open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) { fprintf(2, "touch: %s: cannot create\n", argv[i]); rc = 1; } else { sys_close(fd); sys_utimes(argv[i], sys_time()); }
    }
    return rc;
}
