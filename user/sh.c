#include "libz.h"

/* The Zenix shell: pipes, < > >> redirection, quotes, ; and &, builtins cd,
 * exit, help. */

#define MAXARG 16
#define MAXCMD 8

struct cmd {
    char *argv[MAXARG + 1];
    int argc;
    char *in;
    char *out;
    int append;
};

static int interactive;
/* words are copied here so operators in the line can stay in place */
static char pool[1024];
static int pool_used;
static int in_fd;
static char inbuf[512];
static int inlen, inpos;

/* next line from in_fd into line; 0 at end of input */
static int readline(char *line, int max)
{
    int n = 0;
    for (;;) {
        if (inpos >= inlen) {
            inlen = sys_read(in_fd, inbuf, sizeof inbuf);
            inpos = 0;
            if (inlen <= 0) { inlen = 0; if (n) break; return 0; }
        }
        char c = inbuf[inpos++];
        if (c == '\n') break;
        if (n < max - 1) line[n++] = c;
    }
    line[n] = 0;
    return 1;
}

/* split `s` into words in place; returns the operator that ended the
 * command: 0 end, '|', ';', '&'. Sets *rest to the text after it. */
static int parse_cmd(char *s, struct cmd *c, char **rest)
{
    c->argc = 0;
    c->in = 0;
    c->out = 0;
    c->append = 0;
    int want_in = 0, want_out = 0;
    for (;;) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) { *rest = s; return 0; }
        if (*s == '|' || *s == ';' || *s == '&') {
            int op = *s;
            *s = 0;
            *rest = s + 1;
            return op;
        }
        if (*s == '<') { want_in = 1; s++; continue; }
        if (*s == '>') {
            want_out = 1;
            s++;
            if (*s == '>') { c->append = 1; s++; }
            continue;
        }
        char *w = pool + pool_used;
        char *out = w;
        while (*s && *s != ' ' && *s != '\t' && *s != '|' && *s != ';' &&
               *s != '&' && *s != '<' && *s != '>') {
            if (*s == '"' || *s == '\'') {
                char q = *s++;
                while (*s && *s != q && pool_used + (out - w) < (int)sizeof pool - 1)
                    *out++ = *s++;
                if (*s) s++;
            } else if (pool_used + (out - w) < (int)sizeof pool - 1) {
                *out++ = *s++;
            } else {
                s++;
            }
        }
        *out++ = 0;
        pool_used += (int)(out - w);
        if (want_in) { c->in = w; want_in = 0; }
        else if (want_out) { c->out = w; want_out = 0; }
        else if (c->argc < MAXARG) c->argv[c->argc++] = w;
    }
}

static void status(int pid, const char *name, int code)
{
    if (code >= 128 && interactive)
        printf("[%s (pid %d) %s]\n", name, pid, code == 130 ? "interrupted" : "killed");
}

static void run_pipeline(struct cmd *c, int n, int background)
{
    int pids[MAXCMD], prev = -1;
    for (int i = 0; i < n; i++) {
        char path[80];
        const char *name = c[i].argv[0];
        if (strchr(name, '/')) strcpy(path, name);
        else { strcpy(path, "/bin/"); strcpy(path + 5, name); }
        c[i].argv[c[i].argc] = 0;

        int p[2] = { -1, -1 }, stdio[3] = { -1, -1, -1 }, infd = -1, outfd = -1;
        if (i < n - 1 && sys_pipe(p) < 0) { fprintf(2, "sh: pipe failed\n"); break; }
        if (c[i].in) {
            infd = sys_open(c[i].in, 0);
            if (infd < 0) { fprintf(2, "sh: %s: cannot open\n", c[i].in); pids[i] = -1; goto next; }
            stdio[0] = infd;
        } else if (prev >= 0) stdio[0] = prev;
        if (c[i].out) {
            outfd = sys_open(c[i].out, O_WRONLY | O_CREAT | (c[i].append ? O_APPEND : O_TRUNC));
            if (outfd < 0) { fprintf(2, "sh: %s: cannot write\n", c[i].out); pids[i] = -1; goto next; }
            stdio[1] = outfd;
        } else if (p[1] >= 0) stdio[1] = p[1];
        pids[i] = sys_spawn(path, c[i].argv, stdio);
        if (pids[i] < 0) fprintf(2, "sh: %s: not found\n", name);
    next:
        if (infd >= 0) sys_close(infd);
        if (outfd >= 0) sys_close(outfd);
        if (p[1] >= 0) sys_close(p[1]);
        if (prev >= 0) sys_close(prev);
        prev = p[0];
    }
    if (prev >= 0) sys_close(prev);
    if (background) {
        if (pids[n - 1] >= 0) printf("[%d]\n", pids[n - 1]);
        return;
    }
    if (pids[n - 1] >= 0) sys_setfg(pids[n - 1]);
    for (int i = 0; i < n; i++)
        if (pids[i] >= 0) status(pids[i], c[i].argv[0], sys_wait(pids[i]));
    sys_setfg(0);
}

static int builtin(struct cmd *c)
{
    const char *n = c->argv[0];
    if (!strcmp(n, "cd")) {
        const char *d = c->argc > 1 ? c->argv[1] : "/";
        if (sys_chdir(d) < 0) fprintf(2, "cd: %s: no such directory\n", d);
        return 1;
    }
    if (!strcmp(n, "exit")) sys_exit(c->argc > 1 ? atoi(c->argv[1]) : 0);
    if (!strcmp(n, "help")) {
        printf("builtins: cd exit help\n"
               "programs in /bin: ls cat echo pwd ps free wc grep uname mkdir rmdir rm cp mv touch\n"
               "                  hello spin sh\n"
               "use | to pipe, < > >> to redirect, & to run in the background, ^C to stop\n");
        return 1;
    }
    return 0;
}

static void run_line(char *line)
{
    char *s = line;
    pool_used = 0;
    while (*s) {
        struct cmd c[MAXCMD];
        int n = 0, op, bg = 0, bad = 0;
        do {
            char *rest;
            op = parse_cmd(s, &c[n], &rest);
            s = rest;
            if (c[n].argc < 0) { bad = 1; break; }
            if (c[n].argc == 0 && (op == '|' || n > 0)) {
                if (op || n) { fprintf(2, "sh: empty command\n"); bad = 1; }
                break;
            }
            if (c[n].argc > 0) n++;
        } while (op == '|' && n < MAXCMD);
        if (op == '&') bg = 1;
        if (bad) return;
        if (n == 0) { if (!op) return; continue; }
        if (n == 1 && builtin(&c[0])) continue;
        run_pipeline(c, n, bg);
    }
}

int main(int argc, char **argv)
{
    char line[256], cwd[96];
    if (argc > 1) {
        in_fd = sys_open(argv[1], 0);
        if (in_fd < 0) { fprintf(2, "sh: %s: cannot open\n", argv[1]); return 1; }
    } else {
        in_fd = 0;
        interactive = 1;
    }
    for (;;) {
        if (interactive) {
            sys_getcwd(cwd, sizeof cwd);
            printf("zenix:%s# ", cwd);
        }
        if (!readline(line, sizeof line)) { if (interactive) printf("\n"); return 0; }
        run_line(line);
    }
}
