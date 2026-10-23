/* SPDX-License-Identifier: GPL-2.0-only */
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
/* The stack is 16 KB (the Makefile's STACK_sh is not applied yet: audit B6). The deepest use measured on the test scripts,
 * including a function calling itself 40 deep, is under 6 KB. With no MMU an overflow would corrupt memory silently, so
 * the recursive parts (running, parsing, expanding, globbing) refuse to go deeper than STK_LIMIT bytes below where main
 * started, which leaves about 5 KB for the deepest leaf functions. */
#define STK_LIMIT 11000
static char *stk_top;
static inline int stack_low(void) { char *sp; __asm__ volatile("move.l %%sp,%0" : "=r"(sp)); return stk_top && (u32)(stk_top - sp) > STK_LIMIT; }
static void fatal(const char *m) { errm(m, 0); sys_exit(2); }
static void *aalloc(u32 n)
{
    n = (n + 3) & ~3u;
    if (abrk + n > aend) fatal("out of memory");
    void *p = abrk; abrk += n; return p;
}
static char *adupn(const char *s, u32 n) { char *p = aalloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }
static char *adup(const char *s) { return adupn(s, strlen(s)); }

/* A list of strings that grows as needed (in the arena): expanded arguments, glob matches, for-loop items, words of a
 * command. No silent cap: the only limit is the arena ("out of memory"). v[n] is always 0. */
struct words { char **v; int n, cap; };
static void wv_init(struct words *w, int cap) { w->v = aalloc((u32)(cap + 1) * sizeof(char *)); w->n = 0; w->cap = cap; w->v[0] = 0; }
static void wv_push(struct words *w, char *s)
{
    if (w->n >= w->cap) {
        int nc = w->cap * 2; char **nv = aalloc((u32)(nc + 1) * sizeof(char *));
        memcpy(nv, w->v, (u32)w->n * sizeof(char *)); w->v = nv; w->cap = nc;
    }
    w->v[w->n++] = s; w->v[w->n] = 0;
}

/* persistent strings (variables, history, aliases, functions) */
static char *pdup(const char *s) { u32 n = strlen(s) + 1; char *p = malloc(n); if (!p) fatal("out of memory"); memcpy(p, s, n); return p; }

/* ---------------------------------------------------------------- state */
static int last_status, shell_pid, last_bg, interactive, loop_break, loop_continue, func_return, in_func, exit_requested;
/* the positional parameters $1..: borrowed (the arguments of a function call or of the shell) or, after `set --`,
 * owned copies on the heap (they must outlive the command that set them) */
static char **pos_arg; static int npos; static u8 pos_owned;
static void pos_free(void) { if (pos_owned) { for (int i = 0; i < npos; i++) free(pos_arg[i]); free(pos_arg); } pos_arg = 0; npos = 0; pos_owned = 0; }
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
        char *nm = malloc(strlen(n) + 1);
        if (!nm) { errm(n, "out of memory"); return; }
        strcpy(nm, n);
        v->used = 1; v->name = nm; v->val = 0; v->cap = 0; v->exp = 0;
    }
    u32 need = strlen(val) + 1;
    if (need > v->cap) {                    /* a bigger home for the value; the old one is given back */
        u32 nc = need < 32 ? 32 : need;
        char *nv = realloc(v->val, nc);
        if (!nv) { errm(n, "out of memory"); if (!v->val) { free(v->name); v->used = 0; } return; }
        v->val = nv; v->cap = nc;
    }
    memcpy(v->val, val, need);
    if (exp >= 0) v->exp = (u8)exp;
}
static void var_unset(const char *n) { struct var *v = var_find(n); if (v) { free(v->name); free(v->val); v->name = v->val = 0; v->cap = 0; v->used = 0; } }

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
    u32 dl = strlen(dir);
    while (sys_readdir(fd, &d) > 0) {
        if (strncmp(d.name, base, bl)) continue;
        if (d.name[0] == '.' && base[0] != '.') continue;
        u32 nl = strlen(d.name);
        if (dl + nl + 2 > 200 || nl + 2 > 100) continue;    /* longer than any path a system call takes: not offered */
        char full[200];
        memcpy(full, dir, dl); memcpy(full + dl, d.name, nl + 1);
        char cand[100]; memcpy(cand, d.name, nl + 1);
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
        for (int i = 0; i < 128; i++) if (vars[i].used && !strncmp(vars[i].name, pre + 1, strlen(pre + 1)) && strlen(vars[i].name) < 98) {
            char c[100]; c[0] = '$'; strcpy(c + 1, vars[i].name); n = add_match(m, n, max, c);
        }
    } else if (strchr(pre, '/')) {                                  /* a path */
        char *sl = pre + strlen(pre); while (sl > pre && sl[-1] != '/') sl--;
        char dir[200]; u32 dl = (u32)(sl - pre); memcpy(dir, pre, dl); dir[dl] = 0;
        n = complete_dir(m, n, max, dir, sl, 1, 0);         /* dir is under 200 bytes, each match under 100 */
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
/* ---- Ctrl-R: incremental search backward through the history ---- */
static int hist_find(const char *pat, int from)               /* newest match at or before index from, or nhist */
{
    u32 pl = strlen(pat);
    for (int i = from; i >= 0; i--) {
        const char *h = hist[i];
        for (u32 k = 0; h[k]; k++) if (!strncmp(h + k, pat, pl)) return i;
        if (!pl) return i;
    }
    return nhist;
}
/* 1 = the match was accepted with Enter (run it), 0 = back to editing (buf holds the match, or is unchanged if cancelled) */
static int isearch(char *buf, int max, int *lenp, int *posp)
{
    char pat[48]; int pl = 0, found = nhist;
    pat[0] = 0;
    for (;;) {
        char shown[64]; const char *m = found < nhist ? hist[found] : "";
        strncpy(shown, m, 50); shown[50] = 0;
        outs("\r(reverse-i-search)`"); outs(pat); outs("': "); outs(shown); outs("\033[K");
        unsigned char c;
        if (sys_read(0, &c, 1) != 1) return 0;
        if (c == 18) { if (found > 0) { int f = hist_find(pat, found - 1); if (f < nhist) found = f; } continue; }   /* next older */
        if (c == 127 || c == 8) { if (pl) { pat[--pl] = 0; found = nhist > 0 ? hist_find(pat, nhist - 1) : nhist; } continue; }
        if (c == 7 || c == 3) return 0;                        /* Ctrl-G or Ctrl-C: cancel, keep the line as it was */
        if (c >= 32 && c < 127) {
            if (pl < 46) { pat[pl++] = (char)c; pat[pl] = 0; int f = hist_find(pat, found < nhist ? found : nhist - 1); if (f < nhist) found = f; else found = nhist; }
            continue;
        }
        if (c == 27) {                                          /* an arrow key or Escape: accept the match, swallow the sequence */
            unsigned char s1 = 0, s2 = 0;
            if (sys_read(0, &s1, 1) == 1 && (s1 == '[' || s1 == 'O')) { while (sys_read(0, &s2, 1) == 1 && ((s2 >= '0' && s2 <= '9') || s2 == ';')) ; }
        }
        if (found < nhist && (int)strlen(hist[found]) < max - 2) { strcpy(buf, hist[found]); *lenp = *posp = (int)strlen(buf); }
        return c == 13 || c == 10;
    }
}

/* a history entry into the line: an entry longer than the line (a multi-line command can be 2 KB) is cut, and the
 * bell says so */
static void hist_copy(char *buf, const char *h, int max)
{
    u32 n = strlen(h);
    if (n > (u32)max - 2) { n = (u32)max - 2; outs("\a"); }
    memcpy(buf, h, n); buf[n] = 0;
}

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
        case 18:                                                /* Ctrl-R: search the history */
            if (isearch(buf, max, &len, &pos)) { outs("\r\n"); sys_ttymode(0); buf[len] = 0; return len; }
            break;
        case 16:                                                /* history: previous */
            if (hidx > 0) {
                if (hidx == nhist) { memcpy(saved, buf, (u32)len + 1); }
                hidx--; hist_copy(buf, hist[hidx], max); len = pos = (int)strlen(buf);
            }
            break;
        case 14:                                                /* history: next */
            if (hidx < nhist) {
                hidx++;
                if (hidx == nhist) strcpy(buf, saved); else hist_copy(buf, hist[hidx], max);
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
static const char *lex_err;             /* why lex returned 2, when it is not a plain syntax error */

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
    lex_src = s; ntoks = 0; ti = 0; lex_incomplete = 0; toks = 0; tokcap = 0; lex_err = 0;
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
                if (npend == 8) { lex_err = "more than 8 here-documents on one line"; return 2; }
                pend[npend++] = ntoks - 2;                     /* the << token carries the body (and fd 1 = strip tabs) */
                toks[ntoks - 2].text = toks[ntoks - 1].text;
                toks[ntoks - 2].hdquoted = (strchr(toks[ntoks - 1].text, '\'') || strchr(toks[ntoks - 1].text, '"') || strchr(toks[ntoks - 1].text, '\\')) ? 1 : 0;
                ntoks--;                                       /* the delimiter word is folded into the << token */
                i = e; continue;
            }
            if (c == ';' && s[i + 1] == ';') { new_tok(T_OP, (u32)i, (u32)i + 2); t = &toks[ntoks - 1]; strcpy(t->op, ";;"); i += 2; continue; }
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
enum { N_SIMPLE, N_PIPE, N_AND, N_OR, N_SEQ, N_BG, N_IF, N_WHILE, N_FOR, N_GROUP, N_SUBSH, N_FUNC, N_CASE };
struct caseitem { char **pats; int np; struct node *body; struct caseitem *next; };
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
    struct caseitem *items;         /* N_CASE: the patterns and commands; words[0] is the subject */
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
        if (stops[i][0] == ';' && is_op(t, ";;")) return 1;
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
    struct words words, assigns; wv_init(&words, 16); wv_init(&assigns, 4);
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
        if (words.n == 0 && is_assign(t->text) && !(is_kw(t, "{") )) { wv_push(&assigns, t->text); end = t->end; ti++; continue; }
        wv_push(&words, t->text);                              /* any number of words */
        end = t->end; ti++;
    }
    if (!words.n && !assigns.n && !rhead) { syn("syntax error"); return 0; }
    n->nwords = words.n; n->words = words.v;
    n->nassign = assigns.n; n->assign = assigns.v;
    n->redirs = rhead; n->start = start; n->end = end;
    n->src = adupn(lex_src + start, end - start);
    return n;
}

static struct node *parse_command(void)
{
    if (stack_low()) { syn("nested too deeply"); return 0; }
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
            struct words ws; wv_init(&ws, 8); int has_in = 0;
            if (is_kw(peek(), "in")) {
                has_in = 1; ti++;
                while (peek() && peek()->type == T_WORD) wv_push(&ws, toks[ti++].text);
            }
            n->nwords = has_in ? ws.n : -1;
            n->words = ws.v;
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
        if (is_kw(t, "case")) {
            ti++;
            struct tok *w = peek();
            if (!w || w->type != T_WORD) { if (!w) lex_incomplete = 1; syn("expected a word after case"); return 0; }
            struct node *n = new_node(N_CASE);
            n->words = aalloc(2 * sizeof(char *)); n->words[0] = w->text; n->words[1] = 0; n->nwords = 1; ti++;
            skip_nl();
            if (!is_kw(peek(), "in")) { if (at_eof()) lex_incomplete = 1; syn("expected in"); return 0; }
            ti++;
            struct caseitem **tail = &n->items;
            static const char *const s_item[] = { ";;", "esac", 0 };
            for (;;) {
                while (peek() && (peek()->type == T_NL || is_op(peek(), ";"))) ti++;
                if (!peek()) { lex_incomplete = 1; syn("expected esac"); return 0; }
                if (is_kw(peek(), "esac")) break;
                if (is_op(peek(), "(")) ti++;
                struct caseitem *it = aalloc(sizeof *it); memset(it, 0, sizeof *it);
                struct words pats; wv_init(&pats, 4);
                for (;;) {
                    struct tok *pt = peek();
                    if (!pt || pt->type != T_WORD) { if (!pt) lex_incomplete = 1; syn("expected a pattern"); return 0; }
                    wv_push(&pats, pt->text);
                    ti++;
                    if (is_op(peek(), "|")) { ti++; continue; }
                    break;
                }
                if (!is_op(peek(), ")")) { if (at_eof()) lex_incomplete = 1; syn("expected ) after the pattern"); return 0; }
                ti++;
                it->np = pats.n; it->pats = pats.v;
                while (peek() && peek()->type == T_NL) ti++;
                if (!is_op(peek(), ";;") && !is_kw(peek(), "esac")) { it->body = parse_list(s_item); if (!it->body && perr) return 0; }
                if (is_op(peek(), ";;")) ti++;
                else if (!is_kw(peek(), "esac")) { if (at_eof()) lex_incomplete = 1; syn("expected ;; or esac"); return 0; }
                *tail = it; tail = &it->next;
            }
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
    int cap = 8, n = 0; struct node **cmds = aalloc((u32)cap * sizeof *cmds); cmds[n++] = first;
    while (is_op(peek(), "|")) {
        ti++; skip_nl();
        if (at_eof()) { lex_incomplete = 1; syn("expected a command after |"); return 0; }
        struct node *c = parse_command(); if (!c) return 0;
        if (n == cap) { struct node **nc = aalloc((u32)cap * 2 * sizeof *nc); memcpy(nc, cmds, (u32)n * sizeof *nc); cmds = nc; cap *= 2; }
        cmds[n++] = c;                                     /* any number of stages */
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
static int ex_error;
static long ar_atom(void)
{
    if (stack_low()) {                                          /* (((((...: refuse before the stack runs out */
        if (!ex_error) errm("arithmetic", "nested too deeply");
        ex_error = 1; ar_p += strlen(ar_p); return 0;
    }
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
static struct words *ex_w;
/* ex_error (above): an expansion failed (bad substitution, too deep): the command is not run, status 1 */

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

/* expand a wildcard pattern component by component; matches are appended to out (no limit on their number) */
static void glob_rec(const char *pat, const u8 *q, u32 n, u32 pos, const char *prefix, struct words *out)
{
    if (stack_low()) { errm("wildcard", "nested too deeply"); ex_error = 1; return; }
    while (pos < n && pat[pos] == '/') pos++;
    if (pos >= n) { if (path_exists(prefix)) wv_push(out, adup(prefix)); return; }
    u32 e = pos; while (e < n && pat[e] != '/') e++;
    int last = (e >= n);
    if (!has_glob(pat + pos, q + pos, e - pos)) {
        char np[300]; u32 pl = strlen(prefix);
        if (pl + (e - pos) + 3 > sizeof np) return;
        memcpy(np, prefix, pl); memcpy(np + pl, pat + pos, e - pos); np[pl + e - pos] = 0;
        if (!last) { u32 l = strlen(np); np[l] = '/'; np[l + 1] = 0; }
        glob_rec(pat, q, n, e, np, out);
        return;
    }
    int fd = sys_open(prefix[0] ? prefix : ".", O_RDONLY);
    if (fd < 0) return;
    struct udirent d;
    struct words found; wv_init(&found, 16);
    while (sys_readdir(fd, &d) > 0) {
        if (d.name[0] == '.' && pat[pos] != '.') continue;
        if (!strcmp(d.name, ".") || !strcmp(d.name, "..")) continue;
        if (gm(pat + pos, q + pos, (int)(e - pos), d.name)) wv_push(&found, adup(d.name));
    }
    sys_close(fd);
    sort_strs(found.v, found.n);
    for (int i = 0; i < found.n; i++) {
        char np[300]; u32 pl = strlen(prefix), fl = strlen(found.v[i]);
        if (pl + fl + 3 > sizeof np) continue;              /* longer than any path a system call takes (63) */
        memcpy(np, prefix, pl); memcpy(np + pl, found.v[i], fl); np[pl + fl] = 0;
        if (!last) { if (!path_is_dir(np)) continue; np[pl + fl] = '/'; np[pl + fl + 1] = 0; }
        glob_rec(pat, q, n, e, np, out);
    }
}

static int no_glob_exp;                       /* expanding a case pattern: keep the field and its quote mask, do not glob */
static u8 pat_q[FMAX]; static u32 pat_n;
static void field_end(void)
{
    if (!fhas) return;
    fbuf[fn] = 0;
    if (no_glob_exp) { if (!pat_n) { memcpy(pat_q, fq, fn); pat_n = fn + 1; } }
    {
        int before = ex_w->n;
        if (!no_glob_exp && has_glob(fbuf, fq, fn)) {
            struct words tmp; wv_init(&tmp, 16);
            glob_rec(fbuf, fq, fn, 0, fbuf[0] == '/' ? "/" : "", &tmp);
            sort_strs(tmp.v, tmp.n);
            for (int i = 0; i < tmp.n; i++) wv_push(ex_w, tmp.v[i]);
        }
        if (ex_w->n == before) wv_push(ex_w, adupn(fbuf, fn));          /* no match: the word itself */
    }
    fn = 0; fhas = 0;
}

static void fpush(char c, int quoted)
{
    if (fn < FMAX - 1) { fbuf[fn] = c; fq[fn] = (u8)quoted; fn++; }
    else if (!ex_error) { errm("a word is longer than 4095 characters", 0); ex_error = 1; }
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
/* ${NN}: a positional parameter by number (also 10 and more) */
static const char *pos_param(const char *name)
{
    int k = 0;
    for (const char *p = name; *p; p++) { if (*p < '0' || *p > '9') return 0; if (k < 100000) k = k * 10 + (*p - '0'); }
    if (k == 0) return arg0;
    return k <= npos ? pos_arg[k - 1] : "";
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
        const char *v = isdigit((unsigned char)name[0]) ? pos_param(name) : var_get(name);
        if (!v && !name[1]) v = special_param(name[0], tmp);
        *op = saved;
        if (!len_op && v && (*op == '#' || *op == '%' || *op == '/' || (*op == ':' && (op[1] >= '0' && op[1] <= '9')))) {
            static const u8 zq[512];
            char *val = adup(v); int vl = (int)strlen(val);
            if (*op == '#' || *op == '%') {                     /* remove the shortest (#, %) or longest (##, %%) match of a pattern */
                int longest = op[1] == *op; const char *pat = op + 1 + longest; int pn = (int)strlen(pat); if (pn > 512) pn = 512;
                if (*op == '#') {
                    int cut = 0, found = 0;
                    for (int k = longest ? vl : 0; longest ? k >= 0 : k <= vl; k += longest ? -1 : 1) {
                        char sv = val[k]; val[k] = 0; int m = gm(pat, zq, pn, val); val[k] = sv;
                        if (m) { cut = k; found = 1; break; }
                    }
                    append_val(found ? val + cut : val, quoted);
                } else {
                    int cut = vl, found = 0;
                    for (int k = longest ? 0 : vl; longest ? k <= vl : k >= 0; k += longest ? 1 : -1)
                        if (gm(pat, zq, pn, val + k)) { cut = k; found = 1; break; }
                    if (found) val[cut] = 0;
                    append_val(val, quoted);
                }
            } else if (*op == '/') {                            /* ${v/pat/rep}: first match; ${v//pat/rep}: all */
                int all = op[1] == '/'; char *pat = op + 1 + all, *rep = strchr(pat, '/');
                if (rep) { *rep = 0; rep++; } else rep = (char *)"";
                int pn = (int)strlen(pat); if (pn > 512) pn = 512;
                char *res = aalloc((u32)(vl + 1) * 4 + 64); u32 ro = 0; int k = 0, did = 0;
                while (k <= vl) {
                    int hit = 0;
                    if (pn && (all || !did)) {
                        for (int l = vl - k; l >= 1; l--) {         /* longest match first */
                            char sv = val[k + l]; val[k + l] = 0; int m = gm(pat, zq, pn, val + k); val[k + l] = sv;
                            if (m) { u32 rl = strlen(rep); if (ro + rl + 8 > (u32)(vl + 1) * 4 + 64) break; memcpy(res + ro, rep, rl); ro += rl; k += l; did = 1; hit = 1; break; }
                        }
                    }
                    if (!hit) { if (k < vl) res[ro++] = val[k]; k++; }
                }
                res[ro] = 0; append_val(res, quoted);
            } else {                                            /* ${v:offset} or ${v:offset:length} */
                const char *q = op + 1; int off = atoi(q), len = vl;
                while (*q >= '0' && *q <= '9') q++;
                if (*q == ':') len = atoi(q + 1);
                if (off > vl) off = vl;
                if (len < 0) len = vl - off + len;              /* ${v:1:-1}: a negative length counts from the end */
                if (len < 0) {                                  /* as bash: an error, not an empty string */
                    *op = 0; errm(name, "substring expression < 0"); *op = saved;
                    ex_error = 1; len = 0;
                }
                if (len > vl - off) len = vl - off;
                char *sub = adupn(val + off, (u32)len); append_val(sub, quoted);
            }
            return e + 1;
        }
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
static void expand_word(const char *w, struct words *out);
/* ---- brace expansion: pre{a,b,c}post -> preapost prebpost precpost; {1..5} {a..e} {01..10} {1..10..2} ---- */
static int brace_range(const char *body, int bl, char **items, int *ni, int max)
{
    char tmp[48]; if (bl <= 0 || bl > 40) return 0;
    memcpy(tmp, body, (u32)bl); tmp[bl] = 0;
    char *d1 = strstr(tmp, ".."); if (!d1) return 0;
    *d1 = 0; char *b = d1 + 2, *d2 = strstr(b, ".."), *st = 0;
    if (d2) { *d2 = 0; st = d2 + 2; }
    int step = st ? atoi(st) : 1; if (step < 0) step = -step; if (step == 0) step = 1;
    int a_num = tmp[0] && (isdigit((unsigned char)tmp[0]) || (tmp[0] == '-' && isdigit((unsigned char)tmp[1])));
    int b_num = b[0] && (isdigit((unsigned char)b[0]) || (b[0] == '-' && isdigit((unsigned char)b[1])));
    if (a_num && b_num) {
        int lo = atoi(tmp), hi = atoi(b), w = 0;
        if ((tmp[0] == '0' && tmp[1]) || (b[0] == '0' && b[1])) w = (int)(strlen(tmp) > strlen(b) ? strlen(tmp) : strlen(b));
        int dir = lo <= hi ? 1 : -1;
        for (int v = lo; dir > 0 ? v <= hi : v >= hi; v += dir * step) {
            if (*ni >= max) break;
            char nb[16]; int neg = v < 0; utoa((u32)(neg ? -v : v), nb + 1); char *num = nb + 1;
            char pad[24]; int pl = 0; if (neg) pad[pl++] = '-';
            for (int z = (int)strlen(num) + neg; z < w; z++) pad[pl++] = '0';
            memcpy(pad + pl, num, strlen(num) + 1);
            items[(*ni)++] = adup(pad);
        }
        return 1;
    }
    if (tmp[0] && !tmp[1] && b[0] && !b[1]) {
        int lo = (unsigned char)tmp[0], hi = (unsigned char)b[0], dir = lo <= hi ? 1 : -1;
        for (int v = lo; dir > 0 ? v <= hi : v >= hi; v += dir * step) { if (*ni >= max) break; char c[2] = { (char)v, 0 }; items[(*ni)++] = adup(c); }
        return 1;
    }
    return 0;
}
static int brace_try(const char *w, struct words *out)
{
    for (int i = 0; w[i]; i++) {
        char c = w[i];
        if (c == '\\') { if (w[i + 1]) i++; continue; }
        if (c == '\'') { i++; while (w[i] && w[i] != '\'') i++; if (!w[i]) return 0; continue; }
        if (c == '"') { i++; while (w[i] && w[i] != '"') { if (w[i] == '\\' && w[i + 1]) i++; i++; } if (!w[i]) return 0; continue; }
        if (c == '$' && w[i + 1] == '{') { int d = 0; for (i++; w[i]; i++) { if (w[i] == '{') d++; else if (w[i] == '}' && --d == 0) break; } if (!w[i]) return 0; continue; }
        if (c == '$' && w[i + 1] == '(') { int e = skip_paren(w, i); if (e < 0) return 0; i = e - 1; continue; }
        if (c != '{') continue;
        int depth = 0, commas[32], nc = 0, close = -1;
        for (int j = i; w[j]; j++) {
            char d = w[j];
            if (d == '\\') { if (w[j + 1]) j++; continue; }
            if (d == '\'') { j++; while (w[j] && w[j] != '\'') j++; if (!w[j]) break; continue; }
            if (d == '"') { j++; while (w[j] && w[j] != '"') { if (w[j] == '\\' && w[j + 1]) j++; j++; } if (!w[j]) break; continue; }
            if (d == '$' && w[j + 1] == '{') { int dd = 0; for (j++; w[j]; j++) { if (w[j] == '{') dd++; else if (w[j] == '}' && --dd == 0) break; } if (!w[j]) break; continue; }
            if (d == '{') depth++;
            else if (d == '}') { if (--depth == 0) { close = j; break; } }
            else if (d == ',' && depth == 1 && nc < 31) commas[nc++] = j;
        }
        if (close < 0) continue;
        char *items[64]; int ni = 0;
        if (nc) {
            int from = i + 1;
            for (int k = 0; k <= nc && ni < 64; k++) {
                int to = k < nc ? commas[k] : close;
                items[ni++] = adupn(w + from, (u32)(to - from));
                from = to + 1;
            }
        } else if (!brace_range(w + i + 1, close - i - 1, items, &ni, 64)) continue;
        char *pre = adupn(w, (u32)i), *post = adup(w + close + 1);
        for (int k = 0; k < ni; k++) {
            u32 l = strlen(pre) + strlen(items[k]) + strlen(post);
            char *nw = aalloc(l + 1); strcpy(nw, pre); strcat(nw, items[k]); strcat(nw, post);
            expand_word(nw, out);
        }
        return 1;
    }
    return 0;
}

static void expand_word(const char *w, struct words *out)
{
    if (stack_low()) { errm("expansion", "nested too deeply"); ex_error = 1; return; }
    if (!no_glob_exp && brace_try(w, out)) return;
    ex_w = out; fn = 0; fhas = 0;
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
    struct words f; wv_init(&f, 8);
    /* reuse the quote-aware expander but join the fields with a space */
    expand_word(w, &f);
    for (int k = 0; k < f.n; k++) {
        u32 l = strlen(f.v[k]);
        if (olen + l + 2 >= sizeof out) { errm("a word is longer than 4095 characters", 0); ex_error = 1; break; }
        memcpy(out + olen, f.v[k], l); olen += l;
        if (k + 1 < f.n) out[olen++] = ' ';
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
static char *exit_trap;                 /* trap '...' EXIT: run when the shell ends */

#define MAXJOBS 16
static struct job { int pid; char *cmd; u8 used, done, stopped; int status; int pids[8], np; } jobs[MAXJOBS];
static int add_job(int pid, const char *cmd)
{
    for (int i = 0; i < MAXJOBS; i++) if (!jobs[i].used) { jobs[i].used = 1; jobs[i].done = 0; jobs[i].stopped = 0; jobs[i].pid = pid; jobs[i].pids[0] = pid; jobs[i].np = 1; jobs[i].cmd = pdup(cmd); return i + 1; }
    return 0;
}
static int cur_job;             /* the job most recently started or stopped (what fg and bg use without an argument) */
static void reap_jobs(int announce)
{
    for (int i = 0; i < MAXJOBS; i++) {
        if (!jobs[i].used) continue;
        if (!jobs[i].done && !jobs[i].stopped) {
            int r = sys_waitnb(jobs[i].pid);
            if (r == -2) continue;
            jobs[i].done = 1; jobs[i].status = r;
        }
        if (announce && jobs[i].done) {
            char b[16]; outs("["); utoa((u32)i + 1, b); outs(b); outs("]  Done      "); outs(jobs[i].cmd); outs("\n");
            free(jobs[i].cmd); jobs[i].used = 0;
        }
    }
}

/* ---- redirections: applied to the shell's own fds 0-2, then undone ---- */
/* saved[fd]: -1 not touched, -2 was closed (close it again), else a copy to put back. fds 0-9, so `3>&1 1>&2 2>&3` leaves
 * fd 3 as it was. */
struct rsave { int saved[10]; char *tmpfiles[4]; int ntmp; };
#define NFD_SH 16                       /* file descriptors per process (kernel NFD) */

static void rs_init(struct rsave *r) { for (int i = 0; i < 10; i++) r->saved[i] = -1; r->ntmp = 0; }
/* the copy goes to a free fd of 10-15 (out of the way of 3>&1 and friends), else the lowest free one */
static int dup_high(int fd)
{
    for (int k = NFD_SH - 1; k >= 10; k--) { int t = sys_dup(k); if (t >= 0) { sys_close(t); continue; } if (sys_dup2(fd, k) >= 0) return k; }
    return sys_dup(fd);
}
static void rs_save(struct rsave *r, int fd) { if (fd >= 0 && fd < 10 && r->saved[fd] == -1) { int t = sys_dup(fd); if (t < 0) { r->saved[fd] = -2; return; } sys_close(t); r->saved[fd] = dup_high(fd); } }
static void rs_restore(struct rsave *r)
{
    for (int fd = 9; fd >= 0; fd--) {
        if (r->saved[fd] >= 0) { sys_dup2(r->saved[fd], fd); sys_close(r->saved[fd]); }
        else if (r->saved[fd] == -2) sys_close(fd);
        r->saved[fd] = -1;
    }
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
            /* /tmp/.shPID_RANDOM, created exclusively with mode 0600: another user cannot guess the name ahead, put a link
             * there or read the text (audit D) */
            char name[40] = "/tmp/.sh"; char b[12];
            utoa((u32)shell_pid, b); strcpy(name + strlen(name), b); u32 l = strlen(name); name[l++] = '_';
            u32 um = (u32)sys_umask(077);
            fd = -EEXIST;
            for (int tries = 0; tries < 50 && fd == -EEXIST; tries++) {
                u32 x = sys_hrtime() * 2654435761u ^ (u32)hd_seq++ * 40503u ^ sys_ticks();
                for (int k = 0; k < 8; k++) { name[l + k] = "abcdefghijklmnopqrstuvwxyz012345"[x & 31]; x = (x >> 5) ^ (x * 69069u); }
                name[l + 8] = 0;
                fd = sys_open(name, O_WRONLY | O_CREAT | O_EXCL);
            }
            sys_umask(um);
            if (fd < 0) { errm("cannot create a here-document file in /tmp", 0); return 1; }
            const char *body = r->hdquoted ? r->body : expand_heredoc(r->body);
            sys_write(fd, body, strlen(body)); sys_close(fd);
            fd = sys_open(name, O_RDONLY);
            if (rs->ntmp < 4) rs->tmpfiles[rs->ntmp++] = adup(name);
            else sys_unlink(name);                            /* open already: it goes when the last reader closes it */
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
        if (fd < 0) { errm(target, zwhy(r->kind == 0 ? "cannot open" : "cannot write")); return 1; }
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
    if (pid < 0) { errm(argv[0], zwhy("cannot run")); return -126; }
    return pid;
}

/* wait for a foreground job (a pipeline's processes); the last one holds the terminal, so Ctrl-Z stops that one and
 * the shell stops the others. A stopped job is kept in the job table (fg and bg bring it back). job: its number when it
 * is being brought back to the foreground, else 0. */
static int fg_wait(int *pids, int np, const char *name, int job)
{
    sys_setfg(pids[np - 1]);
    int st = sys_waitu(pids[np - 1]);
    sys_setfg(0);
    if (st == ST_STOPPED) {
        for (int i = 0; i < np - 1; i++) sys_jobctl(pids[i], 0);
        if (!job) job = add_job(pids[np - 1], name);
        if (job) {
            struct job *j = &jobs[job - 1];
            j->stopped = 1; j->done = 0; j->np = np > 8 ? 8 : np;
            for (int i = 0; i < j->np; i++) j->pids[i] = pids[i];
            cur_job = job;
            char b[16]; outs("\n["); utoa((u32)job, b); outs(b); outs("]+  Stopped    "); outs(j->cmd); outs("\n");
        }
        return 128 + 20;
    }
    for (int i = 0; i < np - 1; i++) sys_wait(pids[i]);
    if (job) { free(jobs[job - 1].cmd); jobs[job - 1].used = 0; if (cur_job == job) cur_job = 0; }
    if (st == 139) { errs("["); errs(name); errs(" crashed]\n"); }
    return st;
}
static int wait_fg(int pid, const char *name) { return fg_wait(&pid, 1, name, 0); }

/* ---- builtins ---- */
static int is_builtin(const char *n);
static int run_builtin(char **argv, int argc);
static int run_function(struct func *f, char **argv, int argc);

static int run_words(char **argv, int argc, int *pidout, int background)
{
    if (argc == 0) return 0;
    /* alias on the first word; as in bash, an alias being expanded is not expanded again (alias a=b b=a ends) */
    static struct alias *active[16]; static int nactive;
    struct alias *al = alias_find(argv[0]);
    for (int i = 0; i < nactive && al; i++) if (active[i] == al) al = 0;
    if (al && !background && nactive < 16) {
        struct words nv; wv_init(&nv, 8);
        char *v = adup(al->val); char *p = v;
        while (*p) { while (is_space(*p)) p++; if (!*p) break; wv_push(&nv, p); while (*p && !is_space(*p)) p++; if (*p) *p++ = 0; }
        for (int i = 1; i < argc; i++) wv_push(&nv, argv[i]);
        if (nv.n && strcmp(nv.v[0], argv[0])) {
            active[nactive++] = al;
            int st = run_words(nv.v, nv.n, pidout, background);
            nactive--;
            return st;
        }
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

/* all the words of a command, expanded (no limit on their number); -1 if an expansion failed (said why) */
static int expand_command(struct node *n, char ***argvp)
{
    struct words w; wv_init(&w, 16);
    ex_error = 0;
    for (int i = 0; i < n->nwords; i++) expand_word(n->words[i], &w);
    *argvp = w.v;
    if (ex_error) { ex_error = 0; return -1; }
    return w.n;
}

static int run_simple(struct node *n, int background, int *pidout)
{
    char *mark = abrk;                                      /* released when the command is done */
    char **argv; int argc = expand_command(n, &argv);
    if (argc < 0) { abrk = mark; return 1; }
    struct rsave rs; rs_init(&rs);
    int st = 0;
    /* assignments */
    int nas = n->nassign ? n->nassign : 1;
    char **saved_names = aalloc((u32)nas * sizeof(char *)); char **saved_vals = aalloc((u32)nas * sizeof(char *)); int *saved_had = aalloc((u32)nas * sizeof(int));
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
    if ((n->redirs && apply_redirs(n->redirs, &rs)) || ex_error) { ex_error = 0; rs_restore(&rs); abrk = mark; return 1; }
    if (argc) st = run_words(argv, argc, pidout, background);
    rs_restore(&rs);
    if (argc) for (int i = 0; i < n->nassign; i++) { if (saved_had[i]) var_set(saved_names[i], saved_vals[i], -1); else var_unset(saved_names[i]); env_dirty = 1; }
    if (!background && !pidout) abrk = mark;
    return st;
}

/* ---- pipelines: each stage is a separate process ---- */
/* There is no fork, so a builtin, function or compound command in a pipeline runs in a child shell that is given the text of
 * the command. To make that child a faithful subshell it is also given what the parent knows: the variables that are not
 * exported (the exported ones travel in the environment), the functions and the positional parameters, as a few lines
 * of shell in front of the command. */
static void prefix_quote(char *buf, u32 *o, u32 cap, const char *s)
{
    if (*o + 2 >= cap) return;
    buf[(*o)++] = '\'';
    for (; *s && *o + 6 < cap; s++) {
        if (*s == '\'') { buf[(*o)++] = '\''; buf[(*o)++] = '\\'; buf[(*o)++] = '\''; buf[(*o)++] = '\''; }
        else buf[(*o)++] = *s;
    }
    buf[(*o)++] = '\'';
}
static int mentions(const char *text, const char *name) { return strstr(text, name) != 0; }
static char *subshell_text(const char *cmd)
{
    /* only what the command can reach: variables and functions named in it, or in the functions it calls */
    u8 fin[48], vin[128]; memset(fin, 0, sizeof fin); memset(vin, 0, sizeof vin);
    for (int changed = 1, round = 0; changed && round < 6; round++) {
        changed = 0;
        for (int i = 0; i < 48; i++) {
            if (!funcs[i].used || fin[i]) continue;
            int hit = mentions(cmd, funcs[i].name);
            for (int j = 0; j < 48 && !hit; j++) if (fin[j] && mentions(funcs[j].body, funcs[i].name)) hit = 1;
            if (hit) { fin[i] = 1; changed = 1; }
        }
        for (int i = 0; i < 128; i++) {
            struct var *v = &vars[i];
            if (!v->used || v->exp || vin[i] || !v->val || !is_name_c((unsigned char)v->name[0], 1)) continue;
            int hit = mentions(cmd, v->name);
            for (int j = 0; j < 48 && !hit; j++) if (fin[j] && mentions(funcs[j].body, v->name)) hit = 1;
            if (hit) { vin[i] = 1; changed = 1; }
        }
    }
    int need_pos = npos && strchr(cmd, '$');
    for (int j = 0; j < 48 && npos && !need_pos; j++) if (fin[j] && strchr(funcs[j].body, '$')) need_pos = 1;
    u32 size = strlen(cmd) + 8;
    for (int i = 0; i < 128; i++) if (vin[i]) size += strlen(vars[i].name) + strlen(vars[i].val) * 4 + 8;
    for (int i = 0; i < 48; i++) if (fin[i]) size += strlen(funcs[i].name) + strlen(funcs[i].body) + 8;
    if (need_pos) { size += 12; for (int k = 0; k < npos; k++) size += strlen(pos_arg[k]) * 4 + 4; }
    char *buf = aalloc(size); u32 o = 0;
    for (int i = 0; i < 128; i++) {
        if (!vin[i]) continue;
        u32 nl = strlen(vars[i].name);
        memcpy(buf + o, vars[i].name, nl); o += nl; buf[o++] = '=';
        prefix_quote(buf, &o, size, vars[i].val); buf[o++] = ';'; buf[o++] = ' ';
    }
    for (int i = 0; i < 48; i++) {
        if (!fin[i]) continue;
        u32 nl = strlen(funcs[i].name), bl = strlen(funcs[i].body);
        memcpy(buf + o, funcs[i].name, nl); o += nl; buf[o++] = '('; buf[o++] = ')'; buf[o++] = ' ';
        memcpy(buf + o, funcs[i].body, bl); o += bl; buf[o++] = '\n';
    }
    if (need_pos) {
        memcpy(buf + o, "set --", 6); o += 6;
        for (int k = 0; k < npos; k++) { buf[o++] = ' '; prefix_quote(buf, &o, size, pos_arg[k]); }
        buf[o++] = ';'; buf[o++] = ' ';
    }
    strcpy(buf + o, cmd);
    return buf;
}

static int spawn_stage(struct node *c, int background_text_only)
{
    (void)background_text_only;
    if (c->type == N_SIMPLE && !c->nassign) {
        char **argv; int argc = expand_command(c, &argv);
        if (argc < 0) return -1;
        if (argc && !func_find(argv[0]) && !is_builtin(argv[0]) && !alias_find(argv[0])) {
            struct rsave rs; rs_init(&rs);
            if (c->redirs && apply_redirs(c->redirs, &rs)) { rs_restore(&rs); return -1; }
            int pid = start_program(argv);
            rs_restore(&rs);
            return pid < 0 ? -1 : pid;
        }
    }
    /* builtins, functions and compound commands run in a child shell */
    char *argv[5] = { "sh", "-c", subshell_text(c->src ? c->src : "true"), 0, 0 };
    if (env_dirty) { env_sync(); env_dirty = 0; }
    return sys_spawn("/bin/sh", argv, 0);
}

static int run_pipeline(struct node *p, int background, int *pidout)
{
    char *mark = abrk;
    int *pids = aalloc((u32)p->ncmds * sizeof(int)), np = 0, prev = -1;
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
    if (np == 0) { abrk = mark; return 1; }
    if (pidout) *pidout = pids[np - 1];
    int st = background ? 0 : fg_wait(pids, np, p->src ? p->src : "pipeline", 0);
    abrk = mark;
    return st;
}

/* `local NAME` saves the old value here; the function's end puts it back (dynamic scope, as in bash) */
#define NLOCAL 64
static struct lsave { char *name, *val; u8 had; } lsaves[NLOCAL];
static int nlsave;
static int frame_base;                  /* where the running function's saves start (local_save looks only there) */
static int local_save(const char *name)
{
    for (int i = nlsave - 1; i >= frame_base && i >= 0; i--) if (!strcmp(lsaves[i].name, name)) return 0;   /* saved already */
    if (nlsave >= NLOCAL) { errm("local", "too many local variables (64)"); return 1; }
    const char *old = var_get(name);
    struct var *v = var_find(name);
    lsaves[nlsave].name = pdup(name); lsaves[nlsave].had = old != 0;
    lsaves[nlsave].val = old ? pdup(old) : 0;
    if (v) lsaves[nlsave].had |= (u8)(v->exp << 1);
    nlsave++;
    return 0;
}
static void local_restore(int base)
{
    while (nlsave > base) {
        struct lsave *l = &lsaves[--nlsave];
        if (l->had & 1) var_set(l->name, l->val, (l->had >> 1) & 1); else var_unset(l->name);
        free(l->name); free(l->val); l->name = l->val = 0;
        env_dirty = 1;
    }
}

/* the positional parameters for a call (function, `. file args`): borrowed from argv, given back afterwards */
struct possave { char **v; int n; u8 owned; };
static void pos_push(struct possave *sv, char **v, int n) { sv->v = pos_arg; sv->n = npos; sv->owned = pos_owned; pos_arg = v; npos = n; pos_owned = 0; }
static void pos_pop(struct possave *sv) { pos_free(); pos_arg = sv->v; npos = sv->n; pos_owned = sv->owned; }

static int run_function(struct func *f, char **argv, int argc)
{
    struct possave sv; pos_push(&sv, argv + 1, argc - 1);
    int save_base = frame_base, base = nlsave; frame_base = base;
    in_func++; func_return = 0;
    int st = run_string(f->body);
    if (func_return) { st = func_return - 1; func_return = 0; }
    in_func--;
    local_restore(base); frame_base = save_base;
    pos_pop(&sv);
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
    if (stack_low()) { errm("nested too deeply", 0); return 1; }
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
        char *mark = abrk;                                  /* each statement's allocations go when it is done */
        for (struct node *c = n; c; c = c->b) {
            abrk = mark;
            st = exec_node(c->a);
            if (exit_requested || loop_break || loop_continue || func_return || interrupted) break;
        }
        abrk = mark;
        break;
    }
    case N_BG: {
        struct node *a = n->a; int pid = 0;
        char *mark = abrk;                                  /* released when it has started */
        if (a->type == N_SIMPLE && !a->nassign) {
            char **argv; int argc = expand_command(a, &argv);
            struct rsave rs; rs_init(&rs);
            if (argc < 0) pid = 0;
            else if (argc && !func_find(argv[0]) && !is_builtin(argv[0]) && !alias_find(argv[0])) {
                if (!a->redirs || !apply_redirs(a->redirs, &rs)) { pid = start_program(argv); if (pid < 0) pid = 0; }
                rs_restore(&rs);
            } else pid = spawn_stage(a, 1);
        } else if (a->type == N_PIPE) { run_pipeline(a, 1, &pid); }
        else pid = spawn_stage(a, 1);
        abrk = mark;
        if (pid > 0) {
            last_bg = pid;
            int j = add_job(pid, a->src ? a->src : "job");
            cur_job = j;
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
    case N_WHILE: {
        /* what one round allocates (pipelines, expansions, substitutions) is given back before the next, so a long
         * `while read` loop runs in flat memory; what must outlive a round lives on the heap (variables, set --, jobs) */
        char *mark = abrk;
        st = 0;
        for (;;) {
            abrk = mark;
            int c = exec_node(n->a);
            if (exit_requested) break;
            if (n->until ? c == 0 : c != 0) break;
            st = exec_node(n->b);
            if (loop_break) { loop_break--; break; }
            if (loop_continue) { loop_continue--; continue; }
            if (func_return) break;
        }
        abrk = mark;
        break;
    }
    case N_FOR: {
        struct words it; wv_init(&it, 16);
        ex_error = 0;
        if (n->nwords < 0) { for (int i = 0; i < npos; i++) wv_push(&it, adup(pos_arg[i])); }   /* copies: set -- may change them */
        else for (int i = 0; i < n->nwords; i++) expand_word(n->words[i], &it);
        if (ex_error) { ex_error = 0; st = 1; break; }
        char *mark = abrk;                                  /* the items stay; each round's allocations go (see N_WHILE) */
        st = 0;
        for (int i = 0; i < it.n; i++) {
            abrk = mark;
            var_set(n->name, it.v[i], -1);
            st = exec_node(n->b);
            if (exit_requested) break;
            if (loop_break) { loop_break--; break; }
            if (loop_continue) { loop_continue--; continue; }
            if (func_return) break;
        }
        abrk = mark;
        break;
    }
    case N_GROUP: st = exec_node(n->a); break;
    case N_CASE: {
        char *subject = expand_plain(n->words[0]);
        subject = adup(subject);
        st = 0;
        for (struct caseitem *it = n->items; it; it = it->next) {
            int hit = 0;
            for (int k = 0; k < it->np && !hit; k++) {
                struct words f; wv_init(&f, 4);
                no_glob_exp = 1; pat_n = 0;
                expand_word(it->pats[k], &f);
                no_glob_exp = 0;
                if (f.n && pat_n) hit = gm(f.v[0], pat_q, (int)pat_n - 1, subject);
            }
            if (hit) { if (it->body) st = exec_node(it->body); break; }
        }
        break;
    }
    case N_SUBSH: {
        char *mark = abrk;
        char *inner = adupn(n->src + 1, strlen(n->src) - 2);
        char *argv[5] = { "sh", "-c", subshell_text(inner), 0, 0 };
        if (env_dirty) { env_sync(); env_dirty = 0; }
        int pid = sys_spawn("/bin/sh", argv, 0);
        st = pid < 0 ? 126 : wait_fg(pid, "subshell");
        abrk = mark;
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
    char *argv[5] = { "sh", "-c", subshell_text(cmd), 0, 0 };
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
    if (rc) { errm(lex_err ? lex_err : "syntax error", 0); return 2; }
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
    "pwd", "help", "local", "eval", "umask", "fg", "bg", "trap", 0 };
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
    if (!strcmp(n, "fg") || !strcmp(n, "bg")) {
        int fg = n[0] == 'f', j = 0;
        if (argc > 1) { int pid = job_pid(argv[1]); for (int i = 0; i < MAXJOBS; i++) if (jobs[i].used && jobs[i].pid == pid) j = i + 1; }
        else if (cur_job && jobs[cur_job - 1].used) j = cur_job;
        else for (int i = MAXJOBS - 1; i >= 0; i--) if (jobs[i].used && !jobs[i].done) { j = i + 1; break; }
        if (!j) { errm(n, "no such job"); return 1; }
        struct job *jb = &jobs[j - 1];
        outs(jb->cmd); outs(fg ? "\n" : " &\n");
        jb->stopped = 0;
        for (int i = 0; i < jb->np; i++) sys_jobctl(jb->pids[i], 1);
        if (!fg) { cur_job = j; return 0; }
        int pids[8], np = jb->np; for (int i = 0; i < np; i++) pids[i] = jb->pids[i];
        return fg_wait(pids, np, jb->cmd, j);
    }
    if (!strcmp(n, "trap")) {                                  /* trap 'commands' EXIT   (only EXIT / 0 is supported) */
        if (argc == 1) { if (exit_trap) { outs("trap -- '"); outs(exit_trap); outs("' EXIT\n"); } return 0; }
        if (argc == 2 && !strcmp(argv[1], "-")) { exit_trap = 0; return 0; }
        if (argc < 3) { errm("trap", "usage: trap 'commands' EXIT"); return 2; }
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "EXIT") || !strcmp(argv[i], "0")) { free(exit_trap); exit_trap = argv[1][0] ? pdup(argv[1]) : 0; }
            else if (strcmp(argv[i], "INT") && strcmp(argv[i], "TERM") && strcmp(argv[i], "HUP")) { errm(argv[i], "unsupported signal for trap"); return 1; }
        }
        return 0;
    }
    if (!strcmp(n, "umask")) {
        if (argc > 1) { u32 v = 0; for (const char *c = argv[1]; *c; c++) { if (*c < '0' || *c > '7') { errm("umask", "bad mask"); return 1; } v = v * 8 + (u32)(*c - '0'); } sys_umask(v); return 0; }
        u32 m = (u32)sys_umask(0); sys_umask(m);
        char b[8]; b[0] = '0'; b[1] = (char)('0' + ((m >> 6) & 7)); b[2] = (char)('0' + ((m >> 3) & 7)); b[3] = (char)('0' + (m & 7)); b[4] = '\n'; b[5] = 0;
        outs(b); return 0;
    }
    if (!strcmp(n, "pwd")) { outs(cwd_now()); outs("\n"); return 0; }
    if (!strcmp(n, "test") || !strcmp(n, "[")) return do_test(argv, argc);
    if (!strcmp(n, "read")) return do_read(argv, argc);
    if (!strcmp(n, "cd")) {
        const char *d = argc > 1 ? argv[1] : var_get("HOME");
        char old[128]; strcpy(old, cwd_now());
        if (d && !strcmp(d, "-")) { d = var_get("OLDPWD"); if (d) { outs(d); outs("\n"); } }
        if (!d) { errm("cd", "no directory"); return 1; }
        if (sys_chdir(d) < 0) { errm(d, zwhy("no such directory")); return 1; }
        set_pwd(old); return 0;
    }
    if (!strcmp(n, "exit")) { last_status = argc > 1 ? atoi(argv[1]) : last_status; exit_requested = 1; return last_status; }
    if (!strcmp(n, "export") || !strcmp(n, "local")) {
        int is_export = n[0] == 'e';
        if (!is_export && !in_func) { errm("local", "can only be used in a function"); return 1; }
        if (!is_export) {
            for (int i = 1; i < argc; i++) {
                char *eq = strchr(argv[i], '=');
                if (eq) *eq = 0;
                int bad = local_save(argv[i]);
                if (eq) { var_set(argv[i], eq + 1, -1); *eq = '='; }
                else if (!bad) var_set(argv[i], "", -1);   /* bash: a local without a value is empty here */
                if (bad) return 1;
            }
            env_dirty = 1; return 0;
        }
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
        if (i < argc || (argc > 1 && !strcmp(argv[1], "--"))) {    /* new parameters: copies on the heap (they outlive this command) */
            int k = argc - i;
            char **nv = malloc((u32)(k + 1) * sizeof(char *));
            if (!nv) { errm("set", "out of memory"); return 1; }
            for (int j = 0; j < k; j++) if (!(nv[j] = malloc(strlen(argv[i + j]) + 1))) { while (j--) free(nv[j]); free(nv); errm("set", "out of memory"); return 1; } else strcpy(nv[j], argv[i + j]);
            nv[k] = 0;
            pos_free(); pos_arg = nv; npos = k; pos_owned = 1;
        }
        return 0;
    }
    if (!strcmp(n, "shift")) {
        int k = 1;
        if (argc > 1) { char *e; long v = strtol(argv[1], &e, 10); if (*e || v < 0) { errm(argv[1], "shift count out of range"); return 1; } k = (int)v; }
        if (k > npos) return 1;
        if (pos_owned) { for (int j = 0; j < k; j++) free(pos_arg[j]); memmove(pos_arg, pos_arg + k, (u32)(npos - k + 1) * sizeof(char *)); }
        else pos_arg += k;                                 /* borrowed: just look further on */
        npos -= k; return 0;
    }
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
    if (!strcmp(n, "unalias")) { for (int i = 1; i < argc; i++) { struct alias *a = alias_find(argv[i]); if (a) { free(a->name); free(a->val); a->name = a->val = 0; a->used = 0; } } return 0; }
    if (!strcmp(n, "history")) {
        if (argc > 1 && !strcmp(argv[1], "-c")) { for (int i = 0; i < nhist; i++) free(hist[i]); nhist = 0; return 0; }
        for (int i = 0; i < nhist; i++) { char b[12]; utoa((u32)i + 1, b); for (u32 k = strlen(b); k < 5; k++) outs(" "); outs(b); outs("  "); outs(hist[i]); outs("\n"); }
        return 0;
    }
    if (!strcmp(n, "source") || !strcmp(n, ".")) {
        if (argc < 2) { errm(n, "file name needed"); return 2; }
        char *text = file_slurp(argv[1], 0);
        if (!text) { errm(argv[1], "cannot read"); return 1; }
        struct possave sv;
        if (argc > 2) pos_push(&sv, argv + 2, argc - 2);
        int st = run_string(text);
        if (argc > 2) pos_pop(&sv);
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
        for (int i = 0; i < MAXJOBS; i++) if (jobs[i].used) { char b[16]; outs("["); utoa((u32)i + 1, b); outs(b); outs("]"); outs(cur_job == i + 1 ? "+ " : "  "); outs(jobs[i].done ? "Done       " : jobs[i].stopped ? "Stopped    " : "Running    "); outs(jobs[i].cmd); outs("\n"); if (jobs[i].done) { free(jobs[i].cmd); jobs[i].used = 0; } }
        return 0;
    }
    if (!strcmp(n, "wait")) {
        if (argc > 1) { int pid = job_pid(argv[1]); if (pid <= 0) return 127; int st = sys_wait(pid); for (int i = 0; i < MAXJOBS; i++) if (jobs[i].used && jobs[i].pid == pid) { free(jobs[i].cmd); jobs[i].used = 0; } return st; }
        for (int i = 0; i < MAXJOBS; i++) if (jobs[i].used) { if (!jobs[i].done) sys_wait(jobs[i].pid); free(jobs[i].cmd); jobs[i].used = 0; }
        return 0;
    }
    if (!strcmp(n, "kill")) {
        int rc = 0, op = 2;                              /* 0 stop, 1 continue, 2 end it */
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] == '-' && argv[i][1]) {
                const char *sg = argv[i] + 1;
                if (!strcmp(sg, "s") && i + 1 < argc) sg = argv[++i];
                if (!strcmp(sg, "STOP") || !strcmp(sg, "TSTP") || !strcmp(sg, "SIGSTOP") || !strcmp(sg, "19") || !strcmp(sg, "20")) op = 0;
                else if (!strcmp(sg, "CONT") || !strcmp(sg, "SIGCONT") || !strcmp(sg, "18")) op = 1;
                else op = 2;
                continue;
            }
            int pid = job_pid(argv[i]);
            if (pid <= 0 || (op == 2 ? sys_kill(pid) : sys_jobctl(pid, op)) < 0) { errm(argv[i], zwhy("no such process")); rc = 1; continue; }
            for (int j = 0; j < MAXJOBS; j++) if (jobs[j].used && jobs[j].pid == pid && op < 2) { jobs[j].stopped = op == 0; for (int q = 0; q < jobs[j].np; q++) sys_jobctl(jobs[j].pids[q], op); }
        }
        return rc;
    }
    if (!strcmp(n, "type")) { int rc = 0; for (int i = 1; i < argc; i++) rc |= do_type(argv[i]); return rc; }
    if (!strcmp(n, "help")) {
        outs("Zenix shell. Line editing: arrows, Home/End, Ctrl-A/E/B/F/K/U/W/L, history Up/Down, Tab completes.\n"
             "Language: VAR=value, $VAR, $(cmd), * ? [..], | && || ; &, < > >> 2> 2>&1 << EOF,\n"
             "  if/elif/else/fi, while/until/for/do/done, name() { ...; }, alias, scripts (sh file).\n"
             "Builtins: cd exit export unset set alias unalias history source read shift return break continue\n"
             "  jobs fg bg wait kill type test [ true false : echo pwd eval local umask help\n");
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
        case '$': add = sys_geteuid() == 0 ? "#" : "$"; break;
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
        { "TERM", "vt100" }, { "COLUMNS", "80" }, { 0, 0 } };
    for (int i = 0; defs[i][0]; i++) if (!var_get(defs[i][0])) var_set(defs[i][0], defs[i][1], 1);
    if (!var_get("LINES")) { char b[12]; int ti = sys_ttyinfo(); utoa(ti > 0 ? (u32)ti >> 16 : 25, b); var_set("LINES", b, 1); }   /* 32 PAL, 25 NTSC */
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

static int shell_main(int argc, char **argv)
{
    /* The arena holds the parsed commands of one input line or script; the long scripts of the tests peak near
     * 180 KB, an interactive session needs a few KB. Take an eighth of the free memory (the arena goes to fast RAM
     * when there is any), between 64 and 256 KB, so a small machine keeps room for a second shell. */
    u32 mi[4]; sys_meminfo(mi);
    u32 asize = (mi[1] ? mi[1] : mi[0]) / 8;
    if (asize > ARENA) asize = ARENA;
    if (asize < 65536) asize = 65536;
    while (!(arena = sys_memalloc(asize)) && asize > 32768) asize /= 2;
    if (!arena) { errm("no memory", 0); return 2; }
    abrk = arena; aend = arena + asize;
    shell_pid = sys_getpid();
    import_environment();
    init_vars();
    int ai = 1; char *cmdstr = 0, *script = 0;
    if (argc > 1 && !strcmp(argv[1], "-c")) { if (argc < 3) { errm("-c", "needs a command"); return 2; } cmdstr = argv[2]; ai = 3; if (ai < argc) { arg0 = argv[ai]; ai++; } }
    else if (argc > 1) { script = argv[1]; arg0 = argv[1]; ai = 2; }
    if (ai < argc) { pos_arg = argv + ai; npos = argc - ai; }      /* the shell's own arguments: they live as long as it */
    if (cmdstr) { int st = run_string(cmdstr); return exit_requested ? last_status : st; }
    if (script) {
        char *t = file_slurp(script, 0);
        if (!t) { errm(script, zwhy("cannot open")); return 127; }
        int st = run_string(t);
        return exit_requested ? last_status : st;
    }
    interactive = 1;
    run_startup_file("/etc/profile");
    { char p[160]; const char *h = var_get("HOME"); strncpy(p, h ? h : "/", 150); p[150] = 0; u32 l = strlen(p); if (l && p[l - 1] != '/') p[l++] = '/'; strcpy(p + l, ".shrc");
      struct ustat st;                    /* only a ~/.shrc of the effective user (or root): never another user's commands as root */
      if (sys_stat(p, &st) == 0 && (st.uid == (u32)sys_geteuid() || st.uid == 0)) run_startup_file(p); }
    abrk = arena;
    load_history();
    interactive_loop();
    return last_status;
}

int main(int argc, char **argv)
{
    { char *sp; __asm__ volatile("move.l %%sp,%0" : "=r"(sp)); stk_top = sp; }
    int st = shell_main(argc, argv);
    if (exit_trap) {                                    /* trap '...' EXIT */
        char *t = exit_trap; exit_trap = 0; exit_requested = 0;
        abrk = arena;
        run_string(t);
    }
    return st;
}
