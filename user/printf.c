// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* printf FORMAT [ARGUMENT...]: formatted output as in the shell standard. The format is reused until the arguments
 * are used up. Escapes in the format: \\ \a \b \f \n \r \t \v \NNN (octal) \xHH; conversions: d i u x X o c s b %%
 * with flags, width and precision (also * taking an argument). */
static int rc;
static char **args; static int nargs, argi;

static const char *next_arg(void) { return argi < nargs ? args[argi++] : 0; }

static int esc(const char **pp, char *out)               /* one backslash escape at *pp (after the backslash) */
{
    const char *p = *pp; char c = *p;
    if (!c) { *out = '\\'; return 0; }
    p++;
    switch (c) {
    case 'a': *out = 7; break; case 'b': *out = 8; break; case 'f': *out = 12; break; case 'n': *out = 10; break;
    case 'r': *out = 13; break; case 't': *out = 9; break; case 'v': *out = 11; break; case '\\': *out = '\\'; break;
    case 'x': { int v = 0, n = 0; while (n < 2 && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')))
                { v = v * 16 + (*p <= '9' ? *p - '0' : (*p | 32) - 'a' + 10); p++; n++; }
                if (!n) { *out = '\\'; p--; } else *out = (char)v; break; }
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
        int v = c - '0', n = 1; while (n < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; n++; }
        *out = (char)v; break; }
    default: *out = '\\'; p--; break;
    }
    *pp = p;
    return 1;
}

static long number(const char *s)
{
    if (!s) return 0;
    if (s[0] == '\'' || s[0] == '"') return (unsigned char)s[1];
    char *end; long v = (long)strtoul(s[0] == '-' ? s + 1 : s, &end, 0);
    if (s[0] == '-') v = -v;
    if (*end || end == s) { fprintf(2, "printf: %s: not a completely converted number\n", s); rc = 1; }
    return v;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(2, "usage: printf FORMAT [ARGUMENT...]\n"); return 2; }
    const char *fmt = argv[1];
    args = argv + 2; nargs = argc - 2;
    do {
        int used_before = argi;
        for (const char *p = fmt; *p; p++) {
            if (*p == '\\') { p++; char c; if (esc(&p, &c)) { oputc(c); p--; } else oputc('\\'); continue; }
            if (*p != '%') { oputc(*p); continue; }
            p++;
            if (*p == '%') { oputc('%'); continue; }
            char spec[48]; int sl = 0; spec[sl++] = '%';
            while (*p && strchr("-+ #0", *p) && sl < 10) spec[sl++] = *p++;
            if (*p == '*') { sl += snprintf(spec + sl, 12, "%d", (int)number(next_arg())); p++; }
            else while (*p >= '0' && *p <= '9' && sl < 20) spec[sl++] = *p++;
            if (*p == '.') {
                spec[sl++] = *p++;
                if (*p == '*') { sl += snprintf(spec + sl, 12, "%d", (int)number(next_arg())); p++; }
                else while (*p >= '0' && *p <= '9' && sl < 30) spec[sl++] = *p++;
            }
            while (*p == 'l' || *p == 'h') p++;
            char conv = *p;
            if (!conv) { oputs(spec); break; }
            if (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
                spec[sl++] = conv; spec[sl] = 0;
                long v = number(next_arg());
                if (conv == 'd' || conv == 'i') oprintf(spec, (int)v); else oprintf(spec, (unsigned)v);
            } else if (conv == 'c') {
                const char *a = next_arg(); spec[sl++] = 'c'; spec[sl] = 0; oprintf(spec, a ? a[0] : 0);
            } else if (conv == 's') {
                const char *a = next_arg(); spec[sl++] = 's'; spec[sl] = 0; oprintf(spec, a ? a : "");
            } else if (conv == 'b') {                      /* the argument with its escapes expanded */
                const char *a = next_arg(); char out[512]; int o = 0;
                for (; a && *a && o < 511; a++) { if (*a == '\\') { a++; char c; if (esc(&a, &c)) { out[o++] = c; a--; } else out[o++] = '\\'; } else out[o++] = *a; }
                out[o] = 0; spec[sl++] = 's'; spec[sl] = 0; oprintf(spec, out);
            } else { fprintf(2, "printf: %%%c: invalid conversion\n", conv); return 1; }
        }
        if (argi == used_before) break;                    /* the format used no argument: once is enough */
    } while (argi < nargs);
    return rc;
}
