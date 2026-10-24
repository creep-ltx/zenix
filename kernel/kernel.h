/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <stdint.h>
#include <stdarg.h>
#include "errno.h"              /* the error numbers, shared with the servers and libz */

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef int8_t s8;

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
    u32 ram_pa, ram_size;       /* ramdisk loaded by the bootblock, 0 if none */
};

/* serial.c */
void serial_init(void);
void serial_putc(char c);
void serial_flush(void);
int serial_tx_pending(void);
void serial_tx_drain(void);
void serial_rx_isr(void);               /* level 5: queues the byte only */
void serial_work(void);                 /* process context: the queued bytes to the tty, the 0x1d test hook */
void irq5_c(void);
void serial_puts(const char *s);
void kprintf(const char *fmt, ...);
u32 klog_read(u32 off, char *buf, u32 n);     /* the kernel log: n characters from position off (0 = the first ever) */
void klog_to_screen(void);                    /* the display is up: show the log so far and mirror new messages */
void klog_mirror_off(void);                   /* boot is finished: kprintf goes to the serial port and the log only */
void klog_mirror_on(void);                    /* a panic: its text goes to the screen too */
/* previous-boot log (audit A12): the log lives in the top KLOG_AREA_SIZE bytes of chip RAM (two slots of a 256-byte
 * header and an 8 KB ring; the bootblock reserves the same size), so the next boot after a warm reset can show how
 * this one ended */
#define KLOG_AREA_SIZE  0x4200
#define KLOG_PREV       0x80000000u           /* SYS_klog offset flag: read the previous boot's log (dmesg -p) */
#define KLOG_END_RUNNING 0u                   /* why a boot ended, kept in the log header for the next boot */
#define KLOG_END_PANIC   1u
void klog_init(const struct bootinfo *bi);    /* first thing in kmain: read the previous log, start this one */
void klog_show_prev(void);                    /* print what klog_init found (and the previous boot's last lines) */
void klog_set_reason(u32 r);
u32 klog_area_base(void);                     /* the chip RAM address of the area, 0 if the log is not kept there */
void term_log(const char *s, u32 n);          /* text onto the visible workspace */

/* main.c */
void panic(const char *why) __attribute__((noreturn));
void *memset(void *d, int c, unsigned long n);
void *memcpy(void *d, const void *s, unsigned long n);
void *memmove(void *d, const void *s, unsigned long n);
extern volatile u32 ticks;
u32 hr_time(void);          /* E-clock ticks (eclock_hz), CIA-B timers A+B; wraps after ~1.7 h; never early */
extern u32 eclock_hz;       /* 709379 (PAL) or 715909 (NTSC), read at boot */
u32 rtc_read(void);
extern int tty_mirror;      /* 100 Hz, CIA-A timer A */

/* start.S */
void set_vbr(void *table);
u16 irq_save(void);
u16 irq_save2(void);
void irq_restore(u16 sr);
void ints_on(void);
void ints_off(void);
void cpu_idle(void);
u32 get_usp(void);                   /* the user stack pointer register (the running process's, in the kernel) */
extern u32 kstack_bottom[];          /* the boot stack (pid 0, idle): 8 KB below kstack_top */
void cache_enable(int is040plus);
void cache_flush_code(void);         /* after writing code that will run: I-cache (and Emu68 JIT) flush */
void exc_common(void);
void irq2_entry(void);
void irq_other_entry(void);         /* vectors 24-31 without a handler of their own */
void irq_other_c(u32 fv);

/* mem.c */
#define MEM_CHIP     1u         /* must be chip RAM (DMA: graphics, audio, disk) */
#define MEM_FASTONLY 2u         /* fast RAM only, fail rather than use chip */
#define MEM_LOW1G    4u         /* fast RAM wholly below 1 GB: what the Pi 4's VideoCore can read (the mailbox) */
void mem_init(const struct bootinfo *bi, u32 kernel_pa, u32 kernel_mem);
extern u32 boot_ram_pa, boot_ram_size;
void *mem_alloc(u32 n, u32 flags, u32 owner);   /* 0 if n is 0, above MEM_ALLOC_MAX, or does not fit */
#define MEM_ALLOC_MAX 0x7FFF0000u       /* no block size near 4 GB: the rounding in mem_alloc cannot wrap */
#define MEMALLOC_MAX  (64u << 20)       /* largest region SYS_memalloc gives a process (64 MB) */
void mem_free(void *p);
u32 mem_free_owner(u32 owner);
void mem_stats(u32 *chip_free, u32 *fast_free);
void *kmalloc(u32 n);
void kfree(void *p);

/* gfx.c, term.c: display and the virtual consoles */
#define COLS  80
#define TROWS_MAX 32                    /* text rows on a PAL display; NTSC shows 25 (trows, measured at boot) */
#define NCONS 10                        /* virtual consoles /dev/tty1 .. /dev/tty10 (Amiga+F1..F10) */
extern int trows;
int gfx_font_init(u32 gfxbase);
int gfx_init(void);
int gfx_up(void);
void gfx_panic(void);            /* panic with the display up: dark red background, white text, the text stays */
void gfx_cell(int row, int col, u8 ch, u8 attr);
void gfx_scroll(void);
void gfx_clear_text(void);
void gfx_vblank(void);
void term_putc(int con, char c);
void term_write(int con, const char *s, u32 n);
void term_scroll_view(int delta);       /* Shift+Up/Down: scrollback */
int term_back(void);
int term_size(void);                    /* rows << 16 | columns */
void con_switch(int con);               /* show console con (1..NCONS); an unused one asks init for a login */
int con_in_use(int con);                /* is a live process's controlling tty this console? */
extern int con_cur;                     /* the console on the screen (keyboard and serial input go to it) */
extern u16 con_want;                    /* bit n: console n was shown with nobody on it (init's WAIT_CONSOLE) */
void kbd_isr(void);                     /* level 2: handshake and queue the raw code only */
void kbd_init(void);                    /* one handshake at start (the keyboard may still be waiting for one) */
/* the reset warning (kbd.c): Ctrl+Amiga+Amiga on a keyboard that sends it. KDAT is held low while init syncs the
 * filesystems (kbd_reset_flush, from proc_wait), then released; the tick lets go after 8 s in any case */
#define KRS_NONE     0
#define KRS_HELD     1                  /* KDAT held low, init not yet at it */
#define KRS_FLUSHING 2
#define KRS_DONE     3                  /* KDAT released: the keyboard resets the machine */
extern volatile u8 kbd_reset_state;
void kbd_reset_flush(void);
extern volatile int disk_frozen;        /* after the reset-warning flush: disk writes are refused (-EIO) */
void kbd_tick(void);                    /* from the 100 Hz tick: asks for kbd_work while a key repeats */
void kbd_work(void);                    /* process context: keymap, console keys, repeat, tty */
/* Deferred input work (audit B4, main.c): interrupts only queue raw input and set kwork_pending; kwork() does the
 * rest - tty line discipline, echo, Ctrl-C/Ctrl-Z, console switches, scrollback - in process context:
 * from the idle loop, at the end of every system call, when the tick or keyboard interrupt arrives while user
 * code runs (interrupts enabled again first), and between the pieces of a long terminal write. Never from inside
 * kernel code that an interrupt cut into, and never re-entered, so it needs no locks: the kernel is not preempted. */
extern volatile int kwork_pending;
void kwork(void);
void irq3_entry(void);
void irq3_c(void);

/* ide.c */
int ide_init(void);
u32 ide_sectors(void);
int ide_read(u32 lba, u32 count, void *buf);
int ide_write(u32 lba, u32 count, const void *buf);
int ide_flush(void);                     /* ATA FLUSH CACHE: the drive's write cache is on the platter */
void ide_stray_irq(void);                /* level 2 without a CIA cause: clear Gayle's IDE interrupt latch */

/* blk.c: the one raw disk (IDE or SD card) behind the disk server */
int blk_init(void);
u32 blk_sectors(void);
int blk_read(u32 lba, u32 count, void *buf);
int blk_write(u32 lba, u32 count, const void *buf);
int blk_flush(void);

/* floppy.c: Amiga floppy drives df0..df3 (880 KB, MFM), polled; see the file header */
int fd_info(u32 drive);                  /* bit 0 drive, bit 1 disk, bit 2 write protected; sectors << 8 */
int fd_rw(u32 drive, u32 lba, u32 count, void *buf, int write);
void fd_tick(void);                     /* from the 100 Hz tick: motors off when idle */

/* input.c: mouse (port 1) and joystick (port 2), polled from the 100 Hz tick */
void input_init(void);
void input_poll(void);
void input_read(s32 *out);

/* audio.c: Paula beep (the terminal bell) */
void beep(u32 hz, u32 ms);
void audio_tick(void);                  /* from the 100 Hz tick */

/* a3000.c: the Amiga 3000's on-board SCSI (WD33C93A + DMAC) */
int a3000_init(void);
u32 a3000_sectors(void);
int a3000_read(u32 lba, u32 count, void *buf);
int a3000_write(u32 lba, u32 count, const void *buf);
int a3000_flush(void);                   /* SYNCHRONIZE CACHE; -95 if the drive cannot */

/* emmc.c: the SD card of a Raspberry Pi 4 / CM4 under Emu68 (needs the hwinfo table from stage0) */
int emmc_init(void);
u32 emmc_sectors(void);
int emmc_read(u32 lba, u32 count, void *buf);
int emmc_write(u32 lba, u32 count, const void *buf);
int emmc_flush(void);

#include "ext2.h"

/* ZEXE header, see tools/mkzexe.py */
struct zexe {
    u16 bra, bra_disp;
    u32 magic;                  /* 'ZEXE' */
    u32 image_size, mem_size, nreloc, reloc_off, stack_size, reserved;
};
#define ZEXE_MAGIC 0x5a455845u
#define ZEXE_MEM_MAX   (16u << 20)     /* the loader refuses a program asking for more (image + bss) */
#define ZEXE_STACK_MIN 4096u            /* a stack size in the header, if not 0, lies in this range */
#define ZEXE_STACK_MAX (1u << 20)

/* files, pipes, tty (vfs.c, tty.c) */
#define NFD   16
#define NFILE 64
enum { FT_NONE = 0, FT_INODE, FT_TTY, FT_PIPE_R, FT_PIPE_W, FT_NULL, FT_ZERO };
struct pipe {
    u8 buf[2048];
    u16 rd, wr, count;
    u8 readers, writers;
};
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400
struct file {
    u8 type;
    u16 refs;                   /* fds and processes sharing it: up to NPROC * NFD = 512, so not a byte */
    u16 flags;
    struct pipe *pipe;
    u32 off;                    /* position (FT_TTY: the tty number) */
    u32 ino, mode;              /* FT_INODE: inode number and mode at open */
    u8 fs;                      /* FT_INODE: which mount (vfs.c) the inode belongs to */
};
struct umountreq { const char *path, *port, *dev, *type; };   /* SYS_mount: a filesystem server's port goes on a directory */
struct umountinfo { char path[48], port[16], dev[16], type[8]; };   /* SYS_mountinfo */
struct uproc {                  /* returned by SYS_procinfo */
    u32 pid, ppid, state, mem;
    char name[16];
    s32 nice;                   /* the base priority on the nice scale, -20 (most favoured) .. 19 */
    u32 pri;                    /* the effective level now, 0 (highest) .. 31; PRI_IDLE for pid 0 */
};

/* proc.c */
#define NPROC 32                /* 10 consoles with a login and a shell each, the servers, init and the commands they run */
#define SLICE 5                 /* ticks (100 Hz) */
/* Priorities (DESIGN D10, architecture.md section 6): NLEVEL levels inside the kernel, 0 the highest; users see the
 * nice scale -20 (most favoured) .. 19, level = (nice + 20) * 4 / 5, so nice 0 is level 16. Every process has a base
 * (its nice) and an effective level: the base's level, or better while it serves a better client (priority
 * inheritance, ipc.c). The highest non-empty level runs; round robin within a level. */
#define NLEVEL      32
#define NICE_MIN    (-20)
#define NICE_MAX    19
#define NICE_SERVER (-10)               /* diskd, fsd, zfsd: above normal programs (level 8) */
#define PRI_IDLE    NLEVEL              /* pid 0 runs below every level */
#define NICE_LEVEL(n) ((u8)((((n) - NICE_MIN) * 4) / 5))
#define PRIO_PROCESS 0                  /* SYS_getpriority / SYS_setpriority: who is a pid (0: the caller) */
#define PRIO_PGRP    1                  /* not supported (no process groups): -EINVAL */
#define PRIO_USER    2                  /* who is a user id (0: the caller's real user id) */
#define NREG  8                 /* extra memory regions per process */
#define ENVMAX 1024
enum pstate { P_FREE = 0, P_READY, P_SLEEP, P_WAIT, P_IO, P_ZOMBIE, P_MSG, P_STOP };
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
    u32 kstk, ktop, ustk;       /* stack guard (proc.c): bottom and top of the kernel stack, bottom of the user stack
                                 * (0: none, pid 0); a guard pattern lies at each bottom */
    struct { u32 base, size; } reg[NREG];   /* memalloc'd blocks */
    u32 cwd_ino;
    char cwd[160];              /* the working directory as text (FS_PATHMAX, fsproto.h): a longer one is refused */
    struct file *fd[NFD];
    int killed;
    u8 stop_req, stop_rep;      /* stop asked for (acted on when the process next runs); the parent has been told it is stopped */
    int priv;                   /* may use the raw disk (the filesystem server) */
    u16 uid, ruid, gid, umask;  /* effective and real user id, group id (inherited; a set-user-id program changes only uid); the filesystem server enforces the permissions */
    char env[ENVMAX];           /* environment: NAME=value NUL ... NUL NUL; inherited by children */
    u16 envlen;
    struct proc *rnext;         /* run queue link */
    u8 queued;                  /* on the run queue (of level qlevel) */
    u8 qlevel;
    s8 nice;                    /* base priority, NICE_MIN .. NICE_MAX: inherited, lowered by anyone, raised by root */
    u8 blevel;                  /* NICE_LEVEL(nice), kept so that no path divides */
    u8 prio;                    /* effective level (0 highest): blevel, or a better client's while serving it */
    struct umsg *wait_msg;      /* P_MSG as a client: the request in flight */
    u8 ctty;                    /* controlling tty (console 1..NCONS): the console of fd 0 at spawn, else the parent's */
    u8 wait_con;                /* P_WAIT with WAIT_CONSOLE: a console switch wakes it too */
    int wait_port;              /* P_MSG: as a server the port being listened on, as a client the port called */
    char name[16];
};
extern struct proc procs[NPROC];
extern struct proc *current;
void proc_init(void);
int proc_spawn(const char *path, char *const *argv, int argc,
               struct proc *parent, const int *stdio, int tty);
int proc_spawn_image(const void *img, u32 len, const char *name, int tty, int priv);
void schedule(void);
void proc_exit(int code) __attribute__((noreturn));
#define WAIT_NOHANG   1       /* proc_wait flag: -2 if children exist but none has exited */
#define WAIT_UNTRACED 2       /* proc_wait flag: also report a child that has stopped, once (as ST_STOPPED) */
#define ST_STOPPED    0x114   /* proc_wait result for a stopped child (0x100 + 20, the SIGTSTP number) */
#define WAIT_CONSOLE  4       /* proc_wait flag, root only (init): also return ST_CONSOLE + n when console n was shown
                               * with nobody on it (it wants a login) */
#define ST_CONSOLE    0x200   /* ST_CONSOLE + n: see WAIT_CONSOLE */
int proc_wait(int pid, int flags);
void proc_stop(struct proc *p);       /* Ctrl-Z / jobctl: stop it when it next runs */
void proc_cont(struct proc *p);
void proc_reap_orphans(void);
void sched_tick(u16 sr, int tick);   /* the level-2 interrupt: counts the slice; switches if it cut into user code and one is wanted */
extern volatile u8 need_resched;     /* a process better than the running one is ready (or a sleeper is due) */
extern volatile u32 wake_min;        /* no sleeper is due before this tick (while nsleep) */
void resched(void);                  /* act on need_resched: switch only to a strictly better level */
void sleep_until(u32 tick);          /* current to P_SLEEP until tick (the caller then schedules) */
int proc_renice(struct proc *p, int nice);   /* set the base priority (clamped); the effective level follows */
void prio_raise(struct proc *p, u8 level);   /* inheritance: p serves a client at level: at least that until done */
void prio_recalc(struct proc *p);            /* p finished serving: back to its base or its best waiting client */
u8 ipc_best_client(int pid);                 /* ipc.c: the best level among clients waiting for or served by pid */
int user_ok(const struct proc *p, u32 ptr, u32 len);
/* Stack guard (proc.c): a pattern at the bottom of every kernel and user stack, checked with the stack pointers at
 * every task switch, at every system call, at the tick when it interrupts user code and when a program faults.
 * stack_bad: 0, STK_KERNEL or STK_USER (ksp/usp: the stack pointers now; usp 0 = not known, only the guard).
 * stack_fail: says "pid N (name) overflowed its kernel/user stack" and kills it (exit 139), or panics for pid 0. */
#define STK_KERNEL 1
#define STK_USER   2
int stack_bad(const struct proc *p, u32 ksp, u32 usp);
void stack_fail(struct proc *p, int which);
#define STACK_GUARD0 0x5A53544Bu        /* the guard's first word ("ZSTK"), the one nearest the stack */
/* the cheap form for every system call and tick: the user stack pointer in bounds and the guard's first word intact;
 * the full check (all guard words, both stacks) runs at every task switch, and when this one fails */
static inline int stack_user_suspect(const struct proc *p, u32 usp)
{
    return p->ustk && (usp < p->ustk + 16 || usp > p->base + p->span || *(const u32 *)(unsigned long)p->ustk != STACK_GUARD0);
}
void wake_io(void);
void ready(struct proc *p);         /* make a blocked process runnable */
extern int nsleep;
void block_io(void);
void proc_kill(struct proc *p);
struct proc *proc_by_pid(int pid);

/* ipc.c: ports and messages. A message lives in the sender's memory; only
 * pointers move (no MMU, so the server reads and writes the client's
 * buffers directly). The sender is blocked until the server replies. */
#define NPORT 16
#define MSG_QUEUED  1
#define MSG_SERVING 2
#define MSG_DONE    3
struct umsg {
    struct umsg *next;          /* kernel: queue link */
    int from, server;           /* kernel: client pid, serving pid */
    u32 state;                  /* kernel: MSG_* */
    s32 result;                 /* set by the server's reply */
    u32 type;
    u32 a[4];                   /* arguments, free use */
    u16 uid, gid, umask;        /* kernel: the caller's identity, for the filesystem server */
    u16 kflags;                 /* kernel: UMSG_KERNEL if the kernel sent it for a system call (ipc_kcall), else 0 */
    struct proc *client;        /* kernel: who is waiting for the reply */
};
int port_create(const char *name, int owner);
int port_find(const char *name);
struct proc *port_owner(int port);             /* the process that owns a port, 0 if none */
long ipc_call(int port, struct umsg *m);       /* SYS_call: kflags 0 */
long ipc_kcall(int port, struct umsg *m);      /* the kernel's own requests (vfs.c): kflags UMSG_KERNEL */
#define UMSG_KERNEL 1
struct umsg *ipc_recv(int port, int wait);
struct umsg *ipc_replyrecv(struct umsg *m, s32 result, int port);
long ipc_reply(struct umsg *m, s32 result);
void ipc_proc_exit(int pid);

/* vfs.c */
struct file *file_new(int type);
struct file *file_dup(struct file *f);
void file_close(struct file *f);
int fd_install(struct proc *p, struct file *f);
long vfs_read(struct file *f, void *buf, u32 n);
long vfs_write(struct file *f, const void *buf, u32 n);
long fs_call(u32 type, u32 a0, u32 a1, u32 a2, u32 a3);
struct ustat;
struct exec_src { int m; long ino; struct ustat st; };
long fs_exec_open(const char *path, const struct proc *parent, struct exec_src *x);
long fs_exec_read(const struct exec_src *x, u32 off, void *dst, u32 n);
void fs_exec_close(const struct exec_src *x);      /* FS_CLOSE for an fs_exec_open that succeeded */
long fs_read_file(const char *path, const struct proc *parent, u8 **buf, u32 *size, struct ustat *st);
long vfs_open(const char *path, int flags);
long vfs_stat(const char *path, struct ustat *st);
long vfs_fstat(struct file *f, struct ustat *st);
long vfs_chdir(const char *path);
long vfs_lseek(struct file *f, s32 off, u32 whence);
long vfs_statfs(void *out, u32 mount);
long vfs_mount(const struct umountreq *r);
long vfs_umount(const char *path);
long vfs_mountinfo(u32 index, struct umountinfo *out);
long vfs_path1(u32 op, const char *p, u32 a2, u32 a3);   /* chmod/utimes/lstat/readlink on a path */
long vfs_link(u32 op, const char *a, const char *b);
extern u32 time_base;
long vfs_sync(void);
long vfs_mkdir(const char *path);
long vfs_unlink(const char *path);
long vfs_rmdir(const char *path);
long vfs_rename(const char *from, const char *to);
long vfs_readdir(struct file *f, struct udirent *out);
long vfs_pipe(int fds[2]);

/* tty.c */
void tty_init(void);
void tty_input(u8 c);                   /* to the tty of the console on the screen */
long tty_read(int tty, void *buf, u32 n);
#define TTY_RAW    1                    /* no line editing, no echo: bytes as they come */
#define TTY_NOSIG  2                    /* ^C is delivered as a byte, not a kill */
void tty_setmode(int tty, int mode, int owner_pid);
int tty_getmode(int tty);
void tty_inject(int tty, const char *s);   /* bytes into the input queue, bypassing the line discipline */
void tty_release(int pid);
int tty_take_intr(int tty);             /* and clear: was ^C typed with nobody to kill? */              /* a process exited: undo its mode */
long tty_write(int tty, const void *buf, u32 n);
void tty_setfg(int tty, int pid);
int tty_getfg(int tty);
int tty_of(const struct proc *p);

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
#define SYS_mkdir   25
#define SYS_unlink  26
#define SYS_rmdir   27
#define SYS_rename  28
#define SYS_port_create 29
#define SYS_port_find   30
#define SYS_call        31
#define SYS_recv        32
#define SYS_reply       33
#define SYS_blk_read    34
#define SYS_blk_write   35
#define SYS_hrtime      36   /* E-clock ticks since boot (709379 Hz PAL, 715909 NTSC) */
/* 39 and 40 (the workspace shell requests) and 68 (the login session) were removed in audit batch E: free */
#define SYS_statfs      43   /* filesystem usage of the root: struct ustatfs* */
#define SYS_sync        44
#define SYS_ttymode     45   /* a1 = TTY_* flags for this process's tty */
#define SYS_ttyinfo     46   /* rows << 16 | columns */
#define SYS_envset      47   /* replace this process's environment (inherited by children) */
#define SYS_envget      48
#define SYS_dup         49   /* lowest free fd >= 3 referring to the same file */
#define SYS_dup2        50
#define SYS_ttyintr     51   /* did the user type ^C while this process (a shell) was the one running? */
#define SYS_chmod       52   /* path, mode */
#define SYS_link        53   /* existing, new */
#define SYS_symlink     54   /* target text, link path */
#define SYS_readlink    55   /* path, buffer, max */
#define SYS_utimes      56   /* path, modification time */
#define SYS_lstat       57   /* like stat, but a final symbolic link is not followed */
#define SYS_time        58   /* seconds since 1970 (UTC) */
#define SYS_settime     59
#define SYS_isatty      60
#define SYS_getuid      61
#define SYS_getgid      62
#define SYS_setuid      63   /* root: to anyone; others: only back to their own id */
#define SYS_setgid      64
#define SYS_umask       65   /* mask -> old mask */
#define SYS_chown       66   /* path, uid, gid (-1 keeps one): root only */
#define SYS_geteuid     67
#define SYS_fd_info     76   /* drive -> bit 0 drive present, 1 disk inserted, 2 write protected; sectors << 8 (disk server only) */
#define SYS_fd_rw       77   /* lba | drive << 28 | write << 27, count, buffer -> 0 or -1 (disk server only) */
#define SYS_input       74   /* s32[4] <- mouse dx, dy (since the last call), mouse buttons, joystick bits */
#define SYS_klog        73   /* offset, buffer, length -> characters of the kernel log copied (0 = nothing more) */
#define SYS_blk_ram     42   /* where the boot ramdisk is: u32[2] = address, size (disk server only) */
#define SYS_blk_size    41   /* sectors on the disk (disk server only) */
#define SYS_jobctl      75   /* pid, op: 0 = stop, 1 = continue (own processes, or root) */
#define SYS_mount       69   /* struct umountreq*: put a running filesystem server on a directory */
#define SYS_umount      70   /* path: tell the server to quit and remove the mount */
#define SYS_mountinfo   71   /* index, struct umountinfo*: 0, or -1 past the end */
#define SYS_blk_flush   72   /* make everything written so far durable (disk server only) */
#define SYS_replyrecv   38   /* reply to a message and wait for the next, in one trap */
#define SYS_ttymirror   37   /* 0 = stop copying screen output to the serial port */
#define SYS_getpriority 78   /* which (PRIO_*), who -> 20 - nice (1 .. 40, never a negative errno's range), or -errno */
#define SYS_setpriority 79   /* which, who, nice: anyone lowers their own processes, only root raises */
void syscall_c(u32 *rb);
void trap0_entry(void);
void irq5_entry(void);
void switch_to(struct proc *old, struct proc *next);
void user_first_entry(void);
