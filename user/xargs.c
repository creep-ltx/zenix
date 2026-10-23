#include "libz.h"
/* xargs [-n N] [-I STR] [command [args]]: arguments from standard input (words, with quotes) appended to the command, run in batches */
/* s with every occurrence of pat replaced by with */
static char *subst(const char *s, const char *pat, const char *with)
{
    u32 pl = strlen(pat), wl = strlen(with);
    char *out = xmalloc(strlen(s) + 1 + (wl > pl ? (wl - pl) * (strlen(s) / pl + 1) : 0)); char *o = out;
    while (*s) { if (!strncmp(s, pat, pl)) { memcpy(o, with, wl); o += wl; s += pl; } else *o++ = *s++; }
    *o = 0;
    return out;
}

int main(int argc, char **argv)
{
    int i = 1, maxn = 0; const char *repl = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) maxn = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-I") && i + 1 < argc) repl = argv[++i];
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else { fprintf(2, "xargs: unknown option %s\n", argv[i]); return 2; }
    }
    char *cmd[64]; int nc = 0;
    if (i >= argc) cmd[nc++] = "echo"; else for (; i < argc && nc < 30; i++) cmd[nc++] = argv[i];
    char *items[256]; int ni = 0; char tok[512]; struct rd r; rd_init(&r, 0);
    int rc = 0;
    #define RUN(from, count) do { char *av[300]; int n = 0; \
        if (repl) { for (int q = 0; q < nc; q++) av[n++] = subst(cmd[q], repl, items[from]); } \
        else { for (int q = 0; q < nc; q++) av[n++] = cmd[q]; for (int q = 0; q < (count) && n < 290; q++) av[n++] = items[(from) + q]; } \
        av[n] = 0; char prog[128]; if (strchr(av[0], '/')) strcpy(prog, av[0]); else { strcpy(prog, "/bin/"); strcat(prog, av[0]); } \
        oflush(); int pid = sys_spawn(prog, av, 0); if (pid < 0) { fprintf(2, "xargs: %s: cannot run\n", av[0]); rc = 127; } else { int st = sys_wait(pid); if (st) rc = 123; } } while (0)
    int c;
    for (;;) {
        while ((c = rd_getc(&r)) >= 0 && (repl ? c == '\n' : isspace(c))) ;
        if (c < 0) break;
        int n = 0;
        if (repl) { do { if (n < 510) tok[n++] = (char)c; } while ((c = rd_getc(&r)) >= 0 && c != '\n'); }
        else if (c == '"' || c == '\'') { int q = c; while ((c = rd_getc(&r)) >= 0 && c != q && n < 510) tok[n++] = (char)c; }
        else { do { tok[n++] = (char)c; } while ((c = rd_getc(&r)) >= 0 && !isspace(c) && n < 510); }
        tok[n] = 0;
        if (ni < 256 && !(items[ni++] = strdup(tok))) { fprintf(2, "xargs: out of memory\n"); return 1; }
        if (repl) { RUN(ni - 1, 1); ni = 0; }
        else if (maxn && ni == maxn) { RUN(0, ni); ni = 0; }
    }
    if (!repl && ni) RUN(0, ni);
    return rc;
}
