/* SPDX-License-Identifier: GPL-2.0-only */
/* The rest of lwIP's port for netd (DESIGN D19 2.2, 9.4): sys_now, the asserts, the counted heap, and the sizes
 * netd sets by the machine (their defaults here are the stock A1200's numbers of 7.3). */
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "netd_port.h"

unsigned short netd_tcp_wnd = 2 * 966;
unsigned short netd_tcp_snd_buf = 4096;
unsigned long netd_oos_bytes = 4 * 966;
unsigned short netd_oos_pbufs = 4;
unsigned short netd_reass_pbufs = 8;
unsigned long netd_mem_used, netd_mem_limit;

u32_t sys_now(void) { return (u32_t)netd_os_now_ms(); }

/* every block carries its size in front, so the count is exact; 8 bytes keep the 8-byte alignment of the heap */
#define HDR 8
void *netd_malloc(unsigned long n)
{
    unsigned char *p;
    if (n > 0x7fffff00ul || (netd_mem_limit && netd_mem_used + n + HDR > netd_mem_limit)) return NULL;
    p = malloc(n + HDR);
    if (!p) return NULL;
    *(unsigned long *)p = n;
    netd_mem_used += n + HDR;
    return p + HDR;
}
void *netd_calloc(unsigned long count, unsigned long size)
{
    void *p;
    if (size && count > 0x7fffff00ul / size) return NULL;
    p = netd_malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
void netd_free(void *v)
{
    unsigned char *p = v;
    if (!p) return;
    p -= HDR;
    netd_mem_used -= *(unsigned long *)p + HDR;
    free(p);
}
