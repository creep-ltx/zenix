/* SPDX-License-Identifier: GPL-2.0-only */
/* open()'s flags are Zenix's (DESIGN D27 section 5, frozen: docs/frozen-interfaces.md), from the kernel's headers
 * through the generated zenix-consts.h; picolibc's own header has O_EXCL 0x800, which is Zenix's O_NONBLOCK. Flags
 * Zenix has no meaning for (O_NOCTTY, O_SYNC, ...) are not defined: a port that needs one finds out when
 * it is compiled, not by a flag silently ignored. */
#ifndef _SYS__DEFAULT_FCNTL_H_
#define _SYS__DEFAULT_FCNTL_H_
#include <sys/cdefs.h>
#include <sys/_types.h>
#include <zenix-consts.h>
_BEGIN_STD_C
#ifndef _MODE_T_DECLARED
typedef __mode_t mode_t;
#define _MODE_T_DECLARED
#endif
#ifndef _OFF_T_DECLARED
typedef __off_t off_t;
#define _OFF_T_DECLARED
#endif
#ifndef _PID_T_DECLARED
typedef __pid_t pid_t;
#define _PID_T_DECLARED
#endif
#define O_RDONLY   ZX_O_RDONLY
#define O_WRONLY   ZX_O_WRONLY
#define O_RDWR     ZX_O_RDWR
#define O_ACCMODE  (O_RDONLY | O_WRONLY | O_RDWR)
#define O_CREAT    ZX_O_CREAT
#define O_TRUNC    ZX_O_TRUNC
#define O_APPEND   ZX_O_APPEND
#define O_EXCL     ZX_O_EXCL
#define O_NONBLOCK ZX_O_NONBLOCK
#define O_CLOEXEC  0                    /* what Zenix always does: a spawned program gets only descriptors 0-2 */
#define FD_CLOEXEC 1
#define F_DUPFD    0
#define F_GETFD    1
#define F_SETFD    2
#define F_GETFL    3
#define F_SETFL    4
int open(const char *, int, ...);
int creat(const char *, mode_t);
int fcntl(int, int, ...);
_END_STD_C
#endif
