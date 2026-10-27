/* SPDX-License-Identifier: GPL-2.0-only */
/* Fuzzing netd's input path (DESIGN D19 8.1, 9.1): lwIP with Zenix's configuration and netd's own sockets on top,
 * under libFuzzer (clang -fsanitize=fuzzer,address,undefined), in the spirit of lwIP's test/fuzz (whose sample
 * frames are the seeds of the Ethernet modes). Everything from the line is the fuzzer's:
 *   byte 0              the mode: 0 SLIP (the bytes go through netd's SLIP interface, as from /dev/ttyS0),
 *                       1 Ethernet with a static address, 2 Ethernet with DHCP (netd's DHCP client parses)
 *   then, repeated:     u32 delay (ms, big-endian, at most 10 s), u16 length (big-endian), that many bytes:
 *                       SLIP bytes, or one Ethernet frame
 * Each input starts the same: netd's sockets (a TCP listener on 23, a UDP socket on 5000, an ICMP echo socket, a
 * TCP connection being made to 10.0.2.2:80, a DNS lookup), a fixed random sequence and a clock at 0, so a crash
 * replays. After each chunk netd's turn runs and the sockets are served (accept, read, echo back, close on the end
 * of the stream). At the end of an input everything is closed and the clock runs on 30 s: then every lwIP pool
 * and netd's heap must be back where they were after the start, or the run stops (a leak is a finding too).
 * The checksums are not checked in this build (fuzz-opts.h): almost every mutated packet would fail them. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lwip/opt.h"
#include "lwip/init.h"
#include "lwip/memp.h"
#include "lwip/priv/memp_priv.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include "../../../net/netd/netd.h"
#include "../../../net/port/netd_port.h"

static uint64_t clock_ms;           /* the fake clock, never going back (lwIP's timers) */
static uint64_t input_start;
static uint32_t lcg;
static const uint8_t *in_p;         /* the SLIP bytes of the current chunk */
static size_t in_n;

/* a fixed sequence for every input instead of ChaCha20 (rand.c's is renamed in this build): DNS ids, ports and
 * ISNs the fuzzer can find */
uint32_t netd_rand(void) { lcg = lcg * 1103515245u + 12345u; return lcg >> 1; }
unsigned long netd_os_now_ms(void) { return (unsigned long)clock_ms; }
unsigned long netd_os_clock4us(void) { return (unsigned long)((clock_ms - input_start) * 250); }
int netd_os_serial_open(const char *dev, unsigned long baud) { (void)dev; (void)baud; return 3; }
long netd_os_read(int h, void *buf, unsigned long n)
{
    (void)h;
    if (n > in_n) n = in_n;
    if (!n) return 0;
    memcpy(buf, in_p, n);
    in_p += n;
    in_n -= n;
    return (long)n;
}
long netd_os_write(int h, const void *buf, unsigned long n) { (void)h; (void)buf; return (long)n; }
void netd_os_close(int h) { (void)h; }
void *netd_os_alloc(unsigned long n) { return malloc(n); }
void netd_os_free(void *p) { free(p); }
void netd_os_log(const char *line) { if (getenv("FUZZ_LOG")) fprintf(stderr, "%s\n", line); }
void netd_os_fatal(void) { fprintf(stderr, "netd_os_fatal: an lwIP check failed\n"); abort(); }

static struct netif eth;
static const uint8_t mac[6] = { 0x02, 0x5a, 0x45, 0x4e, 0x49, 0x58 };
static err_t eth_out(struct netif *n, struct pbuf *p) { (void)n; (void)p; return ERR_OK; }
static err_t eth_init(struct netif *n)
{
    n->name[0] = 'e'; n->name[1] = 't';
    n->output = etharp_output;
    n->linkoutput = eth_out;
    n->mtu = 1500;
    n->hwaddr_len = 6;
    memcpy(n->hwaddr, mac, 6);
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    return ERR_OK;
}

static unsigned long heap_base;
static u16_t pool_base[MEMP_MAX];   /* what netd holds for good (its ICMP pcb, the timers) */
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    struct netd_limits l;
    static const unsigned char seed[48] = { 7 };
    (void)argc; (void)argv;
    clock_ms = 1000;
    netd_limits_profile(&l, NETD_PROFILE_SMALL);
    l.heap = 0;                     /* the pools still hold; a heap limit would only hide leaks */
    if (netd_init(&l, seed)) abort();
    {
        unsigned long dns = 0x0302000a;    /* 10.0.2.3 in network order on a little-endian PC */
        unsigned char *b = (unsigned char *)&dns;
        b[0] = 10; b[1] = 0; b[2] = 2; b[3] = 3;
        netd_set_dns(&dns, 1);
    }
    heap_base = netd_mem_used;
    memcpy(pool_base, memp_count, sizeof pool_base);
    return 0;
}

static unsigned gen = 1;
static void sin(unsigned char *a, unsigned ip, unsigned port)
{
    unsigned short fam = NETD_AF_INET;
    memset(a, 0, 16);
    memcpy(a, &fam, 2);
    a[2] = (unsigned char)(port >> 8); a[3] = (unsigned char)port;
    a[4] = (unsigned char)(ip >> 24); a[5] = (unsigned char)(ip >> 16); a[6] = (unsigned char)(ip >> 8); a[7] = (unsigned char)ip;
}

/* serve every socket a little: what a busy program would do */
static void serve(void)
{
    static unsigned char buf[2048];
    unsigned char a[16];
    unsigned alen;
    int i;
    long r;
    alen = 16;
    if (netd_accept(0, gen, 5, gen, a, &alen) == 0) netd_send(5, gen, "welcome\r\n", 9, 0, NULL, 0);
    for (i = 5; i < 7; i++) {
        alen = 16;
        r = netd_recv((unsigned)i, gen, buf, sizeof buf, 0, a, &alen);
        if (r > 0) netd_send((unsigned)i, gen, buf, (unsigned long)r, 0, NULL, 0);
        else if (r == 0) netd_shutdown((unsigned)i, gen, NETD_SHUT_WR);
    }
    alen = 16;
    r = netd_recv(1, gen, buf, sizeof buf, 0, a, &alen);
    if (r > 0) netd_send(1, gen, buf, (unsigned long)r, 0, a, alen);
    alen = 16;
    netd_recv(2, gen, buf, sizeof buf, NETD_MSG_PEEK, a, &alen);
    netd_recv(2, gen, buf, sizeof buf, 0, a, &alen);
    alen = 16;
    r = netd_recv(3, gen, buf, sizeof buf, 0, a, &alen);
    if (netd_bits(3, gen) & NETD_POLLOUT) netd_send(3, gen, "GET / HTTP/1.0\r\n\r\n", 18, 0, NULL, 0);
    netd_recv(4, gen, buf, 16, 0, NULL, NULL);
    {
        struct netd_ready rd[NETD_NSOCK];
        netd_changed(rd, NETD_NSOCK);
    }
}

static void check_empty(void)
{
    int i;
    for (i = 0; i < MEMP_MAX; i++) {
        if (i == MEMP_SYS_TIMEOUT) continue;
        if (memp_count[i] != pool_base[i]) {
            fprintf(stderr, "lwip-fuzz: pool %d holds %u after the input, %u at the start\n", i, memp_count[i], pool_base[i]);
            abort();
        }
    }
    /* lwIP's timers come and go (one-shot ones): their pool is not compared, so neither is their part of the heap
     * (an element and netd_malloc's 8-byte header each) */
    {
        long timers = ((long)memp_count[MEMP_SYS_TIMEOUT] - (long)pool_base[MEMP_SYS_TIMEOUT]) *
                      (long)(MEMP_SIZE + MEMP_ALIGN_SIZE(memp_pools[MEMP_SYS_TIMEOUT]->size) + 8);
        if ((long)netd_mem_used - timers != (long)heap_base) {
            fprintf(stderr, "lwip-fuzz: netd's heap holds %lu bytes (%ld of them timers), %lu at the start\n",
                    netd_mem_used, timers, heap_base);
            abort();
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    unsigned char a[16];
    struct netd_ifcfg c;
    int mode, i;
    size_t pos = 1;
    if (size < 1) return 0;
    mode = data[0] % 3;
    lcg = 12345;
    input_start = clock_ms;
    gen = (gen + 1) & 0x7fff;
    if (!gen) gen = 1;

    if (mode == 0) {
        memset(&c, 0, sizeof c);
        strcpy(c.name, "sl0");
        strcpy(c.driver, "slip");
        strcpy(c.args, "fuzz 115200");
        c.method = NETD_INET_STATIC;
        c.addr = 0x0f02000a; c.mask = 0x00ffffff; c.gateway = 0x0202000a;   /* 10.0.2.15/24 via 10.0.2.2, LE */
        if (netd_if_up(&c)) abort();
    } else {
        ip4_addr_t ip, mask, gw;
        IP4_ADDR(&ip, 10, 0, 2, 15); IP4_ADDR(&mask, 255, 255, 255, 0); IP4_ADDR(&gw, 10, 0, 2, 2);
        if (mode == 2) { ip4_addr_set_zero(&ip); ip4_addr_set_zero(&mask); ip4_addr_set_zero(&gw); }
        netif_add(&eth, &ip, &mask, &gw, NULL, eth_init, ethernet_input);
        netif_set_default(&eth);
        netif_set_up(&eth);
        netif_set_link_up(&eth);
        if (mode == 2) dhcp_start(&eth);
    }
    netd_socket(0, gen, NETD_AF_INET, NETD_SOCK_STREAM, 0, 0);
    sin(a, 0, 23); netd_bind(0, gen, a, 16, 0);
    netd_listen(0, gen, 4);
    netd_socket(1, gen, NETD_AF_INET, NETD_SOCK_DGRAM, 0, 1000);
    sin(a, 0, 5000); netd_bind(1, gen, a, 16, 1000);
    netd_socket(2, gen, NETD_AF_INET, NETD_SOCK_DGRAM, NETD_IPPROTO_ICMP, 1000);
    sin(a, 0x0a000202, 0);
    netd_send(2, gen, "\x08\0\0\0\0\0\0\1ping", 12, 0, a, 16);
    netd_socket(3, gen, NETD_AF_INET, NETD_SOCK_STREAM, 0, 1000);
    sin(a, 0x0a000202, 80); netd_connect(3, gen, a, 16);
    netd_resolve(4, gen, "fuzz.zenix.test", 1000);
    netd_turn();

    while (pos + 6 <= size) {
        uint32_t delay = (uint32_t)data[pos] << 24 | (uint32_t)data[pos + 1] << 16 | (uint32_t)data[pos + 2] << 8 | data[pos + 3];
        size_t len = (size_t)data[pos + 4] << 8 | data[pos + 5];
        pos += 6;
        if (len > size - pos) len = size - pos;
        clock_ms += delay % 10000;
        if (mode == 0) {
            in_p = data + pos; in_n = len;
            netd_turn();
        } else if (len) {
            struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)(len > 1514 ? 1514 : len), PBUF_POOL);
            if (p) {
                pbuf_take(p, data + pos, p->tot_len);
                if (eth.input(p, &eth) != ERR_OK) pbuf_free(p);
            }
            netd_turn();
        }
        pos += len;
        serve();
    }

    /* the end: close everything, let the timers run out, then nothing may be left */
    for (i = 0; i < 7; i++) netd_close((unsigned)i, gen);
    if (mode == 0) netd_if_down("sl0");
    else {
        if (mode == 2) { dhcp_stop(&eth); dhcp_cleanup(&eth); }
        netif_set_down(&eth);
        netif_poll(&eth);               /* its loopback queue (as netd_if_down) */
        netif_remove(&eth);
    }
    {
        struct tcp_pcb *p;
        while ((p = tcp_active_pcbs)) tcp_abort(p);
        while ((p = tcp_tw_pcbs)) tcp_abort(p);
    }
    for (i = 0; i < 120; i++) { clock_ms += 250; sys_check_timeouts(); }
    netd_turn();
    check_empty();
    return 0;
}
