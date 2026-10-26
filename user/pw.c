// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"

/* read a whole file into a malloc'd buffer (64 KB at most: more is refused, never cut) */
char *pw_slurp(const char *path, int *len)
{
    int fd = sys_open(path, O_RDONLY), n = 0, r;
    u32 cap = 4096;
    if (fd < 0) return 0;
    char *buf = malloc(cap);
    while (buf) {
        if ((u32)n + 1 >= cap) {
            if (cap >= 65536) { fprintf(2, "%s is larger than 64 KB: not read\n", path); free(buf); buf = 0; break; }
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

/* replace a whole file safely: write PATH.new, set its mode, then rename it over PATH (atomic: the rename is crash-safe
 * on ext2 and ZenixFS). 0, or -1 after a message naming WHO. */
int pw_replace(const char *who, const char *path, const char *data, u32 len, u32 mode)
{
    char tmp[64];
    if (strlen(path) + 5 > sizeof tmp) { fprintf(2, "%s: %s: path too long\n", who, path); return -1; }
    strcpy(tmp, path); strcat(tmp, ".new");
    int fd = sys_open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0 || sys_write(fd, data, len) != (int)len) {
        fprintf(2, "%s: %s: %s\n", who, tmp, zwhy("cannot write"));
        if (fd >= 0) { sys_close(fd); sys_unlink(tmp); }
        return -1;
    }
    sys_close(fd);
    sys_chmod(tmp, mode);
    if (sys_rename(tmp, path) < 0) { fprintf(2, "%s: cannot replace %s: %s\n", who, path, zwhy("failed")); sys_unlink(tmp); return -1; }
    return 0;
}

/* is NAME in a comma-separated member list? */
int gr_has(const char *list, const char *name)
{
    u32 nl = strlen(name);
    for (const char *p = list; *p; ) {
        const char *c = strchr(p, ','); u32 len = c ? (u32)(c - p) : strlen(p);
        if (len == nl && !strncmp(p, name, nl)) return 1;
        if (!c) break;
        p = c + 1;
    }
    return 0;
}

/* take NAME out of a comma-separated member list, in place */
void gr_drop(char *list, const char *name)
{
    char out[256]; u32 o = 0, nl = strlen(name);
    for (char *p = list; *p; ) {
        char *c = strchr(p, ','); u32 len = c ? (u32)(c - p) : strlen(p);
        if (!(len == nl && !strncmp(p, name, nl)) && len && o + len + 2 < sizeof out) {
            if (o) out[o++] = ',';
            memcpy(out + o, p, len); o += len;
        }
        if (!c) break;
        p = c + 1;
    }
    out[o] = 0; strcpy(list, out);
}

/* add NAME to (ADD) or take it out of GROUP's member list in /etc/group, replacing the file safely. The caller checks
 * first whether NAME is a member already. 0, or -1 after a message naming WHO. */
int gr_edit(const char *who, const char *group, const char *name, int add)
{
    int n = 0;
    char *old = pw_slurp("/etc/group", &n);
    if (!old) { fprintf(2, "%s: cannot read /etc/group\n", who); return -1; }
    u32 nl = strlen(name), gl = strlen(group), o = 0;
    char *nf = malloc((u32)n + nl + 4);
    if (!nf) { fprintf(2, "%s: out of memory\n", who); free(old); return -1; }
    int done = 0;
    for (char *l = old; *l; ) {
        char *e = strchr(l, '\n'); u32 len = e ? (u32)(e - l) : strlen(l);
        char line[300];
        if (!done && len > gl && len < 256 && l[gl] == ':' && !strncmp(l, group, gl)) {
            memcpy(line, l, len); line[len] = 0;
            char *f = line; int k = 0;                       /* the 4th field: name:pass:gid:members */
            for (; *f && k < 3; f++) if (*f == ':') k++;
            if (k == 3) {
                if (!add) gr_drop(f, name);
                else { if (*f) strcat(f, ","); strcat(f, name); }
                done = 1;
            }
            u32 ll = strlen(line); memcpy(nf + o, line, ll); o += ll;
        } else { memcpy(nf + o, l, len); o += len; }
        nf[o++] = '\n';
        if (!e) break;
        l = e + 1;
    }
    free(old);
    if (!done) { fprintf(2, "%s: %s: no such group in /etc/group\n", who, group); free(nf); return -1; }
    int rc = pw_replace(who, "/etc/group", nf, o, 0644);
    free(nf);
    return rc;
}
