/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "regex.h"
/* expr arg op arg ...: arithmetic (+ - \* / %), comparisons, & |, STRING : REGEX, length STRING; prints the result,
 * status 1 if it is 0 or empty */
static char **av; static int ac, ai;
static long num(const char *s, int *ok) { char *e; long v = strtol(s, &e, 10); *ok = *s && !*e; return v; }
static char *val(void);
static char *fmt(long v) { char b[16]; { b[0] = 0; char t[12]; int n = 0; u32 u = (u32)(v < 0 ? -v : v); do { t[n++] = (char)(0x30 + u % 10); u /= 10; } while (u); int k = 0; if (v < 0) b[k++] = 0x2d; while (n) b[k++] = t[--n]; b[k] = 0; } return strdup(b); }
static char *prim(void)
{
    if (ai >= ac) return strdup("");
    if (!strcmp(av[ai], "(")) { ai++; char *v = val(); if (ai < ac && !strcmp(av[ai], ")")) ai++; return v; }
    if (!strcmp(av[ai], "length") && ai + 1 < ac) { ai += 2; return fmt((long)strlen(av[ai - 1])); }
    return strdup(av[ai++]);
}
/* STRING : REGEX (a basic regular expression, anchored at the start of STRING, as POSIX expr): the text of the first
 * \( \) group, or the number of characters matched (0 = no match) */
static char *match(void)
{
    char *l = prim();
    while (ai < ac && !strcmp(av[ai], ":")) {
        ai++;
        char *r = prim();
        char *ere = bre_to_ere(r), *anch = xmalloc(strlen(ere) + 2);
        anch[0] = '^'; strcpy(anch + 1, r[0] == '^' ? ere + 1 : ere);       /* "^" in the expression is the anchor anyway */
        void *re = re_comp(anch, RE_LONGEST);
        if (!re) { dprintf(2, "expr: bad regular expression: %s\n", r); _zexit(2); }
        int caps[2 * RE_GROUPS]; for (int k = 0; k < 2 * RE_GROUPS; k++) caps[k] = -1;
        int len = (int)strlen(l), hit = re_exec(re, l, len, 0, caps);
        int groups = 0; for (const char *q = r; *q; q++) if (q[0] == '\\' && q[1] == '(') { groups = 1; break; }
        char *res;
        if (groups) {
            if (hit && caps[2] >= 0 && caps[3] >= caps[2]) { res = xmalloc((u32)(caps[3] - caps[2]) + 1); memcpy(res, l + caps[2], (u32)(caps[3] - caps[2])); res[caps[3] - caps[2]] = 0; }
            else res = strdup("");
        } else res = fmt(hit ? (long)(caps[1] - caps[0]) : 0);
        re_free(re); free(ere); free(anch);
        l = res;
    }
    return l;
}
static char *mul(void)
{
    char *l = match();
    while (ai < ac && (!strcmp(av[ai], "*") || !strcmp(av[ai], "/") || !strcmp(av[ai], "%"))) {
        char op = av[ai++][0]; char *r = match(); int ok1, ok2; long a = num(l, &ok1), b = num(r, &ok2);
        if (!ok1 || !ok2) { dprintf(2, "expr: non-numeric argument\n"); _zexit(2); }
        if (op != '*' && !b) { dprintf(2, "expr: division by zero\n"); _zexit(2); }
        l = fmt(op == '*' ? a * b : op == '/' ? a / b : a % b);
    }
    return l;
}
static char *add(void)
{
    char *l = mul();
    while (ai < ac && (!strcmp(av[ai], "+") || !strcmp(av[ai], "-"))) {
        char op = av[ai++][0]; char *r = mul(); int ok1, ok2; long a = num(l, &ok1), b = num(r, &ok2);
        if (!ok1 || !ok2) { dprintf(2, "expr: non-numeric argument\n"); _zexit(2); }
        l = fmt(op == '+' ? a + b : a - b);
    }
    return l;
}
static char *cmpop(void)
{
    char *l = add();
    while (ai < ac && (!strcmp(av[ai], "=") || !strcmp(av[ai], "!=") || !strcmp(av[ai], "<") || !strcmp(av[ai], ">") || !strcmp(av[ai], "<=") || !strcmp(av[ai], ">="))) {
        const char *op = av[ai++]; char *r = add(); int ok1, ok2; long a = num(l, &ok1), b = num(r, &ok2);
        int c = (ok1 && ok2) ? (a < b ? -1 : a > b) : strcmp(l, r);
        int t = !strcmp(op, "=") ? c == 0 : !strcmp(op, "!=") ? c != 0 : !strcmp(op, "<") ? c < 0 : !strcmp(op, ">") ? c > 0 : !strcmp(op, "<=") ? c <= 0 : c >= 0;
        l = fmt(t);
    }
    return l;
}
static char *andop(void) { char *l = cmpop(); while (ai < ac && !strcmp(av[ai], "&")) { ai++; char *r = cmpop(); if (!*l || !strcmp(l, "0") || !*r || !strcmp(r, "0")) l = strdup("0"); } return l; }
static char *val(void) { char *l = andop(); while (ai < ac && !strcmp(av[ai], "|")) { ai++; char *r = andop(); if (!*l || !strcmp(l, "0")) l = r; } return l; }
static const char USAGE[] = "usage: expr expression\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    av = argv + 1; ac = argc - 1; ai = 0;
    if (!ac) zusage(USAGE);
    char *v = val();
    dprintf(1, "%s\n", v);
    return (!*v || !strcmp(v, "0")) ? 1 : 0;
}
