/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * libFuzzer harnesses for zpkg's parsers (DESIGN D22 8.1; tools/host/zpkg-test.sh fuzz), one per build:
 *   -DFUZZ_INDEX    the package list, then the trust rules and every block's fields
 *   -DFUZZ_LATEST   latest
 *   -DFUZZ_HTTP     the HTTP response parser, the input cut into pieces the first bytes choose; URLs and redirects
 *   -DFUZZ_FFS      the boot partition's walk over an image made of the input (up to 256 blocks); on a volume it
 *                   accepts, a new file: every block written must have been free (or a bitmap block, or the one
 *                   link block), and the walk must accept the result with the file in it; then an in-place write
 *                   that may touch only that file's data blocks
 *   -DFUZZ_RESOLVE  the resolver: a list from the input, installed packages made from its first blocks
 *   -DFUZZ_FMT      user/zpkgfmt.c (D15's frozen formats, zup's and zpkg's one parser; build with it, sha256.c and
 *                   -DZHOST): an input starting with Z is a bundle (head and table read in pieces as zup -f streams it,
 *                   then the whole bundle in memory), any other a manifest; every entry checked against the rules
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../user/zpkg/zpkg.h"
#include "../../net/http/http.h"
#ifdef FUZZ_FMT
#include <errno.h>
#include "../../user/zpkgfmt.h"
void zf_say(const char *fmt, ...) { (void)fmt; }
static void fmt_pkg_check(struct zf_pkg *p)
{
    if (!zf_name_ok(p->name) || !p->vnum || p->ndep > ZF_MAXDEP)
        __builtin_trap();
    for (int i = 0; i < p->nent; i++)
        if (!zf_path_ok(p->ent[i].path) || !strchr("fcdl", p->ent[i].kind) || (p->ent[i].mode & ~07777u) ||
            (p->ent[i].kind == 'l' && strlen(p->ent[i].target) >= ZF_PMAX - 9))
            __builtin_trap();
}
#endif

#ifdef FUZZ_FFS
struct mem {
    unsigned char *d;
    zp_u32 n;
    unsigned char *written;
};
static int mrd(void *u, zp_u32 b, unsigned char *buf)
{
    struct mem *m = u;
    if (b >= m->n)
        __builtin_trap();               /* the walk asked for a block outside the partition */
    memcpy(buf, m->d + 512 * (size_t)b, 512);
    return 0;
}
static int mwr(void *u, zp_u32 b, unsigned char *buf)
{
    struct mem *m = u;
    if (b >= m->n)
        __builtin_trap();
    memcpy(m->d + 512 * (size_t)b, buf, 512);
    m->written[b] = 1;
    return 0;
}
static int mflush(void *u) { (void)u; return 0; }
static int fsrc(void *u, zp_u32 off, unsigned char *buf, zp_u32 n)
{
    (void)u;
    for (zp_u32 i = 0; i < n; i++)
        buf[i] = (unsigned char)(off + i);
    return 0;
}
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
#if defined(FUZZ_INDEX) || defined(FUZZ_RESOLVE)
    char *t;
    struct zp_index ix;
    const char *why;
    if (size > ZP_INDEX_MAX + 1)
        return 0;
    t = malloc(size + 1);
    memcpy(t, data, size);
    if (zp_index_parse(&ix, t, size, &why) == 0) {
#ifdef FUZZ_INDEX
        struct zp_trust tr = { ix.channel, zp_channel_class(ix.channel), 0, 0, 20261101, 20261026, 0, 0 };
        unsigned w;
        unsigned char d[32];
        volatile size_t sink = 0;
        zp_index_check(&ix, &tr, &w, &why);
        tr.snapshot = ix.snapshot ? ix.snapshot : "x";
        tr.channel = 0;
        zp_index_check(&ix, &tr, &w, &why);
        for (int i = 0; i < ix.npkg; i++) {
            const struct zp_pkg *p = &ix.pkg[i];
            sink += strlen(p->name) + strlen(p->vstr) + (p->desc ? strlen(p->desc) : 0);
            if (p->file)
                sink += strlen(p->file) + (size_t)zp_hex32(d, p->sha);
            for (int k = 0; k < p->ndep; k++)
                sink += strlen(zp_pkg_dep(&ix, p, k)->name);
            for (int k = 0; k < p->ntakes; k++)
                sink += strlen(ix.takes[p->takes0 + k]);
            if (zp_index_find(&ix, p->name) != p)
                __builtin_trap();
            zp_bundle_fits(&ix, p, p->name, p->vnum, ix.channel, tr.keyclass, &why);
        }
        sink += strlen(ix.sys.file) + strlen(ix.sys.vstr) + (size_t)zp_hex32(d, ix.sys.sha);
#else
        /* installed: the first blocks of the list at a version one lower, every other one as a dependency */
        static struct zp_inst in[16];
        static struct zp_dep dep[16][ZP_MAXDEP];
        const char *names[4];
        int n = ix.npkg < 16 ? ix.npkg : 16, nn = 0;
        struct zp_plan pl;
        for (int i = 0; i < n; i++) {
            const struct zp_pkg *p = &ix.pkg[i];
            in[i].name = p->name;
            in[i].vnum = p->vnum - (i & 1);
            in[i].base = p->base;
            in[i].asdep = (unsigned char)(i % 3 == 0);
            in[i].ndep = p->ndep;
            for (int k = 0; k < p->ndep; k++)
                dep[i][k] = *zp_pkg_dep(&ix, p, k);
            in[i].dep = dep[i];
        }
        for (int i = n; i < ix.npkg && nn < 4; i++)
            names[nn++] = ix.pkg[i].name;
        struct zp_world w = { &ix, in, n / 2, ix.sys.vnum - 1, names, nn ? 1 : 0 };
        if (zp_plan_sync(&w, names, nn, 0, &pl, &why) == 0)
            zp_plan_free(&pl);
        if (zp_plan_sync(&w, 0, 0, ZP_SYNC_UU, &pl, &why) == 0)
            zp_plan_free(&pl);
        for (int i = 0; i < n / 2 && nn < 4; i++)
            names[nn++] = in[i].name;
        if (zp_plan_remove(&w, names + (nn > 2 ? 2 : 0), nn > 2 ? nn - 2 : nn, ZP_RM_S, &pl, &why) == 0) {
            for (int i = 0; i < pl.n; i++)
                if (pl.step[i].kind != ZP_STEP_REMOVE || !pl.step[i].from || pl.step[i].from->base)
                    __builtin_trap();
            zp_plan_free(&pl);
        }
#endif
        zp_index_free(&ix);
    }
    free(t);
#elif defined(FUZZ_LATEST)
    zp_u32 s;
    zp_latest_parse((const char *)data, size, &s);
#elif defined(FUZZ_HTTP)
    struct http_resp *r = malloc(sizeof *r);
    size_t off = 2, used, bl;
    const unsigned char *b;
    unsigned long limit;
    int rc = 0;
    if (size < 2) {
        free(r);
        return 0;
    }
    limit = (unsigned long)data[0] << 8 | data[1];
    http_resp_init(r, limit, data[0] & 1);
    while (off < size) {
        size_t k = (size_t)(data[off - 1] % 23) + 1;
        if (k > size - off)
            k = size - off;
        size_t end = off + k;
        while (off < end) {
            rc = http_resp_feed(r, data + off, end - off, &used, &b, &bl);
            if (used > end - off || (rc == HTTP_DATA && (b < data + off || b + bl > data + end || bl > used)))
                __builtin_trap();
            off += used;
            if (rc < 0 || rc == HTTP_END || rc == HTTP_NEED)
                break;
        }
        if (rc < 0 || rc == HTTP_END)
            break;
    }
    if (r->got > limit)
        __builtin_trap();               /* never more body than the limit */
    http_resp_eof(r);
    http_resume(r, 100, 200);
    {
        char *s = malloc(size + 1);
        struct http_url u;
        int hops = 0;
        char req[2200];
        memcpy(s, data, size);
        s[size] = 0;
        if (http_url_parse(&u, s) == 0) {
            if (http_request(req, sizeof req, &u, 5, 1) > 0 && (strchr(u.path, '\n') || strchr(u.host, '\r')))
                __builtin_trap();
            http_redirect(&u, s + (size > 8 ? 8 : 0), &hops);
        }
        free(s);
    }
    free(r);
#elif defined(FUZZ_FMT)
    if (size && data[0] == 'Z') {
        static struct zf_bundle b;
        unsigned need = 0, avail = 0;
        int r;
        while ((r = zf_bundle_head(&b, data, avail, (unsigned)size, &need)) == -EAGAIN) {
            if (need <= avail || need > size)
                __builtin_trap();       /* it must ask for more, and never past the end */
            avail = need;
        }
        if (r == 0 && (b.ntoc < 1 || b.ntoc > ZF_MAXTOC || ZF_HDR + b.ntoc * ZF_TOCE > size))
            __builtin_trap();
        for (unsigned i = 0; r == 0 && i < b.ntoc; i++) {
            char name[41], pname[25];
            unsigned off, sz;
            const unsigned char *sha;
            if (zf_bundle_entry(&b, i, name, &off, &sz, &sha) == 0) {
                if (off > size || sz > size - off)
                    __builtin_trap();
                zf_part_kind(name, pname);
            }
        }
        unsigned char *copy = malloc(size);
        memcpy(copy, data, size);
        if (zf_bundle_parse(&b, copy, (unsigned)size) == 0)
            for (int j = 0; j < b.npkg; j++) {
                fmt_pkg_check(&b.pk[j]);
                if (b.pk[j].man < copy || b.pk[j].man + b.pk[j].manlen > copy + size || b.pk[j].tgz < copy ||
                    b.pk[j].tgz + b.pk[j].tgzlen > copy + size)
                    __builtin_trap();
            }
        zf_bundle_free(&b);
        free(copy);
    } else {
        struct zf_pkg p;
        char *t = malloc(size + 1);
        memcpy(t, data, size);
        t[size] = 0;
        if (zf_manifest_parse(&p, t, (unsigned)size, "fuzz") == 0)
            fmt_pkg_check(&p);
        zf_pkg_free(&p);
    }
#elif defined(FUZZ_FFS)
    struct mem m;
    struct zp_ffs f;
    struct zp_bootvol v;
    struct zp_bootfile bf;
    const char *why;
    static const zp_u32 date[3] = { 1, 2, 3 };
    zp_u32 n = (zp_u32)(size / 512);
    if (n < 8 || n > 256)
        return 0;
    m.n = n;
    m.d = malloc(512 * (size_t)n);
    m.written = calloc(n, 1);
    memcpy(m.d, data, 512 * (size_t)n);
    memset(&f, 0, sizeof f);
    f.blocksize = 512;
    f.reserved = 2;
    f.dostype = data[3] == 3 ? ZP_DOS3 : ZP_DOS1;
    f.nblocks = n;
    f.canflush = 1;
    f.read = mrd;
    f.write = mwr;
    f.flush = mflush;
    f.u = &m;
    if (zp_boot_open(&f, "Zenix", &v, &why) == 0) {
        unsigned char *before = malloc(512 * (size_t)n), *ownb = malloc(n / 8 + 1);
        zp_u32 dir = v.dir, bm[ZP_BOOT_BMPAGES], nbm = v.nbm, k;
        struct zp_bootvol v2;
        memcpy(before, m.d, 512 * (size_t)n);
        memcpy(ownb, v.own, n / 8 + 1);
        memcpy(bm, v.bmblk, sizeof bm);
        if (zp_boot_find(&v, "zenix-update.zup", &bf, &why) == 0)
            zp_boot_file_free(&bf);
        zp_u32 fsz = (zp_u32)(data[size - 1] * 300u);
        struct fz { zp_u32 n; } fzs = { fsz };
        if (zp_boot_create(&v, "zenix-update.zup", fsz, fsrc, &fzs, date, &why) == 0) {
            for (zp_u32 b = 0; b < n; b++) {
                int isbm = 0;
                for (k = 0; k < nbm; k++)
                    isbm |= bm[k] == b;
                if (m.written[b] && (ownb[b >> 3] >> (b & 7) & 1) && !isbm && b != dir) {
                    /* an owned block written: only the one chain link (a header in dir's chain) may be */
                    if (memcmp(before + 512 * (size_t)b, m.d + 512 * (size_t)b, 512) &&
                        (m.d[512 * (size_t)b + 3] != 2))
                        __builtin_trap();
                }
            }
            if (zp_boot_open(&f, "Zenix", &v2, &why))
                __builtin_trap();               /* our own result must pass our own walk */
            if (zp_boot_find(&v2, "zenix-update.zup", &bf, &why) || bf.size != fsz)
                __builtin_trap();
            memset(m.written, 0, n);
            {
                unsigned char *buf = malloc(fsz + 1);
                memset(buf, 0xA5, fsz + 1);
                if (zp_boot_pwrite(&v2, &bf, 0, buf, fsz))
                    __builtin_trap();
                for (zp_u32 b = 0; b < n; b++) {
                    int mine = 0;
                    for (zp_u32 i = 0; i < bf.nblk; i++)
                        mine |= bf.blk[i] == b;
                    if (m.written[b] && !mine)
                        __builtin_trap();       /* an in-place write outside the file's data blocks */
                }
                free(buf);
            }
            zp_boot_file_free(&bf);
            zp_boot_close(&v2);
        }
        free(before);
        free(ownb);
        zp_boot_close(&v);
    }
    free(m.written);
    free(m.d);
#endif
    return 0;
}
