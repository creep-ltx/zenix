/* SPDX-License-Identifier: GPL-2.0-only */
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

/* ---------------------------------------------------------------- memory
 * malloc over the kernel's SYS_memalloc, which gives a process at most NREG (8) extra regions, so small blocks must
 * not use one each. Every block has an 8-byte header: its capacity (the bytes after the header) and a tag saying
 * whether it is a small block in use, a big block in use or a free block.
 *  - Small blocks hold 16, 32, 64 ... 32768 bytes (size classes; the header comes on top, so a request of exactly a
 *    power of two, as buffers and doubling arrays usually are, fills its class). They are cut from chunks the kernel
 *    gives: the first is 4 KB (a program that needs little, such as login waiting for its shell, holds little), each
 *    next one four times the last, up to 512 KB (smaller if memory is short), so a program that needs a lot still
 *    uses few regions (4 + 16 + 64 + 256 + 512 KB in five). A freed small block goes on its class's free list and is used again;
 *    it is never given back to the kernel (that happens when the program ends). O(1), no coalescing; a class that is
 *    empty carves a block of a larger class before taking new chunk space.
 *  - Bigger blocks get a region of their own, and free() gives exactly those back with SYS_memfree.
 * free() and realloc() check the tag: a pointer malloc did not give out (or one freed twice) is reported on
 * standard error and ignored instead of corrupting the lists. */
#define M_SMALL   0x5a4d5300u           /* "ZMS" + class: a small block in use */
#define M_BIG     0x5a4d4247u           /* "ZMBG": a block in its own region */
#define M_FREE    0x5a4d4652u           /* "ZMFR": on a free list */
#define NCLASS    12                    /* capacities 16 << 0 .. 16 << 11 = 32 KB */
#define CAP(k)    (16u << (k))
#define BLK(k)    (CAP(k) + 8u)         /* a class's block: capacity + header */
#define CHUNK_MIN 4096u
#define CHUNK_MAX (512u * 1024)
#define MALLOC_MAX 0x3FFFFFF0u          /* a request above this is refused (no size arithmetic below can wrap) */
struct mhdr { u32 cap, tag; };
static struct mhdr *freel[NCLASS];      /* free blocks; the next pointer is the first word after the header */
static char *chunk; static u32 left;    /* the unused end of the newest chunk */
static u32 next_chunk = CHUNK_MIN;

static void mfree_put(struct mhdr *h, int k)
{
    h->cap = CAP(k);
    h->tag = M_FREE;
    *(struct mhdr **)(h + 1) = freel[k];
    freel[k] = h;
}
/* memory that is not needed now (the end of a chunk, the rest of a carved block): cut it into free blocks of the
 * largest classes that fit, so none of it is lost */
static void carve(char *p, u32 n)
{
    while (n >= BLK(0)) {
        int k = NCLASS - 1;
        while (BLK(k) > n) k--;
        mfree_put((struct mhdr *)p, k);
        p += BLK(k); n -= BLK(k);
    }
}
static struct mhdr *small_get(int k)
{
    u32 sz = BLK(k);
    struct mhdr *h = freel[k];
    if (h) { freel[k] = *(struct mhdr **)(h + 1); return h; }
    if (left < sz) {
        for (int j = k + 1; j < NCLASS; j++)            /* a free block of a larger class: take ours from its start */
            if (freel[j]) {
                h = freel[j]; freel[j] = *(struct mhdr **)(h + 1);
                carve((char *)h + sz, BLK(j) - sz);
                return h;
            }
        u32 want = next_chunk;
        while (want < sz) want *= 2;
        char *c;
        while (!(c = sys_memalloc(want)) && want / 2 >= sz) want /= 2;  /* smaller if memory is short */
        if (!c && !(c = sys_memalloc(want = sz))) return 0;             /* last try: just this block */
        carve(chunk, left);                             /* the old chunk's end, before it is forgotten */
        chunk = c; left = want;
        if (want >= next_chunk) next_chunk = want < CHUNK_MAX / 4 ? want * 4 : CHUNK_MAX;   /* not after a short one */
    }
    h = (struct mhdr *)chunk; chunk += sz; left -= sz;
    return h;
}
void *malloc(u32 n)
{
    if (n > MALLOC_MAX) return 0;
    struct mhdr *h;
    if (n <= CAP(NCLASS - 1)) {
        int k = 0;
        while (CAP(k) < n) k++;
        if (!(h = small_get(k))) return 0;
        h->cap = CAP(k);
        h->tag = M_SMALL | (u32)k;
    } else {
        u32 need = (n + sizeof *h + 15) & ~15u;
        if (!(h = sys_memalloc(need))) return 0;
        h->cap = need - sizeof *h;
        h->tag = M_BIG;
    }
    return h + 1;
}
/* the header of a block malloc gave out, or 0 (reported) */
static struct mhdr *mhdr_of(void *p, const char *who)
{
    struct mhdr *h = (struct mhdr *)p - 1;
    if (((u32)p & 7) || !(h->tag == M_BIG || (h->tag & ~0xFFu) == M_SMALL)) {
        fprintf(2, "libz: %s() of a pointer malloc did not give out (or freed twice): %p\n", who, p);
        return 0;
    }
    return h;
}
void free(void *p)
{
    if (!p) return;
    struct mhdr *h = mhdr_of(p, "free");
    if (!h) return;
    if (h->tag == M_BIG) { h->tag = M_FREE; sys_memfree(h); }
    else mfree_put(h, (int)(h->tag & 0xFF));
}
/* realloc: in place when the block is already big enough, else a new block with the old contents (only as many
 * bytes as the old block holds) and the old one freed. On failure the old block is left as it was. */
/* zeroed n * size bytes; NULL when the product overflows or there is no memory (OpenSSH's bcrypt_pbkdf, D21) */
void *calloc(unsigned long n, unsigned long size)
{
    if (size && n > 0xFFFFFFFFu / size) return 0;
    void *p = malloc((u32)(n * size));
    if (p) memset(p, 0, n * size);
    return p;
}
void *realloc(void *p, unsigned long n)
{
    if (!p) return malloc((u32)n);
    struct mhdr *h = mhdr_of(p, "realloc");
    if (!h) return 0;
    if (n <= h->cap) return p;
    void *q = malloc((u32)n);
    if (!q) return 0;
    memcpy(q, p, h->cap);
    free(p);
    return q;
}

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
/* Colours (DESIGN D14): `auto` means colour when fd is a terminal, TERM is set and not "dumb", and NO_COLOR is not set
 * to something (no-color.org) - one rule for every program */
int color_rule(int fd, const char *term, const char *no_color)
{
    return sys_isatty(fd) == 1 && term && *term && strcmp(term, "dumb") && !(no_color && *no_color);
}
int color_auto(int fd) { return color_rule(fd, getenv("TERM"), getenv("NO_COLOR")); }
int color_arg(const char *arg, int bare)
{
    const char *w;
    if (!strncmp(arg, "--colour", 8)) w = arg + 8;
    else if (!strncmp(arg, "--color", 7)) w = arg + 7;
    else return -1;
    if (!*w) return bare;
    if (*w++ != '=') return -1;
    if (!strcmp(w, "always") || !strcmp(w, "yes") || !strcmp(w, "force")) return 1;
    if (!strcmp(w, "never") || !strcmp(w, "no") || !strcmp(w, "none")) return 0;
    if (!strcmp(w, "auto") || !strcmp(w, "tty") || !strcmp(w, "if-tty")) return 2;
    return -2;
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
        if (n + 2 > *cap) { char *nb = realloc(*line, *cap * 2); if (!nb) break; *line = nb; *cap *= 2; }   /* out of memory: the line is cut here */
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

/* malloc that does not come back empty: out of memory ends the program with a message and status 1 */
char **_zargv;
void *xmalloc(u32 n)
{
    void *p = malloc(n);
    if (!p) { fprintf(2, "%s: out of memory\n", _zargv && _zargv[0] ? _zargv[0] : "libz"); _zexit(1); }
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

/* the error number of the last failed system call (or libz helper), and the reason to print for it */
int zerrno;
static const struct { unsigned char e; const char *text; } errtext[] = {
    { EPERM, "operation not permitted" }, { ENOENT, "no such file or directory" }, { ESRCH, "no such process" },
    { EINTR, "interrupted" }, { EIO, "input/output error" }, { ENXIO, "no such device" },
    { E2BIG, "argument list too long" }, { ENOEXEC, "not a Zenix program" }, { EBADF, "bad file descriptor" },
    { ECHILD, "no child process" }, { EAGAIN, "resource temporarily unavailable" }, { ENOMEM, "out of memory" },
    { EACCES, "permission denied" }, { EFAULT, "bad address" }, { EBUSY, "device or resource busy" },
    { EEXIST, "file exists" }, { EXDEV, "cross-device link" }, { ENODEV, "no such device" },
    { ENOTDIR, "not a directory" }, { EISDIR, "is a directory" }, { EINVAL, "invalid argument" },
    { ENFILE, "too many open files in the system" }, { EMFILE, "too many open files" }, { ENOTTY, "not a terminal" }, { ETXTBSY, "text file busy" },
    { EFBIG, "file too large" }, { ENOSPC, "no space left on device" }, { ESPIPE, "illegal seek" },
    { EROFS, "read-only file system" }, { EMLINK, "too many links" }, { EPIPE, "broken pipe" },
    { ERANGE, "result out of range" }, { ENAMETOOLONG, "file name too long" }, { ENOSYS, "not implemented" },
    { ENOTEMPTY, "directory not empty" }, { ELOOP, "too many levels of symbolic links" },
    { EOPNOTSUPP, "operation not supported" },
    /* the network's (DESIGN D19) */
    { ENOTSOCK, "not a socket" }, { EDESTADDRREQ, "destination address required" }, { EMSGSIZE, "message too long" },
    { EPROTOTYPE, "protocol wrong type for socket" }, { ENOPROTOOPT, "protocol not available" },
    { EPROTONOSUPPORT, "protocol not supported" }, { EAFNOSUPPORT, "address family not supported" },
    { EADDRINUSE, "address already in use" }, { EADDRNOTAVAIL, "cannot assign requested address" },
    { ENETDOWN, "network is down" }, { ENETUNREACH, "network is unreachable" },
    { ECONNABORTED, "connection aborted" }, { ECONNRESET, "connection reset" }, { ENOBUFS, "no buffer space available" },
    { EISCONN, "already connected" }, { ENOTCONN, "not connected" }, { ETIMEDOUT, "connection timed out" },
    { ECONNREFUSED, "connection refused" }, { EHOSTUNREACH, "no route to host" },
    { EALREADY, "operation already in progress" }, { EINPROGRESS, "operation in progress" },
};
/* strerror: the text of an error number (positive or negative), 0 if it has none */
const char *zstrerror(int e)
{
    if (e < 0) e = -e;
    for (unsigned i = 0; i < sizeof errtext / sizeof errtext[0]; i++)
        if (errtext[i].e == e) return errtext[i].text;
    return 0;
}
/* The text for zerrno, else dflt. EPERM (1) gives dflt too: a bare -1, which some calls still return for "failed"
 * without a reason, looks the same (docs/limits.md lists them). */
const char *zwhy(const char *dflt)
{
    const char *t = zerrno > 1 ? zstrerror(zerrno) : 0;
    return t ? t : dflt;
}

/* ---------------------------------------------------------------- directories and paths
 * The two helpers every directory walker uses, so that no walker keeps a name array or a path buffer on its 16 KB
 * stack (a recursive walk takes one level per directory, and there is no guard below the stack). */

/* "dir/name" in a new malloc'd string (no double slash). NULL when it would be longer than a system call takes
 * (zerrno 36, "path too long") or there is no memory (zerrno 12): the caller reports it, nothing is cut. */
char *path_join(const char *dir, const char *name)
{
    u32 dl = strlen(dir), nl = strlen(name);
    u32 slash = dl && dir[dl - 1] != '/';
    if (dl + slash + nl > ZPATH_MAX - 1) { zerrno = ENAMETOOLONG; return 0; }
    char *p = malloc(dl + slash + nl + 1);
    if (!p) { zerrno = ENOMEM; return 0; }
    memcpy(p, dir, dl);
    if (slash) p[dl] = '/';
    memcpy(p + dl + slash, name, nl + 1);
    return p;
}

/* The names in a directory, without "." and "..", in the order the filesystem gives them: *names is a malloc'd
 * array (as long as needed, NULL-terminated) of malloc'd strings, given back with dir_free. Returns the count, or
 * -1 when the directory cannot be read (zerrno from the system call) or memory runs out (zerrno 12); *names is
 * then NULL and nothing is left allocated. */
int dir_list(const char *path, char ***names)
{
    *names = 0;
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    char **v = 0; int n = 0, cap = 0; struct udirent d;
    while (sys_readdir(fd, &d) > 0) {
        if (!strcmp(d.name, ".") || !strcmp(d.name, "..")) continue;
        if (n + 1 >= cap) {
            int nc = cap ? cap * 2 : 32;
            char **nv = realloc(v, (u32)nc * sizeof *nv);
            if (!nv) goto oom;
            v = nv; cap = nc;
        }
        if (!(v[n] = strdup(d.name))) goto oom;
        n++;
    }
    sys_close(fd);
    if (!v && !(v = malloc(sizeof *v))) { zerrno = ENOMEM; return -1; }
    v[n] = 0;
    *names = v;
    return n;
oom:
    sys_close(fd);
    dir_free(v, n);
    zerrno = ENOMEM;
    return -1;
}
void dir_free(char **names, int n)
{
    if (!names) return;
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
}

/* A program by name, as the shell finds it: a name with a slash as it is, else the first PATH directory (default /bin)
 * that has a regular file of that name. 1 and the path in out, or 0 (not found, or longer than max). */
int path_find(const char *name, char *out, u32 max)
{
    struct ustat st;
    if (strchr(name, '/')) {
        if (strlen(name) >= max) return 0;
        strcpy(out, name);
        return sys_stat(out, &st) == 0 && (st.mode & S_IFMT) == S_IFREG;
    }
    const char *p = getenv("PATH");
    if (!p) p = "/bin";
    u32 nl = strlen(name);
    while (*p) {
        const char *e = p; while (*e && *e != ':') e++;
        u32 l = (u32)(e - p);
        if (l + nl + 2 <= max) {
            memcpy(out, p, l);
            if (!l) out[l++] = '.';
            if (out[l - 1] != '/') out[l++] = '/';
            memcpy(out + l, name, nl + 1);
            if (sys_stat(out, &st) == 0 && (st.mode & S_IFMT) == S_IFREG) return 1;
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

/* a basic regular expression (sed, expr :) in the extended syntax regex.c speaks; malloc'd (xmalloc) */
char *bre_to_ere(const char *s)
{
    char *out = xmalloc(strlen(s) * 2 + 2), *o = out; int in_br = 0;
    while (*s) {
        if (in_br) { *o++ = *s; if (*s == ']') in_br = 0; s++; continue; }
        if (*s == '[') { in_br = 1; *o++ = *s++; if (*s == '^') *o++ = *s++; if (*s == ']') *o++ = *s++; continue; }
        if (*s == '\\' && s[1]) {
            char c = s[1];
            if (c == '(' || c == ')' || c == '{' || c == '}' || c == '|' || c == '+' || c == '?') { *o++ = c; s += 2; continue; }
            *o++ = '\\'; *o++ = c; s += 2; continue;
        }
        if (*s == '(' || *s == ')' || *s == '{' || *s == '}' || *s == '|' || *s == '+' || *s == '?') { *o++ = '\\'; *o++ = *s++; continue; }
        *o++ = *s++;
    }
    *o = 0; return out;
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

/* ---------------------------------------------------------------- signals (DESIGN D16) */
/* The kernel sends a handler to one entry (Exec's model, docs/d16-events-and-signals.md section 5): d0 the signal, a0
 * the frame it wrote on the stack, a5 the program's data (D13). The entry hands both to _zsig_run on the stack (the C
 * rules), which calls the handler from this table and gives the frame back with SYS_sigreturn. Only pc-relative
 * branches here: nothing to relocate in a program's shared code. */
static sighandler_t _zsigtab[NSIG];
void _zsig_run(int sig, void *frame) __attribute__((used, noreturn));
void _zsig_run(int sig, void *frame)
{
    int e = zerrno;                         /* the interrupted code's errno stays */
    sighandler_t h = (u32)sig < NSIG ? _zsigtab[sig] : 0;
    if ((u32)h > 1)
        h(sig);
    zerrno = e;
    sys_sigreturn(frame);
    for (;;)
        ;
}
void _zsigentry(void);
__asm__(".section .text._zsigentry,\"ax\",@progbits\n"
        "    .globl _zsigentry\n"
        "_zsigentry:\n"
        "    move.l %a0,-(%sp)\n"
        "    move.l %d0,-(%sp)\n"
        "    bsr.l _zsig_run\n"
        "1:  bra.s 1b\n"
        "    .previous\n");

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    if (sig <= 0 || sig >= NSIG) { zerrno = EINVAL; return -1; }
    sighandler_t prev = _zsigtab[sig];
    struct usigaction u;
    if (act) {
        u.size = sizeof u; u.handler = (u32)act->sa_handler; u.mask = act->sa_mask; u.flags = (u32)act->sa_flags;
        u.entry = (u32)_zsigentry;
        if ((u32)act->sa_handler > 1)
            _zsigtab[sig] = act->sa_handler;    /* first: one may be sent at the end of this very call */
    }
    int r = sys_sigaction(sig, act ? &u : 0);
    if (r < 0) { _zsigtab[sig] = prev; return -1; }
    if (old) { old->sa_handler = r == 1 ? SIG_IGN : r == 2 ? prev : SIG_DFL; old->sa_mask = 0; old->sa_flags = 0; }
    return 0;
}
sighandler_t signal(int sig, sighandler_t h)
{
    struct sigaction a, o;
    a.sa_handler = h; a.sa_mask = 0; a.sa_flags = SA_RESTART;
    return sigaction(sig, &a, &o) < 0 ? SIG_ERR : o.sa_handler;
}
int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    u32 o;
    int r = set ? sys_sigprocmask(how, *set, &o) : sys_sigprocmask(SIG_BLOCK, 0, &o);
    if (r < 0) return -1;
    if (old) *old = o;
    return 0;
}
int kill(int pid, int sig) { return sys_killsig(pid, sig) < 0 ? -1 : 0; }
int raise(int sig) { return kill(sys_getpid(), sig); }
unsigned alarm(unsigned s) { int t = sys_alarm(s * 100); return t > 0 ? (unsigned)(t + 99) / 100 : 0; }
int pause(void) { sys_evwait(0, 0, -1); return -1; }
int setpgid(int pid, int pg) { return sys_setpgid(pid, pg) < 0 ? -1 : 0; }
int getpgid(int pid)
{
    struct uproc p;
    if (!pid) pid = sys_getpid();
    for (u32 i = 0; i < 64; i++)
        if (sys_procinfo(i, &p) == 0 && p.state && (int)p.pid == pid) return (int)p.pgid;
    zerrno = ESRCH;
    return -1;
}
int getpgrp(void) { return getpgid(0); }
int tcsetpgrp(int fd, int pg) { if (!sys_isatty(fd)) { zerrno = ENOTTY; return -1; } return sys_setfgpg(pg) < 0 ? -1 : 0; }
int poll(struct pollfd *f, u32 n, int ms)
{
    int r = sys_evwait(f, n, ms < 0 ? -1 : (ms + 9) / 10);
    return r < 0 ? -1 : r;
}
int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
    struct pollfd p[FD_SETSIZE];
    u32 n = 0;
    (void)ex;
    for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
        short ev = (short)((rd && FD_ISSET(fd, rd) ? POLLIN : 0) | (wr && FD_ISSET(fd, wr) ? POLLOUT : 0));
        if (ev) { p[n].fd = fd; p[n].events = ev; p[n].revents = 0; n++; }
    }
    int ms = tv ? (int)(tv->tv_sec * 1000 + tv->tv_usec / 1000) : -1;
    int r = poll(p, n, ms);
    if (r < 0) return -1;
    if (rd) FD_ZERO(rd);
    if (wr) FD_ZERO(wr);
    if (ex) FD_ZERO(ex);
    int cnt = 0;
    for (u32 i = 0; i < n; i++) {
        int hit = 0;
        if (rd && (p[i].revents & (POLLIN | POLLHUP | POLLERR))) { FD_SET(p[i].fd, rd); hit = 1; }
        if (wr && (p[i].revents & (POLLOUT | POLLERR))) { FD_SET(p[i].fd, wr); hit = 1; }
        if (p[i].revents & POLLNVAL) { zerrno = EBADF; return -1; }
        cnt += hit;
    }
    return cnt;
}
static const char *const signames[NSIG] = { 0, "HUP", "INT", "QUIT", "ILL", "TRAP", "ABRT", "BUS", "FPE", "KILL", "USR1",
    "SEGV", "USR2", "PIPE", "ALRM", "TERM", "STKFLT", "CHLD", "CONT", "STOP", "TSTP", "TTIN", "TTOU", "URG", "XCPU",
    "XFSZ", "VTALRM", "PROF", "WINCH", "IO", "PWR", "SYS" };
const char *zsig_name(int sig) { return sig > 0 && sig < NSIG ? signames[sig] : 0; }
int zsig_num(const char *s)
{
    if (isdigit((unsigned char)*s)) { char *e; long n = strtol(s, &e, 10); return *e || n < 0 || n >= NSIG ? -1 : (int)n; }
    if (!strncmp(s, "SIG", 3)) s += 3;
    for (int i = 1; i < NSIG; i++) if (!strcmp(s, signames[i])) return i;
    return -1;
}

/* ---------------------------------------------------------------- BSD sockets (DESIGN D19 4.4, user/socket.h)
 * Each call fills a struct usockreq and makes one SYS_sock; the kernel checks every pointer and waits (D19 4.3). */
#include "socket.h"
static int sk(u32 op, int fd, struct usockreq *r)
{
    int v = sys_sock(op | (u32)sizeof *r << 16, fd, r);
    return v < 0 ? -1 : v;
}
#define SKREQ(r) struct usockreq r; memset(&r, 0, sizeof r)
int socket(int domain, int type, int protocol)
{
    SKREQ(r);
    if ((u32)domain > 0xFFFF || (u32)protocol > 0xFFFF) { zerrno = EINVAL; return -1; }
    r.level = (unsigned short)domain;
    r.flags = (u32)type;
    r.option = (unsigned short)protocol;
    return sk(SOCK_SOCKET, 0, &r);
}
static int sk_addr(u32 op, int fd, const struct sockaddr *a, socklen_t *len)
{
    SKREQ(r);
    r.addr = (u32)a;
    r.alen = (u32)len;
    return sk(op, fd, &r);
}
int bind(int fd, const struct sockaddr *a, socklen_t len) { return sk_addr(SOCK_BIND, fd, a, &len); }
int connect(int fd, const struct sockaddr *a, socklen_t len) { return sk_addr(SOCK_CONNECT, fd, a, &len); }
int getsockname(int fd, struct sockaddr *a, socklen_t *len) { return sk_addr(SOCK_GETSOCKNAME, fd, a, len); }
int getpeername(int fd, struct sockaddr *a, socklen_t *len) { return sk_addr(SOCK_GETPEERNAME, fd, a, len); }
int listen(int fd, int backlog)
{
    SKREQ(r);
    r.len = backlog < 0 ? 0 : (u32)backlog;
    return sk(SOCK_LISTEN, fd, &r);
}
int accept4(int fd, struct sockaddr *a, socklen_t *len, int flags)
{
    SKREQ(r);
    r.addr = (u32)a;
    r.alen = a ? (u32)len : 0;
    r.flags = (u32)flags;
    return sk(SOCK_ACCEPT, fd, &r);
}
int accept(int fd, struct sockaddr *a, socklen_t *len) { return accept4(fd, a, len, 0); }
int sendto(int fd, const void *buf, u32 n, int flags, const struct sockaddr *a, socklen_t len)
{
    SKREQ(r);
    r.buf = (u32)buf;
    r.len = n;
    r.flags = (u32)flags;
    if (a) { r.addr = (u32)a; r.alen = (u32)&len; }
    return sk(SOCK_SENDTO, fd, &r);
}
int send(int fd, const void *buf, u32 n, int flags) { return sendto(fd, buf, n, flags, 0, 0); }
int recvfrom(int fd, void *buf, u32 n, int flags, struct sockaddr *a, socklen_t *len)
{
    SKREQ(r);
    r.buf = (u32)buf;
    r.len = n;
    r.flags = (u32)flags;
    if (a) { r.addr = (u32)a; r.alen = (u32)len; }
    return sk(SOCK_RECVFROM, fd, &r);
}
int recv(int fd, void *buf, u32 n, int flags) { return recvfrom(fd, buf, n, flags, 0, 0); }
int shutdown(int fd, int how)
{
    SKREQ(r);
    r.flags = (u32)how;
    return sk(SOCK_SHUTDOWN, fd, &r);
}
static int sk_opt(u32 op, int fd, int level, int opt, const void *val, socklen_t *len)
{
    SKREQ(r);
    if ((u32)level > 0xFFFF || (u32)opt > 0xFFFF) { zerrno = ENOPROTOOPT; return -1; }
    r.level = (unsigned short)level;
    r.option = (unsigned short)opt;
    r.val = (u32)val;
    r.vlen = (u32)len;
    return sk(op, fd, &r);
}
int getsockopt(int fd, int level, int opt, void *val, socklen_t *len) { return sk_opt(SOCK_GETSOCKOPT, fd, level, opt, val, len); }
int setsockopt(int fd, int level, int opt, const void *val, socklen_t len) { return sk_opt(SOCK_SETSOCKOPT, fd, level, opt, val, &len); }
static int sk_ioctl(int fd, u32 req, void *p)
{
    SKREQ(r);
    r.option = (unsigned short)req;
    r.val = (u32)p;
    return sk(SOCK_IOCTL, fd, &r);
}
int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *p = va_arg(ap, void *);
    va_end(ap);
    if (req != FIONREAD && req != FIONBIO) { zerrno = ENOTTY; return -1; }
    if (!p) { zerrno = EFAULT; return -1; }
    return sk_ioctl(fd, (u32)req, p);
}
int fcntl(int fd, int cmd, ...)
{
    int arg = 0;
    if (cmd == F_SETFL || cmd == F_SETFD) { va_list ap; va_start(ap, cmd); arg = va_arg(ap, int); va_end(ap); }
    if (cmd == F_GETFD || cmd == F_SETFD) { struct ustat st; return sys_fstat(fd, &st) < 0 ? -1 : 0; }
    if (cmd != F_GETFL && cmd != F_SETFL) { zerrno = EINVAL; return -1; }
    int on = (arg & O_NONBLOCK) != 0;
    int v = sk_ioctl(fd, FIONBIO, cmd == F_GETFL ? 0 : &on);    /* FIONBIO with no value asks (netproto.h) */
    if (v >= 0) return cmd == F_GETFL ? O_RDWR | (v ? O_NONBLOCK : 0) : 0;
    if (zerrno != ENOTSOCK) return -1;
    if (cmd == F_GETFL) return O_RDWR;                          /* other files: no flag to report yet */
    if (on) { zerrno = EINVAL; return -1; }                     /* O_NONBLOCK on pipes and ttys: not yet (D19 4.1) */
    return 0;
}

/* addresses as text */
int inet_pton(int af, const char *s, void *dst)
{
    if (af != AF_INET) { zerrno = EAFNOSUPPORT; return -1; }
    unsigned char a[4];
    for (int i = 0; i < 4; i++) {
        u32 v = 0, nd = 0;
        while (*s >= '0' && *s <= '9') {
            if (nd && !v) return 0;                     /* no leading zeros, as glibc */
            v = v * 10 + (u32)(*s++ - '0');
            if (++nd > 3 || v > 255) return 0;
        }
        if (!nd || *s != (i < 3 ? '.' : 0)) return 0;
        a[i] = (unsigned char)v;
        s++;
    }
    memcpy(dst, a, 4);
    return 1;
}
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    const unsigned char *a = src;
    char t[16];
    if (af != AF_INET) { zerrno = EAFNOSUPPORT; return 0; }
    int n = snprintf(t, sizeof t, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    if ((u32)n >= size) { zerrno = 28; return 0; }      /* ENOSPC */
    memcpy(dst, t, (u32)n + 1);
    return dst;
}
int inet_aton(const char *s, struct in_addr *out)
{
    u32 part[4], n = 0;
    for (;;) {
        if (!isdigit((unsigned char)*s)) return 0;
        char *e;
        unsigned long v = strtoul(s, &e, 0);
        if (e == s || n == 4) return 0;
        part[n++] = (u32)v;
        s = e;
        if (*s == '.') { s++; continue; }
        if (*s && !isspace((unsigned char)*s)) return 0;
        break;
    }
    u32 v = part[n - 1], max = n == 1 ? 0xFFFFFFFFu : n == 2 ? 0xFFFFFFu : n == 3 ? 0xFFFFu : 0xFFu;
    if (v > max) return 0;
    for (u32 i = 0; i + 1 < n; i++) {
        if (part[i] > 255) return 0;
        v |= part[i] << (24 - 8 * i);
    }
    if (out) out->s_addr = htonl(v);
    return 1;
}
in_addr_t inet_addr(const char *s)
{
    struct in_addr a;
    return inet_aton(s, &a) ? a.s_addr : INADDR_NONE;
}
char *inet_ntoa(struct in_addr a)
{
    static char b[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, b, sizeof b);
    return b;
}

/* names (DESIGN D19 4.5) */
static const struct { const char *name; unsigned short port; } services[] = {
    { "echo", 7 }, { "ftp", 21 }, { "ssh", 22 }, { "telnet", 23 }, { "smtp", 25 }, { "domain", 53 }, { "http", 80 },
    { "pop3", 110 }, { "nntp", 119 }, { "ntp", 123 }, { "imap", 143 }, { "https", 443 },
};
/* a name looked up by netd: up to 4 addresses into a[], their count, or an EAI_* error */
static int resolve(const char *name, u32 *a)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return zerrno == ENETDOWN ? EAI_FAIL : EAI_SYSTEM;
    SKREQ(r);
    r.buf = (u32)name;
    r.len = strlen(name);
    int n = sk(SOCK_RESOLVE, s, &r);
    if (n >= 0) n = recv(s, a, 16, 0);
    int e = zerrno;
    sys_close(s);
    if (n > 0) return n / 4;
    zerrno = e;
    if (n == 0 || e == ENOENT) return EAI_NONAME;
    if (e == ETIMEDOUT || e == EAGAIN) return EAI_AGAIN;
    if (e == ENOMEM || e == ENOBUFS) return EAI_MEMORY;
    return e == EINTR ? EAI_SYSTEM : EAI_FAIL;
}
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
    int flags = hints ? hints->ai_flags : 0, fam = hints ? hints->ai_family : AF_UNSPEC;
    int type = hints ? hints->ai_socktype : 0, proto = hints ? hints->ai_protocol : 0;
    u32 addr[4];
    int na = 1;
    *res = 0;
    if (flags & ~(AI_PASSIVE | AI_CANONNAME | AI_NUMERICHOST | AI_NUMERICSERV)) return EAI_BADFLAGS;
    if (fam != AF_UNSPEC && fam != AF_INET) return EAI_FAMILY;
    if (type && type != SOCK_STREAM && type != SOCK_DGRAM) return EAI_SOCKTYPE;
    if (!node && !service) return EAI_NONAME;
    u32 port = 0;
    if (service && *service) {
        char *e;
        port = strtoul(service, &e, 10);
        if (*e || !isdigit((unsigned char)*service)) {
            if (flags & AI_NUMERICSERV) return EAI_NONAME;
            u32 i = 0;
            while (i < sizeof services / sizeof services[0] && strcmp(services[i].name, service)) i++;
            if (i == sizeof services / sizeof services[0]) return EAI_SERVICE;
            port = services[i].port;
        } else if (port > 65535) return EAI_SERVICE;
    }
    struct in_addr in;
    if (!node) addr[0] = htonl(flags & AI_PASSIVE ? INADDR_ANY : INADDR_LOOPBACK);
    else if (inet_aton(node, &in)) addr[0] = in.s_addr;
    else if (flags & AI_NUMERICHOST) return EAI_NONAME;
    else if ((na = resolve(node, addr)) < 0) return na;
    struct addrinfo **tail = res;
    for (int i = 0; i < na; i++)
        for (int t = SOCK_STREAM; t <= SOCK_DGRAM; t++) {
            if (type && type != t) continue;
            struct addrinfo *ai = malloc(sizeof *ai + sizeof(struct sockaddr_in));
            if (!ai) { freeaddrinfo(*res); *res = 0; return EAI_MEMORY; }
            struct sockaddr_in *sin = (struct sockaddr_in *)(ai + 1);
            memset(ai, 0, sizeof *ai + sizeof *sin);
            sin->sin_family = AF_INET;
            sin->sin_port = htons((unsigned short)port);
            sin->sin_addr.s_addr = addr[i];
            ai->ai_family = AF_INET;
            ai->ai_socktype = t;
            ai->ai_protocol = proto ? proto : t == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP;
            ai->ai_addrlen = sizeof *sin;
            ai->ai_addr = (struct sockaddr *)sin;
            if ((flags & AI_CANONNAME) && !*res && node && !(ai->ai_canonname = strdup(node))) {
                free(ai);
                return EAI_MEMORY;
            }
            *tail = ai;
            tail = &ai->ai_next;
        }
    return 0;
}
void freeaddrinfo(struct addrinfo *ai)
{
    while (ai) {
        struct addrinfo *n = ai->ai_next;
        free(ai->ai_canonname);
        free(ai);
        ai = n;
    }
}
const char *gai_strerror(int e)
{
    switch (e) {
    case 0: return "success";
    case EAI_BADFLAGS: return "bad value for ai_flags";
    case EAI_NONAME: return "name or service not known";
    case EAI_AGAIN: return "temporary failure in name resolution";
    case EAI_FAIL: return "non-recoverable failure in name resolution";
    case EAI_FAMILY: return "address family not supported";
    case EAI_SOCKTYPE: return "socket type not supported";
    case EAI_SERVICE: return "service not supported for this socket type";
    case EAI_MEMORY: return "out of memory";
    case EAI_SYSTEM: return zwhy("system error");
    }
    return "unknown error";
}
