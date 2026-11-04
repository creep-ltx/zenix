/* SPDX-License-Identifier: GPL-2.0-only */
/* The hunk check and loader (hunk.h; docs/d29-zine.md section 6). Formats from the AmigaDOS Manual 3rd edition
 * ("Amiga binary file structure") and the Guru Book (ch. 22). One walk serves both: without segments it only checks. */
#include "hunk.h"

struct msg {
    char *p;
    zu32 n, cap;
};

static void ms(struct msg *m, const char *s)
{
    while (*s && m->n + 1 < m->cap)
        m->p[m->n++] = *s++;
    m->p[m->n] = 0;
}

static void mnum(struct msg *m, zu32 v, int hex)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    do
        b[--i] = "0123456789ABCDEF"[v % (hex ? 16 : 10)], v /= hex ? 16 : 10;
    while (v);
    if (hex)
        ms(m, "0x");
    ms(m, b + i);
}

/* the walk's state */
struct walk {
    const zu8 *f;
    zu32 len, pos;
    const char *name;
    struct msg m;
    const struct zh_info *inf;          /* load only */
    struct zh_seg *seg;                 /* load only: every hunk's memory */
    zu8 *tab;                           /* load of an overlay node: the hunk table (its ancestors' segments) */
    zu32 start;                         /* the node's first byte */
    int node;                           /* an overlay node: ends with HUNK_BREAK, relocates to earlier hunks too */
};

static int fail(struct walk *w, const char *what, zu32 type)
{
    if (w->m.cap) {
        w->m.n = 0;
        ms(&w->m, w->name);
        ms(&w->m, what);
        if (type != ~0u) {
            mnum(&w->m, type, 1);
            ms(&w->m, " at byte ");
            mnum(&w->m, w->pos - 4, 0);
            ms(&w->m, ")");
        }
    }
    return -ZE_NOEXEC;
}

/* the next longword (getw: word), or 0 at the end of the file */
static int get(struct walk *w, zu32 *v)
{
    if (w->len - w->pos < 4)
        return 0;
    *v = zbe32(w->f + w->pos);
    w->pos += 4;
    return 1;
}

static int getw(struct walk *w, zu32 *v)
{
    if (w->len - w->pos < 2)
        return 0;
    *v = zbe16(w->f + w->pos);
    w->pos += 2;
    return 1;
}

/* skip n longwords */
static int skip(struct walk *w, zu32 n)
{
    if (n > (w->len - w->pos) / 4)
        return 0;
    w->pos += n * 4;
    return 1;
}

/* the header's size table entry i: bytes and memory attributes; *extra: an attribute longword follows */
static zu32 tabsize(zu32 v, zu32 *memf)
{
    zu32 t = v >> 30;
    *memf = t == 1 ? ZMEMF_CHIP : t == 2 ? ZMEMF_FAST : 0;
    return (v & 0x3FFFFFFFu) * 4;
}

zu32 zh_hunk_size(const zu8 *f, const struct zh_info *inf, zu32 i, zu32 *memf)
{
    zu32 pos = inf->sizes, v = 0;
    for (zu32 k = 0; k <= i; k++) {
        v = zbe32(f + pos);
        pos += 4;
        if (v >> 30 == 3) {             /* both bits: the attributes follow */
            *memf = zbe32(f + pos);
            pos += 4;
            if (k == i)
                return (v & 0x3FFFFFFFu) * 4;
        }
    }
    return tabsize(v, memf);
}

/* one relocation block of the current hunk cur (its size cursz); short: 16-bit words */
static int relocs(struct walk *w, zu32 t, zu32 first, zu32 n, zu32 cur, zu32 cursz)
{
    int sh = t != HUNK_RELOC32;
    for (;;) {
        zu32 cnt, h;
        if (!(sh ? getw(w, &cnt) : get(w, &cnt)))
            return fail(w, " is damaged (its relocations end early)", ~0u);
        if (!cnt)
            break;
        if (!(sh ? getw(w, &h) : get(w, &h)))
            return fail(w, " is damaged (its relocations end early)", ~0u);
        zu32 to = 0;                    /* the target's data address as the program sees it */
        if (h >= first && h - first < n) {
            if (w->seg)
                to = w->seg[h - first].addr + 8;
        } else if (w->node && h < first) {     /* an ancestor's hunk, from the table */
            if (w->tab) {
                zu32 b = zbe32(w->tab + 4 * h);
                if (!b)
                    return fail(w, " has an overlay part that relocates to a hunk not loaded (hunk ", h);
                to = (b << 2) + 4;
            }
        } else
            return fail(w, " is damaged (a relocation names hunk ", h);
        if (cnt > (w->len - w->pos) / (sh ? 2 : 4))
            return fail(w, " is damaged (its relocations end early)", ~0u);
        for (zu32 i = 0; i < cnt; i++) {
            zu32 off = 0;
            if (sh)
                getw(w, &off);
            else
                get(w, &off);
            if (cursz < 4 || off > cursz - 4)
                return fail(w, " is damaged (a relocation outside its hunk, offset ", off);
            if (w->seg) {
                zu8 *p = w->seg[cur].p + 8 + off;
                zput32(p, zbe32(p) + to);
            }
        }
    }
    /* the short form's padding (the Guru Book: a word to the next longword boundary, "must"; files without it exist):
     * taken when the stream ends off a longword and either the file ends after the word or the word read as the
     * start of the next block would not be a block type, while the padded reading is */
    if (sh && (w->pos & 2) && w->len - w->pos >= 2 && zbe16(w->f + w->pos) == 0) {
        if (w->len - w->pos == 2)
            w->pos += 2;
        else if (w->len - w->pos >= 6) {
            zu32 un = zbe32(w->f + w->pos) & 0x1FFFFFFFu, pad = zbe32(w->f + w->pos + 2) & 0x1FFFFFFFu;
            if ((un < HUNK_NAME || un > HUNK_RELRELOC32) && pad >= HUNK_NAME && pad <= HUNK_RELRELOC32)
                w->pos += 2;
        }
    }
    return 0;
}

static int walk(struct walk *w, struct zh_info *inf)
{
    zu32 v, tab, first, last;
    w->pos = w->start;
    if (!get(w, &v) || v != HUNK_HEADER)
        return fail(w, w->node ? " is damaged (no overlay part where its overlay table points)"
                               : " is not an AmigaOS program (no hunk header)", ~0u);
    if (!get(w, &v))
        return fail(w, " is damaged (its header ends early)", ~0u);
    if (v)
        return fail(w, " uses resident libraries in its header: not supported", ~0u);
    if (!get(w, &tab) || !get(w, &first) || !get(w, &last))
        return fail(w, " is damaged (its header ends early)", ~0u);
    if (first > last || last >= tab || last - first >= ZH_MAXHUNKS || (w->node && !first))
        return fail(w, " is damaged (its header lists hunks it cannot have)", ~0u);
    if (tab > ZH_MAXTAB)
        return fail(w, " is too big (its hunk table has more than 16384 slots)", ~0u);
    zu32 n = last - first + 1, tot = 0;
    if (inf) {
        zu8 *z = (zu8 *)inf;
        for (zu32 i = 0; i < sizeof *inf; i++)
            z[i] = 0;
        inf->nhunks = n;
        inf->sizes = w->pos;
        inf->start = w->start, inf->first = first, inf->last = last, inf->tabsize = tab;
    }
    for (zu32 i = 0; i < n; i++) {
        zu32 memf, sz;
        if (!get(w, &v))
            return fail(w, " is damaged (its header ends early)", ~0u);
        sz = tabsize(v, &memf);
        if (v >> 30 == 3 && !get(w, &memf))
            return fail(w, " is damaged (its header ends early)", ~0u);
        if (sz > ZH_MAXBYTES || (tot += sz + 8) > ZH_MAXBYTES)
            return fail(w, " is too big (its hunks need more than 256 MB)", ~0u);
        if (inf)
            inf->bytes[memf & ZMEMF_CHIP ? 1 : memf & ZMEMF_FAST ? 2 : 0] += sz + 8;
    }
    zu32 cur = 0, body = 0, cursz = 0;  /* the hunk being read; body: its CODE/DATA/BSS was seen */
    const struct zh_info *li = w->inf ? w->inf : inf;
    while (cur < n || body) {
        if (!get(w, &v)) {
            if (body && cur + 1 == n) { /* the last HUNK_END missing: the node ends with the file */
                if (inf)
                    inf->end = w->pos;
                return 0;
            }
            return fail(w, " is damaged (it ends before its last hunk)", ~0u);
        }
        zu32 t = v & 0x1FFFFFFFu;
        switch (t) {
        case HUNK_CODE:
        case HUNK_DATA:
        case HUNK_BSS: {
            if (body)                   /* a missing HUNK_END: the previous hunk ends here */
                body = 0, cur++;
            if (cur >= n)
                return fail(w, " is damaged (more hunks than its header lists)", ~0u);
            zu32 memf, words;
            cursz = zh_hunk_size(w->f, li, cur, &memf);
            if (!get(w, &words))
                return fail(w, " is damaged (a hunk ends early)", ~0u);
            words &= 0x3FFFFFFFu;
            if (words > cursz / 4)
                return fail(w, " is damaged (a hunk is larger than its header says)", ~0u);
            if (t != HUNK_BSS) {
                zu32 at = w->pos;
                if (!skip(w, words))
                    return fail(w, " is damaged (a hunk ends early)", ~0u);
                if (w->seg) {
                    zu8 *d = w->seg[cur].p + 8;
                    for (zu32 i = 0; i < words * 4; i++)
                        d[i] = w->f[at + i];
                }
            }
            body = 1;
            break;
        }
        case HUNK_RELOC32:
        case HUNK_RELOC32SHORT:
        case HUNK_DRELOC32: {
            if (!body)
                return fail(w, " is damaged (relocations before any hunk)", ~0u);
            int e = relocs(w, t, first, n, cur, cursz);
            if (e)
                return e;
            break;
        }
        case HUNK_SYMBOL:
            for (;;) {
                if (!get(w, &v))
                    return fail(w, " is damaged (a symbol table ends early)", ~0u);
                if (!v)
                    break;
                if (!skip(w, (v & 0xFFFFFFu)) || !skip(w, 1))
                    return fail(w, " is damaged (a symbol table ends early)", ~0u);
            }
            break;
        case HUNK_DEBUG:
        case HUNK_NAME:
            if (!get(w, &v) || !skip(w, v))
                return fail(w, " is damaged (a debug or name block ends early)", ~0u);
            break;
        case HUNK_END:
            if (!body)
                return fail(w, " is damaged (a hunk end without a hunk)", ~0u);
            body = 0, cur++;
            break;
        case HUNK_OVERLAY:              /* the end of the node (a missing HUNK_END before it ends the last hunk) */
        case HUNK_BREAK:
            if (body)
                body = 0, cur++;
            if (cur < n)
                return fail(w, " is damaged (it ends before its last hunk)", ~0u);
            w->pos -= 4;                /* the block is read after the loop */
            break;
        case HUNK_EXT:
            return fail(w, " has external references (an object file, not a program)", ~0u);
        case HUNK_RELRELOC32:
            return fail(w, " uses relative relocations (HUNK_RELRELOC32): not supported yet", ~0u);
        default:
            return fail(w, " is not an AmigaOS program (unknown hunk ", v);
        }
    }
    /* after the last hunk. The root: its overlay table, or nothing more that ZINE reads (other bytes are ignored,
     * another hunk is refused). An overlay node: HUNK_BREAK or the end of the file. */
    zu32 t = w->len - w->pos >= 4 ? zbe32(w->f + w->pos) & 0x1FFFFFFFu : 0;
    if (w->node) {
        if (t == HUNK_BREAK)
            w->pos += 4;
        else if (t == HUNK_OVERLAY)
            return fail(w, " has an overlay part with an overlay table of its own (nested overlays): not supported", ~0u);
        else if (w->len - w->pos >= 4)
            return fail(w, " is damaged (an overlay part does not end with HUNK_BREAK)", ~0u);
    } else if (t == HUNK_OVERLAY) {
        if (first)                      /* the root of an overlay tree starts at slot 0 (no resident libraries) */
            return fail(w, " is damaged (an overlaid program whose root does not start at hunk 0)", ~0u);
        /* the length longword is the table's upper bound (the Guru Book 22.2.10.3): length + 1 longwords follow */
        w->pos += 4;
        if (!get(w, &v) || v >= (w->len - w->pos) / 4)
            return fail(w, " is damaged (its overlay table ends early)", ~0u);
        if (inf)
            inf->ovl = w->pos, inf->ovl_words = v + 1;
        w->pos += (v + 1) * 4;
    } else if (t == HUNK_BREAK)
        return fail(w, " uses overlays in a form ZINE does not know (HUNK_BREAK without an overlay table)", ~0u);
    else if (t == HUNK_CODE || t == HUNK_DATA || t == HUNK_BSS)
        return fail(w, " is damaged (more hunks than its header lists)", ~0u);
    if (inf)
        inf->end = w->pos;
    return 0;
}

int zh_node(const zu8 *f, zu32 len, zu32 start, const char *name, struct zh_info *inf, char *msg, zu32 cap)
{
    struct walk w = { f, len, 0, name, { msg, 0, cap }, 0, 0, 0, start, 1 };
    if (cap)
        msg[0] = 0;
    if (start > len)
        return fail(&w, " is damaged (an overlay part outside the file)", ~0u);
    return walk(&w, inf);
}

int zh_check(const zu8 *f, zu32 len, const char *name, struct zh_info *inf, char *msg, zu32 cap)
{
    struct walk w = { f, len, 0, name, { msg, 0, cap }, 0, 0, 0, 0, 0 };
    if (cap)
        msg[0] = 0;
    int e = walk(&w, inf);
    if (e || !inf->ovl)
        return e;
    /* an overlaid program: every node after the table, each inside the root's hunk table */
    zu32 p = inf->end;
    while (len - p >= 4 && zbe32(f + p) == HUNK_HEADER) {
        struct zh_info ni;
        e = zh_node(f, len, p, name, &ni, msg, cap);
        if (e)
            return e;
        if (ni.first <= inf->last || ni.last >= inf->tabsize || ni.end <= p) {
            w.pos = p + 4;
            return fail(&w, " is damaged (an overlay part's hunks are outside its hunk table, at byte ", p);
        }
        zu32 nb = ni.bytes[0] + ni.bytes[1] + ni.bytes[2];
        inf->nodes++;
        if (nb > inf->node_max)
            inf->node_max = nb;
        inf->node_all = inf->node_all + nb > ZH_MAXBYTES ? ZH_MAXBYTES : inf->node_all + nb;
        p = ni.end;
    }
    return 0;
}

static int load(const zu8 *f, zu32 len, const struct zh_info *inf, const struct zh_ops *ops, struct zh_seg *work,
                zu8 *tab, int node, zu32 *seglist)
{
    zu32 n = inf->nhunks, i;
    for (i = 0; i < n; i++) {
        zu32 memf, sz = zh_hunk_size(f, inf, i, &memf) + 8;
        int e = ops->alloc(ops->u, sz, memf, &work[i]);
        if (e || !work[i].p || (work[i].addr & 3)) {
            while (i--)
                ops->free(ops->u, &work[i]);
            return e ? e : -ZE_NOMEM;
        }
        work[i].size = sz;
        for (zu32 k = 0; k < sz; k++)
            work[i].p[k] = 0;
    }
    for (i = 0; i < n; i++) {
        zput32(work[i].p, work[i].size);
        zput32(work[i].p + 4, i + 1 < n ? (work[i + 1].addr + 4) >> 2 : 0);
    }
    struct walk w = { f, len, 0, "", { 0, 0, 0 }, inf, work, tab, inf->start, node };
    int e = walk(&w, 0);
    if (e) {
        for (i = 0; i < n; i++)
            ops->free(ops->u, &work[i]);
        return e;
    }
    *seglist = (work[0].addr + 4) >> 2;
    return 0;
}

int zh_load(const zu8 *f, zu32 len, const struct zh_info *inf, const struct zh_ops *ops, struct zh_seg *work,
            zu32 *seglist)
{
    return load(f, len, inf, ops, work, 0, 0, seglist);
}

void zh_table(const struct zh_info *inf, const struct zh_seg *work, zu8 *tab, zu32 tabn)
{
    for (zu32 i = inf->first; i < tabn; i++) {
        zu32 k = i - inf->first;
        zput32(tab + 4 * i, k < inf->nhunks ? (work[k].addr + 4) >> 2 : 0);
    }
}

int zh_load_node(const zu8 *f, zu32 len, const struct zh_info *inf, const struct zh_ops *ops, struct zh_seg *work,
                 zu8 *tab, zu32 tabn, zu32 *seglist)
{
    if (!inf->first || inf->last >= tabn - 1 || tabn < 2)
        return -ZE_NOEXEC;              /* outside the table (whose last entry stays 0) */
    int e = load(f, len, inf, ops, work, tab, 1, seglist);
    if (!e)
        zh_table(inf, work, tab, tabn);
    return e;
}
