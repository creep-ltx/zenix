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
int gr_edit(const char *who, const char *group, const char *name, int add) { return gr_edit_at(who, "/etc/group", group, name, add); }
int gr_edit_at(const char *who, const char *path, const char *group, const char *name, int add)
{
    int n = 0;
    char *old = pw_slurp(path, &n);
    if (!old) { dprintf(2, "%s: cannot read %s\n", who, path); return -1; }
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
    if (!done) { dprintf(2, "%s: %s: no such group in %s\n", who, group, path); free(nf); return -1; }
    int rc = pw_replace(who, path, nf, o, 0644);
    free(nf);
    return rc;
}

/* ---- making and changing /etc/passwd lines: passwd, useradd and zinstall (a new root's files) share these ---- */
int pw_name_ok(const char *n)
{
    if (!*n || strlen(n) > 31 || !((*n >= 'a' && *n <= 'z') || *n == '_')) return 0;
    for (const char *p = n; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    return 1;
}
int pw_field_ok(const char *s) { for (; *s; s++) if (*s == ':' || *s == '\n') return 0; return 1; }

void pw_new_hash(const char *password, char out[100])
{
    out[0] = 0;
    if (!password[0]) return;
    char salt[12]; u32 t = sys_hrtime() ^ ((u32)sys_time() << 7);
    for (int i = 0; i < 8; i++) { salt[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[t % 36]; t = t * 1103515245u + 12345u; }
    salt[8] = 0;
    pw_hash(password, salt, out);
}

/* the field number K (0-based) of a line of LEN bytes: its start and length, or -1 */
static int field_of(const char *l, u32 len, int k, const char **f, u32 *fl)
{
    const char *p = l, *end = l + len;
    for (int i = 0; i < k; i++) { const char *c = memchr(p, ':', (u32)(end - p)); if (!c) return -1; p = c + 1; }
    const char *c = memchr(p, ':', (u32)(end - p));
    *f = p; *fl = c ? (u32)(c - p) : (u32)(end - p);
    return 0;
}
static int field_num(const char *f, u32 fl, u32 *out)
{
    u32 v = 0;
    if (!fl || fl > 5) return -1;
    for (u32 i = 0; i < fl; i++) { if (f[i] < '0' || f[i] > '9') return -1; v = v * 10 + (u32)(f[i] - '0'); }
    if (v > 65535) return -1;
    *out = v;
    return 0;
}

int pw_set_hash_at(const char *who, const char *path, const char *name, const char *hash)
{
    int n = 0;
    char *old_file = pw_slurp(path, &n);                 /* the whole file: no user past some size may be lost */
    if (!old_file) { dprintf(2, "%s: cannot read %s\n", who, path); return -1; }
    u32 lines = 1;
    for (int k = 0; k < n; k++) if (old_file[k] == '\n') lines++;
    u32 cap = (u32)n + lines * (strlen(hash) + 2);      /* each line may get the new hash and a newline */
    char *new_file = malloc(cap);
    if (!new_file) { dprintf(2, "%s: out of memory\n", who); free(old_file); return -1; }
    u32 o = 0, nl = strlen(name), found = 0;
    for (char *l = old_file; *l; ) {
        char *e = strchr(l, '\n'); u32 len = e ? (u32)(e - l) : strlen(l);
        char *c1 = memchr(l, ':', len);
        if (c1 && (u32)(c1 - l) == nl && !strncmp(l, name, nl)) {
            char *c2 = memchr(c1 + 1, ':', len - (u32)(c1 + 1 - l));
            if (c2) {                                      /* name: new hash, then the rest of the line from the next colon */
                u32 hl = strlen(hash), rest = len - (u32)(c2 - l);
                memcpy(new_file + o, name, nl); o += nl; new_file[o++] = ':';
                memcpy(new_file + o, hash, hl); o += hl;
                memcpy(new_file + o, c2, rest); o += rest; new_file[o++] = '\n';
                found = 1;
            }
        } else { memcpy(new_file + o, l, len); o += len; new_file[o++] = '\n'; }
        if (!e) break;
        l = e + 1;
    }
    free(old_file);
    int rc = found ? pw_replace(who, path, new_file, o, 0644) : -1;
    if (!found) dprintf(2, "%s: %s: no such user\n", who, name);
    free(new_file);
    return rc;
}

int pw_add_at(const char *who, const char *path, const char *gpath, const struct pw_add *a, u32 *uidp, u32 *gidp)
{
    int n = 0, gn = 0;
    if (!pw_name_ok(a->name)) { dprintf(2, "%s: %s: a name is 1-31 of a-z 0-9 _ - and starts with a letter or _\n", who, a->name); return 2; }
    if (!pw_field_ok(a->home) || !pw_field_ok(a->shell) || !pw_field_ok(a->comment) || strlen(a->home) > 63 || strlen(a->shell) > 31 ||
        strlen(a->comment) > 47 || a->home[0] != '/') {
        dprintf(2, "%s: the home (absolute, at most 63), shell (31) and comment (47) may not hold ':' or a newline\n", who); return 2;
    }
    char *old = pw_slurp(path, &n), *grp = pw_slurp(gpath, &gn);
    if (!old || !grp) { dprintf(2, "%s: cannot read %s\n", who, old ? gpath : path); free(old); free(grp); return 1; }
    int rc = 1;
    char *nf = 0;
    static unsigned char used[65536 / 8];
    memset(used, 0, sizeof used);
    u32 nl = strlen(a->name);
    for (char *l = old; *l; ) {                          /* the names and numbers in use */
        char *e = strchr(l, '\n'); u32 len = e ? (u32)(e - l) : strlen(l);
        const char *f; u32 fl, v;
        if (field_of(l, len, 0, &f, &fl) == 0 && fl == nl && !strncmp(f, a->name, nl)) { dprintf(2, "%s: %s exists already\n", who, a->name); goto out; }
        if (field_of(l, len, 2, &f, &fl) == 0 && field_num(f, fl, &v) == 0) used[v / 8] |= (unsigned char)(1u << (v % 8));
        if (!e) break;
        l = e + 1;
    }
    /* the group: by name, or by number */
    long gid = -1;
    u32 gl = strlen(a->group), want = 0;
    int bynum = field_num(a->group, gl, &want) == 0;
    for (char *l = grp; *l && gid < 0; ) {
        char *e = strchr(l, '\n'); u32 len = e ? (u32)(e - l) : strlen(l);
        const char *f, *g; u32 fl, gfl, v;
        if (field_of(l, len, 0, &f, &fl) == 0 && field_of(l, len, 2, &g, &gfl) == 0 && field_num(g, gfl, &v) == 0)
            if (bynum ? v == want : (fl == gl && !strncmp(f, a->group, gl))) gid = (long)v;
        if (!e) break;
        l = e + 1;
    }
    if (gid < 0) { dprintf(2, "%s: %s: no such group\n", who, a->group); goto out; }
    if (a->extra) {                                      /* every extra group must exist before anything is written */
        char gl2[128];
        if (strlen(a->extra) >= sizeof gl2) { dprintf(2, "%s: -G: too long\n", who); rc = 2; goto out; }
        strcpy(gl2, a->extra);
        for (char *t = gl2, *c; t; t = c) {
            c = strchr(t, ','); if (c) *c++ = 0;
            int ok = 0;
            u32 tl = strlen(t);
            for (char *l = grp; *l && !ok && tl; ) {
                char *e = strchr(l, '\n'); u32 len = e ? (u32)(e - l) : strlen(l);
                const char *f; u32 fl;
                if (field_of(l, len, 0, &f, &fl) == 0 && fl == tl && !strncmp(f, t, tl)) ok = 1;
                if (!e) break;
                l = e + 1;
            }
            if (!ok) { dprintf(2, "%s: %s: no such group\n", who, t); goto out; }
        }
    }
    long uid = a->uid;
    if (uid < 0) {                                       /* the lowest free number from 1000 */
        for (uid = 1000; uid <= 65535 && (used[uid / 8] & (1u << (uid % 8))); uid++) ;
        if (uid > 65535) { dprintf(2, "%s: no free user number\n", who); goto out; }
    } else if (used[uid / 8] & (1u << (uid % 8))) { dprintf(2, "%s: user number %ld is taken\n", who, uid); goto out; }
    u32 cap = (u32)n + 256;
    nf = malloc(cap);
    if (!nf) { dprintf(2, "%s: out of memory\n", who); goto out; }
    memcpy(nf, old, (u32)n);
    u32 o = (u32)n;
    if (o && nf[o - 1] != '\n') nf[o++] = '\n';
    o += (u32)snprintf(nf + o, cap - o, "%s:%s:%ld:%ld:%s:%s:%s\n", a->name, a->hash ? a->hash : "", uid, gid, a->comment, a->home, a->shell);
    if (o > 65535) { dprintf(2, "%s: %s would pass 64 KB\n", who, path); goto out; }
    if (pw_replace(who, path, nf, o, 0644)) goto out;
    if (a->extra) {
        char gl2[128]; strcpy(gl2, a->extra);
        for (char *t = gl2, *c; t; t = c) {
            c = strchr(t, ','); if (c) *c++ = 0;
            if (gr_edit_at(who, gpath, t, a->name, 1)) goto out;
        }
    }
    *uidp = (u32)uid; *gidp = (u32)gid;
    rc = 0;
out:
    free(old); free(grp); free(nf);
    return rc;
}

