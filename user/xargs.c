// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
/* xargs [-n N] [-I STR] [command [args]]: arguments from standard input (words, with quotes) appended to the command,
 * run in batches. A batch holds at most -n words, and never more than one spawn takes (256 arguments, 8 KB of text):
 * more words make more batches, none is dropped. The command is found through $PATH, as the shell does. */
#define ARGC_MAX  256           /* the kernel's spawn limits (kernel/syscall.c SPAWN_ARGC, SPAWN_ARGBYTES) */
#define ARGB_MAX  8192

/* s with every occurrence of pat (not empty) replaced by with */
static char *subst(const char *s, const char *pat, const char *with)
{
    u32 pl = strlen(pat), wl = strlen(with), n = 0;
    for (const char *q = s; *q; ) { if (!strncmp(q, pat, pl)) { n++; q += pl; } else q++; }
    char *out = xmalloc(strlen(s) + 1 + n * wl), *o = out;
    while (*s) { if (!strncmp(s, pat, pl)) { memcpy(o, with, wl); o += wl; s += pl; } else *o++ = *s++; }
    *o = 0;
    return out;
}

static char **cmd; static int nc, rc;
static const char *repl;

static void run(char **items, int from, int count)
{
    char **av = xmalloc((u32)(nc + count + 1) * sizeof(char *));
    int n = 0;
    if (repl) for (int q = 0; q < nc; q++) av[n++] = subst(cmd[q], repl, items[from]);
    else { for (int q = 0; q < nc; q++) av[n++] = cmd[q]; for (int q = 0; q < count; q++) av[n++] = items[from + q]; }
    av[n] = 0;
    char prog[ZPATH_MAX];
    oflush();
    if (!path_find(av[0], prog, sizeof prog)) { dprintf(2, "xargs: %s: command not found\n", av[0]); rc = 127; }
    else {
        int pid = sys_spawn(prog, av, 0);
        if (pid < 0) { dprintf(2, "xargs: %s: %s\n", av[0], zwhy("cannot run")); rc = 127; }
        else if (sys_wait(pid)) rc = 123;
    }
    if (repl) for (int q = 0; q < nc; q++) free(av[q]);
    free(av);
}

int main(int argc, char **argv)
{
    int i = 1, maxn = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) { maxn = atoi(argv[++i]); if (maxn < 1) { dprintf(2, "xargs: -n needs a number of at least 1\n"); return 2; } }
        else if (!strcmp(argv[i], "-I") && i + 1 < argc) { repl = argv[++i]; if (!repl[0]) { dprintf(2, "xargs: -I needs a non-empty string\n"); return 2; } }
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { dprintf(2, "xargs: unknown option %s\n", argv[i]); return 2; }
    }
    static char *echo_cmd[] = { "echo", 0 };
    if (i >= argc) { cmd = echo_cmd; nc = 1; } else { cmd = argv + i; nc = argc - i; }
    u32 fixed = 0;
    for (int q = 0; q < nc; q++) fixed += strlen(cmd[q]) + 1;
    if (nc >= ARGC_MAX || fixed >= ARGB_MAX) { dprintf(2, "xargs: the command alone is too long to run\n"); return 2; }

    int cap = 64, ni = 0; u32 bytes = fixed;
    char **items = xmalloc((u32)cap * sizeof(char *));
    u32 tcap = 256; char *tok = xmalloc(tcap);
    struct rd r; rd_init(&r, 0);
    int c;
    for (;;) {
        while ((c = rd_getc(&r)) >= 0 && (repl ? c == '\n' : isspace(c))) ;
        if (c < 0) break;
        u32 n = 0;
        #define PUT(ch) do { if (n + 2 > tcap) { tcap *= 2; char *nt = realloc(tok, tcap); if (!nt) { dprintf(2, "xargs: out of memory\n"); return 1; } tok = nt; } tok[n++] = (char)(ch); } while (0)
        if (repl) { do PUT(c); while ((c = rd_getc(&r)) >= 0 && c != '\n'); }
        else if (c == '"' || c == '\'') { int q = c; while ((c = rd_getc(&r)) >= 0 && c != q) PUT(c); }
        else { do PUT(c); while ((c = rd_getc(&r)) >= 0 && !isspace(c)); }
        tok[n] = 0;
        if (n + 1 + fixed > ARGB_MAX) { dprintf(2, "xargs: an argument is too long to run (%u bytes)\n", n); return 1; }
        if (repl) { char *one[1] = { tok }; run(one, 0, 1); continue; }
        if (ni && (nc + ni + 1 > ARGC_MAX || bytes + n + 1 > ARGB_MAX)) {     /* this word does not fit: run what is there first */
            run(items, 0, ni);
            for (int q = 0; q < ni; q++) free(items[q]);
            ni = 0; bytes = fixed;
        }
        if (ni == cap) { cap *= 2; char **nv = realloc(items, (u32)cap * sizeof(char *)); if (!nv) { dprintf(2, "xargs: out of memory\n"); return 1; } items = nv; }
        if (!(items[ni++] = strdup(tok))) { dprintf(2, "xargs: out of memory\n"); return 1; }
        bytes += n + 1;
        if (maxn && ni == maxn) { run(items, 0, ni); for (int q = 0; q < ni; q++) free(items[q]); ni = 0; bytes = fixed; }
    }
    if (!repl && ni) run(items, 0, ni);
    return rc;
}
