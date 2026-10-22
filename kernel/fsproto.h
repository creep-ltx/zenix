#pragma once
/* Messages between the kernel and the filesystem server ("fs" port).
 * type = op, a[0..3] = arguments, result = value or -1 (-5 = server gone).
 * All pointers are plain addresses: no MMU, the server reads and writes
 * the caller's buffers in place. */
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
#define FS_SYNC    13  /* flush everything (writes are write-through today, so a no-op) */
#define FS_CHMOD   14  /* a0 path, a1 cwd ino, a2 mode */
#define FS_LINK    15  /* a0 existing path, a1 new path, a2 cwd ino: a hard link */
#define FS_SYMLINK 16  /* a0 target text, a1 link path, a2 cwd ino */
#define FS_READLINK 17 /* a0 path, a1 cwd ino, a2 buffer, a3 max -> length */
#define FS_UTIMES  18  /* a0 path, a1 cwd ino, a2 modification time */
#define FS_CHOWN   20  /* a0 path, a1 cwd ino, a2 uid, a3 gid (-1 = keep): root only */
#define O_EXEC     0x8000  /* FS_OPEN flag: the caller wants to execute the file (needs an x bit) */
#define FS_LSTAT   19  /* like FS_STAT but a final symbolic link is not followed */
struct ustatfs { unsigned bsize, blocks, bfree, files, ffree; };
