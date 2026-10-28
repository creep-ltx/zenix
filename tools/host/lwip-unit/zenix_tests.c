/* SPDX-License-Identifier: GPL-2.0-only */
/* Tests of Zenix's local patches to lwIP (net/lwip/patches), in the style of lwIP's own suites. */
#include "lwip_check.h"
#include "lwip/memp.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/stats.h"
#include "lwip/prot/tcp.h"
#include "tcp/tcp_helper.h"

Suite *zenix_suite(void);

/* 0001: a heap pool refuses an element past its limit, and the count goes back to 0 */
START_TEST(test_zenix_memp_limit)
{
    struct tcp_pcb *a, *b, *c;
    u16_t old = memp_limit[MEMP_TCP_PCB];
    LWIP_UNUSED_ARG(_i);
    memp_limit[MEMP_TCP_PCB] = 2;
    a = tcp_new();
    b = tcp_new();
    c = tcp_new();                       /* nothing to kill: new pcbs are in no list */
    fail_unless(a != NULL && b != NULL);
    fail_unless(c == NULL, "a third pcb past the limit of 2");
    fail_unless(memp_count[MEMP_TCP_PCB] == 2);
    fail_unless(lwip_stats.memp[MEMP_TCP_PCB]->err >= 1);
    tcp_abort(a);
    c = tcp_new();
    fail_unless(c != NULL, "room again after a free");
    tcp_abort(b);
    tcp_abort(c);
    fail_unless(memp_count[MEMP_TCP_PCB] == 0);
    memp_limit[MEMP_TCP_PCB] = old;
}
END_TEST

/* 0002: ephemeral ports are not handed out in order */
static int scattered(const u16_t *p, int n)
{
    int i, inorder = 0;
    for (i = 0; i < n; i++) {
        fail_unless(p[i] >= 0xc000, "port %u outside the ephemeral range", p[i]);
        if (i && p[i] == p[i - 1] + 1) inorder++;
    }
    return inorder < n / 4;
}
START_TEST(test_zenix_random_ports)
{
    enum { N = 32 };
    u16_t tp[N], up[N];
    struct tcp_pcb *t[N];
    struct udp_pcb *u[N];
    int i;
    u16_t old = memp_limit[MEMP_TCP_PCB], oldu = memp_limit[MEMP_UDP_PCB];
    LWIP_UNUSED_ARG(_i);
    memp_limit[MEMP_TCP_PCB] = memp_limit[MEMP_UDP_PCB] = N + 8;
    for (i = 0; i < N; i++) {
        t[i] = tcp_new();
        fail_unless(t[i] != NULL);
        fail_unless(tcp_bind(t[i], IP_ADDR_ANY, 0) == ERR_OK);
        tp[i] = t[i]->local_port;
        u[i] = udp_new();
        fail_unless(u[i] != NULL);
        fail_unless(udp_bind(u[i], IP_ADDR_ANY, 0) == ERR_OK);
        up[i] = u[i]->local_port;
    }
    fail_unless(scattered(tp, N), "TCP's ephemeral ports come in order");
    fail_unless(scattered(up, N), "UDP's ephemeral ports come in order");
    for (i = 0; i < N; i++) { tcp_abort(t[i]); udp_remove(u[i]); }
    memp_limit[MEMP_TCP_PCB] = old;
    memp_limit[MEMP_UDP_PCB] = oldu;
}
END_TEST

#ifndef UNIT_PBUF                       /* the pbuf build has no tcp_helper.c */
/* 0003: with a zero window, an ACK of length 0 at rcv_nxt is acceptable and gets no ACK back (two such ends ACKed
 * each other for ever over the loopback: security audit 2026-10-28 S22); one past rcv_nxt still gets one, but at
 * most one such ACK each 500 ms */
START_TEST(test_zenix_zero_window_ack)
{
    struct test_tcp_counters counters;
    struct test_tcp_txcounters txc;
    struct netif netif;
    struct tcp_pcb *pcb;
    struct pbuf *p;
    LWIP_UNUSED_ARG(_i);
    test_tcp_init_netif(&netif, &txc, &test_local_ip, &test_netmask);
    memset(&counters, 0, sizeof counters);
    pcb = test_tcp_new_counters_pcb(&counters);
    fail_unless(pcb != NULL);
    tcp_set_state(pcb, ESTABLISHED, &test_local_ip, &test_remote_ip, TEST_LOCAL_PORT, TEST_REMOTE_PORT);
    pcb->rcv_wnd = 0;                   /* the program read nothing: the window is closed */
    p = tcp_create_rx_segment_wnd(pcb, NULL, 0, 0, 0, TCP_ACK, 0);
    fail_unless(p != NULL);
    txc.num_tx_calls = 0;
    test_tcp_input(p, &netif);
    fail_unless(txc.num_tx_calls == 0, "an ACK at rcv_nxt into a zero window was answered (%u)", txc.num_tx_calls);
    p = tcp_create_rx_segment_wnd(pcb, NULL, 0, 3, 0, TCP_ACK, 0);
    fail_unless(p != NULL);
    test_tcp_input(p, &netif);
    fail_unless(txc.num_tx_calls == 1, "a segment past a zero window gets an ACK (%u)", txc.num_tx_calls);
    p = tcp_create_rx_segment_wnd(pcb, NULL, 0, 3, 0, TCP_ACK, 0);
    fail_unless(p != NULL);
    test_tcp_input(p, &netif);
    fail_unless(txc.num_tx_calls == 1, "a second one in the same 500 ms gets none (%u)", txc.num_tx_calls);
    tcp_abort(pcb);
}
END_TEST
#endif

static void setup(void) { lwip_check_ensure_no_alloc(SKIP_POOL(MEMP_SYS_TIMEOUT)); }
static void teardown(void) { lwip_check_ensure_no_alloc(SKIP_POOL(MEMP_SYS_TIMEOUT)); }

Suite *zenix_suite(void)
{
    testfunc tests[] = { TESTFUNC(test_zenix_memp_limit), TESTFUNC(test_zenix_random_ports),
#ifndef UNIT_PBUF
                         TESTFUNC(test_zenix_zero_window_ack)
#endif
    };
    return create_suite("ZENIX", tests, sizeof tests / sizeof tests[0], setup, teardown);
}
