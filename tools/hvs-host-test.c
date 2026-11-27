/* SPDX-License-Identifier: GPL-2.0-only */
/* The PC's test of display/server/hvslist.c (DESIGN D63, docs/d63-pistorm-gfx.md 11): the display list's words for
 * every depth, scaled and unscaled, against values worked out by hand from VideoCore.card's vc6.c and Linux's
 * vc4_regs.h; the geometry for random sizes; the mode list; two hand-made EDID blocks and broken ones; the answer
 * line. Run by tools/hvs-host-test.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../display/server/hvslist.h"
#include "../kernel/hwinfo.h"
#include "../kernel/mbox.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void words(const char *what, const struct hvsplane *p, const unsigned *want, int nw)
{
    unsigned o[40];
    memset(o, 0xA5, sizeof o);
    int n = hvs_list(p, o);
    CHECK(n == nw, "%s: %d words, not %d", what, n, nw);
    for (int i = 0; i < nw && i < n; i++)
        CHECK(o[i] == want[i], "%s: word %d is %08X, not %08X", what, i, o[i], want[i]);
    CHECK(o[n] == 0xA5A5A5A5u, "%s: written past the end", what);
    CHECK(n <= 32, "%s: longer than a list's 32 words", what);
}

static void lists(void)
{
    struct hvsplane a = { 1920, 1080, 32, 7680, 0x3E000000, 1920, 1080 };
    unsigned wa[] = { 0x4800D807, 0, 0x4000FFF0, 0x04380780, 0xDEADBEEF, 0xFE000000, 0xDEADBEEF, 7680, 0x80000000 };
    words("1920x1080x32 unity", &a, wa, 9);

    struct hvsplane b = { 640, 480, 16, 1280, 0x3E000000, 1920, 1080 };
    unsigned wb[] = { 0x51005804, 240, 0x4000FFF0, 0x043805A0, 0x01E00280, 0xDEADBEEF, 0xFE000000, 0xDEADBEEF, 1280,
                      0, 0xC071C780, 0xC071C780, 0, 0x808, 0x808, 0x808, 0x808, 0x80000000 };
    words("640x480x16 on 1920x1080", &b, wb, 18);

    struct hvsplane c = { 320, 256, 8, 320, 0x3E000000, 1920, 1080 };
    unsigned wc[] = { 0x5200780D, 285, 0x4000FFF0, 0x04380546, 0x01000140, 0xDEADBEEF, 0xFE000000, 0xDEADBEEF, 320,
                      0xC0002400, 0, 0xC03CAE80, 0xC03CAE80, 0, 0x808, 0x808, 0x808, 0x808, 0x80000000 };
    words("320x256x8 with the palette", &c, wc, 19);

    struct hvsplane d = { 1280, 1024, 16, 2560, 0x3E000000, 1280, 720 };
    unsigned wd[] = { 0x51005804, 190, 0x4000FFF0, 0x02D00384, 0x04000500, 0xDEADBEEF, 0xFE000000, 0xDEADBEEF, 2560,
                      0, 0xC16C1680, 0xC16C1680, 0, 0x808, 0x808, 0x808, 0x808, 0x80000000 };
    words("1280x1024x16 on 1280x720", &d, wd, 18);

    struct hvsplane e = { 1280, 720, 8, 1280, 0x3E000000, 1280, 720 };   /* the output's size at 8 bits: scaled 1:1 */
    unsigned o[40];
    int n = hvs_list(&e, o);
    CHECK(n == 19 && !(o[0] & 0x8000) && o[11] == 0xC1000080 && o[3] == (720u << 16 | 1280) && o[1] == 0,
          "1280x720x8 native: %d words, ctl %08X ppf %08X", n, o[0], o[11]);

    unsigned k[11], wk[11] = { 0, 0, 0x3FC0000, 0x3FDFEFF, 0x3FDFEFF, 0x1FEFF, 0x3FDFEFF, 0x3FDFEFF, 0x3FC0000, 0, 0 };
    hvs_kernel(k);
    for (int i = 0; i < 11; i++)
        CHECK(k[i] == wk[i], "kernel word %d %08X, not %08X", i, k[i], wk[i]);
    CHECK(hvs_argb(0x0F80) == 0xFFFF8800u && hvs_argb(0) == 0xFF000000u && hvs_argb(0xFFF) == 0xFFFFFFFFu,
          "palette words");
    CHECK(hvs_bpr(1000, 8) == 1008 && hvs_bpr(640, 16) == 1280 && hvs_bpr(1366, 16) == 2736 &&
          hvs_bpr(1366, 32) == 5472 && hvs_bpr(1001, 32) == 4016, "bytes a row");
    CHECK(hvs_format(8) == 1 && hvs_format(16) == 4 && hvs_format(32) == 9 && !hvs_format(24), "formats");
}

static void geometry(void)
{
    static const unsigned outs[3][2] = { { 1920, 1080 }, { 1280, 1024 }, { 1024, 768 } };
    srand(63);
    for (int t = 0; t < 600; t++) {
        unsigned ow = outs[t % 3][0], oh = outs[t % 3][1];
        unsigned w = 320 + (unsigned)rand() % (ow - 319), h = 256 + (unsigned)rand() % (oh - 255);
        struct hvsgeo g;
        hvs_fit(w, h, ow, oh, &g);
        CHECK(g.dw <= ow && g.dh <= oh && g.x + g.dw <= ow && g.y + g.dh <= oh, "%ux%u on %ux%u: outside", w, h, ow, oh);
        CHECK(g.dw >= ow - 1 || g.dh >= oh - 1, "%ux%u on %ux%u: %ux%u fills neither way", w, h, ow, oh, g.dw, g.dh);
        CHECK(g.dw >= w && g.dh >= h, "%ux%u on %ux%u: shrunk to %ux%u", w, h, ow, oh, g.dw, g.dh);
        CHECK((g.x == (ow - g.dw) / 2) && (g.y == (oh - g.dh) / 2), "%ux%u: not centred", w, h);
        long long ax = (long long)g.dw * h, ay = (long long)g.dh * w;    /* square pixels: dw/w == dh/h, within a pixel */
        CHECK(llabs(ax - ay) <= (long long)(w > h ? w : h) * 2, "%ux%u on %ux%u: not square (%ux%u)", w, h, ow, oh,
              g.dw, g.dh);
        struct hvsplane p = { w, h, 8u << (t % 3), hvs_bpr(w, 8u << (t % 3)), 0x3E000000, ow, oh };
        unsigned o[40];
        int n = hvs_list(&p, o);
        CHECK(n >= 9 && n <= 19 && o[n - 1] == 0x80000000u && (o[0] >> 24 & 0x3F) == (unsigned)n - 1,
              "%ux%u: list of %d words, control %08X", w, h, n, o[0]);
    }
}

static unsigned char edid1[128], edid2[128];
static void sum(unsigned char *b)
{
    unsigned s = 0;
    for (int i = 0; i < 127; i++)
        s += b[i];
    b[127] = (unsigned char)(256 - (s & 255));
}
static void make_edids(void)
{
    static const unsigned char head[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    unsigned char *b = edid1;
    memcpy(b, head, 8);
    b[8] = 0x10, b[9] = 0xAC;           /* "DEL" */
    b[18] = 1, b[19] = 3;
    b[35] = 0x21, b[36] = 0x08;         /* 640x480@60, 800x600@60, 1024x768@60 */
    for (int i = 0; i < 8; i++)
        b[38 + 2 * i] = b[39 + 2 * i] = 1;
    b[38] = 0x81, b[39] = 0x80;         /* 1280x1024 (5:4) */
    b[40] = 0xA9, b[41] = 0x40;         /* 1600x1200 (4:3) */
    unsigned char *d = b + 54;          /* 1920x1080, 148.5 MHz */
    d[0] = 0x02, d[1] = 0x3A, d[2] = 0x80, d[4] = 0x70, d[5] = 0x38, d[7] = 0x40;
    d = b + 72;
    d[3] = 0xFC;
    memcpy(d + 5, "DELL U2412M\n ", 13);
    d = b + 90;
    d[3] = 0xFD;                        /* a range descriptor: not a size */
    sum(b);

    b = edid2;                          /* 1280x1024 with no name and its sizes only in the standard timings */
    memcpy(b, head, 8);
    b[8] = 0x4C, b[9] = 0x2D;           /* "SAM" */
    b[18] = 1, b[19] = 4;
    b[35] = 0x80;                       /* 720x400 */
    b[37] = 0x80;                       /* 1152x870 */
    for (int i = 0; i < 8; i++)
        b[38 + 2 * i] = b[39 + 2 * i] = 1;
    b[38] = 0x81, b[39] = 0x80;         /* 1280x1024 */
    b[40] = 0xD1, b[41] = 0xC0;         /* 1920x1080 (16:9) */
    b[42] = 0x81, b[43] = 0x00;         /* 1280x800 (16:10, EDID 1.4) */
    d = b + 72;
    d[3] = 0xFC;
    memcpy(d + 5, "ABCDEFGHIJKLM", 13);  /* thirteen letters, no newline */
    sum(b);
}

static void edids(void)
{
    struct hvsedid e;
    CHECK(hvs_edid(edid1, 128, &e) == 0, "edid 1 refused");
    CHECK(!strcmp(e.name, "DELL U2412M") && !strcmp(e.maker, "DEL"), "edid 1: name '%s' maker '%s'", e.name, e.maker);
    static const unsigned short want1[6][2] = { { 1920, 1080 }, { 640, 480 }, { 800, 600 }, { 1024, 768 },
                                                { 1280, 1024 }, { 1600, 1200 } };
    CHECK(e.n == 6 && e.pref == 0, "edid 1: %d sizes, preferred %d", e.n, e.pref);
    for (int i = 0; i < 6 && i < e.n; i++)
        CHECK(e.w[i] == want1[i][0] && e.h[i] == want1[i][1], "edid 1 size %d: %ux%u", i, e.w[i], e.h[i]);

    CHECK(hvs_edid(edid2, 128, &e) == 0, "edid 2 refused");
    CHECK(!strcmp(e.name, "ABCDEFGHIJKLM") && !strcmp(e.maker, "SAM") && e.pref == -1, "edid 2: '%s' '%s' %d",
          e.name, e.maker, e.pref);
    static const unsigned short want2[5][2] = { { 720, 400 }, { 1152, 870 }, { 1280, 1024 }, { 1920, 1080 },
                                                { 1280, 800 } };
    CHECK(e.n == 5, "edid 2: %d sizes", e.n);
    for (int i = 0; i < 5 && i < e.n; i++)
        CHECK(e.w[i] == want2[i][0] && e.h[i] == want2[i][1], "edid 2 size %d: %ux%u", i, e.w[i], e.h[i]);

    unsigned char bad[128];
    memcpy(bad, edid1, 128);
    bad[100] ^= 1;
    CHECK(hvs_edid(bad, 128, &e) < 0 && e.n == 0 && !e.name[0], "a bad checksum taken");
    memcpy(bad, edid1, 128);
    bad[0] = 1;
    bad[127] = (unsigned char)(bad[127] - 1);
    CHECK(hvs_edid(bad, 128, &e) < 0, "a bad header taken");
    CHECK(hvs_edid(edid1, 64, &e) < 0, "a short block taken");
}

static void modes(void)
{
    struct hvsedid e;
    struct hvsmode m[HVS_MAXMODES + 1];
    hvs_edid(edid1, 128, &e);
    int n = hvs_modes(1920, 1080, 16u << 20, &e, m, HVS_MAXMODES);
    static const unsigned short want[10][2] = { { 320, 256 }, { 640, 480 }, { 640, 512 }, { 800, 600 },
        { 1024, 768 }, { 1280, 720 }, { 1280, 960 }, { 1440, 900 }, { 1280, 1024 }, { 1920, 1080 } };
    CHECK(n == 10, "1920x1080, 16 MB: %d modes", n);
    for (int i = 0; i < 10 && i < n; i++)
        CHECK(m[i].w == want[i][0] && m[i].h == want[i][1] && m[i].bits == 7, "mode %d: %ux%u bits %x", i, m[i].w,
              m[i].h, m[i].bits);
    CHECK(n == 10 && m[9].flags == (HVSM_NATIVE | HVSM_PREF) && m[0].flags == 0, "the native and preferred marks");
    n = hvs_modes(1920, 1080, 4u << 20, &e, m, HVS_MAXMODES);
    int ok = 0;
    for (int i = 0; i < n; i++) {
        if (m[i].w == 1920 && m[i].h == 1080)
            ok += m[i].bits == (HVSB_8 | HVSB_16);
        if (m[i].w == 1280 && m[i].h == 1024)
            ok += m[i].bits == (HVSB_8 | HVSB_16);
        if (m[i].w == 640 && m[i].h == 480)
            ok += m[i].bits == 7;
    }
    CHECK(ok == 3, "4 MB: the depths that fit (%d)", ok);
    n = hvs_modes(1024, 768, 16u << 20, 0, m, HVS_MAXMODES);
    CHECK(n == 5 && m[n - 1].w == 1024 && m[n - 1].flags == HVSM_NATIVE, "1024x768 without an EDID: %d", n);
    hvs_edid(edid2, 128, &e);
    n = hvs_modes(1280, 1024, 16u << 20, &e, m, HVS_MAXMODES);
    int seen800 = 0, seen870 = 0, seen1080 = 0;
    for (int i = 0; i < n; i++) {
        seen800 += m[i].w == 1280 && m[i].h == 800;
        seen870 += m[i].w == 1152 && m[i].h == 870;
        seen1080 += m[i].h == 1080;
        for (int j = 0; j < i; j++)
            CHECK(m[j].w != m[i].w || m[j].h != m[i].h, "a duplicate %ux%u", m[i].w, m[i].h);
        if (i)
            CHECK((unsigned)m[i - 1].w * m[i - 1].h <= (unsigned)m[i].w * m[i].h, "not sorted at %d", i);
    }
    CHECK(seen800 == 1 && seen870 == 1 && !seen1080, "the EDID's sizes that fit, and not the taller one");
    n = hvs_modes(1920, 1080, 16u << 20, &e, m, 4);
    CHECK(n == 4, "the cap: %d", n);
    CHECK(hvs_size_ok(1000, 700, 16, 1920, 1080, 16u << 20) && !hvs_size_ok(1921, 700, 16, 1920, 1080, 16u << 20) &&
          !hvs_size_ok(319, 700, 16, 1920, 1080, 16u << 20) && !hvs_size_ok(640, 255, 16, 1920, 1080, 16u << 20) &&
          !hvs_size_ok(640, 480, 24, 1920, 1080, 16u << 20) && !hvs_size_ok(1920, 1080, 32, 1920, 1080, 8294400u + 4095) &&
          !hvs_size_ok(320, 256, 8, 1920, 1080, 4096), "the size rule");
}

static void text(void)
{
    unsigned w, h, b;
    CHECK(hvs_parse_size("1280x720x16", &w, &h, &b) == 0 && w == 1280 && h == 720 && b == 16, "WxHxBITS");
    CHECK(hvs_parse_size("640x480", &w, &h, &b) == 0 && b == 0, "WxH");
    CHECK(hvs_parse_size("640x480x24", &w, &h, &b) < 0 && hvs_parse_size("640x", &w, &h, &b) < 0 &&
          hvs_parse_size("640x480x16x", &w, &h, &b) < 0 && hvs_parse_size("x480", &w, &h, &b) < 0 &&
          hvs_parse_size("99999999999x2", &w, &h, &b) < 0, "broken sizes taken");
}

/* the Pi's side on a pretend window: the table's checks, the words written and read back, the console kept */
static unsigned win[HVS_WINDOW / 4];
static void pi(void)
{
    static struct hwinfo h;
    static struct hwdisp d;
    struct hvspi p;
    CHECK(hvs_find(&h, &d, &p) != 0, "an empty table taken");
    h.magic = HWINFO_MAGIC;
    h.flags = HW_DT | HW_BCM2711;
    h.soc_base = 0xF2000000;
    h.vc4_base = 0x3E000000;
    h.vc4_size = 16u << 20;
    CHECK(hvs_find(&h, &d, &p) && !strcmp(hvs_find(&h, &d, &p), "the boot did not learn the HDMI's size"),
          "no HDMI size taken");
    d.flags = HWD_SIZE | HWD_EDID;
    d.w = 1920, d.h = 1080;
    memcpy(d.edid, edid1, 128);
    CHECK(hvs_find(&h, &d, &p) == 0 && p.hvs == 0xF2400000u && p.ow == 1920 && p.oh == 1080 && p.edid == d.edid &&
          p.vc4 == 0x3E000000u, "the Pi found");
    h.vc4_base = 0x3FF00000;
    CHECK(hvs_find(&h, &d, &p) != 0, "vc4-mem past 1 GB taken");
    h.vc4_base = 0x3E000000;
    h.flags = HW_DT;
    CHECK(hvs_find(&h, &d, &p) != 0, "a Pi 3 taken");
    d.w = 100;
    h.flags = HW_DT | HW_BCM2711;
    CHECK(hvs_find(&h, &d, &p) != 0, "an HDMI 100 pixels wide taken");

    memset(win, 0, sizeof win);
    p.hvs = (unsigned long)win;
    p.ow = 1920, p.oh = 1080;
    struct hvsplane pl = { 640, 480, 16, 1280, 0x3E000000, 1920, 1080 };
    CHECK(hvs_put(&p, &pl, HVS_DL_B) == 0, "the words put");
    unsigned o[40];
    int n = hvs_list(&pl, o);
    int same = 1;
    for (int i = 0; i < n; i++)
        same &= hvs_rd(hvs_dl(&p, HVS_DL_B + (unsigned)i)) == o[i];
    CHECK(same && hvs_rd(hvs_dl(&p, HVS_DL_BLANK)) == HVS_DL_END && hvs_rd(hvs_dl(&p, HVS_DL_KERNEL + 3)) == 0x3FDFEFF,
          "the list, the blank list, the kernel in the list memory");
    int outside = 0;
    for (unsigned i = 0; i < HVS_DLIST_WORDS; i++)
        if (win[HVS_DLIST / 4 + i] && !(i == HVS_DL_BLANK || (i >= HVS_DL_KERNEL && i < HVS_DL_KERNEL + 11) ||
                                        (i >= HVS_DL_B && i < HVS_DL_B + (unsigned)n)))
            outside++;
    CHECK(!outside, "%d words written outside Zenix's", outside);
    CHECK(hvs_con(&p) == 0, "a console from nothing");
    hvs_con_keep(&p, 0x2E8);
    CHECK(hvs_con(&p) == 0x2E8, "the console kept");
    hvs_con_keep(&p, HVS_DL_A);
    hvs_con_keep(&p, 0);
    CHECK(hvs_con(&p) == 0x2E8, "Zenix's own list or 0 kept as the console");
    hvs_wr((unsigned long)win + HVS_DISPSTAT1, 2u << 30 | 5u << 12);
    CHECK(hvs_running(&p) && hvs_frame(&p) == 5, "the channel's mode and frame");
    hvs_wr((unsigned long)win + HVS_DISPSTAT1, 1u << 30);
    CHECK(!hvs_running(&p), "a channel in INIT taken as running");
}

/* the kernel's mailbox message (kernel/mbox.h, emmc.c's mbox_call): sizes, the end tag, the buffer, the room */
static void mbox(void)
{
    unsigned m[48], le;
    memset(m, 0xA5, sizeof m);
    unsigned n = mbox_build(m, 16, 0x00040003, 0, 0, 2);
    CHECK(n == 8 && __builtin_bswap32(m[0]) == 32 && __builtin_bswap32(m[2]) == 0x40003 && __builtin_bswap32(m[3]) == 8 &&
          !m[1] && !m[4] && !m[7] && m[8] == 0xA5A5A5A5u, "the display size's message (%u words)", n);
    memset(m, 0xA5, sizeof m);
    n = mbox_build(m, 16, 0x00038002, 12, 250000000, 3);
    le = __builtin_bswap32(m[6]);
    CHECK(n == 9 && __builtin_bswap32(m[0]) == 36 && __builtin_bswap32(m[3]) == 12 && __builtin_bswap32(m[5]) == 12 &&
          le == 250000000 && !m[7] && !m[8] && m[9] == 0xA5A5A5A5u, "the clock rate's: its third word 0, then the end tag");
    memset(m, 0xA5, sizeof m);
    n = mbox_build(m, 40, 0x00030020, 0, 0, 34);
    int zero = 1;
    for (int i = 4; i < 40; i++)
        zero &= !m[i];
    CHECK(n == 40 && __builtin_bswap32(m[0]) == 160 && __builtin_bswap32(m[3]) == 136 && zero && m[40] == 0xA5A5A5A5u,
          "the EDID's: 40 words, all of the buffer and the end tag cleared");
    CHECK(mbox_build(m, 16, 0x00030020, 0, 0, 34) == 0 && mbox_build(m, 39, 0x00030020, 0, 0, 34) == 0,
          "a message bigger than its room refused");
}

int main(void)
{
    mbox();
    make_edids();
    lists();
    geometry();
    edids();
    modes();
    text();
    pi();
    printf("hvs-host-test: %d checks, %d failed\n", checks, fails);
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
