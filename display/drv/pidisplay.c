/* SPDX-License-Identifier: GPL-2.0-only */
/* pidisplay: what the Raspberry Pi's video scaler (HVS) shows, on a PiStorm under Emu68 (DESIGN D63,
 * docs/d63-pistorm-gfx.md, docs/man/pidisplay.txt). The display server draws the desktop on the Pi's picture itself
 * (display/server/hvslist.c's words); this program is for people and screenprefs:
 *   pidisplay --list        the HDMI's size and frame rate, the monitor (its EDID), the picture memory, the modes
 *   pidisplay --dump [WORD] the display list the HDMI shows (or the one at WORD of the list memory), a word a line
 *   pidisplay test WxHxBITS (root, from a text console) a test picture on the HDMI for 5 seconds, then back
 *   pidisplay console       (root) the HDMI back to the console's picture (after a display server that was killed)
 * The HDMI's size and the EDID are the kernel's record from the boot (kernel/hwinfo.h struct hwdisp); every register
 * of the Pi is little-endian (hvslist.c's hvs_rd, hvs_wr). */
#include "libz.h"
#include "../../kernel/hwinfo.h"
#include "../server/hvslist.h"
typedef unsigned char u8;

static const char USAGE[] =
    "usage: pidisplay -l|--list\n"
    "       pidisplay -d|--dump [WORD]\n"
    "       pidisplay test WIDTHxHEIGHTxBITS\n"
    "       pidisplay console\n";

static struct hvspi pi;

static int find(void)
{
    if (prot_full()) {
        dprintf(2, "pidisplay: the Pi's video: not built for full memory protection (no Emu68 has a 68k MMU)\n");
        return 1;
    }
    const struct hwinfo *h = (const struct hwinfo *)HWINFO_ADDR;
    if (h->magic == HWINFO_MAGIC && h->fb.magic == HWFB_MAGIC &&
        (h->fb.flags & (HWFB_TEST | HWFB_PI)) == (HWFB_TEST | HWFB_PI)) {
        memset(&pi, 0, sizeof pi);      /* a test's pretend Pi (dpytest -pi): no timer, no EDID; its HVS window and
                                         * picture memory only while a display server runs on it */
        pi.hvs = h->fb.hvs;
        pi.vc4 = h->fb.base;
        pi.vc4size = h->fb.dl_rtg;
        pi.ow = h->fb.width;
        pi.oh = h->fb.height;
        return 0;
    }
    const char *no = hvs_find(h, (const void *)HWDISP_ADDR, &pi);
    if (no) {
        dprintf(2, "pidisplay: not a Raspberry Pi 4 under Emu68 (the hardware table: %s)\n", no);
        return 1;
    }
    return 0;
}
/* the frame rate: two moves of DISPSTAT1's frame count timed by the 1 MHz system timer (at most 150 ms); 0 unknown */
static u32 rate(void)
{
    if (!pi.soc)
        return 0;
    u32 t0 = sys_ticks(), f = hvs_frame(&pi), us[2];
    for (int k = 0; k < 2; k++) {
        u32 g;
        while ((g = hvs_frame(&pi)) == f)
            if (sys_ticks() - t0 > 15)
                return 0;               /* (150 ms: down to a 24 Hz HDMI) */
        f = g;
        us[k] = hvs_rd(pi.soc + 0x3004);
    }
    u32 d = us[1] - us[0];
    return d ? (1000000u + d / 2) / d : 0;
}
static int list(void)
{
    struct hvsedid e;
    int have = pi.edid && hvs_edid(pi.edid, 128, &e) == 0;
    if (pi.edid && !have)
        dprintf(2, "pidisplay: the monitor's EDID: a bad header or checksum (its sizes not used)\n");
    struct hvsmode m[HVS_MAXMODES];
    int n = hvs_modes(pi.ow, pi.oh, pi.vc4size, have ? &e : 0, m, HVS_MAXMODES);
    dprintf(1, "output %ux%u %u\n", pi.ow, pi.oh, rate());
    if (have && (e.name[0] || e.maker[0]))
        dprintf(1, "monitor %s\n", e.name[0] ? e.name : e.maker);
    if (have && e.pref >= 0 && (e.w[e.pref] != pi.ow || e.h[e.pref] != pi.oh))
        dprintf(1, "preferred %ux%u\n", e.w[e.pref], e.h[e.pref]);  /* (config.txt chose another HDMI mode) */
    dprintf(1, "memory 0x%x %u\n", (u32)pi.vc4, pi.vc4size);
    for (int i = 0; i < n; i++)
        dprintf(1, "mode %ux%u%s%s%s%s%s\n", m[i].w, m[i].h, m[i].bits & HVSB_8 ? " 8" : "", m[i].bits & HVSB_16 ?
                " 16" : "", m[i].bits & HVSB_32 ? " 32" : "", m[i].flags & HVSM_NATIVE ? " native" : "",
                m[i].flags & HVSM_PREF ? " preferred" : "");
    return 0;
}
static int dump(const char *w)
{
    if (!pi.hvs) {
        dprintf(2, "pidisplay: --dump: the pretend Pi has no HVS while no desktop runs on it\n");
        return 1;
    }
    u32 at = hvs_rd(pi.hvs + HVS_DISPLIST1), kept = hvs_con(&pi);
    if (w) {
        char *e;
        at = (u32)strtoul(w, &e, 0);
        if (*e || at >= HVS_DLIST_WORDS) {
            dprintf(2, "pidisplay: %s: not a word of the list memory (0-4095)\n", w);
            return 1;
        }
    }
    dprintf(1, "list 0x%x (the HDMI shows 0x%x; the console's 0x%x)\n", at, hvs_rd(pi.hvs + HVS_DISPLIST1), kept);
    for (u32 i = 0; i < 32 && at + i < HVS_DLIST_WORDS; i++) {
        u32 v = hvs_rd(hvs_dl(&pi, at + i));
        dprintf(1, "%03x %08x\n", at + i, v);
        if (v & HVS_DL_END)
            break;
    }
    return 0;
}
static int root(const char *what)
{
    if (sys_getuid() == 0)
        return 0;
    dprintf(2, "pidisplay: %s: root's\n", what);
    return 1;
}
static int desktop_runs(void)
{
    struct uproc p;
    for (u32 k = 0; k < 256; k++)
        if (sys_procinfo(k, &p) == 0 && p.state && p.state != 5 && !strcmp(p.name, "displayd"))
            return 1;
    return 0;
}
/* a test picture: bands of the eight primary colours, a white frame, for 5 seconds; the register put back */
static int test(const char *spec)
{
    unsigned w, h, bits;
    if (root("test"))
        return 1;
    if (hvs_parse_size(spec, &w, &h, &bits) < 0 || !bits) {
        dprintf(2, "pidisplay: %s: not WIDTHxHEIGHTxBITS (BITS 8, 16 or 32)\n", spec);
        zusage(USAGE);
    }
    if (desktop_runs()) {
        dprintf(2, "pidisplay: test: the desktop runs (its display server owns the HDMI)\n");
        return 1;
    }
    if (!hvs_size_ok(w, h, bits, pi.ow, pi.oh, pi.vc4size)) {
        dprintf(2, "pidisplay: %s: not a picture this HDMI takes (%u x %u to %u x %u, at most %u KB)\n", spec,
                HVS_MINW, HVS_MINH, pi.ow, pi.oh, (pi.vc4size - HVS_VC4_SPARE) >> 10);
        return 1;
    }
    if (!pi.soc) {
        dprintf(2, "pidisplay: test: not on a pretend Pi\n");
        return 1;
    }
    if (!hvs_running(&pi)) {
        dprintf(2, "pidisplay: the HDMI: the HVS's channel 1 is not running\n");
        return 1;
    }
    static const unsigned rgb[8] = { 0x000, 0xF00, 0x0F0, 0x00F, 0xFF0, 0x0FF, 0xF0F, 0xFFF };
    struct hvsplane pl = { w, h, bits, hvs_bpr(w, bits), (u32)pi.vc4, pi.ow, pi.oh };
    u32 was = hvs_rd(pi.hvs + HVS_DISPLIST1);
    if (hvs_ours(was)) {                /* a desktop's list left on the HDMI (its server killed): not ours to restore */
        dprintf(2, "pidisplay: test: the HDMI shows a list of Zenix's desktop (0x%x): pidisplay console first\n", was);
        return 1;
    }
    hvs_con_keep(&pi, was);
    signal(SIGINT, SIG_IGN);            /* the 5 seconds always end with the HDMI as it was */
    signal(SIGTERM, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    for (u32 y = 0; y < h; y++) {
        u8 *row = (u8 *)pi.vc4 + y * pl.bpr;
        for (u32 x = 0; x < w; x++) {
            u32 c = x < 4 || y < 4 || x >= w - 4 || y >= h - 4 ? 7 : x * 8 / w, v = hvs_argb(rgb[c]);
            if (bits == 8)
                row[x] = (u8)c;
            else if (bits == 16) {
                u32 p = (v >> 8 & 0xF800) | (v >> 5 & 0x07E0) | (v >> 3 & 0x1F);
                row[2 * x] = (u8)p, row[2 * x + 1] = (u8)(p >> 8);     /* 5:6:5, the low byte first */
            } else
                row[4 * x] = (u8)v, row[4 * x + 1] = (u8)(v >> 8), row[4 * x + 2] = (u8)(v >> 16), row[4 * x + 3] = 0xFF;
        }
    }
    if (hvs_put(&pi, &pl, HVS_DL_A) < 0) {
        dprintf(2, "pidisplay: the HVS's list memory at 0x%x does not keep what is written\n", (u32)hvs_dl(&pi, 0));
        return 1;
    }
    for (u32 i = 0; bits == 8 && i < 8; i++)
        hvs_wr(hvs_dl(&pi, HVS_DL_PAL + i), hvs_argb(rgb[i]));
    dprintf(1, "pidisplay: %ux%u, %u bits on the HDMI's %ux%u for 5 seconds (list 0x%x; it showed 0x%x)\n", w, h,
            bits, pi.ow, pi.oh, HVS_DL_A, was);
    hvs_wr(pi.hvs + HVS_DISPLIST1, HVS_DL_A);
    sys_sleep(500);
    hvs_wr(pi.hvs + HVS_DISPLIST1, was);
    dprintf(1, "pidisplay: the HDMI shows 0x%x again\n", was);
    return 0;
}
static int console(void)
{
    if (root("console"))
        return 1;
    u32 c = hvs_con(&pi);
    if (!c) {
        dprintf(2, "pidisplay: console: no console's list was kept (no desktop ran on the Pi's picture)\n");
        return 1;
    }
    hvs_wr(pi.hvs + HVS_DISPLIST1, c);
    dprintf(1, "pidisplay: the HDMI shows the console's list 0x%x\n", c);
    return 0;
}

int main(int argc, char **argv)
{
    int c, l = 0, d = 0;
    zopt(argc, argv, USAGE);
    for (int i = 1; i < argc && strcmp(argv[i], "--"); i++)   /* the long names of -l and -d */
        if (!strcmp(argv[i], "--list")) argv[i] = "-l";
        else if (!strcmp(argv[i], "--dump")) argv[i] = "-d";
    while ((c = zoptget(argc, argv, "ld", USAGE)) != -1) {
        if (c == 'l') l = 1;
        else d = 1;
    }
    int n = argc - zoptnext;
    const char *cmd = n ? argv[zoptnext] : 0;
    if (l + d > 1 || (l && n) || (d && n > 1) || (!l && !d && !(n == 2 && !strcmp(cmd, "test")) &&
                                                 !(n == 1 && !strcmp(cmd, "console"))))
        zusage(USAGE);
    if (find())
        return 1;
    return l ? list() : d ? dump(n ? cmd : 0) : !strcmp(cmd, "test") ? test(argv[zoptnext + 1]) : console();
}
