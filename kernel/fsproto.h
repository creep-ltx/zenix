#pragma once
/* Messages between the kernel and the filesystem server ("fs" port).
 * type = op, a[0..3] = arguments, result = value or -1 (-5 = server gone).
 * All pointers are plain addresses: no MMU, the server reads and writes
 * the caller's buffers in place.
 * Who may send them: the kernel (struct umsg kflags UMSG_KERNEL, set only by ipc_kcall for a system call, with the
 * caller's ids) and root. Any other message - a user's own SYS_call to the port - is answered -13: inode numbers
 * (FS_READ, FS_WRITE, ...) and FS_QUIT would bypass every permission check (audit D). */
/* One path limit (audit E): an absolute, normalised path - what the kernel builds from the working directory and a
 * relative path, and what a server may be handed - is at most FS_PATHMAX - 1 (159) bytes; the working directory text
 * too. A longer one is refused with -ENAMETOOLONG, never cut. (A path given to a system call is at most 63 bytes.) */
#define FS_PATHMAX 160
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
#define FS_SYNC    13  /* everything written so far is on the medium when this returns (DISK_FLUSH of the device) */
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
struct fsinfo { char dev[16]; char type[8]; };
struct ustatfs { unsigned bsize, blocks, bfree, files, ffree; };
