/* SPDX-License-Identifier: GPL-2.0-only */
/* lwIP's configuration for netd (DESIGN D19: 2.2, 6.2, 9.3, 9.4, 9.5). One configuration for every machine: the
 * limits that depend on the machine's memory are variables netd sets at start (net/port/netd_port.h), the rest is
 * fixed here. */
#ifndef ZENIX_LWIPOPTS_H
#define ZENIX_LWIPOPTS_H

#include "netd_port.h"

/* one thread, the raw (callback) API only: netd's main loop calls lwIP, never an interrupt or a signal handler */
#define NO_SYS                          1
#define SYS_LIGHTWEIGHT_PROT            0
#define LWIP_NETCONN                    0
#define LWIP_SOCKET                     0
#define LWIP_TIMERS                     1

/* protocols: IPv4 with TCP, UDP, ICMP and raw (ping); DHCP and DNS (6.2); Ethernet and ARP; no IPv6 (+33 KB, a
 * later option), no ACD (the ARP probe of an offered address, 6.2), no AutoIP, no IGMP */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0
#define LWIP_ARP                        1
#define LWIP_ETHERNET                   1
#define LWIP_ICMP                       1
#define LWIP_RAW                        1
#define LWIP_UDP                        1
#define LWIP_TCP                        1
#define LWIP_DHCP                       1
#define LWIP_DHCP_DOES_ACD_CHECK        0
#define LWIP_ACD                        0
#define LWIP_AUTOIP                     0
#define LWIP_IGMP                       0
#define LWIP_DNS                        1
#define LWIP_BROADCAST_PING             0
#define LWIP_MULTICAST_PING             0

/* fragments: reassembly stays, sending fragments goes (2.2) */
#define IP_REASSEMBLY                   1
#define IP_FRAG                         0
#define IP_REASS_MAX_PBUFS              netd_reass_pbufs

/* the interfaces inside netd: loopback and SLIP (RFC 1055, MTU 1006 so TCP's MSS is 966: 5.3), polled from the
 * main loop; LWIP_NETIF_HOSTNAME for the `hostname` word of /etc/network/interfaces (sent with DHCP) */
#define LWIP_HAVE_LOOPIF                1
#define LWIP_NETIF_LOOPBACK             1
#define LWIP_LOOPBACK_MAX_PBUFS         16
#define LWIP_HAVE_SLIPIF                1
#define SLIP_MAX_SIZE                   1006
#define SLIP_USE_RX_THREAD              0
#define SLIP_RX_FROM_ISR                0
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_STATUS_CALLBACK      0
#define LWIP_NETIF_LINK_CALLBACK        0
#define LWIP_NUM_NETIF_CLIENT_DATA      0
/* a driver's zero-copy receive hands its buffer to lwIP as a custom pbuf whose free gives it back (2.4) */
#define LWIP_SUPPORT_CUSTOM_PBUF        1

/* memory: everything from netd's heap (netd_malloc counts and caps the bytes), and every pool keeps a count limit
 * that netd sets by the machine (local patch 0001, 9.4); the MEMP_NUM_* below are the defaults, the stock A1200's
 * numbers of 7.3 */
#define MEM_CUSTOM_ALLOCATOR            1
#define MEM_CUSTOM_MALLOC               netd_malloc
#define MEM_CUSTOM_FREE                 netd_free
#define MEM_CUSTOM_CALLOC               netd_calloc
#define MEM_ALIGNMENT                   4
#define MEMP_MEM_MALLOC                 1
#define MEMP_MEM_MALLOC_LIMIT           1
#define MEMP_NUM_TCP_PCB                8
#define MEMP_NUM_TCP_PCB_LISTEN         4
#define MEMP_NUM_TCP_SEG                32
#define MEMP_NUM_UDP_PCB                8
#define MEMP_NUM_RAW_PCB                2
#define MEMP_NUM_PBUF                   16
#define PBUF_POOL_SIZE                  16
#define MEMP_NUM_REASSDATA              2
#define MEMP_NUM_ARP_QUEUE              8
#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 4)

/* TCP: MSS 1460 on Ethernet (the interface's MTU makes it 966 on SLIP: TCP_CALCULATE_EFF_SEND_MSS); the window and
 * the send buffer are netd's numbers for the machine (7.3: 2 x MSS and 4 KB on the stock A1200, 16-32 KB on
 * Ethernet machines), so they are variables (no window scaling: at most 64 KB). lwIP's compile-time checks of
 * these sizes cannot see variables: netd_init() makes the same checks at start (core.c) (LWIP_DISABLE_TCP_SANITY_CHECKS) */
#define TCP_MSS                         1460
#define TCP_CALCULATE_EFF_SEND_MSS      1
#define LWIP_WND_SCALE                  0
#define TCP_WND                         netd_tcp_wnd
#define TCP_SND_BUF                     netd_tcp_snd_buf
#define TCP_SND_QUEUELEN                64
#define LWIP_DISABLE_TCP_SANITY_CHECKS  1
#define TCP_QUEUE_OOSEQ                 1
#define TCP_OOSEQ_BYTES_LIMIT(pcb)      netd_oos_bytes
#define TCP_OOSEQ_PBUFS_LIMIT(pcb)      netd_oos_pbufs
#define TCP_LISTEN_BACKLOG              1
#define TCP_DEFAULT_LISTEN_BACKLOG      0xff
#define LWIP_TCP_KEEPALIVE              1
#define SO_REUSE                        1
#define LWIP_CALLBACK_API               1
#define LWIP_EVENT_API                  0

/* random numbers (9.3): every ephemeral port random (local patch 0002), the initial sequence numbers from netd's
 * SipHash (RFC 6528), DNS query ids and source ports random, one outstanding query per name */
#define LWIP_RANDOMIZE_EACH_LOCAL_PORT  1
#define LWIP_HOOK_FILENAME              "netd_hooks.h"
#define LWIP_HOOK_TCP_ISN               netd_tcp_isn
#define LWIP_DNS_SECURE                 (LWIP_DNS_SECURE_RAND_XID | LWIP_DNS_SECURE_NO_MULTIPLE_OUTSTANDING | \
                                         LWIP_DNS_SECURE_RAND_SRC_PORT)

/* DHCP's name servers go to netd (the hook), not straight into the resolver: /etc/resolv.conf's win (6.1) */
#define LWIP_DHCP_MAX_DNS_SERVERS       0
#define DHCP_ADD_EXTRA_REQUEST_OPTIONS  , 6
#define LWIP_HOOK_DHCP_PARSE_OPTION(netif, dhcp, state, msg, msg_type, option, len, pbuf, offset) \
        do { LWIP_UNUSED_ARG(msg); LWIP_UNUSED_ARG(netif); netd_dhcp_option(dhcp, state, msg_type, option, len, pbuf, offset); } while (0)

/* DNS (6.2): netd answers /etc/hosts itself; lwIP keeps the cache */
#define DNS_TABLE_SIZE                  8
#define DNS_MAX_NAME_LENGTH             256
#define DNS_MAX_SERVERS                 2
#define DNS_LOCAL_HOSTLIST              0
#define LWIP_DNS_SUPPORT_MDNS_QUERIES   0

/* no statistics, no debug output (netd counts frames itself for ifconfig) */
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0
#define LWIP_DEBUG_TIMERNAMES           0
#define LWIP_PROVIDE_ERRNO              0
#define LWIP_ERRNO_STDINCLUDE           0

/* the checksum and the copy in 68020 assembler (net/port/m68k.S); the host build uses lwIP's C and memcpy */
#ifndef NETD_HOST
#define LWIP_CHKSUM                     netd_chksum
#define MEMCPY(dst, src, len)           netd_copy(dst, src, len)
#endif

/* the host build of the tests may change a few of these (tools/host/net-test.sh); never the Zenix build */
#ifdef NETD_HOST_OPTS
#include NETD_HOST_OPTS
#endif

#endif
