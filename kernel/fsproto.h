/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include "errno.h"
/* Messages between the kernel and the filesystem server ("fs" port).
 * type = op, a[0..3] = arguments, result = a value >= 0 or -errno (kernel/errno.h; -EIO: the server is gone), or, only
 * for a sender that set UMSG_LINKS, the FS_LINKED answer below.
 * All pointers are plain addresses: no MMU, the server reads and writes
 * the caller's buffers in place.
 * Who may send them: the kernel (struct umsg kflags UMSG_KERNEL, set only by ipc_kcall for a system call, with the
 * caller's ids) and root. Any other message - a user's own SYS_call to the port - is answered -13: inode numbers
 * (FS_READ, FS_WRITE, ...) and FS_QUIT would bypass every permission check (audit D). */
/* One path limit (audit E; docs/architecture.md section 7, "Paths"): a path given to a system call, an absolute,
 * normalised path - what the kernel builds from the working directory and a relative path, and what a server may be
 * handed - and the working directory text are each at most FS_PATHMAX - 1 (255) bytes. A longer one is refused with
 * -ENAMETOOLONG, never cut. It only grows (it was 160): a server built with a smaller number refuses what does not fit
 * in its own buffers and writes less than this into the kernel's. A newer server needs a kernel at least as new (its
 * text could overrun an older kernel's 160). D15 gives that, with one exception it accepts: "Zenix (previous)" with zup
 * failing leaves a newer root under the older kernel, and then a link whose text passes 159 bytes could overrun. */
#define FS_PATHMAX 256
#define FS_OPEN    1   /* a0 path, a1 cwd ino, a2 flags (O_*), a3 ustat* -> ino */
#define FS_READ    2   /* a0 ino, a1 offset, a2 buffer, a3 length -> bytes */
#define FS_WRITE   3   /* a0 ino, a1 offset, a2 buffer, a3 length -> bytes */
#define FS_STAT    4   /* a0 path, a1 cwd ino, a2 ustat* -> 0 */
#define FS_FSTAT   5   /* a0 ino, a1 ustat* -> 0 */
#define FS_READDIR 6   /* a0 ino, a1 cookie, a2 udirent*, a3 u32* new cookie -> 1, 0 at end */
#define FS_CHDIR   7   /* a0 path, a1 cwd ino -> ino of the directory */
#define FS_MKDIR   8   /* a0 path, a1 cwd ino -> 0 */
#define FS_UNLINK  9   /* a0 path, a1 cwd ino */
#define FS_RMDIR   10  /* a0 path, a1 cwd ino */
#define FS_RENAME  11  /* a0 from, a1 to, a2 cwd ino */
#define FS_STATFS  12  /* a0 struct ustatfs* -> 0 */
#define FS_SYNC    13  /* everything written so far is on the medium when this returns (DISK_FLUSH of the device).
                        * a0 (D23 5.4, 0 from every older caller): 1 "bounded": answer within 2 s with what could be saved
                        * (a network mount at a reboot or the reset warning; local servers may ignore it); any other value
                        * as 0. A bounded sync is never cut short by UMSG_CANCEL */
#define FS_CHMOD   14  /* a0 path, a1 cwd ino, a2 mode */
#define FS_LINK    15  /* a0 existing path, a1 new path, a2 cwd ino: a hard link */
#define FS_SYMLINK 16  /* a0 target text, a1 link path, a2 cwd ino */
#define FS_READLINK 17 /* a0 path, a1 cwd ino, a2 buffer, a3 max -> length */
#define FS_UTIMES  18  /* a0 path, a1 cwd ino, a2 modification time, or FS_UTIME_NOW: an explicit time only for the owner and
                        * root; "now" also for anyone who may write the file */
#define FS_UTIME_NOW 0xFFFFFFFFu
#define FS_CHOWN   20  /* a0 path, a1 cwd ino, a2 uid, a3 gid (-1 = keep): root only */
#define O_EXEC     0x8000  /* FS_OPEN flag: the caller wants to execute the file (needs an x bit) */
#define O_EXCL     0x80    /* FS_OPEN flag with O_CREAT: fail with -17 (EEXIST) if the name exists, also as a symbolic link
                            * (dangling or not); the check and the creation are one request, so no one can come between */
#define FS_LSTAT   19  /* like FS_STAT but a final symbolic link is not followed */
#define FS_ROOT    22  /* a0 struct fsinfo* (the server fills in its device and type) -> inode number of its root directory */
#define FS_QUIT    23  /* sync, release the device, reply 0, exit: sent by umount */
#define FS_CLOSE   24  /* a0 ino: the kernel let go of an inode FS_OPEN gave it (one per successful FS_OPEN). An inode
                        * unlinked while open is freed at its last FS_CLOSE, not at the unlink */
#define FS_MARK    25  /* a0 1: mark the volume "needs a check" (ext2: s_state's clean bit cleared and kept so), 0: the
                        * mark taken back; -> the state before (1 it needed no check, 0 it did) or -errno. Root only (raw
                        * IPC); zup marks the root while it installs (DESIGN D15). fsd's own short mark while it writes
                        * (D17) is not "a need for a check", and 0 never clears a real one (a crash in a request, a failed
                        * write). ZenixFS commits atomically: its server does not know it (-ENOSYS), and zup goes on */
struct fsinfo { char dev[16]; char type[8]; };

/* DESIGN D26 (symbolic links across mounts, docs/d26-links-across-mounts.md 4; append-only, frozen):
 * UMSG_LINKS, a bit of struct umsg's kflags (1 UMSG_KERNEL, 2 UMSG_NETREPLY internal, 4 UMSG_CANCEL D23, 8 this):
 * "the sender takes the FS_LINKED answer, and its path arguments are kernel FS_PATHMAX buffers holding absolute,
 * normalised paths". Only the kernel sets it (ipc_kcall's kflags); a program's SYS_call never carries it.
 * With it, a server follows no symbolic link: at the first one its walk would follow (in the middle of a path, or the
 * last name of a request that follows it: FS_OPEN, FS_STAT, FS_CHDIR, FS_CHMOD, FS_CHOWN, FS_UTIMES, FS_LINK's
 * existing name) it answers -(FS_LINKED + FSL_* bits) and writes TEXT, "this path continues at TEXT", NUL-terminated,
 * at most FS_PATHMAX bytes, into that path argument's buffer. From the request's original path P, the link's name
 * P[S..E) and its target T: an absolute T gives T + P[E..], a relative one P[1..S) + T + P[E..] (from the volume's
 * root, without the leading /). The server does not normalise it. An empty T is -ENOENT, a NUL in T -EIO, T over 255
 * bytes or a TEXT too long -ENAMETOOLONG. The decision comes before anything else of the request (fsd: before its
 * read-only refusal and its clean mark): a bounced request changes nothing. A server answers it only when the bit is
 * set and P starts with /; without the bit it follows links inside its volume as always (old kernels, raw requests).
 * The kernel turns an FS_LINKED answer it did not ask for into -EIO (vfs.c fs_call_on). */
#define UMSG_LINKS   8
/* DESIGN D23 5.1 (frozen): UMSG_CANCEL, set by the kernel only, never cleared: the client of this message was killed
 * (kill -9, or a signal whose default action ends it; never for a caught signal) while it waited, or the message was
 * sent by a process already killed. The client still waits for the reply (G4). A server may answer -EINTR once it no
 * longer uses the client's buffers; one that ignores the bit answers as before. The kernel never restarts the call.
 * FS_CLOSE is never marked; FS_QUIT is carried out marked or not (a marked one may drop what it could not save yet);
 * a bounded FS_SYNC (a0 1) ignores the bit. A server's own calls (fsd's to diskd) may carry it: diskd ignores it.
 * Its server's evwait sees POLLPRI on the port (kernel.h) when a message sent to it (queued or held) is marked. */
#define UMSG_CANCEL  4
/* Supplementary groups (owner 2026-10-31; frozen, append-only): UMSG_GROUPS, a kflags bit set by the kernel only (its
 * requests for a system call, vfs.c): a struct umsg_groups follows the message in memory (UMSG_GROUPS_AT(m), right
 * after struct umsg): n (0 .. NGROUPS) and the caller's supplementary group ids (SYS_groups). Each of them counts as the
 * caller's group as gid does, in every permission check (fsperm.h rule 1, and chmod's set-group-id rule 5). A
 * service's load (D11, read as root) carries n 0. Without the bit (a root program's own SYS_call, a kernel before it)
 * only gid counts. The ids are the kernel's, but a server still clamps n to NGROUPS. */
#define UMSG_GROUPS  16
#define NGROUPS      8          /* supplementary groups a process may have (SYS_groups), besides its gid */
struct umsg_groups { unsigned short n, g[NGROUPS]; };
#define UMSG_GROUPS_AT(m) ((const struct umsg_groups *)((m) + 1))
#define FS_LINKED    1024       /* the answer -(FS_LINKED + bits); -1024 .. -1027 used, -1024 .. -1279 reserved */
#define FS_LINKED_END 1279      /* the reserved range's end: no errno goes there (kernel/errno.h) */
#define FSL_A1       1          /* the path in a[1] was rewritten (FS_RENAME, FS_LINK; always FS_SYMLINK), else a[0] */
#define FSL_LAST     2          /* the link was the last name of the original path: meaningful for FS_OPEN only */
static inline int fsl_is(long r) { return r <= -FS_LINKED && r >= -FS_LINKED_END; }
/* which path argument an FS_LINKED answer may rewrite (the table of D26 4, the same in the servers and the kernel):
 * 1 if BITS is allowed for request TYPE */
static inline int fsl_ok(unsigned type, unsigned bits)
{
    if (bits & ~(unsigned)(FSL_A1 | FSL_LAST)) return 0;
    switch (type) {
    case FS_OPEN: case FS_STAT: case FS_LSTAT: case FS_CHDIR: case FS_MKDIR: case FS_UNLINK: case FS_RMDIR:
    case FS_CHMOD: case FS_CHOWN: case FS_UTIMES: case FS_READLINK:
        return !(bits & FSL_A1);                /* a0 only; FSL_LAST is ignored but for FS_OPEN */
    case FS_RENAME: case FS_LINK:
        return 1;                               /* a0, or a1 with FSL_A1 */
    case FS_SYMLINK:
        return (bits & FSL_A1) != 0;            /* a1 only: a0 is the target text, never rewritten */
    }
    return 0;                                   /* no path argument */
}
/* a server's TEXT (above) into OUT (FS_PATHMAX bytes, not overlapping P or T): P the request's original path, the
 * link's name P[s..e), its target T of n bytes (no NUL needed). 0, or -ENOENT, -EIO, -ENAMETOOLONG */
static inline int fsl_text(char *out, const char *p, unsigned s, unsigned e, const char *t, unsigned n)
{
    unsigned k = 0, i;
    if (!n) return -ENOENT;
    if (n > 255) return -ENAMETOOLONG;
    for (i = 0; i < n; i++) if (!t[i]) return -EIO;
    if (t[0] != '/')
        for (i = 1; i < s; i++) { if (k >= FS_PATHMAX - 1) return -ENAMETOOLONG; out[k++] = p[i]; }
    for (i = 0; i < n; i++) { if (k >= FS_PATHMAX - 1) return -ENAMETOOLONG; out[k++] = t[i]; }
    for (i = e; p[i]; i++) { if (k >= FS_PATHMAX - 1) return -ENAMETOOLONG; out[k++] = p[i]; }
    out[k] = 0;
    return 0;
}
struct ustatfs { unsigned bsize, blocks, bfree, files, ffree; };
