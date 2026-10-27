/* SPDX-License-Identifier: GPL-2.0-only */
/* lwIP's own unit tests (net/lwip/test/unit) with Zenix's lwipopts.h and the local patches: the suites for the
 * parts netd uses. Not run: ip6, mdns, mqtt, sockets, pppos (not in Zenix's lwIP), mem (lwIP's own heap allocator,
 * which Zenix does not compile: MEM_CUSTOM_ALLOCATOR gives lwIP netd's counted heap instead). Built twice: once with every
 * suite but pbuf, once (UNIT_PBUF) with the pbuf suite alone, which needs window scaling (unit-opts.h). */
#include <stdio.h>
#include <stdlib.h>
#include "lwip_check.h"
#include "lwip/init.h"
#include "lwip/stats.h"
#include "lwip/memp.h"
#include "lwip/priv/memp_priv.h"
#include "netd_rand.h"

#include "ip4/test_ip4.h"
#include "udp/test_udp.h"
#include "tcp/test_tcp.h"
#include "tcp/test_tcp_oos.h"
#include "tcp/test_tcp_state.h"
#include "core/test_def.h"
#include "core/test_dns.h"
#include "core/test_netif.h"
#include "core/test_pbuf.h"
#include "core/test_timers.h"
#include "etharp/test_etharp.h"
#include "dhcp/test_dhcp.h"
Suite *zenix_suite(void);       /* zenix_tests.c: the local patches */

u32_t lwip_sys_now;
int tcpip_thread_poll_one(void) { return 0; }      /* NO_SYS: nothing is ever queued for a tcpip thread */
unsigned long netd_os_now_ms(void) { return lwip_sys_now; }
unsigned long netd_os_clock4us(void) { return lwip_sys_now * 250; }
void netd_lwip_assert(int line)
{
    ck_fail_at("lwIP", line, "assertion failed in lwIP (line number only)", (const char *)NULL);
    abort();     /* outside a test */
}

/* as lwip_unittests.c (not taken: it lists every suite) */
Suite *create_suite(const char *name, testfunc *tests, size_t num_tests, SFun setup, SFun teardown)
{
    size_t i;
    Suite *s = suite_create(name);
    for (i = 0; i < num_tests; i++) {
        TCase *tc = tcase_create(name);
        if (setup || teardown) tcase_add_checked_fixture(tc, setup, teardown);
        tcase_add_named_test(tc, tests[i]);
        suite_add_tcase(s, tc);
    }
    return s;
}
/* As lwip_unittests.c, with one change: Zenix's pools come from the heap (MEMP_MEM_MALLOC), so the heap holds
 * the pool elements still in use (the sys timeouts the tests skip, for one); the heap must hold nothing else. */
void lwip_check_ensure_no_alloc(unsigned int skip)
{
    int i;
    unsigned int mask;
    long pools = 0;
    for (i = 0; i < MEMP_MAX; i++)
        pools += (long)lwip_stats.memp[i]->used * (long)(MEMP_SIZE + LWIP_MEM_ALIGN_SIZE(memp_pools[i]->size));
    if (!(skip & SKIP_HEAP))
        fail_unless((long)lwip_stats.mem.used == pools, "mem heap still has %ld bytes allocated outside the pools",
                    (long)lwip_stats.mem.used - pools);
    for (i = 0, mask = 1; i < MEMP_MAX; i++, mask <<= 1)
        if (!(skip & mask)) {
            fail_unless(lwip_stats.memp[i]->used == 0, "memp pool %d still has %d entries allocated", i, (int)lwip_stats.memp[i]->used);
            /* local patch 0001: the count agrees with the statistics */
            fail_unless(memp_count[i] == lwip_stats.memp[i]->used, "memp pool %d: count %d, statistics %d", i,
                        (int)memp_count[i], (int)lwip_stats.memp[i]->used);
        }
}

int main(void)
{
    static const u8_t seed[NETD_SEED_BYTES] = { 1, 2, 3 };
#ifdef UNIT_PBUF
    suite_getter_fn *suites[] = { pbuf_suite };
#else
    suite_getter_fn *suites[] = { ip4_suite, udp_suite, tcp_suite, tcp_oos_suite, tcp_state_suite, def_suite,
                                  dns_suite, netif_suite, timers_suite, etharp_suite, dhcp_suite,
                                  zenix_suite };
#endif
    size_t i;
    SRunner *sr;
    int failed;
    netd_oos_bytes = 0xffffffff;
    netd_oos_pbufs = 0xffff;
    netd_reass_pbufs = 10;
    /* tests that depend on an option Zenix sets differently (each named with the option) */
    ck_skip("test_ip4_frag", "sends fragments: IP_FRAG is 0 (D19 2.2)");
    ck_skip("test_udp_bind", "binds IPv6 pcbs: IPv6 is off");
    ck_skip("test_dhcp", "counts frames: with ACD off lwIP sends the gratuitous ARP when the lease is bound, the test "
            "expects it later");
    ck_skip("test_dhcp_nak", "counts frames: the gratuitous ARP at once (ACD off), as test_dhcp");
    ck_skip("test_127_0_0_1", "counts frames: with ACD off (D19 6.2) netif_set_up sends a gratuitous ARP");
    ck_skip("test_ip4_icmp_replylen_short", "counts frames: the gratuitous ARP (ACD off)");
    ck_skip("test_ip4_icmp_replylen_first_8", "counts frames: the gratuitous ARP (ACD off)");
    netd_rand_seed(seed);
    lwip_init();
    sr = srunner_create(suites[0]());
    for (i = 1; i < sizeof suites / sizeof suites[0]; i++) srunner_add_suite(sr, suites[i]());
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
