// SPDX-License-Identifier: GPL-2.0-only
#include "pw.h"
/* chown [-R] owner[:group] file...    chgrp [-R] group file...   (root only) */
static int recursive, rc, only_group;
static u32 new_uid = 0xFFFFFFFFu, new_gid = 0xFFFFFFFFu;

static void apply(const char *path)
{
    if (sys_chown(path, new_uid, new_gid) < 0) { dprintf(2, "%s: %s: %s\n", only_group ? "chgrp" : "chown", path, zwhy("cannot change owner")); rc = 1; return; }
    struct ustat st;
    if (!recursive || sys_lstat(path, &st) < 0 || (st.mode & S_IFMT) != S_IFDIR) return;
    char **names; int nn = dir_list(path, &names);            /* heap, not stack: one level per directory */
    if (nn < 0) { dprintf(2, "%s: %s: %s\n", only_group ? "chgrp" : "chown", path, zwhy("cannot read")); rc = 1; return; }
    for (int i = 0; i < nn; i++) {
        char *c = path_join(path, names[i]);
        if (c) apply(c); else { dprintf(2, "%s: %s/%s: %s\n", only_group ? "chgrp" : "chown", path, names[i], zwhy("cannot change owner")); rc = 1; }
        free(c);
    }
    dir_free(names, nn);
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
    const char *USAGE = only_group ? "usage: chgrp [-R] group file...\n" : "usage: chown [-R] owner[:group] file...\n";
    zopt(argc, argv, USAGE);
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-R")) { recursive = 1; i++; }
    if (i < argc && !strcmp(argv[i], "--")) i++;
    else if (i < argc && argv[i][0] == '-') zbadopt(argv[i], USAGE);
    if (i + 1 >= argc) zusage(USAGE);
    char spec[64]; strncpy(spec, argv[i++], 63); spec[63] = 0;
    char *colon = strchr(spec, ':');
    if (only_group) { if (id_of(spec, 1, &new_gid)) { dprintf(2, "chgrp: %s: no such group\n", spec); return 1; } }
    else {
        if (colon) *colon = 0;
        if (spec[0] && id_of(spec, 0, &new_uid)) { dprintf(2, "chown: %s: no such user\n", spec); return 1; }
        if (colon && colon[1] && id_of(colon + 1, 1, &new_gid)) { dprintf(2, "chown: %s: no such group\n", colon + 1); return 1; }
    }
    for (; i < argc; i++) apply(argv[i]);
    return rc;
}
