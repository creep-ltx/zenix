#pragma once
/* Zenix user-side syscall stubs.
 * TRAP #0: num in d0, args in d1-d3, result in d0. */

#define SYS_exit   1
#define SYS_putc   2
#define SYS_yield  3
#define SYS_getpid 4
#define SYS_fork   5
#define SYS_exec   6
#define SYS_wait   7
#define SYS_open   8
#define SYS_close  9
#define SYS_read    10
#define SYS_write   11
#define SYS_lseek   12
#define SYS_readdir 13
#define SYS_brk     14
#define SYS_signal  15
#define SYS_kill    16
#define SYS_sigreturn 17
#define SYS_setfg   18
#define SYS_setkmap 19

static inline long __sys(long n, long a, long b, long c)
{
    register long d0 asm("d0") = n;
    register long d1 asm("d1") = a;
    register long d2 asm("d2") = b;
    register long d3 asm("d3") = c;
    asm volatile("trap #0" : "+d"(d0) : "d"(d1), "d"(d2), "d"(d3) : "memory");
    return d0;
}

static inline void sys_exit(int code) { __sys(SYS_exit, code, 0, 0); }
static inline void sys_putc(char ch)  { __sys(SYS_putc, ch, 0, 0); }
static inline void sys_yield(void)    { __sys(SYS_yield, 0, 0, 0); }
static inline long sys_getpid(void)   { return __sys(SYS_getpid, 0, 0, 0); }
static inline long sys_fork(void)     { return __sys(SYS_fork, 0, 0, 0); }
static inline long sys_exec(const char *path, char *const argv[]) { return __sys(SYS_exec, (long)path, (long)argv, 0); }
static inline long sys_brk(long addr)  { return __sys(SYS_brk, addr, 0, 0); }
static inline long sys_signal(long sig, void (*h)(int)) { return __sys(SYS_signal, sig, (long)h, 0); }
static inline long sys_kill(long pid, long sig) { return __sys(SYS_kill, pid, sig, 0); }
static inline long sys_setfg(long pid) { return __sys(SYS_setfg, pid, 0, 0); }
static inline long sys_wait(int *status) { return __sys(SYS_wait, (long)status, 0, 0); }
static inline long sys_open(const char *path, long mode) { return __sys(SYS_open, (long)path, mode, 0); }
static inline long sys_close(long fd) { return __sys(SYS_close, fd, 0, 0); }
static inline long sys_read(long fd, void *buf, long n)  { return __sys(SYS_read, fd, (long)buf, n); }
static inline long sys_write(long fd, const void *buf, long n) { return __sys(SYS_write, fd, (long)buf, n); }

/* tiny libc-ish helpers until newlib (M4) */
static inline long ustrlen(const char *s)
{
    long n = 0;
    while (s[n])
        n++;
    return n;
}

static inline void uputs(const char *s)
{
    sys_write(1, s, ustrlen(s));
}

static inline void uputnum(long v)
{
    char b[12];
    int i = 0;
    unsigned long u;
    if (v < 0) {
        uputs("-");
        u = (unsigned long)-v;
    } else
        u = (unsigned long)v;
    do {
        b[i++] = '0' + (char)(u % 10);
        u /= 10;
    } while (u);
    while (i--)
        sys_write(1, &b[i], 1);
}
