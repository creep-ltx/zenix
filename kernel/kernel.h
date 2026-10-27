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
    u32 upd_pa, upd_size;       /* +0x78, +0x7C: an update bundle zenixboot left in memory for zup (DESIGN D15), 0 if none */
    u32 upd_flags;              /* +0x80: UPD_F_*; an older loader leaves all three 0 (the table is cleared) */
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
/* /dev/ttyS0 (DESIGN D12): one process at a time owns the port; then the console copy, the kernel log's copy and the
 * 0x1d hook are off the wire and received bytes wait for serdev_read */
long serdev_open(void);                 /* -EBUSY when it is open already */
void serdev_close(void);
u32 serdev_take(u8 *buf, u32 n);        /* bytes received so far (0: none yet) */
long serdev_write(const u8 *buf, u32 n);  /* all of it, waiting by ticks; on a kill or a 2 s stall the count, or -EINTR/-EIO */
void kprintf(const char *fmt, ...);
u32 klog_read(u32 off, char *buf, u32 n);     /* the kernel log: n characters from position off (0 = the first ever) */
void klog_to_screen(void);                    /* the display is up: show the log so far and mirror new messages */
void klog_mirror_off(void);                   /* boot is finished: kprintf goes to the serial port and the log only */
void klog_mirror_on(void);                    /* a panic: its text goes to the screen too */
void klog_write(const char *s, u32 n);        /* a service's output (FT_KLOG): into the log as kprintf's */
/* previous-boot log (audit A12): the log lives in the top KLOG_AREA_SIZE bytes of chip RAM (two slots of a 256-byte
 * header and an 8 KB ring; the bootblock reserves the same size), so the next boot after a warm reset can show how
 * this one ended */
#define KLOG_AREA_SIZE  0x4200
#define KLOG_PREV       0x80000000u           /* SYS_klog offset flag: read the previous boot's log (dmesg -p) */
#define KLOG_END_RUNNING 0u                   /* why a boot ended, kept in the log header for the next boot */
#define KLOG_END_PANIC   1u
#define KLOG_END_REBOOT  2u                   /* the reboot system call */
/* the boot note (SYS_reboot): a one-shot choice for the boot menu (boot/bootmenu.c) in the spare bytes of the log
 * area's first header, BOOTNOTE_OFF from the area's start (the top of chip RAM - KLOG_AREA_SIZE). Four longs: magic,
 * kind, ~(magic + kind), 0. The same numbers are in boot/bootmenu.c. */
#define BOOTNOTE_OFF     0xF0u
/* the update note (DESIGN D15): four longs at UPDNOTE_OFF in the same spare bytes (outside every sum, before the boot
 * note): magic, kind (1 done, 2 failed, 3 confirmed, 4 confirmed but the root half failed), version, ~(magic + kind +
 * version). zenixboot reads it before the kernel starts and clears it; klog_init clears it at every start (before
 * every write), so it says what this boot did */
#define UPDNOTE_OFF      0xE0u
#define UPDNOTE_MAGIC    0x5A55504Eu          /* "ZUPN" */
#define BOOTNOTE_MAGIC   0x5A4E5854u          /* "ZNXT" */
#define BOOTNOTE_AMIGAOS 1u
#define BOOTNOTE_ZENIX   2u
long bootnote_set(u32 kind);                  /* -ENODEV when the log area is not in chip RAM (a small machine) */
void machine_reset(void) __attribute__((noreturn));
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
void set_usp(u32 usp);               /* D16: the live user stack pointer (a signal frame, sigreturn) */
extern u32 kstack_bottom[];          /* the boot stack (pid 0, idle): 8 KB below kstack_top */
void cache_enable(int is040plus);
void cache_flush_code(void);         /* after writing code that will run: I-cache (and Emu68 JIT) flush */
void exc_common(void);
void irq2_entry(void);
void irq2_c(u32 *rb);                /* D16: the level-2 entry saves all registers (d0-d7, a0-a6, then the CPU frame) */
void irq_other_entry(void);         /* vectors 24-31 without a handler of their own */
void irq_other_c(u32 fv);

/* mem.c */
#define MEM_CHIP     1u         /* must be chip RAM (DMA: graphics, audio, disk) */
#define MEM_FASTONLY 2u         /* fast RAM only, fail rather than use chip */
#define MEM_LOW1G    4u         /* fast RAM ending below 1 GB: what the Pi 4's VideoCore can read (the mailbox) */
void mem_init(const struct bootinfo *bi, u32 kernel_pa, u32 kernel_mem);
/* DESIGN D15: the update bundle zenixboot left in memory (bootinfo +0x78..+0x80), checked and reserved by mem_init as
 * a block in use, given back by SYS_update RELEASE; SYS_update itself is in syscall.c */
extern u32 upd_pa, upd_size, upd_flags;
void upd_release(void);
#define UPD_INFO      1                 /* struct uupdate (usys.h; at most the size given) */
#define UPD_RELEASE   2                 /* root: the bundle's memory back to the pool */
#define UPD_NOTE      3                 /* root: kind (UPDNOTE_*), version -> the update note */
#define UPD_F_ROLLBACK 1u               /* bootinfo flags: "Zenix (previous)" (zenixboot PREVIOUS) */
#define UPDNOTE_FAILED    2u
#define UPDNOTE_CONFIRMED 3u
#define UPDNOTE_ROOTFAILED 4u           /* CONFIRMED over a FAILED (or ROOTFAILED) of the same version (audit3 B1) */
long updnote_set(u32 kind, u32 version);        /* serial.c: klog area +0xE0, for zenixboot's next start */
extern u32 boot_ram_pa, boot_ram_size;
void *mem_alloc(u32 n, u32 flags, u32 owner);   /* 0 if n is 0, above MEM_ALLOC_MAX, or does not fit */
#define MEM_ALLOC_MAX 0x7FFF0000u       /* no block size near 4 GB: the rounding in mem_alloc cannot wrap */
#define MEMALLOC_MAX  (64u << 20)       /* largest region SYS_memalloc gives a process (64 MB) */
void mem_free(void *p);
u32 mem_free_owner(u32 owner);
void mem_set_owner(void *p, u32 owner);         /* a block changes hands (a loaded program's code becomes a segment's) */
#define SEG_OWNER 0x80000000u                   /* the owner of every shared code segment's block: never a pid (D13) */
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
extern int gfx_depth;            /* bitplanes: 1 to 4 (DESIGN D14; 0: no display) */
extern const u16 gfx_palette[16];       /* the 16 ANSI colours, 12 bits */
extern const u16 gfx_palette2[4];       /* the four colours of 2 planes */
int gfx_set_colours(int colours);       /* SYS_console CON_COLOURS: 2, 4, 8, 16 (0: ask) -> the colours shown, or -errno */
void term_redraw(void);                 /* the console shown, drawn again from its grid (a new depth) */
/* colour: foreground in the low nibble, background in the high one (0x07: grey on black) */
void gfx_cell(int row, int col, u8 ch, u8 attr, u8 colour);
void gfx_scroll(u8 colour);             /* the whole screen up a row; the new row takes colour's background */
void gfx_clear_text(u8 colour);
void gfx_fill_rows(int row, int n, u8 colour);  /* n rows take colour's background (the blitter) */
void gfx_move_rows(int dst, int src, int n);    /* n rows from src to dst, overlapping or not (the blitter) */
void gfx_vblank(void);
void term_putc(int con, char c);
void term_write(int con, const char *s, u32 n);
void term_scroll_view(int delta);       /* Shift+Up/Down: scrollback */
int term_back(void);
int term_ckm(void);                     /* the console on the screen wants ESC O A arrows (DECCKM) */
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
extern volatile u32 reboot_deadline;    /* SYS_reboot's sync: the tick resets the machine at this tick (0: none; A7) */
extern const char *volatile reboot_who; /* ... naming the server it waited for */
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
/* ZEX2: a program with shared code (DESIGN D13, tools/mkzexe.py, docs/d13-shared-code.md): the shared part (code and
 * constants) is file offsets 0..text_size, one copy in memory for every process running it (a segment, seg.c); the
 * data part (table, .data, then .bss) every process has its own copy of, a5 pointing at its start */
struct zex2 {
    u16 bra, bra_disp;
    u32 magic;                  /* 'ZEX2' */
    u32 text_size, data_size, mem_size, nrel_code, nrel_data, reloc_off, stack_size, reserved;
};
#define ZEX2_MAGIC 0x5a455832u
union zhdr { struct zexe v1; struct zex2 v2; };
#define ZEXE_MEM_MAX   (16u << 20)     /* the loader refuses a program asking for more (image + bss) */
#define ZEXE_STACK_MIN 4096u            /* a stack size in the header, if not 0, lies in this range */
#define ZEXE_STACK_MAX (1u << 20)

/* files, pipes, tty (vfs.c, tty.c) */
#define NFD   16
#define NFILE 64
enum { FT_NONE = 0, FT_INODE, FT_TTY, FT_PIPE_R, FT_PIPE_W, FT_NULL, FT_ZERO, FT_SERIAL, FT_KLOG, FT_RANDOM, FT_SOCK };   /* FT_KLOG: a service's output, into the kernel log (D11); FT_RANDOM: /dev/urandom (off 0) and /dev/random (off 1), D19; FT_SOCK: a socket (ino: its ksock), D19 */
struct pipe {
    u8 buf[2048];
    u16 rd, wr, count;
    u8 readers, writers;
    u32 rwait, wwait;           /* D16: the slots waiting to read, to write (event.c) */
};
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800                /* DESIGN D19: never wait (sockets, /dev/random; pipes later). Linux's value */
#define S_IFSOCK 0140000                /* DESIGN D19: fstat's type of a socket (Linux's value) */
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
struct uupdate { u32 addr, size, flags, kvnum; char kver[16]; };   /* SYS_update INFO (D15; frozen, grows at the end) */
struct uproc {                  /* returned by SYS_procinfo */
    u32 pid, ppid, state, mem;
    char name[16];
    s32 nice;                   /* the base priority on the nice scale, -20 (most favoured) .. 19 */
    u32 pri;                    /* the effective level now, 0 (highest) .. 31; PRI_IDLE for pid 0 */
    u32 shared;                 /* D13: bytes of shared code it runs (one copy for all its users; not in mem) */
    u32 pgid;                   /* D16: its process group (rule 8: at the end) */
};
#define UPROC_FIRST 32          /* its first size (pid .. name): what a caller passing size 0 gets (D15 step 0) */
/* the first sizes of the other structs written into a program's memory (frozen, audit3 B3: never sizeof, which
 * grows with the struct): what a caller passing size 0 (every program built before D15) gets. Checked against the
 * structs' first fields in syscall.c (_Static_assert) and by tools/usize-test.sh */
#define USTAT_FIRST      28     /* ino .. links */
#define UDIRENT_FIRST    72     /* ino, type, name[64] */
#define UMOUNTINFO_FIRST 88     /* path[48] .. type[8] */
#define UUPDATE_FIRST    32     /* addr .. kver[16] */
#define USTATFS_FIRST    20     /* struct ustatfs (fsproto.h) */
#define UMEMINFO_FIRST   16     /* SYS_meminfo's four words */
/* how many bytes a caller with a buffer of GIVEN bytes gets of a struct whose first size is FIRST and whose size is
 * NOW: 0 (given) -> first, below first -> 0 (refused), else at most now */
static inline u32 usize_n(u32 given, u32 first, u32 now)
{
    return !given ? first : given < first ? 0 : given < now ? given : now;
}

/* DESIGN D16: events (event.c). Fixed bits per process; objects carry masks of the waiting slots */
#define EV_IO    1u                     /* an object it registered on (tty, pipe, serial device, port) changed */
#define EV_SIG   2u                     /* a signal that runs a handler arrived */
#define EV_CHILD 4u                     /* a child ended or stopped */
#define EV_CON   8u                     /* init: a console wants a login, or the reset warning came */
struct upollfd { s32 fd; u16 events, revents; };    /* SYS_evwait's items: Linux's struct pollfd (frozen) */
#define POLLIN   0x0001                 /* poll's bits: Linux values (frozen) */
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020
#define POLLPORT 0x4000                 /* Zenix: fd is a port the caller owns; POLLIN = a message is queued */
#define EV_MAXITEMS 32
struct proc;
void ev_post(struct proc *p, u32 bits);
void ev_wake(u32 *mask);                /* every slot in the mask gets EV_IO; the mask empties */
void ev_arm(u32 *mask);                 /* the caller registers on an object and clears EV_IO */
long ev_block(u32 bits, int timed, u32 until, int restart);  /* 0 woken, -EINTR killed or a handler due */
long ev_wait_items(u32 uitems, u32 n, s32 timeout);
u16 vfs_poll(struct file *f);           /* registers the caller on the object (vfs.c) */
u16 port_poll(int port);                /* ipc.c */
u16 serdev_poll(void);                  /* serial.c */
extern u32 serdev_wait;
/* DESIGN D16: signals (signal.c). Numbers and default actions as Linux (frozen) */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGKILL  9
#define SIGUSR1  10
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGWINCH 28
#define NSIG     32
#define SIGBIT(n) (1u << (n))
#define SIG_UNCATCHABLE (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))
#define SIG_DUE(p) ((p)->sig_pend & ~(p)->sig_block)        /* a handler is due (only caught ones are pending unblocked) */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define SA_RESTART  0x10000000u
struct usigaction {             /* SYS_sigaction (frozen, grows at the end; size first) */
    u32 size;                   /* USIGACT_FIRST or more */
    u32 handler;                /* 0 default, 1 ignore, anything else: caught (libz's table holds the function) */
    u32 mask, flags;            /* blocked while it runs; SA_RESTART */
    u32 entry;                  /* the program's entry for handlers (0: the one registered before) */
};
#define USIGACT_FIRST 20
struct sigframe {               /* written on the user stack for a handler, read back by sigreturn (frozen, grows at the end) */
    u32 size;                   /* SIGFRAME_FIRST */
    u32 regs[16];               /* d0-d7, a0-a6, a7 (the user stack pointer before the frame) */
    u32 pc;
    u32 ccr;                    /* the condition codes: the status register's low 5 bits */
    u32 sig;
    u32 blocked;                /* the blocked mask before the handler */
};
#define SIGFRAME_FIRST 84
#define SIG_STACK_ROOM (16u + 512u)    /* the frame leaves the guard (16 bytes) and 512 bytes below it (rule 5) */
void sig_send(struct proc *p, int sig);
long sig_group(int g, int sig, int check);
long sig_action(u32 sig, u32 uact);
long sig_procmask(u32 how, u32 set, u32 uold);
long sig_alarm(u32 t);
long sig_setpgid(int pid, int pg);
void sig_deliver(u32 *rb, u32 nr);
long sig_return(u32 *rb, u32 f);
int may_signal(const struct proc *p);   /* syscall.c: 0, -ESRCH or -EACCES */
/* the one test on the return paths: anything for sig_deliver to do? */
#define SIG_WORK(p) (SIG_DUE(p) | (u32)(p)->killed | (p)->stop_req)

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
#define PRIO_PGRP    1                  /* not supported: -EINVAL (frozen, D16 rule 8) */
#define PRIO_USER    2                  /* who is a user id (0: the caller's real user id) */
#define NREG  8                 /* extra memory regions per process */
#define ENVMAX 1024
/* P_WAIT and P_IO are no longer entered (D16: wait and I/O block in P_EV); SYS_procinfo still reports a process in
 * P_EV as one of them (waiting for a child: P_WAIT, else P_IO), so the numbers programs know keep their meaning */
enum pstate { P_FREE = 0, P_READY, P_SLEEP, P_WAIT, P_IO, P_ZOMBIE, P_MSG, P_STOP, P_EV };
struct proc {                   /* start.S relies on the first three: */
    u32 *ksp;                   /*   offset 0 */
    u32 usp;                    /*   offset 4 */
    u32 entry;                  /*   offset 8 */
    int state;
    int pid, ppid;
    int exit_code;
    u32 wake;                   /* P_SLEEP: tick to wake at */
    u32 base, span;             /* the process's own memory (image+bss+stack) */
    u32 kstk, ktop, ustk;       /* stack guard (proc.c): bottom and top of the kernel stack, bottom of the user stack
                                 * (0: none, pid 0); a guard pattern lies at each bottom */
    struct { u32 base, size; } reg[NREG];   /* memalloc'd blocks */
    u32 cwd_ino;
    char cwd[160];              /* the working directory as text (FS_PATHMAX, fsproto.h): a longer one is refused */
    struct file *fd[NFD];
    int killed;
    u8 exiting;                 /* in proc_exit: its own calls (FS_CLOSE) must not end it a second time */
    u8 stop_req, stop_rep;      /* stop asked for (acted on when the process next runs); the parent has been told it is stopped */
    int priv;                   /* may use the raw disk (the filesystem server) */
    u16 uid, ruid, gid, umask;  /* effective and real user id, group id (inherited; a set-user-id program changes only uid); the filesystem server enforces the permissions */
    char *env;                  /* environment: NAME=value NUL ... NUL NUL, envlen bytes (at most ENVMAX) in a block
                                 * the process owns (freed with it); inherited by children. Was a 1 KB array in every
                                 * slot: 32 KB of BSS, given back for D16's memory (2026-10-26) */
    u16 envlen;
    struct proc *rnext;         /* run queue link */
    u8 queued;                  /* on the run queue (of level qlevel) */
    u8 qlevel;
    s8 nice;                    /* base priority, NICE_MIN .. NICE_MAX: inherited, lowered by anyone, raised by root */
    u8 blevel;                  /* NICE_LEVEL(nice), kept so that no path divides */
    u8 prio;                    /* effective level (0 highest): blevel, or a better client's while serving it */
    struct umsg *wait_msg;      /* P_MSG as a client: the request in flight */
    u8 ctty;                    /* controlling tty (console 1..NCONS): the console of fd 0 at spawn, else the parent's */
    u8 wait_con;                /* in wait with WAIT_CONSOLE: a console switch posts it EV_CON */
    int wait_port;              /* P_MSG: as a server the port being listened on, as a client the port called */
    char name[16];
    u32 kill_at;                /* when the first kill came (a service process: 5 s later it ends even mid-request) */
    u8 svc_role;                /* D11: woken to load (SVC_ROLE_LOAD) or to keep a start's deadline (SVC_ROLE_WATCH) */
    u8 svc_depth;               /* D11: service loads under way in this process (at most 2 nested); its calls carry uid 0 */
    u8 call_ns;                 /* D11: the call in flight may not start a server (a no-start call, svc.c) */
    struct seg *seg;            /* D13: the shared code segment it runs (0: none, or a private copy) */
    u32 code, csize;            /* D13, a ZEX2 program: where its code is (the segment's or its own private copy); user
                                 * pointers into it may be read by system calls, never written (user_ok_rd) */
    u32 a5;                     /* D13: the a5 its code needs (the start of its data part; 0 for a ZEXE program) */
    /* D16 (event.c, signal.c) */
    u32 ev_pending, ev_wait;    /* event bits (EV_*) that arrived / it waits for (0 when not in an event wait) */
    u32 sig_pend, sig_block;    /* signals (bit n: signal n) pending, blocked */
    u32 sig_catch, sig_ign, sig_rest;   /* handled by its program, ignored, restarted after the handler (SA_RESTART) */
    u32 sig_entry;              /* the program's entry for handlers (libz), registered by sigaction */
    u32 *sig_mask;              /* per signal: what is blocked while its handler runs (sa_mask); NSIG words from the
                                 * kernel heap at the first sigaction with a mask (0: all empty; 4 KB of BSS saved) */
    u32 alarm_at;               /* alarm: SIGALRM at this tick (while alarm_on) */
    int pgid;                   /* process group (inherited; no parent: its own pid) */
    u8 alarm_on;
    u8 sig_intr;                /* the system call gave up for a signal (a restart is possible, rule 4) */
    u8 kill_sig;                /* ended by this signal's default action: wait status 128 + it */
};
extern struct proc procs[NPROC];
extern struct proc *current;
void proc_init(void);
int proc_spawn(const char *path, char *const *argv, int argc,
               struct proc *parent, const int *stdio, int tty);
int proc_spawn_image(const void *img, u32 len, char *const *argv, int argc, int priv, int tty);   /* no parent; console tty, 0: the kernel log */
void schedule(void);
void proc_exit(int code) __attribute__((noreturn));
void proc_handover(int pid);           /* SYS_exit's second argument: the caller's child becomes its parent's (D18) */
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
extern volatile u32 wake_min;        /* no sleeper or alarm is due before this tick (while ntimer) */
void resched(void);                  /* act on need_resched: switch only to a strictly better level */
void sleep_until(u32 tick);          /* current to P_SLEEP until tick (the caller then schedules) */
int proc_renice(struct proc *p, int nice);   /* set the base priority (clamped); the effective level follows */
void prio_raise(struct proc *p, u8 level);   /* inheritance: p serves a client at level: at least that until done */
void prio_recalc(struct proc *p);            /* p finished serving: back to its base or its best waiting client */
u8 ipc_best_client(int pid);                 /* ipc.c: the best level among clients waiting for or served by pid */
int user_ok_wr(const struct proc *p, u32 ptr, u32 len);   /* the process's own memory: it may be written */
int user_ok_rd(const struct proc *p, u32 ptr, u32 len);   /* ... or its shared code (D13): only read */
u32 user_rd_extent(const struct proc *p, u32 ptr);        /* how many bytes from ptr on user_ok_rd allows (0: none) */
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
void ready(struct proc *p);         /* make a blocked process runnable; a stopped one stays stopped (D16 rule 2) */
extern int ntimer;                  /* sleepers (P_SLEEP) plus alarms set: gates the tick's scan (D16 rule 3) */
void proc_kill(struct proc *p);       /* killed: 1 a kill, 2 a fault (139), 3 a second kill (at once) */
int kill_status(const struct proc *p);   /* the exit status of a killed process: 139 a fault, 128 + n signal n, 130 */
int proc_kill_now(void);              /* the end of a system call: is current killed and may it end now? */
struct proc *proc_by_pid(int pid);

/* ipc.c: ports and messages. A message lives in the sender's memory; only
 * pointers move (no MMU, so the server reads and writes the client's
 * buffers directly). The sender is blocked until the server replies. */
#define NSVC  12               /* D11 service entries (svc.c); port slots 0..NSVC-1 are their held ports */
#define NPORT (NSVC + 16)       /* the held ports, then 16 ordinary ones */
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
struct port {
    int used, owner;            /* owner 0: a held port (D11) whose server is not running */
    u32 gen;                    /* how many times this slot was created: part of the port number (audit G1) */
    struct proc *op;            /* the owner's slot (valid while owned: a dying owner's ports are closed or held) */
    char name[16];
    struct umsg *head, *tail;
    struct proc *waiter;        /* server blocked in recv */
    u32 evw;                    /* D16: the slots polling it (SYS_evwait with POLLPORT) */
};
extern struct port ports[NPORT];
#define PORT_ID(i) ((int)((ports[i].gen << 8) | (u32)(i)))     /* a port number: (generation << 8) | slot */
int port_create(const char *name, int owner);
int port_slot(const char *name);                /* the slot of a port by name, -1 if none */
int port_find(const char *name);
struct proc *port_owner(int port);             /* the process that owns a port, 0 if none */
long ipc_call(int port, struct umsg *m);       /* SYS_call: kflags 0 */
long ipc_kcall(int port, struct umsg *m, int ns);   /* the kernel's own requests (vfs.c): kflags UMSG_KERNEL; ns: SVC_NS_* */
long ipc_wait(struct umsg *m);                 /* the caller's m is queued: wait for the reply (and take a D11 role) */
void ipc_enqueue(struct port *p, struct umsg *m, int front);
void ipc_unqueue(struct port *p, struct umsg *m);
void ipc_finish(struct umsg *m, s32 result);
int ipc_serving(int pid);                      /* is pid serving a request now? */
#define UMSG_KERNEL 1
#define UMSG_NETREPLY 2                 /* D19: answered by the network server's NETSRV_REPLY (sock.c), internal */
struct proc *ipc_client(struct umsg *m);       /* the client of m if it is in flight and served by current, else 0 */
struct umsg *ipc_recv(int port, int wait);
struct umsg *ipc_replyrecv(struct umsg *m, s32 result, int port);
long ipc_reply(struct umsg *m, s32 result);
void ipc_proc_exit(int pid);

/* svc.c: the service manager (DESIGN D11, docs/d11-service-manager.md) */
#define SVC_TEXT 80                     /* a program and its arguments: at most 80 bytes of text, 6 words */
#define SVC_REGISTER 1                  /* SYS_svc op | nice << 8, name, program text (root) */
#define SVC_REMOVE   2                  /* op, name (root) */
#define SVC_SELF     3                  /* op -> how many times the caller's entry was started, 0 if not a service */
#define SVC_SAVE     4                  /* op, buffer, length: the started server leaves a table for its next start */
#define SVC_LOAD     5                  /* op, buffer, length -> the bytes of that table copied (0: none) */
#define SVC_OPENS    6                  /* op, buffer, length -> inode numbers the kernel holds open on the caller's mount */
#define SVC_MIDREQ   0x10000            /* SVC_SELF: the last end was in the middle of a request */
#define SVC_SAVED    512                /* the most a server may save */
#define SVC_EXIT_DAMAGED 86             /* a server's exit status: its saved table is damaged (an essential one: panic) */
#define SVC_NS_CLOSE 1                  /* no-start call: dropped if stopped, queued while loading or starting */
#define SVC_NS_SKIP  2                  /* no-start call: answered -ECONNRESET unless a server runs */
#define SVC_ROLE_LOAD  1
#define SVC_ROLE_WATCH 2
long svc_call(int slot, struct umsg *m, int ns);   /* a call to a held port with no server */
long svc_role(struct umsg *m);          /* the caller was woken to load or watch for the port of its call */
int svc_handover(int slot);             /* port_create of a registered name: the started process takes its port */
struct proc *svc_proc(int slot);        /* who works for a held port now (the loader or the starting server) */
void svc_exit(struct proc *p);          /* p is dying: the entries it served, loaded or watched */
void svc_hurry(void);                   /* the reset warning or reboot began: no more back-off, sleeping loaders go on */
void svc_born(int pid);                 /* proc_load made the server of the load under way in current */
long svc_sys(u32 op, const char *name, const char *text);
long svc_data(u32 op, void *buf, u32 len);   /* SVC_SAVE, SVC_LOAD */
int svc_is(const char *name);           /* is current the process the kernel started (or runs) for that service? */

/* sock.c: sockets (DESIGN D19, docs/d19-network.md 4.1-4.3; the protocol in netproto.h) */
#define NSOCK 32                        /* kernel socket objects, shared by every process (a few kept for root) */
long sock_sys(u32 opw, u32 fd, u32 ureq);
long netsrv_sys(u32 op, u32 a, u32 b);
long sock_rw(struct file *f, void *buf, u32 n, int write);
u16 sock_poll(struct file *f);
void sock_release(u32 i);               /* the socket file's last close */
void sock_server_gone(void);            /* the "net" server ended (svc.c): a new epoch, every socket dead */
extern u32 net_epoch;

/* vfs.c */
struct file *file_new(int type);
struct file *file_dup(struct file *f);
void file_close(struct file *f);
int fd_install(struct proc *p, struct file *f);
long vfs_read(struct file *f, void *buf, u32 n);
long vfs_write(struct file *f, const void *buf, u32 n);
long fs_call(u32 type, u32 a0, u32 a1, u32 a2, u32 a3);
struct ustat;
struct exec_src {                       /* a program file being loaded (fs_exec_open .. fs_exec_close) */
    int m; long ino; struct ustat st;
    struct exec_src *next;              /* the loads in progress (D11) */
    u32 gen;                            /* D13: the mount's generation at the open */
    u8 stale;                           /* D13: the file may have changed since the open: never enter it as a segment */
    u8 wopen;                           /* D13: it was open for writing at the open: load a private copy */
};
long fs_exec_open(const char *path, const struct proc *parent, struct exec_src *x);
long fs_exec_read(const struct exec_src *x, u32 off, void *dst, u32 n);
void fs_exec_close(struct exec_src *x);            /* FS_CLOSE for an fs_exec_open that succeeded */
long vfs_opens(const char *pname, int port, u32 *out, u32 max);
u32 vfs_mount_gen(int m);                       /* D13: changes on every mount and server restart (0: not mounted) */
void vfs_server_started(const char *pname);     /* D13: a filesystem server (re)started on that port (svc.c) */
/* seg.c: shared code segments (DESIGN D13, docs/d13-shared-code.md sections 6-7) */
#define NSEG 8
struct seg {
    u32 code, size;             /* the shared part in memory; code 0: the slot is free */
    u32 sum;                    /* its add-and-rotate checksum, taken after loading */
    u16 refs;                   /* processes running it */
    u8 listed;                  /* found by seg_find: the key still names an unchanged file */
    u8 m;                       /* the key: mount slot, generation, inode, size, modification time */
    u32 gen, ino, fsize, mtime;
};
u32 seg_sum(u32 code, u32 size);
struct seg *seg_find(const struct exec_src *x, u32 size, const char *path);   /* a reference to a listed, intact one */
struct seg *seg_enter(const struct exec_src *x, u32 code, u32 size, u32 sum, const char *path, int *made);   /* list
                                 * it, or the one listed meanwhile (made 0: the caller frees its copy); 0: private */
void seg_put(struct seg *s);                    /* drop a reference; the last one frees the code */
void seg_unlist(int m, long ino);               /* file ino (-1: every file) of mount m may have changed */
int seg_busy(int m, const struct ustat *st);    /* a segment of that file exists (ETXTBSY) */
int seg_on(int m);                              /* a segment loaded from mount m exists */
void seg_stats(u32 *bytes, u32 *saved);         /* shared code in memory, and what sharing it saves */
void seg_crash(struct proc *p, u32 pc);         /* a process running shared code faulted: is the code intact? */   /* SVC_OPENS: the inodes held open on that mount */
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
long vfs_sync(int reboot);             /* 1: SYS_reboot, each server at most 5 s (then the tick resets) */
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
void tty_setfg(int tty, int id, int group);   /* the foreground: a pid, or (group) a process group; 0 nobody */
int tty_fgpid(int tty);                 /* the foreground's pid when it is a single process, else 0 */
u16 tty_poll(int tty);                  /* D16: POLLIN when a read would not wait (registers the caller); POLLOUT */
int tty_of(const struct proc *p);

/* syscalls: TRAP #0, number in d0, args d1-d3, result in d0 */
#define SYS_exit    1    /* status; a2 (DESIGN D18): a child's pid, made the parent's child first (0: none) */
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
#define SYS_reboot      80   /* note (0, BOOTNOTE_AMIGAOS, BOOTNOTE_ZENIX): root only; sync, stop disk writes, reset */
#define SYS_svc         81   /* op (SVC_*), arg, buffer: the service manager (D11, svc.c) */
#define SYS_update      82   /* op (UPD_*), a, b: the update bundle, the kernel's version, the update note (D15) */
/* DESIGN D16, events and signals (docs/d16-events-and-signals.md). SYS_kill's third argument 1: the new form (pid, sig;
 * sig 0 only asks whether it exists; pid 0 the caller's group, -g group g); 0: the old form, as before. SYS_setfg's
 * second argument 1: the foreground is a process group. SYS_jobctl's stop and continue are SIGSTOP and SIGCONT. */
#define SYS_evwait      83   /* struct upollfd *, count, timeout in ticks (-1: none) -> items ready, 0, -EINTR */
#define SYS_sigaction   84   /* sig, struct usigaction * (0: only ask) -> old disposition: 0 default, 1 ignored, 2 caught */
#define SYS_sigprocmask 85   /* how (SIG_BLOCK ...), set, u32 *old (0: none) */
#define SYS_sigreturn   86   /* the frame the entry was given: back from a handler (returns the frame's d0) */
#define SYS_alarm       87   /* ticks (0: off) -> ticks the previous alarm had left */
#define SYS_setpgid     88   /* pid (0: self), pgid (0: pid): oneself or one's own children */
#define SYS_console     89   /* op (CON_*), value: the text console's settings (the 2 MB diet, D14 section 11) */
#define CON_COLOURS     1    /* value 2, 4, 8 or 16: set (root only); 0: ask -> the colours shown now */
/* DESIGN D19, the network (docs/d19-network.md section 4.2; ops and structs in netproto.h) */
#define SYS_sock        90   /* op (SOCK_*) | request size << 16, fd, struct usockreq * -> per op */
#define SYS_netsrv      91   /* op (NETSRV_*), a, b: only the network server */
void syscall_c(u32 *rb);
void trap0_entry(void);
void irq5_entry(void);
void switch_to(struct proc *old, struct proc *next);
void user_first_entry(void);
