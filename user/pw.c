// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"

/* read a whole small file into a static buffer */
static char *slurp(const char *path)
{
    static char buf[4096];
    int fd = sys_open(path, O_RDONLY), n = 0, r;
    if (fd < 0) return 0;
    while (n < (int)sizeof buf - 1 && (r = sys_read(fd, buf + n, sizeof buf - 1 - (u32)n)) > 0) n += r;
    sys_close(fd);
    buf[n] = 0;
    return buf;
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
        if (*l && *l != '#' && fields(l, f, 7) == 7 && (has_uid ? (u32)atoi(f[2]) == uid : !strcmp(f[0], name))) {
            copy(out->name, f[0], sizeof out->name); copy(out->hash, f[1], sizeof out->hash);
            out->uid = (u32)atoi(f[2]); out->gid = (u32)atoi(f[3]);
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
        if (*l && *l != '#' && fields(l, f, 4) == 4 && (has_gid ? (u32)atoi(f[2]) == gid : !strcmp(f[0], name))) {
            copy(out->name, f[0], sizeof out->name); out->gid = (u32)atoi(f[2]); copy(out->members, f[3], sizeof out->members);
            return 0;
        }
        if (!e) break;
        l = e + 1;
    }
    return -1;
}
int gr_byname(const char *name, struct gr *out) { return gr_find(name, 0, 0, out); }
int gr_bygid(u32 gid, struct gr *out) { return gr_find(0, 1, gid, out); }

const char *pw_name(u32 uid, char *buf)
{
    static struct { u32 uid; char name[32]; int ok; } cache[4]; static int next;     /* ls asks for the same few ids again and again */
    for (int i = 0; i < 4; i++) if (cache[i].ok && cache[i].uid == uid) return cache[i].name;
    struct pw p;
    int i = next++ & 3;
    cache[i].uid = uid; cache[i].ok = 1;
    if (pw_byuid(uid, &p) == 0) copy(cache[i].name, p.name, 32); else snprintf(cache[i].name, 32, "%u", uid);
    (void)buf;
    return cache[i].name;
}
const char *gr_name(u32 gid, char *buf)
{
    static struct { u32 gid; char name[32]; int ok; } cache[4]; static int next;
    for (int i = 0; i < 4; i++) if (cache[i].ok && cache[i].gid == gid) return cache[i].name;
    struct gr g;
    int i = next++ & 3;
    cache[i].gid = gid; cache[i].ok = 1;
    if (gr_bygid(gid, &g) == 0) copy(cache[i].name, g.name, 32); else snprintf(cache[i].name, 32, "%u", gid);
    (void)buf;
    return cache[i].name;
}

/* prompt and read a line without echoing it (for passwords); 0 ok, -1 on ^C or end of input */
int read_secret(const char *prompt, char *buf, int max)
{
    int n = 0, rc = 0;
    fputs(prompt, 1);
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
    fputs("\n", 1);
    return rc;
}
