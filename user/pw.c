// SPDX-License-Identifier: GPL-2.0-only
/* The names of ids, and the editing of /etc/passwd and /etc/group (useradd, passwd ...); the parser is zpw.c */
#include "pw.h"

static void copy(char *dst, const char *src, u32 max) { strncpy(dst, src, max - 1); dst[max - 1] = 0; }

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
int pw_initgroups(const char *who, const struct pw *p)
{
    unsigned short g[NGROUPS];
    int n = gr_list(p->name, p->gid, g, NGROUPS);
    if (n > NGROUPS) {
        dprintf(2, "%s: %s is in %d groups; only the first %d count\n", who, p->name, n, NGROUPS);
        n = NGROUPS;
    }
    return sys_setgroups(g, (u32)n);
}
int in_group(u32 gid)
{
    unsigned short g[NGROUPS];
    if ((u32)sys_getgid() == gid) return 1;
    int n = sys_getgroups(g, NGROUPS);
    for (int i = 0; i < n; i++) if (g[i] == gid) return 1;
    return 0;
}
int in_group_named(const char *name)
{
    struct gr g;
    return gr_byname(name, &g) == 0 && in_group(g.gid);
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

/* replace a whole file safely: write PATH.new, set its mode, then rename it over PATH (atomic: the rename is crash-safe
 * on ext2 and ZenixFS). 0, or -1 after a message naming WHO. */
int pw_replace(const char *who, const char *path, const char *data, u32 len, u32 mode)
{
    char tmp[64];
    if (strlen(path) + 5 > sizeof tmp) { dprintf(2, "%s: %s: path too long\n", who, path); return -1; }
    strcpy(tmp, path); strcat(tmp, ".new");
    int fd = sys_open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0 || sys_write(fd, data, len) != (int)len) {
        dprintf(2, "%s: %s: %s\n", who, tmp, zwhy("cannot write"));
        if (fd >= 0) { sys_close(fd); sys_unlink(tmp); }
        return -1;
    }
    sys_close(fd);
    sys_chmod(tmp, mode);
    if (sys_rename(tmp, path) < 0) { dprintf(2, "%s: cannot replace %s: %s\n", who, path, zwhy("failed")); sys_unlink(tmp); return -1; }
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
    if (!old) { dprintf(2, "%s: cannot read /etc/group\n", who); return -1; }
    u32 nl = strlen(name), gl = strlen(group), o = 0;
    char *nf = malloc((u32)n + nl + 4);
    if (!nf) { dprintf(2, "%s: out of memory\n", who); free(old); return -1; }
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
    if (!done) { dprintf(2, "%s: %s: no such group in /etc/group\n", who, group); free(nf); return -1; }
    int rc = pw_replace(who, "/etc/group", nf, o, 0644);
    free(nf);
    return rc;
}
