#pragma once
/* Zenix system calls: TRAP #0, number in d0, args d1-d3, result in d0. */
typedef unsigned int u32;

static inline int sys3(int n, u32 a, u32 b, u32 c)
{
    register int d0 __asm__("d0") = n;
    register u32 d1 __asm__("d1") = a;
    register u32 d2 __asm__("d2") = b;
    register u32 d3 __asm__("d3") = c;
    __asm__ volatile("trap #0" : "+r"(d0) : "r"(d1), "r"(d2), "r"(d3) : "memory", "cc");
    return d0;
}

#define USYS_STRUCTS
struct udirent { u32 ino, type; char name[64]; };
struct ustat { u32 ino, mode, size, mtime, uid, gid, links; };
struct uproc { u32 pid, ppid, state, mem; char name[16]; };

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000

static inline void sys_exit(int c) { sys3(1, (u32)c, 0, 0); for (;;); }
static inline int sys_write(int fd, const void *p, u32 n) { return sys3(2, (u32)fd, (u32)p, n); }
static inline void sys_yield(void) { sys3(3, 0, 0, 0); }
static inline int sys_getpid(void) { return sys3(4, 0, 0, 0); }
static inline int sys_spawn(const char *path, char *const *argv, const int *stdio)
{ return sys3(5, (u32)path, (u32)argv, (u32)stdio); }
static inline int sys_wait(int pid) { return sys3(6, (u32)pid, 0, 0); }
static inline void sys_sleep(u32 ticks) { sys3(7, ticks, 0, 0); }
static inline int sys_open(const char *p, int fl) { return sys3(8, (u32)p, (u32)fl, 0); }
static inline int sys_close(int fd) { return sys3(9, (u32)fd, 0, 0); }
static inline int sys_read(int fd, void *p, u32 n) { return sys3(10, (u32)fd, (u32)p, n); }
static inline int sys_lseek(int fd, int off, int whence) { return sys3(11, (u32)fd, (u32)off, (u32)whence); }
static inline int sys_readdir(int fd, struct udirent *d) { return sys3(12, (u32)fd, (u32)d, 0); }
static inline int sys_stat(const char *p, struct ustat *s) { return sys3(13, (u32)p, (u32)s, 0); }
static inline int sys_fstat(int fd, struct ustat *s) { return sys3(14, (u32)fd, (u32)s, 0); }
static inline int sys_chdir(const char *p) { return sys3(15, (u32)p, 0, 0); }
static inline int sys_getcwd(char *b, u32 n) { return sys3(16, (u32)b, n, 0); }
static inline int sys_pipe(int fds[2]) { return sys3(17, (u32)fds, 0, 0); }
static inline void *sys_memalloc(u32 n) { return (void *)sys3(18, n, 0, 0); }
static inline int sys_memfree(void *p) { return sys3(19, (u32)p, 0, 0); }
static inline u32 sys_ticks(void) { return (u32)sys3(20, 0, 0, 0); }
static inline int sys_procinfo(u32 i, struct uproc *p) { return sys3(21, i, (u32)p, 0); }
static inline int sys_meminfo(u32 out[4]) { return sys3(22, (u32)out, 0, 0); }
static inline int sys_setfg(int pid) { return sys3(23, (u32)pid, 0, 0); }
static inline int sys_kill(int pid) { return sys3(24, (u32)pid, 0, 0); }

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400
static inline int sys_mkdir(const char *p) { return sys3(25, (u32)p, 0, 0); }
static inline int sys_unlink(const char *p) { return sys3(26, (u32)p, 0, 0); }
static inline int sys_rmdir(const char *p) { return sys3(27, (u32)p, 0, 0); }
static inline int sys_rename(const char *a, const char *b) { return sys3(28, (u32)a, (u32)b, 0); }

struct umsg { struct umsg *next; int from, server; u32 state; int result; u32 type; u32 a[4]; u32 client_; };
static inline int sys_port_create(const char *n) { return sys3(29, (u32)n, 0, 0); }
static inline int sys_port_find(const char *n) { return sys3(30, (u32)n, 0, 0); }
static inline int sys_call(int port, struct umsg *m) { return sys3(31, (u32)port, (u32)m, 0); }
static inline struct umsg *sys_recv(int port, int wait) { return (struct umsg *)sys3(32, (u32)port, (u32)wait, 0); }
static inline int sys_reply(struct umsg *m, int result) { return sys3(33, (u32)m, (u32)result, 0); }
static inline int sys_blk_read(u32 lba, u32 n, void *buf) { return sys3(34, lba, n, (u32)buf); }
static inline int sys_blk_write(u32 lba, u32 n, const void *buf) { return sys3(35, lba, n, (u32)buf); }
static inline u32 sys_hrtime(void) { return (u32)sys3(36, 0, 0, 0); }
static inline int sys_ttymirror(int on) { return sys3(37, (u32)on, 0, 0); }
static inline struct umsg *sys_replyrecv(struct umsg *m, int result, int port) { return (struct umsg *)sys3(38, (u32)m, (u32)result, (u32)port); }
static inline int sys_wsreq(void) { return sys3(39, 0, 0, 0); }
static inline int sys_spawn_tty(const char *path, char *const *argv, int tty) { return sys3(40, (u32)path, (u32)argv, (u32)tty); }
static inline int sys_blk_size(void) { return sys3(41, 0, 0, 0); }
static inline int sys_blk_ram(u32 out[2]) { return sys3(42, (u32)out, 0, 0); }
struct ustatfs { unsigned bsize, blocks, bfree, files, ffree; };
static inline int sys_statfs(struct ustatfs *f) { return sys3(43, (u32)f, 0, 0); }
static inline int sys_sync(void) { return sys3(44, 0, 0, 0); }
