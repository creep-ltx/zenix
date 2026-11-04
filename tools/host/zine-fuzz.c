/* SPDX-License-Identifier: GPL-2.0-only */
/* libFuzzer harnesses for ZINE's step-1 cores (tools/host/zine-test.sh fuzz), one per build:
 *   -DFUZZ_HUNK   zh_check on the input; whatever it takes, zh_load must load (the check and the loader agree)
 *   -DFUZZ_VOL    a volume built as the input says (zv_check must take it), names looked up from every directory;
 *                 then damaged by the input and read again: inside the block whatever zv_check takes
 *   -DFUZZ_PLAN   a pool walk made from the input: zp_plan; if planned, no chunk overlaps another, a used block or
 *                 low memory, and every chunk lies in its region */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../../user/zine/hunk.h"
#include "../../user/zine/zvol.h"
#include "../../user/zine/zplan.h"

#ifdef FUZZ_HUNK
static zu32 next;
static int fa(void *u, zu32 size, zu32 memf, struct zh_seg *out)
{
    (void)u, (void)memf;
    out->p = malloc(size);
    if (!out->p)
        return -ZE_NOMEM;
    out->addr = next;
    next += (size + 7) & ~7u;
    return 0;
}
static void ff(void *u, const struct zh_seg *s)
{
    (void)u;
    free(s->p);
}
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
#ifdef FUZZ_HUNK
    struct zh_info inf;
    char msg[160];
    if (size > 1 << 20 || zh_check(data, (zu32)size, "f", &inf, msg, sizeof msg))
        return 0;
    if (inf.bytes[0] + inf.bytes[1] + inf.bytes[2] > (64u << 20))
        return 0;                       /* valid, but more memory than a fuzz run should take */
    static struct zh_seg seg[ZH_MAXHUNKS];
    struct zh_ops ops = { fa, ff, 0 };
    zu32 sl;
    next = 0x1000;
    if (zh_load(data, (zu32)size, &inf, &ops, seg, &sl))
        __builtin_trap();               /* the check took a file the loader refuses */
    if (inf.ovl) {                                  /* an overlaid one: every node the check took loads too */
        static zu8 tab[(ZH_MAXTAB + 1) * 4];
        static struct zh_seg ns[ZH_MAXHUNKS];
        struct zh_info ni;
        zh_table(&inf, seg, tab, inf.tabsize + 1);
        for (zu32 p = inf.end; size - p >= 4 && zbe32(data + p) == HUNK_HEADER; p = ni.end) {
            zu32 nsl;
            if (zh_node(data, (zu32)size, p, "f", &ni, msg, sizeof msg))
                __builtin_trap();
            if (ni.bytes[0] + ni.bytes[1] + ni.bytes[2] > (64u << 20))
                break;
            for (zu32 h = 0; h < ni.first; h++)      /* its ancestors as if loaded (the supervisor's order) */
                if (!zbe32(tab + 4 * h))
                    zput32(tab + 4 * h, 0x4000u + h);
            if (zh_load_node(data, (zu32)size, &ni, &ops, ns, tab, inf.tabsize + 1, &nsl))
                __builtin_trap();
            for (zu32 i = 0; i < ni.nhunks; i++)
                free(ns[i].p);
        }
    }
    for (zu32 i = 0; i < inf.nhunks; i++)
        free(seg[i].p);
#endif
#ifdef FUZZ_VOL
    /* build a volume as the input says, which zv_check must take; look names up; then damage it and read again */
    static zu32 blk[16384];
    zu32 bs = sizeof blk;
    size_t i = 0;
    char nm[24];
    if (size < 4 || zv_init(blk, bs, 64, "Vol") != 0)
        return 0;
    struct zv_hdr *h = (struct zv_hdr *)blk;
    while (i + 3 <= size && data[i] != 0xFF) {
        zu32 parent = data[i] % h->nent, n = data[i + 1] % 20, sz = data[i + 2] * 13u;
        int dir = data[i] & 0x80;
        i += 3;
        if (n > size - i)
            break;
        memcpy(nm, data + i, n);
        nm[n] = 0;
        i += n;
        zu8 *d;
        if (zv_add(blk, parent, nm, dir, 0644, 0, sz, &d) >= 0)
            memset(d, 0x5A, dir ? 0 : sz);
    }
    if (zv_check(blk, bs))
        __builtin_trap();               /* the builder made a block the reader refuses */
    i += i < size;
    char path[40];
    zu32 n = size - i < 39 ? (zu32)(size - i) : 39, out;
    memcpy(path, data + i, n);
    path[n] = 0;
    for (zu32 d = 0; d < h->nent; d++)
        if (zv_find(blk, d, path, &out) == 0 && out >= h->nent)
            __builtin_trap();
    for (zu32 k = 0; k + 1 < n; k += 2)                 /* damage: the path's bytes as offset/value pairs */
        ((zu8 *)blk)[(zu32)path[k] * 37 % (sizeof(struct zv_hdr) + 8 * sizeof(struct zv_ent))] = path[k + 1];
    if (zv_check(blk, bs) == 0)
        for (zu32 d = 0; d < h->nent; d++) {
            const struct zv_ent *e = zv_ent(blk, d);
            volatile zu8 x = 0;
            for (zu32 k = 0; k < e->size; k++)
                x ^= ((const zu8 *)blk)[e->off + k];
            zu32 k = 0, c = 0;
            while ((k = zv_next(blk, d, k)) && c++ < h->nent)
                ;
            zv_find(blk, d, path, &out);
        }
#endif
#ifdef FUZZ_PLAN
    /* regions: chip [0, 1 MB), fast [16 MB, 20 MB); ranges and blocks from the input (sizes in 16-byte units) */
    struct zp_region reg[2] = { { ZMEMF_CHIP, 0, 1u << 20 }, { ZMEMF_FAST, 16u << 20, 20u << 20 } };
    struct zp_range rng[4];
    struct zp_blk blk[256];
    zu32 nr = 0, nb = 0;
    size_t i = 0;
    while (i + 4 <= size && nr < 4 && nb < 250) {
        zu32 chip = data[i] & 1, lo = ((zu32)data[i + 1] << 8 | data[i + 2]) * 16 + (chip ? 0 : 16u << 20);
        zu32 nblk = 1 + data[i + 3] % 8, a = lo;
        i += 4;
        rng[nr] = (struct zp_range){ lo, 0, chip, nb, 0 };
        for (zu32 k = 0; k < nblk && i + 2 <= size && nb < 256; k++, i += 2) {
            zu32 s = (1 + (data[i] & 0x7F)) * 16;
            blk[nb++] = (struct zp_blk){ a, s, data[i + 1] & 1 };
            a += s;
        }
        if (nb == rng[nr].first)
            break;
        rng[nr].hi = a;
        rng[nr].n = nb - rng[nr].first;
        nr++;
    }
    struct zp_in in = { reg, 2, rng, nr, blk, nb, 1000 };
    static struct zp_piece pc[300];
    static struct zp_chunk ch[300];
    struct zp_out o;
    memset(&o, 0, sizeof o);
    o.piece = pc, o.piece_cap = 300, o.chunk = ch, o.chunk_cap = 300;
    if (zp_plan(&in, &o))
        return 0;
    for (zu32 a = 0; a < o.nchunk; a++) {
        const struct zp_chunk *c = &ch[a];
        if (c->reg > 1 || c->addr < reg[c->reg].lower || c->len > reg[c->reg].upper - c->addr || c->addr < ZP_PROGLOW || !c->len)
            __builtin_trap();
        for (zu32 b = 0; b < a; b++)
            if (c->addr < ch[b].addr + ch[b].len && ch[b].addr < c->addr + c->len)
                __builtin_trap();
        if (c->reg == 1)
            for (zu32 b = 0; b < nb; b++)
                if (blk[b].used && c->addr < blk[b].addr + blk[b].size && blk[b].addr < c->addr + c->len)
                    __builtin_trap();
    }
#endif
    return 0;
}
