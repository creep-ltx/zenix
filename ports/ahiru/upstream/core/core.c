/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* core - see core.h. No Amiga headers here, on purpose. */
#include <string.h>
#include <stdarg.h>
#include "core.h"

/* ---- fmt: sprintf without the C library's stdio ----------------
 * libnix sets up stdio for any program that links sprintf, and that
 * setup returns 20 before main when there is no console (run through
 * wasabi, measured 4.10.26). This covers what UP-File uses: flags
 * '-' and '0', width, precision, 'l', and d i u x X c s %. */
static char *putnum(char *d, unsigned long v, int neg, int base, int upper,
                    int width, int left, int zero)
{
    char tmp[16];
    int n = 0, len, pad;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[n++] = dig[v % base];
        v /= base;
    } while (v);
    len = n + neg;
    pad = width > len ? width - len : 0;
    if (!left && !zero) while (pad-- > 0) *d++ = ' ';
    if (neg) *d++ = '-';
    if (!left && zero) while (pad-- > 0) *d++ = '0';
    while (n) *d++ = tmp[--n];
    if (left) while (pad-- > 0) *d++ = ' ';
    return d;
}

int fmt(char *dst, const char *f, ...)
{
    va_list ap;
    char *d = dst;
    va_start(ap, f);
    while (*f) {
        int left = 0, zero = 0, width = 0, prec = -1, lng = 0, c;
        if (*f != '%') {
            *d++ = *f++;
            continue;
        }
        f++;
        for (;; f++) {
            if (*f == '-') left = 1;
            else if (*f == '0') zero = 1;
            else break;
        }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            prec = 0;
            f++;
            while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        while (*f == 'l') {
            lng = 1;
            f++;
        }
        c = *f ? *f++ : 0;
        switch (c) {
        case 'd': case 'i': {
            long v = lng ? va_arg(ap, long) : (long)va_arg(ap, int);
            d = putnum(d, v < 0 ? (unsigned long)(-v) : (unsigned long)v, v < 0, 10, 0,
                       width, left, zero);
            break;
        }
        case 'u': case 'x': case 'X': {
            unsigned long v = lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            d = putnum(d, v, 0, c == 'u' ? 10 : 16, c == 'X', width, left, zero);
            break;
        }
        case 'c': {
            int pad = width > 1 ? width - 1 : 0;
            if (!left) while (pad-- > 0) *d++ = ' ';
            *d++ = (char)va_arg(ap, int);
            if (left) while (pad-- > 0) *d++ = ' ';
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            int n = 0, pad;
            if (!s) s = "(null)";
            while (s[n] && (prec < 0 || n < prec)) n++;
            pad = width > n ? width - n : 0;
            if (!left) while (pad-- > 0) *d++ = ' ';
            memcpy(d, s, n);
            d += n;
            if (left) while (pad-- > 0) *d++ = ' ';
            break;
        }
        case '%':
            *d++ = '%';
            break;
        case 0:
            f--;
            break;
        default:
            *d++ = (char)c;
        }
    }
    *d = 0;
    va_end(ap);
    return (int)(d - dst);
}

static int lower(int c)
{
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 192 && c <= 222 && c != 215) return c + 32;   /* Latin-1 */
    return c;
}

static int isdig(int c) { return c >= '0' && c <= '9'; }

/* Shell sort of an index by name, no case (Knuth gaps: folders come
 * near-sorted); nm gives entry i's name */
void name_sort(int *ix, int n, const char *(*nm)(void *ctx, int i), void *ctx)
{
    int gap = 1, i, j, t;
    while (gap < n / 3) gap = gap * 3 + 1;
    for (; gap > 0; gap /= 3)
        for (i = gap; i < n; i++) {
            t = ix[i];
            for (j = i; j >= gap && nocase(nm(ctx, ix[j - gap]), nm(ctx, t)) > 0; j -= gap) ix[j] = ix[j - gap];
            ix[j] = t;
        }
}

/* in an index sorted by name_sort: the entry called name, -1 none */
int name_find(const int *ix, int n, const char *(*nm)(void *ctx, int i), void *ctx, const char *name)
{
    int lo = 0, hi = n - 1, mid, c;
    while (lo <= hi) {
        mid = (lo + hi) / 2;
        c = nocase(nm(ctx, ix[mid]), name);
        if (c == 0) return ix[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

int nocase(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    int ca, cb;
    for (;;) {
        ca = lower(*x++);
        cb = lower(*y++);
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
}

int natcmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    int ca, cb;
    for (;;) {
        ca = *x;
        cb = *y;
        if (isdig(ca) && isdig(cb)) {
            const unsigned char *sx, *sy;
            int lx, ly, k;
            /* leading zeros do not make a number bigger */
            while (*x == '0' && isdig(x[1])) x++;
            while (*y == '0' && isdig(y[1])) y++;
            sx = x;
            sy = y;
            while (isdig(*x)) x++;
            while (isdig(*y)) y++;
            lx = (int)(x - sx);
            ly = (int)(y - sy);
            if (lx != ly) return lx - ly;
            for (k = 0; k < lx; k++) {
                int d = (int)sx[k] - (int)sy[k];
                if (d) return d;
            }
            continue;
        }
        if (ca == 0 || cb == 0) return ca - cb;
        ca = lower(ca);
        cb = lower(cb);
        if (ca != cb) return ca - cb;
        x++;
        y++;
    }
}

int nchas(const char *hay, const char *needle)
{
    int nl = (int)strlen(needle), hl = (int)strlen(hay), i, j;
    if (nl == 0) return 1;
    for (i = 0; i + nl <= hl; i++) {
        for (j = 0; j < nl; j++)
            if (lower((unsigned char)hay[i + j]) !=
                lower((unsigned char)needle[j])) break;
        if (j == nl) return 1;
    }
    return 0;
}

const char *extof(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL || dot == name) return name + strlen(name);
    return dot + 1;
}

static int inlist(const char *list, const char *ext)
{
    /* list is "|a|b|c|" */
    char key[16];
    int l = (int)strlen(ext), i;
    if (l == 0 || l > 12) return 0;
    key[0] = '|';
    for (i = 0; i < l; i++) key[i + 1] = (char)lower((unsigned char)ext[i]);
    key[l + 1] = '|';
    key[l + 2] = 0;
    return strstr(list, key) != NULL;
}

int kindof(const char *name)
{
    const char *e;
    if (lower((unsigned char)name[0]) == 'm' && lower((unsigned char)name[1]) == 'o' &&
        lower((unsigned char)name[2]) == 'd' && name[3] == '.')
        return K_AUD;                   /* Amiga "mod.name" */
    e = extof(name);
    if (*e == 0) return K_PLAIN;
    if (inlist("|info|", e)) return K_INFO;
    if (inlist("|lha|lzh|lzx|zip|dms|adf|hdf|iso|gz|tgz|tar|7z|rar|xz|bz2|lz|z|", e))
        return K_ARCH;
    if (inlist("|iff|ilbm|lbm|pic|gif|jpg|jpeg|png|bmp|pcx|tga|ham|brush|br|anim|anm|ico|webp|", e))
        return K_IMG;
    if (inlist("|mod|med|mmd0|mmd1|mmd2|xm|s3m|it|8svx|svx|wav|mp3|aiff|aif|sid|ahx|thx|ogg|flac|bp|tfmx|okt|sng|", e))
        return K_AUD;
    if (inlist("|e|c|h|s|asm|i|py|rexx|rx|cpp|hpp|bas|sh|m|amos|", e))
        return K_SRC;
    if (inlist("|txt|doc|readme|guide|md|me|nfo|diz|ans|asc|1st|htm|html|", e))
        return K_DOC;
    return K_PLAIN;
}

void fmtsize(char *dst, unsigned long n)
{
    static const char suf[] = "KMG";
    unsigned long unit = 1024, whole, tenth;
    int s = 0;
    if (n < 1000) {
        fmt(dst, "%lu", n);
        return;
    }
    while (s < 2 && n >= unit * 1000UL) {
        unit *= 1024;
        s++;
    }
    whole = n / unit;
    if (whole < 10) {
        tenth = (n % unit) / (unit / 10);
        if (tenth > 9) tenth = 9;
        fmt(dst, "%lu.%lu%c", whole, tenth, suf[s]);
    } else {
        fmt(dst, "%lu%c", whole, suf[s]);
    }
}

unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | p[3];
}
unsigned int be16(const unsigned char *p) { return (p[0] << 8) | p[1]; }
unsigned long le32(const unsigned char *p)
{
    return ((unsigned long)p[3] << 24) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[1] << 8) | p[0];
}
unsigned int le16(const unsigned char *p) { return (p[1] << 8) | p[0]; }

static int ismodmagic(const unsigned char *m)
{
    static const char *const tags[] = {
        "M.K.", "M!K!", "FLT4", "FLT8", "4CHN", "6CHN", "8CHN", "CD81",
        "OKTA", NULL };
    int i;
    for (i = 0; tags[i]; i++)
        if (memcmp(m, tags[i], 4) == 0) return 1;
    return 0;
}

/* UTF-8 text: only good sequences, at least one beyond ASCII, no
 * control bytes but tab, line ends, form feed and ESC (a sequence cut at
 * the end of what was read is fine). Its 0x80-0x9F bytes would count as
 * controls below, and a 0x9B in one (as in "s acute") as the Amiga's CSI. */
static int utf8text(const unsigned char *buf, long m, int *esc)
{
    long i = 0;
    int high = 0, k, j;
    while (i < m) {
        int c = buf[i];
        if (c < 0x80) {
            if (c < 32 && c != 9 && c != 10 && c != 12 && c != 13 && c != 27) return 0;
            if (c == 27) *esc = 1;
            i++;
            continue;
        }
        if (c < 0xC2 || c > 0xF4) return 0;
        k = c < 0xE0 ? 1 : c < 0xF0 ? 2 : 3;
        for (j = 1; j <= k; j++) {
            if (i + j >= m) return high || j > 1;
            if ((buf[i + j] & 0xC0) != 0x80) return 0;
        }
        high = 1;
        i += k + 1;
    }
    return high;
}

int sniffmem(const unsigned char *buf, long n)
{
    long i, m, pr = 0;
    int esc = 0;
    if (n < 1) return TY_EMPTY;
    if (n >= 4) {
        if (buf[0] == 0 && buf[1] == 0 && buf[2] == 3 && buf[3] == 0xF3)
            return TY_EXEC;
        if (buf[0] == 'P' && buf[1] == 'K' && buf[2] == 3 && buf[3] == 4)
            return TY_ZIP;
        if (buf[0] == 'L' && buf[1] == 'Z' && buf[2] == 'X') return TY_LZX;
        if (memcmp(buf, "DMS!", 4) == 0) return TY_DMS;
    }
    if (n >= 12 && memcmp(buf, "FORM", 4) == 0) return TY_IFF;
    if (n >= 7 && buf[2] == '-' && buf[3] == 'l' && buf[6] == '-')
        return TY_LHA;
    if (n >= 1084 && ismodmagic(buf + 1080)) return TY_MOD;
    m = n > 1024 ? 1024 : n;
    if (utf8text(buf, m, &esc)) return esc ? TY_ANSI : TY_TEXT;
    esc = 0;
    for (i = 0; i < m; i++) {
        int c = buf[i];
        if (c == 0) return TY_OTHER;
        if ((c >= 32 && c <= 126) || c == 9 || c == 10 || c == 13 ||
            c == 12 || c >= 160) pr++;
        else if (c == 27 || c == 155) {
            pr++;
            esc = 1;
        }
    }
    if (pr * 100 >= m * 95) return esc ? TY_ANSI : TY_TEXT;
    return TY_OTHER;
}

/* ---- ANSI ------------------------------------------------------ */

/* parse one escape sequence starting at buf[i] (just past ESC[ or
 * CSI). Returns the index after it; applies SGR to *attr and cursor
 * forward/back to *col. */
static long ansi_seq(const unsigned char *buf, long len, long i,
                     int *attr, int *col)
{
    int p[8], np = 0, k, fin = 0, a = *attr;
    p[0] = 0;
    while (i < len) {
        int c = buf[i++];
        if (c >= '0' && c <= '9') {
            if (p[np] < 10000) p[np] = p[np] * 10 + (c - '0');
        } else if (c == ';') {
            if (np < 7) p[++np] = 0;
        } else if (c >= 0x40 && c <= 0x7E) {
            fin = c;
            break;
        }
        /* '?', ' ' and other intermediates are skipped */
    }
    np++;
    switch (fin) {
    case 'm':
        for (k = 0; k < np; k++) {
            int v = p[k];
            if (v == 0) a = ANSI_DEFATTR;
            else if (v == 1) a |= 64;
            else if (v == 22) a &= ~64;
            else if (v >= 30 && v <= 37) a = (a & ~7) | (v - 30);
            else if (v == 39) a = (a & ~7) | 7;
            else if (v >= 40 && v <= 47) a = (a & ~56) | ((v - 40) << 3);
            else if (v == 49) a &= ~56;
        }
        break;
    case 'C':
        *col += p[0] < 1 ? 1 : p[0];
        break;
    case 'D':
        *col -= p[0] < 1 ? 1 : p[0];
        if (*col < 0) *col = 0;
        break;
    default:
        break;              /* positioning and erasing are ignored */
    }
    *attr = a;
    return i;
}

int ansi_render(const unsigned char *buf, long len, long off, int attr,
                unsigned char *cells, unsigned char *attrs,
                int w, int rows, int artw)
{
    int row = 0, col = 0, filled = 0;
    long i = off;
    unsigned char *rc = cells, *ra = attrs;
    int k, total = w * rows;
    for (k = 0; k < total; k++) {
        cells[k] = ' ';
        attrs[k] = ANSI_DEFATTR;
    }
    while (i < len && row < rows) {
        int c = buf[i++];
        if (c == 27) {
            if (i < len && buf[i] == '[') i = ansi_seq(buf, len, i + 1, &attr, &col);
        } else if (c == 155) {
            i = ansi_seq(buf, len, i, &attr, &col);
        } else if (c == 10) {
            row++;
            col = 0;
            filled = row;
            rc += w;
            ra += w;
        } else if (c == 13) {
            col = 0;
        } else if (c == 9) {
            col = (col | 7) + 1;
        } else if (c == 26) {
            break;          /* SAUCE: the art ends here */
        } else if (c >= 32) {
            if (col >= artw) {
                row++;
                col = 0;
                rc += w;
                ra += w;
                if (row >= rows) break;
            }
            if (col < w) {
                rc[col] = (unsigned char)c;
                ra[col] = (unsigned char)attr;
            }
            col++;
            filled = row + 1;
        }
    }
    return filled > rows ? rows : filled;
}

long ansi_index(const unsigned char *buf, long len, int artw,
                void (*add)(void *ctx, long off, int attr), void *ctx)
{
    long i = 0, n = 1;
    int attr = ANSI_DEFATTR, col = 0;
    add(ctx, 0, attr);
    while (i < len) {
        int c = buf[i++];
        if (c == 27) {
            if (i < len && buf[i] == '[') i = ansi_seq(buf, len, i + 1, &attr, &col);
        } else if (c == 155) {
            i = ansi_seq(buf, len, i, &attr, &col);
        } else if (c == 10) {
            col = 0;
            if (i < len) {
                add(ctx, i, attr);
                n++;
            }
        } else if (c == 13) {
            col = 0;
        } else if (c == 9) {
            col = (col | 7) + 1;
        } else if (c == 26) {
            break;
        } else if (c >= 32) {
            if (col >= artw) {
                col = 0;
                add(ctx, i - 1, attr);
                n++;
            }
            col++;
        }
    }
    return n;
}

/* ---- text and hex ------------------------------------------------ */

long text_row(const unsigned char *buf, long len, long off,
              unsigned char *cells, int w, int wrap)
{
    int col = 0, k;
    long i = off;
    while (i < len) {
        int c = buf[i];
        if (c == 10) {
            i++;
            break;
        }
        if (wrap && col >= w) break;
        i++;
        if (c == 9) {
            do {
                if (col < w) cells[col] = ' ';
                col++;
            } while (col & 7);
        } else if (c == 13) {
            /* nothing */
        } else {
            if (col < w)
                cells[col] = (unsigned char)((c >= 32 && c <= 126) || c >= 160 ? c : '.');
            col++;
        }
    }
    for (k = col; k < w; k++) cells[k] = ' ';
    return i;
}

int hex_bpr(int w)
{
    int b = (w - 10) / 4;    /* 8 offset digits and a space, 3+1 a byte */
    if (b >= 16) return 16;
    if (b >= 8) return 8;
    return 4;
}

void hex_row(const unsigned char *buf, long len, long off, int bpr,
             unsigned char *cells, int w)
{
    static const char hx[] = "0123456789ABCDEF";
    unsigned char line[160];
    int p = 0, k;
    /* eight digits: files of any size stream now, and six wrapped back
     * to 000000 at 16MB (the E CFile's b52 fix) */
    for (k = 7; k >= 0; k--) line[p + k] = hx[(off >> (4 * (7 - k))) & 15];
    p = 8;
    line[p++] = ' ';
    for (k = 0; k < bpr; k++) {
        if (off + k < len) {
            int c = buf[off + k];
            line[p++] = hx[c >> 4];
            line[p++] = hx[c & 15];
        } else {
            line[p++] = ' ';
            line[p++] = ' ';
        }
        line[p++] = ' ';
    }
    line[p++] = ' ';
    for (k = 0; k < bpr; k++) {
        if (off + k < len) {
            int c = buf[off + k];
            line[p++] = (unsigned char)((c >= 32 && c <= 126) || c >= 160 ? c : '.');
        } else
            line[p++] = ' ';
    }
    for (k = 0; k < w; k++) cells[k] = k < p ? line[k] : ' ';
}

/* ---- LhA ----------------------------------------------------------- */

/* name := dir + "/" + name, when there is a dir and it fits */
static void joindir(char *name, int namemax, char *dir)
{
    int dl = (int)strlen(dir), nl = (int)strlen(name);
    if (dl == 0) return;
    if (dir[dl - 1] != '/') {
        dir[dl++] = '/';
        dir[dl] = 0;
    }
    if (dl + nl < namemax) {
        memmove(name + dl, name, nl + 1);
        memcpy(name, dir, dl);
    }
}

int lha_header(const unsigned char *h, long n, char *name, int namemax,
               unsigned long *packed, unsigned long *orig,
               unsigned long *next)
{
    int level, nl, k;
    char dir[128];
    dir[0] = 0;
    name[0] = 0;
    if (n < 22 || h[0] == 0) return 0;
    if (h[2] != '-' || h[6] != '-') return 0;
    level = h[20];
    *packed = le32(h + 7);
    *orig = le32(h + 11);
    if (level < 2) {
        int hsize = h[0];
        nl = h[21];
        if (22 + nl > n) return 0;
        if (nl > namemax - 1) nl = namemax - 1;
        for (k = 0; k < nl; k++) {
            int c = h[22 + k];
            name[k] = (char)(c == '\\' || c == 0xFF ? '/' : c);
        }
        name[nl] = 0;
        if (level == 1 && hsize + 2 <= n) {
            /* level 1 keeps directories in extended headers after
             * the base header; their sizes count in `packed` */
            unsigned int es = le16(h + hsize);
            long o = hsize + 2;
            while (es >= 3 && o + (long)es <= n) {
                if (h[o] == 2) {
                    int dl = (int)es - 3;
                    if (dl > (int)sizeof(dir) - 2) dl = (int)sizeof(dir) - 2;
                    for (k = 0; k < dl; k++) {
                        int c = h[o + 1 + k];
                        dir[k] = (char)(c == 0xFF || c == '\\' ? '/' : c);
                    }
                    dir[dl] = 0;
                }
                {
                    unsigned int nx = le16(h + o + es - 2);
                    o += es;
                    es = nx;
                }
            }
            joindir(name, namemax, dir);
        }
        *next = (unsigned long)hsize + 2 + *packed;
        return 1;
    }
    if (level == 2) {
        unsigned long hsize = le16(h);
        long o = 26;
        unsigned int es = n >= 26 ? le16(h + 24) : 0;
        /* each extended header is [type][data][size of the next] */
        while (es >= 3 && o + (long)es <= n) {
            int type = h[o];
            int dl = (int)es - 3;
            if (type == 1 || type == 2) {
                char *dst = type == 1 ? name : dir;
                int cap = type == 1 ? namemax - 1 : (int)sizeof(dir) - 2;   /* room for joindir's '/' */
                if (dl > cap) dl = cap;
                for (k = 0; k < dl; k++) {
                    int c = h[o + 1 + k];
                    dst[k] = (char)(c == 0xFF || c == '\\' ? '/' : c);
                }
                dst[dl] = 0;
            }
            {
                unsigned int nx = le16(h + o + es - 2);
                o += es;
                es = nx;
            }
        }
        joindir(name, namemax, dir);
        *next = hsize + *packed;
        return 1;
    }
    return 0;
}
