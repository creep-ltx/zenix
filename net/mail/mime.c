/* SPDX-License-Identifier: GPL-2.0-only */
/* mail's message reader (DESIGN D59 section 5): headers (RFC 5322, unfolded; RFC 2047 words; RFC 2231 file names),
 * MIME (RFC 2045-2046: multipart, alternative, message/rfc822, quoted-printable, base64), charsets (UTF-8, ISO-8859-1,
 * ISO-8859-15, Windows-1252; another one: UTF-8 where valid, else ISO-8859-1), HTML made text, dates. Written from
 * the RFCs. Everything here reads untrusted bytes: every index is checked against the length given, the depth is
 * bounded, and what comes out is clean UTF-8 (no C0 but the newline, no DEL, no C1, no bidirectional overrides). */
#include "mail.h"

typedef unsigned char u8;

/* ---- the growing string ---- */
void sb_init(struct sb *b, u32 max) { memset(b, 0, sizeof *b); b->max = max; }

void sb_put(struct sb *b, const char *s, u32 n)
{
    if (b->nomem) return;
    if (b->n + n > b->max) {
        b->full = 1;
        n = b->max > b->n ? b->max - b->n : 0;
        while (n && ((unsigned char)s[n] & 0xC0) == 0x80) n--;     /* whole characters only */
    }
    if (b->n + n + 1 > b->cap) {
        u32 c = b->cap ? b->cap : 256;
        while (c < b->n + n + 1) c *= 2;
        char *p = realloc(b->p, c);
        if (!p) {
            b->nomem = 1;
            return;
        }
        b->p = p;
        b->cap = c;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
void sb_puts(struct sb *b, const char *s) { sb_put(b, s, (u32)strlen(s)); }
void sb_putc(struct sb *b, int c) { char ch = (char)c; sb_put(b, &ch, 1); }
void sb_printf(struct sb *b, const char *fmt, ...)
{
    char t[300];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    if (n > 0) sb_put(b, t, n < (int)sizeof t ? (u32)n : sizeof t - 1);
}
void sb_u8(struct sb *b, u32 u)
{
    char t[4];
    if (u < 0x80) { t[0] = (char)u; sb_put(b, t, 1); }
    else if (u < 0x800) { t[0] = (char)(0xC0 | u >> 6); t[1] = (char)(0x80 | (u & 63)); sb_put(b, t, 2); }
    else if (u < 0x10000) { t[0] = (char)(0xE0 | u >> 12); t[1] = (char)(0x80 | (u >> 6 & 63)); t[2] = (char)(0x80 | (u & 63));
                            sb_put(b, t, 3); }
    else { t[0] = (char)(0xF0 | u >> 18); t[1] = (char)(0x80 | (u >> 12 & 63)); t[2] = (char)(0x80 | (u >> 6 & 63));
           t[3] = (char)(0x80 | (u & 63)); sb_put(b, t, 4); }
}
char *sb_take(struct sb *b)
{
    if (!b->p && !b->nomem) sb_put(b, "", 0);
    if (b->nomem) {
        free(b->p);
        b->p = 0;
    }
    char *p = b->p;
    b->p = 0;
    return p;
}
void sb_free(struct sb *b) { free(b->p); b->p = 0; b->n = b->cap = 0; }

/* ---- characters ---- */
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int ncase(const char *a, const char *b, u32 n)
{
    for (; n; n--, a++, b++)
        if (lc((u8)*a) != lc((u8)*b)) return 1;
    return 0;
}
static int ws(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* one character of UTF-8 at s (n bytes left): its bytes, *cp; not valid UTF-8: 1 byte, read as ISO-8859-1 */
static u32 u8get(const u8 *s, u32 n, u32 *cp)
{
    u8 c = s[0];
    u32 need = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 9;
    if (need == 0) { *cp = c; return 1; }
    if (need > 3 || need >= n) { *cp = c; return 1; }
    u32 u = c & (0x3F >> need);
    for (u32 i = 1; i <= need; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = c; return 1; }
        u = u << 6 | (s[i] & 0x3F);
    }
    static const u32 minv[4] = { 0, 0x80, 0x800, 0x10000 };
    if (u < minv[need] || u > 0x10FFFF || (u >= 0xD800 && u < 0xE000)) { *cp = c; return 1; }
    *cp = u;
    return need + 1;
}

/* may this code point be shown? (newline and tab are the callers') */
static int showable(u32 u)
{
    return u >= 0x20 && !(u >= 0x7F && u < 0xA0) && !(u >= 0x202A && u <= 0x202E) && !(u >= 0x2066 && u <= 0x2069)
           && u != 0xFEFF && u != 0x200E && u != 0x200F;
}

int mail_clean(const char *in, u32 n, char *out, int cap)
{
    int o = 0;
    u32 i = 0;
    while (i < n && in[i]) {
        u32 cp, k = u8get((const u8 *)in + i, n - i, &cp);
        i += k;
        if (cp == '\t' || cp == '\n' || cp == '\r') cp = ' ';
        if (!showable(cp)) continue;
        int need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (o + need > cap - 1) break;
        if (need == 1) out[o++] = (char)cp;
        else if (need == 2) { out[o++] = (char)(0xC0 | cp >> 6); out[o++] = (char)(0x80 | (cp & 63)); }
        else if (need == 3) { out[o++] = (char)(0xE0 | cp >> 12); out[o++] = (char)(0x80 | (cp >> 6 & 63));
                              out[o++] = (char)(0x80 | (cp & 63)); }
        else { out[o++] = (char)(0xF0 | cp >> 18); out[o++] = (char)(0x80 | (cp >> 12 & 63));
               out[o++] = (char)(0x80 | (cp >> 6 & 63)); out[o++] = (char)(0x80 | (cp & 63)); }
    }
    out[o] = 0;
    return o;
}

/* ---- charsets ---- */
enum { CS_AUTO, CS_UTF8, CS_LATIN1, CS_CP1252, CS_LATIN9 };
static const unsigned short cp1252[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };

static int cs_id(const char *c)
{
    char b[24];
    int n = 0;
    for (; *c && n < 23; c++)
        if (*c != '-' && *c != '_' && *c != ' ') b[n++] = (char)lc((u8)*c);
    b[n] = 0;
    if (!strcmp(b, "utf8")) return CS_UTF8;
    if (!strcmp(b, "iso88591") || !strcmp(b, "latin1") || !strcmp(b, "l1")) return CS_LATIN1;
    if (!strcmp(b, "windows1252") || !strcmp(b, "cp1252")) return CS_CP1252;
    if (!strcmp(b, "iso885915") || !strcmp(b, "latin9")) return CS_LATIN9;
    return CS_AUTO;                     /* us-ascii and the unknown: UTF-8 where valid, else ISO-8859-1 */
}

static u32 cs_map(int cs, u8 c)
{
    if (cs == CS_CP1252 && c >= 0x80 && c < 0xA0) return cp1252[c - 0x80] ? cp1252[c - 0x80] : 0xFFFD;
    if (cs == CS_LATIN9)
        switch (c) {
        case 0xA4: return 0x20AC; case 0xA6: return 0x160; case 0xA8: return 0x161; case 0xB4: return 0x17D;
        case 0xB8: return 0x17E; case 0xBC: return 0x152; case 0xBD: return 0x153; case 0xBE: return 0x178;
        }
    return c;
}

static void text_x(struct sb *b, const char *in, u32 n, const char *charset, int tabs)
{
    int cs = cs_id(charset ? charset : ""), col = 0;
    u32 i = 0;
    while (i < n) {
        u32 cp;
        u8 c = (u8)in[i];
        if (c < 0x80 || cs == CS_LATIN1 || cs == CS_CP1252 || cs == CS_LATIN9) {
            cp = c < 0x80 ? c : cs_map(cs, c);
            i++;
        } else
            i += u8get((const u8 *)in + i, n - i, &cp);
        if (cp == '\r') {
            if (i < n && in[i] == '\n') continue;
            cp = '\n';
        }
        if (cp == '\n') {
            sb_putc(b, '\n');
            col = 0;
            continue;
        }
        if (cp == '\t') {
            if (tabs) sb_putc(b, '\t');
            else do sb_putc(b, ' '); while (++col % 8);
            continue;
        }
        if (cp == 0 || cp == 0x0C) cp = ' ';
        if (!showable(cp)) continue;
        sb_u8(b, cp);
        col++;
    }
}

void mime_text(struct sb *b, const char *in, u32 n, const char *charset) { text_x(b, in, n, charset, 0); }
void mime_keep(struct sb *b, const char *in, u32 n) { text_x(b, in, n, "", 1); }

/* ---- transfer encodings ---- */
static int b64v(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
           c == '+' ? 62 : c == '/' ? 63 : -1;
}
u32 b64_decode(const char *in, u32 n, char *out)
{
    u32 o = 0, bits = 0, nb = 0;
    for (u32 i = 0; i < n; i++) {
        if (in[i] == '=') break;
        int v = b64v((u8)in[i]);
        if (v < 0) continue;
        bits = bits << 6 | (u32)v;
        nb += 6;
        if (nb >= 8) {
            nb -= 8;
            out[o++] = (char)(bits >> nb);
        }
    }
    return o;
}
int b64_encode(const unsigned char *in, int n, char *out)
{
    static const char a[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        u32 v = (u32)in[i] << 16 | (i + 1 < n ? (u32)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = a[v >> 18];
        out[o++] = a[v >> 12 & 63];
        out[o++] = i + 1 < n ? a[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? a[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
u32 qp_decode(const char *in, u32 n, char *out, int hdr)
{
    u32 o = 0;
    for (u32 i = 0; i < n; i++) {
        char c = in[i];
        if (c == '_' && hdr) { out[o++] = ' '; continue; }
        if (c != '=') { out[o++] = c; continue; }
        if (i + 2 < n && hexv((u8)in[i + 1]) >= 0 && hexv((u8)in[i + 2]) >= 0) {
            out[o++] = (char)(hexv((u8)in[i + 1]) << 4 | hexv((u8)in[i + 2]));
            i += 2;
            continue;
        }
        u32 j = i + 1;                  /* a soft line break: "=", spaces, the line's end */
        while (j < n && (in[j] == ' ' || in[j] == '\t')) j++;
        if (j < n && in[j] == '\r') j++;
        if (j < n && in[j] == '\n') { i = j; continue; }
        if (j >= n) { i = j; continue; }
        out[o++] = c;                   /* "=" not followed by two hex digits: kept */
    }
    return o;
}

/* ---- headers ---- */
static u32 hdr_end(const char *raw, u32 n)     /* the header block's length (to the empty line, without it) */
{
    if ((n && raw[0] == '\n') || (n > 1 && raw[0] == '\r' && raw[1] == '\n'))
        return 0;                       /* no headers: the body at once */
    for (u32 i = 0; i < n; i++) {
        if (raw[i] != '\n') continue;
        if (i + 1 < n && raw[i + 1] == '\n') return i + 1;
        if (i + 2 < n && raw[i + 1] == '\r' && raw[i + 2] == '\n') return i + 1;
    }
    return n;
}
static u32 body_start(const char *raw, u32 n, u32 he)
{
    if (he < n && raw[he] == '\r') he++;
    if (he < n && raw[he] == '\n') he++;
    return he;
}

/* a header's raw value (unfolded into one line, at most cap - 1 bytes): its length, -1 none */
static int hraw(const char *h, u32 n, const char *name, char *out, int cap)
{
    u32 nl = (u32)strlen(name);
    for (u32 i = 0; i < n;) {
        if (i + nl < n && h[i + nl] == ':' && !ncase(h + i, name, nl)) {
            u32 j = i + nl + 1;
            int o = 0;
            while (j < n) {
                if (h[j] == '\r' || h[j] == '\n') {
                    u32 k = j;
                    if (h[k] == '\r') k++;
                    if (k < n && h[k] == '\n') k++;
                    if (k < n && (h[k] == ' ' || h[k] == '\t')) { j = k; continue; }
                    break;
                }
                if (o < cap - 1) out[o++] = h[j];
                j++;
            }
            while (o && ws((u8)out[o - 1])) o--;
            int s = 0;
            while (s < o && ws((u8)out[s])) s++;
            memmove(out, out + s, (u32)(o - s));
            out[o - s] = 0;
            return o - s;
        }
        while (i < n && h[i] != '\n') i++;
        i++;
    }
    out[0] = 0;
    return -1;
}

char *mime_hdecode(const char *v, u32 n)
{
    struct sb b;
    sb_init(&b, 4096);
    u32 i = 0;
    int last_enc = 0;
    while (i < n) {
        /* an encoded word: =?charset?B|Q?text?= */
        if (v[i] == '=' && i + 1 < n && v[i + 1] == '?') {
            u32 c0 = i + 2, c1 = c0;
            while (c1 < n && v[c1] != '?' && !ws((u8)v[c1])) c1++;
            if (c1 + 2 < n && v[c1] == '?' && v[c1 + 2] == '?' && strchr("BbQq", v[c1 + 1])) {
                u32 t0 = c1 + 3, t1 = t0;
                while (t1 + 1 < n && !(v[t1] == '?' && v[t1 + 1] == '=') && !ws((u8)v[t1])) t1++;
                if (t1 + 1 < n && v[t1] == '?' && v[t1 + 1] == '=' && c1 - c0 < 40) {
                    char cs[40], *dec = malloc(t1 - t0 + 4);
                    memcpy(cs, v + c0, c1 - c0);
                    cs[c1 - c0] = 0;
                    char *st = strchr(cs, '*');     /* RFC 2231's language */
                    if (st) *st = 0;
                    if (dec) {
                        u32 dn = (v[c1 + 1] | 32) == 'b' ? b64_decode(v + t0, t1 - t0, dec) : qp_decode(v + t0, t1 - t0, dec, 1);
                        mime_text(&b, dec, dn, cs);
                        free(dec);
                    }
                    i = t1 + 2;
                    last_enc = 1;
                    continue;
                }
            }
        }
        if (ws((u8)v[i])) {             /* white space: one space, none between two encoded words */
            u32 j = i;
            while (j < n && ws((u8)v[j])) j++;
            if (!(last_enc && j + 1 < n && v[j] == '=' && v[j + 1] == '?'))
                sb_putc(&b, ' ');
            i = j;
            continue;
        }
        u32 j = i;
        while (j < n && !ws((u8)v[j]) && !(v[j] == '=' && j + 1 < n && v[j + 1] == '?')) j++;
        if (j == i) j++;
        mime_text(&b, v + i, j - i, "");
        i = j;
        last_enc = 0;
    }
    char *r = sb_take(&b);
    if (r)
        for (char *p = r; *p; p++)
            if (*p == '\n') *p = ' ';
    return r;
}

char *mime_header(const char *hdr, u32 hlen, const char *name)
{
    char *t = malloc(4096);
    if (!t) return 0;
    int n = hraw(hdr, hlen, name, t, 4096);
    char *r = mime_hdecode(t, n < 0 ? 0 : (u32)n);
    free(t);
    return r;
}

/* a parameter of a header's value (charset=, boundary=, name=, filename=): into out, 1 found */
static int param(const char *v, const char *key, char *out, int cap)
{
    u32 kl = (u32)strlen(key);
    const char *p = v;
    int inq = 0;
    for (; *p; p++) {
        if (*p == '"') inq = !inq;
        if (inq || *p != ';') continue;
        const char *k = p + 1;
        while (ws((u8)*k)) k++;
        if (ncase(k, key, kl)) continue;
        const char *e = k + kl;
        while (ws((u8)*e)) e++;
        if (*e != '=') continue;
        e++;
        while (ws((u8)*e)) e++;
        int o = 0;
        if (*e == '"') {
            for (e++; *e && *e != '"'; e++) {
                if (*e == '\\' && e[1]) e++;
                if (o < cap - 1) out[o++] = *e;
            }
        } else
            for (; *e && *e != ';' && !ws((u8)*e); e++)
                if (o < cap - 1) out[o++] = *e;
        out[o] = 0;
        return 1;
    }
    out[0] = 0;
    return 0;
}

/* RFC 2231: charset'lang'%XX... as UTF-8 appended to b */
static void ext_value(struct sb *b, const char *v, int first)
{
    char cs[24] = "";
    if (first) {
        const char *q = strchr(v, '\'');
        if (q && strchr(q + 1, '\'')) {
            u32 l = (u32)(q - v) < sizeof cs - 1 ? (u32)(q - v) : sizeof cs - 1;
            memcpy(cs, v, l);
            cs[l] = 0;
            v = strchr(q + 1, '\'') + 1;
        }
    }
    char t[200];
    u32 o = 0;
    for (; *v && o < sizeof t; v++) {
        if (*v == '%' && hexv((u8)v[1]) >= 0 && hexv((u8)v[2]) >= 0) {
            t[o++] = (char)(hexv((u8)v[1]) << 4 | hexv((u8)v[2]));
            v += 2;
        } else
            t[o++] = *v;
    }
    mime_text(b, t, o, cs);
}

/* an attachment's file name: filename*, filename*0.., filename, then the same for name; clean, no path */
static void file_name(const char *disp, const char *ctype, char *out, int cap)
{
    struct sb b;
    char t[200], k[24];
    sb_init(&b, 400);
    const char *src[2] = { disp, ctype }, *key[2] = { "filename", "name" };
    for (int s = 0; s < 2 && !b.n; s++) {
        snprintf(k, sizeof k, "%s*", key[s]);
        if (param(src[s], k, t, sizeof t)) { ext_value(&b, t, 1); break; }
        for (int i = 0; i < 20; i++) {
            snprintf(k, sizeof k, "%s*%d*", key[s], i);
            if (param(src[s], k, t, sizeof t)) { ext_value(&b, t, i == 0); continue; }
            snprintf(k, sizeof k, "%s*%d", key[s], i);
            if (param(src[s], k, t, sizeof t)) { sb_puts(&b, t); continue; }
            break;
        }
        if (!b.n && param(src[s], key[s], t, sizeof t)) {
            char *d = mime_hdecode(t, (u32)strlen(t));
            if (d) { sb_puts(&b, d); free(d); }
        }
    }
    int n = mail_clean(b.p ? b.p : "", b.n, out, cap);
    sb_free(&b);
    for (int i = 0; i < n; i++)
        if (out[i] == '/' || out[i] == '\\' || out[i] == ':') out[i] = '_';
    int s = 0;
    while (s < n && (out[s] == '.' || out[s] == ' ')) s++;
    memmove(out, out + s, (u32)(n - s) + 1);
}

/* ---- dates ---- */
static u32 days(int y, int m, int d)    /* days since 1970-01-01 (proleptic Gregorian) */
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (u32)(era * 146097 + (int)doe - 719468);
}
void mail_civil(u32 t, int *y, int *mo, int *d, int *h, int *mi, int *s, int *wd)
{
    u32 z = t / 86400, r = t % 86400;
    *h = (int)(r / 3600);
    *mi = (int)(r / 60 % 60);
    *s = (int)(r % 60);
    *wd = (int)((z + 4) % 7);           /* 1970-01-01 was a Thursday; 0 Sunday */
    int zz = (int)z + 719468, era = zz / 146097;
    unsigned doe = (unsigned)(zz - era * 146097), yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)yoe + era * 400 + (*mo <= 2);
}
static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
u32 mime_date(const char *v)
{
    int d = -1, m = -1, y = -1, h = 0, mi = 0, s = 0, z = 0;
    const char *p = v;
    while (*p) {
        while (*p && (ws((u8)*p) || *p == ',')) p++;
        const char *w = p;
        while (*p && !ws((u8)*p) && *p != ',') p++;
        u32 n = (u32)(p - w);
        if (!n) break;
        if (n >= 3 && isalpha((u8)w[0])) {
            for (int i = 0; i < 12; i++)
                if (!ncase(w, months + i * 3, 3) && m < 0) m = i + 1;
            continue;
        }
        if ((w[0] == '+' || w[0] == '-') && n == 5) {
            int k = atoi(w + 1);
            z = (k / 100 * 60 + k % 100) * 60 * (w[0] == '-' ? -1 : 1);
            continue;
        }
        if (strchr(w, ':') && strchr(w, ':') < p) {
            h = atoi(w);
            mi = atoi(strchr(w, ':') + 1);
            const char *c2 = strchr(strchr(w, ':') + 1, ':');
            s = c2 && c2 < p ? atoi(c2 + 1) : 0;
            continue;
        }
        if (isdigit((u8)w[0])) {
            int k = atoi(w);
            if (d < 0 && n <= 2) d = k;
            else if (y < 0) y = n == 2 ? k + (k < 50 ? 2000 : 1900) : k;
        }
    }
    if (d < 1 || d > 31 || m < 1 || y < 1970 || y > 2105 || h > 23 || mi > 59 || s > 60) return 0;
    long long t = (long long)days(y, m, d) * 86400 + h * 3600 + mi * 60 + s - z;
    return t > 0 && t < 0xFFFFFFFFll ? (u32)t : 0;
}

/* ---- HTML made text ---- */
static void nlines(struct sb *b, int k)  /* the text ends with k line breaks (never at its start) */
{
    while (b->n && b->p[b->n - 1] == ' ') b->n--, b->p[b->n] = 0;
    if (!b->n) return;
    int have = 0;
    while (have < (int)b->n && have < k && b->p[b->n - 1 - have] == '\n') have++;
    for (; have < k; have++) sb_putc(b, '\n');
}
static const struct { const char *n; unsigned short c; } ents[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", ' ' },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "hellip", 0x2026 }, { "mdash", 0x2014 }, { "ndash", 0x2013 },
    { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "euro", 0x20AC },
    { "bull", 0x2022 }, { "middot", 0xB7 }, { "auml", 0xE4 }, { "ouml", 0xF6 }, { "uuml", 0xFC }, { "aring", 0xE5 },
    { "Auml", 0xC4 }, { "Ouml", 0xD6 }, { "Uuml", 0xDC }, { "Aring", 0xC5 }, { "eacute", 0xE9 }, { "szlig", 0xDF },
};
/* an entity at s (after '&', n bytes): its code point, *len the bytes used (with ';'); 0 none */
static u32 entity(const char *s, u32 n, u32 *len)
{
    u32 e = 0;
    while (e < n && e < 12 && s[e] != ';' && !ws((u8)s[e]) && s[e] != '&' && s[e] != '<') e++;
    if (e >= n || s[e] != ';' || !e) return 0;
    *len = e + 1;
    if (s[0] == '#') {
        u32 v = 0;
        int hex = s[1] == 'x' || s[1] == 'X';
        for (u32 i = hex ? 2 : 1; i < e; i++) {
            int d = hex ? hexv((u8)s[i]) : isdigit((u8)s[i]) ? s[i] - '0' : -1;
            if (d < 0 || v > 0x10FFFF) return 0;
            v = v * (hex ? 16 : 10) + (u32)d;
        }
        if (v == 0 || v > 0x10FFFF || (v >= 0xD800 && v < 0xE000)) return 0xFFFD;
        return v >= 0x80 && v < 0xA0 ? cp1252[v - 0x80] ? cp1252[v - 0x80] : 0xFFFD : v;
    }
    for (u32 i = 0; i < sizeof ents / sizeof ents[0]; i++)
        if (strlen(ents[i].n) == e && !strncmp(ents[i].n, s, e)) return ents[i].c;
    return 0;
}
static int tagis(const char *t, u32 n, const char *name)
{
    u32 l = (u32)strlen(name);
    return n == l && !ncase(t, name, l);
}
/* an attribute's value in a tag's text (href=...): into out, its length */
static int attr(const char *t, u32 n, const char *name, char *out, int cap)
{
    u32 l = (u32)strlen(name);
    for (u32 i = 0; i + l < n; i++) {
        if (ncase(t + i, name, l) || (i && !ws((u8)t[i - 1]))) continue;
        u32 j = i + l;
        while (j < n && ws((u8)t[j])) j++;
        if (j >= n || t[j] != '=') continue;
        j++;
        while (j < n && ws((u8)t[j])) j++;
        char q = j < n && (t[j] == '"' || t[j] == '\'') ? t[j++] : 0;
        int o = 0;
        for (; j < n && (q ? t[j] != q : !ws((u8)t[j]) && t[j] != '>'); j++) {
            if (t[j] == '&' && j + 4 < n && !strncmp(t + j, "&amp;", 5)) { if (o < cap - 1) out[o++] = '&'; j += 4; continue; }
            if ((u8)t[j] > 32 && (u8)t[j] < 127 && o < cap - 1) out[o++] = t[j];
        }
        out[o] = 0;
        return o;
    }
    out[0] = 0;
    return 0;
}
void mime_html(struct sb *b, const char *in, u32 n)
{
    u32 i = 0, link_at = 0;
    int space = 0, pre = 0;
    char href[300] = "";
    while (i < n) {
        char c = in[i];
        if (c == '<') {
            if (i + 3 < n && !strncmp(in + i, "<!--", 4)) {     /* a comment */
                const char *e = 0;
                for (u32 j = i + 4; j + 2 < n; j++)
                    if (in[j] == '-' && in[j + 1] == '-' && in[j + 2] == '>') { e = in + j; break; }
                i = e ? (u32)(e - in) + 3 : n;
                continue;
            }
            u32 j = i + 1;
            int close = j < n && in[j] == '/';
            if (close) j++;
            u32 t0 = j;
            while (j < n && (isalnum((u8)in[j]) || in[j] == '!')) j++;
            u32 tn = j - t0;
            if (!tn) {                  /* a lone '<': text */
                if (space) sb_putc(b, ' ');
                space = 0;
                sb_putc(b, '<');
                i++;
                continue;
            }
            char q = 0;                 /* the tag's end, quotes respected */
            for (; j < n; j++) {
                if (q) { if (in[j] == q) q = 0; }
                else if (in[j] == '"' || in[j] == '\'') q = in[j];
                else if (in[j] == '>') break;
            }
            const char *tag = in + t0, *rest = in + t0 + tn;
            u32 rn = j > t0 + tn ? j - t0 - tn : 0;
            i = j < n ? j + 1 : n;
            if (!close && (tagis(tag, tn, "script") || tagis(tag, tn, "style") || tagis(tag, tn, "head") ||
                           tagis(tag, tn, "title"))) {
                /* skipped to its end tag */
                u32 k = i;
                for (; k + tn + 2 < n; k++)
                    if (in[k] == '<' && in[k + 1] == '/' && !ncase(in + k + 2, tag, tn)) break;
                while (k < n && in[k] != '>') k++;
                i = k < n ? k + 1 : n;
                continue;
            }
            if (tagis(tag, tn, "br")) { while (b->n && b->p[b->n - 1] == ' ') b->n--; sb_putc(b, '\n'); space = 0; }
            else if (tagis(tag, tn, "p") || tagis(tag, tn, "div") || tagis(tag, tn, "table") || tagis(tag, tn, "ul") ||
                     tagis(tag, tn, "ol") || tagis(tag, tn, "blockquote") || (tn == 2 && (tag[0] | 32) == 'h' &&
                     tag[1] >= '1' && tag[1] <= '6')) { nlines(b, tagis(tag, tn, "div") ? 1 : 2); space = 0; }
            else if (tagis(tag, tn, "tr")) { nlines(b, 1); space = 0; }
            else if (tagis(tag, tn, "li") && !close) { nlines(b, 1); sb_puts(b, "* "); space = 0; }
            else if (tagis(tag, tn, "hr")) { nlines(b, 1); sb_puts(b, "--------"); nlines(b, 1); space = 0; }
            else if (tagis(tag, tn, "td") || tagis(tag, tn, "th")) space = 1;
            else if (tagis(tag, tn, "pre")) { pre = !close; nlines(b, 1); space = 0; }
            else if (tagis(tag, tn, "img") && !close) {
                char alt[80];
                char ac[80];
                if (attr(rest, rn, "alt", alt, sizeof alt) && mail_clean(alt, (u32)strlen(alt), ac, sizeof ac)) {
                    if (space) sb_putc(b, ' ');
                    sb_printf(b, "[%s]", ac);
                    space = 0;
                }
            } else if (tagis(tag, tn, "a")) {
                if (!close) {
                    attr(rest, rn, "href", href, sizeof href);
                    link_at = b->n;
                } else if (href[0] && (!ncase(href, "http", 4) || !ncase(href, "mailto:", 7))) {
                    const char *h = !ncase(href, "mailto:", 7) ? href + 7 : href;
                    u32 tl = b->n - link_at;
                    if (b->n < link_at || tl != strlen(h) || strncmp(b->p + link_at, h, tl))
                        sb_printf(b, " <%s>", h);
                    href[0] = 0;
                }
            }
            continue;
        }
        if (c == '&') {
            u32 len = 0, cp = entity(in + i + 1, n - i - 1, &len);
            if (cp) {
                if (space) sb_putc(b, ' ');
                space = 0;
                if (cp == ' ' || cp == 0xA0) sb_putc(b, ' ');
                else if (showable(cp)) sb_u8(b, cp);
                i += len + 1;
                continue;
            }
        }
        if (ws((u8)c) && !pre) {
            if (b->n && b->p[b->n - 1] != '\n') space = 1;
            i++;
            continue;
        }
        if (space) sb_putc(b, ' ');
        space = 0;
        sb_putc(b, c);
        i++;
    }
    while (b->n && (b->p[b->n - 1] == '\n' || b->p[b->n - 1] == ' ')) b->n--, b->p[b->n] = 0;
}

/* ---- the MIME tree ---- */
struct ent {                            /* an entity's header facts */
    char type[48], charset[24], boundary[80];
    int enc, attach;
    char *ct, *disp;                    /* the raw (unfolded) values */
};
static int ent_read(struct ent *e, const char *h, u32 hn, const char *deftype)
{
    memset(e, 0, sizeof *e);
    if (!(e->ct = malloc(1024)) || !(e->disp = malloc(1024))) {
        free(e->ct);
        return ME_MEM;
    }
    char cte[40];
    if (hraw(h, hn, "Content-Type", e->ct, 1024) < 0) e->ct[0] = 0;
    if (hraw(h, hn, "Content-Disposition", e->disp, 1024) < 0) e->disp[0] = 0;
    if (hraw(h, hn, "Content-Transfer-Encoding", cte, sizeof cte) < 0) cte[0] = 0;
    int i = 0;
    for (const char *p = e->ct; *p && *p != ';' && !ws((u8)*p) && i < 47; p++)    /* a token's characters only */
        if (isalnum((u8)*p) || strchr("!#$&^_.+-/", *p)) e->type[i++] = (char)lc((u8)*p);
    e->type[i] = 0;
    if (!strchr(e->type, '/')) snprintf(e->type, sizeof e->type, "%s", deftype);
    param(e->ct, "charset", e->charset, sizeof e->charset);
    for (char *p = e->charset; *p; p++)
        if (!isalnum((u8)*p) && !strchr("-_.:", *p)) *p = '_';
    param(e->ct, "boundary", e->boundary, sizeof e->boundary);
    e->enc = !ncase(cte, "base64", 6) ? ENC_B64 : !ncase(cte, "quoted-printable", 16) ? ENC_QP : ENC_7BIT;
    e->attach = !ncase(e->disp, "attachment", 10);
    return 0;
}
static void ent_free(struct ent *e) { free(e->ct); free(e->disp); }

static char *decode(int enc, const char *b, u32 n, u32 *on)
{
    char *o = malloc(n + 4);
    if (!o) return 0;
    *on = enc == ENC_B64 ? b64_decode(b, n, o) : enc == ENC_QP ? qp_decode(b, n, o, 0) : (memcpy(o, b, n), n);
    return o;
}
char *mime_decode_part(const struct mpart *p, u32 *n) { return decode(p->enc, p->body, p->len, n); }

/* a multipart body's parts: start and length of each (at most max); the delimiter "--boundary" at a line's start */
static int split(const char *b, u32 n, const char *bd, u32 *st, u32 *ln, int max)
{
    u32 bl = (u32)strlen(bd);
    int np = -1;
    for (u32 i = 0; i < n;) {
        if ((i == 0 || b[i - 1] == '\n') && i + 2 + bl <= n && b[i] == '-' && b[i + 1] == '-' && !strncmp(b + i + 2, bd, bl)) {
            u32 e = i;                  /* the part before ends before the line break ahead of the delimiter */
            if (e && b[e - 1] == '\n') e--;
            if (e && b[e - 1] == '\r') e--;
            if (np >= 0 && np < max) ln[np] = e > st[np] ? e - st[np] : 0;
            int last = i + 4 + bl <= n && b[i + 2 + bl] == '-' && b[i + 3 + bl] == '-';
            u32 j = i + 2 + bl;
            while (j < n && b[j] != '\n') j++;
            if (last) return np + 1 < max ? np + 1 : max;
            np++;
            if (np < max) { st[np] = j < n ? j + 1 : n; ln[np] = n - st[np]; }
            i = j < n ? j + 1 : n;
            continue;
        }
        while (i < n && b[i] != '\n') i++;
        i++;
    }
    return np + 1 < max ? np + 1 : max;
}

static void walk(struct mmsg *m, struct sb *t, const char *p, u32 n, int depth, const char *deftype);

static void add_att(struct mmsg *m, const struct ent *e, const char *body, u32 len)
{
    if (m->natt >= MAIL_PARTMAX) { m->moreatt++; return; }
    struct mpart *a = &m->att[m->natt++];
    snprintf(a->type, sizeof a->type, "%s", e->type);
    snprintf(a->charset, sizeof a->charset, "%s", e->charset);
    file_name(e->disp, e->ct, a->name, sizeof a->name);
    a->enc = e->enc;
    a->body = body;
    a->len = len;
    a->size = e->enc == ENC_B64 ? len / 4 * 3 : len;
}

static void sep(struct sb *t)
{
    if (t->n) { nlines(t, 1); sb_putc(t, '\n'); }
}

static void walk(struct mmsg *m, struct sb *t, const char *p, u32 n, int depth, const char *deftype)
{
    struct ent e;
    u32 he = hdr_end(p, n), bs = body_start(p, n, he);
    if (ent_read(&e, p, he, deftype) < 0) { t->nomem = 1; return; }
    const char *body = p + bs;
    u32 bn = n - bs;
    if (!strncmp(e.type, "multipart/", 10) && e.boundary[0] && depth < MAIL_DEPTH) {
        u32 *st = malloc(2 * 64 * sizeof *st), *ln = st + 64;    /* (the heap: 9 levels on a 16 KB stack) */
        if (!st) { t->nomem = 1; ent_free(&e); return; }
        int np = split(body, bn, e.boundary, st, ln, 64);
        const char *sub = !strcmp(e.type, "multipart/digest") ? "message/rfc822" : "text/plain";
        if (!strcmp(e.type, "multipart/alternative")) {
            int best = -1, rank = 0;     /* text/plain first, then HTML, else the last */
            for (int i = 0; i < np; i++) {
                struct ent c;
                u32 ch = hdr_end(body + st[i], ln[i]);
                if (ent_read(&c, body + st[i], ch, "text/plain") < 0) { t->nomem = 1; break; }
                int r = !strcmp(c.type, "text/plain") ? 3 : !strcmp(c.type, "text/html") ? 2 :
                        !strncmp(c.type, "multipart/", 10) ? 2 : 1;
                if (r > rank || (r == rank && r == 1)) { rank = r; best = i; }
                ent_free(&c);
            }
            if (best >= 0) walk(m, t, body + st[best], ln[best], depth + 1, "text/plain");
        } else
            for (int i = 0; i < np; i++)
                walk(m, t, body + st[i], ln[i], depth + 1, sub);
        free(st);
    } else if (!strcmp(e.type, "message/rfc822") && !e.attach && depth < MAIL_DEPTH) {
        u32 ih = hdr_end(body, bn);
        static const char *const hn[] = { "From", "Date", "Subject", "To" };
        sep(t);
        sb_puts(t, "-------- message --------\n");
        for (int i = 0; i < 4; i++) {
            char *v = mime_header(body, ih, hn[i]);
            if (v && *v) sb_printf(t, "%s: %.250s\n", hn[i], v);
            free(v);
        }
        sb_putc(t, '\n');
        walk(m, t, body, bn, depth + 1, "text/plain");
    } else if ((!strcmp(e.type, "text/plain") || !strcmp(e.type, "text/html") || !strcmp(e.type, "text/enriched")) &&
               !e.attach) {
        u32 dn;
        char *d = decode(e.enc, body, bn, &dn);
        if (!d) { t->nomem = 1; ent_free(&e); return; }
        sep(t);
        if (!strcmp(e.type, "text/html")) {
            struct sb h;
            sb_init(&h, MAIL_TEXTMAX);
            mime_text(&h, d, dn, e.charset);
            if (h.p) mime_html(t, h.p, h.n);
            if (h.nomem) t->nomem = 1;
            sb_free(&h);
            m->html = 1;
        } else
            mime_text(t, d, dn, e.charset);
        free(d);
    } else
        add_att(m, &e, body, bn);
    ent_free(&e);
}

int mime_parse(struct mmsg *m, const char *raw, u32 n)
{
    static const char *const hn[] = { "From", "To", "Cc", "Subject", "Date", "Reply-To", "Message-ID", "References" };
    memset(m, 0, sizeof *m);
    u32 he = hdr_end(raw, n);
    m->hdr = raw;
    m->hdrlen = he;
    char **f[8] = { &m->from, &m->to, &m->cc, &m->subject, &m->date, &m->replyto, &m->msgid, &m->refs };
    for (int i = 0; i < 8; i++)
        if (!(*f[i] = mime_header(raw, he, hn[i]))) { mime_free(m); return ME_MEM; }
    struct sb t;
    sb_init(&t, MAIL_TEXTMAX);
    walk(m, &t, raw, n, 0, "text/plain");
    m->cut = t.full;
    if (t.nomem) { sb_free(&t); mime_free(m); return ME_MEM; }
    while (t.n && (t.p[t.n - 1] == '\n' || t.p[t.n - 1] == ' ')) t.n--;
    if (t.p) t.p[t.n] = 0;
    m->tlen = t.n;
    if (!(m->text = sb_take(&t))) { mime_free(m); return ME_MEM; }
    return 0;
}

void mime_free(struct mmsg *m)
{
    char *f[9] = { m->from, m->to, m->cc, m->subject, m->date, m->replyto, m->msgid, m->refs, m->text };
    for (int i = 0; i < 9; i++) free(f[i]);
    memset(m, 0, sizeof *m);
}
