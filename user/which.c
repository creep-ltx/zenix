/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
/* which name...: where a program is found in $PATH */
int main(int argc, char **argv)
{
    int rc = 0; const char *path = getenv("PATH"); if (!path) path = "/bin";
    for (int i = 1; i < argc; i++) {
        int found = 0; const char *p = path; char buf[160]; struct ustat st;
        if (strchr(argv[i], '/')) { if (sys_stat(argv[i], &st) == 0) { dprintf(1, "%s\n", argv[i]); continue; } rc = 1; continue; }
        while (*p && !found) {
            const char *e = p; while (*e && *e != ':') e++;
            u32 l = (u32)(e - p); if (l < 100) { memcpy(buf, p, l); if (l && buf[l - 1] != '/') buf[l++] = '/'; strcpy(buf + l, argv[i]); if (sys_stat(buf, &st) == 0 && (st.mode & S_IFMT) == S_IFREG) { dprintf(1, "%s\n", buf); found = 1; } }
            p = *e ? e + 1 : e;
        }
        if (!found) rc = 1;
    }
    return rc;
}
