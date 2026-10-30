/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's memory planner (zplan.h; docs/d29-zine.md section 4) */
#include "zplan.h"

static int piece(struct zp_out *o, zu32 a, zu32 n)
{
    if (o->npiece == o->piece_cap)
        return -ZE_NOMEM;
    o->piece[o->npiece++] = (struct zp_piece){ a, n };
    o->save += n;
    return 0;
}

static int chunk(struct zp_out *o, zu32 a, zu32 n, zu32 reg)
{
    if (o->nchunk && o->chunk[o->nchunk - 1].reg == reg &&
        o->chunk[o->nchunk - 1].addr + o->chunk[o->nchunk - 1].len == a) {
        o->chunk[o->nchunk - 1].len += n;   /* adjoining: one chunk */
        return 0;
    }
    if (o->nchunk == o->chunk_cap)
        return -ZE_NOMEM;
    o->chunk[o->nchunk++] = (struct zp_chunk){ a, n, reg };
    return 0;
}

/* the region holding [a, b), or nreg */
static zu32 region_of(const struct zp_in *in, zu32 a, zu32 b)
{
    for (zu32 i = 0; i < in->nreg; i++)
        if (a >= in->reg[i].lower && b <= in->reg[i].upper)
            return i;
    return in->nreg;
}

static int check(const struct zp_in *in)
{
    for (zu32 i = 0; i < in->nreg; i++) {
        if (in->reg[i].lower >= in->reg[i].upper)
            return -ZE_INVAL;
        for (zu32 j = 0; j < i; j++)
            if (in->reg[i].lower < in->reg[j].upper && in->reg[j].lower < in->reg[i].upper)
                return -ZE_INVAL;
    }
    for (zu32 i = 0; i < in->nrng; i++) {
        const struct zp_range *r = &in->rng[i];
        zu32 g = region_of(in, r->lo, r->hi);
        if (r->lo >= r->hi || (r->lo | r->hi) & 15 || g == in->nreg || r->n == 0 || r->first > in->nblk ||
            r->n > in->nblk - r->first || !(in->reg[g].attrs & ZMEMF_CHIP) != !r->chip || (r->chip && r->lo < ZP_LOW))
            return -ZE_INVAL;
        for (zu32 j = 0; j < i; j++)
            if (r->lo < in->rng[j].hi && in->rng[j].lo < r->hi)
                return -ZE_INVAL;
        zu32 a = r->lo;
        for (zu32 k = 0; k < r->n; k++) {
            const struct zp_blk *b = &in->blk[r->first + k];
            if (b->addr != a || b->size < ZP_HDR || b->size & 15 || b->size > r->hi - a)
                return -ZE_INVAL;
            a += b->size;
        }
        if (a != r->hi)
            return -ZE_INVAL;
    }
    return 0;
}

int zp_plan(const struct zp_in *in, struct zp_out *o)
{
    int e = check(in);
    if (e)
        return e;
    o->npiece = o->nchunk = 0;
    o->save = o->chip_free = o->fast_free = o->fast_total = o->fast_largest = 0;
    o->nsum = ZP_FIXSUMS;
    o->why = ZP_OK;
    if ((e = piece(o, 0, ZP_LOW)))      /* low memory, whole */
        return e;
    for (zu32 g = 0; g < in->nreg; g++) {
        const struct zp_region *R = &in->reg[g];
        int chip = (R->attrs & ZMEMF_CHIP) != 0;
        if (!chip)
            o->fast_total += R->upper - R->lower;
        /* the region's ranges in address order (few: a selection each time) */
        zu32 last = 0, lowdone = !chip || R->lower > ZP_PROGLOW || R->upper < ZP_LOW;
        for (;;) {
            const struct zp_range *r = 0;
            for (zu32 i = 0; i < in->nrng; i++)
                if (region_of(in, in->rng[i].lo, in->rng[i].hi) == g && in->rng[i].lo >= last &&
                    (!r || in->rng[i].lo < r->lo))
                    r = &in->rng[i];
            if (!lowdone && (!r || r->lo >= ZP_PROGLOW)) {
                if ((e = chunk(o, ZP_PROGLOW, ZP_LOW - ZP_PROGLOW, g)))
                    return e;
                o->chip_free += ZP_LOW - ZP_PROGLOW;
                lowdone = 1;
            }
            if (!r)
                break;
            last = r->hi;
            for (zu32 k = 0; k < r->n; k++) {
                const struct zp_blk *b = &in->blk[r->first + k];
                if (b->used)
                    o->nsum++;
                if (chip) {             /* saved: a used block whole, a free one's header */
                    if ((e = piece(o, b->addr, b->used ? b->size : ZP_HDR)))
                        return e;
                } else if (!b->used) {  /* the program gets it; its header is saved */
                    if ((e = piece(o, b->addr, ZP_HDR)) || (e = chunk(o, b->addr, b->size, g)))
                        return e;
                    o->fast_free += b->size;
                    if (b->size > o->fast_largest)
                        o->fast_largest = b->size;
                }
            }
            if (chip) {                 /* the whole range is the program's */
                if ((e = chunk(o, r->lo, r->hi - r->lo, g)))
                    return e;
                o->chip_free += r->hi - r->lo;
            }
        }
    }
    zu32 blk = in->blob_runtime + o->save + o->nsum * ZP_SUMENT + ZP_CRCTAB + ZP_STACK + ZP_RING + ZP_MARGIN;
    o->block = (blk + 15) & ~15u;
    if (!o->fast_total)
        o->why = ZP_NOFAST;
    else if (o->fast_free < o->fast_total / 2)
        o->why = ZP_HOLDS;
    return 0;
}
