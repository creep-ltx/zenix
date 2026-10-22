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
