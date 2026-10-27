/* SPDX-License-Identifier: GPL-2.0-only */
/* For the linked size of netd's core and lwIP on m68k (tools/host/net-test.sh size): an entry that uses every
 * function netd's main program will call, so --gc-sections keeps what netd keeps and drops the rest. The C library
 * and the OS layer are stand-ins (stubs.c), counted apart: on Zenix they are libz and netd's main program. */
#include "../../../net/netd/netd.h"

typedef void (*fn)(void);
void netd_size_entry(void);
void netd_size_entry(void)
{
    static const volatile fn keep[] = {
        (fn)netd_init, (fn)netd_reseed, (fn)netd_turn, (fn)netd_wait_handles, (fn)netd_limits_profile,
        (fn)netd_socket, (fn)netd_bind, (fn)netd_connect, (fn)netd_listen, (fn)netd_accept, (fn)netd_send,
        (fn)netd_recv, (fn)netd_shutdown, (fn)netd_getsockopt, (fn)netd_setsockopt, (fn)netd_getname,
        (fn)netd_ioctl, (fn)netd_close, (fn)netd_resolve, (fn)netd_bits, (fn)netd_changed,
        (fn)netd_parse_interfaces, (fn)netd_parse_resolv, (fn)netd_hosts_load, (fn)netd_config_trusted,
        (fn)netd_if_up, (fn)netd_if_down, (fn)netd_route_default, (fn)netd_set_dns, (fn)netd_if_info,
        (fn)netd_netstat, (fn)netd_get_stats,
    };
    int i;
    for (i = 0; i < (int)(sizeof keep / sizeof keep[0]); i++) keep[i]();
}
