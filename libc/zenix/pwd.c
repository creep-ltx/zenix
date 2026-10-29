/* SPDX-License-Identifier: GPL-2.0-only */
/* Users and groups (DESIGN D27 section 5): getpwnam, getpwuid, getgrnam and getgrgid over /etc/passwd and /etc/group
 * (frozen formats) through zpw.c's parser, the one libz's programs use. Each answer is in a static buffer that the
 * next call of the same kind overwrites, as POSIX allows; the comment field is both pw_comment and pw_gecos. A group's
 * members are at most 127 bytes (zpw.c's struct gr), split into gr_mem. */
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <string.h>
#include "zshare.h"

static struct pw p;
static struct passwd *pw_out(int found)
{
    static struct passwd o;
    if (found < 0) return 0;            /* no such entry (or the file could not be read: errno says) */
    o.pw_name = p.name; o.pw_passwd = p.hash; o.pw_uid = p.uid; o.pw_gid = p.gid;
    o.pw_comment = o.pw_gecos = p.comment; o.pw_dir = p.home; o.pw_shell = p.shell;
    return &o;
}
struct passwd *getpwnam(const char *name) { return pw_out(pw_byname(name, &p)); }
struct passwd *getpwuid(uid_t uid) { return pw_out(pw_byuid(uid, &p)); }

static struct gr g;
static struct group *gr_out(int found)
{
    static struct group o;
    static char *mem[65];               /* "a,b,c": at most 64 names in 127 bytes */
    if (found < 0) return 0;
    int n = 0;
    for (char *p = g.members; *p && n < 64; ) {
        mem[n++] = p;
        char *c = strchr(p, ',');
        if (!c) break;
        *c = 0;
        p = c + 1;
    }
    mem[n] = 0;
    o.gr_name = g.name; o.gr_passwd = (char *)""; o.gr_gid = g.gid; o.gr_mem = mem;
    return &o;
}
struct group *getgrnam(const char *name) { return gr_out(gr_byname(name, &g)); }
struct group *getgrgid(gid_t gid) { return gr_out(gr_bygid(gid, &g)); }

