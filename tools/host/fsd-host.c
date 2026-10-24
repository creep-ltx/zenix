/* SPDX-License-Identifier: GPL-2.0-only */
/* The ext2 filesystem server (user/fsd.c + kernel/ext2.c) on the host, driven by commands on stdin, one per line:
 *   mount [root]            try_mount of the image (root: it must be writable, as the root picker demands)
 *   open PATH FLAGS         FS_OPEN (flags: r w rw c t a, combined: "wct"); prints the inode number or the error
 *   close INO               FS_CLOSE
 *   write INO OFF TEXT      FS_WRITE ("TEXT" may be @N: N bytes of a pattern)
 *   read INO OFF N          FS_READ, prints what came back
 *   fstat INO | stat PATH | lstat PATH
 *   unlink P | rmdir P | mkdir P | symlink TARGET P | link OLD NEW | rename OLD NEW | readlink P | chmod P MODE
 *   ls PATH                 every entry (FS_READDIR)
 *   sync | quit             FS_SYNC | FS_QUIT
 *   writes                  the sectors written to the image so far (hostdisk_writes)
 *   as UID GID              later requests come from this user
 *   raw on|off              later requests come as a user's own SYS_call (no UMSG_KERNEL), not from the kernel
 *   utimes P T | chown P UID GID     FS_UTIMES (T "now" = FS_UTIME_NOW) | FS_CHOWN
 * open FLAGS also take x (O_EXCL).
 * An inode argument "%N" is the inode number the N-th successful open returned.
 * Every command prints one line "CMD -> RESULT ...". Used by tools/ext2-host-test.sh. */
#define main fsd_main
#include "../../user/fsd.c"
#undef main

static unsigned cur_uid, cur_gid, raw;
static int opened[100], nopened;
static u32 num(const char *s) { return s && s[0] == '%' && atoi(s + 1) > 0 && atoi(s + 1) <= nopened ? (u32)opened[atoi(s + 1) - 1] : (u32)atoi(s ? s : "0"); }

static int req(u32 type, u32 a0, u32 a1, u32 a2, u32 a3)
{
    struct umsg m;
    memset(&m, 0, sizeof m);
    m.type = type; m.a[0] = a0; m.a[1] = a1; m.a[2] = a2; m.a[3] = a3;
    m.uid = (unsigned short)cur_uid; m.gid = (unsigned short)cur_gid; m.umask = 022;
    m.from = 7;
    m.kflags = raw ? 0 : UMSG_KERNEL;
    return handle(&m);
}

static u32 flags_of(const char *s)
{
    u32 f = 0; int r = 0, w = 0;
    for (; *s; s++) {
        if (*s == 'r') r = 1;
        if (*s == 'w') w = 1;
        if (*s == 'c') f |= O_CREAT;
        if (*s == 't') f |= O_TRUNC;
        if (*s == 'a') f |= O_APPEND;
        if (*s == 'x') f |= O_EXCL;
    }
    return f | (r && w ? O_RDWR : w ? O_WRONLY : O_RDONLY);
}

int main(void)
{
    char line[1024];
    disk_port = 1;
    ext2_busy = open_busy;
    static char big[1 << 20];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        char *cmd = strtok(line, " "), *a = strtok(0, " "), *b = strtok(0, " "), *c = strtok(0, "");
        if (!cmd || cmd[0] == '#') continue;
        int r = 0;
        struct ustat st;
        if (!strcmp(cmd, "mount")) {
            r = try_mount("hd0", a && !strcmp(a, "root") ? EXT2_RW : EXT2_ANY);
            printf("mount -> %d%s\n", r, ext2_readonly ? " readonly" : "");
            continue;
        }
        if (!strcmp(cmd, "raw")) { raw = a && !strcmp(a, "on"); printf("raw -> %u\n", raw); continue; }
        if (!strcmp(cmd, "as")) { cur_uid = (unsigned)atoi(a); cur_gid = (unsigned)atoi(b); printf("as -> %u %u\n", cur_uid, cur_gid); continue; }
        if (!strcmp(cmd, "open")) { r = req(FS_OPEN, (u32)a, 2, flags_of(b ? b : "r"), (u32)&st); if (r > 0 && nopened < 100) opened[nopened++] = r; }
        else if (!strcmp(cmd, "close")) r = req(FS_CLOSE, num(a), 0, 0, 0);
        else if (!strcmp(cmd, "write")) {
            const char *t = c ? c : "";
            u32 n = (u32)strlen(t);
            if (t[0] == '@') { n = (u32)atoi(t + 1); if (n > sizeof big) n = sizeof big; for (u32 i = 0; i < n; i++) big[i] = (char)('a' + i % 23); t = big; }
            r = req(FS_WRITE, num(a), (u32)atoi(b), (u32)t, n);
        } else if (!strcmp(cmd, "read")) {
            u32 n = (u32)atoi(c);
            if (n > sizeof big - 1) n = sizeof big - 1;
            r = req(FS_READ, num(a), (u32)atoi(b), (u32)big, n);
            if (r > 0) { big[r] = 0; printf("read -> %d %s\n", r, r <= 200 ? big : "(long)"); continue; }
        } else if (!strcmp(cmd, "fstat") || !strcmp(cmd, "stat") || !strcmp(cmd, "lstat")) {
            r = !strcmp(cmd, "fstat") ? req(FS_FSTAT, num(a), (u32)&st, 0, 0)
                                      : req(!strcmp(cmd, "stat") ? FS_STAT : FS_LSTAT, (u32)a, 2, (u32)&st, 0);
            if (r == 0) { printf("%s -> 0 ino %u mode %o size %u links %u uid %u gid %u mtime %u\n", cmd, st.ino, st.mode, st.size, st.links, st.uid, st.gid, st.mtime); continue; }
        }
        else if (!strcmp(cmd, "unlink")) r = req(FS_UNLINK, (u32)a, 2, 0, 0);
        else if (!strcmp(cmd, "rmdir")) r = req(FS_RMDIR, (u32)a, 2, 0, 0);
        else if (!strcmp(cmd, "mkdir")) r = req(FS_MKDIR, (u32)a, 2, 0, 0);
        else if (!strcmp(cmd, "symlink")) r = req(FS_SYMLINK, (u32)a, (u32)b, 2, 0);
        else if (!strcmp(cmd, "link")) r = req(FS_LINK, (u32)a, (u32)b, 2, 0);
        else if (!strcmp(cmd, "rename")) r = req(FS_RENAME, (u32)a, (u32)b, 2, 0);
        else if (!strcmp(cmd, "chmod")) r = req(FS_CHMOD, (u32)a, 2, (u32)strtol(b, 0, 8), 0);
        else if (!strcmp(cmd, "utimes")) r = req(FS_UTIMES, (u32)a, 2, !strcmp(b, "now") ? FS_UTIME_NOW : (u32)atoi(b), 0);
        else if (!strcmp(cmd, "chown")) { char *g = strtok(c, " "); r = req(FS_CHOWN, (u32)a, 2, (u32)atoi(b), g ? (u32)atoi(g) : 0xFFFFFFFFu); }
        else if (!strcmp(cmd, "readlink")) {
            r = req(FS_READLINK, (u32)a, 2, (u32)big, 255);
            if (r > 0) { big[r] = 0; printf("readlink -> %d %s\n", r, big); continue; }
        } else if (!strcmp(cmd, "ls")) {
            struct ustat ds;
            int ino = req(FS_OPEN, (u32)a, 2, O_RDONLY, (u32)&ds);
            if (ino < 0) { printf("ls -> %d\n", ino); continue; }
            u32 cookie = 0, n = 0;
            struct udirent e;
            printf("ls ->");
            while ((r = req(FS_READDIR, (u32)ino, cookie, (u32)&e, (u32)&cookie)) > 0) { printf(" %s", e.name); n++; }
            req(FS_CLOSE, (u32)ino, 0, 0, 0);
            printf(" (%u, %d)\n", n, r);
            continue;
        }
        else if (!strcmp(cmd, "writes")) { extern unsigned long hostdisk_writes; printf("writes -> %lu\n", hostdisk_writes); continue; }
        else if (!strcmp(cmd, "sync")) r = req(FS_SYNC, 0, 0, 0, 0);
        else if (!strcmp(cmd, "quit")) r = req(FS_QUIT, 0, 0, 0, 0);
        else { printf("%s -> unknown command\n", cmd); continue; }
        printf("%s -> %d\n", cmd, r);
    }
    return 0;
}
