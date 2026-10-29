/* SPDX-License-Identifier: GPL-2.0-only */
/* The 64-bit division (DESIGN D27 section 7): __udivdi3, __umoddi3, __divdi3, __moddi3, which gcc calls for a long long
 * (and picolibc's 64-bit time_t) division. libgcc's own reach a table by an absolute address, which a program with
 * shared code (ZEX2) cannot have, so the C library carries these in both its builds, linked before libgcc. C over the
 * 32-bit divu.l every CPU up to the 68060 has (the 64-bit forms are missing on the 060), no table: a 32-bit dividend
 * and divisor take one divu.l, a divisor of 16 bits four (16-bit digits), any other one a shift-and-subtract loop of at
 * most 64 steps. A division by 0 traps as the 32-bit one does. tools/libc-host-test.sh checks them against the PC's. */
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long s64;

u64 __udivdi3(u64 n, u64 d);
u64 __umoddi3(u64 n, u64 d);
s64 __divdi3(s64 a, s64 b);
s64 __moddi3(s64 a, s64 b);

static u64 udivmod(u64 n, u64 d, u64 *rem)
{
    u32 nh = (u32)(n >> 32), nl = (u32)n, dh = (u32)(d >> 32), dl = (u32)d;
    if (!dh) {
        if (!nh) {                                  /* both 32 bits (a division by 0 traps here) */
            *rem = nl % dl;
            return nl / dl;
        }
        if (dl <= 0xFFFFu) {                        /* four 16-bit digits, each step fits 32 bits */
            u32 r = 0, q[4], dig[4] = { nh >> 16, nh & 0xFFFFu, nl >> 16, nl & 0xFFFFu };
            for (int i = 0; i < 4; i++) {
                u32 t = r << 16 | dig[i];
                q[i] = t / dl;
                r = t % dl;
            }
            *rem = r;
            return (u64)(q[0] << 16 | q[1]) << 32 | (q[2] << 16 | q[3]);
        }
    }
    u64 q = 0;
    int s = 0;
    if (d <= n) {                                   /* line the divisor up under the dividend's top bit */
        while (!(d >> 63) && (d << 1) <= n) {
            d <<= 1;
            s++;
        }
        for (; s >= 0; s--) {
            q <<= 1;
            if (n >= d) {
                n -= d;
                q |= 1;
            }
            d >>= 1;
        }
    }
    *rem = n;
    return q;
}

u64 __udivdi3(u64 n, u64 d) { u64 r; return udivmod(n, d, &r); }
u64 __umoddi3(u64 n, u64 d) { u64 r; udivmod(n, d, &r); return r; }
/* signed: the quotient rounds toward zero, the remainder has the dividend's sign (C's rule) */
s64 __divdi3(s64 a, s64 b)
{
    u64 r, ua = a < 0 ? 0 - (u64)a : (u64)a, ub = b < 0 ? 0 - (u64)b : (u64)b;
    u64 q = udivmod(ua, ub, &r);
    return (a < 0) != (b < 0) ? (s64)(0 - q) : (s64)q;
}
s64 __moddi3(s64 a, s64 b)
{
    u64 r, ua = a < 0 ? 0 - (u64)a : (u64)a, ub = b < 0 ? 0 - (u64)b : (u64)b;
    udivmod(ua, ub, &r);
    return a < 0 ? (s64)(0 - r) : (s64)r;
}
