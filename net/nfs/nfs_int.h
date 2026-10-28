/* SPDX-License-Identifier: GPL-2.0-only */
/* nfscd's core, inside (DESIGN D23): the structs and the calls between its files. Not for the OS layer. */
#ifndef NFS_INT_H
#define NFS_INT_H

#include <string.h>
#include "nfs.h"
#include "../../kernel/fsproto.h"
#include "../../kernel/errno.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;
typedef uint64_t u64;

/* D23 12: ESTALE (Linux's 116) is still to be claimed by the main session; until then -EIO */
#ifdef ESTALE
#define NFS_ESTALE ESTALE
#else
#define NFS_ESTALE EIO
#endif

/* the open flags FS_OPEN carries (user/usys.h, frozen; O_EXCL and O_EXEC are in fsproto.h) */
#ifndef O_WRONLY
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#endif

/* struct ustat and struct udirent (user/usys.h; frozen: 28 and 72 bytes): the same layout, written by the core */
struct nfs_ustat { u32 ino, mode, size, mtime, uid, gid, links; };
struct nfs_udirent { u32 ino, type; char name[64]; };

/* ---- XDR (RFC 4506): a cursor, every read checked against the end ---- */
struct xdr { u8 *p, *end; int bad; };
u32 x_u32(struct xdr *x);
u64 x_u64(struct xdr *x);
const u8 *x_fixed(struct xdr *x, u32 n);                 /* n bytes and their padding; 0 when short */
const u8 *x_opaque(struct xdr *x, u32 max, u32 *len);    /* a counted opaque of at most max bytes */
void x_put32(struct xdr *x, u32 v);
void x_put64(struct xdr *x, u64 v);
void x_putfixed(struct xdr *x, const void *b, u32 n);
void x_putopaque(struct xdr *x, const void *b, u32 n);

/* ---- the RPC programs ---- */
#define PG_PMAP  0                       /* portmapper v2, port 111 */
#define PG_MOUNT 1                       /* MOUNT v3, the port from GETPORT */
#define PG_NFS   2                       /* NFSv3, port 2049 */
#define PMAP_PROG 100000u
#define MOUNT_PROG 100005u
#define NFS_PROG 100003u
#define PMAP_PORT 111
#define NFS_PORT 2049

/* NFSv3 procedures (RFC 1813) and the two others */
#define P3_NULL 0
#define P3_GETATTR 1
#define P3_SETATTR 2
#define P3_LOOKUP 3
#define P3_ACCESS 4
#define P3_READLINK 5
#define P3_READ 6
#define P3_WRITE 7
#define P3_CREATE 8
#define P3_MKDIR 9
#define P3_SYMLINK 10
#define P3_REMOVE 12
#define P3_RMDIR 13
#define P3_RENAME 14
#define P3_LINK 15
#define P3_READDIR 16
#define P3_READDIRPLUS 17
#define P3_FSSTAT 18
#define P3_FSINFO 19
#define P3_COMMIT 21
#define PM_GETPORT 3
#define MNT_MNT 1
#define MNT_UMNT 3

#define NFS3_OK 0
#define NFS3ERR_NOENT 2
#define NFS3ERR_EXIST 17
#define NFS3ERR_NOTSUPP 10004
#define NFS3ERR_BAD_COOKIE 10003
#define NFS3ERR_JUKEBOX 10008
#define NFS3ERR_STALE 70
#define NFS3ERR_BADHANDLE 10001

#define NF3REG 1
#define NF3DIR 2
#define NF3BLK 3
#define NF3CHR 4
#define NF3LNK 5
#define NF3SOCK 6
#define NF3FIFO 7

#define AC_READ 1                        /* ACCESS3 bits */
#define AC_LOOKUP 2
#define AC_MODIFY 4
#define AC_EXTEND 8
#define AC_DELETE 16
#define AC_EXECUTE 32

#define FHMAX 64                         /* NFS3_FHSIZE */
#define NAMEMAX 255                      /* a name from the NAS (10: longer fails the reply) */

struct fattr {                           /* fattr3, what Zenix uses of it */
    u32 type, mode, nlink, uid, gid;
    u64 size, fileid;
    u32 mtime, mtime_ns, ctime, ctime_ns;
};

struct sattr {                           /* sattr3 for SETATTR, CREATE, MKDIR, SYMLINK */
    u8 set_mode, set_uid, set_gid, set_size, atime_how, mtime_how;   /* how: 0 keep, 1 server's time, 2 given */
    u32 mode, uid, gid, atime, mtime;
    u64 size;
};

/* a decoded reply: valid while the reply's record is in the receive buffer (data, dir point into it) */
struct res {
    int err;                             /* 0, or -errno (the NFS status mapped, or the transport's) */
    u32 status;                          /* the NFS status as it came (0 when err is the transport's) */
    u8 has_attr, has_dattr, has_pre, has_fh;
    struct fattr attr;                   /* the object's attributes after the call */
    struct fattr dattr;                  /* the directory's after (LOOKUP's dir, wcc's after) */
    u64 pre_size; u32 pre_mtime, pre_mtime_ns, pre_ctime, pre_ctime_ns;   /* wcc's before */
    u8 fhlen; u8 fh[FHMAX];
    u32 count, committed, access;
    int eof;
    const u8 *data;                      /* READ's bytes, READLINK's text: count bytes */
    u8 verf[8];                          /* WRITE's or COMMIT's verifier, READDIR's cookie verifier */
    struct xdr dir;                      /* READDIR(PLUS): the entries, from the first value_follows */
    u64 tbytes, abytes, tfiles, afiles;  /* FSSTAT */
    u32 rtmax, rtpref, wtmax, wtpref, dtpref;   /* FSINFO */
    u32 port;                            /* GETPORT */
};

/* ---- the handle table (4.4) ---- */
#define N_NONE 0xFFFFu
#define NF_ATTR 1                        /* attr is set */
#define NF_STALE 2                       /* a number the kernel held at a restart (4.4 rule 5): -ESTALE */
#define NF_ROOT 4
#define NF_REF 8                         /* referenced since the clock hand passed */
#define NF_USED 16                       /* the slot holds a file */
#define NF_COMMIT 32                     /* a commit is wanted (the last close, 7.3) */
struct node {
    u32 ino;
    u64 fileid;
    u8 fhlen, flags;
    u16 gen;                             /* bumped when the slot is given to another file */
    u16 opens, pins;                     /* the kernel's FS_OPENs, requests that depend on it */
    u16 namegen;                         /* the names cached in it as a directory are valid while it is the same */
    u16 unsaved;                         /* pieces of unsaved data (7.1) */
    u8 acc_uid_ok; u8 acc;               /* the last ACCESS answer: for acc_uid, valid with the attributes */
    u8 cin, spare;                       /* a COMMIT is out */
    u16 acc_uid;                         /* whose: the credential slot and its generation */
    u32 attr_at, acto;                   /* when the attributes were fetched; how long they stay fresh (ms) */
    u32 commit_at;                       /* the commit timer (7.2), 0 none */
    struct fattr a;
    u8 fh[FHMAX];
};

/* ---- the requests nfscd holds (4.3) ---- */
#define WALK_FOLLOW 1                    /* follow a final symbolic link */
#define WALK_PARENT 2                    /* stop at the last name's directory: node = that directory, name = the name */
struct walk {
    u16 cur;                             /* the directory walked (pinned) */
    u16 pos, nlen;                       /* the path's next name in buf / the last name found (WALK_PARENT) */
    u8 depth, flags, linked, busy;       /* symbolic links followed; flags WALK_*; a link was followed; waiting */
    u16 node;                            /* the result (pinned) */
    char buf[FS_PATHMAX + 1];
};

struct op {
    u32 id, type;
    uintptr_t a[4];
    u8 used, step, cred, flags;          /* flags: OPF_* */
    u16 uid, gid, umask;
    int err;
    long done;                           /* bytes read or written so far, entries, ... */
    u32 deadline;                        /* a bounded wait (FS_ROOT 10 s, SYNC 2 s, STATFS 2 s ...), 0 none */
    u32 started;
    u16 pins[6]; u8 npins;
    u8 inflight;                         /* its calls not answered yet */
    u16 node, node2;                     /* the request's target(s) */
    struct walk w[2];
    u8 lwalk;                            /* D26: 1 + the walk whose buf holds the FS_LINKED TEXT, 0 none */
    struct sattr sa;
    u8 cverf[8];                         /* CREATE EXCLUSIVE's verifier */
    u32 pos;                             /* READDIR: the position asked; SYNC: the sequence it covers */
    u32 rb_base, rb_next, rb_mask;       /* READ: the blocks (ops.c read_go) */
    u64 rb_end;
    u32 wlen, wok, werr_at;              /* WRITE: bytes copied; answered; the first failed piece's offset */
};
#define OPF_FRESH 1                      /* its node's attributes were fetched in this request */
#define OPF_WAITMOUNT 2                  /* waits for the mount (lazy) */
#define OPF_WAITROOM 4                   /* a WRITE waiting for room in the unsaved buffer */
#define OPF_CANCEL 8
#define OPF_LINKS 16                     /* D26: the kernel takes FS_LINKED (UMSG_LINKS): no link is followed */
#define OPF_RO 32                        /* D26: a read-only mount's refusal waits until the walk found no link */

/* ---- the calls on the wire (3.4) ---- */
#define C_FREE 0
#define C_WAIT 1                         /* to be sent (again) */
#define C_SENT 2
#define C_LATER 3                        /* JUKEBOX: sent again at jb_at */
struct call {
    u32 xid, seq;
    u8 state, prog, proc, purpose;       /* purpose: the op's step it belongs to, or CP_* */
    s16 op;                              /* the op waiting for it, -1 none (cancelled, or background) */
    s16 piece;                           /* WRITE: the unsaved piece */
    u16 node, node2;
    u8 cred, tmo, brk, jb;               /* credentials; timeouts, breaks, JUKEBOX retries counted */
    u8 walk;                             /* the op's walk it belongs to (0/1) */
    u8 fsync, opref;                     /* FILE_SYNC; counted in its op's inflight */
    u32 sent_at, deadline, jb_at;
    u64 off;
    u32 count;
    u8 verf[8];                          /* READDIR's cookie verifier */
    const char *name, *name2;            /* in the op's walk buffers */
    u16 nlen, nlen2;
    u64 cookie;
    u32 cseq;                            /* COMMIT: the write sequence it covers */
};
#define CP_MOUNT 200                     /* the mount's own calls (no op) */
#define CP_PIECE 201                     /* an unsaved piece's WRITE */
#define CP_COMMIT 202
#define CP_UMNT 203

/* ---- the unsaved buffer (7.1) ---- */
#define PS_COPY 1                        /* bytes are being copied in (waits for room) */
#define PS_SEND 2                        /* its WRITE is out */
#define PS_WRITTEN 3                     /* answered UNSTABLE: kept until a COMMIT with the same verifier */
struct piece {
    u8 state, cred, resend;
    s16 op;                              /* the FS_WRITE waiting for it, -1 */
    u16 node;
    u32 off, len, opoff;                 /* opoff: where it starts in its FS_WRITE */
    u32 cseq;                            /* made when (the order of pieces) */
    u32 wseq;                            /* the write's answer number (a COMMIT sent after covers it) */
    u8 verf[8];
    u8 *data;
};

/* ---- credentials (3.3): AUTH_SYS bodies, shared by refcount ---- */
struct cred { u16 refs, uid, gid, n, gen; u32 g[NFS_NGROUPS]; };   /* gen: bumped when the slot is filled again */

/* ---- the whole state ---- */
struct prof {
    u32 rsize, wsize, nnodes, nrdt, ndc, cap, nops, ncalls, npieces, ncreds, nnames;
};

/* the name cache (6.1): 4-way sets, names up to NC_NAME bytes (longer ones are looked up every time) */
#define NC_NAME 27
struct nament { u16 dir, dgen, dng, child, cgen; u8 len, age; char name[NC_NAME]; };

/* readdir positions (6.3) */
#define RD_STARTS 24
#define RD_CHUNK 32
struct rdt {
    u16 node, gen; u32 lru;
    u8 verf[8];
    u32 ctime, ctime_ns;
    u8 n, logged;
    u32 pos[RD_STARTS]; u64 cookie[RD_STARTS];
};
struct chunk {                           /* the entries of the last chunk listed */
    u16 node, gen; s16 rdt;
    u32 start; u16 n; u8 eof, valid;
    struct nfs_udirent e[RD_CHUNK];
};

/* the data cache (6.2) */
struct dpiece { u16 node, gen; u32 blk, len, ctime, ctime_ns; u64 size; u32 lru; u8 *data; };

struct nfs {
    struct prof pf;
    struct nfs_opts o;
    char dev[16];
    char exp[FS_PATHMAX + 1];
    u32 xid, seq, wseq, lru, cverf;
    u32 seed;
    /* the mount */
    int mstate;                          /* MS_* */
    int merr;
    u8 rootfh[FHMAX]; u8 rootfhlen;
    u8 saved_ok; u8 savedfh[FHMAX]; u8 savedfhlen; u64 savedfileid;
    u32 mport;                           /* mountd's port from GETPORT, 0 unknown */
    u32 rsize, wsize, dtpref, rshift;
    int o_large;
    int noplus;                          /* the NAS refused READDIRPLUS */
    int statfs_ok; struct nfs_ustat_fs { u32 bsize, blocks, bfree, files, ffree; } sfs;
    /* the connection */
    int cstate;                          /* CS_* */
    int cport;
    u32 cstart, backoff, retry_at;
    int notans;                          /* "not answering" (5.3) */
    int quitting, done, killed;
    /* receiving (record marking) */
    u8 *rx; u32 rxcap, rxn;              /* the record so far */
    u32 fraglen; u8 hdr[4]; u8 hdrn; u8 last; u8 skipping; u32 skipxid; u32 skipped;
    /* sending */
    u8 *tx; u32 txcap, txhead, txtail;
    /* tables */
    struct node *nodes; u16 *inoh, *fidh; u32 hmask; u32 hand;
    struct nament *names;
    struct rdt *rdts; struct chunk *chunk;
    struct dpiece *dc;
    struct op *ops; struct call *calls; struct piece *pieces; struct cred *creds;
    u32 unsaved_bytes;
    u32 mem;
    struct nfs_stats st;
    u16 root;                            /* the root's node */
    u32 quit_at;                         /* FS_QUIT: when its commits give up */
    s16 quitop;
    u8 logged_full;
};
extern struct nfs N;

#define MS_NONE 0                        /* never contacted (lazy) */
#define MS_GOING 1                       /* the mount's calls are out */
#define MS_UP 2
#define MS_FAILED 3                      /* refused: merr says why */
#define CS_DOWN 0
#define CS_CONNECTING 1
#define CS_UP 2

/* ---- calls between the files ---- */
/* rpc.c */
int call_new(int prog, int proc, int op, int purpose, int cred);   /* a free call (C_WAIT), or -1 */
void call_free(int c);
void rpc_kick(void);                     /* connect, encode, send what can go */
void rpc_input(const u8 *b, long n);
void rpc_broken(int err);
void rpc_timers(u32 now);
u32 rpc_next(u32 now);
int rpc_busy(int prog);                  /* calls of that program waiting or out */
void rpc_drop_op(int op);                /* the op ended: its calls answer nobody */
/* proto.c */
int enc_args(struct call *c, struct xdr *x);
int dec_res(struct call *c, struct xdr *x, struct res *r);
int dec_dirent(struct xdr *x, int plus, u64 *fileid, const u8 **name, u32 *nlen, u64 *cookie, struct res *r);
int nfs_errno(u32 status);
/* table.c */
int tab_init(void);
struct node *nd(u16 i);
int node_find(u32 ino);                  /* the node of an inode number, -1 */
int node_get(const u8 *fh, u32 fhlen, const struct fattr *a);   /* find or add the file; -1 when the table is full */
void node_attr(u16 i, const struct fattr *a);   /* fresh attributes (invalidates what changed) */
int node_fresh(u16 i);
void node_pin(u16 i);
void node_unpin(u16 i);
void node_ustat(u16 i, struct nfs_ustat *st);
void node_stale_add(u32 ino);
void node_dirchanged(u16 i);
void node_staled(u16 i);
void node_wcc(u16 i, const struct res *r, int ours);   /* a directory's or file's attributes after our own change */
int name_get(u16 dir, const char *name, u32 len);      /* a cached child, -1 */
void name_put(u16 dir, const char *name, u32 len, u16 child);
void name_del(u16 dir, const char *name, u32 len);
u32 ino_for_fileid(u64 fileid);          /* readdir without a handle */
/* data cache and readdir tables (table.c) */
int dc_find(u16 node, u32 blk);
int dc_slot(void);                       /* a free or the oldest piece; -1 when none can be had */
void dc_drop(u16 node);
int rdt_find(u16 node, int make);
/* ops.c */
void op_start(int o);
void op_reply(int o, struct call *c, struct res *r);
void op_finish(int o, long result);
void ops_mounted(void);
void ops_timers(u32 now);
u32 ops_next(u32 now);
void ops_notanswering(void);
void ops_poke(void);
/* write.c */
int wr_start(int o);                     /* FS_WRITE: copy, send */
void wr_piece_reply(struct call *c, struct res *r);
void wr_commit_reply(struct call *c, struct res *r);
void wr_cancel(int o);
void wr_timers(u32 now);
u32 wr_next(u32 now);
int wr_commit_all(int bounded);          /* COMMIT every file with written data; how many pieces remain */
int wr_node_busy(u16 node);              /* pieces of the node in flight */
void wr_drop_node(u16 node);             /* O_TRUNC: the node's unsaved data is gone */
void wr_room(void);                      /* room came: waiting writes go on */
void wr_closed(u16 node);
void wr_reset(void);
int wr_resend_all(void);                 /* a changed verifier: everything written again */
/* core.c */
void *zalloc(u32 n);
void zfree(void *p, u32 n);
int cred_get(const struct nfs_req *r);   /* an interned credential, -1 when the table is full */
void cred_put(int c);
void lg(const char *a, const char *b, long n, const char *c);   /* "nfs0: " a b n c */
void answer_ops_err(int err);
int mount_kick(void);                    /* start the mount if needed: 1 up, 0 going, < 0 failed */
void mount_reply(struct call *c, struct res *r);
void umount_reply(struct call *c, struct res *r);
void core_notanswering(int on);
u32 tdiff(u32 later, u32 earlier);
int due(u32 t, u32 now);

#endif
