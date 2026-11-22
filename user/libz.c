/* SPDX-License-Identifier: GPL-2.0-only */
#include "zshare.h"
/* The allocator, the environment, the buffered output and the error texts are in files of their own that the C
 * library builds too (user/zshare.h: zmalloc.c, zenv.c, zout.c, zerrtab.c); libz.uo is this file and those. */

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n)
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
void *memcpy(void *d, const void *s, size_t n)
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
/* both ends 4-aligned: long words (a quarter of the bus accesses; term's cell model moves its grid on every scroll,
 * as the kernel's console does with its own memmove) */
void *memmove(void *d, const void *s, size_t n)
{
    char *p = d; const char *q = s;
    int lw = n >= 8 && !(((unsigned long)p | (unsigned long)q) & 3);
    if (p < q) {
        if (lw) {
            u32 *pl = (u32 *)p; const u32 *ql = (const u32 *)q;
            for (unsigned long k = n >> 2; k; k--) *pl++ = *ql++;
            p = (char *)pl; q = (const char *)ql; n &= 3;
        }
        while (n--) *p++ = *q++;
    } else if (p != q) {
        p += n; q += n;
        if (lw) {
            for (unsigned long k = n & 3; k; k--) *--p = *--q;   /* the tail first: the same offset in a long */
            u32 *pl = (u32 *)p; const u32 *ql = (const u32 *)q;
            for (unsigned long k = n >> 2; k; k--) *--pl = *--ql;
            p = (char *)pl; q = (const char *)ql;
        } else
            while (n--) *--p = *--q;
    }
    return d;
}
void *memset(void *d, int c, size_t n)
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


/* ---------------------------------------------------------------- formatted output
 * One formatter feeds a "sink": a file descriptor (written in chunks), the buffered standard output, or a memory
 * buffer of limited size. It counts every character it is given, so snprintf returns the full length as C says.
 * Supported: %d %i %u %x %X %o %c %s %p %%, flags - 0 + space #, width and precision as numbers or *, and the
 * length letters h l z t (all the same size here: everything is 32 bits). */
struct sink { int kind, fd; char *buf; u32 max, n, tn; void (*out)(const char *, u32); char tmp[512]; };
enum { SK_FD, SK_MEM, SK_OUT };

static void sink_flush(struct sink *k)
{
    if (!k->tn) return;
    if (k->kind == SK_FD) sys_write(k->fd, k->tmp, k->tn);
    else k->out(k->tmp, k->tn);         /* oprintf's oputn, through a pointer: only oprintf names the buffer */
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

static __attribute__((optimize("Os"))) void fmt_num(struct sink *k, u32 v, u32 base, int neg, int upper, int width, int prec, int zero, int left, int sign, int space, int alt)
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

static __attribute__((optimize("Os"))) void vfmt(struct sink *k, const char *f, va_list ap)
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

int vdprintf(int fd, const char *fmt, va_list ap)
{
    struct sink k; k.kind = SK_FD; k.fd = fd; k.n = k.tn = 0;
    vfmt(&k, fmt, ap); sink_flush(&k);
    return (int)k.n;
}
int vsnprintf(char *buf, size_t max, const char *fmt, va_list ap)
{
    struct sink k; k.kind = SK_MEM; k.buf = buf; k.max = max; k.n = 0; k.tn = 0;
    vfmt(&k, fmt, ap);
    if (max) buf[k.n < max ? k.n : max - 1] = 0;
    return (int)k.n;
}
int dprintf(int fd, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}
int snprintf(char *buf, size_t max, const char *fmt, ...)
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
    struct sink k; k.kind = SK_OUT; k.n = k.tn = 0; k.out = oputn;
    va_list ap; va_start(ap, fmt);
    vfmt(&k, fmt, ap);
    va_end(ap);
    sink_flush(&k);
    return (int)k.n;
}

/* ---------------------------------------------------------------- more of the C library */
char *strdup(const char *s) { u32 n = strlen(s) + 1; char *p = malloc(n); if (p) memcpy(p, s, n); return p; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
char *strrchr(const char *s, int c) { const char *r = 0; for (;; s++) { if (*s == (char)c) r = s; if (!*s) break; } return (char *)r; }
int memcmp(const void *a, const void *b, size_t n)
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
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
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
        if (n + 2 > *cap) { char *nb = realloc(*line, *cap * 2); if (!nb) break; *line = nb; *cap *= 2; }   /* out of memory: the line is cut here */
        (*line)[n++] = (char)c;
    }
    (*line)[n] = 0;
    return (int)n;
}

void _zexit(int status) { if (zout_at_exit) zout_at_exit(); sys_exit(status); for (;;); }

/* malloc that does not come back empty: out of memory ends the program with a message and status 1 */
char **_zargv;
/* The region heap (moved from wasabid, D24; used by wasabid and nfscd, D23), for servers that must stay small: first
 * fit in regions of 16 KB taken from the kernel when needed and given back when empty, neighbours joined on free; a
 * block over a region's size is a region of its own. At most 6 regions (a process has 8, D11's NREG). malloc rounds a
 * block up to a power of two and grows its chunks four times over, which held 112 KB in wasabid's `ps` for one
 * connection where this holds 88 KB (measured in FS-UAE, D24 step 1). */
#define RH_RSZ 16416u                       /* 16 KB and room for four 4 KB blocks (nfscd's pieces, D23) or two 8 KB ones with their headers */
#define RH_NRG 6
#define RH_BUSE 1u                          /* a block's size word: bit 0 in use, bit 1 a region of its own */
#define RH_BOWN 2u
static char *rh_rg[RH_NRG];                /* each starts with its first block's header */
static u32 rh_bsz(const u32 *h) { return h[0] & ~3u; }
#ifdef ZMALLOC_TRACE
void zs_mtrace(char op, u32 a, u32 b, u32 c, int n);
static void *rheap_alloc1(u32 n);
void *rheap_alloc(u32 n) { void *p = rheap_alloc1(n); zs_mtrace('h', n, (u32)p, 0, 2); return p; }
#define rheap_alloc rheap_alloc1
#endif
void *rheap_alloc(u32 n)
{
    u32 need = ((n ? n : 1) + 4 + 7) & ~7u;
    if (need > RH_RSZ) {
        u32 *h = sys_memalloc(need);
        if (!h) return 0;
        h[0] = need | RH_BUSE | RH_BOWN;
        return h + 1;
    }
    for (int pass = 0; pass < 2; pass++)
        for (int r = 0; r < RH_NRG; r++) {
            if (!rh_rg[r]) {
                if (!pass || !(rh_rg[r] = sys_memalloc(RH_RSZ))) continue;
                *(u32 *)rh_rg[r] = RH_RSZ;    /* one free block, the whole region */
            }
            for (u32 o = 0; o < RH_RSZ; o += rh_bsz((u32 *)(rh_rg[r] + o))) {
                u32 *h = (u32 *)(rh_rg[r] + o), sz = rh_bsz(h);
                if ((h[0] & RH_BUSE) || sz < need) continue;
                if (sz - need >= 16) {  /* split: the rest stays free */
                    *(u32 *)((char *)h + need) = sz - need;
                    sz = need;
                }
                h[0] = sz | RH_BUSE;
                return h + 1;
            }
        }
    return 0;
}
u32 rheap_size(const void *p) { return p ? rh_bsz((const u32 *)p - 1) : 0; }
#ifdef ZMALLOC_TRACE
#undef rheap_alloc
static void rheap_free1(void *p);
void rheap_free(void *p) { if (p) zs_mtrace('g', (u32)p, 0, 0, 1); rheap_free1(p); }
#define rheap_free rheap_free1
#endif
void rheap_free(void *p)
{
    u32 *h = (u32 *)p - 1;
    if (!p) return;
    if (h[0] & RH_BOWN) { sys_memfree(h); return; }
    h[0] &= ~RH_BUSE;
    for (int r = 0; r < RH_NRG; r++) {
        if (!rh_rg[r] || (char *)h < rh_rg[r] || (char *)h >= rh_rg[r] + RH_RSZ) continue;
        for (u32 o = 0; o < RH_RSZ; ) {     /* join every run of free neighbours */
            u32 *a = (u32 *)(rh_rg[r] + o), *b;
            while (!(a[0] & RH_BUSE) && o + rh_bsz(a) < RH_RSZ && !((b = (u32 *)((char *)a + rh_bsz(a)))[0] & RH_BUSE))
                a[0] = rh_bsz(a) + rh_bsz(b);
            o += rh_bsz(a);
        }
        if (*(u32 *)rh_rg[r] == RH_RSZ) {     /* all of it free again: back to the kernel */
            sys_memfree(rh_rg[r]);
            rh_rg[r] = 0;
        }
        return;
    }
}

#ifdef ZMALLOC_TRACE
#undef rheap_free
#endif
void *xmalloc(u32 n)
{
    void *p = malloc(n);
    if (!p) { dprintf(2, "%s: out of memory\n", _zargv && _zargv[0] ? _zargv[0] : "libz"); _zexit(1); }
    return p;
}

/* The C side of the program's start: crt0.S calls it with argc and argv where the C rules want them, so no
 * hand-written code reaches a global (in a program with shared code those go through the table in a5, DESIGN D13) */
int main(int argc, char **argv);
void _zstart(int argc, char **argv) __attribute__((noreturn));
void _zstart(int argc, char **argv)
{
    _zargv = argv;
    _zexit(main(argc, argv));
}


/* svctab's and /etc/daemons' line (libz.h) */
int svc_line(char *l, char **name, int *nice, char **prog)
{
    for (char *t = l; *t; t++)
        if (*t == '\t' || *t == '\r') *t = ' ';       /* (a CR: a file edited on another system) */
    for (char *t = l + strlen(l); t > l && t[-1] == ' '; )
        *--t = 0;                                       /* trailing spaces are not part of the program's text */
    char *p = l, *nv;
    while (*p == ' ') p++;
    *name = p;
    if (!*p || *p == '#') return 0;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    while (*p == ' ') p++;
    nv = p;
    if (*p == '-') p++;
    int digits = 0;
    while (*p >= '0' && *p <= '9') { p++; digits++; }
    int sep = *p == ' ' && digits <= 3;                /* a longer number would wrap in atoi */
    if (*p) *p++ = 0;
    while (*p == ' ') p++;
    if (!digits || !sep || !*p) return -EINVAL;
    *nice = atoi(nv);
    *prog = p;
    return 1;
}

/* a basic regular expression (sed, expr :) in the extended syntax regex.c speaks (zhelp.c's); out of memory ends the
 * program with a message (xmalloc's) */
char *bre_to_ere(const char *s)
{
    char *r = zs_bre_to_ere(s);
    if (!r) { dprintf(2, "%s: out of memory\n", _zargv && _zargv[0] ? _zargv[0] : "libz"); _zexit(1); }
    return r;
}

/* priorities (DESIGN D10): the kernel answers getpriority with 20 - nice (1..40), so that no nice value looks like an
 * error number; POSIX callers get the nice value, or -1 with zerrno set (clear zerrno first to tell -1 apart) */
int getpriority(int which, int who)
{
    int r = sys_getpriority(which, who);
    return r < 0 ? -1 : 20 - r;
}
int setpriority(int which, int who, int nice)
{
    return sys_setpriority(which, who, nice) < 0 ? -1 : 0;
}

/* ---- DESIGN D28: UTF-8 as the console decodes it (kernel/term.c's rules), its widths (kernel/cwidth.h) ---- */
#include "../kernel/cwidth.h"

int zu8width(u32 c) { return cw_width(c); }

/* the character at s: a lead and its continuations in Unicode's ranges (no overlong forms, no surrogates, nothing
 * above U+10FFFF) -> its bytes and columns; anything else is one byte of one column */
int zu8next(const char *s, u32 n, int *cols)
{
    const unsigned char *b = (const unsigned char *)s;
    u32 c = b[0], k, cp;
    *cols = 1;
    if (c < 0x80) {
        *cols = c >= 0x20 && c != 0x7F ? 1 : 0;
        return 1;
    }
    if (c < 0xC2 || c > 0xF4)
        return 1;
    k = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (n < k)
        return 1;
    u32 lo = c == 0xE0 ? 0xA0 : c == 0xF0 ? 0x90 : 0x80, hi = c == 0xED ? 0x9F : c == 0xF4 ? 0x8F : 0xBF;
    if (b[1] < lo || b[1] > hi)
        return 1;
    for (u32 i = 2; i < k; i++)
        if ((b[i] & 0xC0) != 0x80)
            return 1;
    cp = k == 2 ? (c & 0x1F) << 6 | (b[1] & 0x3F) :
         k == 3 ? (c & 0x0F) << 12 | (u32)(b[1] & 0x3F) << 6 | (b[2] & 0x3F) :
         (c & 0x07) << 18 | (u32)(b[1] & 0x3F) << 12 | (u32)(b[2] & 0x3F) << 6 | (b[3] & 0x3F);
    int w = cw_width(cp);
    *cols = w < 0 ? 0 : w;
    return (int)k;
}

/* up to 3 bytes back to a lead whose character ends exactly at pos, else one byte */
u32 zu8prev(const char *s, u32 pos)
{
    int cols;
    for (u32 k = 2; k <= 4 && k <= pos; k++) {
        unsigned char b = (unsigned char)s[pos - k];
        if ((b & 0xC0) != 0x80) {
            if (b >= 0xC2 && zu8next(s + pos - k, k, &cols) == (int)k)
                return pos - k;
            break;
        }
    }
    return pos - 1;
}

int zu8cols(const char *s, u32 n)
{
    int c = 0, w;
    for (u32 i = 0; i < n;) {
        i += (u32)zu8next(s + i, n - i, &w);
        c += w;
    }
    return c;
}

u32 zu8fit(const char *s, u32 n, int cols)
{
    u32 i = 0;
    int w;
    while (i < n) {
        int k = zu8next(s + i, n - i, &w);
        if (w > cols)
            break;
        cols -= w;
        i += (u32)k;
    }
    return i;
}

/* DESIGN D60 (docs/d60-full-protection.md 5.5, 11): the peer helpers; no system call while protection is not full */
static signed char prot_seen = -1;
int prot_full(void)
{
    if (prot_seen < 0) {
        u32 w[5];
        prot_seen = (signed char)(sys_meminfo5(w) >= 0 && w[4] == 2);
    }
    return prot_seen;
}
static int peer_ask(struct umsg *m, int op, u32 addr, u32 len)
{
    if (!prot_full())
        return 1;
    struct upeermem pm = { addr, len };
    return len == 0 || sys_peer(m, op, &pm) == 0;
}
int peer_rd(struct umsg *m, u32 addr, u32 len) { return peer_ask(m, PEER_RD, addr, len); }
int peer_wr(struct umsg *m, u32 addr, u32 len) { return peer_ask(m, PEER_WR, addr, len); }
int peer_str(struct umsg *m, u32 addr, u32 max)
{
    u32 pg = 4096 - (addr & 4095);
    return peer_rd(m, addr, max) || (pg < max && peer_rd(m, addr, pg));
}
