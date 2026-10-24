// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
#include "regex.h"

/* awk [-F fs] [-v name=value]... [-f progfile | 'program'] [name=value | file]...
 * A small POSIX awk: pattern-action rules, BEGIN and END, fields and records, associative arrays, user functions, the usual
 * builtins (length substr index split sub gsub match sprintf tolower toupper int sqrt exp log sin cos atan2 rand srand system
 * close getline), printf, output to files and pipes. Numbers are doubles (soft-float, so arithmetic is slow on a 68020 without
 * an FPU). Not done: getline from a plain file into a var with NR bookkeeping subtleties, uninitialised-variable types in
 * every corner, locales, and the gawk extensions. */

typedef unsigned char u8;

/* ================================================================ memory
 * libz's xmalloc/free (size classes, blocks reused, big ones given back; out of memory ends awk with a message).
 * Temporary strings of one statement come from an arena that is emptied after the statement. */
#define xfree free

/* The stack is fixed (Makefile STACK_awk, 64 KB; no MMU, so nothing grows it). Parsing and running recurse (nested
 * expressions, blocks, user functions: about 1.5 KB per function call level), so they refuse to go deeper than STK_LIMIT
 * bytes below where main started, which leaves 12 KB for the deepest leaves (sprintf, regex, number formatting). */
#define STK_LIMIT (65536 - 12288)
static char *stk_top;
static void fatal(const char *fmt, ...);
static inline void stack_check(void)
{
    char *sp; __asm__ volatile("move.l %%sp,%0" : "=r"(sp));
    if (stk_top && (u32)(stk_top - sp) > STK_LIMIT) fatal("program nested too deeply (out of stack)");
}
#define TMP_CHUNK 32768
static char *tmp_chunks[64]; static int ntmp; static u32 tmp_used, tmp_cap;
static void tmp_reset(void) { while (ntmp > 1) xfree(tmp_chunks[--ntmp]); tmp_used = 0; if (ntmp) tmp_cap = TMP_CHUNK; }
static char *talloc(u32 n)
{
    n = (n + 3) & ~3u;
    if (!ntmp || tmp_used + n > tmp_cap) {
        u32 want = n + 8 > TMP_CHUNK ? n + 8 : TMP_CHUNK;
        if (ntmp >= 64) { fprintf(2, "awk: expression too large\n"); _zexit(2); }
        tmp_chunks[ntmp++] = xmalloc(want); tmp_cap = want; tmp_used = 0;
    }
    char *p = tmp_chunks[ntmp - 1] + tmp_used; tmp_used += n; return p;
}
static char *tstrn(const char *s, u32 n) { char *p = talloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }
static char *tstr(const char *s) { return tstrn(s, strlen(s)); }
static char *xstrdup(const char *s) { u32 n = strlen(s); char *p = xmalloc(n + 1); memcpy(p, s, n + 1); return p; }
static void fatal(const char *fmt, ...)
{
    char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    oflush(); fprintf(2, "awk: %s\n", b); _zexit(2);
}

/* ================================================================ numbers: parsing and printing without a math library */
static double pow10i(int e)
{
    double r = 1, b = 10;
    int neg = e < 0; if (neg) e = -e;
    while (e) { if (e & 1) r *= b; b *= b; e >>= 1; }
    return neg ? 1 / r : r;
}
/* parse the longest numeric prefix; *end gets the position after it (== s if there is none) */
static double str2num(const char *s, const char **end)
{
    const char *p = s; int neg = 0;
    while (*p == ' ' || *p == '\t' || *p == '\n') p++;
    const char *start = p;
    if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
    double v = 0; int nd = 0, exp10 = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; nd++; }
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); exp10--; p++; nd++; } }
    if (!nd) { if (end) *end = s; return 0; }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1; int eneg = 0, ev = 0, ed = 0;
        if (*q == '+' || *q == '-') { eneg = *q == '-'; q++; }
        while (*q >= '0' && *q <= '9') { ev = ev * 10 + (*q - '0'); q++; ed++; if (ev > 400) ev = 400; }
        if (ed) { exp10 += eneg ? -ev : ev; p = q; }
    }
    if (exp10) v = exp10 < 0 ? v / pow10i(-exp10) : v * pow10i(exp10);
    if (end) *end = p;
    (void)start;
    return neg ? -v : v;
}
static int looks_numeric(const char *s)               /* the whole string (spaces around allowed) is a number */
{
    const char *e; str2num(s, &e);
    if (e == s) return 0;
    while (*e == ' ' || *e == '\t' || *e == '\n') e++;
    return *e == 0;
}

/* Format a double like printf's %f %e %g (conv is f e g E G). The value is turned into 17 significant decimal digits once,
 * the digit string is rounded where the format needs it, and then written out. */
static void round_digits(char *D, int keep, int *E)
{
    if (keep >= 17) return;
    if (keep < 0) { memset(D, '0', 17); return; }
    int up = D[keep] >= '5';
    for (int i = keep; i < 17; i++) D[i] = '0';
    if (!up) return;
    int i = keep - 1;
    while (i >= 0 && D[i] == '9') { D[i] = '0'; i--; }
    if (i >= 0) D[i]++;
    else { D[0] = '1'; (*E)++; }                         /* carried out of the first digit (also keep == 0): 1 followed by zeros */
}
static int fmt_float(char *out, int max, double v, char conv, int prec, int alt)
{
    char *o = out; int upper = conv == 'E' || conv == 'G', lc = conv | 32;
    #define PUT(c) do { if (o - out < max - 1) *o++ = (c); } while (0)
    if (v != v) { PUT('n'); PUT('a'); PUT('n'); *o = 0; return (int)(o - out); }
    if (v < 0 || (v == 0 && 1 / v < 0)) { PUT('-'); v = -v; }
    if (v > 1.7e308) { PUT('i'); PUT('n'); PUT('f'); *o = 0; return (int)(o - out); }
    if (prec < 0) prec = 6;
    char D[20]; int E = 0;
    if (v == 0) memset(D, '0', 17);
    else {
        while (v >= 1e16) { v /= 1e16; E += 16; }
        while (v >= 10) { v /= 10; E++; }
        while (v < 1e-16) { v *= 1e16; E -= 16; }
        while (v < 1) { v *= 10; E--; }
        unsigned long long d = (unsigned long long)(v * 1e16 + 0.5);
        if (d >= 100000000000000000ULL) { d /= 10; E++; }
        for (int i = 16; i >= 0; i--) { D[i] = (char)('0' + (int)(d % 10)); d /= 10; }
    }
    char *start = o; int style = lc;                      /* 'f' or 'e' */
    if (lc == 'g') {
        int P = prec ? prec : 1;
        round_digits(D, P, &E);
        if (E < -4 || E >= P) { style = 'e'; prec = P - 1; } else { style = 'f'; prec = P - 1 - E; }
    }
    if (style == 'f') {
        round_digits(D, E + 1 + prec, &E);
        if (E >= 0) for (int i = 0; i <= E; i++) PUT(i < 17 ? D[i] : '0'); else PUT('0');
        if (prec > 0 || alt) PUT('.');
        for (int i = 0; i < prec; i++) { int j = E + 1 + i; PUT(j >= 0 && j < 17 ? D[j] : '0'); }
    } else {
        round_digits(D, prec + 1, &E);
        PUT(D[0]);
        if (prec > 0 || alt) PUT('.');
        for (int i = 1; i <= prec; i++) PUT(i < 17 ? D[i] : '0');
        if (lc == 'g' && !alt) {                          /* %g: strip trailing zeros of the mantissa */
            char *e = o; while (e > start && e[-1] == '0') e--; if (e > start && e[-1] == '.') e--; o = e;
        }
        PUT(upper ? 'E' : 'e'); PUT(E < 0 ? '-' : '+');
        int ae = E < 0 ? -E : E; if (ae >= 100) PUT((char)('0' + ae / 100)); PUT((char)('0' + (ae / 10) % 10)); PUT((char)('0' + ae % 10));
        *o = 0; return (int)(o - out);
    }
    if (lc == 'g' && !alt) {                              /* strip trailing zeros after the point */
        char *p = start; while (p < o && *p != '.') p++;
        if (p < o) { while (o > p && o[-1] == '0') o--; if (o > p && o[-1] == '.') o--; }
    }
    *o = 0;
    return (int)(o - out);
    #undef PUT
}

/* ================================================================ values, cells, arrays, symbols */
enum { F_NUM = 1, F_STR = 2, F_STRNUM = 4 };
struct val { u8 fl; double n; const char *s; };           /* a value; s points into a cell or into the temp arena */
struct arr;
struct cell { u8 fl; double n; char *s; struct arr *a; };  /* a variable: scalar (fl) or array (a) */
struct elem { char *key; struct cell c; struct elem *next; };
#define NBUCKET 128
struct arr { struct elem *b[NBUCKET]; int n; };

static struct arr *arr_new(void) { struct arr *a = xmalloc(sizeof *a); memset(a, 0, sizeof *a); return a; }
static u32 hash_str(const char *s) { u32 h = 5381; while (*s) h = h * 33 + (unsigned char)*s++; return h; }
static struct elem *arr_find(struct arr *a, const char *k, int create)
{
    u32 h = hash_str(k) % NBUCKET;
    for (struct elem *e = a->b[h]; e; e = e->next) if (!strcmp(e->key, k)) return e;
    if (!create) return 0;
    struct elem *e = xmalloc(sizeof *e); memset(e, 0, sizeof *e);
    e->key = xstrdup(k); e->next = a->b[h]; a->b[h] = e; a->n++;
    return e;
}
static void cell_free_str(struct cell *c) { if (c->s) { free(c->s); c->s = 0; } }
static void arr_clear(struct arr *a)
{
    for (int i = 0; i < NBUCKET; i++) { struct elem *e = a->b[i]; while (e) { struct elem *n = e->next; free(e->key); cell_free_str(&e->c); if (e->c.a) { arr_clear(e->c.a); free(e->c.a); } free(e); e = n; } a->b[i] = 0; }
    a->n = 0;
}
static void arr_delete(struct arr *a, const char *k)
{
    u32 h = hash_str(k) % NBUCKET;
    for (struct elem **p = &a->b[h]; *p; p = &(*p)->next)
        if (!strcmp((*p)->key, k)) { struct elem *e = *p; *p = e->next; free(e->key); cell_free_str(&e->c); free(e); a->n--; return; }
}

static struct val vnum(double n) { struct val v; v.fl = F_NUM; v.n = n; v.s = 0; return v; }
static struct val vstr(const char *s) { struct val v; v.fl = F_STR; v.n = 0; v.s = s; return v; }
static struct val vstrnum(const char *s) { struct val v; v.s = s; if (looks_numeric(s)) { v.fl = F_STR | F_STRNUM; v.n = str2num(s, 0); } else { v.fl = F_STR; v.n = 0; } return v; }

static const char *convfmt = "%.6g";
static const char *num2str(double n, const char *fmt)
{
    char b[64];
    if (n > -1e16 && n < 1e16 && n == (double)(long long)n) {         /* a whole number prints as an integer, as gawk */
        if (n > -2147483648.0 && n < 2147483648.0) snprintf(b, sizeof b, "%ld", (long)n);
        else {                                     /* beyond 32 bits: two exact halves of 8 digits (n < 1e16, all exact) */
            double m = n < 0 ? -n : n, hi = (double)(long)(m / 1e8);
            if (hi * 1e8 > m) hi -= 1;                                     /* m / 1e8 rounded up: step back */
            long lo = (long)(m - hi * 1e8);
            if (lo >= 100000000) { hi += 1; lo -= 100000000; }
            snprintf(b, sizeof b, "%s%ld%08ld", n < 0 ? "-" : "", (long)hi, lo);
        }
        return tstr(b);
    }
    /* a conversion format: %.6g and friends */
    const char *p = fmt; int prec = 6; char conv = 'g';
    if (*p == '%') { p++; if (*p == '.') { p++; prec = atoi(p); while (isdigit((unsigned char)*p)) p++; } if (*p) conv = *p; }
    fmt_float(b, sizeof b, n, conv, prec, 0);
    return tstr(b);
}
static const char *to_str(struct val v) { if (v.fl & F_STR) return v.s; return num2str(v.n, convfmt); }
static double to_num(struct val v)
{
    if (v.fl & (F_NUM | F_STRNUM)) return v.n;
    return v.s ? str2num(v.s, 0) : 0;
}
static int to_bool(struct val v)
{
    if (v.fl & F_STRNUM) return v.n != 0;
    if (v.fl & F_STR) return v.s && *v.s;
    return v.n != 0;
}
static struct val cell_val(const struct cell *c)
{
    struct val v;
    if (c->fl & F_STR) { v.fl = c->fl; v.n = c->n; v.s = c->s ? c->s : ""; }
    else if (c->fl & F_NUM) { v.fl = F_NUM; v.n = c->n; v.s = 0; }
    else { v.fl = F_STR | F_NUM | F_STRNUM; v.n = 0; v.s = ""; }          /* uninitialised: "" and 0 */
    return v;
}
static void cell_set(struct cell *c, struct val v)
{
    if (c->s && (v.fl & F_STR) && v.s == c->s) { c->fl = v.fl; c->n = v.n; return; }
    char *ns = (v.fl & F_STR) ? xstrdup(v.s ? v.s : "") : 0;
    cell_free_str(c);
    c->s = ns; c->fl = v.fl; c->n = (v.fl & F_NUM) || (v.fl & F_STRNUM) ? v.n : 0;
}

/* symbols: variables, arrays and functions by name */
struct node;
struct func { char *name; int nparams; char **pnames; u8 *pisarr; struct node *body; };
struct sym { char *name; struct cell c; struct func *fn; int special; struct sym *next; };
enum { SP_NONE, SP_NF, SP_NR, SP_FNR, SP_FS, SP_OFS, SP_ORS, SP_RS, SP_SUBSEP, SP_RSTART, SP_RLENGTH, SP_FILENAME, SP_CONVFMT, SP_OFMT, SP_ENVIRON, SP_ARGC, SP_ARGV };
#define NSYMB 256
static struct sym *symtab[NSYMB];
static struct sym *sym_find(const char *name, int create)
{
    u32 h = hash_str(name) % NSYMB;
    for (struct sym *s = symtab[h]; s; s = s->next) if (!strcmp(s->name, name)) return s;
    if (!create) return 0;
    struct sym *s = xmalloc(sizeof *s); memset(s, 0, sizeof *s);
    s->name = xstrdup(name); s->next = symtab[h]; symtab[h] = s;
    return s;
}
static struct sym *sNF, *sNR, *sFNR, *sFS, *sOFS, *sORS, *sRS, *sSUBSEP, *sRSTART, *sRLENGTH, *sFILENAME, *sCONVFMT, *sOFMT, *sENVIRON, *sARGC, *sARGV;
static struct sym *special(const char *name, int kind) { struct sym *s = sym_find(name, 1); s->special = kind; return s; }

/* ================================================================ lexer: the whole program becomes a token array */
enum {
    T_EOF, T_NL, T_NUMBER, T_STRING, T_ERE, T_NAME, T_FUNCNAME, T_BUILTIN,
    T_BEGIN, T_END, T_FUNCTION, T_IF, T_ELSE, T_WHILE, T_FOR, T_DO, T_BREAK, T_CONTINUE, T_NEXT, T_NEXTFILE, T_EXIT, T_RETURN,
    T_DELETE, T_IN, T_GETLINE, T_PRINT, T_PRINTF,
    T_LBRACE, T_RBRACE, T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_SEMI, T_COMMA,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_CARET, T_NOT, T_GT, T_LT, T_PIPE, T_QUESTION, T_COLON, T_TILDE, T_NOMATCH, T_DOLLAR, T_ASSIGN,
    T_ADDASSIGN, T_SUBASSIGN, T_MULASSIGN, T_DIVASSIGN, T_MODASSIGN, T_POWASSIGN, T_EQ, T_LE, T_GE, T_NE, T_INCR, T_DECR, T_AND, T_OR, T_APPEND
};
enum { B_LENGTH = 1, B_SUBSTR, B_INDEX, B_SPLIT, B_SUB, B_GSUB, B_MATCH, B_SPRINTF, B_SIN, B_COS, B_ATAN2, B_EXP, B_LOG, B_SQRT, B_INT, B_RAND, B_SRAND,
       B_TOLOWER, B_TOUPPER, B_SYSTEM, B_CLOSE, B_FFLUSH };
struct tok { u8 t; int b; double num; char *s; int line; };
static struct tok *toks; static int ntok, captok, tp;
static int src_line = 1;

static void tok_add(int t, int b, double num, char *s)
{
    if (ntok == captok) { captok = captok ? captok * 2 : 256; struct tok *nt = xmalloc((u32)captok * sizeof *nt); if (ntok) memcpy(nt, toks, (u32)ntok * sizeof *nt); toks = nt; }
    toks[ntok].t = (u8)t; toks[ntok].b = b; toks[ntok].num = num; toks[ntok].s = s; toks[ntok].line = src_line; ntok++;
}
static const struct { const char *w; int t, b; } keywords[] = {
    { "BEGIN", T_BEGIN, 0 }, { "END", T_END, 0 }, { "function", T_FUNCTION, 0 }, { "func", T_FUNCTION, 0 }, { "if", T_IF, 0 }, { "else", T_ELSE, 0 },
    { "while", T_WHILE, 0 }, { "for", T_FOR, 0 }, { "do", T_DO, 0 }, { "break", T_BREAK, 0 }, { "continue", T_CONTINUE, 0 }, { "next", T_NEXT, 0 },
    { "nextfile", T_NEXTFILE, 0 }, { "exit", T_EXIT, 0 }, { "return", T_RETURN, 0 }, { "delete", T_DELETE, 0 }, { "in", T_IN, 0 }, { "getline", T_GETLINE, 0 },
    { "print", T_PRINT, 0 }, { "printf", T_PRINTF, 0 },
    { "length", T_BUILTIN, B_LENGTH }, { "substr", T_BUILTIN, B_SUBSTR }, { "index", T_BUILTIN, B_INDEX }, { "split", T_BUILTIN, B_SPLIT },
    { "sub", T_BUILTIN, B_SUB }, { "gsub", T_BUILTIN, B_GSUB }, { "match", T_BUILTIN, B_MATCH }, { "sprintf", T_BUILTIN, B_SPRINTF },
    { "sin", T_BUILTIN, B_SIN }, { "cos", T_BUILTIN, B_COS }, { "atan2", T_BUILTIN, B_ATAN2 }, { "exp", T_BUILTIN, B_EXP }, { "log", T_BUILTIN, B_LOG },
    { "sqrt", T_BUILTIN, B_SQRT }, { "int", T_BUILTIN, B_INT }, { "rand", T_BUILTIN, B_RAND }, { "srand", T_BUILTIN, B_SRAND },
    { "tolower", T_BUILTIN, B_TOLOWER }, { "toupper", T_BUILTIN, B_TOUPPER }, { "system", T_BUILTIN, B_SYSTEM }, { "close", T_BUILTIN, B_CLOSE },
    { "fflush", T_BUILTIN, B_FFLUSH }, { 0, 0, 0 }
};
static int prev_allows_division(void)               /* is a / here a divide (after an operand) rather than the start of a regex */
{
    if (!ntok) return 0;
    int t = toks[ntok - 1].t;
    return t == T_NUMBER || t == T_STRING || t == T_NAME || t == T_RPAREN || t == T_RBRACKET || t == T_DOLLAR || t == T_INCR || t == T_DECR || t == T_BUILTIN;
}
static void lex(const char *p)
{
    while (*p) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\r') { p++; continue; }
        if (c == '\\' && p[1] == '\n') { p += 2; src_line++; continue; }
        if (c == '\\' && p[1] == '\r' && p[2] == '\n') { p += 3; src_line++; continue; }
        if (c == '\n') { tok_add(T_NL, 0, 0, 0); src_line++; p++; continue; }
        if (c == '#') { while (*p && *p != '\n') p++; continue; }
        if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)p[1]))) {
            const char *e; double v = str2num(p, &e); tok_add(T_NUMBER, 0, v, 0); p = e; continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            const char *q = p; while (isalnum((unsigned char)*q) || *q == '_') q++;
            char *w = xmalloc((u32)(q - p) + 1); memcpy(w, p, (u32)(q - p)); w[q - p] = 0;
            int found = 0;
            for (int i = 0; keywords[i].w; i++) if (!strcmp(w, keywords[i].w)) { tok_add(keywords[i].t, keywords[i].b, 0, w); found = 1; break; }
            if (!found) tok_add(*q == '(' ? T_FUNCNAME : T_NAME, 0, 0, w);
            p = q; continue;
        }
        if (c == '"') {
            char *buf = xmalloc(strlen(p) + 1); u32 n = 0; p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    p++;
                    switch (*p) {
                    case 'n': buf[n++] = '\n'; break; case 't': buf[n++] = '\t'; break; case 'r': buf[n++] = '\r'; break; case '\\': buf[n++] = '\\'; break;
                    case '"': buf[n++] = '"'; break; case '/': buf[n++] = '/'; break; case 'a': buf[n++] = 7; break; case 'b': buf[n++] = 8; break;
                    case 'f': buf[n++] = 12; break; case 'v': buf[n++] = 11; break; case '\n': src_line++; break;
                    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': { int v = 0, k = 0; while (k < 3 && *p >= '0' && *p <= '7') { v = v * 8 + (*p - '0'); p++; k++; } p--; buf[n++] = (char)v; break; }
                    default: buf[n++] = '\\'; buf[n++] = *p; break;     /* kept for the regex engine: "\\." arrives as \. */
                    }
                    p++;
                } else buf[n++] = *p++;
            }
            if (*p != '"') fatal("line %d: unterminated string", src_line);
            p++; buf[n] = 0; tok_add(T_STRING, 0, 0, buf); continue;
        }
        if (c == '/' && !prev_allows_division()) {
            char *buf = xmalloc(strlen(p) + 1); u32 n = 0; p++;
            int inbr = 0;
            while (*p && (*p != '/' || inbr) && *p != '\n') {
                if (*p == '\\' && p[1] == '/') { buf[n++] = '/'; p += 2; continue; }
                if (*p == '\\' && p[1]) { buf[n++] = *p++; buf[n++] = *p++; continue; }
                if (*p == '[' && !inbr) { inbr = 1; buf[n++] = *p++; if (*p == '^') buf[n++] = *p++; if (*p == ']') buf[n++] = *p++; continue; }
                if (*p == ']' && inbr) inbr = 0;
                buf[n++] = *p++;
            }
            if (*p != '/') fatal("line %d: unterminated regular expression", src_line);
            p++; buf[n] = 0; tok_add(T_ERE, 0, 0, buf); continue;
        }
        p++;
        #define OP2(ch, tt) if (c == ch[0] && *p == ch[1]) { p++; tok_add(tt, 0, 0, 0); continue; }
        OP2("&&", T_AND) OP2("||", T_OR) OP2("==", T_EQ) OP2("<=", T_LE) OP2(">=", T_GE) OP2("!=", T_NE) OP2("++", T_INCR) OP2("--", T_DECR)
        OP2("+=", T_ADDASSIGN) OP2("-=", T_SUBASSIGN) OP2("*=", T_MULASSIGN) OP2("/=", T_DIVASSIGN) OP2("%=", T_MODASSIGN) OP2("^=", T_POWASSIGN)
        OP2("!~", T_NOMATCH) OP2(">>", T_APPEND)
        if (c == '*' && *p == '*') { p++; if (*p == '=') { p++; tok_add(T_POWASSIGN, 0, 0, 0); } else tok_add(T_CARET, 0, 0, 0); continue; }
        switch (c) {
        case '{': tok_add(T_LBRACE, 0, 0, 0); break; case '}': tok_add(T_RBRACE, 0, 0, 0); break; case '(': tok_add(T_LPAREN, 0, 0, 0); break;
        case ')': tok_add(T_RPAREN, 0, 0, 0); break; case '[': tok_add(T_LBRACKET, 0, 0, 0); break; case ']': tok_add(T_RBRACKET, 0, 0, 0); break;
        case ';': tok_add(T_SEMI, 0, 0, 0); break; case ',': tok_add(T_COMMA, 0, 0, 0); break; case '+': tok_add(T_PLUS, 0, 0, 0); break;
        case '-': tok_add(T_MINUS, 0, 0, 0); break; case '*': tok_add(T_STAR, 0, 0, 0); break; case '/': tok_add(T_SLASH, 0, 0, 0); break;
        case '%': tok_add(T_PERCENT, 0, 0, 0); break; case '^': tok_add(T_CARET, 0, 0, 0); break; case '!': tok_add(T_NOT, 0, 0, 0); break;
        case '>': tok_add(T_GT, 0, 0, 0); break; case '<': tok_add(T_LT, 0, 0, 0); break; case '|': tok_add(T_PIPE, 0, 0, 0); break;
        case '?': tok_add(T_QUESTION, 0, 0, 0); break; case ':': tok_add(T_COLON, 0, 0, 0); break; case '~': tok_add(T_TILDE, 0, 0, 0); break;
        case '$': tok_add(T_DOLLAR, 0, 0, 0); break; case '=': tok_add(T_ASSIGN, 0, 0, 0); break;
        default: fatal("line %d: unexpected character '%c'", src_line, c);
        }
    }
    tok_add(T_NL, 0, 0, 0); tok_add(T_EOF, 0, 0, 0);
}

/* ================================================================ syntax tree and parser */
enum {
    N_NUM, N_STR, N_ERE, N_VAR, N_LOCAL, N_FIELD, N_INDEX, N_ASSIGN, N_COND, N_OR, N_AND, N_IN, N_MATCH, N_NOMATCH, N_CMP, N_CONCAT, N_BINOP,
    N_NEG, N_PLUS, N_NOT, N_PREINC, N_POSTINC, N_CALL, N_BUILTIN, N_GETLINE, N_GROUP,
    S_BLOCK, S_EXPR, S_PRINT, S_PRINTF, S_IF, S_WHILE, S_DO, S_FOR, S_FORIN, S_BREAK, S_CONTINUE, S_NEXT, S_NEXTFILE, S_EXIT, S_RETURN, S_DELETE
};
struct node {
    u8 t; int op; int idx; int n; double num; char *s; struct sym *sym; void *re;
    struct node *a, *b, *c, *d; struct node **list; struct func *fn;
};
static struct node *newn(int t) { struct node *n = xmalloc(sizeof *n); memset(n, 0, sizeof *n); n->t = (u8)t; return n; }
static struct node *mk1(int t, struct node *a) { struct node *n = newn(t); n->a = a; return n; }
static struct node *mk2(int t, struct node *a, struct node *b) { struct node *n = newn(t); n->a = a; n->b = b; return n; }

static int tk(void) { return toks[tp].t; }
static int tk_at(int i) { return toks[tp + i].t; }
static void syntax(const char *m) { fatal("syntax error at source line %d: %s", toks[tp].line, m); }
static void eat(int t, const char *what) { if (tk() != t) syntax(what); tp++; }
static void opt_nl(void) { while (tk() == T_NL) tp++; }
static void skip_terms(void) { while (tk() == T_NL || tk() == T_SEMI) tp++; }

static struct func *cur_fn;                           /* the function being parsed: its parameters are local */
static int no_in, in_print;                           /* parsing the init part of a for: 'in' is not an operator; in a print: '>' redirects */
static struct node *expr(void);
static struct node *stmt(void);
static struct node *ternary(void);
static struct node *concat_expr(void);
static struct node *unary(void);
static struct node *postfix(void);

static int is_lvalue(struct node *n) { return n->t == N_VAR || n->t == N_LOCAL || n->t == N_FIELD || n->t == N_INDEX; }
static int local_index(const char *name)
{
    if (!cur_fn) return -1;
    for (int i = 0; i < cur_fn->nparams; i++) if (!strcmp(cur_fn->pnames[i], name)) return i;
    return -1;
}
static struct node *name_node(const char *name)
{
    int li = local_index(name);
    if (li >= 0) { struct node *n = newn(N_LOCAL); n->idx = li; n->s = (char *)name; return n; }
    struct node *n = newn(N_VAR); n->sym = sym_find(name, 1); return n;
}
static void mark_array_param(struct node *n) { if (n->t == N_LOCAL && cur_fn) cur_fn->pisarr[n->idx] = 1; }

static struct node **parse_list(int *count, int closer)         /* comma separated expressions up to (not including) closer */
{
    struct node **v = xmalloc(16 * sizeof *v); int n = 0, cap = 16;
    if (tk() != closer) {
        for (;;) {
            opt_nl();
            if (n == cap) { cap *= 2; struct node **nv = xmalloc((u32)cap * sizeof *nv); memcpy(nv, v, (u32)n * sizeof *nv); v = nv; }
            v[n++] = expr();
            opt_nl();
            if (tk() == T_COMMA) { tp++; continue; }
            break;
        }
    }
    *count = n; return v;
}

static struct node *primary(void)
{
    stack_check();
    int t = tk();
    struct node *n;
    switch (t) {
    case T_NUMBER: n = newn(N_NUM); n->num = toks[tp].num; tp++; return n;
    case T_STRING: n = newn(N_STR); n->s = toks[tp].s; tp++; return n;
    case T_ERE: n = newn(N_ERE); n->s = toks[tp].s; tp++; return n;
    case T_DOLLAR: {
        tp++;
        struct node *idx;
        if (tk() == T_INCR || tk() == T_DECR) { int op = tk() == T_INCR ? 1 : -1; tp++; struct node *lv = primary(); if (!is_lvalue(lv)) syntax("++/-- needs a variable"); idx = mk1(N_PREINC, lv); idx->op = op; }
        else if (tk() == T_MINUS) { tp++; idx = mk1(N_NEG, primary()); }
        else idx = primary();
        return mk1(N_FIELD, idx);
    }
    case T_INCR: case T_DECR: { int op = t == T_INCR ? 1 : -1; tp++; struct node *lv = primary(); if (!is_lvalue(lv)) syntax("++/-- needs a variable"); n = mk1(N_PREINC, lv); n->op = op; return n; }
    case T_LPAREN: {
        tp++;
        int sv_in = no_in, sv_pr = in_print; no_in = 0; in_print = 0;
        opt_nl();
        struct node *e = expr();
        opt_nl();
        if (tk() == T_COMMA) {                                  /* (i, j) in array */
            struct node **subs = xmalloc(8 * sizeof *subs); int ns = 0; subs[ns++] = e;
            while (tk() == T_COMMA) { tp++; opt_nl(); if (ns < 8) subs[ns++] = expr(); opt_nl(); }
            eat(T_RPAREN, "expected )");
            if (tk() != T_IN) syntax("expected 'in' after (subscript list)");
            tp++;
            if (tk() != T_NAME) syntax("expected an array name");
            struct node *an = name_node(toks[tp].s); tp++; mark_array_param(an);
            n = newn(N_IN); n->list = subs; n->n = ns; n->a = an; no_in = sv_in; in_print = sv_pr; return n;
        }
        eat(T_RPAREN, "expected )");
        no_in = sv_in; in_print = sv_pr;
        return mk1(N_GROUP, e);
    }
    case T_NAME: {
        const char *name = toks[tp].s; tp++;
        struct node *base = name_node(name);
        if (tk() == T_LBRACKET) {
            tp++; n = newn(N_INDEX); n->a = base; mark_array_param(base);
            int sv_in = no_in, sv_pr = in_print; no_in = 0; in_print = 0;
            n->list = parse_list(&n->n, T_RBRACKET);
            no_in = sv_in; in_print = sv_pr;
            eat(T_RBRACKET, "expected ]");
            if (n->n == 0) syntax("empty subscript");
            return n;
        }
        return base;
    }
    case T_FUNCNAME: {
        n = newn(N_CALL); n->s = toks[tp].s; tp++;
        eat(T_LPAREN, "expected (");
        int sv_in = no_in, sv_pr = in_print; no_in = 0; in_print = 0;
        n->list = parse_list(&n->n, T_RPAREN);
        no_in = sv_in; in_print = sv_pr;
        eat(T_RPAREN, "expected )");
        return n;
    }
    case T_BUILTIN: {
        n = newn(N_BUILTIN); n->op = toks[tp].b; tp++;
        if (tk() == T_LPAREN) {
            tp++;
            int sv_in = no_in, sv_pr = in_print; no_in = 0; in_print = 0;
            n->list = parse_list(&n->n, T_RPAREN);
            no_in = sv_in; in_print = sv_pr;
            eat(T_RPAREN, "expected )");
            if ((n->op == B_SPLIT || n->op == B_MATCH) && n->n >= 2 && n->op == B_SPLIT) mark_array_param(n->list[1]);
        } else if (n->op != B_LENGTH) syntax("a builtin function needs (arguments)");
        return n;
    }
    case T_GETLINE: {
        tp++; n = newn(N_GETLINE); n->op = 0;
        if (tk() == T_NAME || tk() == T_DOLLAR) { n->a = postfix(); if (!is_lvalue(n->a)) syntax("getline needs a variable"); }
        if (tk() == T_LT) { tp++; n->op = 1; n->b = postfix(); }
        return n;
    }
    case T_MINUS: tp++; return mk1(N_NEG, unary());
    case T_PLUS: tp++; return mk1(N_PLUS, unary());
    case T_NOT: tp++; return mk1(N_NOT, unary());
    default: syntax("unexpected token");
    }
    return 0;
}
static struct node *postfix(void)
{
    struct node *p = primary();
    if (is_lvalue(p) && (tk() == T_INCR || tk() == T_DECR)) { struct node *n = mk1(N_POSTINC, p); n->op = tk() == T_INCR ? 1 : -1; tp++; return n; }
    return p;
}
static struct node *power(void)
{
    struct node *l = postfix();
    if (tk() == T_CARET) {
        tp++;
        struct node *r = (tk() == T_MINUS || tk() == T_PLUS || tk() == T_NOT) ? unary() : power();
        struct node *n = mk2(N_BINOP, l, r); n->op = '^'; return n;
    }
    return l;
}
static struct node *unary(void)
{
    if (tk() == T_NOT) { tp++; return mk1(N_NOT, unary()); }
    if (tk() == T_MINUS) { tp++; return mk1(N_NEG, unary()); }
    if (tk() == T_PLUS) { tp++; return mk1(N_PLUS, unary()); }
    return power();
}
static struct node *mul(void)
{
    struct node *l = unary();
    while (tk() == T_STAR || tk() == T_SLASH || tk() == T_PERCENT) {
        int op = tk() == T_STAR ? '*' : tk() == T_SLASH ? '/' : '%'; tp++;
        struct node *r = unary(); l = mk2(N_BINOP, l, r); l->op = op;
    }
    return l;
}
static struct node *additive(void)
{
    struct node *l = mul();
    while (tk() == T_PLUS || tk() == T_MINUS) {
        int op = tk() == T_PLUS ? '+' : '-'; tp++;
        struct node *r = mul(); l = mk2(N_BINOP, l, r); l->op = op;
    }
    return l;
}
static int starts_operand(void)
{
    switch (tk()) {
    case T_NUMBER: case T_STRING: case T_ERE: case T_NAME: case T_FUNCNAME: case T_BUILTIN: case T_DOLLAR: case T_NOT: case T_LPAREN: case T_INCR: case T_DECR: return 1;
    default: return 0;
    }
}
static struct node *concat_expr(void)
{
    struct node *l = additive();
    while (starts_operand() && !(tk() == T_NOT && (tk_at(1) == T_ASSIGN))) {
        struct node *r = additive(); l = mk2(N_CONCAT, l, r);
    }
    return l;
}
static struct node *cmp_expr(void)
{
    struct node *l = concat_expr();
    for (;;) {
        if (tk() == T_PIPE && tk_at(1) == T_GETLINE) {              /* "cmd" | getline [var] */
            tp += 2; struct node *g = newn(N_GETLINE); g->op = 2; g->b = l;
            if (tk() == T_NAME || tk() == T_DOLLAR) { g->a = postfix(); if (!is_lvalue(g->a)) syntax("getline needs a variable"); }
            l = g; continue;
        }
        int t = tk();
        if (t == T_LT || t == T_LE || t == T_EQ || t == T_NE || t == T_GE || (t == T_GT && !in_print)) {
            tp++;
            struct node *r = concat_expr();
            l = mk2(N_CMP, l, r); l->op = t; continue;
        }
        break;
    }
    return l;
}
static struct node *match_expr(void)
{
    struct node *l = cmp_expr();
    while (tk() == T_TILDE || tk() == T_NOMATCH) {
        int neg = tk() == T_NOMATCH; tp++;
        struct node *r = cmp_expr(); l = mk2(neg ? N_NOMATCH : N_MATCH, l, r);
    }
    return l;
}
static struct node *in_expr(void)
{
    struct node *l = match_expr();
    while (!no_in && tk() == T_IN) {
        tp++;
        if (tk() != T_NAME) syntax("expected an array name after 'in'");
        struct node *an = name_node(toks[tp].s); tp++; mark_array_param(an);
        struct node *n = newn(N_IN); n->list = xmalloc(sizeof(struct node *)); n->list[0] = l; n->n = 1; n->a = an; l = n;
    }
    return l;
}
static struct node *and_expr(void)
{
    struct node *l = in_expr();
    while (tk() == T_AND) { tp++; opt_nl(); l = mk2(N_AND, l, in_expr()); }
    return l;
}
static struct node *or_expr(void)
{
    struct node *l = and_expr();
    while (tk() == T_OR) { tp++; opt_nl(); l = mk2(N_OR, l, and_expr()); }
    return l;
}
static struct node *ternary(void)
{
    struct node *c = or_expr();
    if (tk() == T_QUESTION) {
        tp++; opt_nl();
        struct node *a = expr(); opt_nl();
        eat(T_COLON, "expected : in ?:"); opt_nl();
        struct node *b = expr();
        struct node *n = newn(N_COND); n->a = c; n->b = a; n->c = b; return n;
    }
    return c;
}
static struct node *expr(void)
{
    struct node *l = ternary();
    int t = tk();
    if ((t == T_ASSIGN || t == T_ADDASSIGN || t == T_SUBASSIGN || t == T_MULASSIGN || t == T_DIVASSIGN || t == T_MODASSIGN || t == T_POWASSIGN) && is_lvalue(l)) {
        tp++; opt_nl();
        struct node *r = expr();
        struct node *n = mk2(N_ASSIGN, l, r);
        n->op = t == T_ASSIGN ? 0 : t == T_ADDASSIGN ? '+' : t == T_SUBASSIGN ? '-' : t == T_MULASSIGN ? '*' : t == T_DIVASSIGN ? '/' : t == T_MODASSIGN ? '%' : '^';
        return n;
    }
    return l;
}

/* ---- statements ---- */
static int at_stmt_end(void) { int t = tk(); return t == T_SEMI || t == T_NL || t == T_RBRACE || t == T_EOF; }
static void end_simple(void)
{
    if (tk() == T_SEMI || tk() == T_NL) { skip_terms(); return; }
    if (tk() == T_RBRACE || tk() == T_EOF) return;
    syntax("expected ; or newline");
}
static struct node *block(void)
{
    eat(T_LBRACE, "expected {");
    struct node *b = newn(S_BLOCK); struct node **v = xmalloc(16 * sizeof *v); int n = 0, cap = 16;
    skip_terms();
    while (tk() != T_RBRACE) {
        if (tk() == T_EOF) syntax("missing }");
        if (n == cap) { cap *= 2; struct node **nv = xmalloc((u32)cap * sizeof *nv); memcpy(nv, v, (u32)n * sizeof *nv); v = nv; }
        v[n++] = stmt();
        skip_terms();
    }
    tp++;
    b->list = v; b->n = n; return b;
}
static int redirect_next(void) { int t = tk(); return t == T_GT || t == T_APPEND || t == T_PIPE; }
static struct node *simple_stmt(void)
{
    int t = tk();
    if (t == T_PRINT || t == T_PRINTF) {
        tp++;
        struct node *n = newn(t == T_PRINT ? S_PRINT : S_PRINTF);
        int sv = in_print; in_print = 1;
        if (!at_stmt_end() && !redirect_next()) {
            int done = 0;
            if (tk() == T_LPAREN) {                              /* print (a, b) > "file": a parenthesised list */
                int depth = 0, i = 0;
                for (;; i++) { int tt = tk_at(i); if (tt == T_LPAREN) depth++; else if (tt == T_RPAREN && --depth == 0) break; else if (tt == T_EOF) break; }
                int after = tk_at(i + 1);
                if (tk_at(i) == T_RPAREN && (after == T_NL || after == T_SEMI || after == T_RBRACE || after == T_GT || after == T_APPEND || after == T_PIPE || after == T_EOF)) {
                    tp++;
                    int sv2 = in_print; in_print = 0;
                    n->list = parse_list(&n->n, T_RPAREN);
                    in_print = sv2;
                    eat(T_RPAREN, "expected )"); done = 1;
                }
            }
            if (!done) {
                struct node **v = xmalloc(16 * sizeof *v); int c = 0, cap = 16;
                for (;;) {
                    if (c == cap) { cap *= 2; struct node **nv = xmalloc((u32)cap * sizeof *nv); memcpy(nv, v, (u32)c * sizeof *nv); v = nv; }
                    v[c++] = expr();
                    if (tk() == T_COMMA) { tp++; opt_nl(); continue; }
                    break;
                }
                n->list = v; n->n = c;
            }
        }
        if (redirect_next()) { n->op = tk(); tp++; n->a = concat_expr(); }
        in_print = sv;
        return n;
    }
    if (t == T_DELETE) {
        tp++; if (tk() != T_NAME) syntax("expected an array name after delete");
        struct node *an = name_node(toks[tp].s); tp++; mark_array_param(an);
        struct node *n = newn(S_DELETE); n->a = an;
        if (tk() == T_LBRACKET) { tp++; n->list = parse_list(&n->n, T_RBRACKET); eat(T_RBRACKET, "expected ]"); }
        return n;
    }
    struct node *n = newn(S_EXPR); n->a = expr(); return n;
}
static struct node *stmt(void)
{
    stack_check();
    int t = tk();
    switch (t) {
    case T_LBRACE: return block();
    case T_SEMI: tp++; return newn(S_BLOCK);
    case T_IF: {
        tp++; eat(T_LPAREN, "expected ( after if");
        struct node *n = newn(S_IF); n->a = expr(); opt_nl(); eat(T_RPAREN, "expected ) after the condition"); opt_nl();
        n->b = stmt();
        int i = 0; while (tk_at(i) == T_NL || tk_at(i) == T_SEMI) i++;
        if (tk_at(i) == T_ELSE) { tp += i + 1; opt_nl(); n->c = stmt(); }
        return n;
    }
    case T_WHILE: {
        tp++; eat(T_LPAREN, "expected ( after while");
        struct node *n = newn(S_WHILE); n->a = expr(); opt_nl(); eat(T_RPAREN, "expected )"); 
        if (tk() == T_SEMI) { tp++; n->b = newn(S_BLOCK); return n; }
        opt_nl(); n->b = stmt(); return n;
    }
    case T_DO: {
        tp++; opt_nl(); struct node *n = newn(S_DO); n->b = stmt(); skip_terms();
        if (tk() != T_WHILE) syntax("expected while after do body");
        tp++; eat(T_LPAREN, "expected ("); n->a = expr(); eat(T_RPAREN, "expected )"); end_simple(); return n;
    }
    case T_FOR: {
        tp++; eat(T_LPAREN, "expected ( after for");
        if (tk() == T_NAME && tk_at(1) == T_IN && tk_at(2) == T_NAME && tk_at(3) == T_RPAREN) {
            struct node *n = newn(S_FORIN); n->a = name_node(toks[tp].s); tp += 2;
            n->b = name_node(toks[tp].s); mark_array_param(n->b); tp += 2; opt_nl(); n->c = stmt(); return n;
        }
        struct node *n = newn(S_FOR);
        int sv = no_in; no_in = 0;
        if (tk() != T_SEMI) { n->a = simple_stmt(); }
        eat(T_SEMI, "expected ; in for"); opt_nl();
        if (tk() != T_SEMI) n->b = expr();
        eat(T_SEMI, "expected ; in for"); opt_nl();
        if (tk() != T_RPAREN) n->c = simple_stmt();
        eat(T_RPAREN, "expected ) after for"); no_in = sv;
        if (tk() == T_SEMI) { tp++; n->d = newn(S_BLOCK); return n; }
        opt_nl(); n->d = stmt(); return n;
    }
    case T_BREAK: tp++; end_simple(); return newn(S_BREAK);
    case T_CONTINUE: tp++; end_simple(); return newn(S_CONTINUE);
    case T_NEXT: tp++; end_simple(); return newn(S_NEXT);
    case T_NEXTFILE: tp++; end_simple(); return newn(S_NEXTFILE);
    case T_EXIT: { tp++; struct node *n = newn(S_EXIT); if (!at_stmt_end()) n->a = expr(); end_simple(); return n; }
    case T_RETURN: { tp++; struct node *n = newn(S_RETURN); if (!at_stmt_end()) n->a = expr(); end_simple(); return n; }
    default: { struct node *n = simple_stmt(); end_simple(); return n; }
    }
}

/* ---- program ---- */
struct rule { struct node *pat, *pat2; struct node *action; int in_range; struct rule *next; };
static struct rule *begin_rules, *end_rules, *main_rules, **main_tail = &main_rules, **begin_tail = &begin_rules, **end_tail = &end_rules;
static void parse_program(void)
{
    skip_terms();
    while (tk() != T_EOF) {
        if (tk() == T_FUNCTION) {
            tp++;
            if (tk() != T_NAME && tk() != T_FUNCNAME) syntax("expected a function name");
            struct func *f = xmalloc(sizeof *f); memset(f, 0, sizeof *f);
            f->name = toks[tp].s; tp++;
            struct sym *s = sym_find(f->name, 1); s->fn = f;
            eat(T_LPAREN, "expected (");
            char *names[32]; int np = 0;
            while (tk() == T_NAME) { if (np < 32) names[np++] = toks[tp].s; tp++; opt_nl(); if (tk() == T_COMMA) { tp++; opt_nl(); } }
            eat(T_RPAREN, "expected )");
            f->nparams = np; f->pnames = xmalloc((u32)(np + 1) * sizeof(char *)); memcpy(f->pnames, names, (u32)np * sizeof(char *));
            f->pisarr = xmalloc((u32)np + 1); memset(f->pisarr, 0, (u32)np + 1);
            opt_nl();
            cur_fn = f; f->body = block(); cur_fn = 0;
        } else {
            struct rule *r = xmalloc(sizeof *r); memset(r, 0, sizeof *r);
            int kind = 0;
            if (tk() == T_BEGIN) { tp++; kind = 1; }
            else if (tk() == T_END) { tp++; kind = 2; }
            else if (tk() != T_LBRACE) {
                r->pat = expr();
                if (tk() == T_COMMA) { tp++; opt_nl(); r->pat2 = expr(); }
            }
            if (tk() == T_LBRACE) r->action = block();
            else if (kind) syntax("BEGIN and END need an { action }");
            if (kind == 1) { *begin_tail = r; begin_tail = &r->next; }
            else if (kind == 2) { *end_tail = r; end_tail = &r->next; }
            else { *main_tail = r; main_tail = &r->next; }
        }
        skip_terms();
    }
}

/* ================================================================ the record, the fields and the special variables */
static char *rec;                                         /* $0 */
static int rec_stale;                                     /* $0 must be rebuilt from the fields */
static struct cell *flds; static int nf, fcap, fields_valid;
static double nr, fnr;
static int exit_code;
static struct cell *frame;                                /* the locals of the running function */
static int call_depth;
static double rand_seed = 1, rand_prev = 1;

static const char *strvar(struct sym *s) { return (s->c.fl & F_STR) && s->c.s ? s->c.s : ""; }
static void set_strvar(struct sym *s, const char *v) { struct val x = vstr(v); cell_set(&s->c, x); }

static void fields_clear(void) { for (int i = 1; i <= nf; i++) cell_free_str(&flds[i]); nf = 0; }
static void fields_grow(int n)
{
    if (n < fcap) return;
    int nc = fcap ? fcap : 32; while (nc <= n) nc *= 2;
    struct cell *nflds = xmalloc((u32)nc * sizeof *nflds); memset(nflds, 0, (u32)nc * sizeof *nflds);
    if (fcap) memcpy(nflds, flds, (u32)fcap * sizeof *nflds);
    flds = nflds; fcap = nc;
}
static void add_field(const char *s, u32 len)
{
    fields_grow(nf + 1); nf++;
    struct cell *c = &flds[nf]; cell_free_str(c);
    char *t = xmalloc(len + 1); memcpy(t, s, len); t[len] = 0;
    c->s = t; c->fl = looks_numeric(t) ? (F_STR | F_STRNUM) : F_STR; c->n = (c->fl & F_STRNUM) ? str2num(t, 0) : 0;
}

/* regular expressions: compiled once per distinct text */
struct recache { char *pat; void *re; };
static struct recache recache[32]; static int re_next;
static void *get_re(const char *pat)
{
    for (int i = 0; i < 32; i++) if (recache[i].pat && !strcmp(recache[i].pat, pat)) return recache[i].re;
    void *re = re_comp(pat, RE_LONGEST);
    if (!re) fatal("bad regular expression: %s", pat);
    int i = re_next++ & 31;
    if (recache[i].pat) { free(recache[i].pat); re_free(recache[i].re); }
    recache[i].pat = xstrdup(pat); recache[i].re = re;
    return re;
}

static void split_fields(const char *str, int len, const char *fs)
{
    fields_clear();
    if (!strcmp(fs, " ")) {
        int i = 0;
        while (i < len) {
            while (i < len && (str[i] == ' ' || str[i] == '\t' || str[i] == '\n')) i++;
            if (i >= len) break;
            int j = i; while (j < len && str[j] != ' ' && str[j] != '\t' && str[j] != '\n') j++;
            add_field(str + i, (u32)(j - i)); i = j;
        }
    } else if (!len) {
    } else if (fs[0] && !fs[1] && fs[0] != '\\') {
        int i = 0; char sep = fs[0];
        for (;;) { int j = i; while (j < len && str[j] != sep) j++; add_field(str + i, (u32)(j - i)); if (j >= len) break; i = j + 1; }
    } else if (!fs[0]) {
        for (int i = 0; i < len; i++) add_field(str + i, 1);
    } else {
        void *re = get_re(fs); int from = 0, caps[2 * RE_GROUPS], start = 0;
        while (from <= len && re_exec(re, str, len, from, caps)) {
            if (caps[1] == caps[0]) { from = caps[0] + 1; continue; }        /* an empty match separates nothing */
            add_field(str + start, (u32)(caps[0] - start)); start = caps[1]; from = caps[1];
        }
        add_field(str + start, (u32)(len - start));
    }
}
static void ensure_fields(void)
{
    if (fields_valid) return;
    fields_valid = 1;
    const char *fs = strvar(sFS);
    if (!strcmp(strvar(sRS), "") && strcmp(fs, " ")) {          /* paragraph mode: a newline also separates fields */
        char *copy = xstrdup(rec ? rec : ""); u32 n = strlen(copy);
        for (u32 i = 0; i < n; i++) if (copy[i] == '\n') copy[i] = fs[0] && !fs[1] ? fs[0] : '\n';
        split_fields(copy, (int)n, fs); free(copy);
    } else split_fields(rec ? rec : "", rec ? (int)strlen(rec) : 0, fs);
}
static void set_record(const char *s)
{
    if (rec) free(rec);
    rec = xstrdup(s); rec_stale = 0; fields_valid = 0;
}
static void rebuild_record(void)
{
    const char *ofs = strvar(sOFS); u32 ol = strlen(ofs), total = 1;
    for (int i = 1; i <= nf; i++) total += (u32)(flds[i].s ? strlen(flds[i].s) : 0) + ol;
    char *r = xmalloc(total + 1); u32 o = 0;
    for (int i = 1; i <= nf; i++) {
        if (i > 1) { memcpy(r + o, ofs, ol); o += ol; }
        struct val v = cell_val(&flds[i]); const char *s = to_str(v); u32 l = strlen(s);
        if (o + l + ol + 2 > total) { total = o + l + ol + 64; char *nr2 = xmalloc(total + 1); memcpy(nr2, r, o); free(r); r = nr2; }
        memcpy(r + o, s, l); o += l;
    }
    r[o] = 0; if (rec) free(rec); rec = r; rec_stale = 0;
}
static struct val get_field(int i)
{
    if (i < 0) fatal("trying to access $%d", i);
    if (i == 0) { if (rec_stale) rebuild_record(); return vstrnum(rec ? rec : ""); }
    ensure_fields();
    if (i > nf) { struct val v = vstr(""); v.fl |= F_STRNUM; return v; }
    return cell_val(&flds[i]);
}
static void set_field(int i, struct val v)
{
    if (i < 0) fatal("trying to access $%d", i);
    if (i == 0) { set_record(to_str(v)); return; }
    ensure_fields();
    if (i > nf) { fields_grow(i); for (int k = nf + 1; k <= i; k++) { cell_free_str(&flds[k]); flds[k].fl = F_STR; flds[k].s = xstrdup(""); } nf = i; }
    cell_set(&flds[i], v); rec_stale = 1;
}
static void set_nf(int n)
{
    ensure_fields();
    if (n < 0) n = 0;
    if (n > nf) { fields_grow(n); for (int k = nf + 1; k <= n; k++) { cell_free_str(&flds[k]); flds[k].fl = F_STR; flds[k].s = xstrdup(""); } }
    else for (int k = n + 1; k <= nf; k++) cell_free_str(&flds[k]);
    nf = n; rec_stale = 1;
}

static struct val sym_get(struct sym *s)
{
    switch (s->special) {
    case SP_NF: ensure_fields(); return vnum(nf);
    case SP_NR: return vnum(nr);
    case SP_FNR: return vnum(fnr);
    default: return cell_val(&s->c);
    }
}
static void sym_set(struct sym *s, struct val v)
{
    switch (s->special) {
    case SP_NF: set_nf((int)to_num(v)); return;
    case SP_NR: nr = to_num(v); return;
    case SP_FNR: fnr = to_num(v); return;
    case SP_CONVFMT: cell_set(&s->c, v); convfmt = strvar(s); return;
    default: cell_set(&s->c, v);
    }
}

/* ================================================================ arrays and lvalues */
static struct arr *get_arr(struct node *base)
{
    if (base->t == N_VAR) { struct cell *c = &base->sym->c; if (!c->a) c->a = arr_new(); return c->a; }
    if (base->t == N_LOCAL) { struct cell *c = &frame[base->idx]; if (!c->a) { c->a = arr_new(); c->fl |= 8; } return c->a; }   /* 8: this frame owns the array */
    fatal("not an array");
    return 0;
}
static struct val eval(struct node *n);
static const char *join_subs(struct node **list, int n)
{
    if (n == 1) return to_str(eval(list[0]));
    const char *sep = strvar(sSUBSEP); u32 sl = strlen(sep);
    char *parts[8]; u32 total = 1; if (n > 8) n = 8;
    for (int i = 0; i < n; i++) { parts[i] = (char *)to_str(eval(list[i])); total += strlen(parts[i]) + sl; }
    char *r = talloc(total); u32 o = 0;
    for (int i = 0; i < n; i++) { if (i) { memcpy(r + o, sep, sl); o += sl; } u32 l = strlen(parts[i]); memcpy(r + o, parts[i], l); o += l; }
    r[o] = 0; return r;
}
static struct cell *elem_cell(struct node *n)                       /* the cell of an array element, created on first use */
{
    struct arr *a = get_arr(n->a);
    const char *key = join_subs(n->list, n->n);
    return &arr_find(a, key, 1)->c;
}
static struct val lv_get(struct node *n)
{
    switch (n->t) {
    case N_VAR: return sym_get(n->sym);
    case N_LOCAL: return cell_val(&frame[n->idx]);
    case N_FIELD: return get_field((int)to_num(eval(n->a)));
    case N_INDEX: return cell_val(elem_cell(n));
    default: fatal("not a variable"); return vnum(0);
    }
}
static void lv_set(struct node *n, struct val v)
{
    switch (n->t) {
    case N_VAR: sym_set(n->sym, v); return;
    case N_LOCAL: cell_set(&frame[n->idx], v); return;
    case N_FIELD: set_field((int)to_num(eval(n->a)), v); return;
    case N_INDEX: cell_set(elem_cell(n), v); return;
    default: fatal("cannot assign");
    }
}

/* ================================================================ output and input streams */
struct ostream { char *name; int kind; int fd; int pid; char *buf; u32 len; struct ostream *next; };
static struct ostream *ostreams;
struct istream { char *name; int kind; int fd; int pid; struct rd r; struct istream *next; };   /* kind 1 file, 2 command */
static struct istream *istreams;

static void ostream_flush(struct ostream *o) { if (o->len) { sys_write(o->fd, o->buf, o->len); o->len = 0; } }
static void ostream_write(struct ostream *o, const char *s, u32 n)
{
    if (o->len + n > 4096) { ostream_flush(o); if (n > 4096) { sys_write(o->fd, s, n); return; } }
    memcpy(o->buf + o->len, s, n); o->len += n;
}
static int spawn_sh(const char *cmd, int in_fd, int out_fd)
{
    char *argv[4] = { "sh", "-c", (char *)cmd, 0 };
    int stdio[3] = { in_fd, out_fd, 2 };
    return sys_spawn("/bin/sh", argv, stdio);
}
static struct ostream *get_ostream(const char *name, int kind)
{
    for (struct ostream *o = ostreams; o; o = o->next) if (!strcmp(o->name, name) && (o->kind == kind || (o->kind != '|' && kind != '|'))) return o;
    struct ostream *o = xmalloc(sizeof *o); memset(o, 0, sizeof *o);
    o->name = xstrdup(name); o->kind = kind; o->buf = xmalloc(4096);
    if (kind == '|') {
        int pp[2]; if (sys_pipe(pp) < 0) fatal("cannot make a pipe");
        oflush();
        o->pid = spawn_sh(name, pp[0], 1);
        sys_close(pp[0]);
        if (o->pid < 0) fatal("cannot run: %s", name);
        o->fd = pp[1];
    } else if (!strcmp(name, "/dev/stdout") || !strcmp(name, "-")) o->fd = 1;
    else if (!strcmp(name, "/dev/stderr")) o->fd = 2;
    else {
        o->fd = sys_open(name, O_WRONLY | O_CREAT | (kind == 'a' ? O_APPEND : O_TRUNC));
        if (o->fd < 0) fatal("cannot open %s for writing", name);
    }
    o->next = ostreams; ostreams = o;
    return o;
}
static int close_ostream(struct ostream *o)
{
    int st = 0;
    ostream_flush(o);
    if (o->fd > 2) sys_close(o->fd);
    if (o->kind == '|') { oflush(); st = sys_wait(o->pid); }
    for (struct ostream **p = &ostreams; *p; p = &(*p)->next) if (*p == o) { *p = o->next; break; }
    free(o->name); free(o->buf); free(o);
    return st;
}
static void flush_all_output(void) { oflush(); for (struct ostream *o = ostreams; o; o = o->next) ostream_flush(o); }
static void close_all_output(void)
{
    oflush();
    while (ostreams) close_ostream(ostreams);
    oflush();
}

/* read one record from a stream according to RS; returns the length, or -1 at the end. The result is in a malloc'd buffer *line. */
static int read_record(struct rd *r, char **line, u32 *cap)
{
    const char *rs = strvar(sRS);
    if (!rs[0]) {                                         /* paragraph mode */
        static char *tmpl; static u32 tcap; char *acc = 0; u32 alen = 0, acap = 0; int got = 0;
        for (;;) {
            int n = rd_getline(r, &tmpl, &tcap);
            if (n < 0) break;
            if (n == 0) { if (!got) continue; break; }
            got = 1;
            if (alen + (u32)n + 2 > acap) { acap = alen + (u32)n + 256; char *na = xmalloc(acap); if (alen) memcpy(na, acc, alen); if (acc) free(acc); acc = na; }
            if (alen) acc[alen++] = '\n';
            memcpy(acc + alen, tmpl, (u32)n); alen += (u32)n;
        }
        if (!got) { if (acc) free(acc); return -1; }
        if (alen + 1 > *cap) { if (*line) free(*line); *cap = alen + 64; *line = xmalloc(*cap); }
        memcpy(*line, acc, alen); (*line)[alen] = 0; free(acc); return (int)alen;
    }
    if (rs[0] == '\n' && !rs[1]) return rd_getline(r, line, cap);
    if (rs[1]) fatal("RS of more than one character is not supported (\"%s\")", rs);
    u32 n = 0; int c; char sep = rs[0];
    if (!*line) { *cap = 128; *line = xmalloc(*cap); }
    for (;;) {
        c = rd_getc(r);
        if (c < 0) { if (!n) return -1; break; }
        if (c == (unsigned char)sep) break;
        if (n + 2 > *cap) { *cap *= 2; char *nl = xmalloc(*cap); memcpy(nl, *line, n); free(*line); *line = nl; }
        (*line)[n++] = (char)c;
    }
    (*line)[n] = 0; return (int)n;
}
static struct istream *get_istream(const char *name, int kind)
{
    for (struct istream *s = istreams; s; s = s->next) if (!strcmp(s->name, name) && s->kind == kind) return s;
    struct istream *s = xmalloc(sizeof *s); memset(s, 0, sizeof *s);
    s->name = xstrdup(name); s->kind = kind;
    if (kind == 2) {
        int pp[2]; if (sys_pipe(pp) < 0) fatal("cannot make a pipe");
        oflush();
        s->pid = spawn_sh(name, 0, pp[1]);
        sys_close(pp[1]);
        if (s->pid < 0) { sys_close(pp[0]); free(s->name); free(s); return 0; }
        s->fd = pp[0];
    } else {
        s->fd = (!strcmp(name, "-") || !strcmp(name, "/dev/stdin")) ? 0 : sys_open(name, O_RDONLY);
        if (s->fd < 0) { free(s->name); free(s); return 0; }
    }
    rd_init(&s->r, s->fd);
    s->next = istreams; istreams = s;
    return s;
}
static int close_istream(struct istream *s)
{
    int st = 0;
    if (s->fd > 0) sys_close(s->fd);
    if (s->kind == 2) st = sys_wait(s->pid);
    for (struct istream **p = &istreams; *p; p = &(*p)->next) if (*p == s) { *p = s->next; break; }
    free(s->name); free(s);
    return st;
}

/* the main input: the files named in ARGV (and assignments var=value among them) */
static int argv_index = 1; static struct istream *main_in; static int main_in_is_stdin, any_file;
static int open_next_main(void)
{
    for (;;) {
        double argc = to_num(cell_val(&sARGC->c));
        if (argv_index >= (int)argc) {
            if (main_in_is_stdin || any_file) return 0;       /* a file was already read */
            main_in_is_stdin = 1; main_in = xmalloc(sizeof *main_in); memset(main_in, 0, sizeof *main_in); main_in->name = xstrdup("-"); main_in->fd = 0; rd_init(&main_in->r, 0);
            return 1;
        }
        char key[16]; snprintf(key, sizeof key, "%d", argv_index++);
        struct elem *e = arr_find(sARGV->c.a, key, 0);
        if (!e) continue;
        const char *arg = to_str(cell_val(&e->c));
        if (!arg[0]) continue;
        const char *eq = strchr(arg, '=');
        if (eq && eq > arg && (isalpha((unsigned char)arg[0]) || arg[0] == '_')) {      /* var=value */
            int ok = 1; for (const char *q = arg; q < eq; q++) if (!isalnum((unsigned char)*q) && *q != '_') ok = 0;
            if (ok) { char *nm = tstrn(arg, (u32)(eq - arg)); struct sym *s = sym_find(nm, 1); sym_set(s, vstrnum(tstr(eq + 1))); continue; }
        }
        main_in = xmalloc(sizeof *main_in); memset(main_in, 0, sizeof *main_in);
        main_in->name = xstrdup(arg);
        main_in->fd = !strcmp(arg, "-") ? 0 : sys_open(arg, O_RDONLY);
        if (main_in->fd < 0) { fprintf(2, "awk: cannot open %s\n", arg); free(main_in->name); free(main_in); main_in = 0; exit_code = 2; continue; }
        rd_init(&main_in->r, main_in->fd);
        set_strvar(sFILENAME, arg); fnr = 0; any_file = 1;
        return 1;
    }
}
static char *main_line; static u32 main_cap;
static int next_main_record(void)               /* 1 = a record is in main_line, 0 = no more input */
{
    for (;;) {
        if (!main_in && !open_next_main()) return 0;
        int n = read_record(&main_in->r, &main_line, &main_cap);
        if (n >= 0) { nr++; fnr++; return 1; }
        if (main_in->fd > 0) sys_close(main_in->fd);
        free(main_in->name); free(main_in); main_in = 0;
        if (main_in_is_stdin) return 0;
    }
}
static void skip_main_file(void) { if (main_in) { if (main_in->fd > 0) sys_close(main_in->fd); free(main_in->name); free(main_in); main_in = 0; } }

/* ================================================================ printf */
static const char *do_sprintf(const char *fmt, struct node **args, int nargs)
{
    char *out = talloc(1); u32 olen = 0, ocap = 0; (void)out;
    char *buf = 0; u32 cap = 0;
    #define APPEND(s, n) do { u32 _n = (n); if (olen + _n + 1 > cap) { cap = (olen + _n + 256) * 2; char *nb = xmalloc(cap); if (olen) memcpy(nb, buf, olen); if (buf) free(buf); buf = nb; } memcpy(buf + olen, (s), _n); olen += _n; } while (0)
    (void)ocap;
    int ai = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { APPEND(p, 1); continue; }
        if (p[1] == '%') { APPEND("%", 1); p++; continue; }
        char spec[40]; int sl = 0; spec[sl++] = '%'; p++;
        while (*p && strchr("-+ #0", *p) && sl < 8) spec[sl++] = *p++;
        if (*p == '*') { double w = ai < nargs ? to_num(eval(args[ai++])) : 0; sl += snprintf(spec + sl, 12, "%d", (int)w); p++; }
        else while (isdigit((unsigned char)*p) && sl < 16) spec[sl++] = *p++;
        if (*p == '.') {
            spec[sl++] = *p++;
            if (*p == '*') { double w = ai < nargs ? to_num(eval(args[ai++])) : 0; sl += snprintf(spec + sl, 12, "%d", (int)w); p++; }
            else while (isdigit((unsigned char)*p) && sl < 24) spec[sl++] = *p++;
        }
        while (*p == 'l' || *p == 'h' || *p == 'q' || *p == 'L') p++;
        char conv = *p; if (!conv) { APPEND(spec, (u32)sl); break; }
        struct val v = ai < nargs ? eval(args[ai++]) : vstr("");
        char tmp[128];
        switch (conv) {
        case 'd': case 'i': { double d = to_num(v); long iv = d > 2147483647.0 ? 2147483647L : d < -2147483648.0 ? -2147483647L - 1 : (long)d;
            spec[sl++] = 'd'; spec[sl] = 0; int n = snprintf(tmp, sizeof tmp, spec, (int)iv); APPEND(tmp, (u32)n); break; }
        case 'o': case 'x': case 'X': case 'u': { double d = to_num(v); unsigned long uv = d < 0 ? (unsigned long)(long)d : d > 4294967295.0 ? 4294967295UL : (unsigned long)d;
            spec[sl++] = conv; spec[sl] = 0; int n = snprintf(tmp, sizeof tmp, spec, (unsigned)uv); APPEND(tmp, (u32)n); break; }
        case 'c': { char ch;
            if ((v.fl & F_STR) && !(v.fl & F_STRNUM)) { const char *s = v.s; ch = s ? s[0] : 0; } else ch = (char)(int)to_num(v);
            spec[sl++] = 'c'; spec[sl] = 0; int n = snprintf(tmp, sizeof tmp, spec, ch); APPEND(tmp, (u32)n); break; }
        case 's': { const char *s = to_str(v); spec[sl++] = 's'; spec[sl] = 0; int n = snprintf(0, 0, spec, s); char *t = xmalloc((u32)n + 1); snprintf(t, (u32)n + 1, spec, s); APPEND(t, (u32)n); free(t); break; }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
            int flags_left = 0, flags_zero = 0, flags_plus = 0, flags_space = 0, alt = 0, width = 0, prec = -1;
            const char *q = spec + 1;
            for (; *q && strchr("-+ #0", *q); q++) { if (*q == '-') flags_left = 1; else if (*q == '0') flags_zero = 1; else if (*q == '+') flags_plus = 1; else if (*q == ' ') flags_space = 1; else alt = 1; }
            while (isdigit((unsigned char)*q)) width = width * 10 + (*q++ - '0');
            if (*q == '.') { q++; prec = 0; while (isdigit((unsigned char)*q)) prec = prec * 10 + (*q++ - '0'); }
            char num[400]; double d = to_num(v);
            int n = fmt_float(num, sizeof num, d, conv == 'F' ? 'f' : conv, prec, alt);
            char sign = 0; char *body = num;
            if (num[0] == '-') { sign = '-'; body++; n--; } else if (flags_plus) sign = '+'; else if (flags_space) sign = ' ';
            int total = n + (sign ? 1 : 0);
            int padn = width > total ? width - total : 0;
            if (!flags_left && !flags_zero) for (int k = 0; k < padn; k++) APPEND(" ", 1);
            if (sign) APPEND(&sign, 1);
            if (!flags_left && flags_zero) for (int k = 0; k < padn; k++) APPEND("0", 1);
            APPEND(body, (u32)n);
            if (flags_left) for (int k = 0; k < padn; k++) APPEND(" ", 1);
            break; }
        default: APPEND(spec, (u32)sl); APPEND(&conv, 1); break;
        }
    }
    if (!buf) return tstr("");
    buf[olen] = 0;
    char *r = tstrn(buf, olen); free(buf);
    return r;
    #undef APPEND
}

/* ================================================================ math without a math library */
static double m_sqrt(double x) { if (x <= 0) return 0; double g = x > 1 ? x / 2 : 1; for (int i = 0; i < 60; i++) { double ng = (g + x / g) / 2; if (ng == g) break; g = ng; } return g; }
static double m_exp(double x)
{
    if (x > 709) return 1e308 * 10;
    if (x < -745) return 0;
    int k = 0; while (x > 0.5) { x /= 2; k++; } while (x < -0.5) { x /= 2; k++; }
    double term = 1, sum = 1; for (int i = 1; i < 30; i++) { term = term * x / i; sum += term; }
    while (k--) sum *= sum;
    return sum;
}
static double m_log(double x)
{
    if (x <= 0) return x == 0 ? -1e308 * 10 : (x - x) / (x - x);
    int e = 0; while (x > 1.5) { x /= 2.718281828459045; e++; } while (x < 0.6) { x *= 2.718281828459045; e--; }
    double y = (x - 1) / (x + 1), y2 = y * y, term = y, sum = 0;
    for (int i = 1; i < 60; i += 2) { sum += term / i; term *= y2; }
    return 2 * sum + e;
}
static double m_reduce(double x) { const double pi2 = 6.283185307179586; while (x > 1e5 || x < -1e5) x /= 10; while (x > 3.141592653589793) x -= pi2; while (x < -3.141592653589793) x += pi2; return x; }
static double m_sin(double x) { x = m_reduce(x); double term = x, sum = x; for (int i = 1; i < 20; i++) { term = -term * x * x / ((2 * i) * (2 * i + 1)); sum += term; } return sum; }
static double m_cos(double x) { x = m_reduce(x); double term = 1, sum = 1; for (int i = 1; i < 20; i++) { term = -term * x * x / ((2 * i - 1) * (2 * i)); sum += term; } return sum; }
static double m_atan(double x)
{
    int neg = x < 0; if (neg) x = -x; int inv = x > 1; if (inv) x = 1 / x;
    for (int i = 0; i < 3; i++) x = x / (1 + m_sqrt(1 + x * x));          /* halve the angle three times */
    double term = x, sum = 0, x2 = x * x; for (int i = 1; i < 40; i += 2) { sum += term / i * ((i / 2) % 2 ? -1 : 1); term *= x2; }
    sum *= 8; if (inv) sum = 1.5707963267948966 - sum; return neg ? -sum : sum;
}
static double m_atan2(double y, double x)
{
    const double pi = 3.141592653589793;
    if (x > 0) return m_atan(y / x);
    if (x < 0) return y >= 0 ? m_atan(y / x) + pi : m_atan(y / x) - pi;
    return y > 0 ? pi / 2 : y < 0 ? -pi / 2 : 0;
}
static double m_rand(void) { u32 s = (u32)(rand_prev * 4294967296.0); s = s * 1103515245u + 12345u; rand_prev = (double)(s >> 8) / 16777216.0 + 1.0 / 33554432.0; if (rand_prev >= 1) rand_prev -= 1; return rand_prev; }

/* ================================================================ evaluation */
enum { C_NORMAL = 0, C_BREAK, C_CONTINUE, C_NEXT, C_NEXTFILE, C_EXIT, C_RETURN };
static int exec(struct node *n);
static struct val retval;
static int exit_requested;

static int compare(struct val a, struct val b)
{
    if ((a.fl & (F_NUM | F_STRNUM)) && (b.fl & (F_NUM | F_STRNUM))) { double x = to_num(a), y = to_num(b); return x < y ? -1 : x > y ? 1 : 0; }
    int c = strcmp(to_str(a), to_str(b)); return c < 0 ? -1 : c > 0 ? 1 : 0;
}
static void *node_re(struct node *n)                      /* the regular expression a node stands for: a /literal/ or a dynamic string */
{
    if (n->t == N_ERE) { if (!n->re) n->re = get_re(n->s); return n->re; }
    return get_re(to_str(eval(n)));
}
static int re_match_val(void *re, const char *s) { int caps[2 * RE_GROUPS]; return re_exec(re, s, (int)strlen(s), 0, caps); }

static struct val call_func(struct node *n)
{
    struct sym *s = sym_find(n->s, 0);
    if (!s || !s->fn) fatal("calling undefined function %s", n->s);
    struct func *f = s->fn;
    if (n->n > f->nparams) fatal("function %s called with %d arguments, accepts only %d", f->name, n->n, f->nparams);
    struct cell *nf2 = xmalloc((u32)(f->nparams + 1) * sizeof *nf2); memset(nf2, 0, (u32)(f->nparams + 1) * sizeof *nf2);
    for (int i = 0; i < n->n; i++) {
        struct node *a = n->list[i];
        if ((a->t == N_VAR || a->t == N_LOCAL) && (f->pisarr[i] || (a->t == N_VAR ? a->sym->c.a != 0 : frame[a->idx].a != 0))) {
            nf2[i].a = get_arr(a);                           /* an array goes by reference */
        } else cell_set(&nf2[i], eval(a));
    }
    struct cell *saved = frame; frame = nf2;
    ++call_depth;
    retval = vstr(""); retval.fl = F_STR | F_NUM | F_STRNUM; retval.n = 0;
    int c = exec(f->body);
    call_depth--;
    struct val r = retval;
    if (r.fl & F_STR) r.s = tstr(r.s ? r.s : "");
    frame = saved;
    for (int i = 0; i < f->nparams; i++) {
        cell_free_str(&nf2[i]);
        if (nf2[i].a && (nf2[i].fl & 8)) { arr_clear(nf2[i].a); free(nf2[i].a); }
    }
    free(nf2);
    if (c == C_EXIT) exit_requested = 1;
    return r;
}

static __attribute__((noinline)) struct val do_getline(struct node *n)
{
    char *line = 0; u32 cap = 0; int got;
    if (n->op == 0) {                                        /* getline [var] : the next record of the main input */
        got = next_main_record();
        if (got) {
            if (n->a) lv_set(n->a, vstrnum(tstr(main_line))); else set_record(main_line);
            return vnum(1);
        }
        return vnum(0);
    }
    const char *name = to_str(eval(n->b));
    struct istream *s = get_istream(name, n->op == 1 ? 1 : 2);
    if (!s) return vnum(-1);
    got = read_record(&s->r, &line, &cap);
    if (got < 0) { if (line) free(line); return vnum(0); }
    if (n->op == 2) nr++;
    if (n->a) lv_set(n->a, vstrnum(tstr(line))); else { set_record(line); }
    free(line);
    return vnum(1);
}

static __attribute__((noinline)) struct val do_builtin(struct node *n)
{
    struct node **a = n->list; int na = n->n;
    #define ARG(i) (i < na ? eval(a[i]) : vstr(""))
    switch (n->op) {
    case B_LENGTH: {
        if (na == 0) { struct val v = get_field(0); return vnum((double)strlen(to_str(v))); }
        if ((a[0]->t == N_VAR && a[0]->sym->c.a) || (a[0]->t == N_LOCAL && frame[a[0]->idx].a)) return vnum(get_arr(a[0])->n);
        return vnum((double)strlen(to_str(eval(a[0]))));
    }
    case B_SUBSTR: {
        const char *s = to_str(ARG(0)); double m = to_num(ARG(1)); double len = (double)strlen(s);
        double cnt = na > 2 ? to_num(ARG(2)) : 1e30;
        double mr = m >= 0 ? (double)(long)(m + 0.5) : -(double)(long)(-m + 0.5);       /* round */
        double cr = cnt >= 0 ? (double)(long)(cnt + 0.5) : 0;
        double from = mr, to = mr + cr;                                                 /* characters from..to-1, 1-based */
        if (from < 1) from = 1;
        if (to > len + 1) to = len + 1;
        if (to <= from) return vstr("");
        return vstr(tstrn(s + (long)from - 1, (u32)(to - from)));
    }
    case B_INDEX: {
        const char *s = to_str(ARG(0)), *t = to_str(ARG(1)); const char *p = strstr(s, t);
        return vnum(p ? (double)(p - s + 1) : 0);
    }
    case B_SPLIT: {
        const char *s = to_str(ARG(0)); struct arr *arr = get_arr(a[1]);
        const char *fs = strvar(sFS); void *sepre = 0;
        if (na > 2) { if (a[2]->t == N_ERE) sepre = node_re(a[2]); else fs = to_str(eval(a[2])); }
        arr_clear(arr);
        struct cell *savef = flds; int savenf = nf, savecap = fcap, savevalid = fields_valid; flds = 0; nf = 0; fcap = 0;      /* split into a private field list */
        if (sepre) {
            int len = (int)strlen(s), from = 0, start = 0, caps[2 * RE_GROUPS];
            while (from <= len && re_exec(sepre, s, len, from, caps)) { if (caps[1] == caps[0]) { from = caps[0] + 1; continue; } add_field(s + start, (u32)(caps[0] - start)); start = caps[1]; from = caps[1]; }
            if (len) add_field(s + start, (u32)(len - start));
        } else split_fields(s, (int)strlen(s), fs);
        int cnt = nf;
        for (int i = 1; i <= cnt; i++) { char key[16]; snprintf(key, sizeof key, "%d", i); struct elem *e = arr_find(arr, key, 1); cell_set(&e->c, cell_val(&flds[i])); }
        fields_clear(); flds = savef; nf = savenf; fcap = savecap; fields_valid = savevalid;
        return vnum(cnt);
    }
    case B_SUB: case B_GSUB: {
        void *re = node_re(a[0]); const char *repl = to_str(ARG(1));
        struct node *target = na > 2 ? a[2] : 0;
        struct val tv = target ? (is_lvalue(target) ? lv_get(target) : eval(target)) : get_field(0);
        const char *s = to_str(tv); int len = (int)strlen(s), from = 0, count = 0, caps[2 * RE_GROUPS];
        char *out = xmalloc((u32)len * 2 + 64); u32 ol = 0, oc = (u32)len * 2 + 64; int last = 0;
        #define OUT(p, k) do { u32 _k = (k); if (ol + _k + 1 > oc) { oc = (ol + _k) * 2 + 64; char *nb = xmalloc(oc); memcpy(nb, out, ol); free(out); out = nb; } memcpy(out + ol, (p), _k); ol += _k; } while (0)
        while (from <= len && re_exec(re, s, len, from, caps)) {
            int ms = caps[0], me = caps[1];
            OUT(s + last, (u32)(ms - last));
            for (const char *r = repl; *r; r++) {
                if (*r == '\\' && (r[1] == '&' || r[1] == '\\')) { OUT(r + 1, 1); r++; }
                else if (*r == '&') OUT(s + ms, (u32)(me - ms));
                else OUT(r, 1);
            }
            count++; last = me;
            if (me == ms) { if (ms < len) OUT(s + ms, 1); last = ms + 1; from = ms + 1; } else from = me;
            if (n->op == B_SUB) break;
        }
        if (last <= len) OUT(s + last, (u32)(len - last));
        out[ol] = 0;
        if (count && (!target || is_lvalue(target))) { struct val nv = vstr(tstrn(out, ol)); if (target) lv_set(target, nv); else set_field(0, nv); }
        free(out);
        return vnum(count);
        #undef OUT
    }
    case B_MATCH: {
        const char *s = to_str(ARG(0)); void *re = node_re(a[1]); int caps[2 * RE_GROUPS];
        if (re_exec(re, s, (int)strlen(s), 0, caps)) {
            sym_set(sRSTART, vnum(caps[0] + 1)); sym_set(sRLENGTH, vnum(caps[1] - caps[0]));
            return vnum(caps[0] + 1);
        }
        sym_set(sRSTART, vnum(0)); sym_set(sRLENGTH, vnum(-1)); return vnum(0);
    }
    case B_SPRINTF: {
        if (na < 1) fatal("sprintf needs a format");
        return vstr(do_sprintf(to_str(eval(a[0])), a + 1, na - 1));
    }
    case B_SIN: return vnum(m_sin(to_num(ARG(0))));
    case B_COS: return vnum(m_cos(to_num(ARG(0))));
    case B_ATAN2: return vnum(m_atan2(to_num(ARG(0)), to_num(ARG(1))));
    case B_EXP: return vnum(m_exp(to_num(ARG(0))));
    case B_LOG: return vnum(m_log(to_num(ARG(0))));
    case B_SQRT: return vnum(m_sqrt(to_num(ARG(0))));
    case B_INT: { double d = to_num(ARG(0)); return vnum(d >= 0 ? (double)(long long)d : -(double)(long long)(-d)); }
    case B_RAND: return vnum(m_rand());
    case B_SRAND: { double old = rand_seed; rand_seed = na ? to_num(ARG(0)) : (double)sys_time(); rand_prev = rand_seed / 4294967296.0 + 0.3; if (rand_prev >= 1) rand_prev -= 1; return vnum(old); }
    case B_TOLOWER: case B_TOUPPER: {
        const char *s = to_str(ARG(0)); char *r = tstr(s); for (char *p = r; *p; p++) *p = (char)(n->op == B_TOLOWER ? tolower((unsigned char)*p) : toupper((unsigned char)*p));
        return vstr(r);
    }
    case B_SYSTEM: {
        const char *cmd = to_str(ARG(0)); flush_all_output();
        int pid = spawn_sh(cmd, 0, 1); if (pid < 0) return vnum(-1);
        sys_setfg(pid); int st = sys_wait(pid); sys_setfg(sys_getpid());
        return vnum(st);
    }
    case B_CLOSE: {
        const char *name = to_str(ARG(0)); int st = -1;
        for (struct ostream *o = ostreams; o; o = o->next) if (!strcmp(o->name, name)) { st = close_ostream(o); break; }
        for (struct istream *s = istreams; s; s = s->next) if (!strcmp(s->name, name)) { st = close_istream(s); break; }
        return vnum(st);
    }
    case B_FFLUSH: oflush(); for (struct ostream *o = ostreams; o; o = o->next) ostream_flush(o); return vnum(0);
    }
    return vnum(0);
    #undef ARG
}

/* eval and exec recurse (expressions, statements, user functions), so their frames stay small: the big helpers they call
 * (do_builtin, do_print, do_getline) are kept out of line. */
static struct val eval(struct node *n)
{
    stack_check();
    switch (n->t) {
    case N_NUM: return vnum(n->num);
    case N_STR: return vstr(n->s);
    case N_ERE: return vnum(re_match_val(node_re(n), to_str(get_field(0))));
    case N_VAR: case N_LOCAL: case N_FIELD: case N_INDEX: return lv_get(n);
    case N_GROUP: return eval(n->a);
    case N_ASSIGN: {
        struct val v;
        if (n->op == 0) {
            v = eval(n->b);
            if (n->a->t == N_FIELD && !(v.fl & F_STR) ) { }
        } else {
            double l = to_num(lv_get(n->a)), r = to_num(eval(n->b)), res;
            switch (n->op) {
            case '+': res = l + r; break; case '-': res = l - r; break; case '*': res = l * r; break;
            case '/': if (r == 0) fatal("division by zero in /="); res = l / r; break;
            case '%': if (r == 0) fatal("division by zero in %%="); res = l - (double)(long long)(l / r) * r; break;
            default: { double rr = 1; long e = (long)r; int neg = e < 0; if (neg) e = -e; for (long i = 0; i < e; i++) rr *= l; res = neg ? 1 / rr : rr; }
            }
            v = vnum(res);
        }
        lv_set(n->a, v);
        return v;
    }
    case N_COND: return to_bool(eval(n->a)) ? eval(n->b) : eval(n->c);
    case N_OR: return vnum(to_bool(eval(n->a)) || to_bool(eval(n->b)));
    case N_AND: return vnum(to_bool(eval(n->a)) && to_bool(eval(n->b)));
    case N_NOT: return vnum(!to_bool(eval(n->a)));
    case N_NEG: return vnum(-to_num(eval(n->a)));
    case N_PLUS: return vnum(to_num(eval(n->a)));
    case N_IN: { struct arr *arr = get_arr(n->a); return vnum(arr_find(arr, join_subs(n->list, n->n), 0) != 0); }
    case N_MATCH: case N_NOMATCH: {
        const char *s = to_str(eval(n->a)); int m = re_match_val(node_re(n->b), s);
        return vnum(n->t == N_MATCH ? m : !m);
    }
    case N_CMP: {
        int c = compare(eval(n->a), eval(n->b));
        switch (n->op) { case T_LT: return vnum(c < 0); case T_LE: return vnum(c <= 0); case T_EQ: return vnum(c == 0); case T_NE: return vnum(c != 0); case T_GT: return vnum(c > 0); default: return vnum(c >= 0); }
    }
    case N_CONCAT: {
        const char *l = to_str(eval(n->a)); u32 ll = strlen(l);
        const char *r = to_str(eval(n->b)); u32 rl = strlen(r);
        char *s = talloc(ll + rl + 1); memcpy(s, l, ll); memcpy(s + ll, r, rl + 1); return vstr(s);
    }
    case N_BINOP: {
        double l = to_num(eval(n->a)), r = to_num(eval(n->b));
        switch (n->op) {
        case '+': return vnum(l + r); case '-': return vnum(l - r); case '*': return vnum(l * r);
        case '/': if (r == 0) fatal("division by zero"); return vnum(l / r);
        case '%': if (r == 0) fatal("division by zero in %%"); return vnum(l - (double)(long long)(l / r) * r);
        default: { double rr = 1; long e = (long)r; int neg = e < 0; if (neg) e = -e; double b = l; while (e) { if (e & 1) rr *= b; b *= b; e >>= 1; } return vnum(neg ? 1 / rr : rr); }
        }
    }
    case N_PREINC: { double v = to_num(lv_get(n->a)) + n->op; struct val nv = vnum(v); lv_set(n->a, nv); return nv; }
    case N_POSTINC: { double old = to_num(lv_get(n->a)); lv_set(n->a, vnum(old + n->op)); return vnum(old); }
    case N_CALL: return call_func(n);
    case N_BUILTIN: return do_builtin(n);
    case N_GETLINE: return do_getline(n);
    }
    fatal("internal error: bad expression node %d", n->t);
    return vnum(0);
}

/* ================================================================ statements */
static __attribute__((noinline)) void do_print(struct node *n)
{
    char *line = 0; u32 len = 0;
    if (n->t == S_PRINT) {
        const char *ofs = strvar(sOFS), *ors = strvar(sORS); u32 ol = strlen(ofs);
        if (n->n == 0) { const char *s = to_str(get_field(0)); u32 l = strlen(s); line = talloc(l + strlen(ors) + 1); memcpy(line, s, l); len = l; }
        else {
            const char **parts = (const char **)talloc((u32)n->n * sizeof *parts); u32 total = 0; int np = n->n;
            for (int i = 0; i < np; i++) { struct val v = eval(n->list[i]); parts[i] = (v.fl & F_NUM) && !(v.fl & F_STR) ? num2str(v.n, strvar(sOFMT)[0] ? strvar(sOFMT) : convfmt) : to_str(v); total += strlen(parts[i]) + ol; }
            line = talloc(total + strlen(ors) + 1);
            for (int i = 0; i < np; i++) { if (i) { memcpy(line + len, ofs, ol); len += ol; } u32 l = strlen(parts[i]); memcpy(line + len, parts[i], l); len += l; }
        }
        u32 rl = strlen(ors); memcpy(line + len, ors, rl); len += rl;
    } else {
        if (n->n < 1) fatal("printf needs a format");
        const char *s = do_sprintf(to_str(eval(n->list[0])), n->list + 1, n->n - 1);
        len = strlen(s); line = (char *)s;
    }
    if (!n->op) { oputn(line, len); if (sys_isatty(1)) oflush(); return; }
    const char *dest = to_str(eval(n->a));
    struct ostream *o = get_ostream(dest, n->op == T_PIPE ? '|' : n->op == T_APPEND ? 'a' : 'w');
    ostream_write(o, line, len);
    if (o->fd == 1 || o->fd == 2) { ostream_flush(o); if (o->fd == 1) oflush(); }
}

static int exec(struct node *n)
{
    stack_check();
    if (!n) return C_NORMAL;
    switch (n->t) {
    case S_BLOCK:
        for (int i = 0; i < n->n; i++) { int c = exec(n->list[i]); if (c) return c; if (!call_depth) tmp_reset(); }
        return C_NORMAL;
    case S_EXPR: eval(n->a); return exit_requested ? C_EXIT : C_NORMAL;
    case S_PRINT: case S_PRINTF: do_print(n); return C_NORMAL;
    case S_IF: if (to_bool(eval(n->a))) return exec(n->b); return n->c ? exec(n->c) : C_NORMAL;
    case S_WHILE:
        while (to_bool(eval(n->a))) {
            int c = exec(n->b);
            if (c == C_BREAK) break;
            if (c == C_CONTINUE) continue;
            if (c) return c;
            if (!call_depth) tmp_reset();
        }
        return C_NORMAL;
    case S_DO:
        do {
            int c = exec(n->b);
            if (c == C_BREAK) break;
            if (c == C_CONTINUE) continue;
            if (c) return c;
        } while (to_bool(eval(n->a)));
        return C_NORMAL;
    case S_FOR: {
        if (n->a) exec(n->a);
        while (!n->b || to_bool(eval(n->b))) {
            int c = exec(n->d);
            if (c == C_BREAK) break;
            if (c && c != C_CONTINUE) return c;
            if (n->c) exec(n->c);
            if (!call_depth) tmp_reset();
        }
        return C_NORMAL;
    }
    case S_FORIN: {
        struct arr *arr = get_arr(n->b);
        int cnt = 0; for (int i = 0; i < NBUCKET; i++) for (struct elem *e = arr->b[i]; e; e = e->next) cnt++;
        char **keys = xmalloc((u32)(cnt + 1) * sizeof *keys); int k = 0;
        for (int i = 0; i < NBUCKET; i++) for (struct elem *e = arr->b[i]; e; e = e->next) keys[k++] = xstrdup(e->key);
        int rc = C_NORMAL;
        for (int i = 0; i < k; i++) {
            if (!arr_find(arr, keys[i], 0)) continue;              /* deleted meanwhile */
            lv_set(n->a, vstrnum(keys[i]));
            int c = exec(n->c);
            if (c == C_BREAK) break;
            if (c == C_CONTINUE) continue;
            if (c) { rc = c; break; }
        }
        for (int i = 0; i < k; i++) free(keys[i]);
        free(keys);
        return rc;
    }
    case S_BREAK: return C_BREAK;
    case S_CONTINUE: return C_CONTINUE;
    case S_NEXT: return C_NEXT;
    case S_NEXTFILE: return C_NEXTFILE;
    case S_EXIT: if (n->a) exit_code = (int)to_num(eval(n->a)); return C_EXIT;
    case S_RETURN: retval = n->a ? eval(n->a) : vstr(""); if (retval.fl & F_STR) retval.s = tstr(retval.s ? retval.s : ""); return C_RETURN;
    case S_DELETE: {
        struct arr *arr = get_arr(n->a);
        if (n->n) arr_delete(arr, join_subs(n->list, n->n)); else arr_clear(arr);
        return C_NORMAL;
    }
    }
    fatal("internal error: bad statement node %d", n->t);
    return C_NORMAL;
}

/* ================================================================ the main loop and the command line */
static int rule_matches(struct rule *r)
{
    if (!r->pat) return 1;
    if (!r->pat2) return to_bool(eval(r->pat));
    if (!r->in_range) {
        if (!to_bool(eval(r->pat))) return 0;
        r->in_range = 1;
    }
    if (to_bool(eval(r->pat2))) r->in_range = 0;
    return 1;
}
static void run_record(void)
{
    for (struct rule *r = main_rules; r; r = r->next) {
        if (!rule_matches(r)) continue;
        int c;
        if (!r->action) { oputs(to_str(get_field(0))); oputs(strvar(sORS)); if (sys_isatty(1)) oflush(); c = C_NORMAL; }
        else c = exec(r->action);
        tmp_reset();
        if (c == C_NEXT) return;
        if (c == C_NEXTFILE) { skip_main_file(); return; }
        if (c == C_EXIT || exit_requested) { exit_requested = 1; return; }
    }
}
static char *read_file_text(const char *path)
{
    int fd = strcmp(path, "-") ? sys_open(path, O_RDONLY) : 0;
    if (fd < 0) fatal("cannot open program file %s", path);
    u32 cap = 4096, n = 0; char *b = xmalloc(cap); int r;
    for (;;) { if (n + 2048 > cap) { cap *= 2; char *nb = xmalloc(cap); memcpy(nb, b, n); free(b); b = nb; } r = sys_read(fd, b + n, 2048); if (r <= 0) break; n += (u32)r; }
    b[n] = 0; if (fd) sys_close(fd); return b;
}
static char *unescape(const char *s)
{
    char *o = xmalloc(strlen(s) + 1); u32 n = 0;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            switch (*s) { case 'n': o[n++] = '\n'; break; case 't': o[n++] = '\t'; break; case 'r': o[n++] = '\r'; break; case '\\': o[n++] = '\\'; break; case '"': o[n++] = '"'; break; default: o[n++] = '\\'; o[n++] = *s; }
        } else o[n++] = *s;
    }
    o[n] = 0; return o;
}
static void assign_cmdline(const char *spec)           /* name=value from -v or the argument list */
{
    const char *eq = strchr(spec, '='); char *name = tstrn(spec, (u32)(eq - spec));
    struct sym *s = sym_find(name, 1); char *val = unescape(eq + 1);
    sym_set(s, vstrnum(tstr(val))); free(val);
}

int main(int argc, char **argv)
{
    { char *sp; __asm__ volatile("move.l %%sp,%0" : "=r"(sp)); stk_top = sp; }
    sNF = special("NF", SP_NF); sNR = special("NR", SP_NR); sFNR = special("FNR", SP_FNR); sFS = special("FS", SP_FS); sOFS = special("OFS", SP_OFS);
    sORS = special("ORS", SP_ORS); sRS = special("RS", SP_RS); sSUBSEP = special("SUBSEP", SP_SUBSEP); sRSTART = special("RSTART", SP_RSTART);
    sRLENGTH = special("RLENGTH", SP_RLENGTH); sFILENAME = special("FILENAME", SP_FILENAME); sCONVFMT = special("CONVFMT", SP_CONVFMT);
    sOFMT = special("OFMT", SP_OFMT); sENVIRON = special("ENVIRON", SP_ENVIRON); sARGC = special("ARGC", SP_ARGC); sARGV = special("ARGV", SP_ARGV);
    set_strvar(sFS, " "); set_strvar(sOFS, " "); set_strvar(sORS, "\n"); set_strvar(sRS, "\n"); set_strvar(sSUBSEP, "\034");
    set_strvar(sCONVFMT, "%.6g"); set_strvar(sOFMT, "%.6g"); convfmt = strvar(sCONVFMT);
    cell_set(&sRSTART->c, vnum(0)); cell_set(&sRLENGTH->c, vnum(-1));
    sENVIRON->c.a = arr_new(); sARGV->c.a = arr_new();
    if (environ) for (char **e = environ; *e; e++) { const char *eq = strchr(*e, '='); if (!eq) continue; char *k = tstrn(*e, (u32)(eq - *e)); struct elem *el = arr_find(sENVIRON->c.a, k, 1); cell_set(&el->c, vstrnum(tstr(eq + 1))); }

    char *progtext = 0; int i = 1;
    char *vspecs[32]; int nv = 0; const char *fsopt = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *o = argv[i];
        if (!strcmp(o, "--")) { i++; break; }
        if (o[1] == 'F') { fsopt = o[2] ? o + 2 : (i + 1 < argc ? argv[++i] : ""); }
        else if (o[1] == 'v') { const char *a = o[2] ? o + 2 : (i + 1 < argc ? argv[++i] : ""); if (nv < 32 && strchr(a, '=')) vspecs[nv++] = (char *)a; else fatal("-v needs name=value"); }
        else if (o[1] == 'f') {
            const char *f = o[2] ? o + 2 : (i + 1 < argc ? argv[++i] : "");
            char *t = read_file_text(f);
            if (progtext) { char *c = xmalloc(strlen(progtext) + strlen(t) + 2); strcpy(c, progtext); strcat(c, "\n"); strcat(c, t); progtext = c; } else progtext = t;
        }
        else if (!strcmp(o, "--version") || !strcmp(o, "-version")) { printf("awk (Zenix) 1\n"); return 0; }
        else fatal("unknown option %s", o);
    }
    if (!progtext) { if (i >= argc) { fprintf(2, "usage: awk [-F fs] [-v var=value] [-f progfile | 'program'] [file ...]\n"); return 2; } progtext = argv[i++]; }
    if (fsopt) { char *u = unescape(fsopt); if (!strcmp(u, "t")) { free(u); u = xstrdup("\t"); } set_strvar(sFS, u); free(u); }
    for (int k = 0; k < nv; k++) assign_cmdline(vspecs[k]);

    char *progcopy = xstrdup(progtext);
    lex(progcopy); parse_program();

    struct elem *e0 = arr_find(sARGV->c.a, "0", 1); cell_set(&e0->c, vstr("awk"));
    int ac = 1;
    for (; i < argc; i++, ac++) { char key[16]; snprintf(key, sizeof key, "%d", ac); struct elem *e = arr_find(sARGV->c.a, key, 1); cell_set(&e->c, vstrnum(argv[i])); }
    cell_set(&sARGC->c, vnum(ac));

    for (struct rule *r = begin_rules; r && !exit_requested; r = r->next) {
        int c = exec(r->action); tmp_reset();
        if (c == C_EXIT || exit_requested) exit_requested = 1;
    }
    if (main_rules || end_rules) {
        if (!exit_requested) {
            while (next_main_record()) {
                set_record(main_line);
                run_record();
                if (exit_requested) break;
            }
        }
    }
    exit_requested = 0;
    int skip_end = 0; (void)skip_end;
    for (struct rule *r = end_rules; r; r = r->next) {
        int c = exec(r->action); tmp_reset();
        if (c == C_EXIT || exit_requested) break;
    }
    close_all_output();
    return exit_code;
}
