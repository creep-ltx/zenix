/* SPDX-License-Identifier: GPL-2.0-only */
/* Ahiru's lists and path arithmetic on Zenix (ports/ahiru/README): upstream's ahiru/src/list.c compiled unchanged, but
 * for the parent of a Unix path. Upstream's AmigaDOS shapes take "Work:a" up to "Work:" and a volume's root up to the
 * volume list; read as a Unix path, "/tmp" would go straight to the volume list. Here "/tmp" goes up to "/", and "/"
 * to the volume list (the mount table, ports/ahiru/zenix/os.c); "/" names itself in that list (lastpart), so the
 * cursor lands on it. Paths inside an archive ("/tmp/a.lha/dir") go up one part, as upstream's. The upstream functions
 * are renamed by the defines and declared static first, so they are compiled and never emitted. */
#include <string.h>
#define parentpath up_parentpath
#define lastpart up_lastpart
static void up_parentpath(char *dst, const char *path);
static const char *up_lastpart(const char *path);
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../upstream/ahiru/src/list.c"
#undef parentpath
#undef lastpart

void parentpath(char *dst, const char *path)
{
    char *s;
    int l;
    if (path[0] != '/') {               /* an AmigaOS shape after all ("" the volume list) */
        up_parentpath(dst, path);
        return;
    }
    if (dst != path) strcpy(dst, path);
    l = (int)strlen(dst);
    while (l > 1 && dst[l - 1] == '/') dst[--l] = 0;
    if (!strcmp(dst, "/")) {
        dst[0] = 0;                     /* the root's parent is the volume list */
        return;
    }
    s = strrchr(dst, '/');
    if (s == dst) s[1] = 0;
    else *s = 0;
}

const char *lastpart(const char *path)
{
    return strcmp(path, "/") ? up_lastpart(path) : path;
}
