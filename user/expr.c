#include "libz.h"
/* expr arg op arg ...: arithmetic (+ - \* / %), comparisons, & |, length STRING; prints the result, status 1 if it is 0 or empty */
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
static char *mul(void)
{
    char *l = prim();
    while (ai < ac && (!strcmp(av[ai], "*") || !strcmp(av[ai], "/") || !strcmp(av[ai], "%"))) {
        char op = av[ai++][0]; char *r = prim(); int ok1, ok2; long a = num(l, &ok1), b = num(r, &ok2);
        if (!ok1 || !ok2) { fprintf(2, "expr: non-numeric argument\n"); _zexit(2); }
        if (op != '*' && !b) { fprintf(2, "expr: division by zero\n"); _zexit(2); }
        l = fmt(op == '*' ? a * b : op == '/' ? a / b : a % b);
    }
    return l;
}
static char *add(void)
{
    char *l = mul();
    while (ai < ac && (!strcmp(av[ai], "+") || !strcmp(av[ai], "-"))) {
        char op = av[ai++][0]; char *r = mul(); int ok1, ok2; long a = num(l, &ok1), b = num(r, &ok2);
        if (!ok1 || !ok2) { fprintf(2, "expr: non-numeric argument\n"); _zexit(2); }
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
int main(int argc, char **argv)
{
    av = argv + 1; ac = argc - 1; ai = 0;
    if (!ac) { fprintf(2, "usage: expr expression\n"); return 2; }
    char *v = val();
    printf("%s\n", v);
    return (!*v || !strcmp(v, "0")) ? 1 : 0;
}
