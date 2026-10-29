/* SPDX-License-Identifier: GPL-2.0-only */
/* /etc/passwd and /etc/group read, and a secret read without echo (user/zpw.h): one source for libz's programs (pw.c's
 * callers) and the C library's getpwnam, getpwuid, getgrnam, getgrgid and getpass (user/zshare.h) */
#include "zshare.h"

/* read a whole file into a malloc'd buffer (64 KB at most: more is refused, never cut) */
char *pw_slurp(const char *path, int *len)
{
    int fd = sys_open(path, O_RDONLY), n = 0, r;
    u32 cap = 4096;
    if (fd < 0) return 0;
    char *buf = malloc(cap);
    while (buf) {
        if ((u32)n + 1 >= cap) {
            if (cap >= 65536) {             /* said without the formatter (the C library's getpw* link none) */
                static const char msg[] = " is larger than 64 KB: not read\n";
                sys_write(2, path, strlen(path));
                sys_write(2, msg, sizeof msg - 1);
                free(buf); buf = 0; break;
            }
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = 0; break; }
            buf = nb; cap *= 2;
        }
        r = sys_read(fd, buf + n, cap - 1 - (u32)n);
        if (r < 0) { free(buf); buf = 0; break; }
        if (r == 0) break;
        n += r;
    }
    sys_close(fd);
    if (!buf) return 0;
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}
static char *slurp(const char *path)          /* the last file read stays until the next call */
{
    static char *last;
    free(last);
    return last = pw_slurp(path, 0);
}

/* a uid/gid field: decimal digits only, 0-65535 (an empty or non-numeric field is not 0, which is root) */
static int idnum(const char *s, u32 *out)
{
    u32 v = 0;
    if (!*s) return 0;
    for (; *s; s++) { if (*s < '0' || *s > '9') return 0; v = v * 10 + (u32)(*s - '0'); if (v > 65535) return 0; }
    *out = v;
    return 1;
}

/* split one line at ':' into up to n fields (in place) */
static int fields(char *line, char **f, int n)
{
    int i = 0;
    f[i++] = line;
    for (char *p = line; *p && i < n; p++)
        if (*p == ':') { *p = 0; f[i++] = p + 1; }
    return i;
}

static void copy(char *dst, const char *src, u32 max) { strncpy(dst, src, max - 1); dst[max - 1] = 0; }

static int pw_find(const char *name, int has_uid, u32 uid, struct pw *out)
{
    char *file = slurp("/etc/passwd");
    if (!file) return -1;
    for (char *l = file; *l; ) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        char *f[8];
        u32 u, g;
        if (*l && *l != '#' && fields(l, f, 7) == 7 && idnum(f[2], &u) && idnum(f[3], &g) &&
            (has_uid ? u == uid : !strcmp(f[0], name))) {
            copy(out->name, f[0], sizeof out->name); copy(out->hash, f[1], sizeof out->hash);
            out->uid = u; out->gid = g;
            copy(out->comment, f[4], sizeof out->comment); copy(out->home, f[5], sizeof out->home); copy(out->shell, f[6], sizeof out->shell);
            return 0;
        }
        if (!e) break;
        l = e + 1;
    }
    return -1;
}
int pw_byname(const char *name, struct pw *out) { return pw_find(name, 0, 0, out); }
int pw_byuid(u32 uid, struct pw *out) { return pw_find(0, 1, uid, out); }

static int gr_find(const char *name, int has_gid, u32 gid, struct gr *out)
{
    char *file = slurp("/etc/group");
    if (!file) return -1;
    for (char *l = file; *l; ) {
        char *e = strchr(l, '\n'); if (e) *e = 0;
        char *f[5];
        u32 g;
        if (*l && *l != '#' && fields(l, f, 4) == 4 && idnum(f[2], &g) && (has_gid ? g == gid : !strcmp(f[0], name))) {
            copy(out->name, f[0], sizeof out->name); out->gid = g; copy(out->members, f[3], sizeof out->members);
            return 0;
        }
        if (!e) break;
        l = e + 1;
    }
    return -1;
}
int gr_byname(const char *name, struct gr *out) { return gr_find(name, 0, 0, out); }
int gr_bygid(u32 gid, struct gr *out) { return gr_find(0, 1, gid, out); }

/* prompt and read a line without echoing it (for passwords); 0 ok, -1 on ^C or end of input */
int read_secret(const char *prompt, char *buf, int max)
{
    int n = 0, rc = 0;
    sys_write(1, prompt, strlen(prompt));
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    for (;;) {
        char c;
        if (sys_read(0, &c, 1) != 1 || c == 3 || c == 4) { rc = -1; break; }
        if (c == '\r' || c == '\n') break;
        if (c == 8 || c == 127) { if (n) n--; }
        else if (c == 21) n = 0;
        else if (n < max - 1) buf[n++] = c;
    }
    sys_ttymode(0);
    buf[n] = 0;
    sys_write(1, "\n", 1);
    return rc;
}
