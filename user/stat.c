#include "libz.h"
/* stat file...: inode, mode, links, size, time */
int main(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct ustat st;
        if (sys_stat(argv[i], &st) < 0) { fprintf(2, "stat: %s: not found\n", argv[i]); rc = 1; continue; }
        const char *t = (st.mode & S_IFMT) == S_IFDIR ? "directory" : (st.mode & S_IFMT) == S_IFLNK ? "symbolic link" : "regular file";
        printf("  File: %s\n  Size: %u  Type: %s\n Inode: %u  Links: %u  Mode: %04o  Uid: %u  Gid: %u\nModify: %u (seconds since the filesystem's epoch)\n",
               argv[i], st.size, t, st.ino, st.links, st.mode & 07777, st.uid, st.gid, st.mtime);
    }
    return rc;
}
