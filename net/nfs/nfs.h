/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * nfscd's core (DESIGN D23, docs/d23-nfs.md, step 0 of section 13): the NFS client of one mount, without system
 * calls. XDR, ONC RPC over TCP with record marking (RFC 5531), AUTH_SYS, portmapper GETPORT (RFC 1833), MOUNT v3 and
 * NFSv3 (RFC 1813); the handle table and inode numbers (4.4), the attribute, name and data caches (6), the directory
 * positions (6.3), the requests as small state machines (4.3, 4.5), the unsaved buffer with UNSTABLE writes, COMMIT
 * and the write verifier (7), the request and reconnect rules (3.4, 5.3) and the cancel (5.2).
 *
 * The same files build for /sbin/nfscd (its OS layer: the port, the socket, evwait; D23 13 step 2, the main
 * session's) and for the PC tests and fuzzing (tools/host/nfs-test.sh). One mount per process: the core's state is
 * its own (static), set up by nfs_init.
 *
 * The OS layer's loop (4.3): evwait on the port (POLLPORT | POLLIN | POLLPRI) and the socket (POLLIN, and POLLOUT
 * while nfs_poll says NFS_P_OUT), with nfs_poll's timeout. Then: bytes from the socket -> nfs_input; a finished
 * connect -> nfs_connected; up to 8 requests from the port -> nfs_request (while nfs_poll says NFS_P_TAKE); on POLLPRI
 * the held requests whose message has UMSG_CANCEL -> nfs_cancel; and always nfs_tick. NFS_P_DONE: end the process.
 * The OS layer never calls the core from inside one of the callbacks below.
 */
#ifndef NFS_H
#define NFS_H

#include <stdint.h>

/* ---- what the OS layer gives the core (D23 11.1; nfs_connect/nfs_disconnect/nfs_save added, see below) ---- */
uint32_t nfs_now(void);                          /* milliseconds, any start, may wrap */
long nfs_send(const void *buf, long n);          /* bytes for the socket: how many it took (0..n), < 0 broken */
void nfs_log(const char *text);                  /* one line for the kernel log (no newline) */
void *nfs_alloc(uint32_t n);                     /* the counted heap with the profile's cap: 0 when full */
void nfs_free(void *p);
void nfs_answer(uint32_t id, long result);       /* the request id's answer; the core never touches its buffers after */
/* One TCP connection at a time, to the mount's host (resolved by the OS layer at each connect, D23 4.1) on PORT,
 * from the mount's privileged source port with SO_REUSEADDR (3.2, 3.4: the same port on every reconnect), the socket
 * O_NONBLOCK. 0: started (nfs_connected follows later), < 0: failed at once. nfs_disconnect: close by abort (no
 * TIME_WAIT), at once; nothing more about that connection reaches the core. */
int nfs_connect(int port);
void nfs_disconnect(void);
/* D11's saved table (SVC_SAVE): the export's root, at the first contact (4.1); at most NFS_SAVEMAX bytes */
void nfs_save(const void *buf, uint32_t n);

/* ---- the core's entry points ---- */
#define NFS_NGROUPS 16                           /* supplementary groups in AUTH_SYS (3.3; RFC 5531 allows 16) */
#define NFS_SAVEMAX 80                           /* the saved table's record (of D11's 512) */
#define NFS_REQ_KERNEL 1                         /* kflags: UMSG_KERNEL (the kernel sent it for a system call) */
#define NFS_REQ_CANCEL 4                         /* kflags: UMSG_CANCEL (D23 12, proposed value 4) */

struct nfs_req {                                 /* a Zenix request: the message's fields, pointers as they are */
    uint32_t id;                                 /* the OS layer's id for it (the message); nfs_answer gives it back */
    uint32_t type;                               /* FS_* (kernel/fsproto.h) */
    uintptr_t a[4];                              /* the arguments: numbers, or pointers into the client's memory */
    uint16_t uid, gid, umask, kflags;            /* the kernel's stamp */
    uint16_t ngroups;                            /* the caller's supplementary groups (/etc/group, read at the start) */
    uint32_t groups[NFS_NGROUPS];
};

struct nfs_conf {
    const char *dev;                             /* "nfs0": struct fsinfo's device, every log line's prefix */
    const char *export_path;                     /* "/volume1/zenix": the export (the OS layer keeps the host) */
    const char *codes;                           /* the option codes word of D23 4.1, or 0 for the defaults */
    int large;                                   /* the large profile (netd's rule, 9.2) */
    uint32_t cache_bytes;                        /* large profile: data cache + unsaved cap (an eighth of free fast
                                                  * RAM, at most 1 MB; 0: 256 KB) */
    uint32_t seed;                               /* random: the first xid, the create verifiers */
    const void *saved; uint32_t nsaved;          /* SVC_LOAD's record from an earlier start, or 0 */
    const uint32_t *opens; int nopens;           /* SVC_OPENS: the inode numbers the kernel holds on this mount */
};

/* the option codes (4.1), parsed by nfs_init; nfs_codes_parse is also mount's checker on the PC side */
struct nfs_opts {
    int soft;                                    /* s (default h: hard) */
    uint32_t timeo_ms;                           /* tN: seconds (default 30) */
    int retrans;                                 /* rN (default 3) */
    int ro;                                      /* o */
    int sync;                                    /* y: FILE_SYNC writes */
    int actimeo;                                 /* aN: every attribute cache time N seconds; -1 the defaults */
    int lazy;                                    /* l: FS_ROOT answered without the network */
};
int nfs_codes_parse(const char *codes, struct nfs_opts *o);     /* 0, or -EINVAL for a code it does not know */

int nfs_init(const struct nfs_conf *c);          /* 0, or -ENOMEM / -EINVAL; frees an earlier instance first */
void nfs_request(const struct nfs_req *r);       /* a request in: answered now or later through nfs_answer */
void nfs_input(const void *buf, long n);         /* n > 0 bytes from the socket; 0 the other side closed; < 0 broken */
void nfs_connected(int err);                     /* the connect started by nfs_connect: 0 up, < 0 failed */
void nfs_tick(void);                             /* timers and sending; call after every wake-up */
void nfs_cancel(uint32_t id);                    /* the held request's message has UMSG_CANCEL (5.2) */
void nfs_kill(void);                             /* nfscd was killed: answer everything -EIO, log, NFS_P_DONE (4.3) */
void nfs_end(void);                              /* free everything (the tests; nfscd simply ends) */

#define NFS_P_OUT  1                             /* poll the socket for POLLOUT: bytes wait to be sent */
#define NFS_P_TAKE 2                             /* the core takes new requests (a free slot) */
#define NFS_P_DONE 4                             /* FS_QUIT done, or killed: end the process */
#define NFS_P_CONN 8                             /* a connection is up or being made */
int nfs_poll(uint32_t *timeout_ms);              /* the bits above; *timeout_ms: until the next timer (~0: none) */

/* counters for the tests and the log */
struct nfs_stats {
    uint32_t calls, replies, resends, reconnects, timeouts, unsaved, dropped_late, ops, nodes, mem;
};
void nfs_get_stats(struct nfs_stats *s);

#endif
