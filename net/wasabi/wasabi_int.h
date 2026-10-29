/* SPDX-License-Identifier: GPL-2.0-only */
/* wasabid's core, inside (DESIGN D24): the state shared by its files. Not for the OS layer. */
#ifndef WASABI_INT_H
#define WASABI_INT_H

#include <string.h>
#include "wasabi.h"
#include "../../kernel/errno.h"

#ifndef EPROTO
#define EPROTO 71
#endif
#ifndef EMSGSIZE
#define EMSGSIZE 90
#endif
#ifndef ENETDOWN
#define ENETDOWN 100
#endif

#define WSB_OUTCAP WSB_SMALL            /* one connection's send queue: a whole frame of up to 4 KB (header included:
                                         * exactly libz's 4 KB block, not the next size's 8 KB; measured, step 1) */
#define WSB_BG     8                    /* detached commands and let-go ones the daemon still reaps (decision 6) */
#define WSB_BADKEY 8                    /* addresses that sent a wrong key, remembered for the pause */
#define WSB_SPEEDMAX (256u << 20)       /* SPEED: 1 byte to 256 MB, as Wasabi */
#define WSB_SPEEDROOM 65536u            /* SPEED through a file: the size plus this must be free (decision 11) */
#define WSB_TEXTMAX 3600u               /* TEXT's longest wait, seconds */
#define WSB_KEYTEXT 2048                /* KEY's text, as Wasabi */
#define WSB_INPUTS_TICKS 250            /* INPUTS' waits together (5 s), as Wasabi */
#define WSB_REFUSED_MS 60000            /* the refused count is written at most this often (and at the end) */
#define WSB_LSMAX  32768u               /* a directory's names held for LS; a bigger directory: ERR after what fits */
#define WSB_GRID   (128u * 100u * 3u)   /* the largest console grid TEXT reads */

/* the frame receiver's states (struct wsb_rx's st) */
#define RX_HDR   0
#define RX_SMALL 1
#define RX_BODY  2
#define RX_FIRST 3

/* what a connection is doing */
enum { J_NONE, J_PUT, J_SPEEDIN, J_GET, J_SPEEDOUT, J_LS, J_OUT, J_RUN, J_LOG, J_FREE, J_TEXT, J_GRAB, J_INPUTS,
       J_REBOOT, J_EXIT };

struct conn {
    int fd;                             /* -1: free */
    uint32_t peer;
    int hello;                          /* WELCOME sent */
    int closing;                        /* 1: close when the queue is sent; 2: our side shut, the peer's end awaited */
    uint32_t closeby;                   /* closing's deadline */
    uint32_t last;                      /* the last progress, ms */
    uint32_t holdto;                    /* a HELLO held for the wrong-key pause is served then (held != 0) */
    int held;
    struct wsb_rx rx;
    uint8_t *in, *out;                  /* WSB_SMALL and WSB_OUTCAP bytes, per connection */
    uint32_t outn, outoff;              /* the queue: bytes in it, bytes of it sent */
    int job;
    /* the job's */
    int slot;                           /* J_RUN, J_FREE: the slot */
    int xfd;                            /* PUT, GET, SPEED through a file: the file (counted in g.nfd) */
    uint32_t xsize, xgot, xflags, xprot;
    int xerr;                           /* a PUT's or SPEED's error found mid-body: answered at END */
    const char *xwhy;                   /* its words (0: the error number's) */
    char xpath[WSB_PATHMAX + 1], xtmp[WSB_PATHMAX + 1];
    uint8_t xmagic[4];
    char *buf;                          /* J_OUT, J_LS, J_TEXT, J_INPUTS: the core's heap block for the answer */
    uint32_t blen, bpos;
    const char *bend;                   /* J_OUT: after the lines, an ERR with these words (0: END) */
    uint32_t until;                     /* J_FREE, J_TEXT, J_INPUTS, J_EXIT: the next time to act */
    uint32_t lpos, lseq, lhb;           /* J_LOG: the log's position, the frame count, the last heartbeat */
    int lreplay;                        /* J_LOG: the previous boot's lines still to send (offset, -1 done) */
    void *re;                           /* J_TEXT: the compiled pattern */
    int con;                            /* J_TEXT, J_GRAB: the console */
    uint32_t changes, gen;              /* J_TEXT: the change counter last read; J_GRAB: at the start */
    int line, lines;                    /* J_GRAB: the next display line, how many */
    uint8_t mods;                       /* KEY, J_INPUTS: bit m, raw modifier 0x60 + m held (raw_keys) */
};

/* a run slot (5.1) and the children the daemon only reaps */
struct slot {
    int pid;                            /* 0: free */
    int out, err;                       /* the pipes' read ends, -1 closed (err -1 from the start when merged) */
    int conn;                           /* the client's connection, -1 when it went away */
    int ended;                          /* sh was reaped: drain the pipes, then EXIT */
    int status;
    uint32_t started;                   /* ms */
    uint32_t killat;                    /* SIGKILL then (0: none) */
    uint32_t letgo;                     /* after the SIGKILL: let go of it then if it has not ended (0: none) */
    int freer;                          /* the connection waiting in FREE for it, -1 */
    char cmd[48];                       /* for SLOTS */
};
struct kid {
    int pid;                            /* 0: free */
    int stuck;                          /* 1: a slot's command let go of (SLOTS: stuck), 0: a detached one */
    int num;                            /* stuck: the slot it had */
    uint32_t started;
    char cmd[48];
};

struct wsb {
    struct wsb_conf conf;
    int init, trial;                    /* --init, --trial */
    unsigned port;
    int self;
    int nfd;                            /* descriptors in use, counted against WSB_NFD */
    int lfd, ufd[2], nullfd;
    uint32_t netretry;                  /* the listener's next try while the network is down (0: up) */
    struct conn c[WSB_CONNS];
    struct slot s[WSB_SLOTS];
    struct kid k[WSB_BG];
    int xbusy;                          /* the one file transfer is taken */
    uint32_t badaddr[WSB_BADKEY];
    int nbad;
    uint32_t refused, refused_saved, refused_at;
    int logconn;                        /* the debug stream's subscriber, -1 */
    char *replay;                       /* the previous boot's ending and (after a panic) its last lines */
    uint32_t nreplay;
    char cpu[40];                       /* the kernel log's "cpu: " line */
    int emu68;                          /* -1 unknown, 0 no, 1 yes (the "pi:" line) */
    char uname[96], banner[96], caps[sizeof WSB_CAPS];
    int conops;
    uint32_t bgseq, tmpseq;
    int quit;                           /* -1 running; else the exit status once everything is closed */
    uint32_t quitby;
    int rebootpend, rebootarg;          /* REBOOT answered: /bin/reboot starts when the others are closed, or then */
    uint32_t rebootby;
    int rebootpid, rebootout, rebootconn, rebootst, rebootdone;
    char rtext[200];                    /* what /bin/reboot said if it came back */
    uint32_t rtn;
    uint32_t now;
};
extern struct wsb g;

/* text into a buffer, never past cap (the core's small formatter: no printf on either side) */
struct wsb_buf { char *p; uint32_t n, cap; };
void bs(struct wsb_buf *b, const char *s);
void bsn(struct wsb_buf *b, const char *s, uint32_t n);
void bu(struct wsb_buf *b, uint32_t v);
void bi(struct wsb_buf *b, int v);
void bx8(struct wsb_buf *b, uint32_t v);          /* 0x%08x */
void bc(struct wsb_buf *b, char ch);
void bip(struct wsb_buf *b, uint32_t a);          /* a.b.c.d */
void put32(uint8_t *p, uint32_t v);
void put16(uint8_t *p, uint32_t v);

/* core.c: the send queue and answers */
uint32_t qroom(struct conn *c);                    /* payload bytes a frame may have now */
int q(struct conn *c, uint8_t tag, const void *p, uint32_t n);    /* 0, or -1 no room */
uint8_t *qres(struct conn *c, uint32_t max);       /* room for a frame's payload of up to max bytes, 0 none */
void qcommit(struct conn *c, uint8_t tag, uint32_t n);
int qerr(struct conn *c, int e, int write, const char *words);    /* ERR: the code from e, words then e's words */
int qok(struct conn *c);
int qend(struct conn *c);
int qlines(struct conn *c, char *buf, uint32_t n, const char *errwords);  /* J_OUT: buf (heap) as DATA, END/ERR */
void conn_drop(int i);
void conn_jobdone(struct conn *c);
int fd_take(int n);                                /* n descriptors of the budget: 0, or -EMFILE */
void fd_give(int n);
void say(const char *a, const char *b);           /* a log line "wasabid: A B" */
int path_ok(const char *path);                     /* absolute and at most WSB_PATHMAX: 0 or -errno */
int on_nfs(const char *path);                      /* 1: the resolved path lies on an nfs mount */
int mount_of(const char *path, int *nfs);          /* its mount's index (wsb_os_mount's), -1 none */

/* files.c */
void cmd_put(struct conn *c, const uint8_t *p, uint32_t len);
void cmd_get(struct conn *c, const uint8_t *p, uint32_t len);
void cmd_ls(struct conn *c, const uint8_t *p, uint32_t len);
void cmd_del(struct conn *c, const uint8_t *p, uint32_t len, int dir);
void cmd_speed(struct conn *c, const uint8_t *p, uint32_t len);
void body_piece(struct conn *c, const uint8_t *b, uint32_t n, int last);
void body_end(struct conn *c);
void produce_files(struct conn *c);
void xfer_abort(struct conn *c);

/* run.c */
void cmd_run(struct conn *c, const uint8_t *p, uint32_t len);
void cmd_slots(struct conn *c);
void cmd_free(struct conn *c, const uint8_t *p, uint32_t len);
void cmd_ps(struct conn *c);
void cmd_kill(struct conn *c, const uint8_t *p, uint32_t len);
void cmd_reboot(struct conn *c, const uint8_t *p, uint32_t len);
void cmd_exit(struct conn *c, const uint8_t *p, uint32_t len, int restart);
int run_poll(struct wsb_pollfd *f, int *who, int n, uint32_t *tmo);
void run_ready(int k, int who, short rev);
void run_tick(uint32_t *tmo);
void run_conn_gone(int ci);
void run_stopall(void);
int runs_busy(void);
void tree_kill(int pid, int sig);

/* info.c */
void cmd_info(struct conn *c);
void cmd_health(struct conn *c);
void cmd_debug(struct conn *c);
void log_scan(void);
void produce_log(struct conn *c);
void log_tick(uint32_t *tmo);
void farewell(const char *why);

/* screen.c */
void cmd_text(struct conn *c, const uint8_t *p, uint32_t len);
void cmd_grab(struct conn *c, const uint8_t *p, uint32_t len);
void cmd_sum(struct conn *c, const uint8_t *p, uint32_t len);
void cmd_screen(struct conn *c, const uint8_t *p, uint32_t len);
void cmd_windows(struct conn *c);
void cmd_key(struct conn *c, const uint8_t *p, uint32_t len);
void cmd_inputs(struct conn *c, const uint8_t *p, uint32_t len);
void produce_screen(struct conn *c);
void screen_tick(struct conn *c, uint32_t *tmo);
void screen_free(struct conn *c);

#define TMO(t, ms) do { if ((uint32_t)(ms) < *(t)) *(t) = (uint32_t)(ms); } while (0)
#define LEFT(when) ((int32_t)((when) - g.now) > 0 ? (uint32_t)((when) - g.now) : 0u)
#define DUE(when) ((int32_t)(g.now - (when)) >= 0)

#endif
