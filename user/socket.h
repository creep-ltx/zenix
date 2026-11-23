/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* BSD sockets for Zenix programs (DESIGN D19 4.4): what <sys/socket.h>, <netinet/in.h>, <arpa/inet.h>, <netdb.h>,
 * <fcntl.h> and <sys/ioctl.h> give on Linux, with Linux's structures and values, so ported programs need no changes
 * beyond the include. Each call is a few lines over SYS_sock (kernel/netproto.h, user/zsock.c); --gc-sections keeps
 * them out of programs that do not call them. Errors: -1 and errno (zerrno in libz). The network itself is netd
 * (DESIGN D19), started by the kernel at the first socket call. One header for both libraries (DESIGN D27 section 6):
 * libz's programs include it as "socket.h" (with libz.h and the kernel's netproto.h, whose structs and SOCK_* ops some
 * of them use); in the C library it is <sys/socket.h>, <netinet/in.h>, <arpa/inet.h> and <netdb.h> (libc/zenix/include),
 * its numbers from the kernel's headers through zenix-consts.h (tools/libc-consts.sh). */
#ifdef _ZENIX_SOCKET_LIBC
#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <zenix-consts.h>
_BEGIN_STD_C
typedef __socklen_t socklen_t;
typedef __uint32_t in_addr_t;
#define __zx_size size_t
#define __zx_ssize ssize_t
#ifndef AF_INET                         /* (the C library's own zsock.c has the kernel's netproto.h first) */
#define AF_INET       ZX_AF_INET
#define AF_UNSPEC     ZX_AF_UNSPEC
#define SOCK_STREAM   ZX_SOCK_STREAM
#define SOCK_DGRAM    ZX_SOCK_DGRAM
#define SOCK_RAW      ZX_SOCK_RAW
#define SOCK_NONBLOCK ZX_SOCK_NONBLOCK
#define SOCK_CLOEXEC  ZX_SOCK_CLOEXEC
#define SOL_SOCKET    ZX_SOL_SOCKET
#define SO_ERROR      ZX_SO_ERROR
#define SO_RCVTIMEO   ZX_SO_RCVTIMEO
#define SO_SNDTIMEO   ZX_SO_SNDTIMEO
#define SO_REUSEADDR  ZX_SO_REUSEADDR
#define SO_TYPE       ZX_SO_TYPE
#define SO_BROADCAST  ZX_SO_BROADCAST
#define SO_SNDBUF     ZX_SO_SNDBUF
#define SO_RCVBUF     ZX_SO_RCVBUF
#define SO_KEEPALIVE  ZX_SO_KEEPALIVE
#define SO_LINGER     ZX_SO_LINGER
#define MSG_PEEK      ZX_MSG_PEEK
#define MSG_DONTWAIT  ZX_MSG_DONTWAIT
#define MSG_NOSIGNAL  ZX_MSG_NOSIGNAL
#define IPPROTO_IP    ZX_IPPROTO_IP
#define IPPROTO_ICMP  ZX_IPPROTO_ICMP
#define IPPROTO_TCP   ZX_IPPROTO_TCP
#define IPPROTO_UDP   ZX_IPPROTO_UDP
#define TCP_NODELAY   ZX_TCP_NODELAY
#define SHUT_RD       ZX_SHUT_RD
#define SHUT_WR       ZX_SHUT_WR
#define SHUT_RDWR     ZX_SHUT_RDWR
#endif
#else
#include "libz.h"
#include "../kernel/netproto.h"
typedef u32 socklen_t;
typedef u32 in_addr_t;
#define __zx_size u32
#define __zx_ssize int
#endif

typedef unsigned short sa_family_t;
typedef unsigned short in_port_t;
struct in_addr { in_addr_t s_addr; };                       /* network order */
struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct sockaddr_in { sa_family_t sin_family; in_port_t sin_port; struct in_addr sin_addr; unsigned char sin_zero[8]; };
struct linger { int l_onoff, l_linger; };                  /* SO_LINGER: {1, 0} closes by a reset */
struct sockaddr_storage { sa_family_t ss_family; char ss_pad[126]; };  /* 128 bytes, as Linux */
#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE      ((in_addr_t)0xffffffff)
#define INET_ADDRSTRLEN  16
/* the 68k is big-endian: network order is the machine's */
#ifndef htons
#define htons(x) ((unsigned short)(x))
#define ntohs(x) ((unsigned short)(x))
#define htonl(x) ((in_addr_t)(x))
#define ntohl(x) ((in_addr_t)(x))
#endif
#define PF_INET AF_INET
#define PF_UNSPEC AF_UNSPEC

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *a, socklen_t len);
int connect(int fd, const struct sockaddr *a, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *a, socklen_t *len);
int accept4(int fd, struct sockaddr *a, socklen_t *len, int flags);   /* flags: SOCK_NONBLOCK, SOCK_CLOEXEC */
__zx_ssize send(int fd, const void *buf, __zx_size n, int flags);
__zx_ssize sendto(int fd, const void *buf, __zx_size n, int flags, const struct sockaddr *a, socklen_t len);
__zx_ssize recv(int fd, void *buf, __zx_size n, int flags);
__zx_ssize recvfrom(int fd, void *buf, __zx_size n, int flags, struct sockaddr *a, socklen_t *len);
int shutdown(int fd, int how);
int getsockopt(int fd, int level, int opt, void *val, socklen_t *len);
int setsockopt(int fd, int level, int opt, const void *val, socklen_t len);
int getsockname(int fd, struct sockaddr *a, socklen_t *len);
int getpeername(int fd, struct sockaddr *a, socklen_t *len);

/* fcntl: F_GETFL gives O_RDWR, with O_NONBLOCK on a socket that has it; F_SETFL sets or clears O_NONBLOCK on a socket
 * (the file's flag, DESIGN D19 4.1: not yet on other files, -1 EINVAL); F_GETFD 0, F_SETFD accepted (there is no
 * exec to close on) */
#ifndef _ZENIX_SOCKET_LIBC                /* the C library: <fcntl.h> and <sys/ioctl.h> */
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
int fcntl(int fd, int cmd, ...);
int ioctl(int fd, unsigned long req, ...);      /* FIONREAD (int *), FIONBIO (int *) on a socket */
#endif

/* addresses as text */
int inet_pton(int af, const char *src, void *dst);                    /* 1, 0 (not an address), -1 (EAFNOSUPPORT) */
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);
int inet_aton(const char *cp, struct in_addr *a);                     /* also "127.1", "0x7f000001", as BSD */
in_addr_t inet_addr(const char *cp);                                  /* INADDR_NONE when not an address */
char *inet_ntoa(struct in_addr a);                                    /* a static buffer */

/* names (DESIGN D19 4.5): numbers are read here; a name is looked up by netd (/etc/hosts, its cache, DNS) through a
 * socket that becomes readable when the answer is in. Services are port numbers only (there is no /etc/services) */
struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};
#define AI_PASSIVE     0x0001
#define AI_CANONNAME   0x0002
#define AI_NUMERICHOST 0x0004
#define AI_NUMERICSERV 0x0400
#define EAI_BADFLAGS  -1        /* glibc's values */
#define EAI_NONAME    -2
#define EAI_AGAIN     -3
#define EAI_FAIL      -4
#define EAI_FAMILY    -6
#define EAI_SOCKTYPE  -7
#define EAI_SERVICE   -8
#define EAI_MEMORY   -10
#define EAI_SYSTEM   -11
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int e);
/* the one connection helper (docs/audit-2026-11-23-one-system.md OS-38): the name looked up, a socket, the connection
 * made within ZCONNECT_MS (one limit for every network program). zconnect: the socket (blocking unless ZC_NONBLOCK;
 * ZC_UDP: a datagram socket, connected at once), or a negative error number and *why (a fixed text: "no such host",
 * "connection refused", "connection timed out" ...). zconnect_begin starts it and gives the socket at once (a program
 * that keeps its own loop: it waits for POLLOUT and calls zconnect_end with 0 ms, or with the time left) */
#define ZCONNECT_MS 30000
#define ZC_UDP      1
#define ZC_NONBLOCK 2
int zconnect(const char *host, int port, int flags, const char **why);
int zconnect_begin(const char *host, int port, int flags, const char **why);
int zconnect_end(int fd, int ms, const char **why);   /* 1 made, 0 not yet, < 0 failed (the socket closed) */
#ifdef _ZENIX_SOCKET_LIBC
_END_STD_C
#endif
