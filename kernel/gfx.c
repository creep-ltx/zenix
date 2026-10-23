/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#include "hw.h"

/* Display: 640 pixels wide hires, ONE bitplane: half the chip-bus DMA of
 * two planes, which matters on a stock A1200 where the CPU shares chip RAM
 * with the display. The whole screen is text (grey on black): 32 rows of 8
 * lines on a PAL display (256 lines), 25 on NTSC (200 lines); gfx_init
 * measures which one the machine shows (the beam's last line), so no row is
 * ever hidden below the picture. Scrolling the text is a pointer bump in the
 * Copper list, applied at vertical blank; only when the window reaches the
 * end of the buffer (twice the screen's height) does the blitter copy the
 * visible rows back to the top (the copy leaves identical pixels, so the
 * switch cannot be seen). An inverted cell is just the glyph bits inverted.
 * Glyphs come from the Kickstart ROM's Topaz 8, found through GfxBase before
 * the OS memory is reused. */

/* first display line; 0x2C is the standard start. Emulator windows
 * crop the top, so screenshot tests build with DIW_V=0x38 (make DIWV=0x38). */
#ifndef DIW_V
#define DIW_V 0x2C
#endif
#define STRIDE  80
#define BUFL    (2 * TROWS_MAX * 8)

int trows = TROWS_MAX;                  /* text rows on this display: 32 (PAL) or 25 (NTSC) */
static int dlines = 256;                /* display lines: 8 * trows */
static u8 *tbuf;                        /* chip RAM */
static volatile u16 *cop;
static int off;                         /* drawing window top (lines) */
static volatile int hw_off = -1;        /* what the Copper list shows */
static u8 glyphs[224][8];               /* Latin-1 32..255 */
static int up;

/* ---- font from the ROM ---- */

#define TF_YSIZE 20
#define TF_XSIZE 24
#define TF_LOCHAR 32
#define TF_HICHAR 33
#define TF_CHARDATA 34
#define TF_MODULO 38
#define TF_CHARLOC 40
#define GB_TEXTFONTS 140
#define GB_DEFAULTFONT 154

static int plausible(u32 a)
{
    return (a >= 0x400 && a < 0x08000000u) || (a >= 0xF00000u && a < 0x1000000u);
}

static int font_extract(u32 tf)
{
    if (!plausible(tf) || (tf & 1))
        return -1;
    u8 *f = (u8 *)tf;
    u16 ysize = *(u16 *)(f + TF_YSIZE), xsize = *(u16 *)(f + TF_XSIZE);
    u8 lo = f[TF_LOCHAR], hi = f[TF_HICHAR];
    u32 data = *(u32 *)(f + TF_CHARDATA), charloc = *(u32 *)(f + TF_CHARLOC);
    u16 modulo = *(u16 *)(f + TF_MODULO);
    if (ysize != 8 || xsize != 8 || lo > 32 || hi < 126 ||
        !plausible(data) || !plausible(charloc) || modulo > 512)
        return -1;
    for (int ch = 32; ch < 256; ch++) {
        if (ch > hi) {
            memcpy(glyphs[ch - 32], glyphs['?' - 32], 8);
            continue;
        }
        const u8 *loc = (const u8 *)(charloc + (u32)(ch - lo) * 4);
        u16 bitoff = (u16)((loc[0] << 8) | loc[1]);
        for (int y = 0; y < 8; y++) {
            const u8 *row = (const u8 *)(data + (u32)y * modulo + (bitoff >> 3));
            u16 pair = (u16)((row[0] << 8) | row[1]);
            glyphs[ch - 32][y] = (u8)(pair >> (8 - (bitoff & 7)));
        }
    }
    return 0;
}

/* call before mem_init: the font structures live in memory the OS left behind */
int gfx_font_init(u32 gfxbase)
{
    if (!plausible(gfxbase))
        return -1;
    u32 node = *(u32 *)(gfxbase + GB_TEXTFONTS);
    for (int i = 0; i < 16 && plausible(node); i++) {
        if (*(u32 *)node == 0)
            break;
        if (font_extract(node) == 0)
            return 0;
        node = *(u32 *)node;
    }
    return font_extract(*(u32 *)(gfxbase + GB_DEFAULTFONT));
}

/* ---- blitter ---- */

static void blit_wait(void)
{
    (void)DMACONR;
    while (DMACONR & DMAB_BLTBUSY)
        ;
}

/* copy `words` 16-bit words, chip to chip, no overlap */
static void blit_copy(const void *src, void *dst, u32 words)
{
    blit_wait();
    BLTCON0 = 0x09F0;                   /* use A and D, D = A */
    BLTCON1 = 0;
    BLTAFWM = BLTALWM = 0xFFFF;
    BLTAMOD = BLTDMOD = 0;
    BLTAPTH = (u16)((u32)src >> 16);
    BLTAPTL = (u16)(u32)src;
    BLTDPTH = (u16)((u32)dst >> 16);
    BLTDPTL = (u16)(u32)dst;
    BLTSIZE = (u16)(((words / 40) << 6) | 40);   /* 40 words wide */
}

static void blit_clear(void *dst, u32 words)
{
    blit_wait();
    BLTCON0 = 0x0100;                   /* D only, D = 0 */
    BLTCON1 = 0;
    BLTDMOD = 0;
    BLTDPTH = (u16)((u32)dst >> 16);
    BLTDPTL = (u16)(u32)dst;
    BLTSIZE = (u16)(((words / 40) << 6) | 40);
}

/* ---- drawing ---- */

/* attr bits: 1 inverse, 2 underline, 4 bold. One plane: a set bit is the
 * foreground colour, so inverse is just the glyph bits inverted. */
static void put_glyph(u8 *line0, int col, u8 ch, u8 attr)
{
    const u8 *g = glyphs[(ch < 32 ? '?' : ch) - 32];
    for (int y = 0; y < 8; y++) {
        u8 b = g[y];
        if (attr & 4)
            b |= (u8)(b >> 1);                  /* bold: smear one pixel */
        if ((attr & 2) && y == 7)
            b = 0xFF;                           /* underline */
        line0[y * STRIDE + col] = (attr & 1) ? (u8)~b : b;
    }
}

void gfx_cell(int row, int col, u8 ch, u8 attr)
{
    if (up)
        put_glyph(tbuf + (u32)(off + row * 8) * STRIDE, col, ch, attr);
}

/* Copper list layout (words): 0-3 the two colours, 4-7 the bitplane pointer (5 and 7 are the scroll words), 8-9 end */
#define CTXT 4

static void cop_point(int idx, const u8 *p0)
{
    cop[idx + 1] = (u16)((u32)p0 >> 16);
    cop[idx + 3] = (u16)(u32)p0;
}

/* vertical-blank interrupt: apply a scroll */
void gfx_vblank(void)
{
    int o = off;
    if (up && o != hw_off) {
        cop_point(CTXT, tbuf + (u32)o * STRIDE);
        hw_off = o;
    }
}

void gfx_scroll(void)
{
    if (!up)
        return;
    if (off + 8 + dlines <= BUFL) {
        off += 8;
        blit_clear(tbuf + (u32)(off + (trows - 1) * 8) * STRIDE, 8 * STRIDE / 2);
    } else {                            /* end of buffer: copy rows 1.. back to the top */
        blit_copy(tbuf + (u32)(off + 8) * STRIDE, tbuf,
                  (u32)(trows - 1) * 8 * STRIDE / 2);
        blit_clear(tbuf + (u32)((trows - 1) * 8) * STRIDE, 8 * STRIDE / 2);
        off = 0;
    }
    blit_wait();
}

void gfx_clear_text(void)
{
    if (!up)
        return;
    off = 0;
    blit_clear(tbuf, (u32)BUFL * STRIDE / 2);
    blit_wait();
}

/* The beam's last line over three frames (about 60 ms, timed with the E-clock counter, which main.c starts before
 * this): 312 on a PAL display, 262 on NTSC. It follows what the chips show (BEAMCON0 as Kickstart left it), so an
 * ECS/AGA machine switched to the other standard counts as what it shows. 0 if the beam did not move. */
static int frame_lines(void)
{
    int max = 0;
    u32 t0 = hr_time(), n = 0;
    while ((u32)(hr_time() - t0) < 42000u && ++n < 2000000u) {
        int v = (int)((VPOSL >> 8) & 0x1FF);
        if (v > max)
            max = v;
    }
    return max;
}

int gfx_init(void)
{
    int fl = frame_lines();
    if (fl && fl < 280) {               /* NTSC: 200 lines */
        trows = 25;
        dlines = 200;
    }
    kprintf("display: beam's last line %d: %s, %d text rows\n", fl, trows == 25 ? "NTSC" : "PAL", trows);
    tbuf = mem_alloc((u32)BUFL * STRIDE, MEM_CHIP, 0);
    cop = mem_alloc(64, MEM_CHIP, 0);
    if (!tbuf || !cop)
        return -1;
    memset(tbuf, 0, (u32)BUFL * STRIDE);
    cop[0] = 0x0180; cop[1] = 0x000;    /* black background */
    cop[2] = 0x0182; cop[3] = 0xBBB;    /* grey text */
    cop[4] = 0x00E0; cop[6] = 0x00E2;   /* BPL1PTH/L */
    cop_point(CTXT, tbuf);
    cop[8] = 0xFFFF; cop[9] = 0xFFFE;
    off = 0;
    hw_off = 0;

    BPLCON0 = 0x9200;                   /* hires, 1 bitplane, colour burst */
    BPLCON1 = 0;
    BPLCON2 = 0;
    BPL1MOD = 0;
    DDFSTRT = 0x3C;
    DDFSTOP = 0xD4;
    DIWSTRT = (u16)((DIW_V << 8) | 0x81);
    DIWSTOP = (u16)((((DIW_V + dlines) & 0xFF) << 8) | 0xC1);   /* the stop line's bit 8 is implied (not bit 7) */
    COP1LCH = (u16)((u32)cop >> 16);
    COP1LCL = (u16)(u32)cop;
    COPJMP1 = 0;
    DMACON = DMAF_SETCLR | DMAF_MASTER | DMAF_BPLEN | DMAF_COPEN | DMAF_BLTEN;
    INTREQ = INTF_VERTB;
    INTENA = INTF_SETCLR | INTF_VERTB;
    up = 1;
    return 0;
}

int gfx_up(void) { return up; }

/* a kernel panic with the display up (interrupts are off): show the scroll position the text was drawn at (the
 * vertical-blank interrupt that normally applies it no longer runs) and turn the background dark red with white
 * text, so the photo of the screen shows at once that it stopped; the text stays */
void gfx_panic(void)
{
    if (!up)
        return;
    gfx_vblank();
    cop[1] = 0x800;
    cop[3] = 0xFFF;
}
