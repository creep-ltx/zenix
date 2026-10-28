/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* BSD sockets for Zenix programs (DESIGN D19 4.4): what <sys/socket.h>, <netinet/in.h>, <arpa/inet.h>, <netdb.h>,
 * <fcntl.h> and <sys/ioctl.h> give on Linux, with Linux's structures and values, so ported programs need no changes
 * beyond the include. Each call is a few lines in libz over SYS_sock (kernel/netproto.h); --gc-sections keeps them out
 * of programs that do not call them. Errors: -1 and zerrno, as every libz call. The network itself is netd (DESIGN
 * D19), started by the kernel at the first socket call. */
#include "libz.h"
#include "../kernel/netproto.h"

typedef u32 socklen_t;
typedef unsigned short sa_family_t;
typedef unsigned short in_port_t;
typedef u32 in_addr_t;
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
#define htons(x) ((unsigned short)(x))
#define ntohs(x) ((unsigned short)(x))
#define htonl(x) ((u32)(x))
#define ntohl(x) ((u32)(x))
#define PF_INET AF_INET
#define PF_UNSPEC AF_UNSPEC

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *a, socklen_t len);
int connect(int fd, const struct sockaddr *a, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *a, socklen_t *len);
int accept4(int fd, struct sockaddr *a, socklen_t *len, int flags);   /* flags: SOCK_NONBLOCK, SOCK_CLOEXEC */
int send(int fd, const void *buf, u32 n, int flags);
int sendto(int fd, const void *buf, u32 n, int flags, const struct sockaddr *a, socklen_t len);
int recv(int fd, void *buf, u32 n, int flags);
int recvfrom(int fd, void *buf, u32 n, int flags, struct sockaddr *a, socklen_t *len);
int shutdown(int fd, int how);
int getsockopt(int fd, int level, int opt, void *val, socklen_t *len);
int setsockopt(int fd, int level, int opt, const void *val, socklen_t len);
int getsockname(int fd, struct sockaddr *a, socklen_t *len);
int getpeername(int fd, struct sockaddr *a, socklen_t *len);

/* fcntl: F_GETFL gives O_RDWR, with O_NONBLOCK on a socket that has it; F_SETFL sets or clears O_NONBLOCK on a socket
 * (the file's flag, DESIGN D19 4.1: not yet on other files, -1 EINVAL); F_GETFD 0, F_SETFD accepted (there is no
 * exec to close on) */
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
int fcntl(int fd, int cmd, ...);
int ioctl(int fd, unsigned long req, ...);      /* FIONREAD (int *), FIONBIO (int *) on a socket */

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
