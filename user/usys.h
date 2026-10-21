#pragma once
/* Zenix system calls: TRAP #0, number in d0, args d1-d3, result in d0. */
typedef unsigned int u32;

static inline int sys3(int n, u32 a, u32 b, u32 c)
{
    register int d0 __asm__("d0") = n;
    register u32 d1 __asm__("d1") = a;
    register u32 d2 __asm__("d2") = b;
    register u32 d3 __asm__("d3") = c;
    __asm__ volatile("trap #0" : "+r"(d0) : "r"(d1), "r"(d2), "r"(d3) : "memory", "cc");
    return d0;
}

static inline void sys_exit(int c) { sys3(1, (u32)c, 0, 0); for (;;); }
static inline int sys_write(int fd, const void *p, u32 n) { return sys3(2, (u32)fd, (u32)p, n); }
static inline void sys_yield(void) { sys3(3, 0, 0, 0); }
static inline int sys_getpid(void) { return sys3(4, 0, 0, 0); }
static inline int sys_spawn(const char *name) { return sys3(5, (u32)name, 0, 0); }
static inline int sys_wait(int pid) { return sys3(6, (u32)pid, 0, 0); }
static inline void sys_sleep(u32 ticks) { sys3(7, ticks, 0, 0); }

static inline u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static inline void puts_(const char *s) { sys_write(1, s, slen(s)); }
static inline void putnum(int v)
{
    char b[12]; int i = 12, neg = v < 0;
    unsigned u = neg ? -(unsigned)v : (unsigned)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    sys_write(1, b + i, (u32)(12 - i));
}
