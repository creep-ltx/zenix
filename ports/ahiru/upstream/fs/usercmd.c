/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* usercmd - see usercmd.h */
#include <string.h>
#include "usercmd.h"
#include "ops.h"
#include "../core/core.h"
#include "../core/util.h"

/* s onto out, in quotes when quote is set. Inside quotes '*' is the
 * shell's escape, so it goes as "**"; a '"' or a control character (a
 * newline would start a second command) cannot be passed safely: 0 */
static int put(char *out, int size, int *o, const char *s, int quote)
{
    if (quote) {
        const unsigned char *c;
        for (c = (const unsigned char *)s; *c; c++)
            if (*c == '"' || *c < 32 || (*c >= 0x80 && *c < 0xA0)) return 0;
    }
    if (*o + 3 >= size) return 0;
    if (quote) out[(*o)++] = '"';
    for (; *s; s++) {
        if (*o + 4 >= size) return 0;
        if (quote && *s == '*') out[(*o)++] = '*';
        out[(*o)++] = *s;
    }
    if (quote) out[(*o)++] = '"';
    return 1;
}

int uc_expand(const char *tpl, struct place *p, int cur, const char *other, char *out, int size)
{
    char path[CPATH];
    int o = 0, i, any;
    while (*tpl) {
        if (*tpl == '{') {
            const char *e = strchr(tpl, '}');
            int l = e ? (int)(e - tpl - 1) : 0;
            if (e && l == 4 && strncmp(tpl + 1, "file", 4) == 0) {
                if (cur < 0 || !buildfull(path, p->path, p->ent[cur].name) || !put(out, size, &o, path, 1)) return 0;
            } else if (e && l == 4 && strncmp(tpl + 1, "name", 4) == 0) {
                if (cur < 0 || !put(out, size, &o, p->ent[cur].name, 1)) return 0;
            } else if (e && l == 3 && strncmp(tpl + 1, "dir", 3) == 0) {
                if (!put(out, size, &o, p->path, 1)) return 0;
            } else if (e && l == 5 && strncmp(tpl + 1, "other", 5) == 0) {
                if (!other || !put(out, size, &o, other, 1)) return 0;
            } else if (e && l == 6 && strncmp(tpl + 1, "marked", 6) == 0) {
                for (i = 0, any = 0; i < p->n; i++) {
                    if (!p->ent[i].mark) continue;
                    if (any && !put(out, size, &o, " ", 0)) return 0;
                    if (!buildfull(path, p->path, p->ent[i].name) || !put(out, size, &o, path, 1)) return 0;
                    any = 1;
                }
                if (!any && (cur < 0 || !buildfull(path, p->path, p->ent[cur].name) || !put(out, size, &o, path, 1)))
                    return 0;
            } else {
                if (o + 2 >= size) return 0;
                out[o++] = *tpl++;
                continue;
            }
            tpl = e + 1;
            continue;
        }
        if (o + 2 >= size) return 0;
        out[o++] = *tpl++;
    }
    out[o] = 0;
    return o > 0;
}
