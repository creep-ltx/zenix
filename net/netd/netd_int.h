/* SPDX-License-Identifier: GPL-2.0-only */
/* What netd's core files share among themselves (net/netd: core.c, sock.c, iface.c, config.c). */
#ifndef ZENIX_NETD_INT_H
#define ZENIX_NETD_INT_H

#include "lwip/opt.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "netd.h"

extern struct netd_limits netd_lim;
extern struct netd_stats netd_st;

/* core.c */
void netd_logf(const char *fmt, ...);         /* "netd: ..." with %s %d %u %lu, no line end */
void netd_mem_refused(void);

/* sock.c */
void netd_sock_init(void);
void netd_sock_turn(void);                     /* the closes that waited for memory */

/* iface.c */
void netd_if_init(void);
void netd_if_turn(void);                       /* read the lines, send what waits */
int netd_if_handles(int *h, int *want_write, int max);
int netd_addr_is_local(const ip4_addr_t *a);   /* an address of one of netd's interfaces (or 127.x, or any) */
unsigned netd_route_mtu(const ip4_addr_t *dst);   /* the MTU of the interface a packet to dst leaves by; 0: no route */

/* config.c */
int netd_hosts_find(const char *name, u32_t *addrs, int max);   /* /etc/hosts: how many addresses (network order) */
int netd_parse_ip4(const char *s, u32_t *a);   /* dotted quad only; 1 if it is one */

#endif
