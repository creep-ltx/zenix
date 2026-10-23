/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* chmod MODE file...: MODE octal (644) or symbolic ([ugoa]*[+-=][rwxX]+ , comma separated) */
static int apply(const char *spec, unsigned cur, unsigned *out, int isdir)
{
    if (isdigit((unsigned char)spec[0])) { char *e; *out = (unsigned)strtoul(spec, &e, 8); return !*e; }
    unsigned mode = cur & 07777;
    while (*spec) {
        unsigned who = 0;
        while (*spec == 'u' || *spec == 'g' || *spec == 'o' || *spec == 'a') { who |= *spec == 'u' ? 0700 : *spec == 'g' ? 070 : *spec == 'o' ? 07 : 0777; spec++; }
        if (!who) who = 0777;
        while (*spec == '+' || *spec == '-' || *spec == '=') {
            char op = *spec++; unsigned bits = 0;
            while (*spec && *spec != ',' && *spec != '+' && *spec != '-' && *spec != '=') {
                unsigned b = 0;
                if (*spec == 'r') b = 0444; else if (*spec == 'w') b = 0222; else if (*spec == 'x') b = 0111;
                else if (*spec == 'X') b = (isdir || (cur & 0111)) ? 0111 : 0;
                else return 0;
                bits |= b; spec++;
            }
            bits &= who;
            if (op == '+') mode |= bits; else if (op == '-') mode &= ~bits; else mode = (mode & ~who) | bits;
        }
        if (*spec == ',') spec++; else if (*spec) return 0;
    }
    *out = mode; return 1;
}
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(2, "usage: chmod MODE file...\n"); return 2; }
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        struct ustat st;
        if (sys_stat(argv[i], &st) < 0) { fprintf(2, "chmod: %s: not found\n", argv[i]); rc = 1; continue; }
        unsigned mode;
        if (!apply(argv[1], st.mode, &mode, (st.mode & S_IFMT) == S_IFDIR)) { fprintf(2, "chmod: invalid mode '%s'\n", argv[1]); return 2; }
        if (sys_chmod(argv[i], mode) < 0) { fprintf(2, "chmod: %s: cannot change\n", argv[i]); rc = 1; }
    }
    return rc;
}
