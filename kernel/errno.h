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
#define ERANGE       34     /* result out of range */
#define ENAMETOOLONG 36     /* file name or path too long */
#define ENOSYS       38     /* no such system call / not implemented */
#define ENOTEMPTY    39     /* directory not empty */
#define ELOOP        40     /* too many symbolic links */
#define EOPNOTSUPP   95     /* operation not supported (here) */
#define ECONNRESET  104     /* the server is gone: it died while serving, could not be started, or was given up (D11) */
