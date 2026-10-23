/* SPDX-License-Identifier: GPL-2.0-only */
/* The disk server's messages (kernel/diskproto.h) answered from an image file, for host builds of the Zenix
 * filesystem code. One device: the whole image (or the part given by HOSTDISK_START/HOSTDISK_SECTORS, in sectors).
 * The image comes from the environment variable HOSTDISK or from hostdisk_use(). Counts every write so a test can
 * check that a read-only mount wrote nothing. */
#include "zhost.h"
#include "../../kernel/diskproto.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int fd = -1;
static unsigned long long start_sec;
static u32 nsec;
unsigned long hostdisk_writes;                  /* sectors written so far */
unsigned long hostdisk_limit = ~0ul;            /* fail writes after this many sectors (a disk that dies) */

int hostdisk_use(const char *path, u32 start, u32 sectors)
{
    if (fd >= 0) close(fd);
    fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    struct stat st;
    fstat(fd, &st);
    start_sec = start;
    nsec = sectors ? sectors : (u32)(st.st_size / 512 - start);
    return 0;
}

static void lazy_open(void)
{
    if (fd >= 0) return;
    const char *p = getenv("HOSTDISK");
    const char *s = getenv("HOSTDISK_START"), *n = getenv("HOSTDISK_SECTORS");
    if (!p || hostdisk_use(p, s ? (u32)atoi(s) : 0, n ? (u32)atoi(n) : 0)) {
        fprintf(stderr, "hostdisk: set HOSTDISK to an image file\n");
        exit(9);
    }
}

int sys_call(int port, struct umsg *m)
{
    (void)port;
    lazy_open();
    switch (m->type) {
    case DISK_OPEN: return 0;
    case DISK_UNMOUNT: case DISK_FLUSH: case DISK_RESCAN: return 0;
    case DISK_INFO: case DISK_LIST: {
        if (m->type == DISK_LIST && m->a[0] != 0) return -1;
        struct diskinfo *d = (struct diskinfo *)m->a[1];
        memset(d, 0, sizeof *d);
        strcpy(d->name, "hd0");
        d->sectors = nsec;
        d->maxsect = 256;
        return 0;
    }
    case DISK_READ: case DISK_WRITE: {
        u32 lba = m->a[1], n = m->a[2];
        if (n == 0 || lba + n < lba || lba + n > nsec) return -1;
        off_t off = (off_t)((start_sec + lba) * 512);
        if (m->type == DISK_READ)
            return pread(fd, (void *)m->a[3], (size_t)n * 512, off) == (ssize_t)n * 512 ? 0 : -1;
        if (hostdisk_writes + n > hostdisk_limit) return -1;
        hostdisk_writes += n;
        return pwrite(fd, (const void *)m->a[3], (size_t)n * 512, off) == (ssize_t)n * 512 ? 0 : -1;
    }
    }
    return -1;
}

int sys_port_find(const char *name) { (void)name; return 1; }
int sys_port_create(const char *name) { (void)name; return 2; }
struct umsg *sys_recv(int port, int wait) { (void)port; (void)wait; return (struct umsg *)-1; }
struct umsg *sys_replyrecv(struct umsg *m, int r, int port) { (void)m; (void)r; (void)port; return (struct umsg *)-1; }
int sys_reply(struct umsg *m, int r) { (void)m; (void)r; return 0; }
void sys_sleep(u32 t) { (void)t; }
static u32 host_ticks;
u32 sys_ticks(void) { return host_ticks += 1; }
long sys_time(void) { return 1761190000; }
void *xmalloc(u32 n) { void *p = malloc(n); if (!p) { fprintf(stderr, "out of memory\n"); exit(1); } return p; }
