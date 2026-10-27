/* SPDX-License-Identifier: GPL-2.0-only */
/* lwIP's hooks (LWIP_HOOK_FILENAME): TCP's initial sequence numbers from netd's SipHash (RFC 6528, D19 9.3); DHCP's
 * options lwIP does not decode itself */
#ifndef ZENIX_NETD_HOOKS_H
#define ZENIX_NETD_HOOKS_H
#include "lwip/ip_addr.h"
u32_t netd_tcp_isn(const ip_addr_t *local_ip, u16_t local_port, const ip_addr_t *remote_ip, u16_t remote_port);
/* DHCP's name servers (option 6) for netd itself (net/netd/iface.c), never straight into lwIP's resolver: D19 6.1 */
struct dhcp;
struct pbuf;
void netd_dhcp_option(struct dhcp *d, int msg_type, int option, int len, struct pbuf *p, int offset);
#endif
