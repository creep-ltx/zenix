/* SPDX-License-Identifier: GPL-2.0-only */
/* BSD sockets (DESIGN D19 4.4, DESIGN D27 section 5): the one header libz's programs use too, user/socket.h, with
 * Linux's structures and Zenix's (Linux's) numbers from the kernel's headers; <netinet/in.h>, <netinet/tcp.h>,
 * <arpa/inet.h> and <netdb.h> are this one. */
#ifndef _ZENIX_SYS_SOCKET_H_
#define _ZENIX_SYS_SOCKET_H_
#define _ZENIX_SOCKET_LIBC 1
#include "../../../../user/socket.h"
#endif
