/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's in-memory volume: builder and reader (zvol.h; docs/d29-zine.md section 5.6) */
#include "zvol.h"

#define EPOCH78 252460800u              /* 1978-01-01 in Unix seconds: AmigaDOS's day 0 */

static struct zv_hdr *H(const void *b)
{
    return (struct zv_hdr *)b;
}

static struct zv_ent *E(const void *b, zu32 i)
{
    return (struct zv_ent *)((zu8 *)b + sizeof(struct zv_hdr)) + i;
}

zu32 zv_need(zu32 n, zu32 bytes, zu32 nfiles)
{
    if (n > 0x100000u || nfiles > 0x100000u || bytes > 0x40000000u)
        return 0;
    return sizeof(struct zv_hdr) + n * sizeof(struct zv_ent) + bytes + 3 * nfiles;
}

int zv_fits(zu32 nfiles, zu32 bytes, zu32 free_fast)
{
    return nfiles <= ZV_MAXFILES && bytes <= ZV_MAXBYTES && bytes <= free_fast / 2;
}

static zu8 up(zu8 c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7))
        return c - 0x20;
    return c;
}

int zv_same(const char *a, zu32 n, const char *b)
{
    zu32 i;
    for (i = 0; i < n && b[i]; i++)
        if (up((zu8)a[i]) != up((zu8)b[i]))
            return 0;
    return i == n && !b[i];
}

/* UTF-8 to Latin-1 into out (max bytes); the length, or a ZV_E_ code */
static int latin1(const char *s, char *out, int max)
{
    int n = 0;
    const zu8 *p = (const zu8 *)s;
    while (*p) {
        zu32 c = *p++;
        if (c >= 0x80) {
            if ((c != 0xC2 && c != 0xC3) || (*p & 0xC0) != 0x80)
                return ZV_E_NAME;       /* outside Latin-1, or not UTF-8 */
            c = (c & 0x1F) << 6 | (*p++ & 0x3F);
        }
        if (c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0) || c == ':' || c == '/')
            return ZV_E_NAME;
        if (n == max)
            return ZV_E_LONG;
        out[n++] = (char)c;
    }
    out[n] = 0;
    if (!n || (n == 1 && out[0] == '.') || (n == 2 && out[0] == '.' && out[1] == '.'))
        return ZV_E_NAME;
    return n;
}

int zv_init(void *blk, zu32 size, zu32 cap, const char *name)
{
    if (!cap || zv_need(cap, 0, 0) == 0 || size < zv_need(cap, 0, 0))
        return -ZE_NOSPC;
    struct zv_hdr *h = H(blk);
    h->magic = ZV_MAGIC;
    h->version = ZV_VERSION;
    h->nent = 1;
    h->cap = cap;
    h->size = size;
    h->data = zv_need(cap, 0, 0);
    h->used = 0;
    if (latin1(name, h->volname, ZV_VOLMAX) < 0) {
        const char *z = "ZINE";
        int i = 0;
        while ((h->volname[i] = z[i]))
            i++;
    }
    struct zv_ent *r = E(blk, 0);
    zu8 *p = (zu8 *)r;
    for (zu32 i = 0; i < sizeof *r; i++)
        p[i] = 0;
    r->dir = 1;
    r->off = h->data;
    for (r->namelen = 0; (r->name[r->namelen] = h->volname[r->namelen]); r->namelen++)
        ;
    return 0;
}

int zv_add(void *blk, zu32 parent, const char *name, int dir, zu32 mode, zu32 mtime, zu32 size, zu8 **data)
{
    struct zv_hdr *h = H(blk);
    struct zv_ent e;
    int n = latin1(name, e.name, ZV_NAMEMAX);
    if (n < 0)
        return n;
    if (parent >= h->nent || !E(blk, parent)->dir)
        return ZV_E_PARENT;
    if (h->nent >= h->cap)
        return ZV_E_FULL;
    if (dir)
        size = 0;
    zu32 room = h->size - h->data - h->used;
    if (size > room || ((size + 3) & ~3u) > room)
        return ZV_E_SPACE;
    struct zv_ent *d = E(blk, h->nent);
    d->parent = parent;
    d->size = size;
    zu32 u = mode >> 6;
    d->prot = (u & 4 ? 0 : ZV_FIBF_READ) | (u & 2 ? 0 : ZV_FIBF_WRITE | ZV_FIBF_DELETE) | (u & 1 ? 0 : ZV_FIBF_EXECUTE);
    if (mtime < EPOCH78)
        mtime = EPOCH78;
    zu32 t = mtime - EPOCH78;
    d->days = t / 86400;
    d->mins = t % 86400 / 60;
    d->ticks = t % 60 * 50;
    d->off = h->data + h->used;
    d->dir = dir ? 1 : 0;
    d->namelen = (zu16)n;
    for (int i = 0; i <= n; i++)
        d->name[i] = e.name[i];
    h->used += (size + 3) & ~3u;
    if (data)
        *data = (zu8 *)blk + d->off;
    return (int)h->nent++;
}

const char *zv_reason(int e)
{
    switch (e) {
    case ZV_E_NAME: return "its name is not an AmigaDOS name (Latin-1, no ':' or '/')";
    case ZV_E_LONG: return "its name is longer than 107 bytes";
    case ZV_E_FULL: return "too many files";
    case ZV_E_SPACE: return "no room left in the volume";
    case ZV_E_PARENT: return "its directory was left out";
    }
    return "unknown";
}

int zv_check(const void *blk, zu32 len)
{
    const struct zv_hdr *h = H(blk);
    if (len < sizeof *h || h->magic != ZV_MAGIC || h->version != ZV_VERSION || h->size != len || !h->cap ||
        h->nent < 1 || h->nent > h->cap || zv_need(h->cap, 0, 0) == 0 || h->data != zv_need(h->cap, 0, 0) ||
        h->data > len || h->used > len - h->data || !E(blk, 0)->dir)
        return -ZE_INVAL;
    for (zu32 i = 0; i < h->nent; i++) {
        const struct zv_ent *e = E(blk, i);
        if ((i && e->parent >= i) || (i && !E(blk, e->parent)->dir) || e->namelen > ZV_NAMEMAX ||
            e->name[e->namelen] || e->off < h->data || e->size > len - e->off || e->off - h->data > h->used ||
            (e->dir && e->size))
            return -ZE_INVAL;
    }
    return 0;
}

const struct zv_ent *zv_ent(const void *blk, zu32 i)
{
    return i < H(blk)->nent ? E(blk, i) : 0;
}

/* 1 when a (n bytes) and b (0-ended) are the same bytes */
static int exact(const char *a, zu32 n, const char *b)
{
    zu32 i;
    for (i = 0; i < n && b[i]; i++)
        if (a[i] != b[i])
            return 0;
    return i == n && !b[i];
}

/* the entry named s (n bytes) in directory dir, or 0: the name exactly as written first, else the first entry that is
 * the same but for case (AmigaDOS's rule; two such names can come from a Unix filesystem) */
static zu32 child(const void *blk, zu32 dir, const char *s, zu32 n)
{
    zu32 first = 0;
    for (zu32 i = 1; i < H(blk)->nent; i++)
        if (E(blk, i)->parent == dir && zv_same(s, n, E(blk, i)->name)) {
            if (exact(s, n, E(blk, i)->name))
                return i;
            if (!first)
                first = i;
        }
    return first;
}

zu32 zv_twin(const void *blk, zu32 i)
{
    if (!i || i >= H(blk)->nent)
        return 0;
    const struct zv_ent *e = E(blk, i);
    for (zu32 k = 1; k < i; k++)
        if (E(blk, k)->parent == e->parent && zv_same(e->name, e->namelen, E(blk, k)->name))
            return k;
    return 0;
}

int zv_find(const void *blk, zu32 cur, const char *path, zu32 *out)
{
    const char *p = path, *c = path;
    while (*c && *c != ':')
        c++;
    if (cur >= H(blk)->nent || !E(blk, cur)->dir)
        return -ZE_NOTDIR;
    if (*c) {                           /* a device or volume */
        zu32 n = (zu32)(c - p);
        static const char *const sub[] = { "C", "S", "L", "LIBS", "DEVS", "FONTS" };
        if (!n || zv_same(p, n, H(blk)->volname) || zv_same(p, n, "SYS") || zv_same(p, n, "PROGDIR"))
            cur = 0;
        else {
            zu32 k;
            for (k = 0; k < sizeof sub / sizeof sub[0]; k++)
                if (zv_same(p, n, sub[k]))
                    break;
            if (k == sizeof sub / sizeof sub[0])
                return -ZE_NODEV;
            cur = child(blk, 0, p, n);
            if (!cur || !E(blk, cur)->dir)
                return -ZE_NOENT;
        }
        p = c + 1;
    }
    while (*p) {
        const char *s = p;
        while (*p && *p != '/')
            p++;
        zu32 n = (zu32)(p - s);
        if (!n) {                       /* "/" alone: the parent */
            if (!cur)
                return -ZE_NOENT;
            cur = E(blk, cur)->parent;
        } else {
            if (!E(blk, cur)->dir)
                return -ZE_NOTDIR;
            zu32 k = child(blk, cur, s, n);
            if (!k)
                return -ZE_NOENT;
            cur = k;
        }
        if (*p == '/') {
            p++;
            if (*p && !E(blk, cur)->dir)
                return -ZE_NOTDIR;
        }
    }
    *out = cur;
    return 0;
}

zu32 zv_next(const void *blk, zu32 dir, zu32 after)
{
    for (zu32 i = after + 1; i < H(blk)->nent; i++)
        if (E(blk, i)->parent == dir)
            return i;
    return 0;
}
