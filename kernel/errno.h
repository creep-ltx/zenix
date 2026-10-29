/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* Error numbers, the Linux/POSIX values, one table for the kernel, the servers and libz (audit batch E). A call that
 * fails returns the negative number (-ENOENT); libz keeps the last one in zerrno and zwhy()/zstrerror() turn it
 * into text. Dependency-free, so the host tests include it too. */
#define EPERM         1     /* operation not permitted (not root, not the owner) */
#define ENOENT        2     /* no such file or directory */
#define ESRCH         3     /* no such process */
#define EINTR         4     /* interrupted (Ctrl-C) */
#define EIO           5     /* input/output error (the disk, a damaged filesystem) */
#define ENXIO         6     /* no such device */
#define E2BIG         7     /* argument list too long */
#define ENOEXEC       8     /* not a Zenix program */
#define EBADF         9     /* bad file descriptor */
#define ECHILD       10     /* no child process */
#define EAGAIN       11     /* try again (the process table is full) */
#define ENOMEM       12     /* out of memory */
#define EACCES       13     /* permission denied */
#define EFAULT       14     /* bad address (a pointer outside the caller's memory) */
#define EBUSY        16     /* in use (a mounted device, a busy mount point) */
#define EEXIST       17     /* file exists */
#define EXDEV        18     /* across filesystems (rename, link) */
#define ENODEV       19     /* no such device (or the filesystem type is unknown) */
#define ENOTDIR      20     /* not a directory */
#define EISDIR       21     /* is a directory */
#define EINVAL       22     /* invalid argument */
#define ENFILE       23     /* the system's open-file table is full */
#define EMFILE       24     /* the process has no free file descriptor */
#define ENOTTY       25     /* not a terminal */
#define ETXTBSY      26     /* text file busy: the program's shared code is in memory (D13) */
#define EFBIG        27     /* file too large */
#define ENOSPC       28     /* no space left on the device */
#define ESPIPE       29     /* seek on a pipe or terminal */
#define EROFS        30     /* read-only filesystem */
#define EMLINK       31     /* too many links */
#define EPIPE        32     /* broken pipe */
#define EDOM         33     /* a math function's argument outside its domain (DESIGN D27: the C library) */
#define ERANGE       34     /* result out of range */
#define ENAMETOOLONG 36     /* file name or path too long */
#define ENOSYS       38     /* no such system call / not implemented */
#define ENOTEMPTY    39     /* directory not empty */
#define ELOOP        40     /* too many symbolic links */
#define EOVERFLOW    75     /* a value too large for its type (DESIGN D27: the C library) */
#define EILSEQ       84     /* an invalid or incomplete multibyte character (DESIGN D27: the C library's UTF-8) */
#define ENOTSOCK     88     /* not a socket (DESIGN D19: the network's numbers, Linux's values) */
#define EDESTADDRREQ 89     /* destination address required */
#define EMSGSIZE     90     /* message too long */
#define EPROTOTYPE   91     /* protocol wrong type for socket */
#define ENOPROTOOPT  92     /* protocol not available (an unknown socket option) */
#define EPROTONOSUPPORT 93  /* protocol not supported */
#define EOPNOTSUPP   95     /* operation not supported (here) */
#define ENOTSUP      EOPNOTSUPP     /* the same number, as Linux (DESIGN D27) */
#define EAFNOSUPPORT 97     /* address family not supported */
#define EADDRINUSE   98     /* address already in use */
#define EADDRNOTAVAIL 99    /* cannot assign the requested address */
#define ENETDOWN    100     /* the network is down: no network server, it ended, or it was given up (on a socket) */
#define ENETUNREACH 101     /* network is unreachable */
#define ECONNABORTED 103    /* connection aborted (the address it was bound to went away) */
#define ECONNRESET  104     /* the server is gone: it died while serving, could not be started, or was given up (D11);
                             * on a socket: reset by the peer (the server's absence is ENETDOWN there, D19) */
#define ENOBUFS     105     /* no buffer space (the network server's limits) */
#define EISCONN     106     /* already connected */
#define ENOTCONN    107     /* not connected */
#define ETIMEDOUT   110     /* connection timed out */
#define ECONNREFUSED 111    /* connection refused */
#define EHOSTUNREACH 113    /* no route to host */
#define EALREADY    114     /* a connect is already under way */
#define EINPROGRESS 115     /* the connect goes on (a non-blocking socket) */
#define ESTALE      116     /* stale file handle (D23: an NFS file the server no longer knows, or one opened before
                             * nfscd started again) */
/* 1024 .. 1279 are reserved: no errno goes there. -1024 .. -1279 is the filesystem servers' FS_LINKED answer to the
 * kernel (DESIGN D26, kernel/fsproto.h), never seen by a program. */
