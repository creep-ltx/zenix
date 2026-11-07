/* SPDX-License-Identifier: GPL-2.0-only */
/* Settings files written whole or in part (cfgfile.h). Cold code: -Os. */
#include "libz.h"
#include "cfgfile.h"

#define CFG_MAX 4096

int cfg_write(const char *path, const char *text)
{
    char *t2 = malloc(strlen(path) + 5);
    if (!t2)
        return -ENOMEM;
    strcpy(t2, path);
    strcat(t2, ".new");
    sys_unlink(t2);
    int fd = sys_open(t2, O_WRONLY | O_CREAT | O_EXCL), n = (int)strlen(text), r = fd;
    if (fd >= 0) {
        r = sys_write(fd, text, (u32)n);
        sys_close(fd);
        if (r == n)
            r = sys_rename(t2, path);
        else if (r >= 0)
            r = -ENOSPC;
        if (r < 0)
            sys_unlink(t2);
    }
    free(t2);
    return r < 0 ? r : 0;
}

static int is_key(const char *l, const char *k)
{
    while (*l == ' ' || *l == '\t')
        l++;
    u32 n = strlen(k);
    return !strncmp(l, k, n) && (!l[n] || l[n] == ' ' || l[n] == '\t' || l[n] == '\r');
}

int cfg_set(const char *path, const char *const *key, const char *const *line, int n)
{
    char *b = malloc(CFG_MAX + 1), *o = malloc(CFG_MAX + 256);
    int fd = sys_open(path, O_RDONLY), got = 0, r = 0, k = 0;
    u32 done = 0;
    if (!b || !o) {
        r = -ENOMEM;
        goto out;
    }
    while (fd >= 0 && got <= CFG_MAX && (r = sys_read(fd, b + got, (u32)(CFG_MAX + 1 - got))) > 0)
        got += r;
    r = 0;
    if (got > CFG_MAX) {
        r = -EFBIG;
        goto out;
    }
    b[got] = 0;
    for (char *l = b, *e; *l; l = e) {
        e = strchr(l, '\n');
        if (e)
            *e++ = 0;
        else
            e = l + strlen(l);
        const char *put = l;
        for (int i = 0; i < n; i++)
            if (is_key(l, key[i])) {
                put = done & (1u << i) ? 0 : line[i];
                done |= 1u << i;
                break;
            }
        if (put) {
            u32 m = strlen(put);
            if (k + m + 1 > CFG_MAX) {
                r = -EFBIG;
                goto out;
            }
            memcpy(o + k, put, m);
            k += (int)m;
            o[k++] = '\n';
        }
    }
    for (int i = 0; i < n; i++)
        if (!(done & (1u << i))) {
            u32 m = strlen(line[i]);
            if (k + m + 1 > CFG_MAX + 255) {
                r = -EFBIG;
                goto out;
            }
            memcpy(o + k, line[i], m);
            k += (int)m;
            o[k++] = '\n';
        }
    o[k] = 0;
    r = cfg_write(path, o);
out:
    if (fd >= 0)
        sys_close(fd);
    free(b);
    free(o);
    return r;
}
