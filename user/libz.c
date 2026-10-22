#include "libz.h"

u32 strlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, u32 n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)); return r; }
char *strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c ? 0 : (char *)s;
}
char *strstr(const char *h, const char *n)
{
    u32 l = strlen(n);
    if (!l) return (char *)h;
    for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h;
    return 0;
}
void *memcpy(void *d, const void *s, unsigned long n)
{
    char *p = d; const char *q = s;
    if (n >= 8 && !(((unsigned long)p | (unsigned long)q) & 3)) {      /* aligned: longwords */
        u32 *pl = (u32 *)p; const u32 *ql = (const u32 *)q;
        for (unsigned long k = n >> 2; k; k--) *pl++ = *ql++;
        p = (char *)pl; q = (const char *)ql; n &= 3;
    }
    while (n--) *p++ = *q++;
    return d;
}
void *memmove(void *d, const void *s, unsigned long n)
{
    char *p = d; const char *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
void *memset(void *d, int c, unsigned long n)
{
    char *p = d;
    if (n >= 8 && !((unsigned long)p & 3)) {
        u32 v = (u32)(unsigned char)c * 0x01010101u, *pl = (u32 *)p;
        for (unsigned long k = n >> 2; k; k--) *pl++ = v;
        p = (char *)pl; n &= 3;
    }
    while (n--) *p++ = (char)c;
    return d;
}
int atoi(const char *s)
{
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* malloc: bump allocation inside 32 KB chunks from the kernel; big blocks
 * get their own region. free() only returns big blocks. */
static char *chunk; static u32 left;
void *malloc(u32 n)
{
    n = (n + 7) & ~7u;
    if (n >= 8192) return sys_memalloc(n);
    if (n > left) {
        chunk = sys_memalloc(32768);
        if (!chunk) return 0;
        left = 32768;
    }
    void *p = chunk; chunk += n; left -= n;
    return p;
}
void free(void *p) { if (p) sys_memfree(p); }

static int emit(char *b, int n, char c) { if (n < 255) b[n] = c; return n + 1; }

static int fmtnum(char *b, int n, u32 v, u32 base, int neg, int width, int zero, int left_adj)
{
    char t[12]; int i = 0;
    do { u32 d = v % base; t[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= base; } while (v);
    if (neg) t[i++] = '-';
    int pad = width > i ? width - i : 0;
    if (!left_adj && !zero) while (pad--) n = emit(b, n, ' ');
    else if (!left_adj) { if (neg) { n = emit(b, n, '-'); i--; } while (pad--) n = emit(b, n, '0'); }
    while (i--) n = emit(b, n, t[i]);
    if (left_adj) while (pad--) n = emit(b, n, ' ');
    return n;
}

static int vfmt(char *b, const char *f, va_list ap)
{
    int n = 0;
    for (; *f; f++) {
        if (*f != '%') { n = emit(b, n, *f); continue; }
        f++;
        int zero = 0, left_adj = 0, width = 0;
        if (*f == '-') { left_adj = 1; f++; }
        if (*f == '0') { zero = 1; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        switch (*f) {
        case 'd': { int v = va_arg(ap, int);
            n = fmtnum(b, n, v < 0 ? -(u32)v : (u32)v, 10, v < 0, width, zero, left_adj); break; }
        case 'u': n = fmtnum(b, n, va_arg(ap, u32), 10, 0, width, zero, left_adj); break;
        case 'x': n = fmtnum(b, n, va_arg(ap, u32), 16, 0, width, zero, left_adj); break;
        case 'c': n = emit(b, n, (char)va_arg(ap, int)); break;
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)";
            int l = (int)strlen(s), pad = width > l ? width - l : 0;
            if (!left_adj) while (pad--) n = emit(b, n, ' ');
            while (*s) n = emit(b, n, *s++);
            if (left_adj) while (pad--) n = emit(b, n, ' ');
            break; }
        case '%': n = emit(b, n, '%'); break;
        default: n = emit(b, n, '?'); break;
        }
    }
    return n;
}

int fprintf(int fd, const char *fmt, ...)
{
    char b[256]; va_list ap;
    va_start(ap, fmt);
    int n = vfmt(b, fmt, ap);
    va_end(ap);
    if (n > 255) n = 255;
    return sys_write(fd, b, (u32)n);
}
int printf(const char *fmt, ...)
{
    char b[256]; va_list ap;
    va_start(ap, fmt);
    int n = vfmt(b, fmt, ap);
    va_end(ap);
    if (n > 255) n = 255;
    return sys_write(1, b, (u32)n);
}
