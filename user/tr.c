/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* tr [-d] [-s] SET1 [SET2]: translate, delete or squeeze characters; ranges a-z, classes [:upper:] [:lower:] [:digit:] [:alpha:] [:alnum:] [:space:], \n \t \\ */
static int expand(const char *s, unsigned char *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        if (s[0] == '[' && s[1] == ':') {
            const char *e = strstr(s, ":]");
            if (e) {
                char nm[12]; u32 l = (u32)(e - s - 2); if (l > 10) l = 10; memcpy(nm, s + 2, l); nm[l] = 0;
                for (int c = 0; c < 256 && n < max; c++) {
                    int ok = !strcmp(nm, "upper") ? isupper(c) : !strcmp(nm, "lower") ? islower(c) : !strcmp(nm, "digit") ? isdigit(c) : !strcmp(nm, "alpha") ? isalpha(c) : !strcmp(nm, "alnum") ? isalnum(c) : !strcmp(nm, "space") ? isspace(c) : 0;
                    if (ok) out[n++] = (unsigned char)c;
                }
                s = e + 2; continue;
            }
        }
        int c = (unsigned char)*s++;
        if (c == '\\' && *s) { c = *s++; c = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c; }
        if (*s == '-' && s[1]) {
            int hi = (unsigned char)s[1]; s += 2;
            if (hi == '\\' && *s) { hi = *s++; hi = hi == 'n' ? '\n' : hi == 't' ? '\t' : hi; }
            for (int k = c; k <= hi && n < max; k++) out[n++] = (unsigned char)k;
        } else out[n++] = (unsigned char)c;
    }
    return n;
}
static const char USAGE[] = "usage: tr [-d] [-s] SET1 [SET2]\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int del = 0, sq = 0, i = 1;
    for (; zisopt(argc, argv, &i, USAGE); i++) for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'd') del = 1; else if (*p == 's') sq = 1; else zbadch(*p, USAGE); }
    if (i >= argc) zusage(USAGE);
    static unsigned char s1[512], s2[512]; int n1 = expand(argv[i], s1, 512), n2 = i + 1 < argc ? expand(argv[i + 1], s2, 512) : 0;
    unsigned char map[256]; char in1[256], in2[256];
    for (int c = 0; c < 256; c++) { map[c] = (unsigned char)c; in1[c] = in2[c] = 0; }
    for (int k = 0; k < n1; k++) { in1[s1[k]] = 1; if (!del && n2) map[s1[k]] = s2[k < n2 ? k : n2 - 1]; }
    for (int k = 0; k < n2; k++) in2[s2[k]] = 1;
    int last = -1; char b[4096]; int n;            /* 4 KB a call, as wc and tee (audit 2026-11-23 OS-15) */
    while ((n = sys_read(0, b, sizeof b)) > 0)
        for (int k = 0; k < n; k++) {
            int c = (unsigned char)b[k];
            if (del && in1[c]) continue;
            int o = map[c];
            if (sq && (n2 ? in2[o] : in1[c]) && o == last) continue;
            oputc(o); last = o;
        }
    return 0;
}
