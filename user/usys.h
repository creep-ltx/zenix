/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* Zenix system calls: TRAP #0, number in d0, args d1-d3, result in d0 (a negative error number on failure). */
typedef unsigned int u32;
#include "../kernel/errno.h"

#ifdef ZENIX_LIBC
#include <errno.h>                       /* the C library (DESIGN D27): its errno is the one variable, its headers */
#include <fcntl.h>                       /* give the constants below, from the same kernel headers */
#include <signal.h>
#include <sys/stat.h>
#include <poll.h>                        /* struct pollfd and the POLL* bits (the same layout and numbers) */
#define zerrno errno
#endif
extern int zerrno;                       /* libz: the error number of the last failed system call (EACCES ...) */
static inline int sys3(int n, u32 a, u32 b, u32 c)
{
    register int d0 __asm__("d0") = n;
    register u32 d1 __asm__("d1") = a;
    register u32 d2 __asm__("d2") = b;
    register u32 d3 __asm__("d3") = c;
    __asm__ volatile("trap #0" : "+r"(d0) : "r"(d1), "r"(d2), "r"(d3) : "memory", "cc");
    if (d0 < 0)
        zerrno = -d0;
    return d0;
}

#define USYS_STRUCTS
/* Frozen interfaces (CLAUDE.md): a struct the kernel or a server writes into our memory only grows at its end, and we
 * pass our buffer's size (sizeof) so a newer kernel never writes past it; an older one writes its smaller struct */
struct udirent { u32 ino, type; char name[64]; };
struct ustat { u32 ino, mode, size, mtime, uid, gid, links; };
struct uproc { u32 pid, ppid, state, mem; char name[16]; int nice; u32 pri; u32 shared; u32 pgid; };   /* nice -20..19; pri: the level now, 0 highest, 32 idle; mem: its own memory; shared: the shared code it runs (D13); pgid: its process group (D16) */

#ifndef ZENIX_LIBC                       /* the C library: its own headers have them (libc/zenix/include) */
#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#endif

static inline void sys_exit(int c) { sys3(1, (u32)c, 0, 0); for (;;); }
/* end, the child `pid` handed to our parent first (DESIGN D18: login stepping aside for init); login's status then is
 * ST_HANDED + that pid, so init knows the console's session goes on in it */
static inline void sys_exit_handing(int c, int pid) { sys3(1, (u32)c, (u32)pid, 0); for (;;); }
#define ST_HANDED 0x40000000
static inline int sys_write(int fd, const void *p, u32 n) { return sys3(2, (u32)fd, (u32)p, n); }
static inline void sys_yield(void) { sys3(3, 0, 0, 0); }
static inline int sys_getpid(void) { return sys3(4, 0, 0, 0); }
static inline int sys_spawn(const char *path, char *const *argv, const int *stdio)
{ return sys3(5, (u32)path, (u32)argv, (u32)stdio); }
static inline int sys_wait(int pid) { return sys3(6, (u32)pid, 0, 0); }
static inline int sys_waitnb(int pid) { return sys3(6, (u32)pid, 1, 0); }   /* -2: not yet */
#define ST_STOPPED 0x114        /* sys_waitu result: the child was stopped (Ctrl-Z) */
static inline int sys_waitu(int pid) { return sys3(6, (u32)pid, 2, 0); }    /* like wait, but also returns ST_STOPPED once for a stopped child */
#define ST_CONSOLE 0x200        /* sys_waitcon result ST_CONSOLE + n: console n was shown with nobody on it */
static inline int sys_waitcon(void) { return sys3(6, (u32)-1, 4, 0); }    /* init (root): any child, or a console that wants a login */
static inline int sys_jobctl(int pid, int op) { return sys3(75, (u32)pid, (u32)op, 0); }   /* op 0 = stop, 1 = continue */
static inline void sys_sleep(u32 ticks) { sys3(7, ticks, 0, 0); }
static inline int sys_open(const char *p, int fl) { return sys3(8, (u32)p, (u32)fl, 0); }
static inline int sys_close(int fd) { return sys3(9, (u32)fd, 0, 0); }
static inline int sys_read(int fd, void *p, u32 n) { return sys3(10, (u32)fd, (u32)p, n); }
static inline int sys_lseek(int fd, int off, int whence) { return sys3(11, (u32)fd, (u32)off, (u32)whence); }
static inline int sys_readdir(int fd, struct udirent *d) { return sys3(12, (u32)fd, (u32)d, sizeof *d); }
static inline int sys_stat(const char *p, struct ustat *s) { return sys3(13, (u32)p, (u32)s, sizeof *s); }
static inline int sys_fstat(int fd, struct ustat *s) { return sys3(14, (u32)fd, (u32)s, sizeof *s); }
static inline int sys_chdir(const char *p) { return sys3(15, (u32)p, 0, 0); }
static inline int sys_getcwd(char *b, u32 n) { return sys3(16, (u32)b, n, 0); }
static inline int sys_pipe(int fds[2]) { return sys3(17, (u32)fds, 0, 0); }
static inline int sys_pipe2(int fds[2], int flags) { return sys3(17, (u32)fds, (u32)flags, 0); }   /* flags: O_NONBLOCK (both ends) */
static inline void *sys_memalloc(u32 n) { return (void *)sys3(18, n, 0, 0); }   /* 0 (NULL) on failure */
#define MEMA_CHIP 1             /* DESIGN D38: chip RAM (the blitter, the Copper and the sprites reach only that) */
static inline void *sys_memalloc_chip(u32 n) { return (void *)sys3(18, n, MEMA_CHIP, 0); }
static inline int sys_memfree(void *p) { return sys3(19, (u32)p, 0, 0); }
static inline u32 sys_ticks(void) { return (u32)sys3(20, 0, 0, 0); }
static inline u32 sys_uptime(void) { return (u32)sys3(20, 1, 0, 0); }   /* whole seconds since boot (F9) */
static inline int sys_procinfo(u32 i, struct uproc *p) { return sys3(21, i, (u32)p, sizeof *p); }
static inline int sys_meminfo(u32 out[4]) { return sys3(22, (u32)out, 16, 0); }
static inline int sys_setfg(int pid) { return sys3(23, (u32)pid, 0, 0); }
static inline int sys_kill(int pid) { return sys3(24, (u32)pid, 0, 0); }   /* the old form: end it (status 130) */

#ifndef ZENIX_LIBC                       /* the C library: its own headers have them (libc/zenix/include) */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400
#define O_EXCL   0x80     /* with O_CREAT: fail with -17 if the name exists (also as a symbolic link) */
#endif
static inline int sys_mkdir(const char *p) { return sys3(25, (u32)p, 0, 0); }
static inline int sys_unlink(const char *p) { return sys3(26, (u32)p, 0, 0); }
static inline int sys_rmdir(const char *p) { return sys3(27, (u32)p, 0, 0); }
static inline int sys_rename(const char *a, const char *b) { return sys3(28, (u32)a, (u32)b, 0); }

struct umsg { struct umsg *next; int from, server; u32 state; int result; u32 type; u32 a[4]; unsigned short uid, gid, umask, kflags; u32 client_; };
#define UMSG_KERNEL 1     /* kflags: the kernel sent this message for a system call (servers trust only these, and root) */
#define UMSG_CANCEL 4     /* kflags: its client was killed while waiting (DESIGN D23, kernel/fsproto.h): -EINTR is allowed */
#define UMSG_LINKS 8      /* kflags: the kernel takes the FS_LINKED answer (DESIGN D26, kernel/fsproto.h); only it sets it */
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
static inline int sys_input(int *out) { return sys3(74, (u32)out, 0, 0); }   /* out[4]: mouse dx, dy, buttons, joystick */
static inline int sys_klog(u32 off, void *buf, u32 n) { return sys3(73, off, (u32)buf, n); }   /* kernel log: characters copied */
static inline int sys_fd_info(u32 d) { return sys3(76, d, 0, 0); }   /* floppy: bit 0 drive, 1 disk, 2 write protected; sectors << 8 */
static inline int sys_fd_rw(u32 d, u32 lba, u32 n, void *buf, int wr) { return sys3(77, lba | d << 28 | (u32)(wr != 0) << 27, n, (u32)buf); }
static inline int sys_blk_size(void) { return sys3(41, 0, 0, 0); }
static inline int sys_blk_ram(u32 out[2]) { return sys3(42, (u32)out, 0, 0); }
struct ustatfs { unsigned bsize, blocks, bfree, files, ffree; };
static inline int sys_statfs(struct ustatfs *f) { return sys3(43, (u32)f, 0, sizeof *f); }
static inline int sys_statfs_mount(struct ustatfs *f, u32 mount) { return sys3(43, (u32)f, mount, sizeof *f); }
static inline int sys_sync(void) { return sys3(44, 0, 0, 0); }
#define TTY_RAW   1
#define TTY_NOSIG 2
static inline int sys_ttymode(int flags) { return sys3(45, (u32)flags, 0, 0); }
static inline int sys_ttyinfo(void) { return sys3(46, 0, 0, 0); }   /* rows << 16 | columns */
/* DESIGN D20 7.2: a terminal's settings, named by a descriptor (a console, a pty's slave or master) */
#define TTYC_GWINSZ 1           /* arg struct winsize *: its size (a console's: the screen's) */
#define TTYC_SWINSZ 2           /* arg struct winsize *: a pty's new size (SIGWINCH to its foreground on a change) */
#define TTYC_OWNER  3           /* arg a uid: root only, a pty: who may open its slave */
#define TTYC_SCTTY  4           /* arg 0: this tty becomes the caller's controlling tty, the caller its session leader */
static inline int sys_ttyctl(int fd, int op, u32 arg) { return sys3(95, (u32)fd, (u32)op, arg); }
static inline int sys_envset(const char *block, u32 len) { return sys3(47, (u32)block, len, 0); }
static inline int sys_envget(char *block, u32 max) { return sys3(48, (u32)block, max, 0); }
static inline int sys_dup(int fd) { return sys3(49, (u32)fd, 0, 0); }
static inline int sys_dup2(int from, int to) { return sys3(50, (u32)from, (u32)to, 0); }
static inline int sys_ttyintr(void) { return sys3(51, 0, 0, 0); }
static inline int sys_chmod(const char *p, u32 mode) { return sys3(52, (u32)p, mode, 0); }
static inline int sys_link(const char *a, const char *b) { return sys3(53, (u32)a, (u32)b, 0); }
static inline int sys_symlink(const char *t, const char *p) { return sys3(54, (u32)t, (u32)p, 0); }
static inline int sys_readlink(const char *p, char *buf, u32 max) { return sys3(55, (u32)p, (u32)buf, max); }
static inline int sys_utimes(const char *p, u32 t) { return sys3(56, (u32)p, t, 0); }
static inline int sys_lstat(const char *p, struct ustat *st) { return sys3(57, (u32)p, (u32)st, sizeof *st); }
static inline u32 sys_time(void) { return (u32)sys3(58, 0, 0, 0); }
static inline int sys_settime(u32 t) { return sys3(59, t, 0, 0); }
/* from a checked source (sntp): the kernel marks the clock CLOCK_NET (D21 8.3) */
static inline int sys_settime_checked(u32 t) { return sys3(59, t, 1, 0); }
#define CLOCK_RTC 1     /* sys_clockbits: the battery clock set the time at boot */
#define CLOCK_NET 2     /* sys_clockbits: set this boot from a checked source (sntp) */
/* who set the time this boot: CLOCK_RTC, CLOCK_NET (negative from a kernel before them) */
static inline int sys_clockbits(void) { return sys3(20, 2, 0, 0); }
static inline int sys_isatty(int fd) { return sys3(60, (u32)fd, 0, 0); }
static inline int sys_getuid(void) { return sys3(61, 0, 0, 0); }
static inline int sys_getgid(void) { return sys3(62, 0, 0, 0); }
static inline int sys_setuid(u32 id) { return sys3(63, id, 0, 0); }
static inline int sys_setgid(u32 id) { return sys3(64, id, 0, 0); }
static inline int sys_umask(u32 mask) { return sys3(65, mask, 0, 0); }
static inline int sys_chown(const char *p, u32 uid, u32 gid) { return sys3(66, (u32)p, uid, gid); }
static inline int sys_geteuid(void) { return sys3(67, 0, 0, 0); }
/* supplementary groups (SYS_groups 94, kernel/kernel.h): at most NGROUPS ids besides the gid, inherited, kept across
 * set-user-id. get: max 0 counts them, a smaller room than the list is -EINVAL; set: root only (login, su, sudo) */
#ifndef NGROUPS
#define NGROUPS 8         /* kernel/fsproto.h */
#endif
static inline int sys_getgroups(unsigned short *g, u32 max) { return sys3(94, 0, (u32)g, max); }
static inline int sys_setgroups(const unsigned short *g, u32 n) { return sys3(94, 1, (u32)g, n); }
struct umountreq { const char *path, *port, *dev, *type; };
struct umountinfo { char path[48], port[16], dev[16], type[8]; };
static inline int sys_mount(const struct umountreq *r) { return sys3(69, (u32)r, 0, 0); }
static inline int sys_umount(const char *path) { return sys3(70, (u32)path, 0, 0); }
static inline int sys_mountinfo(u32 index, struct umountinfo *out) { return sys3(71, index, (u32)out, sizeof *out); }
static inline int sys_blk_flush(void) { return sys3(72, 0, 0, 0); }
#define PRIO_PROCESS 0    /* priorities (DESIGN D10): who is a pid, 0 = the caller */
#define PRIO_PGRP    1    /* not supported (-EINVAL) */
#define PRIO_USER    2    /* who is a user id, 0 = the caller's */
static inline int sys_getpriority(int which, int who) { return sys3(78, (u32)which, (u32)who, 0); }   /* 20 - nice, or -errno */
static inline int sys_setpriority(int which, int who, int nice) { return sys3(79, (u32)which, (u32)who, (u32)nice); }
#define BOOTNOTE_AMIGAOS 1u                     /* SYS_reboot: the boot menu starts this system at once (kernel.h) */
#define BOOTNOTE_ZENIX   2u
#define REBOOT_HALT      0x100u    /* SYS_reboot: halt instead of the reset (halt, poweroff, shutdown) */
static inline int sys_reboot(u32 note) { return sys3(80, note, 0, 0); }
/* the service manager (DESIGN D11): register a port name with a program ("@image args" or "/path args", at most 80
 * bytes, 6 words) and its nice (root); remove it (root); how many times the caller's service was started (0: none) */
#define SVC_REGISTER 1
#define SVC_REMOVE   2
#define SVC_SELF     3
#define SVC_TEXT_MAX 80
static inline int sys_svc_register(const char *name, const char *prog, int nice)
{ return sys3(81, SVC_REGISTER | ((u32)nice & 0xFF) << 8, (u32)name, (u32)prog); }
static inline int sys_svc_remove(const char *name) { return sys3(81, SVC_REMOVE, (u32)name, 0); }
static inline int sys_svc_self(void) { return sys3(81, SVC_SELF, 0, 0); }
#define SVC_SAVE     4
#define SVC_LOAD     5
#define SVC_EXIT_DAMAGED 86     /* a server's exit status: its saved table is damaged (the kernel stops for diskd) */
static inline int sys_svc_save(const void *buf, u32 n) { return sys3(81, SVC_SAVE, (u32)buf, n); }   /* at most 512 bytes */
static inline int sys_svc_load(void *buf, u32 n) { return sys3(81, SVC_LOAD, (u32)buf, n); }        /* bytes copied, 0: none */
#define SVC_OPENS    6
#define SVC_MIDREQ   0x10000    /* in sys_svc_self(): the server's last end was in the middle of a request */
static inline int sys_svc_opens(u32 *ino, u32 bytes) { return sys3(81, SVC_OPENS, (u32)ino, bytes); }   /* how many */
/* updates (DESIGN D15, docs/d15-self-updates.md): INFO fills struct uupdate (at most the size given: frozen, grows at
 * the end) with the bundle zenixboot left in memory (0 = none), its flags and the running kernel's version; RELEASE
 * gives the bundle's memory back (root); NOTE leaves zenixboot the update note: a kind and a version (root). The
 * bundle is read in place and never passed to a system call (it is not the caller's memory: -EFAULT). */
struct uupdate { u32 addr, size, flags, kvnum; char kver[16]; };
#define UPD_INFO      1
#define UPD_RELEASE   2
#define UPD_NOTE      3
#define UPD_F_ROLLBACK 1u       /* flags: the owner chose "Zenix (previous)" (zenixboot PREVIOUS) */
#define UPD_F_TRIAL    2u       /* flags: an unconfirmed trial runs (D22 4.3): zpkg installs nothing live. Kept after
                                 * UPD_RELEASE; an older zenixboot leaves it 0 (zenixboot sets it from D22 step 7) */
#define UPDNOTE_DONE      1u    /* NOTE kinds: zup installed the root half (or it was installed already) */
#define UPDNOTE_FAILED    2u    /* zup refused the bundle or rolled the root back */
#define UPDNOTE_CONFIRMED 3u    /* login's prompt is up on console 1: this kernel works (login.c) */
#define UPDNOTE_ROOTFAILED 4u   /* the kernel makes a CONFIRMED over a FAILED of the same version this (audit3 B1) */
static inline int sys_update(u32 op, u32 a, u32 b) { return sys3(82, op, a, b); }
/* events and signals (DESIGN D16, docs/d16-events-and-signals.md; libz.h has the POSIX calls on top of these) */
#ifndef ZENIX_LIBC                       /* the C library: its <poll.h> (libc/zenix/include, from these numbers) */
struct pollfd { int fd; short events, revents; };
#define POLLIN   0x0001         /* Linux's values */
#define POLLPRI  0x0002         /* on a port (D23): a message sent to it was cancelled (UMSG_CANCEL) since the last POLLPRI */
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020
#define POLLPORT 0x4000         /* Zenix: fd is a port this process owns; POLLIN when a message is queued */
#define POLLRDBAND 0x0080       /* on a port (D38): something was posted into its window (SYS_post) since the last look */
#endif
static inline int sys_evwait(struct pollfd *f, u32 n, int ticks) { return sys3(83, (u32)f, n, (u32)ticks); }   /* ticks -1: no limit */
struct usigaction { u32 size, handler, mask, flags, entry; };   /* handler 0 default, 1 ignore, else caught */
static inline int sys_sigaction(int sig, const struct usigaction *a) { return sys3(84, (u32)sig, (u32)a, 0); }   /* old: 0 default, 1 ignored, 2 caught */
static inline int sys_sigprocmask(int how, u32 set, u32 *old) { return sys3(85, (u32)how, set, (u32)old); }
static inline int sys_sigreturn(void *frame) { return sys3(86, (u32)frame, 0, 0); }
static inline int sys_alarm(u32 ticks) { return sys3(87, ticks, 0, 0); }       /* ticks the previous alarm had left */
static inline int sys_setpgid(int pid, int pg) { return sys3(88, (u32)pid, (u32)pg, 0); }
static inline int sys_killsig(int pid, int sig) { return sys3(24, (u32)pid, (u32)sig, 1); }   /* pid 0: own group, -g: group g */
static inline int sys_setfgpg(int pg) { return sys3(23, (u32)pg, 1, 0); }     /* the tty's foreground: process group pg */
#define CON_COLOURS 1           /* sys_console: value 2, 4, 8 or 16 (root), 0 asks -> the colours shown now */
static inline int sys_console(int op, int v) { return sys3(89, (u32)op, (u32)v, 0); }   /* the 2 MB diet (D14 section 11) */
/* DESIGN D20 4.5: a console's 8-bit font and rules, the console named by a descriptor (op | (fd + 1) << 8) */
#define CON_FONT 2              /* a PSF1 font (mode 0, height 8, 2,052 bytes) for the console; a null pointer drops it */
#define CON_RULES 3             /* the rules: bits, mask of the bits to change (0 asks) -> the bits before */
#define CONR_PCCTRL 1           /* bytes 1-31 but BEL BS HT LF CR ESC are glyphs */
#define CONR_WRAPNOW 2          /* the last column wraps at once */
#define CONR_CLRHOME 4          /* ESC[2J homes the cursor */
#define CONR_BRIGHT 8           /* SGR 1 is the bright foreground, not bold */
#define CONR_ICE 16             /* SGR 5 and 6 the bright background */
#define CONR_CGAPAL 32          /* the CGA palette while shown */
#define CONR_LFONLY 64          /* LF without CR */
#define CONR_8BIT 128           /* DESIGN D28: 8-bit mode with Topaz (no UTF-8; the keys Latin-1) */
/* DESIGN D28 7: the console's Unicode font (ZCF1) for every console: root, no descriptor; a null pointer drops it */
#define CON_UFONT 4
static inline int sys_conufont(const void *zcf, u32 n) { return sys3(89, CON_UFONT, (u32)zcf, n); }
static inline int sys_confont(int fd, const void *psf, u32 n) { return sys3(89, CON_FONT | (u32)(fd + 1) << 8, (u32)psf, n); }
static inline int sys_conrules(int fd, u32 bits, u32 mask) { return sys3(89, CON_RULES | (u32)(fd + 1) << 8, bits, mask); }
/* DESIGN D24 6.1-6.3, the remote control's console ops: root only (-EPERM), any console's descriptor; a console never
 * used -ENXIO. The structs (kernel/kernel.h, frozen): the caller fills size (sizeof), the buffer and its size. */
#define CON_READ 5              /* the grid: struct uconread, cells of 3 bytes (glyph, CONA_*, colour) -> cell bytes */
#define CON_LINE 6              /* a display line of the shown console: struct uconline, a pen a pixel -> pens */
#define CON_INPUT 7             /* kind CONI_RAW raw key codes (the keyboard), CONI_TEXT text (that console's tty) */
#define CONI_RAW 0
#define CONI_TEXT 1
#define CONI_MAX 256            /* bytes a call -> how many were taken */
struct uconread {            /* cells: rows x cols of 3 bytes, row by row */
    u32 size, cells, ncells;
    unsigned short rows, cols, crow, ccol;
    u32 changes;
    unsigned char flags, csize;
    unsigned short pad;
};
#define UCONREAD_FIRST 28       /* the first sizes (frozen); the kernel writes back into size the bytes it filled */
#define UCONLINE_FIRST 56
_Static_assert(__builtin_offsetof(struct uconread, pad) + 2 == UCONREAD_FIRST, "uconread's first size");
#define CONRD_SHOWN 1           /* the console on the display */
#define CONRD_FONT 2            /* an 8-bit font is loaded (CON_FONT): its glyphs, CP437 for the PC's */
#define CONRD_BACK 4            /* the display shows its scrollback */
#define CONRD_CURSOR 8          /* the cursor is shown */
#define CONA_INV 1
#define CONA_UND 2
#define CONA_BOLD 4
struct uconline {
    u32 size, pens, npens;
    unsigned short width, height, depth, pad;
    u32 generation;
    unsigned short palette[16]; /* 12 bits 0x0RGB, pens 0 .. 2^depth - 1 */
};
_Static_assert(__builtin_offsetof(struct uconline, palette) + 32 == UCONLINE_FIRST, "uconline's first size");
static inline int sys_conread(int fd, struct uconread *r) { return sys3(89, CON_READ | (u32)(fd + 1) << 8, (u32)r, 0); }
/* DESIGN D28: CON_READ's a3 1, cells of 4 bytes (the 16-bit value big-endian, CONA_*, colour): a value above 0xFF is
 * a code point, 0xFFFE a character beyond U+FFFF, 0xFFFF the right half of a wide character */
#define CONRF_CELL4 1
static inline int sys_conread4(int fd, struct uconread *r) { return sys3(89, CON_READ | (u32)(fd + 1) << 8, (u32)r, CONRF_CELL4); }
static inline int sys_conline(int fd, int line, struct uconline *d) { return sys3(89, CON_LINE | (u32)(fd + 1) << 8, (u32)line, (u32)d); }
static inline int sys_coninput(int fd, int kind, const void *b, u32 n) { return sys3(89, CON_INPUT | (u32)(fd + 1) << 8, (u32)b, n | (u32)kind << 16); }
/* DESIGN D38 step 2: a console in graphics mode for the display server (root; kernel/kernel.h has the whole contract).
 * CON_GRAB on a console's descriptor -> a descriptor that reads struct ugevent records; CON_GACK on that one answers a
 * GEV_RELEASE. The structs are frozen (the first sizes below). */
#define CON_GRAB 8
#define CON_GACK 9
struct ucongrab {
    u32 size, cop, sprite;      /* the Copper list and sprite 0 (the pointer), both the caller's chip RAM */
    unsigned short sprh;        /* the sprite's height in lines */
    signed char hotx, hoty;     /* its hot spot */
    unsigned short width, height, depth, chips;     /* back: the screen, the text console's planes, GCHIP_* */
    u32 topaz;                  /* (size 28 or more) a 1,792-byte buffer for the console's Topaz 8, Latin-1 32-255 */
};
#define UCONGRAB_FIRST 24
#define UCONGRAB_TOPAZ 28
#define GCHIP_AGA 1
_Static_assert(__builtin_offsetof(struct ucongrab, topaz) == UCONGRAB_FIRST, "ucongrab's first size");
struct ugevent {
    unsigned char type, code;   /* GEV_*; a key's raw code, bit 7 up */
    unsigned short qual;        /* GQ_* */
    short x, y;                 /* the pointer; GEV_RELEASE: its number, GEV_LOST: how many */
    unsigned char ch[4];        /* a key's character, UTF-8 */
    u32 time;                   /* the 100 Hz tick */
};
_Static_assert(sizeof(struct ugevent) == 16, "ugevent is 16 bytes");
#define GEV_KEY 1
#define GEV_POINTER 2
#define GEV_RELEASE 3
#define GEV_ACQUIRE 4
#define GEV_LOST 5
#define GQ_LSHIFT 0x0001
#define GQ_RSHIFT 0x0002
#define GQ_CAPS 0x0004
#define GQ_CTRL 0x0008
#define GQ_LALT 0x0010
#define GQ_RALT 0x0020
#define GQ_LAMIGA 0x0040
#define GQ_RAMIGA 0x0080
#define GQ_REPEAT 0x0200
#define GQ_MBUTTON 0x1000
#define GQ_RBUTTON 0x2000
#define GQ_LBUTTON 0x4000
static inline int sys_congrab(int fd, struct ucongrab *g) { return sys3(89, CON_GRAB | (u32)(fd + 1) << 8, (u32)g, 0); }
static inline int sys_congack(int gfd, u32 seq) { return sys3(89, CON_GACK | (u32)(gfd + 1) << 8, seq, 0); }
/* DESIGN D38 step 3: serving clients that are not the kernel (kernel/kernel.h has the contract) */
#define PEER_CTTY 1             /* the client's controlling tty */
#define PEER_RD 2               /* struct upeermem: those bytes are the client's to read -> 0, else -EFAULT */
#define PEER_WR 3
#define PEER_PORT 4             /* arg a port: 0 if the client owns it */
struct upeermem { u32 addr, len; };
static inline int sys_peer(void *msg, int op, void *arg) { return sys3(96, (u32)msg, (u32)op, (u32)arg); }
#define POST_BUF 1              /* the port's owner: its window and who may post into it */
#define POST_WRITE 2            /* the poster: bytes into the window, the owner woken (POLLRDBAND) */
struct upostbuf { u32 addr, size; int from; };
struct upost { u32 off, src, len; };
static inline int sys_post(int op, int port, void *arg) { return sys3(97, (u32)op, (u32)port, (u32)arg); }
#ifndef ZENIX_LIBC                       /* the C library: its own headers have them (libc/zenix/include) */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
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
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#define SA_RESTART  0x10000000
#endif
/* DESIGN D19, the network: the raw calls (the BSD functions over them are libz's, step 4); ops and structs in
 * kernel/netproto.h */
#ifndef ZENIX_LIBC                       /* the C library: its own headers have them (libc/zenix/include) */
#define O_NONBLOCK 0x800        /* never wait (sockets, /dev/random) */
#define S_IFSOCK 0140000        /* fstat's type of a socket */
#endif
static inline int sys_sock(u32 op, int fd, void *req) { return sys3(90, op, (u32)fd, (u32)req); }
static inline int sys_netsrv(u32 op, u32 a, u32 b) { return sys3(91, op, a, b); }
static inline int sys_drv(u32 op, u32 a, u32 b) { return sys3(92, op, a, b); }   /* DESIGN D19 step 7: the network's drivers (DRV_*, netproto.h) */
