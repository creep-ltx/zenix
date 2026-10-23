/* SPDX-License-Identifier: GPL-2.0-only */
/* The disk server (user/diskd.c) on the host, over an image file (HOSTDISK) as the whole disk "ide0", driven by
 * commands on stdin, one per line (tools/disk-host-test.sh):
 *   list                    DISK_LIST: every device with its handle (slot), start, sectors, mounted pid
 *   open NAME plain|raw|mount PID     DISK_OPEN from process PID
 *   write H LBA PID | read H LBA      one sector through handle H
 *   unmount H PID | rescan | flush H
 *   dead PID                that process no longer exists (sys_procinfo stops listing it)
 *   as UID                  later requests come from this user id (0 = root, the default)
 *   sh COMMAND              run a host command (to change the image between steps)
 * Every command prints one line "COMMAND LINE -> RESULT". */
#include "zhost.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int fd = -1;
static u32 nsec;
static u32 deadpid[16]; static int ndead;
static int flush_result;
/* the disk is read as the 68k sees it: longs are big-endian (the RDB code reads longs) */
static void swap(void *buf, u32 n) { u32 *w = buf; for (u32 i = 0; i < n * 128; i++) w[i] = __builtin_bswap32(w[i]); }
int sys_blk_read(u32 lba, u32 n, void *buf)
{
    if (lba + n > nsec || pread(fd, buf, n * 512, (off_t)lba * 512) != (ssize_t)n * 512) return -1;
    swap(buf, n);
    return 0;
}
int sys_blk_write(u32 lba, u32 n, const void *buf)
{
    static u8 t[512 * 64];
    if (lba + n > nsec || n > 64) return -1;
    memcpy(t, buf, n * 512); swap(t, n);
    return pwrite(fd, t, n * 512, (off_t)lba * 512) != (ssize_t)n * 512 ? -1 : 0;
}
int sys_blk_size(void) { return (int)nsec; }
int sys_blk_ram(u32 *r) { r[0] = r[1] = 0; return -1; }
int sys_blk_flush(void) { return flush_result; }
int sys_fd_info(u32 d) { (void)d; return 0; }
int sys_fd_rw(u32 d, u32 lba, u32 n, void *buf, int w) { (void)d; (void)lba; (void)n; (void)buf; (void)w; return -1; }
int sys_procinfo(u32 i, struct uproc *p)                  /* processes 1..40 exist unless declared dead */
{
    if (i >= 40) return -1;
    memset(p, 0, sizeof *p);
    p->pid = i + 1; p->state = 1;
    for (int k = 0; k < ndead; k++) if (deadpid[k] == p->pid) p->state = 0;
    return 0;
}

#define main diskd_main
#include "../../user/diskd.c"
#undef main

static unsigned cur_uid;
static int req(u32 type, u32 a0, u32 a1, u32 a2, u32 a3, int from)
{
    struct umsg m;
    memset(&m, 0, sizeof m);
    m.type = type; m.a[0] = a0; m.a[1] = a1; m.a[2] = a2; m.a[3] = a3; m.from = from; m.uid = (unsigned short)cur_uid;
    return handle(&m);
}

int main(void)
{
    const char *p = getenv("HOSTDISK");
    if (!p || (fd = open(p, O_RDWR)) < 0) { fprintf(stderr, "set HOSTDISK\n"); return 9; }
    struct stat st; fstat(fd, &st); nsec = (u32)(st.st_size / 512);
    if (getenv("NOFLUSH")) flush_result = -95;
    disk_sectors = nsec;
    disk_caps = sys_blk_flush() == 0 ? DISKCAP_FLUSH : 0;
    scan();
    char line[512];
    static u8 sec[512];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        char whole[512];
        strcpy(whole, line);
        if (!strncmp(line, "sh ", 3)) { int r = system(line + 3); printf("sh -> %d\n", r); fflush(stdout); continue; }
        char *cmd = strtok(line, " "), *a = strtok(0, " "), *b = strtok(0, " "), *c = strtok(0, " ");
        if (!cmd || cmd[0] == '#') continue;
        int r;
        if (!strcmp(cmd, "list")) {
            struct diskinfo d;
            for (u32 i = 0; req(DISK_LIST, i, (u32)&d, 0, 0, 1) == 0; i++)
                printf("list -> %s h%d start %u sectors %u type %08x mounted %u caps %u\n", d.name, find(d.name), d.start, d.sectors, d.dostype, d.mounted, d.caps);
            continue;
        }
        if (!strcmp(cmd, "open")) r = req(DISK_OPEN, (u32)a, !strcmp(b, "raw") ? DISK_RAW : !strcmp(b, "mount") ? DISK_MOUNT : 0, 0, 0, atoi(c));
        else if (!strcmp(cmd, "write")) { memset(sec, 0x5A, 512); r = req(DISK_WRITE, (u32)atoi(a), (u32)atoi(b), 1, (u32)sec, atoi(c)); }
        else if (!strcmp(cmd, "read")) r = req(DISK_READ, (u32)atoi(a), (u32)atoi(b), 1, (u32)sec, 1);
        else if (!strcmp(cmd, "unmount")) r = req(DISK_UNMOUNT, (u32)atoi(a), 0, 0, 0, atoi(b));
        else if (!strcmp(cmd, "rescan")) r = req(DISK_RESCAN, 0, 0, 0, 0, 1);
        else if (!strcmp(cmd, "flush")) r = req(DISK_FLUSH, (u32)atoi(a), 0, 0, 0, 1);
        else if (!strcmp(cmd, "as")) { cur_uid = (unsigned)atoi(a); r = 0; }
        else if (!strcmp(cmd, "dead")) { if (ndead < 16) deadpid[ndead++] = (u32)atoi(a); r = 0; }
        else { printf("%s -> unknown\n", cmd); continue; }
        printf("%s -> %d\n", whole, r);
        fflush(stdout);
    }
    return 0;
}
