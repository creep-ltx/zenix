#include "libz.h"

/* sh - the Zenix shell.
 *
 * Interactive: a line editor (cursor keys, Home/End, Ctrl-A/E/B/F/K/U/W/L/D,
 * history with Up/Down and Ctrl-P/N, Tab completion of commands, paths and
 * variables), a prompt from $PS1, history kept in ~/.sh_history.
 * Language: variables and the environment, $VAR ${VAR} $? $$ $# $1.. $@ $*,
 * $(command), ~, wildcards * ? [..], quotes and backslashes, pipes,
 * < > >> 2> 2>&1 &> << (here-documents), && || ; &, ! , if/elif/else/fi,
 * while/until/for loops, functions, aliases, scripts (sh file, . file), -c.
 * Builtins: cd exit export unset set alias unalias history source . read
 * shift return break continue jobs wait kill type test [ true false : echo
 * pwd help. Everything else is a program found through $PATH. */

#define E_HZ 0
typedef unsigned char u8;

/* ---------------------------------------------------------------- output */
static void outs(const char *s) { sys_write(1, s, strlen(s)); }
static void errs(const char *s) { sys_write(2, s, strlen(s)); }
static void errm(const char *a, const char *b) { errs("sh: "); errs(a); if (b) { errs(": "); errs(b); } errs("\n"); }

/* ---------------------------------------------------------------- arena
 * Everything a command line needs (tokens, tree, expansions) comes from one
 * region that is reset after each line. */
#define ARENA 262144
static char *arena, *abrk, *aend;
static void fatal(const char *m) { errm(m, 0); sys_exit(2); }
static void *aalloc(u32 n)
{
    n = (n + 3) & ~3u;
    if (abrk + n > aend) fatal("out of memory");
    void *p = abrk; abrk += n; return p;
}
static char *adupn(const char *s, u32 n) { char *p = aalloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }
static char *adup(const char *s) { return adupn(s, strlen(s)); }

/* persistent strings (variables, history, aliases, functions) */
static char *pdup(const char *s) { u32 n = strlen(s) + 1; char *p = malloc(n); if (!p) fatal("out of memory"); memcpy(p, s, n); return p; }

/* ---------------------------------------------------------------- state */
static int last_status, shell_pid, last_bg, interactive, loop_break, loop_continue, func_return, in_func, exit_requested;
static char *pos_arg[24]; static int npos;
static char *arg0 = "sh";

struct var { char *name, *val; u32 cap; u8 exp, used; };
static struct var vars[128];

static struct var *var_find(const char *n)
{
    for (int i = 0; i < 128; i++) if (vars[i].used && !strcmp(vars[i].name, n)) return &vars[i];
    return 0;
}
static const char *var_get(const char *n) { struct var *v = var_find(n); return v ? v->val : 0; }
static void env_sync(void);
static void var_set(const char *n, const char *val, int exp)
{
    struct var *v = var_find(n);
    if (!v) {
        for (int i = 0; i < 128; i++) if (!vars[i].used) { v = &vars[i]; break; }
        if (!v) { errm("too many variables", 0); return; }
        v->used = 1; v->name = pdup(n); v->val = 0; v->cap = 0; v->exp = 0;
    }
    u32 need = strlen(val) + 1;
    if (need > v->cap) { v->val = malloc(need < 32 ? 32 : need); v->cap = need < 32 ? 32 : need; }
    memcpy(v->val, val, need);
    if (exp >= 0) v->exp = (u8)exp;
}
static void var_unset(const char *n) { struct var *v = var_find(n); if (v) v->used = 0; }

/* the exported variables go to the kernel, which gives them to every child */
static void env_sync(void)
{
    static char blk[1024];
    u32 n = 0;
    for (int i = 0; i < 128; i++) {
        if (!vars[i].used || !vars[i].exp) continue;
        u32 a = strlen(vars[i].name), b = strlen(vars[i].val);
        if (n + a + b + 3 > sizeof blk) break;
        memcpy(blk + n, vars[i].name, a); n += a; blk[n++] = '=';
        memcpy(blk + n, vars[i].val, b); n += b; blk[n++] = 0;
    }
    blk[n++] = 0;
    sys_envset(blk, n);
}

struct alias { char *name, *val; u8 used; };
static struct alias aliases[48];
static struct alias *alias_find(const char *n) { for (int i = 0; i < 48; i++) if (aliases[i].used && !strcmp(aliases[i].name, n)) return &aliases[i]; return 0; }
struct func { char *name, *body; u8 used; };
static struct func funcs[48];
static struct func *func_find(const char *n) { for (int i = 0; i < 48; i++) if (funcs[i].used && !strcmp(funcs[i].name, n)) return &funcs[i]; return 0; }

#define HMAX 200
static char *hist[HMAX]; static int nhist;
static void hist_add(const char *l)
{
    if (!*l || (nhist && !strcmp(hist[nhist - 1], l))) return;
    if (nhist == HMAX) { free(hist[0]); memmove(hist, hist + 1, (HMAX - 1) * sizeof(char *)); nhist--; }
    hist[nhist++] = pdup(l);
}

/* ---------------------------------------------------------------- small helpers */
static int is_space(int c) { return c == ' ' || c == '\t' || c == '\n'; }
static int is_name_c(int c, int first) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (!first && c >= '0' && c <= '9'); }
static void utoa(u32 v, char *b) { char t[12]; int i = 0; do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v); int k = 0; while (i) b[k++] = t[--i]; b[k] = 0; }
static int path_is_dir(const char *p) { struct ustat st; return sys_stat(p, &st) == 0 && (st.mode & S_IFMT) == S_IFDIR; }
static int path_is_file(const char *p) { struct ustat st; return sys_stat(p, &st) == 0 && (st.mode & S_IFMT) == S_IFREG; }
static int path_exists(const char *p) { struct ustat st; return sys_stat(p, &st) == 0; }

static char *file_slurp(const char *path, u32 *len)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct ustat st;
    if (sys_fstat(fd, &st) < 0) { sys_close(fd); return 0; }
    char *b = aalloc(st.size + 2);
    u32 got = 0; int n;
    while (got < st.size && (n = sys_read(fd, b + got, st.size - got)) > 0) got += (u32)n;
    sys_close(fd);
    b[got] = 0; if (len) *len = got;
    return b;
}

/* ================================================================ line editor */
#define LMAX 480
static char *expand_prompt(const char *ps, int *vislen);
static int builtin_names(const char **out, int max);

static void put_n(const char *s, int n) { sys_write(1, s, (u32)n); }

/* redraw the line: prompt, the visible part of the text, cursor in place */
static void redraw(const char *prompt, int plen, const char *buf, int len, int pos, int *scroll)
{
    int avail = 77 - plen;
    if (avail < 10) avail = 10;
    if (pos < *scroll) *scroll = pos;
    if (pos > *scroll + avail - 1) *scroll = pos - avail + 1;
    if (len <= avail) *scroll = 0;
    int shown = len - *scroll; if (shown > avail) shown = avail;
    outs("\r"); outs(prompt);
    put_n(buf + *scroll, shown);
    outs("\033[K");
    int back = shown - (pos - *scroll);
    if (back > 0) { char e[16]; e[0] = 27; e[1] = '['; utoa((u32)back, e + 2); u32 l = strlen(e); e[l] = 'D'; e[l + 1] = 0; outs(e); }
}

/* ---- completion ---- */
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void sort_strs(char **v, int n)
{
    for (int i = 1; i < n; i++) { char *t = v[i]; int j = i; while (j > 0 && strcmp(v[j - 1], t) > 0) { v[j] = v[j - 1]; j--; } v[j] = t; }
    (void)cmp_str;
}

static int add_match(char **m, int n, int max, const char *s)
{
    for (int i = 0; i < n; i++) if (!strcmp(m[i], s)) return n;
    if (n < max) m[n++] = adup(s);
    return n;
}

static int complete_dir(char **m, int n, int max, const char *dir, const char *base, int dirs_only_slash, int cmd_only_exec)
{
    int fd = sys_open(dir[0] ? dir : ".", O_RDONLY);
    if (fd < 0) return n;
    struct udirent d;
    u32 bl = strlen(base);
    while (sys_readdir(fd, &d) > 0) {
        if (strncmp(d.name, base, bl)) continue;
        if (d.name[0] == '.' && base[0] != '.') continue;
        char full[200]; u32 dl = strlen(dir);
        memcpy(full, dir, dl); strcpy(full + dl, d.name);
        char cand[100]; strcpy(cand, d.name);
        int isdir = path_is_dir(full);
        if (cmd_only_exec && !isdir && !path_is_file(full)) continue;
        if (isdir && dirs_only_slash) { u32 l = strlen(cand); cand[l] = '/'; cand[l + 1] = 0; }
        n = add_match(m, n, max, cand);
    }
    sys_close(fd);
    return n;
}

/* returns the number of matches; the matches are strings in the arena */
static int find_completions(const char *buf, int pos, char **m, int max, int *wstart)
{
    int ws = pos;
    while (ws > 0 && !is_space(buf[ws - 1]) && !strchr("|;&<>(", buf[ws - 1])) ws--;
    *wstart = ws;
    int cmdpos = 1;
    for (int i = 0; i < ws; i++) if (!is_space(buf[i]) && !strchr("|;&(", buf[i])) { cmdpos = 0; break; }
    /* after a | ; & ( the next word is a command again */
    { int i = ws - 1; while (i >= 0 && is_space(buf[i])) i--; if (i >= 0 && strchr("|;&(", buf[i])) cmdpos = 1; }
    char pre[200]; int pl = pos - ws; if (pl > 190) pl = 190;
    memcpy(pre, buf + ws, (u32)pl); pre[pl] = 0;
    int n = 0;
    if (pre[0] == '$' && !strchr(pre, '/')) {                       /* variable names */
        for (int i = 0; i < 128; i++) if (vars[i].used && !strncmp(vars[i].name, pre + 1, strlen(pre + 1))) {
            char c[100]; c[0] = '$'; strcpy(c + 1, vars[i].name); n = add_match(m, n, max, c);
        }
    } else if (strchr(pre, '/')) {                                  /* a path */
        char *sl = pre + strlen(pre); while (sl > pre && sl[-1] != '/') sl--;
        char dir[200]; u32 dl = (u32)(sl - pre); memcpy(dir, pre, dl); dir[dl] = 0;
        n = complete_dir(m, n, max, dir, sl, 1, 0);
        for (int i = 0; i < n; i++) { char full[300]; strcpy(full, dir); strcpy(full + dl, m[i]); m[i] = adup(full); }
    } else if (cmdpos) {                                            /* a command */
        const char *bn[64]; int nb = builtin_names(bn, 64);
        for (int i = 0; i < nb; i++) if (!strncmp(bn[i], pre, (u32)pl)) n = add_match(m, n, max, bn[i]);
        for (int i = 0; i < 48; i++) if (aliases[i].used && !strncmp(aliases[i].name, pre, (u32)pl)) n = add_match(m, n, max, aliases[i].name);
        for (int i = 0; i < 48; i++) if (funcs[i].used && !strncmp(funcs[i].name, pre, (u32)pl)) n = add_match(m, n, max, funcs[i].name);
        const char *path = var_get("PATH"); if (!path) path = "/bin";
        char dir[100]; const char *p = path;
        while (*p) {
            const char *e = p; while (*e && *e != ':') e++;
            u32 l = (u32)(e - p); if (l > 90) l = 90;
            memcpy(dir, p, l); if (l && dir[l - 1] != '/') dir[l++] = '/'; dir[l] = 0;
            n = complete_dir(m, n, max, dir, pre, 0, 1);
            p = *e ? e + 1 : e;
        }
    } else {
        n = complete_dir(m, n, max, "", pre, 1, 0);
    }
    sort_strs(m, n);
    return n;
}

static void list_matches(char **m, int n)
{
    int w = 0; for (int i = 0; i < n; i++) { int l = (int)strlen(m[i]); if (l > w) w = l; }
    w += 2; int cols = 78 / w; if (cols < 1) cols = 1;
    outs("\r\n");
    for (int i = 0; i < n; i++) {
        outs(m[i]);
        if ((i + 1) % cols == 0 || i == n - 1) outs("\r\n");
        else for (int k = (int)strlen(m[i]); k < w; k++) outs(" ");
    }
}

/* read one edited line; returns its length, -1 at end of input (Ctrl-D on an empty line), -2 when cancelled with Ctrl-C */
static int readline(const char *prompt, int plen, char *buf, int max)
{
    int len = 0, pos = 0, scroll = 0, hidx = nhist, tabs = 0;
    static char saved[LMAX];
    buf[0] = 0;
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    redraw(prompt, plen, buf, len, pos, &scroll);
    for (;;) {
        unsigned char c;
        if (sys_read(0, &c, 1) != 1) { sys_ttymode(0); return -1; }
        int again_tab = 0;
        if (c == 27) {
            unsigned char s1, s2 = 0;
            if (sys_read(0, &s1, 1) != 1) continue;
            if (s1 == '[' || s1 == 'O') {
                int num = 0;
                for (;;) {
                    if (sys_read(0, &s2, 1) != 1) break;
                    if (s2 >= '0' && s2 <= '9') { num = num * 10 + (s2 - '0'); continue; }
                    break;
                }
                if (s2 == 'A') c = 16;                         /* up = Ctrl-P */
                else if (s2 == 'B') c = 14;                    /* down = Ctrl-N */
                else if (s2 == 'C') c = 6;                     /* right = Ctrl-F */
                else if (s2 == 'D') c = 2;                     /* left = Ctrl-B */
                else if (s2 == 'H') c = 1;                     /* Home */
                else if (s2 == 'F') c = 5;                     /* End */
                else if (s2 == '~') { c = num == 1 || num == 7 ? 1 : num == 4 || num == 8 ? 5 : num == 3 ? 4 : 0; if (num == 3) c = 0x80; }
                else c = 0;
                if (!c) continue;
            } else if (s1 == 'b') { while (pos > 0 && is_space(buf[pos - 1])) pos--; while (pos > 0 && !is_space(buf[pos - 1])) pos--; redraw(prompt, plen, buf, len, pos, &scroll); continue; }
            else if (s1 == 'f') { while (pos < len && is_space(buf[pos])) pos++; while (pos < len && !is_space(buf[pos])) pos++; redraw(prompt, plen, buf, len, pos, &scroll); continue; }
            else if (s1 == 'd') { int e = pos; while (e < len && is_space(buf[e])) e++; while (e < len && !is_space(buf[e])) e++; memmove(buf + pos, buf + e, (u32)(len - e + 1)); len -= e - pos; redraw(prompt, plen, buf, len, pos, &scroll); continue; }
            else continue;
        }
        if (c != 9) tabs = 0;
        switch (c) {
        case 13: case 10:
            outs("\r\n"); sys_ttymode(0); buf[len] = 0; return len;
        case 3:                                                 /* Ctrl-C: cancel the line */
            outs("^C\r\n"); sys_ttymode(0); buf[0] = 0; return -2;
        case 4:                                                 /* Ctrl-D: end of input, or delete */
            if (len == 0) { outs("\r\n"); sys_ttymode(0); return -1; }
            /* fall through */
        case 0x80:                                              /* Del */
            if (pos < len) { memmove(buf + pos, buf + pos + 1, (u32)(len - pos)); len--; }
            break;
        case 127: case 8:
            if (pos > 0) { memmove(buf + pos - 1, buf + pos, (u32)(len - pos + 1)); pos--; len--; }
            break;
        case 1: pos = 0; break;
        case 5: pos = len; break;
        case 2: if (pos > 0) pos--; break;
        case 6: if (pos < len) pos++; break;
        case 11: len = pos; buf[len] = 0; break;                /* Ctrl-K */
        case 21: memmove(buf, buf + pos, (u32)(len - pos + 1)); len -= pos; pos = 0; break;   /* Ctrl-U */
        case 23: { int s = pos; while (s > 0 && is_space(buf[s - 1])) s--; while (s > 0 && !is_space(buf[s - 1])) s--;
                   memmove(buf + s, buf + pos, (u32)(len - pos + 1)); len -= pos - s; pos = s; break; }   /* Ctrl-W */
        case 12: outs("\033[2J\033[H"); break;                  /* Ctrl-L */
        case 20: if (pos > 0 && len > 1) { if (pos == len) pos--; char t = buf[pos - 1]; buf[pos - 1] = buf[pos]; buf[pos] = t; pos++; } break;   /* Ctrl-T */
        case 16:                                                /* history: previous */
            if (hidx > 0) {
                if (hidx == nhist) { memcpy(saved, buf, (u32)len + 1); }
                hidx--; strcpy(buf, hist[hidx]); len = pos = (int)strlen(buf);
            }
            break;
        case 14:                                                /* history: next */
            if (hidx < nhist) {
                hidx++;
                if (hidx == nhist) strcpy(buf, saved); else strcpy(buf, hist[hidx]);
                len = pos = (int)strlen(buf);
            }
            break;
        case 9: {                                               /* Tab */
            char *m[160]; int ws;
            char *mark = abrk;
            int n = find_completions(buf, pos, m, 160, &ws);
            if (n == 0) { abrk = mark; break; }
            const char *first = m[0]; int common = (int)strlen(first);
            for (int i = 1; i < n; i++) { int k = 0; while (k < common && m[i][k] == first[k]) k++; common = k; }
            int have = pos - ws;
            if (common > have && len + (common - have) < max - 2) {
                int add = common - have;
                memmove(buf + pos + add, buf + pos, (u32)(len - pos + 1));
                memcpy(buf + pos, first + have, (u32)add);
                pos += add; len += add;
                if (n == 1 && first[common - 1] != '/' && len < max - 2) {   /* a single match: finish the word */
                    memmove(buf + pos + 1, buf + pos, (u32)(len - pos + 1)); buf[pos] = ' '; pos++; len++;
                }
            } else if (n > 1) {
                if (tabs) { list_matches(m, n); }
                else again_tab = 1;
            } else if (n == 1 && first[common - 1] != '/' && len < max - 2) {
                memmove(buf + pos + 1, buf + pos, (u32)(len - pos + 1)); buf[pos] = ' '; pos++; len++;
            }
            abrk = mark;
            break;
        }
        default:
            if (c >= 32 && len < max - 2) {
                memmove(buf + pos + 1, buf + pos, (u32)(len - pos + 1));
                buf[pos++] = (char)c; len++;
            }
        }
        if (again_tab) tabs = 1;
        redraw(prompt, plen, buf, len, pos, &scroll);
    }
}

/* ================================================================ lexer */
enum { T_WORD, T_OP, T_NL };
struct tok { u8 type, fd, hdquoted; char op[4]; char *text; u32 start, end; char *heredoc; };
static struct tok *toks;
static int ntoks, ti, tokcap;
static const char *lex_src;
static int lex_incomplete;
static char *perr;

static int skip_paren(const char *s, int i)               /* s[i..] = "$(": index after the matching ')', or -1 */
{
    int d = 1; i += 2;
    while (s[i]) {
        char c = s[i];
        if (c == '\\' && s[i + 1]) { i += 2; continue; }
        if (c == '\'') { i++; while (s[i] && s[i] != '\'') i++; if (!s[i]) return -1; i++; continue; }
        if (c == '"') { i++; while (s[i] && s[i] != '"') { if (s[i] == '\\' && s[i + 1]) i++; i++; } if (!s[i]) return -1; i++; continue; }
        if (c == '(') d++;
        if (c == ')' && --d == 0) return i + 1;
        i++;
    }
    return -1;
}

static int is_op_char(int c) { return c == ';' || c == '&' || c == '|' || c == '<' || c == '>' || c == '(' || c == ')'; }

/* find the end of a word starting at i; -1 if a quote or construct is unterminated */
static int word_end(const char *s, int i)
{
    while (s[i] && !is_space(s[i]) && !is_op_char(s[i])) {
        char c = s[i];
        if (c == '\\') { if (!s[i + 1]) return i + 1; i += 2; continue; }
        if (c == '\'') { i++; while (s[i] && s[i] != '\'') i++; if (!s[i]) return -1; i++; continue; }
        if (c == '"') {
            i++;
            while (s[i] && s[i] != '"') {
                if (s[i] == '\\' && s[i + 1]) { i += 2; continue; }
                if (s[i] == '$' && s[i + 1] == '(') { int e = skip_paren(s, i); if (e < 0) return -1; i = e; continue; }
                if (s[i] == '`') { i++; while (s[i] && s[i] != '`') i++; if (!s[i]) return -1; i++; continue; }
                i++;
            }
            if (!s[i]) return -1;
            i++; continue;
        }
        if (c == '`') { i++; while (s[i] && s[i] != '`') i++; if (!s[i]) return -1; i++; continue; }
        if (c == '$' && s[i + 1] == '(') { int e = skip_paren(s, i); if (e < 0) return -1; i = e; continue; }
        if (c == '$' && s[i + 1] == '{') { i += 2; while (s[i] && s[i] != '}') i++; if (!s[i]) return -1; i++; continue; }
        i++;
    }
    return i;
}

static void new_tok(int type, u32 start, u32 end)
{
    if (ntoks >= tokcap) {                              /* grow: the token list is as long as the script needs */
        int nc = tokcap ? tokcap * 2 : 256;
        struct tok *nt = aalloc((u32)nc * sizeof(struct tok));
        if (ntoks) memcpy(nt, toks, (u32)ntoks * sizeof(struct tok));
        toks = nt; tokcap = nc;
    }
    struct tok *t = &toks[ntoks++];
    memset(t, 0, sizeof *t);
    t->type = (u8)type; t->start = start; t->end = end; t->fd = 255;
}

/* returns 0, 1 = incomplete (more input needed), 2 = syntax error */
static int lex(const char *s)
{
    int i = 0, pend[8], npend = 0;
    lex_src = s; ntoks = 0; ti = 0; lex_incomplete = 0; toks = 0; tokcap = 0;
    while (s[i]) {
        char c = s[i];
        if (c == ' ' || c == '\t') { i++; continue; }
        if (c == '\\' && s[i + 1] == '\n') { i += 2; continue; }
        if (c == '#') { while (s[i] && s[i] != '\n') i++; continue; }
        if (c == '\n') {
            new_tok(T_NL, (u32)i, (u32)i + 1); i++;
            for (int k = 0; k < npend; k++) {               /* here-document bodies follow this newline */
                struct tok *h = &toks[pend[k]];
                const char *delim = h->text;
                char dl[100]; u32 dn = 0;
                for (const char *q = delim; *q && dn < 98; q++) if (*q != '\'' && *q != '"' && *q != '\\') dl[dn++] = *q;
                dl[dn] = 0;
                u32 rem = strlen(s + i);
                char *body = aalloc(rem + 2); u32 blen = 0; int found = 0;
                while (s[i]) {
                    int e = i; while (s[e] && s[e] != '\n') e++;
                    int ls = i;
                    if (h->fd == 1) while (ls < e && s[ls] == '\t') ls++;
                    if ((u32)(e - ls) == dn && !strncmp(s + ls, dl, dn)) { found = 1; i = s[e] ? e + 1 : e; break; }
                    memcpy(body + blen, s + ls, (u32)(e - ls)); blen += (u32)(e - ls); body[blen++] = '\n';
                    i = s[e] ? e + 1 : e;
                }
                if (!found) return 1;
                body[blen] = 0; h->heredoc = body;
            }
            npend = 0;
            continue;
        }
        if (is_op_char(c)) {
            char two = s[i + 1];
            struct tok *t;
            if (c == '&' && two == '&') { new_tok(T_OP, (u32)i, (u32)i + 2); strcpy(toks[ntoks - 1].op, "&&"); i += 2; continue; }
            if (c == '|' && two == '|') { new_tok(T_OP, (u32)i, (u32)i + 2); strcpy(toks[ntoks - 1].op, "||"); i += 2; continue; }
            if (c == '>' && two == '>') { new_tok(T_OP, (u32)i, (u32)i + 2); strcpy(toks[ntoks - 1].op, ">>"); i += 2; continue; }
            if (c == '>' && two == '&') { new_tok(T_OP, (u32)i, (u32)i + 2); strcpy(toks[ntoks - 1].op, ">&"); i += 2; continue; }
            if (c == '&' && two == '>') { new_tok(T_OP, (u32)i, (u32)i + 2); strcpy(toks[ntoks - 1].op, "&>"); i += 2; continue; }
            if (c == '<' && two == '<') {
                int dash = s[i + 2] == '-';
                new_tok(T_OP, (u32)i, (u32)i + 2 + (u32)dash); t = &toks[ntoks - 1]; strcpy(t->op, "<<"); t->fd = dash ? 1 : 0;
                i += 2 + dash;
                while (s[i] == ' ' || s[i] == '\t') i++;
                int e = word_end(s, i);
                if (e < 0) return 1;
                if (e == i) return 2;
                new_tok(T_WORD, (u32)i, (u32)e); toks[ntoks - 1].text = adupn(s + i, (u32)(e - i));
                if (npend < 8) pend[npend++] = ntoks - 2;      /* the << token carries the body (and fd 1 = strip tabs) */
                toks[ntoks - 2].text = toks[ntoks - 1].text;
                toks[ntoks - 2].hdquoted = (strchr(toks[ntoks - 1].text, '\'') || strchr(toks[ntoks - 1].text, '"') || strchr(toks[ntoks - 1].text, '\\')) ? 1 : 0;
                ntoks--;                                       /* the delimiter word is folded into the << token */
                i = e; continue;
            }
            new_tok(T_OP, (u32)i, (u32)i + 1); t = &toks[ntoks - 1]; t->op[0] = c; t->op[1] = 0; i++;
            continue;
        }
        int e = word_end(s, i);
        if (e < 0) return 1;
        /* N> and N< : a single digit right before a redirection operator */
        if (e == i + 1 && s[i] >= '0' && s[i] <= '9' && (s[e] == '>' || s[e] == '<')) {
            int fd = s[i] - '0'; i = e;
            char c2 = s[i], two = s[i + 1];
            new_tok(T_OP, (u32)i, (u32)i + 1); struct tok *t = &toks[ntoks - 1]; t->fd = (u8)fd;
            if (c2 == '>' && two == '>') { strcpy(t->op, ">>"); i += 2; t->end = (u32)i; }
            else if (c2 == '>' && two == '&') { strcpy(t->op, ">&"); i += 2; t->end = (u32)i; }
            else { t->op[0] = c2; t->op[1] = 0; i++; }
            continue;
        }
        new_tok(T_WORD, (u32)i, (u32)e);
        toks[ntoks - 1].text = adupn(s + i, (u32)(e - i));
        i = e;
    }
    if (npend) return 1;
    return 0;
}

/* ================================================================ parser */
enum { N_SIMPLE, N_PIPE, N_AND, N_OR, N_SEQ, N_BG, N_IF, N_WHILE, N_FOR, N_GROUP, N_SUBSH, N_FUNC };
struct redir { u8 fd, kind; u8 hdquoted; char *word, *body; struct redir *next; };   /* kind: 0 < 1 > 2 >> 3 >& 4 &> 5 << */
struct node {
    u8 type, negate, until;
    struct node *a, *b, *c;
    int nwords; char **words;
    int nassign; char **assign;
    struct redir *redirs;
    struct node **cmds; int ncmds;
    char *name; char *src;
    u32 start, end;
};

static struct node *new_node(int type)
{
    struct node *n = aalloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->type = (u8)type;
    return n;
}

static struct node *parse_list(const char *const *stops);
static struct node *parse_andor(const char *const *stops);

static int at_eof(void) { return ti >= ntoks; }
static struct tok *peek(void) { return ti < ntoks ? &toks[ti] : 0; }
static int is_op(struct tok *t, const char *o) { return t && t->type == T_OP && !strcmp(t->op, o); }
static int is_kw(struct tok *t, const char *w) { return t && t->type == T_WORD && !strcmp(t->text, w); }
static void skip_nl(void) { while (ti < ntoks && toks[ti].type == T_NL) ti++; }
static void syn(const char *m) { if (!perr) perr = (char *)m; }

static int at_stop(const char *const *stops)
{
    struct tok *t = peek();
    if (!t || !stops) return 0;
    for (int i = 0; stops[i]; i++) {
        if (stops[i][0] == ')' && is_op(t, ")")) return 1;
        if (t->type == T_WORD && !strcmp(t->text, stops[i])) return 1;
    }
    return 0;
}

static int is_assign(const char *w)
{
    if (!is_name_c((unsigned char)w[0], 1)) return 0;
    int i = 1; while (is_name_c((unsigned char)w[i], 0)) i++;
    return w[i] == '=';
}

static struct node *parse_simple(void)
{
    struct node *n = new_node(N_SIMPLE);
    char *words[64]; char *assigns[24]; int nw = 0, na = 0;
    struct redir *rhead = 0, **rtail = &rhead;
    u32 start = toks[ti].start, end = start;
    for (;;) {
        struct tok *t = peek();
        if (!t) break;
        if (t->type == T_NL) break;
        if (t->type == T_OP) {
            const char *o = t->op;
            if (!strcmp(o, "<") || !strcmp(o, ">") || !strcmp(o, ">>") || !strcmp(o, ">&") || !strcmp(o, "&>") || !strcmp(o, "<<")) {
                struct redir *r = aalloc(sizeof *r); memset(r, 0, sizeof *r);
                r->kind = !strcmp(o, "<") ? 0 : !strcmp(o, ">") ? 1 : !strcmp(o, ">>") ? 2 : !strcmp(o, ">&") ? 3 : !strcmp(o, "&>") ? 4 : 5;
                r->fd = t->fd != 255 ? t->fd : (r->kind == 0 || r->kind == 5) ? 0 : 1;
                if (r->kind == 5) { r->body = t->heredoc; r->hdquoted = t->hdquoted; end = t->end; ti++; }
                else {
                    ti++;
                    struct tok *w = peek();
                    if (!w || w->type != T_WORD) { if (!w) lex_incomplete = 1; syn("missing redirection target"); return 0; }
                    r->word = w->text; end = w->end; ti++;
                }
                *rtail = r; rtail = &r->next;
                continue;
            }
            break;                                                 /* | ; & && || ( ) end the command */
        }
        if (nw == 0 && na < 24 && is_assign(t->text) && !(is_kw(t, "{") )) { assigns[na++] = t->text; end = t->end; ti++; continue; }
        if (nw < 63) words[nw++] = t->text;
        end = t->end; ti++;
    }
    if (!nw && !na && !rhead) { syn("syntax error"); return 0; }
    n->nwords = nw; n->words = aalloc((u32)(nw + 1) * sizeof(char *)); memcpy(n->words, words, (u32)nw * sizeof(char *)); n->words[nw] = 0;
    n->nassign = na; n->assign = aalloc((u32)(na + 1) * sizeof(char *)); memcpy(n->assign, assigns, (u32)na * sizeof(char *));
    n->redirs = rhead; n->start = start; n->end = end;
    n->src = adupn(lex_src + start, end - start);
    return n;
}

static struct node *parse_command(void)
{
    struct tok *t = peek();
    if (!t) { lex_incomplete = 1; syn("unexpected end"); return 0; }
    if (t->type == T_WORD) {
        u32 start = t->start;
        if (is_kw(t, "if")) {
            ti++;
            static const char *const s_then[] = { "then", 0 };
            struct node *n = new_node(N_IF);
            n->a = parse_list(s_then); if (!n->a) return 0;
            if (!is_kw(peek(), "then")) { if (at_eof()) lex_incomplete = 1; syn("expected then"); return 0; }
            ti++;
            static const char *const s_else[] = { "elif", "else", "fi", 0 };
            n->b = parse_list(s_else); if (!n->b) return 0;
            struct node *cur = n;
            while (is_kw(peek(), "elif")) {
                ti++;
                struct node *e = new_node(N_IF); e->until = 1;
                e->a = parse_list(s_then); if (!e->a) return 0;
                if (!is_kw(peek(), "then")) { if (at_eof()) lex_incomplete = 1; syn("expected then"); return 0; }
                ti++;
                e->b = parse_list(s_else); if (!e->b) return 0;
                cur->c = e; cur = e;
            }
            if (is_kw(peek(), "else")) {
                ti++;
                static const char *const s_fi[] = { "fi", 0 };
                cur->c = parse_list(s_fi); if (!cur->c) return 0;
            }
            if (!is_kw(peek(), "fi")) { if (at_eof()) lex_incomplete = 1; syn("expected fi"); return 0; }
            n->end = toks[ti].end; ti++;
            n->start = start; n->src = adupn(lex_src + start, n->end - start);
            return n;
        }
        if (is_kw(t, "while") || is_kw(t, "until")) {
            int until = is_kw(t, "until"); ti++;
            static const char *const s_do[] = { "do", 0 };
            struct node *n = new_node(N_WHILE); n->until = (u8)until;
            n->a = parse_list(s_do); if (!n->a) return 0;
            if (!is_kw(peek(), "do")) { if (at_eof()) lex_incomplete = 1; syn("expected do"); return 0; }
            ti++;
            static const char *const s_done[] = { "done", 0 };
            n->b = parse_list(s_done); if (!n->b) return 0;
            if (!is_kw(peek(), "done")) { if (at_eof()) lex_incomplete = 1; syn("expected done"); return 0; }
            n->end = toks[ti].end; ti++;
            n->start = start; n->src = adupn(lex_src + start, n->end - start);
            return n;
        }
        if (is_kw(t, "for")) {
            ti++;
            struct tok *v = peek();
            if (!v || v->type != T_WORD) { if (!v) lex_incomplete = 1; syn("expected a variable name"); return 0; }
            struct node *n = new_node(N_FOR); n->name = v->text; ti++;
            char *ws[64]; int nw = 0; int has_in = 0;
            if (is_kw(peek(), "in")) {
                has_in = 1; ti++;
                while (peek() && peek()->type == T_WORD && nw < 63) ws[nw++] = toks[ti++].text;
            }
            n->nwords = has_in ? nw : -1;
            n->words = aalloc((u32)(nw + 1) * sizeof(char *)); memcpy(n->words, ws, (u32)nw * sizeof(char *));
            if (is_op(peek(), ";") || (peek() && peek()->type == T_NL)) { while (is_op(peek(), ";") || (peek() && peek()->type == T_NL)) ti++; }
            if (!is_kw(peek(), "do")) { if (at_eof()) lex_incomplete = 1; syn("expected do"); return 0; }
            ti++;
            static const char *const s_done[] = { "done", 0 };
            n->b = parse_list(s_done); if (!n->b) return 0;
            if (!is_kw(peek(), "done")) { if (at_eof()) lex_incomplete = 1; syn("expected done"); return 0; }
            n->end = toks[ti].end; ti++;
            n->start = start; n->src = adupn(lex_src + start, n->end - start);
            return n;
        }
        if (is_kw(t, "{")) {
            ti++;
            static const char *const s_br[] = { "}", 0 };
            struct node *n = new_node(N_GROUP);
            n->a = parse_list(s_br); if (!n->a) return 0;
            if (!is_kw(peek(), "}")) { if (at_eof()) lex_incomplete = 1; syn("expected }"); return 0; }
            n->end = toks[ti].end; ti++;
            n->start = start; n->src = adupn(lex_src + start, n->end - start);
            return n;
        }
        if (is_kw(t, "function") || (ti + 2 < ntoks && is_name_c((unsigned char)t->text[0], 1) && is_op(&toks[ti + 1], "(") && is_op(&toks[ti + 2], ")"))) {
            int kw = is_kw(t, "function");
            if (kw) ti++;
            struct tok *nm = peek();
            if (!nm || nm->type != T_WORD) { syn("expected a function name"); return 0; }
            char *name = nm->text; ti++;
            if (is_op(peek(), "(")) { ti++; if (!is_op(peek(), ")")) { syn("expected )"); return 0; } ti++; }
            skip_nl();
            if (!is_kw(peek(), "{")) { if (at_eof()) lex_incomplete = 1; syn("expected { after the function name"); return 0; }
            u32 bstart = toks[ti].start;
            struct node *body = parse_command(); if (!body) return 0;
            struct node *n = new_node(N_FUNC); n->name = name; n->a = body;
            n->src = adupn(lex_src + bstart, body->end - bstart);
            n->start = start; n->end = body->end;
            return n;
        }
    } else if (is_op(t, "(")) {
        u32 start = t->start;
        ti++;
        static const char *const s_p[] = { ")", 0 };
        struct node *n = new_node(N_SUBSH);
        n->a = parse_list(s_p); if (!n->a) return 0;
        if (!is_op(peek(), ")")) { if (at_eof()) lex_incomplete = 1; syn("expected )"); return 0; }
        n->end = toks[ti].end; ti++;
        n->start = start; n->src = adupn(lex_src + start, n->end - start);
        return n;
    }
    return parse_simple();
}

static struct node *parse_pipeline(void)
{
    int neg = 0;
    if (is_kw(peek(), "!")) { neg = 1; ti++; }
    struct node *first = parse_command(); if (!first) return 0;
    if (!is_op(peek(), "|")) { first->negate = (u8)neg; return first; }
    struct node *cmds[16]; int n = 0; cmds[n++] = first;
    while (is_op(peek(), "|")) {
        ti++; skip_nl();
        if (at_eof()) { lex_incomplete = 1; syn("expected a command after |"); return 0; }
        struct node *c = parse_command(); if (!c) return 0;
        if (n < 16) cmds[n++] = c;
    }
    struct node *p = new_node(N_PIPE);
    p->negate = (u8)neg; p->ncmds = n; p->cmds = aalloc((u32)n * sizeof(struct node *)); memcpy(p->cmds, cmds, (u32)n * sizeof(struct node *));
    p->start = first->start; p->end = cmds[n - 1]->end;
    return p;
}

static struct node *parse_andor(const char *const *stops)
{
    (void)stops;
    struct node *l = parse_pipeline(); if (!l) return 0;
    while (is_op(peek(), "&&") || is_op(peek(), "||")) {
        int and = is_op(peek(), "&&"); ti++; skip_nl();
        if (at_eof()) { lex_incomplete = 1; syn("expected a command"); return 0; }
        struct node *r = parse_pipeline(); if (!r) return 0;
        struct node *n = new_node(and ? N_AND : N_OR); n->a = l; n->b = r; l = n;
    }
    return l;
}

static struct node *parse_list(const char *const *stops)
{
    skip_nl();
    if (at_eof()) { lex_incomplete = 1; syn("unexpected end of input"); return 0; }
    if (at_stop(stops)) { syn("empty command list"); return 0; }
    struct node *first = parse_andor(stops); if (!first) return 0;
    struct node *head = 0, *tail = 0, *cur = first;
    for (;;) {
        struct tok *t = peek();
        if (is_op(t, "&")) { ti++; struct node *bg = new_node(N_BG); bg->a = cur; cur = bg; }
        else if (is_op(t, ";") || (t && t->type == T_NL)) ti++;
        else {
            if (!head) return cur;                                 /* one statement: no chain */
            struct node *w = new_node(N_SEQ); w->a = cur; tail->b = w; tail = w;
            return head;
        }
        for (;;) { t = peek(); if (is_op(t, ";") || (t && t->type == T_NL)) ti++; else break; }
        /* cur is complete: link it into the chain (right-leaning: executed by a loop, never recursion) */
        struct node *w = new_node(N_SEQ); w->a = cur;
        if (!head) head = tail = w; else { tail->b = w; tail = w; }
        if (at_eof() || at_stop(stops)) return head;
        cur = parse_andor(stops); if (!cur) return 0;
    }
}

/* ================================================================ arithmetic: $(( ... )) */
static const char *ar_p;
static long ar_or(void);
static void ar_ws(void) { while (*ar_p == ' ' || *ar_p == '\t') ar_p++; }
static long ar_atom(void)
{
    ar_ws();
    if (*ar_p == '(') { ar_p++; long v = ar_or(); ar_ws(); if (*ar_p == ')') ar_p++; return v; }
    if (*ar_p == '-') { ar_p++; return -ar_atom(); }
    if (*ar_p == '+') { ar_p++; return ar_atom(); }
    if (*ar_p == '!') { ar_p++; return !ar_atom(); }
    if (*ar_p >= '0' && *ar_p <= '9') {
        long v = 0;
        if (ar_p[0] == '0' && (ar_p[1] == 'x' || ar_p[1] == 'X')) {
            ar_p += 2;
            while ((*ar_p >= '0' && *ar_p <= '9') || (*ar_p >= 'a' && *ar_p <= 'f') || (*ar_p >= 'A' && *ar_p <= 'F')) { int d = *ar_p <= '9' ? *ar_p - '0' : (*ar_p | 32) - 'a' + 10; v = v * 16 + d; ar_p++; }
        } else while (*ar_p >= '0' && *ar_p <= '9') v = v * 10 + (*ar_p++ - '0');
        return v;
    }
    if (is_name_c((unsigned char)*ar_p, 1)) {
        char nm[40]; int k = 0;
        while (is_name_c((unsigned char)*ar_p, 0) && k < 39) nm[k++] = *ar_p++;
        nm[k] = 0;
        const char *v = var_get(nm);
        return v ? atoi(v) : 0;
    }
    return 0;
}
static long ar_mul(void)
{
    long v = ar_atom();
    for (;;) {
        ar_ws();
        if (*ar_p == '*') { ar_p++; v *= ar_atom(); }
        else if (*ar_p == '/') { ar_p++; long d = ar_atom(); v = d ? v / d : 0; }
        else if (*ar_p == '%') { ar_p++; long d = ar_atom(); v = d ? v % d : 0; }
        else return v;
    }
}
static long ar_add(void)
{
    long v = ar_mul();
    for (;;) { ar_ws(); if (*ar_p == '+') { ar_p++; v += ar_mul(); } else if (*ar_p == '-') { ar_p++; v -= ar_mul(); } else return v; }
}
static long ar_rel(void)
{
    long v = ar_add();
    for (;;) {
        ar_ws();
        if (ar_p[0] == '<' && ar_p[1] == '=') { ar_p += 2; v = v <= ar_add(); }
        else if (ar_p[0] == '>' && ar_p[1] == '=') { ar_p += 2; v = v >= ar_add(); }
        else if (ar_p[0] == '<') { ar_p++; v = v < ar_add(); }
        else if (ar_p[0] == '>') { ar_p++; v = v > ar_add(); }
        else return v;
    }
}
static long ar_eq(void)
{
    long v = ar_rel();
    for (;;) {
        ar_ws();
        if (ar_p[0] == '=' && ar_p[1] == '=') { ar_p += 2; v = v == ar_rel(); }
        else if (ar_p[0] == '!' && ar_p[1] == '=') { ar_p += 2; v = v != ar_rel(); }
        else return v;
    }
}
static long ar_and(void) { long v = ar_eq(); for (;;) { ar_ws(); if (ar_p[0] == '&' && ar_p[1] == '&') { ar_p += 2; long r = ar_eq(); v = v && r; } else return v; } }
static long ar_or(void) { long v = ar_and(); for (;;) { ar_ws(); if (ar_p[0] == '|' && ar_p[1] == '|') { ar_p += 2; long r = ar_and(); v = v || r; } else return v; } }
static long arith(const char *text) { ar_p = text; return ar_or(); }

/* ================================================================ expansion */
static int run_capture(const char *cmd, char **out);   /* $(...) */
static int run_string(const char *text);

#define FMAX 4096
static char fbuf[FMAX]; static u8 fq[FMAX]; static u32 fn; static int fhas;
static char **ex_out; static int *ex_n; static int ex_max;

static int gm(const char *p, const u8 *q, int pn, const char *s)
{
    while (pn > 0) {
        char c = *p;
        if (!*q && c == '*') {
            while (pn > 0 && *p == '*' && !*q) { p++; q++; pn--; }
            if (!pn) return 1;
            for (const char *t = s;; t++) { if (gm(p, q, pn, t)) return 1; if (!*t) break; }
            return 0;
        }
        if (!*s) return 0;
        if (!*q && c == '?') { p++; q++; pn--; s++; continue; }
        if (!*q && c == '[') {
            int j = 1, neg = 0;
            if (j < pn && (p[j] == '!' || p[j] == '^')) { neg = 1; j++; }
            int start = j, e = j;
            if (e < pn && p[e] == ']') e++;
            while (e < pn && p[e] != ']') e++;
            if (e >= pn) { if (*s != '[') return 0; p++; q++; pn--; s++; continue; }
            int match = 0;
            for (int k = start; k < e; k++) {
                if (k + 2 < e && p[k + 1] == '-') { if (*s >= p[k] && *s <= p[k + 2]) match = 1; k += 2; }
                else if (p[k] == *s) match = 1;
            }
            if (match == neg) return 0;
            p += e + 1; q += e + 1; pn -= e + 1; s++; continue;
        }
        if (c != *s) return 0;
        p++; q++; pn--; s++;
    }
    return !*s;
}

static int has_glob(const char *s, const u8 *q, u32 n)
{
    for (u32 i = 0; i < n; i++) if (!q[i] && (s[i] == '*' || s[i] == '?' || s[i] == '[')) return 1;
    return 0;
}

/* expand a wildcard pattern component by component; matches are appended to out */
static void glob_rec(const char *pat, const u8 *q, u32 n, u32 pos, const char *prefix, char **out, int *cnt, int max)
{
    while (pos < n && pat[pos] == '/') pos++;
    if (pos >= n) { if (*cnt < max && path_exists(prefix)) out[(*cnt)++] = adup(prefix); return; }
    u32 e = pos; while (e < n && pat[e] != '/') e++;
    int last = (e >= n);
    if (!has_glob(pat + pos, q + pos, e - pos)) {
        char np[300]; u32 pl = strlen(prefix);
        if (pl + (e - pos) + 3 > sizeof np) return;
        memcpy(np, prefix, pl); memcpy(np + pl, pat + pos, e - pos); np[pl + e - pos] = 0;
        if (!last) { u32 l = strlen(np); np[l] = '/'; np[l + 1] = 0; }
        glob_rec(pat, q, n, e, np, out, cnt, max);
        return;
    }
    int fd = sys_open(prefix[0] ? prefix : ".", O_RDONLY);
    if (fd < 0) return;
    struct udirent d;
    char *found[120]; int nf = 0;
    while (sys_readdir(fd, &d) > 0 && nf < 120) {
        if (d.name[0] == '.' && pat[pos] != '.') continue;
        if (!strcmp(d.name, ".") || !strcmp(d.name, "..")) continue;
        if (gm(pat + pos, q + pos, (int)(e - pos), d.name)) found[nf++] = adup(d.name);
    }
    sys_close(fd);
    sort_strs(found, nf);
    for (int i = 0; i < nf; i++) {
        char np[300]; u32 pl = strlen(prefix), fl = strlen(found[i]);
        if (pl + fl + 3 > sizeof np) continue;
        memcpy(np, prefix, pl); memcpy(np + pl, found[i], fl); np[pl + fl] = 0;
        if (!last) { if (!path_is_dir(np)) continue; np[pl + fl] = '/'; np[pl + fl + 1] = 0; }
        glob_rec(pat, q, n, e, np, out, cnt, max);
    }
}

static void field_end(void)
{
    if (!fhas) return;
    fbuf[fn] = 0;
    if (*ex_n < ex_max) {
        int before = *ex_n;
        if (has_glob(fbuf, fq, fn)) {
            char *tmp[200]; int cnt = 0;
            glob_rec(fbuf, fq, fn, 0, fbuf[0] == '/' ? "/" : "", tmp, &cnt, 200);
            if (cnt) { sort_strs(tmp, cnt); for (int i = 0; i < cnt && *ex_n < ex_max; i++) ex_out[(*ex_n)++] = tmp[i]; }
        }
        if (*ex_n == before) ex_out[(*ex_n)++] = adupn(fbuf, fn);         /* no match: the word itself */
    }
    fn = 0; fhas = 0;
}

static void fpush(char c, int quoted)
{
    if (fn < FMAX - 1) { fbuf[fn] = c; fq[fn] = (u8)quoted; fn++; }
    fhas = 1;
}

static void append_val(const char *v, int quoted)
{
    if (quoted) { for (; *v; v++) fpush(*v, 1); return; }
    const char *ifs = var_get("IFS"); if (!ifs) ifs = " \t\n";
    for (; *v; v++) {
        if (strchr(ifs, *v)) { if (fhas || fn) field_end(); }
        else fpush(*v, 0);
    }
}

static const char *special_param(char c, char *buf)
{
    if (c == '?') { utoa((u32)(last_status & 0xFF), buf); return buf; }
    if (c == '$') { utoa((u32)shell_pid, buf); return buf; }
    if (c == '#') { utoa((u32)npos, buf); return buf; }
    if (c == '!') { utoa((u32)last_bg, buf); return buf; }
    if (c == '0') return arg0;
    if (c >= '1' && c <= '9') { int k = c - '1'; return k < npos ? pos_arg[k] : ""; }
    return 0;
}

/* handles the $-expression at w[i]; returns the index after it */
static int expand_dollar(const char *w, int i, int quoted)
{
    char c = w[i + 1];
    char tmp[16];
    if (c == '(' && w[i + 2] == '(') {                          /* $(( arithmetic )) */
        int e = skip_paren(w, i);
        if (e > i + 4 && w[e - 2] == ')') {
            char *ex = adupn(w + i + 3, (u32)(e - i - 5));
            char b[16]; long v = arith(ex);
            if (v < 0) { b[0] = '-'; utoa((u32)-v, b + 1); } else utoa((u32)v, b);
            append_val(b, quoted);
            return e;
        }
    }
    if (c == '(') {
        int e = skip_paren(w, i);
        if (e < 0) { fpush('$', quoted); return i + 1; }
        char *cmd = adupn(w + i + 2, (u32)(e - i - 3)), *res = 0;
        run_capture(cmd, &res);
        append_val(res ? res : "", quoted);
        return e;
    }
    if (c == '{') {
        int e = i + 2; while (w[e] && w[e] != '}') e++;
        if (!w[e]) { fpush('$', quoted); return i + 1; }
        char *body = adupn(w + i + 2, (u32)(e - i - 2));
        int len_op = body[0] == '#';
        char *name = body + len_op; char *op = name;
        if (isdigit((unsigned char)*op)) { while (isdigit((unsigned char)*op)) op++; }
        else if (*op && strchr("?#$!@*", *op)) op++;
        else while (is_name_c((unsigned char)*op, op == name)) op++;
        char *def = 0; int mode = 0;
        if (*op == ':' && (op[1] == '-' || op[1] == '=' || op[1] == '+')) { mode = op[1]; def = op + 2; }
        else if (*op == '-' || *op == '+') { mode = *op; def = op + 1; }
        char saved = *op; *op = 0;
        const char *v = var_get(name);
        if (!v && !name[1]) v = special_param(name[0], tmp);
        *op = saved;
        if (len_op) { char b[12]; utoa(v ? strlen(v) : 0, b); append_val(b, quoted); }
        else if (mode == '-') append_val((v && *v) ? v : def, quoted);
        else if (mode == '=') { if (!v || !*v) { *op = 0; var_set(name, def, -1); *op = saved; v = def; } append_val(v, quoted); }
        else if (mode == '+') append_val((v && *v) ? def : "", quoted);
        else append_val(v ? v : "", quoted);
        return e + 1;
    }
    if (c == '@' || c == '*') {
        if (quoted && c == '@') { for (int k = 0; k < npos; k++) { append_val(pos_arg[k], 1); if (k + 1 < npos) field_end(); } }
        else for (int k = 0; k < npos; k++) { append_val(pos_arg[k], quoted); if (k + 1 < npos) { if (quoted) fpush(' ', 1); else field_end(); } }
        return i + 2;
    }
    if (c == '?' || c == '$' || c == '#' || c == '!' || (c >= '0' && c <= '9')) {
        append_val(special_param(c, tmp), quoted);
        return i + 2;
    }
    if (is_name_c((unsigned char)c, 1)) {
        int e = i + 1; while (is_name_c((unsigned char)w[e], 0)) e++;
        char *name = adupn(w + i + 1, (u32)(e - i - 1));
        const char *v = var_get(name);
        append_val(v ? v : "", quoted);
        return e;
    }
    fpush('$', quoted);
    return i + 1;
}

/* expand one raw word into zero or more fields */
static void expand_word(const char *w, char **out, int *n, int max)
{
    ex_out = out; ex_n = n; ex_max = max; fn = 0; fhas = 0;
    int i = 0;
    if (w[0] == '~' && (w[1] == '/' || !w[1])) {
        const char *h = var_get("HOME"); if (!h) h = "/";
        for (; *h; h++) fpush(*h, 1);
        i = 1;
    }
    while (w[i]) {
        char c = w[i];
        if (c == '\\') { if (w[i + 1]) { fpush(w[i + 1], 1); i += 2; } else i++; continue; }
        if (c == '\'') { i++; fhas = 1; while (w[i] && w[i] != '\'') fpush(w[i++], 1); if (w[i]) i++; continue; }
        if (c == '"') {
            i++; fhas = 1;
            while (w[i] && w[i] != '"') {
                if (w[i] == '\\' && (w[i + 1] == '$' || w[i + 1] == '`' || w[i + 1] == '"' || w[i + 1] == '\\')) { fpush(w[i + 1], 1); i += 2; }
                else if (w[i] == '$') i = expand_dollar(w, i, 1);
                else if (w[i] == '`') {
                    int e = i + 1; while (w[e] && w[e] != '`') e++;
                    char *cmd = adupn(w + i + 1, (u32)(e - i - 1)), *res = 0;
                    run_capture(cmd, &res); append_val(res ? res : "", 1);
                    i = w[e] ? e + 1 : e;
                } else fpush(w[i++], 1);
            }
            if (w[i]) i++;
            continue;
        }
        if (c == '`') {
            int e = i + 1; while (w[e] && w[e] != '`') e++;
            char *cmd = adupn(w + i + 1, (u32)(e - i - 1)), *res = 0;
            run_capture(cmd, &res); append_val(res ? res : "", 0);
            i = w[e] ? e + 1 : e;
            continue;
        }
        if (c == '$') { i = expand_dollar(w, i, 0); continue; }
        fpush(c, 0); i++;
    }
    field_end();
}

/* expand without splitting or globbing (assignments, redirection targets, here-documents) */
static char *expand_plain(const char *w)
{
    static char out[FMAX];
    u32 olen = 0;
    char *flds[64]; int nf = 0;
    /* reuse the quote-aware expander but join the fields with a space and ignore globbing results */
    char *save_ifs = 0; (void)save_ifs;
    expand_word(w, flds, &nf, 64);
    for (int k = 0; k < nf; k++) {
        u32 l = strlen(flds[k]);
        if (olen + l + 2 >= sizeof out) break;
        memcpy(out + olen, flds[k], l); olen += l;
        if (k + 1 < nf) out[olen++] = ' ';
    }
    out[olen] = 0;
    return adup(out);
}

/* here-document bodies: $-expansion, no quote processing */
static char *expand_heredoc(const char *body)
{
    static char out[FMAX];
    u32 olen = 0;
    for (int i = 0; body[i] && olen < FMAX - 8;) {
        if (body[i] == '\\' && (body[i + 1] == '$' || body[i + 1] == '\\' || body[i + 1] == '`')) { out[olen++] = body[i + 1]; i += 2; continue; }
        if (body[i] == '$') {
            char tmp[16]; const char *v = 0; int e = i;
            char c = body[i + 1];
            if (c == '{') { e = i + 2; while (body[e] && body[e] != '}') e++; char *nm = adupn(body + i + 2, (u32)(e - i - 2)); v = var_get(nm); if (!v && !nm[1]) v = special_param(nm[0], tmp); e = body[e] ? e + 1 : e; }
            else if (c == '?' || c == '$' || c == '#' || c == '!' || (c >= '0' && c <= '9')) { v = special_param(c, tmp); e = i + 2; }
            else if (is_name_c((unsigned char)c, 1)) { e = i + 1; while (is_name_c((unsigned char)body[e], 0)) e++; char *nm = adupn(body + i + 1, (u32)(e - i - 1)); v = var_get(nm); }
            else { out[olen++] = '$'; i++; continue; }
            if (v) { u32 l = strlen(v); if (olen + l < FMAX - 8) { memcpy(out + olen, v, l); olen += l; } }
            i = e; continue;
        }
        out[olen++] = body[i++];
    }
    out[olen] = 0;
    return adup(out);
}

/* ================================================================ running commands */
static int exec_node(struct node *n);
static int env_dirty = 1;

#define MAXJOBS 16
static struct job { int pid; char *cmd; u8 used, done; int status; } jobs[MAXJOBS];
static int add_job(int pid, const char *cmd)
{
    for (int i = 0; i < MAXJOBS; i++) if (!jobs[i].used) { jobs[i].used = 1; jobs[i].done = 0; jobs[i].pid = pid; jobs[i].cmd = pdup(cmd); return i + 1; }
    return 0;
}
static void reap_jobs(int announce)
{
    for (int i = 0; i < MAXJOBS; i++) {
        if (!jobs[i].used) continue;
        if (!jobs[i].done) {
            int r = sys_waitnb(jobs[i].pid);
            if (r == -2) continue;
            jobs[i].done = 1; jobs[i].status = r;
        }
        if (announce) {
            char b[16]; outs("["); utoa((u32)i + 1, b); outs(b); outs("]  Done      "); outs(jobs[i].cmd); outs("\n");
            free(jobs[i].cmd); jobs[i].used = 0;
        }
    }
}

/* ---- redirections: applied to the shell's own fds 0-2, then undone ---- */
struct rsave { int saved[3]; char *tmpfiles[4]; int ntmp; };

static void rs_init(struct rsave *r) { r->saved[0] = r->saved[1] = r->saved[2] = -1; r->ntmp = 0; }
static void rs_save(struct rsave *r, int fd) { if (fd >= 0 && fd < 3 && r->saved[fd] < 0) r->saved[fd] = sys_dup(fd); }
static void rs_restore(struct rsave *r)
{
    for (int fd = 0; fd < 3; fd++)
        if (r->saved[fd] >= 0) { sys_dup2(r->saved[fd], fd); sys_close(r->saved[fd]); r->saved[fd] = -1; }
    for (int i = 0; i < r->ntmp; i++) sys_unlink(r->tmpfiles[i]);
    r->ntmp = 0;
}

static int apply_redirs(struct redir *r, struct rsave *rs)
{
    static int hd_seq;
    for (; r; r = r->next) {
        int fd;
        if (r->fd > 2 && r->kind != 3) { errm("only fds 0, 1 and 2 can be redirected", 0); return 1; }
        if (r->kind == 5) {                                   /* here-document: a temporary file */
            char name[40] = "/tmp/.sh"; char b[12];
            utoa((u32)shell_pid, b); strcpy(name + strlen(name), b); u32 l = strlen(name); name[l++] = '_'; utoa((u32)hd_seq++, name + l);
            fd = sys_open(name, O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0) { errm("cannot create a here-document file in /tmp", 0); return 1; }
            const char *body = r->hdquoted ? r->body : expand_heredoc(r->body);
            sys_write(fd, body, strlen(body)); sys_close(fd);
            fd = sys_open(name, O_RDONLY);
            if (rs->ntmp < 4) rs->tmpfiles[rs->ntmp++] = adup(name);
            rs_save(rs, 0); sys_dup2(fd, 0); sys_close(fd);
            continue;
        }
        char *target = expand_plain(r->word);
        if (r->kind == 3) {                                   /* N>&M */
            if (!strcmp(target, "-")) { rs_save(rs, r->fd); continue; }
            int m = atoi(target);
            if (m < 0 || m > 9) { errm("bad file descriptor", target); return 1; }
            rs_save(rs, r->fd); if (sys_dup2(m, r->fd) < 0) { errm("bad file descriptor", target); return 1; }
            continue;
        }
        int flags = r->kind == 0 ? O_RDONLY : r->kind == 2 ? (O_WRONLY | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC);
        fd = sys_open(target, flags);
        if (fd < 0) { errm(target, r->kind == 0 ? "cannot open" : "cannot write"); return 1; }
        if (r->kind == 4) { rs_save(rs, 1); rs_save(rs, 2); sys_dup2(fd, 1); sys_dup2(fd, 2); }
        else { rs_save(rs, r->fd); sys_dup2(fd, r->fd); }
        sys_close(fd);
    }
    return 0;
}

/* ---- finding and starting programs ---- */
static int find_program(const char *name, char *out, u32 max)
{
    if (strchr(name, '/')) { if (strlen(name) >= max) return 0; strcpy(out, name); return path_is_file(out); }
    const char *path = var_get("PATH"); if (!path) path = "/bin";
    const char *p = path;
    while (*p) {
        const char *e = p; while (*e && *e != ':') e++;
        u32 l = (u32)(e - p);
        if (l + strlen(name) + 2 < max) {
            memcpy(out, p, l); if (l && out[l - 1] != '/') out[l++] = '/'; strcpy(out + l, name);
            if (path_is_file(out)) return 1;
        }
        p = *e ? e + 1 : e;
    }
    return 0;
}

static int start_program(char **argv)
{
    char path[160];
    if (!find_program(argv[0], path, sizeof path)) { errm(argv[0], "command not found"); return -127; }
    if (env_dirty) { env_sync(); env_dirty = 0; }
    int pid = sys_spawn(path, argv, 0);
    if (pid < 0) { errm(argv[0], "cannot run"); return -126; }
    return pid;
}

static int wait_fg(int pid, const char *name)
{
    sys_setfg(pid);
    int st = sys_wait(pid);
    sys_setfg(0);
    if (st == 139) { errs("["); errs(name); errs(" crashed]\n"); }
    return st;
}

/* ---- builtins ---- */
static int is_builtin(const char *n);
static int run_builtin(char **argv, int argc);
static int run_function(struct func *f, char **argv, int argc);

static int run_words(char **argv, int argc, int *pidout, int background)
{
    if (argc == 0) return 0;
    /* alias on the first word (once) */
    struct alias *al = alias_find(argv[0]);
    if (al && !background) {
        char **nv = aalloc(64 * sizeof(char *)); int nn = 0;
        char *v = adup(al->val); char *p = v;
        while (*p && nn < 40) { while (is_space(*p)) p++; if (!*p) break; nv[nn++] = p; while (*p && !is_space(*p)) p++; if (*p) *p++ = 0; }
        for (int i = 1; i < argc && nn < 63; i++) nv[nn++] = argv[i];
        if (nn && strcmp(nv[0], argv[0])) { nv[nn] = 0; return run_words(nv, nn, pidout, background); }
    }
    struct func *f = func_find(argv[0]);
    if (f) return run_function(f, argv, argc);
    if (is_builtin(argv[0])) return run_builtin(argv, argc);
    int pid = start_program(argv);
    if (pid < 0) return -pid;
    if (pidout) *pidout = pid;
    if (background) return 0;
    return wait_fg(pid, argv[0]);
}

static int expand_command(struct node *n, char **argv, int max)
{
    int argc = 0;
    for (int i = 0; i < n->nwords && argc < max - 1; i++) expand_word(n->words[i], argv, &argc, max - 1);
    argv[argc] = 0;
    return argc;
}

static int run_simple(struct node *n, int background, int *pidout)
{
    char *mark = abrk;                                      /* released when the command is done */
    char **argv = aalloc(96 * sizeof(char *)); int argc = expand_command(n, argv, 96);
    struct rsave rs; rs_init(&rs);
    int st = 0;
    /* assignments */
    char **saved_names = aalloc(24 * sizeof(char *)); char **saved_vals = aalloc(24 * sizeof(char *)); int *saved_had = aalloc(24 * sizeof(int));
    for (int i = 0; i < n->nassign; i++) {
        char *eq = strchr(n->assign[i], '='); *eq = 0;
        char *val = expand_plain(eq + 1);
        if (argc) {                                        /* NAME=v command: only for this command */
            const char *old = var_get(n->assign[i]);
            saved_names[i] = adup(n->assign[i]); saved_had[i] = old != 0; saved_vals[i] = old ? adup(old) : 0;
            var_set(n->assign[i], val, 1);
        } else var_set(n->assign[i], val, -1);
        *eq = '=';
        env_dirty = 1;
    }
    if (n->redirs && apply_redirs(n->redirs, &rs)) { rs_restore(&rs); abrk = mark; return 1; }
    if (argc) st = run_words(argv, argc, pidout, background);
    rs_restore(&rs);
    if (argc) for (int i = 0; i < n->nassign; i++) { if (saved_had[i]) var_set(saved_names[i], saved_vals[i], -1); else var_unset(saved_names[i]); env_dirty = 1; }
    if (!background && !pidout) abrk = mark;
    return st;
}

/* ---- pipelines: each stage is a separate process ---- */
static int spawn_stage(struct node *c, int background_text_only)
{
    (void)background_text_only;
    if (c->type == N_SIMPLE && !c->nassign) {
        char **argv = aalloc(96 * sizeof(char *)); int argc = expand_command(c, argv, 96);
        if (argc && !func_find(argv[0]) && !is_builtin(argv[0]) && !alias_find(argv[0])) {
            struct rsave rs; rs_init(&rs);
            if (c->redirs && apply_redirs(c->redirs, &rs)) { rs_restore(&rs); return -1; }
            int pid = start_program(argv);
            rs_restore(&rs);
            return pid < 0 ? -1 : pid;
        }
    }
    /* builtins, functions and compound commands run in a child shell */
    char *argv[5] = { "sh", "-c", c->src ? c->src : (char *)"true", 0, 0 };
    if (env_dirty) { env_sync(); env_dirty = 0; }
    return sys_spawn("/bin/sh", argv, 0);
}

static int run_pipeline(struct node *p, int background, int *pidout)
{
    int pids[16], np = 0, prev = -1;
    struct rsave rs; rs_init(&rs);
    for (int i = 0; i < p->ncmds; i++) {
        int pp[2] = { -1, -1 };
        if (i < p->ncmds - 1 && sys_pipe(pp) < 0) { errm("cannot make a pipe", 0); break; }
        rs_save(&rs, 0); rs_save(&rs, 1);
        if (prev >= 0) sys_dup2(prev, 0);
        if (pp[1] >= 0) sys_dup2(pp[1], 1);
        if (pp[1] >= 0) sys_close(pp[1]);
        if (prev >= 0) sys_close(prev);
        int pid = spawn_stage(p->cmds[i], 0);
        rs_restore(&rs);
        prev = pp[0];
        if (pid >= 0) pids[np++] = pid;
    }
    if (prev >= 0) sys_close(prev);
    if (np == 0) return 1;
    if (pidout) *pidout = pids[np - 1];
    if (background) return 0;
    sys_setfg(pids[np - 1]);
    int st = 0;
    for (int i = 0; i < np; i++) { int s = sys_wait(pids[i]); if (i == np - 1) st = s; }
    sys_setfg(0);
    return st;
}

static int run_function(struct func *f, char **argv, int argc)
{
    char *save_pos[24]; int save_n = npos;
    memcpy(save_pos, pos_arg, sizeof save_pos);
    npos = argc - 1 < 23 ? argc - 1 : 23;
    for (int i = 0; i < npos; i++) pos_arg[i] = argv[i + 1];
    in_func++; func_return = 0;
    char *saved_arena = abrk;
    int st = run_string(f->body);
    (void)saved_arena;
    if (func_return) { st = func_return - 1; func_return = 0; }
    in_func--;
    npos = save_n; memcpy(pos_arg, save_pos, sizeof save_pos);
    return st;
}

static int interrupted;
static int check_intr(void)
{
    if (!interactive) return 0;
    if (sys_ttyintr()) interrupted = 1;
    return interrupted;
}

static int cond_true(struct node *c) { return exec_node(c) == 0; }

static int exec_node(struct node *n)
{
    if (!n) return 0;
    int st = 0;
    if (exit_requested) return last_status;
    if (check_intr()) return 130;
    switch (n->type) {
    case N_SIMPLE: st = run_simple(n, 0, 0); break;
    case N_PIPE: st = run_pipeline(n, 0, 0); break;
    case N_AND: st = exec_node(n->a); if (st == 0 && !exit_requested) st = exec_node(n->b); break;
    case N_OR: st = exec_node(n->a); if (st != 0 && !exit_requested) st = exec_node(n->b); break;
    case N_SEQ: {
        for (struct node *c = n; c; c = c->b) {
            st = exec_node(c->a);
            if (exit_requested || loop_break || loop_continue || func_return || interrupted) break;
        }
        break;
    }
    case N_BG: {
        struct node *a = n->a; int pid = 0;
        if (a->type == N_SIMPLE && !a->nassign) {
            char **argv = aalloc(96 * sizeof(char *)); int argc = expand_command(a, argv, 96);
            struct rsave rs; rs_init(&rs);
            if (argc && !func_find(argv[0]) && !is_builtin(argv[0]) && !alias_find(argv[0])) {
                if (!a->redirs || !apply_redirs(a->redirs, &rs)) { pid = start_program(argv); if (pid < 0) pid = 0; }
                rs_restore(&rs);
            } else pid = spawn_stage(a, 1);
        } else if (a->type == N_PIPE) { run_pipeline(a, 1, &pid); }
        else pid = spawn_stage(a, 1);
        if (pid > 0) {
            last_bg = pid;
            int j = add_job(pid, a->src ? a->src : "job");
            if (interactive) { char b[16]; outs("["); utoa((u32)j, b); outs(b); outs("] "); utoa((u32)pid, b); outs(b); outs("\n"); }
        }
        st = 0; break;
    }
    case N_IF: {
        struct node *cur = n; st = 0;
        for (;;) {
            if (cond_true(cur->a)) { st = exec_node(cur->b); break; }
            struct node *nx = cur->c;
            if (!nx) { st = 0; break; }
            if (nx->type == N_IF && nx->until) { cur = nx; continue; }
            st = exec_node(nx);
            break;
        }
        break;
    }
    case N_WHILE:
        st = 0;
        for (;;) {
            int c = exec_node(n->a);
            if (exit_requested) break;
            if (n->until ? c == 0 : c != 0) break;
            st = exec_node(n->b);
            if (loop_break) { loop_break--; break; }
            if (loop_continue) { loop_continue--; continue; }
            if (func_return) break;
        }
        break;
    case N_FOR: {
        char **items = aalloc(160 * sizeof(char *)); int ni = 0;
        if (n->nwords < 0) { for (int i = 0; i < npos && ni < 159; i++) items[ni++] = pos_arg[i]; }
        else for (int i = 0; i < n->nwords; i++) expand_word(n->words[i], items, &ni, 159);
        st = 0;
        for (int i = 0; i < ni; i++) {
            var_set(n->name, items[i], -1);
            st = exec_node(n->b);
            if (exit_requested) break;
            if (loop_break) { loop_break--; break; }
            if (loop_continue) { loop_continue--; continue; }
            if (func_return) break;
        }
        break;
    }
    case N_GROUP: st = exec_node(n->a); break;
    case N_SUBSH: {
        char *inner = adupn(n->src + 1, strlen(n->src) - 2);
        char *argv[5] = { "sh", "-c", inner, 0, 0 };
        if (env_dirty) { env_sync(); env_dirty = 0; }
        int pid = sys_spawn("/bin/sh", argv, 0);
        st = pid < 0 ? 126 : wait_fg(pid, "subshell");
        break;
    }
    case N_FUNC: {
        struct func *f = func_find(n->name);
        if (!f) for (int i = 0; i < 48; i++) if (!funcs[i].used) { f = &funcs[i]; f->used = 1; f->name = pdup(n->name); f->body = 0; break; }
        if (f) { if (f->body) free(f->body); f->body = pdup(n->src); }
        st = 0; break;
    }
    }
    if (n->negate) st = st == 0;
    last_status = st;
    return st;
}

/* the redirections after a compound command ("done < file") are not supported yet; they work on simple commands */

/* ---- command substitution and scripts ---- */
static int run_capture(const char *cmd, char **out)
{
    int pp[2];
    *out = 0;
    if (sys_pipe(pp) < 0) return 1;
    struct rsave rs; rs_init(&rs);
    rs_save(&rs, 1);
    sys_dup2(pp[1], 1); sys_close(pp[1]);
    char *argv[5] = { "sh", "-c", (char *)cmd, 0, 0 };
    if (env_dirty) { env_sync(); env_dirty = 0; }
    int pid = sys_spawn("/bin/sh", argv, 0);
    rs_restore(&rs);
    if (pid < 0) { sys_close(pp[0]); return 126; }
    u32 cap = 512, len = 0; char *b = aalloc(cap);
    for (;;) {
        if (len + 256 > cap) { char *nb = aalloc(cap * 2); memcpy(nb, b, len); b = nb; cap *= 2; }
        int n = sys_read(pp[0], b + len, 256);
        if (n <= 0) break;
        len += (u32)n;
    }
    sys_close(pp[0]);
    int st = sys_wait(pid);
    while (len && b[len - 1] == '\n') len--;
    b[len] = 0; *out = b;
    return st;
}

static int run_string(const char *text)
{
    struct node *tree;
    int rc = lex(text);
    if (rc == 1) { errm("unexpected end of input", 0); return 2; }
    if (rc) { errm("syntax error", 0); return 2; }
    if (!ntoks) return last_status;
    perr = 0; lex_incomplete = 0;
    tree = parse_list(0);
    if (!tree) { errm(perr ? perr : "syntax error", 0); return 2; }
    if (ti < ntoks) { errm("syntax error near unexpected token", 0); return 2; }
    int st = exec_node(tree);
    return st;
}

/* ================================================================ builtins */
static const char *const bnames[] = {
    "cd", "exit", "export", "unset", "set", "alias", "unalias", "history", "source", ".", "read", "shift",
    "return", "break", "continue", "jobs", "wait", "kill", "type", "test", "[", "true", "false", ":", "echo",
    "pwd", "help", "local", "eval", 0 };
static int is_builtin(const char *n) { for (int i = 0; bnames[i]; i++) if (!strcmp(bnames[i], n)) return 1; return 0; }
static int builtin_names(const char **out, int max) { int n = 0; while (bnames[n] && n < max) { out[n] = bnames[n]; n++; } return n; }

static const char *cwd_now(void) { static char b[128]; if (sys_getcwd(b, sizeof b) < 0) strcpy(b, "/"); return b; }

static void set_pwd(const char *old)
{
    if (old) var_set("OLDPWD", old, 1);
    var_set("PWD", cwd_now(), 1);
    env_dirty = 1;
}

/* ---- test / [ ---- */
static char **t_a; static int t_n, t_i;
static int t_expr(void);
static int t_isunary(const char *s) { return s[0] == '-' && s[1] && !s[2] && strchr("efdrwxsznLh", s[1]); }
static int t_isbin(const char *s) { return !strcmp(s, "=") || !strcmp(s, "==") || !strcmp(s, "!=") || !strcmp(s, "-eq") || !strcmp(s, "-ne") || !strcmp(s, "-lt") || !strcmp(s, "-le") || !strcmp(s, "-gt") || !strcmp(s, "-ge"); }
static int t_primary(void)
{
    if (t_i >= t_n) return 0;
    if (!strcmp(t_a[t_i], "(")) { t_i++; int v = t_expr(); if (t_i < t_n && !strcmp(t_a[t_i], ")")) t_i++; return v; }
    if (t_i + 2 < t_n + 0 && t_isbin(t_a[t_i + 1]) && !(t_i + 1 >= t_n)) {
        const char *a = t_a[t_i], *op = t_a[t_i + 1], *b = t_a[t_i + 2]; t_i += 3;
        if (!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(a, b);
        if (!strcmp(op, "!=")) return strcmp(a, b) != 0;
        int x = atoi(a), y = atoi(b);
        return !strcmp(op, "-eq") ? x == y : !strcmp(op, "-ne") ? x != y : !strcmp(op, "-lt") ? x < y : !strcmp(op, "-le") ? x <= y : !strcmp(op, "-gt") ? x > y : x >= y;
    }
    if (t_i + 1 < t_n && t_isunary(t_a[t_i])) {
        char o = t_a[t_i][1]; const char *a = t_a[t_i + 1]; t_i += 2;
        struct ustat st;
        switch (o) {
        case 'e': return sys_stat(a, &st) == 0;
        case 'f': return sys_stat(a, &st) == 0 && (st.mode & S_IFMT) == S_IFREG;
        case 'd': return sys_stat(a, &st) == 0 && (st.mode & S_IFMT) == S_IFDIR;
        case 'r': return sys_stat(a, &st) == 0 && (st.mode & 0444);
        case 'w': return sys_stat(a, &st) == 0 && (st.mode & 0222);
        case 'x': return sys_stat(a, &st) == 0 && (st.mode & 0111);
        case 'L': case 'h': return sys_lstat(a, &st) == 0 && (st.mode & S_IFMT) == S_IFLNK;
        case 's': return sys_stat(a, &st) == 0 && st.size > 0;
        case 'z': return *a == 0;
        case 'n': return *a != 0;
        }
    }
    return t_a[t_i++][0] != 0;                         /* a lone string is true when it is not empty */
}
static int t_not(void) { if (t_i < t_n && !strcmp(t_a[t_i], "!")) { t_i++; return !t_not(); } return t_primary(); }
static int t_and(void) { int v = t_not(); while (t_i < t_n && !strcmp(t_a[t_i], "-a")) { t_i++; int r = t_not(); v = v && r; } return v; }
static int t_expr(void) { int v = t_and(); while (t_i < t_n && !strcmp(t_a[t_i], "-o")) { t_i++; int r = t_and(); v = v || r; } return v; }

static int do_test(char **argv, int argc)
{
    int n = argc - 1;
    if (!strcmp(argv[0], "[")) { if (n < 1 || strcmp(argv[argc - 1], "]")) { errm("[: missing ]", 0); return 2; } n--; }
    t_a = argv + 1; t_n = n; t_i = 0;
    if (n == 0) return 1;
    return t_expr() ? 0 : 1;
}

static int strspn_ok(const char *s) { for (; *s; s++) if (!strchr("neE", *s)) return 0; return 1; }

static int do_echo(char **argv, int argc)
{
    int nl = 1, esc = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1] && strspn_ok(argv[i] + 1); i++) {
        if (strchr(argv[i], 'n')) nl = 0;
        if (strchr(argv[i], 'e')) esc = 1;
    }
    for (int first = 1; i < argc; i++, first = 0) {
        if (!first) outs(" ");
        if (!esc) { outs(argv[i]); continue; }
        for (const char *p = argv[i]; *p; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'n') outs("\n"); else if (*p == 't') outs("\t"); else if (*p == 'e' || *p == 'E') outs("\033");
                else if (*p == 'r') outs("\r"); else if (*p == 'a') outs("\a"); else if (*p == 'c') return 0;
                else if (*p == '\\') outs("\\"); else { char b[3] = { '\\', *p, 0 }; outs(b); }
            } else { char b[2] = { *p, 0 }; outs(b); }
        }
    }
    if (nl) outs("\n");
    return 0;
}

static int do_read(char **argv, int argc)
{
    char line[512]; int n = 0; int eof = 0;
    int vi = 1;
    while (vi < argc && argv[vi][0] == '-') {
        if (!strcmp(argv[vi], "-p") && vi + 1 < argc) { outs(argv[vi + 1]); vi += 2; } else vi++;
    }
    for (;;) {
        char c;
        int r = sys_read(0, &c, 1);
        if (r <= 0) { eof = 1; break; }
        if (c == '\n') break;
        if (n < (int)sizeof line - 1) line[n++] = c;
    }
    line[n] = 0;
    if (vi >= argc) { var_set("REPLY", line, -1); return eof && !n; }
    const char *ifs = var_get("IFS"); if (!ifs) ifs = " \t\n";
    char *p = line;
    for (int k = vi; k < argc; k++) {
        while (*p && strchr(ifs, *p)) p++;
        if (k == argc - 1) { var_set(argv[k], p, -1); break; }
        char *e = p; while (*e && !strchr(ifs, *e)) e++;
        char sv = *e; *e = 0; var_set(argv[k], p, -1); *e = sv;
        p = e;
    }
    return eof && !n;
}

static int do_type(const char *n)
{
    char path[160];
    if (alias_find(n)) { outs(n); outs(" is an alias\n"); return 0; }
    if (func_find(n)) { outs(n); outs(" is a function\n"); return 0; }
    if (is_builtin(n)) { outs(n); outs(" is a shell builtin\n"); return 0; }
    if (find_program(n, path, sizeof path)) { outs(n); outs(" is "); outs(path); outs("\n"); return 0; }
    errm(n, "not found");
    return 1;
}

static int job_pid(const char *a)
{
    if (a[0] == '%') { int j = atoi(a + 1); if (j >= 1 && j <= MAXJOBS && jobs[j - 1].used) return jobs[j - 1].pid; return -1; }
    return atoi(a);
}

static int run_builtin(char **argv, int argc)
{
    const char *n = argv[0];
    if (!strcmp(n, "echo")) return do_echo(argv, argc);
    if (!strcmp(n, "true") || !strcmp(n, ":")) return 0;
    if (!strcmp(n, "false")) return 1;
    if (!strcmp(n, "pwd")) { outs(cwd_now()); outs("\n"); return 0; }
    if (!strcmp(n, "test") || !strcmp(n, "[")) return do_test(argv, argc);
    if (!strcmp(n, "read")) return do_read(argv, argc);
    if (!strcmp(n, "cd")) {
        const char *d = argc > 1 ? argv[1] : var_get("HOME");
        char old[128]; strcpy(old, cwd_now());
        if (d && !strcmp(d, "-")) { d = var_get("OLDPWD"); if (d) { outs(d); outs("\n"); } }
        if (!d) { errm("cd", "no directory"); return 1; }
        if (sys_chdir(d) < 0) { errm(d, "no such directory"); return 1; }
        set_pwd(old); return 0;
    }
    if (!strcmp(n, "exit")) { last_status = argc > 1 ? atoi(argv[1]) : last_status; exit_requested = 1; return last_status; }
    if (!strcmp(n, "export") || !strcmp(n, "local")) {
        int is_export = n[0] == 'e';
        if (argc == 1) { for (int i = 0; i < 128; i++) if (vars[i].used && vars[i].exp) { outs("export "); outs(vars[i].name); outs("=\""); outs(vars[i].val); outs("\"\n"); } return 0; }
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (eq) { *eq = 0; var_set(argv[i], eq + 1, is_export ? 1 : -1); *eq = '='; }
            else if (is_export) { const char *v = var_get(argv[i]); var_set(argv[i], v ? v : "", 1); }
        }
        env_dirty = 1; return 0;
    }
    if (!strcmp(n, "unset")) { for (int i = 1; i < argc; i++) var_unset(argv[i]); env_dirty = 1; return 0; }
    if (!strcmp(n, "set")) {
        if (argc == 1) { for (int i = 0; i < 128; i++) if (vars[i].used) { outs(vars[i].name); outs("="); outs(vars[i].val); outs("\n"); } return 0; }
        int i = 1;
        while (i < argc && argv[i][0] == '-' && argv[i][1] && strcmp(argv[i], "--")) i++;       /* -e -x ... accepted, ignored */
        if (i < argc && !strcmp(argv[i], "--")) i++;
        if (i < argc || (argc > 1 && !strcmp(argv[1], "--"))) { npos = 0; for (; i < argc && npos < 23; i++) pos_arg[npos++] = adup(argv[i]); }
        return 0;
    }
    if (!strcmp(n, "shift")) { int k = argc > 1 ? atoi(argv[1]) : 1; if (k > npos) return 1; memmove(pos_arg, pos_arg + k, (u32)(npos - k) * sizeof(char *)); npos -= k; return 0; }
    if (!strcmp(n, "alias")) {
        if (argc == 1) { for (int i = 0; i < 48; i++) if (aliases[i].used) { outs("alias "); outs(aliases[i].name); outs("='"); outs(aliases[i].val); outs("'\n"); } return 0; }
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq) { struct alias *a = alias_find(argv[i]); if (a) { outs("alias "); outs(a->name); outs("='"); outs(a->val); outs("'\n"); } else errm(argv[i], "alias not found"); continue; }
            *eq = 0;
            struct alias *a = alias_find(argv[i]);
            if (!a) for (int k = 0; k < 48; k++) if (!aliases[k].used) { a = &aliases[k]; a->used = 1; a->name = pdup(argv[i]); a->val = 0; break; }
            if (a) { if (a->val) free(a->val); a->val = pdup(eq + 1); }
            *eq = '=';
        }
        return 0;
    }
    if (!strcmp(n, "unalias")) { for (int i = 1; i < argc; i++) { struct alias *a = alias_find(argv[i]); if (a) a->used = 0; } return 0; }
    if (!strcmp(n, "history")) {
        if (argc > 1 && !strcmp(argv[1], "-c")) { for (int i = 0; i < nhist; i++) free(hist[i]); nhist = 0; return 0; }
        for (int i = 0; i < nhist; i++) { char b[12]; utoa((u32)i + 1, b); for (u32 k = strlen(b); k < 5; k++) outs(" "); outs(b); outs("  "); outs(hist[i]); outs("\n"); }
        return 0;
    }
    if (!strcmp(n, "source") || !strcmp(n, ".")) {
        if (argc < 2) { errm(n, "file name needed"); return 2; }
        char *text = file_slurp(argv[1], 0);
        if (!text) { errm(argv[1], "cannot read"); return 1; }
        char *save_pos[24]; int save_n = npos;
        if (argc > 2) { memcpy(save_pos, pos_arg, sizeof save_pos); npos = 0; for (int i = 2; i < argc && npos < 23; i++) pos_arg[npos++] = argv[i]; }
        int st = run_string(text);
        if (argc > 2) { npos = save_n; memcpy(pos_arg, save_pos, sizeof save_pos); }
        return st;
    }
    if (!strcmp(n, "eval")) {
        u32 len = 0; for (int i = 1; i < argc; i++) len += strlen(argv[i]) + 1;
        char *s = aalloc(len + 1); u32 o = 0;
        for (int i = 1; i < argc; i++) { u32 l = strlen(argv[i]); memcpy(s + o, argv[i], l); o += l; s[o++] = ' '; }
        s[o] = 0; return run_string(s);
    }
    if (!strcmp(n, "return")) { if (!in_func) { errm("return", "not in a function"); return 1; } func_return = (argc > 1 ? atoi(argv[1]) : last_status) + 1; return func_return - 1; }
    if (!strcmp(n, "break")) { loop_break = argc > 1 ? atoi(argv[1]) : 1; return 0; }
    if (!strcmp(n, "continue")) { loop_continue = argc > 1 ? atoi(argv[1]) : 1; return 0; }
    if (!strcmp(n, "jobs")) {
        reap_jobs(0);
        for (int i = 0; i < MAXJOBS; i++) if (jobs[i].used) { char b[16]; outs("["); utoa((u32)i + 1, b); outs(b); outs("]  "); outs(jobs[i].done ? "Done      " : "Running   "); outs(jobs[i].cmd); outs("\n"); if (jobs[i].done) { free(jobs[i].cmd); jobs[i].used = 0; } }
        return 0;
    }
    if (!strcmp(n, "wait")) {
        if (argc > 1) { int pid = job_pid(argv[1]); if (pid <= 0) return 127; int st = sys_wait(pid); for (int i = 0; i < MAXJOBS; i++) if (jobs[i].used && jobs[i].pid == pid) { free(jobs[i].cmd); jobs[i].used = 0; } return st; }
        for (int i = 0; i < MAXJOBS; i++) if (jobs[i].used) { if (!jobs[i].done) sys_wait(jobs[i].pid); free(jobs[i].cmd); jobs[i].used = 0; }
        return 0;
    }
    if (!strcmp(n, "kill")) {
        int rc = 0;
        for (int i = 1; i < argc; i++) { if (argv[i][0] == '-') continue; int pid = job_pid(argv[i]); if (pid <= 0 || sys_kill(pid) < 0) { errm(argv[i], "no such process"); rc = 1; } }
        return rc;
    }
    if (!strcmp(n, "type")) { int rc = 0; for (int i = 1; i < argc; i++) rc |= do_type(argv[i]); return rc; }
    if (!strcmp(n, "help")) {
        outs("Zenix shell. Line editing: arrows, Home/End, Ctrl-A/E/B/F/K/U/W/L, history Up/Down, Tab completes.\n"
             "Language: VAR=value, $VAR, $(cmd), * ? [..], | && || ; &, < > >> 2> 2>&1 << EOF,\n"
             "  if/elif/else/fi, while/until/for/do/done, name() { ...; }, alias, scripts (sh file).\n"
             "Builtins: cd exit export unset set alias unalias history source read shift return break continue\n"
             "  jobs wait kill type test [ true false : echo pwd eval local help\n");
        return 0;
    }
    return 127;
}

/* ================================================================ prompt and main */
static char *expand_prompt(const char *ps, int *vislen)
{
    char out[256]; int o = 0, vis = 0, hidden = 0;
    const char *home = var_get("HOME");
    for (const char *p = ps; *p && o < 240; p++) {
        if (*p != '\\') { out[o++] = *p; if (!hidden) vis++; continue; }
        p++;
        if (!*p) break;
        const char *add = 0; char tmp[130];
        switch (*p) {
        case 'w': case 'W': {
            const char *c = cwd_now();
            if (*p == 'w' && home && home[1] && !strncmp(c, home, strlen(home)) && (c[strlen(home)] == '/' || !c[strlen(home)])) { tmp[0] = '~'; strcpy(tmp + 1, c + strlen(home)); add = tmp; }
            else if (*p == 'W') { const char *b = c + strlen(c); while (b > c && b[-1] != '/') b--; add = (*b ? b : "/"); }
            else add = c;
            break;
        }
        case 'u': add = var_get("USER"); if (!add) add = "root"; break;
        case 'h': add = var_get("HOSTNAME"); if (!add) add = "zenix"; break;
        case '$': { const char *u = var_get("USER"); add = (!u || !strcmp(u, "root")) ? "#" : "$"; break; }
        case 'n': out[o++] = '\r'; out[o++] = '\n'; vis = 0; continue;
        case 'e': out[o++] = 27; continue;
        case '[': hidden = 1; continue;
        case ']': hidden = 0; continue;
        case '\\': out[o++] = '\\'; if (!hidden) vis++; continue;
        default: out[o++] = '\\'; out[o++] = *p; if (!hidden) vis += 2; continue;
        }
        if (add) for (; *add && o < 240; add++) { out[o++] = *add; if (!hidden) vis++; }
    }
    out[o] = 0; *vislen = vis;
    return adup(out);
}

static void import_environment(void)
{
    static char blk[1024];
    int n = sys_envget(blk, sizeof blk);
    if (n <= 0) return;
    if (n > (int)sizeof blk) n = (int)sizeof blk;
    for (int i = 0; i < n && blk[i];) {
        char *eq = strchr(blk + i, '=');
        if (eq) { *eq = 0; var_set(blk + i, eq + 1, 1); }
        i += (int)strlen(blk + i) + 1;
    }
}

static void init_vars(void)
{
    static const char *const defs[][2] = {
        { "PATH", "/bin" }, { "HOME", "/home" }, { "USER", "root" }, { "HOSTNAME", "zenix" }, { "SHELL", "/bin/sh" },
        { "TERM", "vt100" }, { "LINES", "31" }, { "COLUMNS", "80" }, { 0, 0 } };
    for (int i = 0; defs[i][0]; i++) if (!var_get(defs[i][0])) var_set(defs[i][0], defs[i][1], 1);
    if (!var_get("PS1")) var_set("PS1", "\\h:\\w\\$ ", 0);
    if (!var_get("PS2")) var_set("PS2", "> ", 0);
    if (!var_get("IFS")) var_set("IFS", " \t\n", 0);
    var_set("PWD", cwd_now(), 1);
    env_dirty = 1;
}

static char *hist_path(void)
{
    static char p[160];
    const char *h = var_get("HOME"); if (!h) h = "/";
    strcpy(p, h); u32 l = strlen(p); if (l && p[l - 1] != '/') p[l++] = '/'; strcpy(p + l, ".sh_history");
    return p;
}

static void load_history(void)
{
    u32 len; char *t = file_slurp(hist_path(), &len);
    if (!t) return;
    for (char *p = t; *p;) { char *e = strchr(p, '\n'); if (e) *e = 0; hist_add(p); if (!e) break; p = e + 1; }
}

static void save_history(void)
{
    int fd = sys_open(hist_path(), O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    int from = nhist > 100 ? nhist - 100 : 0;
    for (int i = from; i < nhist; i++) { sys_write(fd, hist[i], strlen(hist[i])); sys_write(fd, "\n", 1); }
    sys_close(fd);
}

static void run_startup_file(const char *path)
{
    char *t = file_slurp(path, 0);
    if (t) run_string(t);
}

static void interactive_loop(void)
{
    static char line[LMAX], full[2048];
    for (;;) {
        abrk = arena;
        reap_jobs(1);
        int plen;
        const char *ps = var_get("PS1");
        char *prompt = expand_prompt(ps ? ps : "$ ", &plen);
        int n = readline(prompt, plen, line, LMAX);
        if (n == -1) break;
        if (n == -2 || !line[0]) continue;
        strcpy(full, line);
        for (;;) {                                           /* keep reading while the command is incomplete */
            int rc = lex(full);
            if (rc != 1) break;
            int pl2; char *p2 = expand_prompt(var_get("PS2") ? var_get("PS2") : "> ", &pl2);
            int m = readline(p2, pl2, line, LMAX);
            if (m < 0) { full[0] = 0; break; }
            u32 l = strlen(full); if (l + strlen(line) + 2 >= sizeof full) break;
            if (l && full[l - 1] == '\\') full[l - 1] = 0;   /* a trailing backslash joins lines */
            else full[l++] = '\n';
            strcpy(full + l - (full[l - 1] == 0 ? 0 : 0), line);
        }
        if (!full[0]) continue;
        hist_add(full);
        interrupted = 0;
        sys_ttyintr();
        last_status = run_string(full);
        if (interrupted) { outs("\r\n"); last_status = 130; }
        loop_break = loop_continue = 0;
        if (exit_requested) break;
    }
    save_history();
}

int main(int argc, char **argv)
{
    arena = sys_memalloc(ARENA);
    if (!arena) { errm("no memory", 0); return 2; }
    abrk = arena; aend = arena + ARENA;
    shell_pid = sys_getpid();
    import_environment();
    init_vars();
    int ai = 1; char *cmdstr = 0, *script = 0;
    if (argc > 1 && !strcmp(argv[1], "-c")) { if (argc < 3) { errm("-c", "needs a command"); return 2; } cmdstr = argv[2]; ai = 3; if (ai < argc) { arg0 = argv[ai]; ai++; } }
    else if (argc > 1) { script = argv[1]; arg0 = argv[1]; ai = 2; }
    for (; ai < argc && npos < 23; ai++) pos_arg[npos++] = argv[ai];
    if (cmdstr) { int st = run_string(cmdstr); return exit_requested ? last_status : st; }
    if (script) {
        char *t = file_slurp(script, 0);
        if (!t) { errm(script, "cannot open"); return 127; }
        int st = run_string(t);
        return exit_requested ? last_status : st;
    }
    interactive = 1;
    run_startup_file("/etc/profile");
    { char p[160]; const char *h = var_get("HOME"); strcpy(p, h ? h : "/"); u32 l = strlen(p); if (l && p[l - 1] != '/') p[l++] = '/'; strcpy(p + l, ".shrc"); run_startup_file(p); }
    abrk = arena;
    load_history();
    interactive_loop();
    return last_status;
}
