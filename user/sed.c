#include "libz.h"
#include "regex.h"
/* sed [-n] [-E|-r] [-i] [-e script] [-f file] [script] [file...]
 * Commands: s/re/repl/[g p N i]  d  p  q  n  N  a\ i\ c\ (text)  y/abc/xyz/  =  with addresses
 * N, $, /re/, N,M, /re/,/re/, addr!  Basic regular expressions by default (\( \) \{ \} \| \+ \?),
 * extended with -E. In the replacement: & is the match, \1..\9 the groups, \n a newline. */

#define MAXCMD 200
struct cmd {
    char op;                              /* s d p q n N a i c y = { } */
    char a1t, a2t, neg;                   /* address types: 0 none, 'n' number, '$' last, '/' regex */
    long a1n, a2n; void *a1r, *a2r;
    int in_range; long range_end;
    void *re; char *repl; int sflags, snth;       /* s: flags: 1 g, 2 p, 4 i */
    char *text; char *ytab;               /* a i c text; y map */
};
static struct cmd cmds[MAXCMD]; static int ncmd;
static int o_n, o_E, o_i;
static struct rd rdr; static char *nextline; static u32 nextcap; static int have_next, lineno;
static int quit_now;

/* convert a basic regular expression to the extended syntax the engine speaks */
static char *bre_to_ere(const char *s)
{
    char *out = malloc(strlen(s) * 2 + 2), *o = out; int in_br = 0;
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
static void *compile(const char *re, int icase)
{
    char *e = o_E ? strdup(re) : bre_to_ere(re);
    void *p = re_comp(e, (icase ? RE_ICASE : 0) | RE_LONGEST);
    if (!p) { fprintf(2, "sed: bad regular expression: %s\n", re); _zexit(2); }
    return p;
}

static void die(const char *m, const char *a) { fprintf(2, "sed: %s%s%s\n", m, a ? ": " : "", a ? a : ""); _zexit(2); }

/* read a delimited part of s (after the opening delimiter); returns the part, advances *p past the closing one */
static char *part(const char **p, char delim, int is_regex)
{
    char *o = malloc(strlen(*p) + 2), *w = o; const char *s = *p;
    while (*s && *s != delim) {
        if (*s == '\\' && s[1]) {
            if (s[1] == delim) { *w++ = delim; s += 2; continue; }
            if (!is_regex && s[1] == 'n') { *w++ = '\n'; s += 2; continue; }
            *w++ = *s++; *w++ = *s++; continue;
        }
        *w++ = *s++;
    }
    if (!*s) die("unterminated command", 0);
    *w = 0; *p = s + 1; return o;
}

static int addr(const char **p, char *t, long *n, void **r)
{
    const char *s = *p;
    if (isdigit((unsigned char)*s)) { *n = strtol(s, (char **)&s, 10); *t = 'n'; }
    else if (*s == '$') { *t = '$'; s++; }
    else if (*s == '/') { s++; char *re = part(&s, '/', 1); *r = compile(re, 0); *t = '/'; }
    else return 0;
    *p = s; return 1;
}

static void parse_script(const char *s)
{
    while (*s) {
        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == ';') s++;
        if (!*s) break;
        if (*s == '#') { while (*s && *s != '\n') s++; continue; }
        if (ncmd >= MAXCMD) die("too many commands", 0);
        struct cmd *c = &cmds[ncmd]; memset(c, 0, sizeof *c);
        if (addr(&s, &c->a1t, &c->a1n, &c->a1r)) {
            if (*s == ',') { s++; if (!addr(&s, &c->a2t, &c->a2n, &c->a2r)) die("bad address", 0); }
        }
        while (*s == ' ') s++;
        if (*s == '!') { c->neg = 1; s++; while (*s == ' ') s++; }
        c->op = *s++;
        switch (c->op) {
        case 's': {
            char d = *s++; if (!d) die("unterminated s command", 0);
            char *re = part(&s, d, 1); c->repl = part(&s, d, 0);
            int icase = 0;
            for (;; s++) {
                if (*s == 'g') c->sflags |= 1; else if (*s == 'p') c->sflags |= 2; else if (*s == 'i' || *s == 'I') icase = 1;
                else if (isdigit((unsigned char)*s)) { c->snth = (int)strtol(s, (char **)&s, 10); s--; }
                else break;
            }
            c->re = compile(re, icase);
            break;
        }
        case 'y': {
            char d = *s++; char *a = part(&s, d, 0), *b = part(&s, d, 0);
            if (strlen(a) != strlen(b)) die("y: strings differ in length", 0);
            c->ytab = malloc(256); for (int k = 0; k < 256; k++) c->ytab[k] = (char)k;
            for (int k = 0; a[k]; k++) c->ytab[(unsigned char)a[k]] = b[k];
            break;
        }
        case 'a': case 'i': case 'c': {
            if (*s == '\\') { s++; while (*s == ' ') s++; if (*s == '\n') s++; }
            else while (*s == ' ') s++;
            const char *e = s; while (*e && *e != '\n') e++;
            c->text = malloc((u32)(e - s) + 1); memcpy(c->text, s, (u32)(e - s)); c->text[e - s] = 0; s = e;
            break;
        }
        case 'd': case 'p': case 'q': case 'n': case 'N': case '=': case '{': case '}': break;
        default: { char b[2] = { c->op, 0 }; die("unknown command", b); }
        }
        ncmd++;
    }
}

static int read_line(char **line, u32 *cap)
{
    if (have_next) { /* the lookahead line */
        char *t = *line; u32 tc = *cap; *line = nextline; *cap = nextcap; nextline = t; nextcap = tc; have_next = 0;
        return (int)strlen(*line) >= 0;
    }
    return rd_getline(&rdr, line, cap) >= 0;
}
static int is_last(void)
{
    if (have_next) return 0;
    if (rd_getline(&rdr, &nextline, &nextcap) >= 0) { have_next = 1; return 0; }
    return 1;
}

static int addr_match(struct cmd *c, const char *line)
{
    int m;
    if (!c->a1t) m = 1;
    else if (!c->a2t) {
        m = c->a1t == 'n' ? lineno == c->a1n : c->a1t == '$' ? is_last() : re_exec(c->a1r, line, (int)strlen(line), 0, 0);
    } else {
        if (c->in_range) {
            m = 1;
            if (c->a2t == 'n') { if (lineno >= c->a2n) c->in_range = 0; }
            else if (c->a2t == '$') { if (is_last()) c->in_range = 0; }
            else if (re_exec(c->a2r, line, (int)strlen(line), 0, 0)) c->in_range = 0;
        } else {
            int start = c->a1t == 'n' ? lineno == c->a1n : c->a1t == '$' ? is_last() : re_exec(c->a1r, line, (int)strlen(line), 0, 0);
            m = start;
            if (start) {
                c->in_range = 1;
                if (c->a2t == 'n' && lineno >= c->a2n) c->in_range = 0;
                else if (c->a2t == '$' && is_last()) c->in_range = 0;
            }
        }
    }
    return c->neg ? !m : m;
}

/* one s command on the pattern space; returns 1 when a substitution was made */
static int do_subst(struct cmd *c, char **ps, u32 *cap)
{
    const char *line = *ps; int len = (int)strlen(line), from = 0, count = 0, did = 0, caps[2 * RE_GROUPS];
    char *out = malloc((u32)len * 2 + 64); u32 oc = (u32)len * 2 + 64, on = 0;
    #define OPUT(ch) do { if (on + 2 > oc) { char *nb = malloc(oc * 2); memcpy(nb, out, on); out = nb; oc *= 2; } out[on++] = (char)(ch); } while (0)
    int copied = 0;
    while (from <= len && re_exec(c->re, line, len, from, caps)) {
        int ms = caps[0], me = caps[1];
        count++;
        if (c->snth && count < c->snth) { for (int k = copied; k < me; k++) OPUT(line[k]); copied = me; from = me > ms ? me : me + 1; if (me == ms && ms < len) { OPUT(line[ms]); copied = ms + 1; } continue; }
        for (int k = copied; k < ms; k++) OPUT(line[k]);
        for (const char *r = c->repl; *r; r++) {
            if (*r == '&') { for (int k = ms; k < me; k++) OPUT(line[k]); }
            else if (*r == '\\' && r[1] >= '1' && r[1] <= '9') { int g = r[1] - '0'; r++; if (caps[2 * g] >= 0) for (int k = caps[2 * g]; k < caps[2 * g + 1]; k++) OPUT(line[k]); }
            else if (*r == '\\' && r[1]) { r++; OPUT(*r == 'n' ? '\n' : *r == 't' ? '\t' : *r); }
            else OPUT(*r);
        }
        did = 1; copied = me;
        if (!(c->sflags & 1)) break;
        if (me == ms) { if (ms < len) OPUT(line[ms]); copied = ms + 1; from = ms + 1; } else from = me;
    }
    if (!did) { free(out); return 0; }
    for (int k = copied; k < len; k++) OPUT(line[k]);
    out[on] = 0;
    *ps = out; *cap = oc;
    return 1;
}

static int run_stream(int fd, int outfd)
{
    rd_init(&rdr, fd); lineno = 0; have_next = 0; quit_now = 0;
    char *ps = 0, *hold = 0; u32 pcap = 0, hcap = 0; (void)hold; (void)hcap;
    for (int k = 0; k < ncmd; k++) cmds[k].in_range = 0;
    #define OUTS(str) do { if (outfd == 1) oputs(str); else sys_write(outfd, str, strlen(str)); } while (0)
    #define OUTNL() do { if (outfd == 1) oputc('\n'); else sys_write(outfd, "\n", 1); } while (0)
    while (read_line(&ps, &pcap)) {
        lineno++;
        int deleted = 0, printed_auto = !o_n; const char *pending = 0;
        for (int k = 0; k < ncmd && !quit_now; k++) {
            struct cmd *c = &cmds[k];
            if (c->op == '}') continue;
            if (c->op == '{') { if (!addr_match(c, ps)) { int d = 1; while (d && ++k < ncmd) { if (cmds[k].op == '{') d++; else if (cmds[k].op == '}') d--; } } continue; }
            if (!addr_match(c, ps)) continue;
            switch (c->op) {
            case 's': if (do_subst(c, &ps, &pcap) && (c->sflags & 2)) { OUTS(ps); OUTNL(); } break;
            case 'd': deleted = 1; k = ncmd; break;
            case 'p': OUTS(ps); OUTNL(); break;
            case 'q': quit_now = 1; break;
            case '=': { char b[16]; b[0] = 0; char t[12]; int n = 0; long v = lineno; do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v); int j = 0; while (n) b[j++] = t[--n]; b[j] = 0; OUTS(b); OUTNL(); break; }
            case 'a': pending = c->text; break;
            case 'i': OUTS(c->text); OUTNL(); break;
            case 'c': if (!c->a2t || !c->in_range) { OUTS(c->text); OUTNL(); } deleted = 1; k = ncmd; break;
            case 'y': for (char *q = ps; *q; q++) *q = c->ytab[(unsigned char)*q]; break;
            case 'n': if (printed_auto) { OUTS(ps); OUTNL(); } if (!read_line(&ps, &pcap)) { deleted = 1; k = ncmd; break; } lineno++; break;
            case 'N': { char *nl = 0; u32 nc = 0; if (read_line(&nl, &nc)) { lineno++; char *j = malloc(strlen(ps) + strlen(nl) + 2); strcpy(j, ps); strcat(j, "\n"); strcat(j, nl); ps = j; pcap = (u32)strlen(j) + 1; } else { quit_now = 1; } break; }
            }
        }
        if (!deleted && printed_auto) { OUTS(ps); OUTNL(); }
        if (pending) { OUTS(pending); OUTNL(); }
        if (quit_now) break;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1, have_script = 0; char *script = malloc(1); script[0] = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "-e") && i + 1 < argc) { char *n = malloc(strlen(script) + strlen(argv[i + 1]) + 2); strcpy(n, script); strcat(n, argv[++i]); strcat(n, "\n"); script = n; have_script = 1; continue; }
        if (!strcmp(argv[i], "-f") && i + 1 < argc) {
            int fd = sys_open(argv[++i], O_RDONLY); if (fd < 0) die("cannot read the script", argv[i]);
            struct rd r; rd_init(&r, fd); char *ln = 0; u32 cp = 0;
            while (rd_getline(&r, &ln, &cp) >= 0) { char *n = malloc(strlen(script) + strlen(ln) + 2); strcpy(n, script); strcat(n, ln); strcat(n, "\n"); script = n; }
            sys_close(fd); have_script = 1; continue;
        }
        for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'n') o_n = 1; else if (*p == 'E' || *p == 'r') o_E = 1; else if (*p == 'i') o_i = 1; else if (*p == 's') ; else die("unknown option", argv[i]); }
    }
    if (!have_script) { if (i >= argc) { fprintf(2, "usage: sed [-nEi] [-e script] [-f file] [script] [file...]\n"); return 2; } script = argv[i++]; }
    parse_script(script);
    if (i >= argc) { run_stream(0, 1); oflush(); return 0; }
    int rc = 0;
    for (; i < argc; i++) {
        int fd = sys_open(argv[i], O_RDONLY);
        if (fd < 0) { fprintf(2, "sed: %s: %s\n", argv[i], zwhy("cannot open")); rc = 2; continue; }
        if (o_i) {
            char tmp[160]; strcpy(tmp, argv[i]); strcat(tmp, ".sedtmp");
            int out = sys_open(tmp, O_WRONLY | O_CREAT | O_TRUNC); if (out < 0) { fprintf(2, "sed: cannot write %s\n", tmp); rc = 2; sys_close(fd); continue; }
            run_stream(fd, out); sys_close(fd); sys_close(out);
            sys_unlink(argv[i]); sys_rename(tmp, argv[i]);
        } else { run_stream(fd, 1); sys_close(fd); }
        if (quit_now) break;
    }
    oflush();
    return rc;
}
