/* SPDX-License-Identifier: GPL-2.0-only */
/* What lwIP's unit tests need on top of Zenix's lwipopts.h (net/port/lwipopts.h includes this file last in the
 * test build only, NETD_HOST_OPTS). Each change is something a test reads, not a different stack:
 *  - statistics: the tests check that every pool and the heap are empty after each test;
 *  - LWIP_TESTMODE and the test hooks the suites call (static ARP entries, the netif status callback);
 *  - pool limits big enough for the timers the tests add (upstream: 8 more sys timeouts), and for the tests' own traffic (the pbuf tests queue ~200 KB, the TCP tests 40 segments);
 *  - the window and the send buffer as constants (variables in Zenix's build: the tests size arrays with them),
 *    upstream's test numbers: TCP_WND 10 x MSS, TCP_SND_BUF 12 x MSS;
 *  - test_pbuf.c alone needs window scaling (UNIT_PBUF: a second binary), which Zenix's build does not have, and
 *    pool buffers smaller than 1 KB to build its chains (upstream's 592 bytes, from its TCP_MSS of 536);
 *  - lwIP's own initial sequence numbers (the TCP tests compute them from tcp_ticks): netd's ISN hook is tested by
 *    rand-test and the netd test instead;
 *  - test_ip4.c polls the tcpip thread that NO_SYS has not got (main.c gives it an empty stand-in). */
#undef LWIP_STATS
#define LWIP_STATS                      1
#define MEM_STATS                       1
#define MEMP_STATS                      1
#define MIB2_STATS                      1
#define LWIP_TESTMODE                   1
#define ETHARP_SUPPORT_STATIC_ENTRIES   1
#define LWIP_NETIF_EXT_STATUS_CALLBACK  1
#define LWIP_MEM_ILLEGAL_FREE(msg)      /* the mem tests check lwip_stats.mem.illegal instead */
#undef PBUF_POOL_SIZE
#define PBUF_POOL_SIZE                  400
#undef MEMP_NUM_TCP_SEG
#define MEMP_NUM_TCP_SEG                64
#undef MEMP_NUM_PBUF
#define MEMP_NUM_PBUF                   64
#undef TCP_WND
#define TCP_WND                         (10 * TCP_MSS)
#undef TCP_SND_BUF
#define TCP_SND_BUF                     (12 * TCP_MSS)
#ifdef UNIT_PBUF
#undef LWIP_WND_SCALE
#define LWIP_WND_SCALE                  1
#define TCP_RCV_SCALE                   0
#define PBUF_POOL_BUFSIZE               592
#endif
int tcpip_thread_poll_one(void);
#undef LWIP_HOOK_TCP_ISN
#undef MEMP_NUM_SYS_TIMEOUT
#define MEMP_NUM_SYS_TIMEOUT            (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)
