/* SPDX-License-Identifier: GPL-2.0-only */
/* The error texts: one table for libz (zstrerror, zwhy) and the C library (strerror, libc/zenix/strerror.c), so a
 * program says the same words with either (user/zshare.h) */
#include "zshare.h"

/* the error number of the last failed system call (or libz helper), and the reason to print for it */
#ifndef ZENIX_LIBC
int zerrno;                             /* the C library has errno (usys.h: zerrno is errno there) */
#endif
/* The table as one list (X-macro): the numbers as bytes and the texts as one string, each ended by its 0, so the table
 * holds no pointers: no relocations and no pointer table in every program that links it (2026-11-11: a 62-entry
 * table of pointers was 372 bytes plus 62 relocations, 248 bytes, in every program's file) */
#define ERRTAB \
    X(EPERM, "operation not permitted") X(ENOENT, "no such file or directory") X(ESRCH, "no such process") \
    X(EINTR, "interrupted") X(EIO, "input/output error") X(ENXIO, "no such device") \
    X(E2BIG, "argument list too long") X(ENOEXEC, "not a Zenix program") X(EBADF, "bad file descriptor") \
    X(ECHILD, "no child process") X(EAGAIN, "resource temporarily unavailable") X(ENOMEM, "out of memory") \
    X(EACCES, "permission denied") X(EFAULT, "bad address") X(EBUSY, "device or resource busy") \
    X(EEXIST, "file exists") X(EXDEV, "cross-device link") X(ENODEV, "no such device") X(ENOTDIR, "not a directory") \
    X(EISDIR, "is a directory") X(EINVAL, "invalid argument") X(ENFILE, "too many open files in the system") \
    X(EMFILE, "too many open files") X(ENOTTY, "not a terminal") X(ETXTBSY, "text file busy") \
    X(EFBIG, "file too large") X(ENOSPC, "no space left on device") X(ESPIPE, "illegal seek") \
    X(EROFS, "read-only file system") X(EMLINK, "too many links") X(EPIPE, "broken pipe") \
    X(EDOM, "argument out of domain") X(ERANGE, "result out of range") X(ENAMETOOLONG, "file name too long") \
    X(ENOSYS, "not implemented") X(ENOTEMPTY, "directory not empty") X(ELOOP, "too many levels of symbolic links") \
    X(EOVERFLOW, "value too large for its type") X(EILSEQ, "invalid or incomplete multibyte character") \
    X(EOPNOTSUPP, "operation not supported") X(ENOTSOCK, "not a socket") \
    X(EDESTADDRREQ, "destination address required") X(EMSGSIZE, "message too long") \
    X(EPROTOTYPE, "protocol wrong type for socket") X(ENOPROTOOPT, "protocol not available") \
    X(EPROTONOSUPPORT, "protocol not supported") X(EAFNOSUPPORT, "address family not supported") \
    X(EADDRINUSE, "address already in use") X(EADDRNOTAVAIL, "cannot assign requested address") \
    X(ENETDOWN, "network is down") X(ENETUNREACH, "network is unreachable") X(ECONNABORTED, "connection aborted") \
    X(ECONNRESET, "connection reset") X(ENOBUFS, "no buffer space available") X(EISCONN, "already connected") \
    X(ENOTCONN, "not connected") X(ETIMEDOUT, "connection timed out") X(ECONNREFUSED, "connection refused") \
    X(EHOSTUNREACH, "no route to host") X(EALREADY, "operation already in progress") \
    X(EINPROGRESS, "operation in progress") X(ESTALE, "stale file handle")
#define X(e, t) e,
static const unsigned char errnum[] = { ERRTAB };
#undef X
#define X(e, t) t "\0"
static const char errstr[] = ERRTAB;
#undef X
/* strerror: the text of an error number (positive or negative), 0 if it has none */
const char *zstrerror(int e)
{
    if (e < 0) e = -e;
    const char *t = errstr;
    for (unsigned i = 0; i < sizeof errnum; i++) {
        if (errnum[i] == e) return t;
        while (*t++) ;
    }
    return 0;
}
/* The text for zerrno, else dflt. EPERM (1) gives dflt too: a bare -1, which some calls still return for "failed"
 * without a reason, looks the same (docs/limits.md lists them). */
const char *zwhy(const char *dflt)
{
    const char *t = zerrno > 1 ? zstrerror(zerrno) : 0;
    return t ? t : dflt;
}
