/* SPDX-License-Identifier: GPL-2.0-only */
/* netd's core: start, the limits by the machine, the turn of the main loop, the log (DESIGN D19 7.3, 9.3, 9.4, 9.5). */
#include <stdarg.h>
#ifdef NETD_HOST
#include <stdio.h>
#endif
#include "lwip/init.h"
#include "lwip/memp.h"
#include "lwip/timeouts.h"
#include "lwip/tcp.h"
#include "netd_int.h"
#include "netd_port.h"
#include "netd_rand.h"

struct netd_limits netd_lim;
struct netd_stats netd_st;

void netd_logf(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    netd_os_log(line);
}

/* lwIP's asserts report only their line (net/port/arch/cc.h): log it and end; the service manager starts netd
 * again (D11), and every socket of this netd fails with -ENETDOWN (D19 4.3 rule 10) */
void netd_lwip_assert(int line)
{
    netd_logf("netd: lwIP check failed at line %d", line);
    netd_os_fatal();
}

void netd_mem_refused(void) { netd_st.mem_refused++; }

/* D19 7.3: the stock A1200 on SLIP (16-24 KB of data: MSS 966, a window of 2 x MSS, a 4 KB send buffer, 8 TCP
 * connections, 16 buffers) and an Ethernet machine (100-200 KB: MSS 1460, a 16-32 KB window, 64-128 receive
 * buffers). The numbers 7.3 does not give are this file's choice. */
void netd_limits_profile(struct netd_limits *l, int profile)
{
    if (profile == NETD_PROFILE_LARGE) {
        l->tcp_pcbs = 32; l->tcp_listen = 8; l->tcp_segs = 256; l->udp_pcbs = 16;
        l->pbufs = 64; l->ref_pbufs = 64; l->reass = 4; l->reass_pbufs = 32; l->arp_queue = 16;
        l->tcp_wnd = 16384; l->tcp_snd_buf = 16384; l->oos_bytes = 16384; l->oos_pbufs = 16;
        l->backlog = 16; l->udp_rxq = 32; l->udp_rxq_bytes = 32768; l->heap = 200 * 1024ul;
    } else {
        l->tcp_pcbs = 8; l->tcp_listen = 4; l->tcp_segs = 32; l->udp_pcbs = 8;
        l->pbufs = 16; l->ref_pbufs = 16; l->reass = 2; l->reass_pbufs = 8; l->arp_queue = 4;
        l->tcp_wnd = 2 * 966; l->tcp_snd_buf = 4096; l->oos_bytes = 2 * 966; l->oos_pbufs = 4;
        l->backlog = 4; l->udp_rxq = 8; l->udp_rxq_bytes = 4096; l->heap = 24 * 1024ul;
    }
}

/* the checks lwIP makes at compile time for its sizes, made here because netd's are variables (lwipopts.h) */
static int limits_ok(const struct netd_limits *l)
{
    unsigned mss = TCP_MSS;
    if (l->tcp_pcbs < 1 || l->tcp_listen < 1 || l->udp_pcbs < 2 || l->pbufs < 2 || l->tcp_segs < 2) return 0;
    if (l->tcp_wnd < 536 || l->tcp_snd_buf < 2 * 536) return 0;                  /* at least two minimum segments */
    if ((unsigned long)TCP_SND_QUEUELEN < 2ul * (l->tcp_snd_buf / 536)) return 0;   /* TCP_SND_QUEUELEN's rule */
    if (l->reass > l->reass_pbufs) return 0;                                    /* each reassembly holds 2 pbufs */
    if (l->backlog < 1 || l->backlog > 255 || l->udp_rxq < 1) return 0;
    (void)mss;
    return 1;
}

static void apply(const struct netd_limits *l)
{
    memp_limit[MEMP_TCP_PCB] = l->tcp_pcbs;
    memp_limit[MEMP_TCP_PCB_LISTEN] = l->tcp_listen;
    memp_limit[MEMP_TCP_SEG] = l->tcp_segs;
    memp_limit[MEMP_UDP_PCB] = l->udp_pcbs;
    memp_limit[MEMP_RAW_PCB] = 2;
    memp_limit[MEMP_PBUF_POOL] = l->pbufs;
    memp_limit[MEMP_PBUF] = l->ref_pbufs;
    memp_limit[MEMP_REASSDATA] = l->reass;
#if ARP_QUEUEING
    memp_limit[MEMP_ARP_QUEUE] = l->arp_queue;
#endif
    netd_tcp_wnd = l->tcp_wnd;
    netd_tcp_snd_buf = l->tcp_snd_buf;
    netd_oos_bytes = l->oos_bytes;
    netd_oos_pbufs = l->oos_pbufs;
    netd_reass_pbufs = l->reass_pbufs;
    netd_mem_limit = l->heap;
}

int netd_init(const struct netd_limits *l, const unsigned char seed[48])
{
    if (!limits_ok(l)) return -EINVAL;
    netd_lim = *l;
    /* the keys before lwip_init (9.3): its first ephemeral ports and DNS ids are drawn from them */
    netd_rand_seed(seed);
    lwip_init();
    apply(l);
    netd_sock_init();
    netd_if_init();
    return 0;
}

void netd_reseed(const unsigned char seed[48]) { netd_rand_seed(seed); }

unsigned long netd_turn(void)
{
    u32_t t;
    netd_if_turn();
    netif_poll_all();               /* the loopback interface's queue */
    netd_in_timers = 1;             /* a name lookup that ends here got no answer (sock.c, dns_done) */
    sys_check_timeouts();
    netd_in_timers = 0;
    netd_sock_turn();
    netd_if_turn();                 /* what the timers and the loopback sent */
    t = sys_timeouts_sleeptime();
    {
        struct netif *n;
        NETIF_FOREACH(n) if (n->loop_first) return 0;     /* the loopback has more: no sleep */
    }
    return t > 1000 ? 1000 : t;
}

int netd_wait_handles(int *h, int *want_write, int max) { return netd_if_handles(h, want_write, max); }

void netd_get_stats(struct netd_stats *s) { *s = netd_st; }
