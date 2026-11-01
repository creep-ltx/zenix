/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * zpkg's package list and latest (DESIGN D22 3.3, frozen once built: "zpkgindex 1", its words, growth only) and the
 * trust rules for a list (5.1-5.3). The list is text in the manifests' grammar: a word, its arguments, one line each;
 * unknown words are ignored (a later list's growth), a known word with the wrong arguments or twice is refused. The
 * signature over the whole file is checked before any of this runs. tools/mkrepo.py writes the lists and has the
 * same grammar in Python (parse_index); the host test runs both on the same files.
 */
#include <string.h>
#include <stdlib.h>
#include "zpkg.h"

static int name_ok(const char *s)
{
    size_t n = 0;
    if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9')))
        return 0;
    for (; *s; s++, n++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '+' || *s == '-'))
            return 0;
    return n <= ZP_NAMEMAX;
}

static int vstr_ok(const char *s)
{
    size_t n = 0;
    for (; *s; s++, n++)
        if (*s <= ' ' || *s > '~')
            return 0;
    return n >= 1 && n <= ZP_VSTRMAX;
}

int zp_snapshot_name_ok(const char *s)
{
    size_t n = 0;
    for (; *s; s++, n++) {
        int an = (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9');
        if (!an && (n == 0 || (*s != '.' && *s != '_' && *s != '-')))
            return 0;
    }
    /* NAME.part is a cut run's folder (mkrepo), never a snapshot */
    return n >= 1 && n <= ZP_SNAPMAX && !(n >= 5 && !strcmp(s - 5, ".part"));
}

static int sha_ok(const char *s)
{
    int i;
    for (i = 0; i < 64; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return s[64] == 0;
}

int zp_hex32(unsigned char out[32], const char *hex)
{
    int i;
    for (i = 0; i < 64; i++) {
        char c = hex[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (v < 0)
            return ZP_EINVAL;
        if (i & 1)
            out[i / 2] |= (unsigned char)v;
        else
            out[i / 2] = (unsigned char)(v << 4);
    }
    return 0;
}

/* a decimal u32: digits only, at most 10, no overflow */
static int num(const char *s, zp_u32 *v)
{
    zp_u32 x = 0;
    int n = 0;
    if (!*s)
        return -1;
    for (; *s; s++, n++) {
        if (*s < '0' || *s > '9' || n == 10)
            return -1;
        if (x > (0xFFFFFFFFu - (zp_u32)(*s - '0')) / 10)
            return -1;
        x = x * 10 + (zp_u32)(*s - '0');
    }
    *v = x;
    return 0;
}

long zp_days(zp_u32 ymd)
{
    long y = (long)(ymd / 10000), m = (long)(ymd / 100 % 100), d = (long)(ymd % 100);
    static const unsigned char mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int leap;
    if (y < 1970 || y > 9999 || m < 1 || m > 12 || d < 1)
        return -1;
    leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    if (d > mdays[m - 1] + (m == 2 && leap))
        return -1;
    /* days from civil (Howard Hinnant's algorithm), 1970-01-01 = 0 */
    y -= m <= 2;
    {
        long era = y / 400, yoe = y - era * 400;
        long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }
}

/* the file name a block must have: NAME-VNUM.zup */
static int file_is(const char *file, const char *name, zp_u32 vnum)
{
    size_t n = strlen(name);
    zp_u32 v;
    const char *p = file + n + 1, *e;
    if (strncmp(file, name, n) || file[n] != '-')
        return 0;
    e = p + strlen(p);
    if (e - p < 5 || strcmp(e - 4, ".zup"))
        return 0;
    {
        char buf[12];
        size_t k = (size_t)(e - 4 - p);
        if (k == 0 || k > 10 || (k > 1 && p[0] == '0'))
            return 0;
        memcpy(buf, p, k);
        buf[k] = 0;
        if (num(buf, &v) || v != vnum)
            return 0;
    }
    return 1;
}

#define MAXW 9
/* split a line into words at single spaces; -1 on an empty word (a leading, trailing or double space); the count,
 * at most MAXW: the rest stays in the last word (no known line has that many words, so it is refused by its count,
 * or ignored as an unknown word's) */
static int words(char *l, char **w)
{
    int n = 0;
    char *q;
    if (*l == ' ')
        return -1;
    for (q = l; *q; q++)
        if (*q == ' ' && (q[1] == ' ' || !q[1]))
            return -1;
    while (*l) {
        w[n++] = l;
        if (n == MAXW)
            break;
        while (*l && *l != ' ')
            l++;
        if (!*l)
            break;
        *l++ = 0;
    }
    return n;
}

static int cmp_pkg(const void *a, const void *b)
{
    return strcmp(((const struct zp_pkg *)a)->name, ((const struct zp_pkg *)b)->name);
}

void zp_index_free(struct zp_index *ix)
{
    free(ix->pkg);
    free(ix->dep);
    free((void *)ix->takes);
    memset(ix, 0, sizeof *ix);
}

#define BAD(s) do { *why = (s); goto bad; } while (0)
int zp_index_parse(struct zp_index *ix, char *text, size_t len, const char **why)
{
    size_t i;
    int npkg = 0, ndep = 0, ntakes = 0, have = 0;
    char *l, *e;
    struct zp_pkg *cur = 0;
    static const char *dummy;
    enum { H_CHANNEL = 1, H_SERIAL = 2, H_DATE = 4, H_EXPIRES = 8, H_SYSTEM = 16, H_SNAPSHOT = 32 };
    enum { P_VERSION = 1, P_GROUP = 2, P_FILE = 4, P_SIZE = 8, P_DESC = 16 };
    unsigned char *pflags = 0;

    if (!why)
        why = &dummy;
    memset(ix, 0, sizeof *ix);
    if (len > ZP_INDEX_MAX)
        { *why = "the package list is longer than 256 KB"; return ZP_E2BIG; }
    /* bytes: printable ASCII and newlines only (no NUL, no CR, nothing to confuse a terminal) */
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c != '\n' && (c < 32 || c > 126))
            { *why = "the package list has a byte that is not printable ASCII"; return ZP_EINVAL; }
        if (c == '\n' && i + 4 < len && !strncmp(text + i + 1, "pkg ", 4))
            npkg++;
        if (c == '\n' && i + 8 < len && !strncmp(text + i + 1, "depends ", 8))
            ndep++;
        if (c == '\n' && i + 6 < len && !strncmp(text + i + 1, "takes ", 6))
            ntakes++;
    }
    text[len] = 0;
    if (strncmp(text, "zpkgindex 1\n", 12) && strcmp(text, "zpkgindex 1"))
        { *why = "not a \"zpkgindex 1\" package list"; return ZP_EINVAL; }
    if (npkg > (int)(ZP_INDEX_MAX / 8) || ndep > 65535 || ntakes > 65535)
        { *why = "too many lines"; return ZP_E2BIG; }
    ix->pkg = malloc(sizeof *ix->pkg * (size_t)(npkg ? npkg : 1));
    ix->dep = malloc(sizeof *ix->dep * (size_t)(ndep ? ndep : 1));
    ix->takes = malloc(sizeof *ix->takes * (size_t)(ntakes ? ntakes : 1));
    pflags = malloc((size_t)(npkg ? npkg : 1));
    if (!ix->pkg || !ix->dep || !ix->takes || !pflags) {
        free(pflags);
        zp_index_free(ix);
        *why = "out of memory";
        return ZP_ENOMEM;
    }
    ndep = ntakes = 0;
    l = text + 11;                      /* past "zpkgindex 1" */
    if (*l)
        l++;
    for (; *l; l = e) {
        char *w[MAXW], *desc = 0;
        int nw;
        unsigned char *pf;
        for (e = l; *e && *e != '\n'; e++)
            ;
        if (*e)
            *e++ = 0;
        if (!*l)
            continue;
        if (!strncmp(l, "desc ", 5)) {
            desc = l + 5;               /* the rest of the line, as it is */
            l[4] = 0;
            w[0] = l;
            nw = 1;
        } else if ((nw = words(l, w)) < 0)
            BAD("a line with an empty word");
        if (!strcmp(w[0], "pkg")) {
            if (nw != 2 || !name_ok(w[1]))
                BAD("a bad package name");
            cur = &ix->pkg[ix->npkg];
            memset(cur, 0, sizeof *cur);
            pflags[ix->npkg++] = 0;
            cur->name = w[1];
            cur->dep0 = (unsigned short)ndep;
            cur->takes0 = (unsigned short)ntakes;
            continue;
        }
        if (!cur) {
            if (!strcmp(w[0], "channel")) {
                if (nw != 2 || (have & H_CHANNEL))
                    BAD("a bad channel line");
                ix->channel = w[1];
                have |= H_CHANNEL;
            } else if (!strcmp(w[0], "snapshot")) {
                if (nw != 2 || (have & H_SNAPSHOT) || !zp_snapshot_name_ok(w[1]))
                    BAD("a bad snapshot line");
                ix->snapshot = w[1];
                have |= H_SNAPSHOT;
            } else if (!strcmp(w[0], "serial") || !strcmp(w[0], "date") || !strcmp(w[0], "expires")) {
                int bit = w[0][0] == 's' ? H_SERIAL : w[0][0] == 'd' ? H_DATE : H_EXPIRES;
                zp_u32 *v = bit == H_SERIAL ? &ix->serial : bit == H_DATE ? &ix->date : &ix->expires;
                if (nw != 2 || (have & bit) || num(w[1], v))
                    BAD("a bad serial, date or expires line");
                if (bit != H_SERIAL && zp_days(*v) < 0)
                    BAD("a date that is not a date");
                have |= bit;
            } else if (!strcmp(w[0], "system")) {
                struct zp_system *s = &ix->sys;
                if (nw != 8 || (have & H_SYSTEM) || num(w[1], &s->vnum) || !vstr_ok(w[2]) || num(w[4], &s->fsize) ||
                    !sha_ok(w[5]) || strcmp(w[6], "min") || num(w[7], &s->min))
                    BAD("a bad system line");
                if (!file_is(w[3], "zenix", s->vnum))
                    BAD("the system bundle is not zenix-VNUM.zup");
                if (s->fsize <= 96 || s->fsize > ZP_BUNDLE_MAX)
                    BAD("the system bundle's size is out of range");
                s->vstr = w[2];
                s->file = w[3];
                s->sha = w[5];
                have |= H_SYSTEM;
            } else if (!strcmp(w[0], "systemz")) {   /* (a list before it has no such line; an older zpkg ignores it: growth) */
                struct zp_system *s = &ix->sysz;
                if (nw != 8 || s->file || num(w[1], &s->vnum) || !vstr_ok(w[2]) || num(w[4], &s->fsize) ||
                    !sha_ok(w[5]) || strcmp(w[6], "min") || num(w[7], &s->min))
                    BAD("a bad systemz line");
                if (!file_is(w[3], "zenixz", s->vnum))
                    BAD("the ZenixFS system bundle is not zenixz-VNUM.zup");
                if (s->fsize <= 96 || s->fsize > ZP_BUNDLE_MAX)
                    BAD("the ZenixFS system bundle's size is out of range");
                s->vstr = w[2];
                s->file = w[3];
                s->sha = w[5];
            }
            continue;                   /* other header words: growth */
        }
        pf = &pflags[ix->npkg - 1];
        if (!strcmp(w[0], "version")) {
            if (nw != 3 || (*pf & P_VERSION) || num(w[1], &cur->vnum) || !vstr_ok(w[2]))
                BAD("a bad version line");
            cur->vstr = w[2];
            *pf |= P_VERSION;
        } else if (!strcmp(w[0], "group")) {
            if (nw != 2 || (*pf & P_GROUP) || strcmp(w[1], "base"))
                BAD("a bad group line (only \"group base\")");
            cur->base = 1;
            *pf |= P_GROUP;
        } else if (!strcmp(w[0], "file")) {
            if (nw != 4 || (*pf & P_FILE) || num(w[2], &cur->fsize) || !sha_ok(w[3]))
                BAD("a bad file line");
            if (cur->fsize <= 96 || cur->fsize > ZP_BUNDLE_MAX)
                BAD("a package file's size is out of range");
            cur->file = w[1];
            cur->sha = w[3];
            *pf |= P_FILE;
        } else if (!strcmp(w[0], "size")) {
            if (nw != 2 || (*pf & P_SIZE) || num(w[1], &cur->size))
                BAD("a bad size line");
            *pf |= P_SIZE;
        } else if (!strcmp(w[0], "depends")) {
            if (nw != 3 || !name_ok(w[1]) || cur->ndep == ZP_MAXDEP)
                BAD("a bad depends line (NAME VERSION, at most 8)");
            if (num(w[2], &ix->dep[ndep].vnum))
                BAD("a bad depends line (NAME VERSION, at most 8)");
            ix->dep[ndep++].name = w[1];
            cur->ndep++;
        } else if (!strcmp(w[0], "takes")) {
            if (nw != 2 || !name_ok(w[1]) || cur->ntakes == ZP_MAXTAKES)
                BAD("a bad takes line (NAME, at most 8)");
            ix->takes[ntakes++] = w[1];
            cur->ntakes++;
        } else if (!strcmp(w[0], "desc")) {
            size_t n = desc ? strlen(desc) : 0;
            if ((*pf & P_DESC) || n < 1 || n > ZP_DESCMAX)
                BAD("a bad desc line (1-72 characters)");
            cur->desc = desc;
            *pf |= P_DESC;
        }
        /* other words in a block: growth */
    }
    if ((have & (H_CHANNEL | H_SERIAL | H_DATE | H_EXPIRES | H_SYSTEM)) != (H_CHANNEL | H_SERIAL | H_DATE | H_EXPIRES | H_SYSTEM))
        BAD("the package list lacks a channel, serial, date, expires or system line");
    if (ix->expires < ix->date)
        BAD("the package list expires before its own date");
    for (i = 0; i < (size_t)ix->npkg; i++) {
        struct zp_pkg *p = &ix->pkg[i];
        *why = 0;
        if ((pflags[i] & (P_VERSION | P_SIZE)) != (P_VERSION | P_SIZE))
            BAD("a package block without a version or size line");
        if (!p->file && !p->base)
            BAD("a package block without a file line");
        if (p->file && !file_is(p->file, p->name, p->vnum))
            BAD("a package's file is not NAME-VNUM.zup");
    }
    free(pflags);
    pflags = 0;
    qsort(ix->pkg, (size_t)ix->npkg, sizeof *ix->pkg, cmp_pkg);
    for (i = 1; i < (size_t)ix->npkg; i++)
        if (!strcmp(ix->pkg[i - 1].name, ix->pkg[i].name))
            BAD("a package is in the list twice");
    return 0;
bad:
    free(pflags);
    zp_index_free(ix);
    return ZP_EINVAL;
}

const struct zp_pkg *zp_index_find(const struct zp_index *ix, const char *name)
{
    int lo = 0, hi = ix->npkg - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2, c = strcmp(name, ix->pkg[mid].name);
        if (!c)
            return &ix->pkg[mid];
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return 0;
}

int zp_latest_parse(const char *buf, size_t n, zp_u32 *serial)
{
    char t[ZP_LATEST_MAX + 1];
    size_t i;
    if (n > ZP_LATEST_MAX)
        return ZP_E2BIG;
    memcpy(t, buf, n);
    if (n && t[n - 1] == '\n')
        n--;
    t[n] = 0;
    for (i = 0; i < n; i++)
        if (t[i] < ' ' || t[i] > '~')
            return ZP_EINVAL;
    if (strncmp(t, "zpkglatest 1 ", 13))
        return ZP_EINVAL;
    if (num(t + 13, serial))
        return ZP_EINVAL;
    return 0;
}

int zp_latest_decide(zp_u32 latest, zp_u32 stored)
{
    return latest == stored ? ZP_LATEST_SAME : latest < stored ? ZP_LATEST_BEHIND : ZP_LATEST_NEWER;
}

int zp_channel_class(const char *c)
{
    if (!c)
        return 0;
    if (!strcmp(c, "test"))
        return ZP_KEY_TEST;
    if (!strcmp(c, "testing") || !strcmp(c, "stable"))
        return ZP_KEY_RELEASE;
    return 0;
}

zp_u32 zp_today(const struct zp_trust *t)
{
    return t->clock > t->clockfloor ? t->clock : t->clockfloor;
}

/* D22 amendment 1: a snapshot's list only for that snapshot, by name; a channel's list never carries the line */
static int snap_fits(const struct zp_index *ix, const struct zp_trust *t, const char **why)
{
    if (t->snapshot && (!ix->snapshot || strcmp(ix->snapshot, t->snapshot)))
        { *why = "the package list is not the snapshot asked for"; return ZP_ESNAPSHOT; }
    if (!t->snapshot && ix->snapshot)
        { *why = "the package list is a snapshot's, not the channel's"; return ZP_ESNAPSHOT; }
    return 0;
}

int zp_index_usable(const struct zp_index *ix, const struct zp_trust *t, unsigned *warn, const char **why)
{
    zp_u32 today = zp_today(t);
    long dd = zp_days(ix->date), dt = zp_days(today);
    static const char *dummy;
    unsigned w = 0;
    int rc;
    if (!why)
        why = &dummy;
    if (warn)
        *warn = 0;
    if ((rc = snap_fits(ix, t, why)))
        return rc;
    if (t->snapshot)
        w |= ZP_W_UNCHECKED;
    else if (today > ix->expires) {
        if (!(t->flags & ZP_T_IGNORE_EXPIRY)) {
            *why = "the package list has expired (or the clock is ahead: check the date)";
            if (warn)
                *warn = w;
            return ZP_EEXPIRED;
        }
        w |= ZP_W_EXPIRY_IGNORED;
    }
    if (dd >= 0 && dt >= 0 && dt + 1 < dd)
        w |= ZP_W_CLOCK_BEHIND;
    if (warn)
        *warn = w;
    return 0;
}

int zp_index_check(const struct zp_index *ix, const struct zp_trust *t, unsigned *warn, const char **why)
{
    static const char *dummy;
    int cls = zp_channel_class(ix->channel), i;
    zp_u32 vmax = ix->sys.vnum;
    if (!why)
        why = &dummy;
    if (warn)
        *warn = 0;
    if (!cls)
        { *why = "the package list names an unknown channel"; return ZP_ECHANNEL; }
    if (t->channel && strcmp(ix->channel, t->channel))
        { *why = "the package list is of another channel"; return ZP_ECHANNEL; }
    if (cls != t->keyclass)
        { *why = "the package list's channel does not fit the key that signed it"; return ZP_ECLASS; }
    if ((i = snap_fits(ix, t, why)))
        return i;
    for (i = 0; i < ix->npkg; i++)
        if (ix->pkg[i].vnum > vmax)
            vmax = ix->pkg[i].vnum;
    /* the list's own shape, a snapshot's too (it is a channel's list with one line more) */
    if (ix->serial / 100 > ix->date)
        { *why = "the package list's serial is above its own date's"; return ZP_ESERIAL; }
    if (ix->serial < vmax)
        { *why = "the package list's serial is below a version it names"; return ZP_ESERIAL; }
    if (t->snapshot) {
        /* a snapshot is old on purpose: no floor; its name binds it, and once stored it never changes */
        if (t->stored && ix->serial != t->stored)
            { *why = "the snapshot is not the one stored before (its serial changed)"; return ZP_EREPLAY; }
    } else if (t->stored ? ix->serial <= t->stored : ix->serial < t->floor)
        { *why = "an older package list (a replay, or a mirror that is behind)"; return ZP_EREPLAY; }
    return zp_index_usable(ix, t, warn, why);
}

int zp_bundle_fits(const struct zp_index *ix, const struct zp_pkg *p, const char *name, zp_u32 hdr_vnum,
                   const char *hdr_channel, int keyclass, const char **why)
{
    static const char *dummy;
    if (!why)
        why = &dummy;
    if (p ? (!name || strcmp(name, p->name) || hdr_vnum != p->vnum) : hdr_vnum != ix->sys.vnum)
        { *why = "the file's name or version is not the list's"; return ZP_EINVAL; }
    if (!zp_channel_class(hdr_channel) || zp_channel_class(hdr_channel) != keyclass)
        { *why = "the file's channel does not fit the key"; return ZP_ECLASS; }
    return 0;
}
