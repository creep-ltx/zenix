/* SPDX-License-Identifier: GPL-2.0-only */
/* netd's random numbers (DESIGN D19 9.3): ChaCha20 (RFC 8439) with fast key erasure for LWIP_RAND (DNS ids and
 * ports, ephemeral ports, ICMP echo ids), and TCP's initial sequence numbers by RFC 6528: SipHash-2-4 keyed with
 * 128 random bits over the connection's addresses and ports, plus a clock that ticks every 4 microseconds.
 * netd seeds both from /dev/urandom before lwip_init and again when /dev/random says the pool is seeded, then
 * every few minutes (netd_rand_seed). Nothing here reads a device: the caller hands the bytes in. */
#include "lwip/opt.h"
#include "lwip/ip_addr.h"
#include "netd_port.h"
#include "netd_rand.h"

static u32_t key[8];            /* ChaCha20's key; replaced by the first half of every block (fast key erasure) */
static u32_t nonce_ctr;         /* counts blocks, so a key is never used twice even before its erasure */
static u8_t out[32];            /* the second half of the last block, given out from the end */
static unsigned outn;
static u32_t sipkey[4];         /* the ISN key */

static u32_t rd32(const u8_t *p) { return (u32_t)p[0] | (u32_t)p[1] << 8 | (u32_t)p[2] << 16 | (u32_t)p[3] << 24; }
static void wr32(u8_t *p, u32_t v) { p[0] = (u8_t)v; p[1] = (u8_t)(v >> 8); p[2] = (u8_t)(v >> 16); p[3] = (u8_t)(v >> 24); }
#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) a += b, d ^= a, d = ROTL32(d, 16), c += d, b ^= c, b = ROTL32(b, 12), \
                       a += b, d ^= a, d = ROTL32(d, 8), c += d, b ^= c, b = ROTL32(b, 7)

void netd_chacha20_block(const u32_t k[8], u32_t counter, const u32_t n[3], u8_t blk[64])
{
    u32_t s[16], x[16];
    int i;
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (i = 0; i < 8; i++) s[4 + i] = k[i];
    s[12] = counter; s[13] = n[0]; s[14] = n[1]; s[15] = n[2];
    for (i = 0; i < 16; i++) x[i] = s[i];
    for (i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]); QR(x[1], x[5], x[9], x[13]); QR(x[2], x[6], x[10], x[14]); QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]); QR(x[1], x[6], x[11], x[12]); QR(x[2], x[7], x[8], x[13]); QR(x[3], x[4], x[9], x[14]);
    }
    for (i = 0; i < 16; i++) wr32(blk + 4 * i, x[i] + s[i]);
}

static void refill(void)
{
    static const u32_t zero[3];
    u8_t blk[64];
    int i;
    netd_chacha20_block(key, nonce_ctr++, zero, blk);
    for (i = 0; i < 8; i++) key[i] = rd32(blk + 4 * i);
    for (i = 0; i < 32; i++) out[i] = blk[32 + i];
    for (i = 0; i < 64; i++) blk[i] = 0;
    outn = 32;
}

void netd_rand_seed(const u8_t seed[NETD_SEED_BYTES])
{
    int i;
    /* the new key is mixed with the old one (a weak new seed never makes the stream worse) */
    for (i = 0; i < 8; i++) key[i] ^= rd32(seed + 4 * i);
    refill();
    for (i = 0; i < 4; i++) sipkey[i] = rd32(seed + 32 + 4 * i) ^ netd_rand();
}

u32_t netd_rand(void)
{
    u32_t v;
    if (outn < 4) refill();
    outn -= 4;
    v = rd32(out + outn);
    wr32(out + outn, 0);
    return v;
}

/* SipHash-2-4 (Aumasson and Bernstein), 64-bit lanes as pairs of 32-bit words: the 68020 has no 64-bit
 * arithmetic, and gcc's long long code for it is large */
typedef struct { u32_t hi, lo; } w64;
static w64 add(w64 a, w64 b) { w64 r; r.lo = a.lo + b.lo; r.hi = a.hi + b.hi + (r.lo < a.lo); return r; }
static w64 xr(w64 a, w64 b) { w64 r; r.hi = a.hi ^ b.hi; r.lo = a.lo ^ b.lo; return r; }
static w64 rotl(w64 a, int n)
{
    w64 r;
    if (n >= 32) { u32_t t = a.hi; a.hi = a.lo; a.lo = t; n -= 32; }
    if (n == 0) return a;
    r.hi = (a.hi << n) | (a.lo >> (32 - n));
    r.lo = (a.lo << n) | (a.hi >> (32 - n));
    return r;
}
static void sipround(w64 v[4])
{
    v[0] = add(v[0], v[1]); v[1] = rotl(v[1], 13); v[1] = xr(v[1], v[0]); v[0] = rotl(v[0], 32);
    v[2] = add(v[2], v[3]); v[3] = rotl(v[3], 16); v[3] = xr(v[3], v[2]);
    v[0] = add(v[0], v[3]); v[3] = rotl(v[3], 21); v[3] = xr(v[3], v[0]);
    v[2] = add(v[2], v[1]); v[1] = rotl(v[1], 17); v[1] = xr(v[1], v[2]); v[2] = rotl(v[2], 32);
}

void netd_siphash24(const u32_t k[4], const u8_t *m, unsigned len, u8_t res[8])
{
    w64 v[4], k0, k1, mi, f;
    unsigned i, left = len & 7;
    k0.lo = k[0]; k0.hi = k[1]; k1.lo = k[2]; k1.hi = k[3];
    v[0].hi = 0x736f6d65; v[0].lo = 0x70736575; v[1].hi = 0x646f7261; v[1].lo = 0x6e646f6d;
    v[2].hi = 0x6c796765; v[2].lo = 0x6e657261; v[3].hi = 0x74656462; v[3].lo = 0x79746573;
    v[0] = xr(v[0], k0); v[1] = xr(v[1], k1); v[2] = xr(v[2], k0); v[3] = xr(v[3], k1);
    for (i = 0; i + 8 <= len; i += 8) {
        mi.lo = rd32(m + i); mi.hi = rd32(m + i + 4);
        v[3] = xr(v[3], mi); sipround(v); sipround(v); v[0] = xr(v[0], mi);
    }
    mi.lo = 0; mi.hi = (u32_t)(len & 0xff) << 24;
    for (; left; left--) {
        u32_t b = m[i + left - 1];
        if (left > 4) mi.hi |= b << (8 * (left - 5)); else mi.lo |= b << (8 * (left - 1));
    }
    v[3] = xr(v[3], mi); sipround(v); sipround(v); v[0] = xr(v[0], mi);
    v[2].lo ^= 0xff;
    sipround(v); sipround(v); sipround(v); sipround(v);
    f = xr(xr(v[0], v[1]), xr(v[2], v[3]));
    wr32(res, f.lo); wr32(res + 4, f.hi);
}

/* RFC 6528: ISN = M + F(localip, localport, remoteip, remoteport, secretkey), M the 4 microsecond clock */
u32_t netd_tcp_isn(const ip_addr_t *local_ip, u16_t local_port, const ip_addr_t *remote_ip, u16_t remote_port)
{
    u8_t m[12], h[8];
    u32_t l = ip4_addr_get_u32(ip_2_ip4(local_ip)), r = ip4_addr_get_u32(ip_2_ip4(remote_ip));
    wr32(m, l); wr32(m + 4, r);
    m[8] = (u8_t)local_port; m[9] = (u8_t)(local_port >> 8); m[10] = (u8_t)remote_port; m[11] = (u8_t)(remote_port >> 8);
    netd_siphash24(sipkey, m, sizeof m, h);
    return rd32(h) + netd_os_clock4us();
}
