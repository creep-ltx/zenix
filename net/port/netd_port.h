/* SPDX-License-Identifier: GPL-2.0-only */
/* What lwIP needs from netd (DESIGN D19 2.2, 9.3, 9.4): the limits set by the machine, the heap, the random numbers.
 * Included by lwipopts.h, so it may use no lwIP type. */
#ifndef ZENIX_NETD_PORT_H
#define ZENIX_NETD_PORT_H

/* lwIP's sizes that netd sets at start by the machine's memory (netd_init, net/netd/core.c); lwipopts.h
 * names them in TCP_WND, TCP_SND_BUF, TCP_OOSEQ_*_LIMIT and IP_REASS_MAX_PBUFS. Plain variables, not a struct:
 * lwIP tests some of these names in #if lines, where an identifier counts as 0 and a '.' is an error. */
extern unsigned short netd_tcp_wnd;      /* the receive window of every TCP connection, bytes */
extern unsigned short netd_tcp_snd_buf;  /* the send buffer of every TCP connection, bytes */
extern unsigned long netd_oos_bytes;     /* out-of-order data kept per connection, bytes */
extern unsigned short netd_oos_pbufs;    /* and buffers */
extern unsigned short netd_reass_pbufs;  /* buffers held by IP reassembly, all datagrams together */

/* lwIP's heap: every byte counted, refused past netd_mem_limit (0 = no limit) */
void *netd_malloc(unsigned long n);
void *netd_calloc(unsigned long count, unsigned long size);
void netd_free(void *p);
extern unsigned long netd_mem_used, netd_mem_limit;

/* the clocks, from the operating system layer (netd's main program on Zenix, the test harness on the PC):
 * milliseconds for lwIP's timers (sys_now), and a clock ticking every 4 microseconds for TCP's ISNs (RFC 6528) */
unsigned long netd_os_now_ms(void);
unsigned long netd_os_clock4us(void);

/* the hot loops in assembler (net/port/m68k.S, D19 2.2); the host build of the tests keeps lwIP's own C */
#ifndef NETD_HOST
unsigned short netd_chksum(const void *data, int len);
void *netd_copy(void *dst, const void *src, unsigned long n);
#endif

#endif
