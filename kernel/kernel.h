#pragma once
#include <stdint.h>
#include <stdarg.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

/* bootinfo v3, written by the bootblock at 0x1000 */
#define MEMF_CHIP 2u
#define MEMF_FAST 4u
struct bootregion { u32 attrs, lower, upper; };
struct bootinfo {
    u16 attn;                   /* exec AttnFlags: bit1 68020, bit2 68030, ... */
    u16 nregions;
    u32 kernel_pa;
    u32 kernel_size;
    u32 font;                   /* unused for now */
    struct bootregion r[8];
};

/* serial.c */
void serial_init(void);
void serial_putc(char c);
void serial_flush(void);
int serial_tx_pending(void);
void serial_tx_drain(void);
void serial_rx_isr(void);
void irq5_c(void);
void serial_puts(const char *s);
void kprintf(const char *fmt, ...);

/* main.c */
void panic(const char *why) __attribute__((noreturn));
void *memset(void *d, int c, unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n);
extern volatile u32 ticks;      /* 100 Hz, CIA-A timer A */

/* start.S */
void set_vbr(void *table);
u16 irq_save(void);
u16 irq_save2(void);
void irq_restore(u16 sr);
void ints_on(void);
void ints_off(void);
void cpu_idle(void);
void cache_enable(void);
void exc_common(void);
void irq2_entry(void);

/* mem.c */
#define MEM_CHIP     1u         /* must be chip RAM (DMA: graphics, audio, disk) */
#define MEM_FASTONLY 2u         /* fast RAM only, fail rather than use chip */
void mem_init(const struct bootinfo *bi, u32 kernel_pa, u32 kernel_mem);
void *mem_alloc(u32 n, u32 flags, u32 owner);
void mem_free(void *p);
u32 mem_free_owner(u32 owner);
void mem_stats(u32 *chip_free, u32 *fast_free);
void *kmalloc(u32 n);
void kfree(void *p);

/* ide.c */
int ide_init(void);
int ide_read(u32 lba, u32 count, void *buf);
int ide_write(u32 lba, u32 count, const void *buf);

/* ext2.c: read-only for now */
#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
struct inode {
    u32 ino, size, mtime, block[15];
    u16 mode, uid, gid, links;
};
struct udirent {                /* returned by SYS_readdir */
    u32 ino;
    u32 type;
    char name[64];
};
struct ustat {                  /* returned by SYS_stat / SYS_fstat */
    u32 ino, mode, size, mtime;
    u32 uid, gid, links;
};
int ext2_mount(void);
int ext2_iget(u32 ino, struct inode *out);
u32 ext2_bmap(const struct inode *in, u32 n);
int ext2_read(const struct inode *in, u32 off, void *dst, u32 n);
int ext2_readdir(const struct inode *dir, u32 *cookie, struct udirent *out);
int ext2_lookup(const struct inode *dir, const char *name, u32 len, u32 *ino);
int ext2_namei(const char *path, u32 cwd, struct inode *out, int depth);

/* ZEXE header, see tools/mkzexe.py */
struct zexe {
    u16 bra, bra_disp;
    u32 magic;                  /* 'ZEXE' */
    u32 image_size, mem_size, nreloc, reloc_off, stack_size, reserved;
};
#define ZEXE_MAGIC 0x5a455845u

/* files, pipes, tty (vfs.c, tty.c) */
#define NFD   16
#define NFILE 64
enum { FT_NONE = 0, FT_INODE, FT_TTY, FT_PIPE_R, FT_PIPE_W };
struct pipe {
    u8 buf[2048];
    u16 rd, wr, count;
    u8 readers, writers;
};
struct file {
    u8 type, refs;
    struct pipe *pipe;
    u32 off;
    struct inode in;
};
struct uproc {                  /* returned by SYS_procinfo */
    u32 pid, ppid, state, mem;
    char name[16];
};

/* proc.c */
#define NPROC 16
#define SLICE 5                 /* ticks (100 Hz) */
#define NREG  8                 /* extra memory regions per process */
enum pstate { P_FREE = 0, P_READY, P_SLEEP, P_WAIT, P_IO, P_ZOMBIE };
struct proc {                   /* start.S relies on the first three: */
    u32 *ksp;                   /*   offset 0 */
    u32 usp;                    /*   offset 4 */
    u32 entry;                  /*   offset 8 */
    int state;
    int pid, ppid;
    int exit_code;
    int wait_for;               /* P_WAIT: child pid, -1 = any */
    u32 wake;                   /* P_SLEEP: tick to wake at */
    u32 base, span;             /* the process's own memory (image+bss+stack) */
    struct { u32 base, size; } reg[NREG];   /* memalloc'd blocks */
    u32 cwd_ino;
    char cwd[96];
    struct file *fd[NFD];
    int killed;
    char name[16];
};
extern struct proc procs[NPROC];
extern struct proc *current;
void proc_init(void);
int proc_spawn(const char *path, char *const *argv, int argc,
               struct proc *parent, const int *stdio);
void schedule(void);
void proc_exit(int code) __attribute__((noreturn));
int proc_wait(int pid);
void proc_reap_orphans(void);
void sched_tick(u16 sr);
int user_ok(const struct proc *p, u32 ptr, u32 len);
void wake_io(void);
void block_io(void);
void proc_kill(struct proc *p);
struct proc *proc_by_pid(int pid);

/* vfs.c */
struct file *file_new(int type);
struct file *file_dup(struct file *f);
void file_close(struct file *f);
int fd_install(struct proc *p, struct file *f);
long vfs_read(struct file *f, void *buf, u32 n);
long vfs_write(struct file *f, const void *buf, u32 n);
long vfs_open(const char *path, int flags);
long vfs_readdir(struct file *f, struct udirent *out);
void vfs_stat_inode(const struct inode *in, struct ustat *st);
long vfs_pipe(int fds[2]);
void vfs_chdir_text(char *cwd, const char *path);

/* tty.c */
void tty_init(void);
void tty_input(u8 c);
long tty_read(void *buf, u32 n);
extern int tty_fg;

/* syscalls: TRAP #0, number in d0, args d1-d3, result in d0 */
#define SYS_exit    1
#define SYS_write   2
#define SYS_yield   3
#define SYS_getpid  4
#define SYS_spawn   5
#define SYS_wait    6
#define SYS_sleep   7
#define SYS_open    8
#define SYS_close   9
#define SYS_read    10
#define SYS_lseek   11
#define SYS_readdir 12
#define SYS_stat    13
#define SYS_fstat   14
#define SYS_chdir   15
#define SYS_getcwd  16
#define SYS_pipe    17
#define SYS_memalloc 18
#define SYS_memfree  19
#define SYS_ticks   20
#define SYS_procinfo 21
#define SYS_meminfo 22
#define SYS_setfg   23
#define SYS_kill    24
void syscall_c(u32 *rb);
void trap0_entry(void);
void irq5_entry(void);
void switch_to(struct proc *old, struct proc *next);
void user_first_entry(void);
