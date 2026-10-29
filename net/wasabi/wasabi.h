/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * wasabid's core (DESIGN D24, docs/d24-remote.md, step 0 of section 14): the remote control daemon, speaking the
 * owner's Wasabi protocol version 1 (the frames, tags and payloads of Wasabi's PROTOCOL.md, D24 3), without system
 * calls. The connections and their frames, HELLO and caps, discovery, the address filter and the wrong-key pause
 * (3.1, 3.2), the commands of D24 4, the run slots and the descriptor budget (5.1), files (5.3), the kernel log
 * stream (5.4), REBOOT and its guard (5.5), HEALTH (5.6), /etc/wasabid.conf (5.7) and the console commands (6.1-6.3,
 * 11: TEXT).
 *
 * The same files build for /sbin/wasabid (its OS layer: Zenix's system calls through libz, D24 14 step 1, the main
 * session's) and for the PC tests and fuzzing (tools/host/wasabi-test.sh: an OS layer over Linux's calls, and one in
 * memory for the fuzzer). One daemon per process: the core's state is its own (static), set up by wsb_init.
 *
 * The core owns the loop: wsb_step builds the poll set, calls wsb_os_poll once with the timeout of its next timer
 * (at most 100 ms while a child lives or a subscriber listens, 5.1, 5.4), then serves what is ready, a piece at a
 * time: every socket and pipe is read and written only after poll said so, and never waited on (5.1). The OS layer's
 * main is:   int st = wsb_init(argc, argv); while (st == WSB_GOON) st = wsb_step(); return st;
 *
 * The OS layer's duties before wsb_init: SIGPIPE ignored; SIGCHLD's handler (if any) only sets a flag (the core
 * reaps every child it started with wsb_os_wait, 5.1); the environment of the commands is WSB_ENV (Zenix: set once
 * with SYS_envset, inherited by every spawn; Linux: given to execve).
 *
 * Descriptors are the OS layer's small numbers (Zenix's and Linux's file descriptors); the core counts them against
 * WSB_NFD (5.1's table) before every accept, spawn and open. Errors are negative errno numbers (Linux's, as
 * Zenix's): -EAGAIN "not now, poll again", -ENETDOWN "no network yet" (5.7: tried again every 10 s).
 */
#ifndef WASABI_H
#define WASABI_H

#include <stdint.h>

/* ---- the protocol (Wasabi's PROTOCOL.md, version 1) ---- */
#define WSB_PROTO       1
#define WSB_PORT        1236            /* TCP, Zenix's own (3.1) */
#define WSB_DISC_A      1234            /* UDP discovery: Wasabi's port and Zenix's (3.1) */
#define WSB_DISC_B      1236
#define WSB_MAXPAYLOAD  65536           /* a longer length closes the connection */
#define WSB_SMALL       4096            /* every frame but a PUT's or SPEED's streamed DATA, both ways (9) */
#define WSB_CAPS "ping,info,ls,put,get,run,del,mkdir,debug,reboot,rebootto,restart,ps,kill,speed,speedfile,quit,grab," \
                 "screen,windows,hb,key,health,detach,slots,sum,inputs,zenix,text"          /* exact, D24 4 */

/* tags: Wasabi's, and TEXT (D24 11, 0x59) */
#define WT_HELLO   0x01
#define WT_WELCOME 0x02
#define WT_ERR     0x03
#define WT_OK      0x04
#define WT_PING    0x05
#define WT_PONG    0x06
#define WT_PUT     0x10
#define WT_GET     0x11
#define WT_DATA    0x12
#define WT_END     0x13
#define WT_LS      0x14
#define WT_DEL     0x15
#define WT_MKDIR   0x16
#define WT_RUN     0x20
#define WT_STDOUT  0x21
#define WT_STDERR  0x22
#define WT_EXIT    0x23
#define WT_DEBUG   0x30
#define WT_LOG     0x32
#define WT_REBOOT  0x40
#define WT_INFO    0x41
#define WT_RESTART 0x42
#define WT_PS      0x43
#define WT_KILL    0x44
#define WT_SPEED   0x45
#define WT_QUIT    0x46
#define WT_GRAB    0x48
#define WT_SCREEN  0x49
#define WT_KEY     0x4B
#define WT_WINDOWS 0x4C
#define WT_HEALTH  0x4D
#define WT_SLOTS   0x50
#define WT_FREE    0x51
#define WT_SLOT    0x52
#define WT_SUM     0x57
#define WT_INPUTS  0x58
#define WT_TEXT    0x59
/* every other tag (SNOOP, INSTALL, MOUSE, LIVE, CLIP, UI, UIACT, AREXX, PORTS, unknown ones): ERR "not on Zenix" */

/* flag bits on the wire */
#define WR_MERGE   1                    /* RUN: stderr into stdout */
#define WR_DETACH  2                    /* RUN: /tmp/wasabi-bg-N, no slot */
#define WR_SLOT    4                    /* RUN: a SLOT frame first */
#define WF_ASKONLY 1                    /* FREE: SIGINT only, answer "asked" */
#define WB_AMIGAOS 2                    /* REBOOT (caps rebootto): AmigaOS next */
#define WB_ZENIX   4                    /* REBOOT: Zenix next */
#define WB_NOGUARD 8                    /* REBOOT: skip the guard (5.5) */
#define WQ_FORCE   1                    /* RESTART, QUIT: the slots' commands SIGINT, SIGKILL after 3 s */
#define WK_FORCE   1                    /* KILL: SIGKILL (clear: SIGINT) */
#define WS_FROMAMIGA 1                  /* SPEED: the daemon sends */
#define WX_CELLS   1                    /* TEXT: with each cell's style and colour */
#define WG_NOTES   1                    /* GRAB's flags word after the name (caps zenix): STDERR notes welcome; without
                                         * it the note is only logged (an older client fails on any frame but DATA) */

/* ---- the daemon's limits and times (D24 5) ---- */
#define WSB_NFD       16                /* descriptors a process has (NFD) */
#define WSB_CONNS     4                 /* client connections; the fifth: ERR "busy", closed */
#define WSB_SLOTS     2                 /* run slots; a third: ERR "all run slots are busy" */
#define WSB_XFERS     1                 /* file transfers (PUT/GET/SPEED through a file); another: ERR "busy" */
#define WSB_PATHMAX   63                /* the system calls' limit (5.3) */
#define WSB_STALL_MS  10000             /* no progress in either direction: the connection is dropped (3.2) */
#define WSB_HELLO_MS  10000             /* no WELCOME this long after accept: dropped, however slowly bytes come (B2-1) */
#define WSB_KILL_MS   3000              /* SIGINT, then SIGKILL after this (FREE, a slot whose client went, --force) */
#define WSB_HB_MS     5000              /* heartbeats on the log stream */
#define WSB_LOG_MS    100               /* the kernel log is read this often while a subscriber listens */
#define WSB_CHILD_MS  100               /* the longest poll while a child lives (5.1) */
#define WSB_PAUSE_MS  1000              /* the next HELLO from an address that sent a wrong key waits this long */
#define WSB_DRAIN_MS  1000              /* REBOOT: the most it waits for its sockets to drain (5.5) */
#define WSB_NETWAIT_MS 10000            /* a network that is down: tried again this often (5.7) */
#define WSB_KEYMIN    16                /* /etc/wasabid.conf's key: at least this many characters */
#define WSB_INPUTS    340               /* INPUTS records, one 4 KB frame (4) */
#define WSB_PANICLINES 20               /* the previous boot's last lines replayed after a panic (5.4) */
#define WSB_ST_STOP    0                /* exit statuses: a stop (QUIT, no or a bad configuration): not started again */
#define WSB_ST_RESTART 75               /* init's "start me again at once" (D24 7) */
#define WSB_ST_FAIL    1                /* could not start (init's back-off applies) */
#define WSB_GOON      (-1)              /* wsb_step: carry on */

/* the commands' environment (5.1), NULL-terminated */
#define WSB_ENV { "HOME=/root", "PATH=/bin:/sbin", "USER=root", "TERM=dumb", 0 }

/* ==== what the OS layer gives the core ==== */

/* -- time and the log -- */
uint32_t wsb_os_now(void);                         /* milliseconds, any start, may wrap */
uint32_t wsb_os_uptime(void);                      /* whole seconds since boot (SYS_ticks 1) */
void wsb_os_log(const char *line);                 /* one line for the kernel log (no newline): standard error, which
                                                    * is /dev/kmsg under init (6.5) */
void *wsb_os_alloc(uint32_t n);                    /* the heap: 0 when full (every result checked) */
void wsb_os_free(void *p);
const char *wsb_os_strerror(int e);                /* the words of an error number (libz's zstrerror), 0 if unknown */

/* -- poll: Linux's struct pollfd and bits (frozen in D16) -- */
struct wsb_pollfd { int fd; short events, revents; };
#define WSB_POLLIN  0x0001
#define WSB_POLLOUT 0x0004
#define WSB_POLLERR 0x0008
#define WSB_POLLHUP 0x0010
#define WSB_POLLNVAL 0x0020
int wsb_os_poll(struct wsb_pollfd *f, int n, int timeout_ms);  /* ready count, 0 timeout, -EINTR (a signal) */

/* -- sockets (IPv4; addresses and ports in host order; every socket O_NONBLOCK) -- */
int wsb_os_listen(unsigned port);                  /* TCP on every address, SO_REUSEADDR, a backlog of 4 -> fd */
int wsb_os_udp(unsigned port);                     /* UDP bound on every address -> fd */
int wsb_os_accept(int lfd, uint32_t *peer);        /* -> the new connection's fd (O_NONBLOCK), -EAGAIN: none */
long wsb_os_recv(int fd, void *b, long n);         /* bytes, 0 the other side closed, -EAGAIN, < 0 broken */
long wsb_os_send(int fd, const void *b, long n);   /* MSG_NOSIGNAL: bytes taken (may be fewer), -EAGAIN, < 0 broken */
long wsb_os_recvfrom(int fd, void *b, long n, uint32_t *addr, unsigned *port);
long wsb_os_sendto(int fd, const void *b, long n, uint32_t addr, unsigned port);
int wsb_os_shutdown(int fd);                       /* SHUT_WR: our side is done (REBOOT's drain: the peer's end is
                                                    * then awaited, at most WSB_DRAIN_MS) */
void wsb_os_close(int fd);                         /* sockets, pipes and files */

/* -- files (paths absolute, at most WSB_PATHMAX bytes; the core checks before it calls) -- */
#define WSB_O_READ   0                             /* open's flags: the core's own; the OS layer maps them */
#define WSB_O_WRITE  1                             /* write only, created (mode), O_EXCL: never an existing name */
#define WSB_O_APPEND 2                             /* write only, created if missing, appended (the refused count) */
struct wsb_stat {
    uint32_t mode;                                 /* Unix type and permission bits (S_IF* as Linux's, Zenix's) */
    uint32_t size, mtime, uid;                     /* mtime: Unix seconds (unsigned, SYS_time's rule) */
};
int wsb_os_open(const char *path, int flags, uint32_t mode);    /* O_NONBLOCK always (5.3) -> fd */
long wsb_os_read(int fd, void *b, long n);
long wsb_os_write(int fd, const void *b, long n);
int wsb_os_fstat(int fd, struct wsb_stat *st);
int wsb_os_stat(const char *path, struct wsb_stat *st);        /* follows symbolic links (LS shows the target, 4) */
int wsb_os_lstat(const char *path, struct wsb_stat *st);
int wsb_os_rename(const char *from, const char *to);
int wsb_os_unlink(const char *path);
int wsb_os_rmdir(const char *path);
int wsb_os_mkdir(const char *path);
int wsb_os_chmod(const char *path, uint32_t mode);
int wsb_os_opendir(const char *path);              /* -> fd */
int wsb_os_readdir(int fd, char name[64]);         /* 1 an entry (never . or ..), 0 the end, < 0 an error */
int wsb_os_realpath(const char *path, char *out, int cap);      /* links and .. resolved (5.3) */
struct wsb_mount { char path[48], type[8]; };      /* SYS_mountinfo's umountinfo, the fields the core reads */
int wsb_os_mount(unsigned i, struct wsb_mount *m); /* the i-th mount: 0, or -ENOENT past the last */
struct wsb_statfs { uint32_t bsize, blocks, bfree; };
int wsb_os_statfs(unsigned mount, struct wsb_statfs *f);       /* the mount of that index (wsb_os_mount's) */

/* -- processes -- */
/* argv[0] the program's path; in, out, err the child's standard files (the pipes' write ends, /dev/null, a file);
 * the child in a process group of its own (pgid = its pid), before it can run a command (D16). -> pid */
int wsb_os_spawn(const char *const *argv, int in, int out, int err);
int wsb_os_pipe(int fds[2]);                       /* fds[0] the daemon's read end, fds[1] the child's write end
                                                    * (blocking for the child; the core reads only after POLLIN) */
int wsb_os_wait(int pid);                          /* the child's status if it ended (reaped), -EAGAIN if not yet */
int wsb_os_kill(int pid, int sig);                 /* Linux's signal numbers; pid < 0: the process group -pid */
struct wsb_proc {
    uint32_t pid, ppid, pgid, state;               /* state: uproc's (the PS line's run/ready/wait/stop) */
    int nice;
    char name[16];
};
int wsb_os_proc(unsigned i, struct wsb_proc *p);   /* the i-th process table slot: 0 a process, 1 a free slot,
                                                    * -EINVAL past the table's end */
int wsb_os_self(void);                             /* the daemon's own pid */

/* -- the machine -- */
int wsb_os_meminfo(uint32_t *chip_free, uint32_t *fast_free);
int wsb_os_uname(char *out, int cap);              /* uname -a's line, without the newline */
struct wsb_iface { char name[8]; uint32_t addr, rx, tx, bad; };   /* SOCK_IFLIST's unetif: addr host order */
int wsb_os_ifaces(struct wsb_iface *t, int max);   /* rows written */
/* SYS_klog as it is: off with WSB_KLOG_PREV (the previous boot's log) and/or WSB_KLOG_OLDEST (answers the offset of
 * the oldest character the ring holds); characters copied */
#define WSB_KLOG_PREV   0x80000000u
#define WSB_KLOG_OLDEST 0x40000000u
int wsb_os_klog(uint32_t off, void *buf, uint32_t n);

/* -- the consoles (D24 6.1-6.3; the main session's kernel ops, root only, -ENXIO for a console never used; the OS
 * layer opens /dev/ttyN for the call and closes it: one descriptor for a moment). con: 1..WSB_NCONS -- */
#define WSB_NCONS 10
int wsb_os_con_ops(void);                          /* 1: the kernel has CON_READ, CON_LINE and CON_INPUT (D24 14 step 3);
                                                    * 0: caps leave out the console commands, which answer ERR */
struct wsb_con_head {                              /* CON_READ's header (6.1), in the core's terms */
    uint16_t rows, cols, crow, ccol;               /* the grid and the cursor */
    uint32_t changes;                              /* raised on every draw */
    uint8_t shown;                                 /* the console on the display */
    uint8_t pcfont;                                /* a PC font is loaded: every glyph is CP437 (D20) */
};
/* the grid: rows x cols cells of 3 bytes (glyph, style bits, colour: foreground low nibble, background high), at
 * most cap bytes; -> 0, *h filled */
int wsb_os_con_read(int con, struct wsb_con_head *h, uint8_t *cells, uint32_t cap);
struct wsb_con_disp {                              /* CON_LINE's answer besides the pens (6.2) */
    uint16_t width, height, depth;                 /* pixels, display lines, planes */
    uint32_t generation;                           /* raised when the depth or the planes change */
    uint8_t rgb[16][3];                            /* the palette as the Copper shows it (pens 0..2^depth-1) */
};
/* one display line of the shown console, one pen a pixel (width bytes, at most cap); -ENXIO if con is not shown */
int wsb_os_con_line(int con, int line, uint8_t *pens, uint32_t cap, struct wsb_con_disp *d);
#define WSB_IN_RAW  0                              /* CON_INPUT's kinds (6.3): raw key codes into the keyboard path */
#define WSB_IN_TEXT 1                              /* text into that console's tty (Latin-1) */
int wsb_os_con_input(int con, int kind, const uint8_t *b, int n);   /* n <= 256: how many were taken */

/* ==== the core's entry points ==== */

/* Arguments as wasabid's (D24 5.3, 7, 4 INSTALL): --init (started by init: RESTART allowed), --trial (a trial
 * instance), --port N (the TCP port, over the configuration's). Reads /etc/wasabid.conf (root's, mode 600, else
 * refused), /var/lib/wasabid.refused and the previous boot's kernel log; opens the listener and the discovery
 * sockets. WSB_GOON (carry on with wsb_step), or the exit status (WSB_ST_STOP for no or a bad configuration, logged). */
int wsb_init(int argc, char **argv);
/* One turn of the loop: WSB_GOON, or the exit status (everything closed, children signalled). */
int wsb_step(void);
/* Free everything (the tests; wasabid simply ends). */
void wsb_end(void);

/* ==== pure parts, for the tests and the fuzzer (no OS layer call) ==== */

/* -- /etc/wasabid.conf (5.7): key TEXT, port N, allow CIDR|any (repeatable), name NAME; # comments -- */
#define WSB_KEYMAX   127                           /* Wasabi's HELLO key buffer */
#define WSB_ALLOWMAX 8                             /* Wasabi's MAX_ALLOW */
#define WSB_NAMEMAX  31
struct wsb_conf {
    char key[WSB_KEYMAX + 1];
    unsigned port;
    int nallow, allow_any;
    uint32_t allow_net[WSB_ALLOWMAX], allow_mask[WSB_ALLOWMAX];
    char name[WSB_NAMEMAX + 1];                    /* the discovery name, default "zenix" */
};
/* the file's bytes -> 0, or -EINVAL with *line the first bad line and *why its reason */
int wsb_conf_parse(struct wsb_conf *c, const char *text, uint32_t n, int *line, const char **why);
/* RFC 1918, loopback, link-local, and the allow lines (allow any: everything) */
int wsb_allowed(const struct wsb_conf *c, uint32_t addr);

/* -- frames: one connection's receiver (3.2, 9) -- */
struct wsb_rx {
    int stream;                                    /* the core's: DATA may be longer than WSB_SMALL (a PUT's or a
                                                    * SPEED's body), handed over in pieces */
    int st;                                        /* the receiver's */
    uint8_t hdr[5];
    uint8_t tag;
    uint32_t len, got;
    uint8_t *buf;                                  /* WSB_SMALL bytes, the caller's (per connection) */
};
#define WSB_RX_NEED  0                             /* everything given was taken; give more */
#define WSB_RX_FRAME 1                             /* a whole frame: *tag, buf, *len */
#define WSB_RX_PIECE 2                             /* a piece of a streamed DATA: *piece, *len; *last at its end */
#define WSB_RX_TLS   (-71)                         /* the first byte is 0x16, a TLS ClientHello: close (-EPROTO) */
#define WSB_RX_BIG   (-90)                         /* longer than 64 KB, or than 4 KB where it may not be (-EMSGSIZE) */
void wsb_rx_init(struct wsb_rx *r, uint8_t *buf);
int wsb_rx_feed(struct wsb_rx *r, const uint8_t *in, uint32_t n, uint32_t *used, uint8_t *tag,
                const uint8_t **piece, uint32_t *len, int *last);

/* a frame's payload fields: u32, u16 and str (u16 length and bytes, Latin-1); 0 or -EINVAL past the end. A string
 * is NUL-terminated into out (cap bytes, a longer one is -ENAMETOOLONG) */
int wsb_get32(const uint8_t *p, uint32_t len, uint32_t *off, uint32_t *v);
int wsb_get16(const uint8_t *p, uint32_t len, uint32_t *off, uint32_t *v);
int wsb_getstr(const uint8_t *p, uint32_t len, uint32_t *off, char *out, uint32_t cap);

/* -- the mapping of files (4 LS, PUT) -- */
/* Unix mode -> AmigaDOS protection bits for LS (the low four active low: d e w r from the owner's w and r, x as e;
 * the group and other bits as AmigaDOS's) */
uint32_t wsb_mode_to_prot(uint32_t mode);
/* PUT's protbits -> the mode to set: 0xFFFFFFFF keeps old (or 0755 for a Zenix program, zexe, else 0644 when new:
 * old 0); bit 31 a Unix mode (caps zenix); else AmigaDOS's bits mapped */
uint32_t wsb_prot_to_mode(uint32_t prot, uint32_t old, int zexe);
/* Unix seconds -> a DateStamp: days since 1978-01-01, minutes, ticks (1/50 s) */
void wsb_datestamp(uint32_t t, uint32_t *days, uint32_t *mins, uint32_t *ticks);
/* an error number -> ERR's u32 code: the nearest AmigaDOS code, so the client's names read right (main session's
 * decision 2: ENOENT 205, EEXIST 203, EACCES/EPERM 223 for a write and 224 for a read, ENOSPC 221, ENOTEMPTY 216,
 * ENOTDIR/EISDIR 212, EBUSY/ETXTBSY 202, EROFS 214; else 0). e: negative or positive; write: 1 a write, 0 a read */
uint32_t wsb_errcode(int e, int write);
/* is this a process name REBOOT's guard stops at (zup, fsck.*, mkfs.*, fdisk, dd; 5.5)? */
int wsb_guarded(const char *name);

#endif
