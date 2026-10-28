/* SPDX-License-Identifier: GPL-2.0-only */
/* rzrules - where a download goes (DESIGN D20 5): the rules are in rzrules.h. No system calls, no library calls. */
#include "rzrules.h"

int rz_clean(const char *offered, char *out, int outlen)
{
    int i, max = outlen - 1 < RZ_NAMEMAX ? outlen - 1 : RZ_NAMEMAX;
    if (!offered || !offered[0] || offered[0] == '.' || max < 1) {
        if (outlen > 0)
            out[0] = 0;
        return -1;
    }
    for (i = 0; offered[i] && i < max; i++) {
        unsigned char c = (unsigned char)offered[i];
        /* a control character (C0, DEL, C1) or a drawer separator the engine did not take: '_' */
        out[i] = (char)(c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0) || c == '/' ? '_' : c);
    }
    out[i] = 0;
    return 0;
}

static int put(char *out, int pos, int outlen, const char *s)
{
    while (*s && pos < outlen - 1)
        out[pos++] = *s++;
    out[pos < outlen ? pos : outlen - 1] = 0;
    return pos;
}

static void numbered(const char *name, int n, char *out, int outlen)
{
    char d[8];
    int k = 0, pos = put(out, 0, outlen, name);
    if (n == 0)
        return;
    d[7] = 0;
    k = 7;
    do {
        d[--k] = (char)('0' + n % 10);
        n /= 10;
    } while (n && k > 1);
    d[--k] = '.';
    put(out, pos, outlen, d + k);
}

void rz_partname(const char *target, char *out, int outlen)
{
    put(out, put(out, 0, outlen, target), outlen, ".part");
}

int rz_choose(const char *name, unsigned long size, int from, int flags, rz_look look, void *u,
              char *target, int tlen, unsigned long *partlen, int *index)
{
    char part[RZ_NAMEMAX + 16];
    unsigned long have;
    int n;
    *partlen = 0;
    for (n = from < 0 ? 0 : from; n <= RZ_MAXN; n++) {
        numbered(name, n, target, tlen);
        *index = n;
        if (look(u, target, &have)) {
            if (have == size && !(flags & RZF_NOSKIP))
                return RZ_SKIP;         /* the same file is already here */
            continue;                   /* another file: never overwritten */
        }
        rz_partname(target, part, sizeof part);
        if (!look(u, part, &have) || have == 0)
            return RZ_FRESH;            /* free (an empty part holds nothing to keep) */
        if (!(flags & RZF_NORESUME) && have < size) {
            *partlen = have;
            return RZ_RESUME;
        }
        /* a part that is not this file's (longer, or refused by ZCRC): kept, the next name */
    }
    target[0] = 0;
    return RZ_FULL;
}

/* ---- the file side of the engine's XferIO ---- */

#include "xfer.h"

static void say(struct rz_dl *d, int what, const char *name, const char *other)
{
    if (d->fs->say)
        d->fs->say(d->fs->u, what, name, other);
}

static void made_is(const char *s, char *made, int madelen)
{
    if (madelen > 0)
        put(made, 0, madelen, s);
}

/* open target.part as rz_choose said (FRESH or RESUME), or say why not */
static void *open_part(struct rz_dl *d, int how)
{
    void *h;
    rz_partname(d->target, d->part, sizeof d->part);
    h = d->fs->open(d->fs->u, d->part, how);
    if (!h)
        say(d, RZ_SAY_NOOPEN, d->name, d->part);
    return h;
}

void *rz_wopen(struct rz_dl *d, const char *name, unsigned long size, char *made, int madelen, unsigned long *have)
{
    unsigned long partlen;
    int r;
    d->size = size;
    d->written = 0;
    d->target[0] = d->part[0] = 0;
    if (have)
        *have = 0;
    if (rz_clean(name, d->name, sizeof d->name)) {
        /* said with its controls as '?' (the sender's bytes: rz's status lines are one a line, audit 2026-10-28 S13) */
        char shown[RZ_NAMEMAX + 1];
        int i;
        for (i = 0; name && name[i] && i < RZ_NAMEMAX; i++) {
            unsigned char c = (unsigned char)name[i];
            shown[i] = (char)(c < 0x20 || c == 0x7F || (c >= 0x80 && c < 0xA0) ? '?' : c);
        }
        shown[i] = 0;
        say(d, RZ_SAY_REFUSED, shown, 0);
        return 0;
    }
    r = rz_choose(d->name, size, 0, have ? 0 : RZF_NORESUME, d->fs->look, d->fs->u, d->target, sizeof d->target,
                  &partlen, &d->index);
    if (r == RZ_SKIP) {
        say(d, RZ_SAY_SAME, d->name, d->target);
        return 0;
    }
    if (r == RZ_FULL) {
        say(d, RZ_SAY_FULL, d->name, 0);
        return 0;
    }
    made_is(d->target, made, madelen);
    if (d->index > 0)
        say(d, RZ_SAY_OTHER, d->name, d->target);
    if (r == RZ_RESUME) {
        void *h = open_part(d, RZ_OPEN_APPEND);
        if (h)
            *have = partlen;            /* the engine asks ZCRC before it goes on (rz_wresume) */
        return h;
    }
    return open_part(d, RZ_OPEN_NEW);
}

void *rz_wresume(struct rz_dl *d, void *f, int answered, unsigned long crc, char *made, int madelen,
                 unsigned long *have)
{
    unsigned char buf[512];
    unsigned long left = *have, mine = 0xFFFFFFFFUL, partlen;
    void *h;
    int r;
    if (answered && (h = d->fs->open(d->fs->u, d->part, RZ_OPEN_READ))) {
        while (left > 0) {
            long k = d->fs->read(d->fs->u, h, buf, left < sizeof buf ? (long)left : (long)sizeof buf);
            if (k <= 0)
                break;
            mine = xfer_crc32(mine, buf, k);
            left -= (unsigned long)k;
        }
        d->fs->close(d->fs->u, h);
        if (left == 0 && (~mine & 0xFFFFFFFFUL) == crc) {
            say(d, RZ_SAY_RESUME, d->name, d->part);
            return f;                   /* the same file: on from *have */
        }
    }
    /* not the same file (or the sender could not say): the part stays as it is, the next free name from the start */
    d->fs->close(d->fs->u, f);
    say(d, RZ_SAY_NOTSAME, d->name, d->part);
    *have = 0;
    r = rz_choose(d->name, d->size, d->index + 1, RZF_NORESUME, d->fs->look, d->fs->u, d->target, sizeof d->target,
                  &partlen, &d->index);
    if (r == RZ_SKIP) {
        say(d, RZ_SAY_SAME, d->name, d->target);
        return 0;
    }
    if (r == RZ_FULL) {
        say(d, RZ_SAY_FULL, d->name, 0);
        return 0;
    }
    made_is(d->target, made, madelen);
    say(d, RZ_SAY_OTHER, d->name, d->target);
    return open_part(d, RZ_OPEN_NEW);
}

long rz_wwrite(struct rz_dl *d, void *f, const unsigned char *b, long n)
{
    long k = d->fs->write(d->fs->u, f, b, n);
    if (k > 0)
        d->written += (unsigned long)k;
    return k;
}

void rz_wclose(struct rz_dl *d, void *f, int ok)
{
    unsigned long partlen, have;
    int r;
    d->fs->close(d->fs->u, f);
    if (!ok) {
        if (d->fs->look(d->fs->u, d->part, &have) && have == 0)
            d->fs->remove(d->fs->u, d->part);   /* nothing came: nothing to keep */
        else
            say(d, RZ_SAY_KEPT, d->name, d->part);
        return;
    }
    if (d->fs->look(d->fs->u, d->target, &have)) {
        /* the name appeared while the file came: never over it */
        char was[sizeof d->target];
        put(was, 0, sizeof was, d->target);
        r = rz_choose(d->name, d->size, d->index + 1, RZF_NORESUME | RZF_NOSKIP, d->fs->look, d->fs->u, d->target,
                      sizeof d->target, &partlen, &d->index);
        if (r != RZ_FRESH) {
            say(d, RZ_SAY_KEPT, d->name, d->part);
            return;
        }
        say(d, RZ_SAY_RENAMED, was, d->target);
    }
    if (d->fs->rename(d->fs->u, d->part, d->target))
        say(d, RZ_SAY_NOOPEN, d->name, d->target);
}
