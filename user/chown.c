// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
/* chown [-R] owner[:group] file...    chgrp [-R] group file...   (root only) */
static int recursive, rc, only_group;
static u32 new_uid = 0xFFFFFFFFu, new_gid = 0xFFFFFFFFu;

static void apply(const char *path)
{
    if (sys_chown(path, new_uid, new_gid) < 0) { fprintf(2, "%s: %s: %s\n", only_group ? "chgrp" : "chown", path, zwhy("cannot change owner")); rc = 1; return; }
    struct ustat st;
    if (!recursive || sys_lstat(path, &st) < 0 || (st.mode & S_IFMT) != S_IFDIR) return;
    int fd = sys_open(path, O_RDONLY); if (fd < 0) return;
    char *names[512]; int nn = 0; struct udirent d;
    while (sys_readdir(fd, &d) > 0 && nn < 512) { if (strcmp(d.name, ".") && strcmp(d.name, "..")) names[nn++] = strdup(d.name); }
    sys_close(fd);
    for (int i = 0; i < nn; i++) { char c[256]; snprintf(c, sizeof c, "%s/%s", path, names[i]); apply(c); }
}

static int id_of(const char *s, int group, u32 *out)
{
    if (*s >= '0' && *s <= '9') { *out = (u32)atoi(s); return 0; }
    if (group) { struct gr g; if (gr_byname(s, &g)) return -1; *out = g.gid; }
    else { struct pw p; if (pw_byname(s, &p)) return -1; *out = p.uid; }
    return 0;
}

int main(int argc, char **argv)
{
    const char *me = strrchr(argv[0], '/'); me = me ? me + 1 : argv[0];
    only_group = !strcmp(me, "chgrp");
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-R")) { recursive = 1; i++; }
    if (i + 1 >= argc) { fprintf(2, "usage: %s [-R] %s file...\n", me, only_group ? "group" : "owner[:group]"); return 2; }
    char spec[64]; strncpy(spec, argv[i++], 63); spec[63] = 0;
    char *colon = strchr(spec, ':');
    if (only_group) { if (id_of(spec, 1, &new_gid)) { fprintf(2, "chgrp: %s: no such group\n", spec); return 1; } }
    else {
        if (colon) *colon = 0;
        if (spec[0] && id_of(spec, 0, &new_uid)) { fprintf(2, "chown: %s: no such user\n", spec); return 1; }
        if (colon && colon[1] && id_of(colon + 1, 1, &new_gid)) { fprintf(2, "chown: %s: no such group\n", colon + 1); return 1; }
    }
    for (; i < argc; i++) apply(argv[i]);
    return rc;
}
