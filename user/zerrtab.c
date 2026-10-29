/* SPDX-License-Identifier: GPL-2.0-only */
/* The error texts: one table for libz (zstrerror, zwhy) and the C library (strerror, libc/zenix/strerror.c), so a
 * program says the same words with either (user/zshare.h) */
#include "zshare.h"

/* the error number of the last failed system call (or libz helper), and the reason to print for it */
#ifndef ZENIX_LIBC
int zerrno;                             /* the C library has errno (usys.h: zerrno is errno there) */
#endif
static const struct { unsigned char e; const char *text; } errtext[] = {
    { EPERM, "operation not permitted" }, { ENOENT, "no such file or directory" }, { ESRCH, "no such process" },
    { EINTR, "interrupted" }, { EIO, "input/output error" }, { ENXIO, "no such device" },
    { E2BIG, "argument list too long" }, { ENOEXEC, "not a Zenix program" }, { EBADF, "bad file descriptor" },
    { ECHILD, "no child process" }, { EAGAIN, "resource temporarily unavailable" }, { ENOMEM, "out of memory" },
    { EACCES, "permission denied" }, { EFAULT, "bad address" }, { EBUSY, "device or resource busy" },
    { EEXIST, "file exists" }, { EXDEV, "cross-device link" }, { ENODEV, "no such device" },
    { ENOTDIR, "not a directory" }, { EISDIR, "is a directory" }, { EINVAL, "invalid argument" },
    { ENFILE, "too many open files in the system" }, { EMFILE, "too many open files" }, { ENOTTY, "not a terminal" }, { ETXTBSY, "text file busy" },
    { EFBIG, "file too large" }, { ENOSPC, "no space left on device" }, { ESPIPE, "illegal seek" },
    { EROFS, "read-only file system" }, { EMLINK, "too many links" }, { EPIPE, "broken pipe" }, { EDOM, "argument out of domain" },
    { ERANGE, "result out of range" }, { ENAMETOOLONG, "file name too long" }, { ENOSYS, "not implemented" },
    { ENOTEMPTY, "directory not empty" }, { ELOOP, "too many levels of symbolic links" },
    { EOVERFLOW, "value too large for its type" }, { EILSEQ, "invalid or incomplete multibyte character" },
    { EOPNOTSUPP, "operation not supported" },
    /* the network's (DESIGN D19) */
    { ENOTSOCK, "not a socket" }, { EDESTADDRREQ, "destination address required" }, { EMSGSIZE, "message too long" },
    { EPROTOTYPE, "protocol wrong type for socket" }, { ENOPROTOOPT, "protocol not available" },
    { EPROTONOSUPPORT, "protocol not supported" }, { EAFNOSUPPORT, "address family not supported" },
    { EADDRINUSE, "address already in use" }, { EADDRNOTAVAIL, "cannot assign requested address" },
    { ENETDOWN, "network is down" }, { ENETUNREACH, "network is unreachable" },
    { ECONNABORTED, "connection aborted" }, { ECONNRESET, "connection reset" }, { ENOBUFS, "no buffer space available" },
    { EISCONN, "already connected" }, { ENOTCONN, "not connected" }, { ETIMEDOUT, "connection timed out" },
    { ECONNREFUSED, "connection refused" }, { EHOSTUNREACH, "no route to host" },
    { EALREADY, "operation already in progress" }, { EINPROGRESS, "operation in progress" },
    { ESTALE, "stale file handle" },
};
/* strerror: the text of an error number (positive or negative), 0 if it has none */
const char *zstrerror(int e)
{
    if (e < 0) e = -e;
    for (unsigned i = 0; i < sizeof errtext / sizeof errtext[0]; i++)
        if (errtext[i].e == e) return errtext[i].text;
    return 0;
}
/* The text for zerrno, else dflt. EPERM (1) gives dflt too: a bare -1, which some calls still return for "failed"
 * without a reason, looks the same (docs/limits.md lists them). */
const char *zwhy(const char *dflt)
{
    const char *t = zerrno > 1 ? zstrerror(zerrno) : 0;
    return t ? t : dflt;
}
