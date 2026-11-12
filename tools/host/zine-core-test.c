/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's step-1 cores on the PC (docs/d29-zine.md section 15, "Host tests"; tools/host/zine-test.sh): the hunk check
 * and loader, the in-memory volume, the memory planner, the library offset table and the assigns (zpath, 16.8), each with the sanitizers.
 * Prints PASS/FAIL lines; exits non-zero on a failure. `zine-core-test seeds DIR` writes the test programs (fuzz seeds). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../user/zine/hunk.h"
#include "../../user/zine/zvol.h"
#include "../../user/zine/zplan.h"
#include "../../user/zine/lvo.h"
#include "../../user/zine/zpath.h"
#include "../../kernel/errno.h"

_Static_assert(ZE_NOENT == ENOENT && ZE_NOEXEC == ENOEXEC && ZE_NOMEM == ENOMEM && ZE_NODEV == ENODEV &&
               ZE_NOTDIR == ENOTDIR && ZE_INVAL == EINVAL && ZE_NOSPC == ENOSPC && ZE_NAMETOOLONG == ENAMETOOLONG,
               "zcore.h's errno numbers are the kernel's");

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL " __VA_ARGS__); printf(" (line %d)\n", __LINE__); } } while (0)

/* ---- a hunk file builder */
static unsigned char F[8192];
static unsigned FL;
static void L(unsigned v) { F[FL] = v >> 24; F[FL + 1] = v >> 16; F[FL + 2] = v >> 8; F[FL + 3] = v; FL += 4; }
static void W(unsigned v) { F[FL] = v >> 8; F[FL + 1] = v; FL += 2; }

/* the test program: hunk 0 code (any memory; 6 longwords, 3 of them in the file), hunk 1 data (chip, 4 longwords),
 * hunk 2 BSS (fast, 8 longwords; with attr: an attribute longword). Code: 32-bit relocations at 0 -> hunk 1 and
 * 4 -> hunk 2; data: short ones (5 words, so a padding word) at 4 and 12 -> hunk 0, in the form: 0 padded, 1 not
 * padded, 2 as 0x3F7, 3 padded and the file ending there. ends: the code's HUNK_END is there. */
static void build(int form, int attr, int ends)
{
    FL = 0;
    L(HUNK_HEADER); L(0); L(3); L(0); L(2);
    L(6); L(4 | 1u << 30);
    if (attr) { L(8 | 3u << 30); L(ZMEMF_FAST | ZMEMF_PUBLIC); } else L(8 | 2u << 30);
    L(HUNK_NAME); L(1); L(0x434f4445);
    L(HUNK_CODE); L(3); L(0x10); L(0x20); L(0x4e754e75);
    L(HUNK_RELOC32); L(1); L(1); L(0); L(1); L(2); L(4); L(0);
    L(HUNK_SYMBOL); L(1); L(0x5f6d6169); L(8); L(2); L(0x5f646174); L(0x61000000); L(4); L(0);
    if (ends) L(HUNK_END);
    L(HUNK_DATA | 1u << 30); L(4); L(0x100); L(0x00000200); L(0x300); L(0x400);
    if (form != 3) {
        L(HUNK_DEBUG); L(2); L(0x11111111); L(0x22222222);
    }
    L(form == 2 ? HUNK_DRELOC32 : HUNK_RELOC32SHORT); W(2); W(0); W(4); W(12); W(0);    /* 5 words: odd */
    if (form == 0 || form == 2 || form == 3) W(0);
    if (form == 3)
        return;                         /* the file ends: the last HUNK_END missing too */
    L(HUNK_END);
    L(HUNK_BSS | 2u << 30); L(8);
    L(HUNK_END);
}

struct halloc { int n, fail_at, live; unsigned memf[8]; unsigned next; };
static int h_alloc(void *u, zu32 size, zu32 memf, struct zh_seg *out)
{
    struct halloc *a = u;
    if (a->n == a->fail_at)
        return -ZE_NOMEM;
    a->memf[a->n++ & 7] = memf;
    out->p = malloc(size);
    memset(out->p, 0xEE, size);
    out->addr = a->next;
    a->next += (size + 0xFFF) & ~0xFFFu;
    a->live++;
    return 0;
}
static void h_free(void *u, const struct zh_seg *s)
{
    struct halloc *a = u;
    free(s->p);
    a->live--;
}

static void free_all(struct zh_seg *s, int n, struct halloc *a)
{
    for (int i = 0; i < n; i++)
        h_free(a, &s[i]);
}

/* the file reader of zh_check_rd (16.12): from F, counting the reads; fail_at: the read that fails */
static int rd_reads, rd_fail_at = -1;
static int rd_read(void *u, zu32 off, zu8 *buf, zu32 n)
{
    (void)u;
    if (rd_reads++ == rd_fail_at || off > FL || n > FL - off)
        return -1;
    memcpy(buf, F + off, n);
    return 0;
}

static void test_hunk_rd(void)
{
    char msg[160];
    struct zh_info a, b;
    static zu8 win[64];
    struct zh_rd rd = { rd_read, 0, win, sizeof win };
    for (int form = 0; form < 3; form++) {
        build(form, 1, 1);
        int r1 = zh_check(F, FL, "prog", &a, msg, sizeof msg);
        int r2 = zh_check_rd(F, 48, FL, &rd, "prog", &b, msg, sizeof msg);
        CHECK(r1 == 0 && r2 == 0 && !memcmp(&a, &b, sizeof a), "hunk rd form %d: the same check from the file (%s)", form, msg);
        struct halloc h1 = { 0, -1, 0, {0}, 0x00200000 }, h2 = { 0, -1, 0, {0}, 0x00200000 };
        struct zh_ops o1 = { h_alloc, h_free, &h1 }, o2 = { h_alloc, h_free, &h2 };
        struct zh_seg s1[3], s2[3];
        zu32 l1, l2;
        r1 = zh_load(F, FL, &a, &o1, s1, &l1);
        r2 = zh_load_rd(F, 48, FL, &rd, &b, &o2, s2, &l2);
        int same = r1 == 0 && r2 == 0 && l1 == l2;
        for (int i = 0; same && i < 3; i++)
            same = s1[i].size == s2[i].size && !memcmp(s1[i].p, s2[i].p, s1[i].size);
        CHECK(same, "hunk rd form %d: the same load from the file", form);
        if (!r1) free_all(s1, 3, &h1);
        if (!r2) free_all(s2, 3, &h2);
        /* a read that fails anywhere: refused (or, past the hunks, taken), never a crash */
        for (int k = 0; k < 40; k++) {
            rd_reads = 0, rd_fail_at = k;
            struct halloc h3 = { 0, -1, 0, {0}, 0x00200000 };
            struct zh_ops o3 = { h_alloc, h_free, &h3 };
            if (zh_check_rd(F, 48, FL, &rd, "prog", &b, msg, sizeof msg) == 0) {
                rd_reads = 0;
                if (zh_load_rd(F, 48, FL, &rd, &b, &o3, s2, &l2) == 0)
                    free_all(s2, 3, &h3);
            }
            CHECK(h3.live == 0, "hunk rd: a failed read at %d frees everything", k);
        }
        rd_fail_at = -1;
    }
    /* every prefix through the reader: the same answers as in memory */
    build(0, 0, 1);
    unsigned good = FL;
    int diff = 0;
    for (unsigned n = 0; n < good; n++) {
        FL = n;
        int r1 = zh_check(F, n, "p", &a, msg, sizeof msg), r2 = zh_check_rd(F, 48, n, &rd, "p", &b, msg, sizeof msg);
        diff += (r1 == 0) != (r2 == 0);
    }
    FL = good;
    CHECK(!diff, "hunk rd: every prefix answered as in memory (%d differ)", diff);
}

static void test_hunk(void)
{
    test_hunk_rd();
    char msg[160];
    struct zh_info inf;
    for (int form = 0; form < 4; form++)
        for (int attr = 0; attr < 2; attr++)
            for (int ends = 0; ends < 2; ends++) {
                if (form == 3 && attr)
                    continue;
                build(form, attr, ends);
                int r = zh_check(F, FL, "prog", &inf, msg, sizeof msg);
                if (form == 3) {        /* hunk 2 never comes: refused */
                    CHECK(r == -ZE_NOEXEC && strstr(msg, "ends before its last hunk"), "hunk: a file ending in hunk 1 refused (%s)", msg);
                    continue;
                }
                CHECK(r == 0, "hunk form %d attr %d: checked (%s)", form, attr, msg);
                if (r)
                    continue;
                CHECK(inf.nhunks == 3 && inf.bytes[0] == 32 && inf.bytes[1] == 24 && inf.bytes[2] == 40, "hunk: the sizes by memory type");
                struct halloc a = { 0, -1, 0, {0}, 0x00200000 };
                struct zh_ops ops = { h_alloc, h_free, &a };
                struct zh_seg s[3];
                zu32 sl;
                r = zh_load(F, FL, &inf, &ops, s, &sl);
                CHECK(r == 0 && a.live == 3, "hunk form %d: loaded", form);
                if (r)
                    continue;
                CHECK(a.memf[0] == 0 && a.memf[1] == ZMEMF_CHIP && a.memf[2] == (attr ? (ZMEMF_FAST | ZMEMF_PUBLIC) : ZMEMF_FAST), "hunk: memory types asked for");
                CHECK(sl == (s[0].addr + 4) >> 2 && zbe32(s[0].p) == 32 && zbe32(s[0].p + 4) == (s[1].addr + 4) >> 2 &&
                      zbe32(s[1].p + 4) == (s[2].addr + 4) >> 2 && zbe32(s[2].p + 4) == 0 && zbe32(s[2].p) == 40, "hunk: the segment list");
                CHECK(zbe32(s[0].p + 8) == 0x10 + s[1].addr + 8 && zbe32(s[0].p + 12) == 0x20 + s[2].addr + 8 &&
                      zbe32(s[0].p + 16) == 0x4e754e75 && zbe32(s[0].p + 20) == 0 && zbe32(s[0].p + 28) == 0, "hunk: code relocated, the rest zero");
                CHECK(zbe32(s[1].p + 8) == 0x100 && zbe32(s[1].p + 12) == 0x200 + s[0].addr + 8 && zbe32(s[1].p + 16) == 0x300 &&
                      zbe32(s[1].p + 20) == 0x400 + s[0].addr + 8, "hunk form %d: short relocations", form);
                int z = 1;
                for (int i = 8; i < 40; i++) z &= s[2].p[i] == 0;
                CHECK(z, "hunk: BSS zero");
                free_all(s, 3, &a);
                /* an allocation failing on the way: everything given back */
                for (int k = 0; k < 3; k++) {
                    struct halloc b = { 0, k, 0, {0}, 0x00200000 };
                    struct zh_ops o2 = { h_alloc, h_free, &b };
                    CHECK(zh_load(F, FL, &inf, &o2, s, &sl) == -ZE_NOMEM && b.live == 0, "hunk: alloc %d fails, all freed", k);
                }
            }
    /* one hunk whose short relocations are the file's end, padded: taken */
    FL = 0;
    L(HUNK_HEADER); L(0); L(1); L(0); L(0); L(2);
    L(HUNK_CODE); L(2); L(0x11); L(0x22);
    L(HUNK_RELOC32SHORT); W(1); W(0); W(4); W(0); W(0);
    CHECK(zh_check(F, FL, "prog", &inf, msg, sizeof msg) == 0 && zh_check(F, FL - 2, "prog", &inf, msg, sizeof msg) == 0,
          "hunk: short relocations ending the file, padded and not");
    /* the refusals, each with its message */
    build(0, 0, 1);
    unsigned good = FL;
    unsigned char keep[8192];
    memcpy(keep, F, FL);
#define REF(desc, edit, want) do { memcpy(F, keep, good); FL = good; edit; \
        int rr = zh_check(F, FL, "prog", &inf, msg, sizeof msg); \
        CHECK(rr == -ZE_NOEXEC && strstr(msg, want), "hunk refusal %s: got \"%s\"", desc, msg); } while (0)
    unsigned end = good - 12;                    /* the BSS block: HUNK_BSS, size, HUNK_END */
    REF("overlay table cut", (FL = good, L(HUNK_OVERLAY), L(0)), "prog is damaged (its overlay table ends early)");
    REF("break before the last hunk", (FL = end, L(HUNK_BREAK)), "ends before its last hunk");
    REF("break without a table", (FL = good, L(HUNK_BREAK)), "HUNK_BREAK without an overlay table");
    REF("ext", (FL = end, L(HUNK_EXT), L(0)), "prog has external references");
    REF("relreloc", (FL = end, L(HUNK_RELRELOC32), L(0)), "relative relocations");
    REF("unknown", (FL = end, L(0x3F9)), "prog is not an AmigaOS program (unknown hunk 0x3F9 at byte ");
    REF("reloc16", (FL = end, L(HUNK_RELOC16)), "unknown hunk 0x3ED");
    REF("resident names", (FL = 4, L(1)), "resident libraries");
    REF("no header", (FL = 0, L(HUNK_CODE)), "no hunk header");
    REF("short header", (FL = 12), "header ends early");
    REF("bad hunk numbers", (F[15] = 3), "lists hunks it cannot have");
    REF("reloc to hunk 3", (F[87] = 3), "names hunk 0x3");
    REF("reloc outside", (F[91] = 21), "outside its hunk");
    REF("larger than table", (F[51] = 7), "larger than its header says");
    REF("one hunk more", (FL = good, L(HUNK_BSS), L(1), L(HUNK_END)), "more hunks than its header lists");
    REF("ends early", (FL = good - 4 - 12), "ends before its last hunk");
    REF("huge", (FL = 20, L(0x3FFFFFFF)), "too big");
    /* a cruncher's form (d29 16.12): two hunks listed, the second of size 0, one before HUNK_OVERLAY (a table of one
     * longword), then the packed data the program reads itself: taken, the second hunk allocated empty */
    {
        unsigned char k2[8192];
        FL = 0;
        L(HUNK_HEADER); L(0); L(2); L(0); L(1); L(3); L(0);
        L(HUNK_CODE); L(3); L(0x11); L(0x22); L(0x33);
        L(HUNK_RELOC32); L(1); L(1); L(4); L(0);
        L(HUNK_END);
        L(HUNK_OVERLAY); L(0); L(HUNK_BREAK);
        unsigned dat = FL;
        L(0x12345678); L(0x9abcdef0);
        memcpy(k2, F, FL);
        int r = zh_check(F, FL, "crunch", &inf, msg, sizeof msg);
        CHECK(r == 0 && inf.nhunks == 2 && inf.ovl && inf.ovl_words == 1 && inf.end == dat && inf.nodes == 0,
              "hunk: the cruncher's overlay form taken (%s)", msg);
        struct halloc a = { 0, -1, 0, {0}, 0x00200000 };
        struct zh_ops ops = { h_alloc, h_free, &a };
        struct zh_seg s2[2];
        zu32 sl;
        r = r ? r : zh_load(F, FL, &inf, &ops, s2, &sl);
        CHECK(r == 0 && a.live == 2 && zbe32(s2[0].p) == 20 && zbe32(s2[1].p) == 8 && zbe32(s2[0].p + 4) == (s2[1].addr + 4) >> 2 &&
              zbe32(s2[0].p + 12) == 0x22 + s2[1].addr + 8 && zbe32(s2[1].p + 4) == 0,
              "hunk: the cruncher's form loaded, its second hunk empty and relocated to");
        if (!r)
            free_all(s2, 2, &a);
        /* a node's hunks still all come before its HUNK_BREAK: the same form inside a node is refused (zovl's
         * nodes), and a root ending with the file before its last hunk (Animotion) stays refused */
        memcpy(F, k2, dat);
        FL = dat - 12;
        CHECK(zh_check(F, FL, "cut", &inf, msg, sizeof msg) == -ZE_NOEXEC && strstr(msg, "ends before its last hunk"),
              "hunk: the root cut after its first hunk refused (%s)", msg);
    }
    build(0, 0, 1);
    memcpy(keep, F, FL);
    /* the walk is safe on every prefix and every one-byte change; whatever the check accepts loads */
    int accepted = 0, refused = 0, mismatch = 0;
    for (unsigned n = 0; n < good; n++) {
        memcpy(F, keep, good);
        if (zh_check(F, n, "p", &inf, msg, sizeof msg) == 0) accepted++; else refused++;
    }
    for (unsigned i = 0; i < good; i++)
        for (int v = 0; v < 3; v++) {
            memcpy(F, keep, good);
            F[i] = v == 0 ? 0 : v == 1 ? 0xFF : F[i] ^ 0x01;
            if (zh_check(F, good, "p", &inf, msg, sizeof msg)) { refused++; continue; }
            accepted++;
            struct halloc a = { 0, -1, 0, {0}, 0x00200000 };
            struct zh_ops ops = { h_alloc, h_free, &a };
            struct zh_seg s[ZH_MAXHUNKS];
            zu32 sl;
            if (zh_load(F, good, &inf, &ops, s, &sl)) mismatch++;
            else free_all(s, inf.nhunks, &a);
        }
    CHECK(mismatch == 0, "hunk: %d changed files the check took did not load", mismatch);
    printf("hunk: %d damaged files refused, %d still valid and loaded\n", refused, accepted);
}

/* ---- overlays (d29 16.6): a root of two hunks (code with the overlay header, data), the overlay table, node A (one
 * code hunk in slot 2, relocated to the root's data and to itself), node B (code in slot 2 relocated to its BSS in
 * slot 3), then bytes that are no node (data the program reads itself). The hunk table has 4 slots + its 0. */
static unsigned ov_a, ov_b, ov_tail, ov_tab;
static void build_ovl(int nest, int nobreak)
{
    FL = 0;
    L(HUNK_HEADER); L(0); L(4); L(0); L(1); L(8); L(2);
    L(HUNK_CODE); L(8); L(0x60160000); L(0xABCD); L(0); L(0); L(0); L(0); L(0x4e754e75); L(0);
    L(HUNK_END);
    L(HUNK_DATA); L(2); L(0x5a4f564c); L(0);
    L(HUNK_END);
    L(HUNK_OVERLAY); L(18); ov_tab = FL;
    L(3); L(0); L(0);                                   /* the offset to the entries, the ordinate (1 level), its 0 */
    for (int e = 0; e < 2; e++) {                       /* entries A and B: seek offsets filled below */
        L(0); L(0); L(0); L(1); L(e + 1); L(2); L(2); L(4);
    }
    ov_a = FL;
    L(HUNK_HEADER); L(0); L(4); L(2); L(2); L(2);
    L(HUNK_CODE); L(2); L(0x10); L(0x20);
    L(HUNK_RELOC32); L(1); L(1); L(0); L(1); L(2); L(4); L(0);
    L(HUNK_END);
    if (nest) { L(HUNK_OVERLAY); L(0); L(0); }
    else if (!nobreak) L(HUNK_BREAK);
    else L(HUNK_CODE);
    ov_b = FL;
    L(HUNK_HEADER); L(0); L(4); L(2); L(3); L(1); L(2 | 2u << 30);
    L(HUNK_CODE); L(1); L(0x8);
    L(HUNK_RELOC32SHORT); W(1); W(3); W(0); W(0);
    L(HUNK_END);
    L(HUNK_BSS); L(2); L(HUNK_END);
    L(HUNK_BREAK);
    ov_tail = FL;
    L(0x44415441);                                      /* "DATA" */
    F[ov_tab + 12 + 3] = (unsigned char)ov_a; F[ov_tab + 12 + 2] = (unsigned char)(ov_a >> 8);
    F[ov_tab + 44 + 3] = (unsigned char)ov_b; F[ov_tab + 44 + 2] = (unsigned char)(ov_b >> 8);
}

static void test_overlay(void)
{
    char msg[160];
    struct zh_info inf, na, nb;
    build_ovl(0, 0);
    CHECK(zh_check(F, FL, "prog", &inf, msg, sizeof msg) == 0, "ovl: an overlaid program checked (%s)", msg);
    CHECK(inf.nhunks == 2 && inf.tabsize == 4 && inf.ovl == ov_tab && inf.ovl_words == 19 && inf.end == ov_a,
          "ovl: the root, its table (%u %u %u %u)", inf.nhunks, inf.tabsize, inf.ovl, inf.ovl_words);
    CHECK(inf.nodes == 2 && inf.node_max == 12 + 16 && inf.node_all == 16 + 12 + 16, "ovl: two nodes, their memory (%u %u %u)",
          inf.nodes, inf.node_max, inf.node_all);
    struct halloc a = { 0, -1, 0, {0}, 0x00200000 };
    struct zh_ops ops = { h_alloc, h_free, &a };
    struct zh_seg r[2], sa[1], sb[2];
    zu32 sl, sla, slb;
    unsigned char tab[5 * 4];
    memset(tab, 0x55, sizeof tab);
    CHECK(zh_load(F, FL, &inf, &ops, r, &sl) == 0 && a.live == 2, "ovl: the root loaded");
    zh_table(&inf, r, tab, 5);
    CHECK(zbe32(tab) == (r[0].addr + 4) >> 2 && zbe32(tab + 4) == (r[1].addr + 4) >> 2 && zbe32(tab + 8) == 0 &&
          zbe32(tab + 12) == 0 && zbe32(tab + 16) == 0, "ovl: the hunk table after the root");
    CHECK(zh_node(F, FL, ov_a, "prog", &na, msg, sizeof msg) == 0 && na.first == 2 && na.last == 2 && na.end == ov_b,
          "ovl: node A checked (%s)", msg);
    CHECK(zh_load_node(F, FL, &na, &ops, sa, tab, 5, &sla) == 0 && a.live == 3, "ovl: node A loaded");
    CHECK(sla == (sa[0].addr + 4) >> 2 && zbe32(tab + 8) == sla && zbe32(tab + 12) == 0 &&
          zbe32(sa[0].p + 8) == 0x10 + r[1].addr + 8 && zbe32(sa[0].p + 12) == 0x20 + sa[0].addr + 8,
          "ovl: node A relocated to the root's data and to itself, in the table");
    CHECK(zh_node(F, FL, ov_b, "prog", &nb, msg, sizeof msg) == 0 && nb.first == 2 && nb.last == 3 && nb.end == ov_tail,
          "ovl: node B checked (%s)", msg);
    free_all(sa, 1, &a);                                /* the supervisor frees A, loads B */
    CHECK(zh_load_node(F, FL, &nb, &ops, sb, tab, 5, &slb) == 0 && a.live == 4, "ovl: node B loaded");
    CHECK(zbe32(tab + 8) == slb && zbe32(tab + 12) == (sb[1].addr + 4) >> 2 && zbe32(tab + 16) == 0 &&
          zbe32(sb[0].p + 8) == 0x8 + sb[1].addr + 8 && a.memf[(a.n - 1) & 7] == ZMEMF_FAST, "ovl: node B, its BSS in slot 3");
    free_all(sb, 2, &a);                                /* and A again: slot 3 cleared */
    CHECK(zh_load_node(F, FL, &na, &ops, sa, tab, 5, &sla) == 0 && zbe32(tab + 12) == 0, "ovl: node A again, B's slot 3 cleared");
    free_all(sa, 1, &a);
    zput32(tab + 4, 0);                                 /* an ancestor not loaded */
    CHECK(zh_load_node(F, FL, &na, &ops, sa, tab, 5, &sla) == -ZE_NOEXEC && a.live == 2, "ovl: a relocation to a hunk not loaded refused, freed");
    CHECK(zh_load_node(F, FL, &na, &ops, sa, tab, 3, &sla) == -ZE_NOEXEC && a.live == 2, "ovl: a node outside the table refused");
    free_all(r, 2, &a);
    /* the refusals */
    build_ovl(1, 0);
    CHECK(zh_check(F, FL, "prog", &inf, msg, sizeof msg) == -ZE_NOEXEC && strstr(msg, "nested overlays"), "ovl: nested overlays refused (%s)", msg);
    build_ovl(0, 1);
    CHECK(zh_check(F, FL, "prog", &inf, msg, sizeof msg) == -ZE_NOEXEC && strstr(msg, "does not end with HUNK_BREAK"), "ovl: a node without HUNK_BREAK refused (%s)", msg);
    build_ovl(0, 0);
    F[ov_a + 15] = 0;                                   /* node A's first slot 0 */
    CHECK(zh_check(F, FL, "prog", &inf, msg, sizeof msg) == -ZE_NOEXEC && strstr(msg, "lists hunks it cannot have"), "ovl: a node in slot 0 refused (%s)", msg);
    build_ovl(0, 0);
    F[ov_a + 15] = 1; F[ov_a + 19] = 1; F[ov_a + 63] = 1;  /* node A in the root's slot 1 */
    CHECK(zh_check(F, FL, "prog", &inf, msg, sizeof msg) == -ZE_NOEXEC && strstr(msg, "outside its hunk table"), "ovl: a node over the root's slots refused (%s)", msg);
    build_ovl(0, 0);
    F[11] = 3;                                          /* the root's table 3 slots: node B's slot 3 beyond */
    CHECK(zh_check(F, FL, "prog", &inf, msg, sizeof msg) == -ZE_NOEXEC && strstr(msg, "outside its hunk table"), "ovl: a node beyond the table refused (%s)", msg);
    build_ovl(0, 0);
    F[ov_tab - 1] = 200;                                /* the table's length beyond the file */
    CHECK(zh_check(F, FL, "prog", &inf, msg, sizeof msg) == -ZE_NOEXEC && strstr(msg, "overlay table ends early"), "ovl: a table longer than the file refused (%s)", msg);
    build_ovl(0, 0);
    F[8] = 0x40; F[9] = F[10] = F[11] = 0;             /* the root's table size 0x40000000: (n + 1) * 4 would wrap */
    CHECK(zh_check(F, FL, "prog", &inf, msg, sizeof msg) == -ZE_NOEXEC && strstr(msg, "more than 16384 slots"), "ovl: a table size of 0x40000000 refused (%s)", msg);
    build_ovl(0, 0);
    F[8] = F[9] = F[10] = F[11] = 0xFF;
    CHECK(zh_check(F, FL, "prog", &inf, msg, sizeof msg) == -ZE_NOEXEC && strstr(msg, "more than 16384 slots"), "ovl: a table size of 0xFFFFFFFF refused (%s)", msg);
    build_ovl(0, 0);
    F[15] = 1; F[19] = 2;                               /* the root in slots 1-2 */
    CHECK(zh_check(F, FL, "prog", &inf, msg, sizeof msg) == -ZE_NOEXEC && strstr(msg, "does not start at hunk 0"), "ovl: a root not at slot 0 refused (%s)", msg);
    build_ovl(0, 0);
    FL = ov_tail - 8;                                   /* node B ends with the file: no HUNK_END, no HUNK_BREAK */
    CHECK(zh_check(F, FL, "prog", &inf, msg, sizeof msg) == 0 && inf.nodes == 2, "ovl: a last node ending with the file taken (%s)", msg);
    CHECK(zh_node(F, FL, ov_b, "prog", &nb, msg, sizeof msg) == 0 && nb.end == FL, "ovl: its end is the file's end (%u)", nb.end);
    /* a cruncher's data container (Spaceballs' Peek, d29 16.7): a code hunk and two BSS hunks of which the header's
     * sizes are larger (one chip), HUNK_OVERLAY of two longwords (the data's length and length - 1, no symbols),
     * HUNK_BREAK, then data: taken, no nodes, the data left to the program; its end before the data */
    FL = 0;
    L(HUNK_HEADER); L(0); L(3); L(0); L(2); L(2); L(0x40000040u); L(0x30);
    L(HUNK_CODE); L(2); L(0x4e754e75); L(0); L(HUNK_END);
    L(HUNK_BSS); L(1); L(HUNK_END);
    L(HUNK_BSS); L(1); L(HUNK_END);
    L(HUNK_OVERLAY); L(1); L(24); L(23); L(HUNK_BREAK);
    unsigned pk_data = FL;
    for (int i = 0; i < 6; i++) L(0x5A5A0000u + (unsigned)i);
    CHECK(zh_check(F, FL, "prog", &inf, msg, sizeof msg) == 0 && inf.ovl && inf.ovl_words == 2 && inf.nodes == 0 &&
          inf.end <= pk_data && inf.bytes[1] == 0x100 + 8, "ovl: Peek's form taken, no nodes, chip BSS from the header (%s)", msg);
    build_ovl(0, 0);
    CHECK(zh_node(F, FL, ov_tail, "prog", &na, msg, sizeof msg) == -ZE_NOEXEC && zh_node(F, FL, FL + 4, "prog", &na, msg, sizeof msg) == -ZE_NOEXEC,
          "ovl: zh_node where no node is, or past the end, refused");
    /* every prefix and one-byte change: whatever the check takes, the root and every node load */
    build_ovl(0, 0);
    unsigned good = FL, nodes_loaded = 0, mismatch = 0, ok = 0;
    unsigned char keep[8192];
    memcpy(keep, F, FL);
    for (unsigned n = 0; n <= good; n++)
        for (unsigned i = 0; i < good + 1; i++)
            for (int v = 0; v < (i < good && n == good ? 3 : 1); v++) {
                memcpy(F, keep, good);
                if (i < good && n == good)
                    F[i] = v == 0 ? 0 : v == 1 ? 0xFF : F[i] ^ 0x01;
                else if (i)
                    break;
                if (zh_check(F, n, "p", &inf, msg, sizeof msg))
                    continue;
                ok++;
                struct halloc b = { 0, -1, 0, {0}, 0x00200000 };
                struct zh_ops o2 = { h_alloc, h_free, &b };
                static struct zh_seg s[ZH_MAXHUNKS], s2[ZH_MAXHUNKS];
                if (zh_load(F, n, &inf, &o2, s, &sl)) { mismatch++; continue; }
                if (inf.ovl) {
                    static unsigned char t2[(ZH_MAXTAB + 1) * 4];
                    zu32 tn = inf.tabsize + 1;
                    CHECK(tn <= sizeof t2 / 4, "ovl: the table size within ZH_MAXTAB");
                    zh_table(&inf, s, t2, tn);
                    for (zu32 p = inf.end; n - p >= 4 && zbe32(F + p) == HUNK_HEADER;) {
                        if (zh_node(F, n, p, "p", &na, msg, sizeof msg)) { mismatch++; break; }
                        if (zh_load_node(F, n, &na, &o2, s2, t2, tn, &sla)) { mismatch++; break; }
                        nodes_loaded++;
                        free_all(s2, na.nhunks, &b);
                        p = na.end;
                    }
                }
                free_all(s, inf.nhunks, &b);
                CHECK(b.live == 0, "ovl: everything freed");
            }
    CHECK(mismatch == 0, "ovl: %u changed files the check took did not load", mismatch);
    printf("ovl: %u prefixes and changes taken, %u nodes loaded from them\n", ok, nodes_loaded);
}

/* ---- the volume */
static void test_vol(void)
{
    static zu32 blkw[16384];
    void *b = blkw;
    zu32 sz = zv_need(16, 300, 6);
    CHECK(sz && sz <= sizeof blkw, "vol: zv_need");
    CHECK(zv_init(b, sz, 16, "Arte demo") == 0 && !strcmp(zv_ent(b, 0)->name, "Arte demo"), "vol: init, the volume name");
    zu8 *d;
    int arte = zv_add(b, 0, "Arte", 0, 0755, 252460800u + 86400 + 61, 10, &d);
    CHECK(arte == 1, "vol: a file");
    memcpy(d, "0123456789", 10);
    int data = zv_add(b, 0, "data", 1, 0755, 0, 0, 0);
    int mod = zv_add(b, data, "Music.mod", 0, 0444, 0, 5, &d);
    memcpy(d, "MUSIC", 5);
    int rk = zv_add(b, data, "R\xc3\xa4ksm\xc3\xb6rg\xc3\xa5s", 0, 0200, 0, 1, &d);   /* UTF-8 Latin-1 letters */
    int c = zv_add(b, 0, "c", 1, 0755, 0, 0, 0);
    int dir = zv_add(b, c, "Dir", 0, 0700, 0, 0, 0);
    CHECK(data > 0 && mod > 0 && rk > 0 && c > 0 && dir > 0, "vol: entries added");
    /* 16.10: names the same but for case are both kept (a Unix filesystem has them); zv_twin names the earlier */
    int arte2 = zv_add(b, 0, "ARTE", 0, 0644, 0, 0, 0);
    int rk2 = zv_add(b, data, "r\xc3\x84KSM\xc3\x96RG\xc3\x85S", 0, 0644, 0, 0, 0);
    int xyl = zv_add(b, data, "XYL.TP3", 0, 0644, 0, 0, 0);
    CHECK(arte2 > 0 && rk2 > 0 && xyl > 0, "vol: names the same but for case both added");
    CHECK(zv_twin(b, arte2) == (zu32)arte && zv_twin(b, rk2) == (zu32)rk && !zv_twin(b, arte) && !zv_twin(b, xyl) &&
          !zv_twin(b, 0) && !zv_twin(b, 999), "vol: zv_twin names the earlier of two names the same but for case");
    CHECK(zv_add(b, data, "Arte", 0, 0644, 0, 0, 0) > 0, "vol: the same name in another directory");
    CHECK(zv_add(b, 0, "a:b", 0, 0644, 0, 0, 0) == ZV_E_NAME && zv_add(b, 0, "\xe6\x97\xa5", 0, 0644, 0, 0, 0) == ZV_E_NAME &&
          zv_add(b, 0, ".", 0, 0644, 0, 0, 0) == ZV_E_NAME && zv_add(b, 0, "\x81x", 0, 0644, 0, 0, 0) == ZV_E_NAME &&
          zv_add(b, 0, "", 0, 0644, 0, 0, 0) == ZV_E_NAME, "vol: names outside AmigaDOS's rules left out");
    char lng[120];
    memset(lng, 'x', 108); lng[108] = 0;
    CHECK(zv_add(b, 0, lng, 0, 0644, 0, 0, 0) == ZV_E_LONG, "vol: a name over 107 bytes left out");
    lng[107] = 0;
    CHECK(zv_add(b, 0, lng, 0, 0644, 0, 0, 0) > 0, "vol: a name of 107 bytes");
    CHECK(zv_add(b, arte, "x", 0, 0644, 0, 0, 0) == ZV_E_PARENT, "vol: a file is no directory");
    CHECK(zv_add(b, 0, "big", 0, 0644, 0, 1000, &d) == ZV_E_SPACE, "vol: no room");
    const struct zv_ent *e = zv_ent(b, arte);
    CHECK(e->prot == 0 && e->days == 1 && e->mins == 1 && e->ticks == 50, "vol: bits and date (rwx, 1978-01-02 00:01:01)");
    CHECK(zv_ent(b, mod)->prot == (ZV_FIBF_WRITE | ZV_FIBF_DELETE | ZV_FIBF_EXECUTE) && zv_ent(b, rk)->prot == (ZV_FIBF_READ | ZV_FIBF_EXECUTE) &&
          zv_ent(b, mod)->days == 0, "vol: r-- and -w- bits, a date before 1978");
    CHECK(!strcmp(zv_ent(b, rk)->name, "R\xe4ksm\xf6rg\xe5s"), "vol: UTF-8 made Latin-1");
    /* 16.12: a name that is not UTF-8 (copied byte for byte from an AmigaDOS disk) is taken as Latin-1 as it is */
    int raw = zv_add(b, c, "Cr\xe8me \xe0 la Demo", 0, 0644, 0, 0, 0), raw2 = zv_add(b, c, "\xc3", 0, 0644, 0, 0, 0);
    CHECK(raw > 0 && !strcmp(zv_ent(b, raw)->name, "Cr\xe8me \xe0 la Demo") && raw2 > 0 &&
          !strcmp(zv_ent(b, raw2)->name, "\xc3"), "vol: a Latin-1 name that is not UTF-8 taken byte for byte");
    CHECK(zv_check(b, sz) == 0, "vol: zv_check takes it");
    zu32 o;
#define FIND(cur, p, want, res) do { o = 99; int rr = zv_find(b, cur, p, &o); \
        CHECK(rr == (res) && (rr || o == (zu32)(want)), "vol: find \"%s\" from %d: %d -> %u", p, (int)(cur), rr, o); } while (0)
    FIND(0, "Arte", arte, 0);
    FIND(0, "arte", arte, 0);
    FIND(0, "DATA/music.MOD", mod, 0);
    FIND(0, "arte demo:data/Music.mod", mod, 0);
    FIND(data, "SYS:Arte", arte, 0);
    FIND(data, "PROGDIR:Arte", arte, 0);
    FIND(data, ":data", data, 0);
    FIND(data, "/Arte", arte, 0);
    FIND(0, "data//Arte", arte, 0);
    FIND(data, "R\xc4KSM\xd6RG\xc5S", rk, 0);
    FIND(0, "C:Dir", dir, 0);
    FIND(0, "c:", c, 0);
    FIND(0, "data/", data, 0);
    FIND(0, "", 0, 0);
    FIND(0, "/", 0, -ZE_NOENT);
    FIND(0, "LIBS:x", 0, -ZE_NOENT);
    FIND(0, "NIL:", 0, -ZE_NODEV);
    FIND(0, "nothere", 0, -ZE_NOENT);
    FIND(0, "Arte/x", 0, -ZE_NOTDIR);
    FIND(arte, "x", 0, -ZE_NOTDIR);
    /* 16.10: the exact name first, else the first added; a path's every name found without case */
    FIND(0, "ARTE", arte2, 0);
    FIND(0, "aRTE", arte, 0);
    FIND(data, "R\xe4ksm\xf6rg\xe5s", rk, 0);
    FIND(data, "r\xc4KSM\xd6RG\xc5S", rk2, 0);
    FIND(data, "r\xe4KSM\xf6RG\xe5S", rk, 0);
    FIND(0, "PROGDIR:data/xyl.tp3", xyl, 0);
    FIND(0, "DATA/Xyl.Tp3", xyl, 0);
    FIND(0, "arte DEMO:Data/xYL.tP3", xyl, 0);
    FIND(0, "Data/XYL.TP3/", xyl, 0);
    FIND(0, "Data/XYL.TP", 0, -ZE_NOENT);
    /* AmigaDOS's case rule: ASCII and the Latin-1 letters (a grave to thorn against A grave to Thorn), not the
     * multiplication and division signs, not sharp s against y diaeresis, nothing else */
    CHECK(zv_same("abcZ", 4, "ABCz") && zv_same("\xe0\xe9\xf6\xf8\xfe", 5, "\xc0\xc9\xd6\xd8\xde") &&
          !zv_same("\xd7", 1, "\xf7") && !zv_same("\xdf", 1, "\xff") && !zv_same("@", 1, "`") &&
          !zv_same("[", 1, "{") && !zv_same("\xa0", 1, "\x80") && !zv_same("a", 1, "ab"),
          "vol: the case rule (ASCII, Latin-1 letters only)");
    zu32 k = zv_next(b, data, 0), n = 0;
    while (k) n++, k = zv_next(b, data, k);
    CHECK(n == 5 && zv_next(b, data, 0) == (zu32)mod, "vol: a directory's entries in order");
    /* damage is found */
    struct zv_hdr *h = b;
    h->nent = h->cap + 1;
    CHECK(zv_check(b, sz) == -ZE_INVAL, "vol: a bad count refused");
    h->nent = 3;
    ((struct zv_ent *)(h + 1))[2].parent = 2;
    CHECK(zv_check(b, sz) == -ZE_INVAL, "vol: a loop refused");
    ((struct zv_ent *)(h + 1))[2].parent = 0;
    ((struct zv_ent *)(h + 1))[1].off = sz;
    CHECK(zv_check(b, sz) == -ZE_INVAL && zv_check(b, sz - 4) == -ZE_INVAL, "vol: data outside, a wrong length refused");
    CHECK(zv_fits(2000, 48u << 20, 64u << 20) && !zv_fits(2001, 1, 64u << 20) && !zv_fits(1, (48u << 20) + 1, 64u << 20) &&
          !zv_fits(1, 3u << 20, (3u << 20) - 1) && zv_fits(1, 3u << 20, 3u << 20),
          "vol: the limits (2000 files, 48 MB, the budget given)");
    CHECK(zv_init(b, sz, 1, "a:b") == 0 && !strcmp(h->volname, "ZINE") && zv_add(b, 0, "x", 0, 0644, 0, 0, 0) == ZV_E_FULL,
          "vol: a volume name that is no AmigaDOS name, a full table");
}

/* ---- the planner, on a small model of memory: chip 512 KB (the RAM disk and the klog area outside the pools), fast
 * 1 MB from 0x200000 (the kernel at its bottom outside the pools) */
#define CHIPSZ 0x80000u
#define FASTLO 0x200000u
#define FASTSZ 0x100000u
static unsigned char M[FASTLO + FASTSZ];

static void test_plan(void)
{
    struct zp_region reg[2] = { { ZMEMF_CHIP | ZMEMF_PUBLIC, 0, CHIPSZ }, { ZMEMF_FAST | ZMEMF_PUBLIC, FASTLO, FASTLO + FASTSZ } };
    struct zp_blk blk[64];
    struct zp_range rng[4];
    int nb = 0, nr = 0;
    zu32 klog = CHIPSZ - 0x4200, rd0 = 0x40000, rd1 = 0x50000, kend = FASTLO + 0x30000;
    /* chip: [0x2000, rd0) and [rd1, klog): used, free, used ... */
    zu32 cuts[][3] = { { 0x2000, rd0, 0 }, { rd1, klog, 1 } };
    for (int r = 0; r < 2; r++) {
        zu32 a = cuts[r][0], hi = cuts[r][1];
        rng[nr] = (struct zp_range){ a, hi, 1, nb, 0 };
        int used = 1;
        while (a < hi) {
            zu32 s = hi - a > 0x6000 ? 0x3000u + (zu32)(nb % 3) * 0x800u : hi - a;
            blk[nb++] = (struct zp_blk){ a, s, used };
            used ^= 1;
            a += s;
        }
        rng[nr].n = nb - rng[nr].first;
        nr++;
    }
    /* fast: small used blocks, a big free middle, big used blocks at the top */
    {
        zu32 a = kend, hi = FASTLO + FASTSZ;
        rng[nr] = (struct zp_range){ a, hi, 0, nb, 0 };
        zu32 sz[] = { 0x400, 0x200, 0x100, 0x800 }, u[] = { 1, 0, 1, 1 };
        for (int i = 0; i < 4; i++) { blk[nb++] = (struct zp_blk){ a, sz[i], u[i] }; a += sz[i]; }
        zu32 top = hi - 0x20000;
        blk[nb++] = (struct zp_blk){ a, top - a, 0 };
        blk[nb++] = (struct zp_blk){ top, 0x10000, 1 };
        blk[nb++] = (struct zp_blk){ top + 0x10000, 0x10000, 1 };
        rng[nr].n = nb - rng[nr].first;
        nr++;
    }
    struct zp_in in = { reg, 2, rng, nr, blk, nb, 20000 };
    struct zp_piece pc[80];
    struct zp_chunk ch[80];
    struct zp_out o;
    memset(&o, 0, sizeof o);
    o.piece = pc, o.piece_cap = 80, o.chunk = ch, o.chunk_cap = 80;
    int r = zp_plan(&in, &o);
    CHECK(r == 0 && o.why == ZP_OK, "plan: planned (%d, why %d)", r, o.why);
    /* the model: random contents; remember them, save the pieces, let the "program" write all its memory, put back */
    srand(7);
    for (zu32 i = 0; i < sizeof M; i++) M[i] = rand();
    static unsigned char before[sizeof M], save[sizeof M];
    memcpy(before, M, sizeof M);
    zu32 at = 0, given = 0;
    for (zu32 i = 0; i < o.npiece; i++) { memcpy(save + at, M + pc[i].addr, pc[i].len); at += pc[i].len; }
    CHECK(at == o.save, "plan: the saved bytes add up");
    static unsigned char mark[sizeof M];
    memset(mark, 0, sizeof M);
    int overlap = 0;
    for (zu32 i = 0; i < o.nchunk; i++) {
        for (zu32 k = ch[i].addr; k < ch[i].addr + ch[i].len; k++) overlap += mark[k]++ != 0;
        memset(M + ch[i].addr, 0xA5, ch[i].len);
        given += ch[i].len;
        CHECK(ch[i].addr % 16 == 0 && ch[i].len % 16 == 0 && ch[i].len >= 16, "plan: chunk %u aligned", i);
    }
    CHECK(overlap == 0, "plan: nothing given twice");
    CHECK(given == o.chip_free + o.fast_free, "plan: the free bytes add up");
    at = 0;
    for (zu32 i = 0; i < o.npiece; i++) { memcpy(M + pc[i].addr, save + at, pc[i].len); at += pc[i].len; }
    /* everything Zenix owns is as before: outside the pools, every used block, every free block's header, low memory */
    int lost = 0, kept_given = 0;
    for (zu32 k = 0; k < sizeof M; k++) {
        int inpool = 0, freedata = 0;
        for (int b = 0; b < nb; b++)
            if (k >= blk[b].addr && k < blk[b].addr + blk[b].size) { inpool = 1; freedata = !blk[b].used && k >= blk[b].addr + 16; }
        int region = k < CHIPSZ || k >= FASTLO;
        if (!freedata && region && M[k] != before[k]) lost++;
        if (region && (!inpool && k >= 0x2000) && mark[k]) kept_given++;
        if (k < 0x1000 && mark[k]) kept_given++;
    }
    CHECK(lost == 0, "plan: after the restore every byte Zenix owns is back (%d differ)", lost);
    CHECK(kept_given == 0, "plan: the kernel, the RAM disk, the klog area and the low page are never given (%d)", kept_given);
    CHECK(o.chip_free == (0x2000 - 0x1000) + (rd0 - 0x2000) + (klog - rd1), "plan: chip: low memory, the pools, around the RAM disk");
    zu32 ff = 0x200 + (FASTLO + FASTSZ - 0x20000 - (kend + 0x400 + 0x200 + 0x100 + 0x800));
    CHECK(o.fast_free == ff && o.fast_largest == ff - 0x200 && o.fast_total == FASTSZ, "plan: fast: exactly Zenix's free blocks");
    CHECK(o.block % 16 == 0 && o.block >= 20000 + o.save + 3 * (16u << 10), "plan: the ZINE block's size (%u)", o.block);
    /* 16.12: blocks that are the program's already (its hunks loaded by zine): a chip one neither saved (but its
     * header) nor given, the chunks split around it; a fast one counted as the program's for the half rule */
    {
        zu32 cf = o.chip_free;
        blk[0].used = ZP_KEEP;
        blk[nb - 3].used = ZP_KEEP;     /* the big free fast block as a program's region */
        memset(&o, 0, sizeof o);
        o.piece = pc, o.piece_cap = 80, o.chunk = ch, o.chunk_cap = 80;
        int rk = zp_plan(&in, &o);
        int inchunk = 0, insaved = 0, hdr = 0;
        for (zu32 i = 0; i < o.nchunk; i++)
            if (ch[i].addr < blk[0].addr + blk[0].size && blk[0].addr < ch[i].addr + ch[i].len)
                inchunk++;
        for (zu32 i = 0; i < o.npiece; i++) {
            if (pc[i].addr == blk[0].addr && pc[i].len == 16)
                hdr++;
            else if (pc[i].addr < blk[0].addr + blk[0].size && blk[0].addr < pc[i].addr + pc[i].len)
                insaved++;
        }
        CHECK(rk == 0 && o.why == ZP_OK && !inchunk && !insaved && hdr == 1 && o.chip_free == cf - blk[0].size &&
              o.fast_prog == blk[nb - 3].size, "plan: the program's own blocks kept out (%d, why %d)", rk, o.why);
        blk[0].used = 1;
        blk[nb - 3].used = 0;
        memset(&o, 0, sizeof o);
        o.piece = pc, o.piece_cap = 80, o.chunk = ch, o.chunk_cap = 80;
    }
    /* 16.18: the unused end of a used block (the ZINE block's) given too, in address order; refused in a free block,
     * over a chunk, in chip RAM, across two blocks */
    {
        memset(&o, 0, sizeof o);
        o.piece = pc, o.piece_cap = 80, o.chunk = ch, o.chunk_cap = 80;
        CHECK(zp_plan(&in, &o) == 0, "give: planned");
        zu32 n0 = o.nchunk, ff0 = o.fast_free, top = FASTLO + FASTSZ - 0x20000;
        int g1 = zp_give(&in, &o, top + 0x8001, top + 0x10000);     /* rounded in to top + 0x8010 */
        int g2 = zp_give(&in, &o, kend + 0x100, kend + 0x400);      /* the first small used block: before the chunks */
        int order = 1;
        for (zu32 i = 1; i < o.nchunk; i++)
            if (ch[i].reg == ch[i - 1].reg && ch[i].addr <= ch[i - 1].addr) order = 0;
        CHECK(g1 == 0 && g2 == 0 && o.nchunk == n0 + 2 && o.fast_free == ff0 + (0x8000 - 0x10) + 0x300 && order &&
              o.fast_largest >= 0x7FF0, "give: two used blocks' ends become chunks, in address order (%d %d)", g1, g2);
        CHECK(zp_give(&in, &o, top - 0x1000, top - 0x800) == -ZE_INVAL, "give: memory of a free block is refused");
        CHECK(zp_give(&in, &o, top + 0x9000, top + 0xA000) == -ZE_INVAL, "give: over a chunk given before: refused");
        CHECK(zp_give(&in, &o, 0x2100, 0x2800) == -ZE_INVAL, "give: chip RAM is refused");
        CHECK(zp_give(&in, &o, top + 0x10010, top + 0x10000 + 0x10000) == 0 &&
              zp_give(&in, &o, top + 0xF000, top + 0x10000 + 0x100) == -ZE_INVAL, "give: across two blocks: refused");
        CHECK(zp_give(&in, &o, top, top + 0x100) == -ZE_INVAL, "give: a block's header is never given");
        o.chunk_cap = o.nchunk;
        CHECK(zp_give(&in, &o, kend + 0x800, kend + 0xA00) == -ZE_NOMEM, "give: a full array");
        memset(&o, 0, sizeof o);
        o.piece = pc, o.piece_cap = 80, o.chunk = ch, o.chunk_cap = 80;
    }
    /* refusals and bad walks */
    blk[nb - 3].used = 1;
    CHECK(zp_plan(&in, &o) == 0 && o.why == ZP_HOLDS, "plan: Zenix holding more than half the fast RAM is refused");
    blk[nb - 3].used = 0;
    in.nreg = 1; in.nrng = 2;
    CHECK(zp_plan(&in, &o) == 0 && o.why == ZP_NOFAST, "plan: a chip-only machine is refused");
    in.nreg = 2; in.nrng = nr;
    blk[1].size -= 16;
    CHECK(zp_plan(&in, &o) == -ZE_INVAL, "plan: blocks that do not tile their range");
    blk[1].size += 16;
    rng[0].lo = 0x1000; blk[0].addr = 0x1000; blk[0].size += 0x1000;
    CHECK(zp_plan(&in, &o) == -ZE_INVAL, "plan: a chip pool in low memory");
    rng[0].lo = 0x2000; blk[0].addr = 0x2000; blk[0].size -= 0x1000;
    rng[2].chip = 1;
    CHECK(zp_plan(&in, &o) == -ZE_INVAL, "plan: a range of the wrong kind");
    rng[2].chip = 0;
    reg[1].lower = CHIPSZ - 16;
    CHECK(zp_plan(&in, &o) == -ZE_INVAL, "plan: regions over each other");
    reg[1].lower = FASTLO;
    o.piece_cap = 3;
    CHECK(zp_plan(&in, &o) == -ZE_NOMEM, "plan: arrays too small");
    o.piece_cap = 80;
    CHECK(zp_plan(&in, &o) == 0 && o.why == ZP_OK, "plan: as before again");
}

static void test_lvo(void)
{
    CHECK(zl_name("exec.library", 198) && !strcmp(zl_name("exec.library", 198), "AllocMem"), "lvo: exec AllocMem");
    CHECK(zl_name("dos.library", 114) && !strcmp(zl_name("dos.library", 114), "Info"), "lvo: dos Info (section 15's example)");
    CHECK(zl_name("graphics.library", 222) && !strcmp(zl_name("graphics.library", 222), "LoadView"), "lvo: graphics LoadView");
    CHECK(zl_name("intuition.library", 78) && !strcmp(zl_name("intuition.library", 78), "CloseWorkBench"), "lvo: intuition CloseWorkBench");
    CHECK(zl_name("ciab.resource", 6) && !strcmp(zl_name("ciab.resource", 6), "AddICRVector") &&
          !strcmp(zl_name("potgo.resource", 18), "WritePotgo"), "lvo: the resources");
    CHECK(!zl_name("exec.library", 786) && !zl_name("exec.library", 199) && !zl_name("exec.library", 24) &&
          !zl_name("dos.library", 486) && !zl_name("timer.device", 42), "lvo: beyond V37, odd, reserved, unknown: no name");
    CHECK(ZL_exec_OpenLibrary == 552 && ZL_graphics_WaitTOF == 270 && ZL_dos_Output == 60, "lvo: the defines");
}

/* the assigns and Execute's Assign command (zpath.h; docs/d29-zine.md 16.8) */
static void test_path(void)
{
    static struct za_table t;
    char o[200];
    za_init(&t);
    CHECK(za_expand(&t, "LIBS:x.library", 0, o, sizeof o) && !strcmp(o, "SYS:LIBS/x.library"), "path: LIBS: 0 (%s)", o);
    CHECK(za_expand(&t, "libs:x.library", 1, o, sizeof o) && !strcmp(o, "PROGDIR:x.library"), "path: LIBS: 1 (%s)", o);
    CHECK(za_expand(&t, "LIBS:x.library", 2, o, sizeof o) && !strcmp(o, "RAM:x.library"), "path: LIBS: 2 (%s)", o);
    CHECK(!za_expand(&t, "LIBS:x.library", 3, o, sizeof o), "path: LIBS: has three");
    CHECK(za_expand(&t, "data/pic", 0, o, sizeof o) && !strcmp(o, "data/pic") && !za_expand(&t, "data/pic", 1, o, sizeof o),
          "path: an unassigned path stands for itself only");
    CHECK(za_ram("RAM:a") && !strcmp(za_ram("ram:a"), "a") && za_ram("Ram Disk:b/c") && !za_ram("RAMX:a") &&
          !za_ram("ram") && !strcmp(za_ram("RAM:"), ""), "path: RAM: and Ram Disk:");
    /* the memory files' keys (16.9): one name per file, whatever the path's form */
    {
        static const char *const kv[][2] = {
            { "data", "SYS:data" }, { "PROGDIR:data", "SYS:data" }, { "sys:Data", "SYS:Data" }, { "MyDemo:data", "SYS:data" },
            { ":data", "SYS:data" }, { "c:Assign", "SYS:C/Assign" }, { "LIBS:x.library", "SYS:LIBS/x.library" },
            { "ram:x", "RAM:x" }, { "Ram Disk:x", "RAM:x" }, { "RAM:", "RAM:" }, { "t:tmp.1", "T:tmp.1" },
            { "env:Sys/def_pref", "ENV:Sys/def_pref" }, { "dh0:a/b/c", "DH0:a/b/c" }, { "a/b//c", "SYS:a/c" },
            { "a//b", "SYS:b" }, { "a/b/", "SYS:a/b" }, { "C:/x", "SYS:x" }, { "a//", "SYS:" }, { 0, 0 } };
        for (int i = 0; kv[i][0]; i++)
            CHECK(za_key(kv[i][0], "MyDemo", o, sizeof o) && !strcmp(o, kv[i][1]), "key: %s -> %s (%s)", kv[i][0], kv[i][1], o);
        CHECK(!za_key("/x", "MyDemo", o, sizeof o) && !za_key("t:/x", "MyDemo", o, sizeof o) && !za_key("a///x", 0, o, sizeof o),
              "key: above a device's top refused");
        CHECK(!za_key("ram:abcdefgh", 0, o, 10) && za_key("ram:abcde", 0, o, 10) && !strcmp(o, "RAM:abcde"),
              "key: the limit (max bytes with the end)");
        char o2[200];                                   /* 16.10: a write to "Foo" and a read of "FOO" meet */
        CHECK(za_key("PROGDIR:Data/Xyl.\xe5\xe4", "MyDemo", o, sizeof o) && za_key("mydemo:DATA/xYL.\xc5\xc4", "MyDemo", o2, sizeof o2) &&
              zv_same(o, (zu32)strlen(o), o2) && za_key("t:\xd7", 0, o, sizeof o) && za_key("T:\xf7", 0, o2, sizeof o2) &&
              !zv_same(o, (zu32)strlen(o), o2), "key: keys compare without case (Latin-1 letters too, not x and the division sign)");
        for (int n = 1; n < 14; n++) {                  /* every short room: never written past (ASan) */
            char *b = malloc((size_t)n);
            CHECK(za_key("dh0:abc/def", 0, b, (zu32)n) == (n >= 12), "key: room %d", n);
            free(b);
        }
    }
    struct za_cmd c;
    const char *cmd = "Ram:Assign LIBS: Ram:";
    CHECK(za_parse(cmd, strlen(cmd), &c) == 1 && !strcmp(c.name, "LIBS") && c.n == 1 && !strcmp(c.dir[0], "Ram:") &&
          !c.add && !c.remove, "path: \"%s\" parsed", cmd);
    CHECK(za_apply(&t, &c) == 0 && za_expand(&t, "LIBS:p.library", 0, o, sizeof o) && !strcmp(o, "Ram:p.library") &&
          !za_expand(&t, "LIBS:p.library", 1, o, sizeof o), "path: LIBS: is Ram: alone");
    cmd = "C:Assign LIBS: SYS:LIBS/\n";
    CHECK(za_parse(cmd, strlen(cmd), &c) == 1 && za_apply(&t, &c) == 0 && za_expand(&t, "LIBS:p", 0, o, sizeof o) &&
          !strcmp(o, "SYS:LIBS/p"), "path: back to SYS:LIBS/ (%s)", o);
    cmd = "assign DATA: \"Ram Disk:my dir\" ADD";
    CHECK(za_parse(cmd, strlen(cmd), &c) == 1 && c.add && c.n == 1 && !strcmp(c.dir[0], "Ram Disk:my dir") &&
          za_apply(&t, &c) == 0 && za_expand(&t, "DATA:f", 0, o, sizeof o) && !strcmp(o, "Ram Disk:my dir/f"),
          "path: a quoted directory, ADD to a new name (%s)", o);
    cmd = "Assign X: DATA:sub";         /* an assign through another */
    CHECK(za_parse(cmd, strlen(cmd), &c) == 1 && za_apply(&t, &c) == 0 && za_expand(&t, "X:f", 0, o, sizeof o) &&
          !strcmp(o, "Ram Disk:my dir/sub/f"), "path: an assign through another (%s)", o);
    cmd = "Assign X:";
    CHECK(za_parse(cmd, strlen(cmd), &c) == 1 && za_apply(&t, &c) == 0 && za_expand(&t, "X:f", 0, o, sizeof o) &&
          !strcmp(o, "X:f"), "path: an assign removed");
    cmd = "Assign A: B: C: D: E:";
    CHECK(za_parse(cmd, strlen(cmd), &c) == -1, "path: four directories refused");
    CHECK(za_parse("Assign", 6, &c) == -1 && za_parse("assign LIST", 11, &c) == -1 && za_parse("Assign X: DISMOUNT", 18, &c) == -1,
          "path: a listing and options ZINE does not do");
    CHECK(za_parse("run >nil: assign", 16, &c) == 0 && za_parse("setpatch", 8, &c) == 0 && za_parse("", 0, &c) == 0 &&
          za_parse("Assigner X: Y", 13, &c) == 0, "path: other commands are not Assign");
    cmd = "Assign LIBS: Ram:; echo hi";
    CHECK(za_parse(cmd, strlen(cmd), &c) == 1 && c.n == 1 && !strcmp(c.dir[0], "Ram:"), "path: the command ends at ';'");
    cmd = "Assign LIBS: Ram:";
    CHECK(za_parse(cmd, 13, &c) == 1 && c.n == 0, "path: the command ends at its length");
    {   /* 16.11: Execute's command lines */
        struct zx_cmd x;
        const char *l = "zpart >NIL: one  \"two words\"  ";
        CHECK(zx_parse(l, strlen(l), &x) == 1 && !strcmp(x.name, "zpart") && !strcmp(x.out, "NIL:") && !x.in[0] &&
              !x.run && !x.append && x.at == 0 && x.arglen == 16 && !memcmp(l + x.args, "one  \"two words\"", 16) &&
              x.next == strlen(l), "exec: name, >NIL:, the arguments as given, blanks at the end off");
        l = "run >nil: setpatch\nC:Part2 <in.txt >> \"Ram Disk:log\" a b ; a comment\n  \n";
        CHECK(zx_parse(l, strlen(l), &x) == 1 && x.run && !strcmp(x.name, "setpatch") && !strcmp(x.out, "nil:") &&
              x.arglen == 0 && x.next == 19 && x.at == 10, "exec: a leading run, its redirection, the line's end");
        CHECK(zx_parse(l + 19, strlen(l) - 19, &x) == 1 && !strcmp(x.name, "C:Part2") && !strcmp(x.in, "in.txt") &&
              !strcmp(x.out, "Ram Disk:log") && x.append && x.arglen == 3 && !memcmp(l + 19 + x.args, "a b", 3),
              "exec: < and >> with a quoted file after the name; a ';' comment ends the arguments");
        CHECK(zx_parse("  \n", 3, &x) == 0 && x.next == 3 && zx_parse("; only", 6, &x) == 0 && zx_parse("run", 3, &x) == 0 &&
              zx_parse("", 0, &x) == 0, "exec: empty lines, comments, a lone run");
        CHECK(zx_parse(">", 1, &x) == -1 && zx_parse("x >", 3, &x) == -1 && zx_parse(">a:", 3, &x) == -1,
              "exec: a redirection with no file or no command");
        l = "\"my part\" x;y";
        CHECK(zx_parse(l, strlen(l), &x) == 1 && !strcmp(x.name, "my part") && x.arglen == 1, "exec: a quoted name");
        l = "echo \"a;b\" c";
        CHECK(zx_parse(l, strlen(l), &x) == 1 && x.arglen == 7, "exec: a ';' inside quotes is no comment");
        l = "part one >NIL:";
        CHECK(zx_parse(l, strlen(l), &x) == 1 && !x.out[0] && x.arglen == 9, "exec: '>' after the arguments is an argument (the AmigaDOS Manual)");
        l = "Run Run x";
        CHECK(zx_parse(l, strlen(l), &x) == 1 && x.run && !strcmp(x.name, "Run"), "exec: only the first run is taken off");
        l = "x";
        CHECK(zx_parse(l, 0, &x) == 0, "exec: the line ends at its length");
        char lp[300];
        memset(lp, 'a', 299);
        lp[299] = 0;
        CHECK(zx_parse(lp, 299, &x) == -1, "exec: a name longer than 95 bytes");
        lp[0] = '>';
        CHECK(zx_parse(lp, 299, &x) == -1, "exec: a file longer than 95 bytes");
        for (size_t n = 0; n <= strlen(l = "run >nil: \"a b\" <c >>d e f ; g\nh"); n++) {
            char *b = malloc(n ? n : 1);        /* every prefix, read within its length only (ASan) */
            memcpy(b, l, n);
            CHECK(zx_parse(b, (zu32)n, &x) >= -1 && x.next <= n, "exec: prefix %zu", n);
            free(b);
        }
        CHECK(zx_known("setpatch") && zx_known("C:SetPatch") && zx_known("SYS:c/STACK") && zx_known("LoadWB") &&
              zx_known("cd") && !zx_known("1") && !zx_known("45") && !zx_known("Parts/Second") && !zx_known("setpatchx") &&
              !zx_known("") && !zx_known("Assign") && !zx_known("run"), "exec: the Shell's and C:'s commands known, others not");
    }
    /* the table: 8 names, then refused; a long path refused */
    za_init(&t);
    char nm[16];
    int e = 0;
    const char *d[1] = { "RAM:" };
    for (int i = 0; i < 7; i++) {
        snprintf(nm, sizeof nm, "N%d", i);
        e |= za_set(&t, nm, strlen(nm), d, 1, 0);
    }
    CHECK(e == 0 && za_set(&t, "N9", 2, d, 1, 0) == -ZE_NOSPC, "path: 8 assigns, the ninth refused");
    CHECK(za_set(&t, "N0", 2, d, 1, 1) == 0 && za_set(&t, "N0", 2, d, 1, 1) == 0 && za_set(&t, "N0", 2, d, 1, 1) == -ZE_NOSPC,
          "path: 3 directories, the fourth refused");
    char lp[300];
    memset(lp, 'a', 299);
    lp[299] = 0;
    CHECK(za_parse(lp, 299, &c) == 0, "path: a long word that is no Assign");
    memcpy(lp, "Assign Q: ", 10);
    CHECK(za_parse(lp, 299, &c) == -1, "path: a directory longer than 95 bytes refused");
    CHECK(!za_expand(&t, "LIBS:x", 0, o, 8), "path: a result longer than the room is none");
    /* every prefix and byte of a command: no crash (the sanitizers) */
    cmd = "Ram:Assign \"LIBS:\" \"Ram Disk:x\" ADD DEFER";
    for (size_t n = 0; n <= strlen(cmd); n++)
        za_parse(cmd, (zu32)n, &c);
    passes++;
}

/* d29 16.18: zh_lean, the plain big program loaded from its file even when it would fit beside its hunks */
static void test_lean(void)
{
    struct zh_info i;
    memset(&i, 0, sizeof i);
    zu32 K = 1024;
    i.end = 3339 * K;
    i.bytes[1] = 200 * K;
    CHECK(zh_lean(&i, 3339 * K, 1700 * K, 9300 * K), "lean: a big plain program is loaded from its file");
    i.end = 200 * K - 2, i.bytes[1] = 0;
    CHECK(zh_lean(&i, 200 * K, 1700 * K, 2000 * K), "lean: a few bytes of padding after its hunks are no data");
    i.end = 3339 * K, i.ovl = 1;
    CHECK(!zh_lean(&i, 3339 * K, 1700 * K, 9300 * K), "lean: not an overlaid one");
    i.ovl = 0, i.end = 3000 * K;
    CHECK(!zh_lean(&i, 3339 * K, 1700 * K, 9300 * K), "lean: not one with data after its hunks");
    i.end = 100 * K;
    CHECK(!zh_lean(&i, 100 * K, 1700 * K, 1000 * K), "lean: not below 128 KB");
    i.end = 400 * K;
    CHECK(!zh_lean(&i, 400 * K, 1700 * K, 9300 * K), "lean: not below a sixteenth of the free memory");
    i.end = 3339 * K, i.bytes[1] = 900 * K;
    CHECK(!zh_lean(&i, 3339 * K, 1700 * K, 9300 * K), "lean: not when its chip hunks need over half the free chip RAM");
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "seeds")) {      /* the test programs, as the hunk fuzzer's seeds */
        for (int form = 0; form < 4; form++) {
            char p[512];
            build(form, form & 1, form != 1);
            snprintf(p, sizeof p, "%s/form%d", argv[2], form);
            FILE *f = fopen(p, "wb");
            if (!f || fwrite(F, 1, FL, f) != FL)
                return 1;
            fclose(f);
        }
        char p[512];
        build_ovl(0, 0);                                /* and the overlaid one */
        snprintf(p, sizeof p, "%s/overlay", argv[2]);
        FILE *f = fopen(p, "wb");
        if (!f || fwrite(F, 1, FL, f) != FL)
            return 1;
        fclose(f);
        return 0;
    }
    test_hunk();
    test_lean();
    test_overlay();
    test_vol();
    test_plan();
    test_lvo();
    test_path();
    printf("%s: %d checks passed, %d failed\n", fails ? "FAIL zine-core" : "PASS zine-core", passes, fails);
    return fails != 0;
}
