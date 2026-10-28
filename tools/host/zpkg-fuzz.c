/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * libFuzzer harnesses for zpkg's parsers (DESIGN D22 8.1; tools/host/zpkg-test.sh fuzz), one per build:
 *   -DFUZZ_INDEX    the package list, then the trust rules and every block's fields
 *   -DFUZZ_LATEST   latest
 *   -DFUZZ_HTTP     the HTTP response parser, the input cut into pieces the first bytes choose; URLs and redirects
 *   -DFUZZ_FFS      the slot walk over an image made of the input (up to 256 blocks), then a write that may touch
 *                   only the slot's data blocks
 *   -DFUZZ_RESOLVE  the resolver: a list from the input, installed packages made from its first blocks
 * (zpkgfmt.c's manifest and bundle parsers are the main session's step 1; their harness comes with them.)
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../user/zpkg/zpkg.h"
#include "../../net/http/http.h"

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
#elif defined(FUZZ_FFS)
    struct mem m;
    struct zp_ffs f;
    struct zp_slot s;
    const char *why;
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
    f.u = &m;
    if (zp_slot_find(&f, "Zenix", "zenix-update.slot", &s, &why) == 0) {
        unsigned char *buf = malloc(s.size);
        memset(buf, 0xA5, s.size);
        if (zp_slot_pwrite(&f, &s, 0, buf, s.size))
            __builtin_trap();
        for (zp_u32 b = 0; b < n; b++) {
            int mine = 0;
            for (zp_u32 i = 0; i < s.nblk; i++)
                mine |= s.blk[i] == b;
            if (m.written[b] && !mine)
                __builtin_trap();       /* a write outside the slot's data blocks */
        }
        free(buf);
        zp_slot_free(&s);
    }
    free(m.written);
    free(m.d);
#endif
    return 0;
}
