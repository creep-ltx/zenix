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

/* ---------------------------------------------------------------- formatted output
 * One formatter feeds a "sink": a file descriptor (written in chunks), the buffered standard output, or a memory
 * buffer of limited size. It counts every character it is given, so snprintf returns the full length as C says.
 * Supported: %d %i %u %x %X %o %c %s %p %%, flags - 0 + space #, width and precision as numbers or *, and the
 * length letters h l z t (all the same size here: everything is 32 bits). */
struct sink { int kind, fd; char *buf; u32 max, n, tn; char tmp[512]; };
enum { SK_FD, SK_MEM, SK_OUT };

static void sink_flush(struct sink *k)
{
    if (!k->tn) return;
    if (k->kind == SK_FD) sys_write(k->fd, k->tmp, k->tn);
    else oputn(k->tmp, k->tn);
    k->tn = 0;
}
static void put(struct sink *k, char c)
{
    k->n++;
    if (k->kind == SK_MEM) { if (k->max && k->n < k->max) k->buf[k->n - 1] = c; return; }
    k->tmp[k->tn++] = c;
    if (k->tn == sizeof k->tmp) sink_flush(k);
}
static void pad(struct sink *k, char c, int n) { while (n-- > 0) put(k, c); }

static void fmt_num(struct sink *k, u32 v, u32 base, int neg, int upper, int width, int prec, int zero, int left, int sign, int space, int alt)
{
    char t[34]; int i = 0;
    if (!(v == 0 && prec == 0)) do { u32 d = v % base; t[i++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10); v /= base; } while (v);
    int digits = i; if (prec > digits) digits = prec;
    char pre[3]; int pl = 0;
    if (neg) pre[pl++] = '-'; else if (sign) pre[pl++] = '+'; else if (space) pre[pl++] = ' ';
    if (alt && base == 16 && i) { pre[pl++] = '0'; pre[pl++] = upper ? 'X' : 'x'; }
    int extra0 = (alt && base == 8 && digits == i && i) ? 1 : 0;
    int total = pl + digits + extra0;
    if (!left && !(zero && prec < 0)) pad(k, ' ', width - total);
    for (int j = 0; j < pl; j++) put(k, pre[j]);
    if (!left && zero && prec < 0) pad(k, '0', width - total);
    pad(k, '0', digits - i + extra0);
    while (i--) put(k, t[i]);
    if (left) pad(k, ' ', width - total);
}

static void vfmt(struct sink *k, const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f != '%') { put(k, *f); continue; }
        f++;
        int zero = 0, left = 0, sign = 0, space = 0, alt = 0, width = 0, prec = -1;
        for (;; f++) {
            if (*f == '-') left = 1; else if (*f == '0') zero = 1; else if (*f == '+') sign = 1;
            else if (*f == ' ') space = 1; else if (*f == '#') alt = 1; else break;
        }
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } f++; }
        else while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 't') f++;
        switch (*f) {
        case 'd': case 'i': { int v = va_arg(ap, int);
            fmt_num(k, v < 0 ? -(u32)v : (u32)v, 10, v < 0, 0, width, prec, zero, left, sign, space, 0); break; }
        case 'u': fmt_num(k, va_arg(ap, u32), 10, 0, 0, width, prec, zero, left, 0, 0, 0); break;
        case 'x': fmt_num(k, va_arg(ap, u32), 16, 0, 0, width, prec, zero, left, 0, 0, alt); break;
        case 'X': fmt_num(k, va_arg(ap, u32), 16, 0, 1, width, prec, zero, left, 0, 0, alt); break;
        case 'o': fmt_num(k, va_arg(ap, u32), 8, 0, 0, width, prec, zero, left, 0, 0, alt); break;
        case 'p': fmt_num(k, va_arg(ap, u32), 16, 0, 0, width, prec, zero, left, 0, 0, 1); break;
        case 'c': if (!left) pad(k, ' ', width - 1); put(k, (char)va_arg(ap, int)); if (left) pad(k, ' ', width - 1); break;
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)";
            int l = 0; while (s[l] && (prec < 0 || l < prec)) l++;
            if (!left) pad(k, ' ', width - l);
            for (int j = 0; j < l; j++) put(k, s[j]);
            if (left) pad(k, ' ', width - l);
            break; }
        case '%': put(k, '%'); break;
        case 0: return;
        default: put(k, '?'); break;
        }
    }
}

int vfprintf(int fd, const char *fmt, va_list ap)
{
    struct sink k; k.kind = SK_FD; k.fd = fd; k.n = k.tn = 0;
    vfmt(&k, fmt, ap); sink_flush(&k);
    return (int)k.n;
}
int vsnprintf(char *buf, u32 max, const char *fmt, va_list ap)
{
    struct sink k; k.kind = SK_MEM; k.buf = buf; k.max = max; k.n = 0; k.tn = 0;
    vfmt(&k, fmt, ap);
    if (max) buf[k.n < max ? k.n : max - 1] = 0;
    return (int)k.n;
}
int fprintf(int fd, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}
int printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(1, fmt, ap);
    va_end(ap);
    return n;
}
int snprintf(char *buf, u32 max, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, max, fmt, ap);
    va_end(ap);
    return n;
}
int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, 0x7fffffff, fmt, ap);
    va_end(ap);
    return n;
}
int oprintf(const char *fmt, ...)
{
    struct sink k; k.kind = SK_OUT; k.n = k.tn = 0;
    va_list ap; va_start(ap, fmt);
    vfmt(&k, fmt, ap);
    va_end(ap);
    sink_flush(&k);
    return (int)k.n;
}

/* ---------------------------------------------------------------- more of the C library */
char *strdup(const char *s) { u32 n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strncpy(char *d, const char *s, u32 n) { u32 i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
char *strrchr(const char *s, int c) { const char *r = 0; for (;; s++) { if (*s == (char)c) r = s; if (!*s) break; } return (char *)r; }
int memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *p = a, *q = b;
    while (n--) { if (*p != *q) return *p - *q; p++; q++; }
    return 0;
}
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

unsigned long strtoul(const char *s, char **end, int base)
{
    unsigned long v = 0;
    while (isspace(*s)) s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; base = 16; }
    else if (base == 0 && s[0] == '0') base = 8;
    else if (base == 0) base = 10;
    for (;; s++) {
        int d = isdigit(*s) ? *s - '0' : isalpha(*s) ? tolower(*s) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end) *end = (char *)s;
    return v;
}
long strtol(const char *s, char **end, int base)
{
    while (isspace(*s)) s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    long v = (long)strtoul(s, end, base);
    return neg ? -v : v;
}

/* a shell sort: small, no recursion, fine for the sizes commands see */
void qsort(void *base, u32 n, u32 size, int (*cmp)(const void *, const void *))
{
    char *b = base, tmp[256];
    if (size > sizeof tmp || n < 2) return;
    for (u32 gap = n / 2; gap > 0; gap /= 2)
        for (u32 i = gap; i < n; i++) {
            memcpy(tmp, b + i * size, size);
            u32 j = i;
            while (j >= gap && cmp(b + (j - gap) * size, tmp) > 0) { memcpy(b + j * size, b + (j - gap) * size, size); j -= gap; }
            memcpy(b + j * size, tmp, size);
        }
}

/* ---------------------------------------------------------------- the environment
 * The kernel keeps NAME=value NUL ... NUL NUL per process, inherited by children;
 * the first use copies it into a table the program owns. */
char **environ;
static char env_store[1024];
static char *env_ptrs[64];
static int env_ready;

static void env_load(void)
{
    if (env_ready) return;
    env_ready = 1;
    int n = sys_envget(env_store, sizeof env_store), k = 0;
    if (n > (int)sizeof env_store) n = (int)sizeof env_store;
    for (int i = 0; i < n && env_store[i] && k < 63;) { env_ptrs[k++] = env_store + i; i += (int)strlen(env_store + i) + 1; }
    env_ptrs[k] = 0;
    environ = env_ptrs;
}
static void env_save(void)
{
    static char blk[1024];
    u32 n = 0;
    for (int i = 0; env_ptrs[i]; i++) { u32 l = strlen(env_ptrs[i]); if (n + l + 2 > sizeof blk) break; memcpy(blk + n, env_ptrs[i], l); n += l; blk[n++] = 0; }
    blk[n++] = 0;
    sys_envset(blk, n);
}
char *getenv(const char *name)
{
    env_load();
    u32 l = strlen(name);
    for (int i = 0; env_ptrs[i]; i++) if (!strncmp(env_ptrs[i], name, l) && env_ptrs[i][l] == '=') return env_ptrs[i] + l + 1;
    return 0;
}
int unsetenv(const char *name)
{
    env_load();
    u32 l = strlen(name);
    for (int i = 0; env_ptrs[i]; i++)
        if (!strncmp(env_ptrs[i], name, l) && env_ptrs[i][l] == '=') { for (int k = i; env_ptrs[k]; k++) env_ptrs[k] = env_ptrs[k + 1]; env_save(); return 0; }
    return 0;
}
int setenv(const char *name, const char *value, int overwrite)
{
    env_load();
    if (!overwrite && getenv(name)) return 0;
    unsetenv(name);
    int k = 0; while (env_ptrs[k]) k++;
    if (k >= 63) return -1;
    char *e = malloc(strlen(name) + strlen(value) + 2);
    if (!e) return -1;
    strcpy(e, name); strcat(e, "="); strcat(e, value);
    env_ptrs[k] = e; env_ptrs[k + 1] = 0;
    env_save();
    return 0;
}

/* ---------------------------------------------------------------- buffered line reader */
void rd_init(struct rd *r, int fd) { r->fd = fd; r->pos = r->len = r->eof = 0; }
static int rd_fill(struct rd *r)
{
    if (r->eof) return 0;
    int n = sys_read(r->fd, r->buf, sizeof r->buf);
    if (n <= 0) { r->eof = 1; return 0; }
    r->pos = 0; r->len = n;
    return 1;
}
int rd_getc(struct rd *r)
{
    if (r->pos >= r->len && !rd_fill(r)) return -1;
    return (unsigned char)r->buf[r->pos++];
}
int rd_getline(struct rd *r, char **line, u32 *cap)
{
    u32 n = 0;
    if (!*line) { *cap = 128; *line = malloc(*cap); if (!*line) return -1; }
    int c = rd_getc(r);
    if (c < 0) return -1;
    for (; c >= 0 && c != '\n'; c = rd_getc(r)) {
        if (n + 2 > *cap) { char *nb = malloc(*cap * 2); if (!nb) break; memcpy(nb, *line, n); *line = nb; *cap *= 2; }
        (*line)[n++] = (char)c;
    }
    (*line)[n] = 0;
    return (int)n;
}

/* ---------------------------------------------------------------- buffered output */
static char obuf[4096]; static u32 olen;
void oflush(void) { if (olen) { int r = sys_write(1, obuf, olen); olen = 0; if (r < 0) sys_exit(141); } }   /* the reader is gone: end, as SIGPIPE would */
void oputn(const char *s, u32 n)
{
    if (n >= sizeof obuf) { oflush(); if (sys_write(1, s, n) < 0) sys_exit(141); return; }
    if (olen + n > sizeof obuf) oflush();
    memcpy(obuf + olen, s, n); olen += n;
}
void oputs(const char *s) { oputn(s, strlen(s)); }
void oputc(int c) { char b = (char)c; oputn(&b, 1); }
void _zexit(int status) { oflush(); sys_exit(status); for (;;); }

/* realloc: the small allocator cannot grow in place, so allocate, copy and let the old one go. The old size is
 * not known: copy up to the new size, which is safe because allocations are in one address space with slack
 * only for blocks of at least the old size (callers grow). */
void *realloc(void *p, unsigned long n)
{
    if (!p) return malloc((u32)n);
    void *q = malloc((u32)n);
    if (!q) return 0;
    memcpy(q, p, n);                       /* callers only grow; over-reading the old block is harmless here */
    return q;
}

/* the code of the last failed system call, and the reason to print for it */
int zerrno;
const char *zwhy(const char *dflt) { return zerrno == 13 ? "permission denied" : dflt; }
