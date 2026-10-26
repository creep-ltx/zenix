/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#include "hw.h"

/* Display: 640 pixels wide hires, the whole screen text: 32 rows of 8 lines on a PAL display (256 lines), 25 on NTSC
 * (200 lines); gfx_init measures which one the machine shows (the beam's last line), so no row is ever hidden below
 * the picture. How many bitplanes (DESIGN D14) depends on the machine, because the display takes chip-bus time from
 * the CPU, which shares chip RAM on a stock A1200:
 *   - AGA (Lisa's id) with fast RAM or 2 MB of chip RAM: 4 planes, 16 colours, fetched 64 bits at a time (FMODE 3),
 *     which costs the same 40 bus slots per line as one plane at the old 16-bit fetch. Never 4 planes at FMODE 0:
 *     that takes every slot of the visible lines from the CPU.
 *   - OCS/ECS with fast RAM: 3 planes, 8 colours (a bright colour is shown as the colour plus bold).
 *   - otherwise (chip RAM only, less than 2 MB): 1 plane, grey on black, as before D14.
 * make GFXDEPTH=1|3|4 builds a kernel that uses that depth wherever it can (the screenshot tests).
 * The planes are interleaved: the planes of a line lie one after another (80 bytes each), so one base pointer serves
 * all planes and a clear or copy of whole lines is one blit. Scrolling the text is a pointer bump in the Copper list,
 * applied at vertical blank; only when the window reaches the end of the buffer (twice the screen's height) does the
 * blitter copy the visible rows back to the top (the copy leaves identical pixels, so the switch cannot be seen).
 * Region scrolls (CSI S/T/L/M, a scroll region) are one blitter copy too, and erased rows a blitter fill with the
 * background colour. A glyph is drawn by the CPU, one byte per plane per glyph line: (glyph & m1) ^ m2 per plane, m1
 * set where the foreground and background bits differ, m2 where the background bit is set. Glyphs come from the
 * Kickstart ROM's Topaz 8, found through GfxBase before the OS memory is reused. */

/* first display line; 0x2C is the standard start. Emulator windows
 * crop the top, so screenshot tests build with DIW_V=0x38 (make DIWV=0x38). */
#ifndef DIW_V
#define DIW_V 0x2C
#endif
#ifndef GFX_DEPTH
#define GFX_DEPTH 0                     /* 0: chosen by the machine (gfx_init); make GFXDEPTH=n sets 1, 3 or 4 */
#endif
/* hires at FMODE 3 (64-bit fetch): the fetch window and scroll delay that show all 640 pixels, found by the screenshot
 * test (tools/colour-test.sh: all 80 columns visible, the first and the last distinct, nothing repeated); knowledge
 * base amiga/zenix-colour-console.md */
#ifndef F3_DDFSTRT
#define F3_DDFSTRT 0x38
#endif
#ifndef F3_DDFSTOP
#define F3_DDFSTOP 0xC0
#endif
#ifndef F3_BPLCON1
#define F3_BPLCON1 0x0000
#endif
#ifndef F3_FMODE
#define F3_FMODE 3                      /* only the screenshot experiments that find the values above change it */
#endif
#ifndef F3_MODADJ
#define F3_MODADJ 0                     /* bytes added to the modulo (a fetch window wider than 640 pixels: negative) */
#endif
#define DENISEID REG16(0xDFF07C)        /* Lisa (AGA) reads 0xF8 in the low byte; OCS has no such register */
#define COP_WAIT_LINE 0x20              /* the Copper waits for this line before it moves the plane pointers */
#define STRIDE  80                      /* bytes per plane per line */
#define BUFL    (2 * TROWS_MAX * 8)

int trows = TROWS_MAX;                  /* text rows on this display: 32 (PAL) or 25 (NTSC) */
int gfx_depth;                          /* bitplanes: 1, 3 or 4 (0: no display) */
static int dlines = 256;                /* display lines: 8 * trows */
static u32 lstride;                     /* bytes per display line: 80 per plane */
static int aga;
static u8 *tbuf;                        /* chip RAM */
static volatile u16 *cop;
static int cop_col, cop_ptr;            /* where the colours and the plane pointers are in the Copper list (words) */
static int off;                         /* drawing window top (lines) */
static volatile int hw_off = -1;        /* what the Copper list shows */
static u8 glyphs[224][8] __attribute__((aligned(4)));  /* Latin-1 32..255 */
static int up;

/* The 16 ANSI colours, 12 bits (on AGA with LOCT 0 each value fills the low nibbles too). Grey on black stays the
 * default text (colour 7 on 0); blue is lighter than VGA's so that it reads on black in Topaz. */
const u16 gfx_palette[16] = {
    0x000, 0xC22, 0x2B2, 0xBB2, 0x45E, 0xB3B, 0x2BB, 0xBBB,
    0x666, 0xF66, 0x6F6, 0xFF6, 0x88F, 0xF7F, 0x6FF, 0xFFF,
};

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

/* A pointer left by AmigaOS is followed only if it lies in RAM that exec listed (the bootinfo regions) or in the
 * Kickstart ROM. Accelerators and Emu68 put fast RAM - and with it GfxBase and the font lists - far above 16 MB
 * (Emu68: 0x08000000..., 0x41000000...), so a fixed "below 128 MB" window lost the font on a PiStorm (first boot
 * on the owner's A1200, 2026-10-23: "display: not available (no ROM font)"). */
static int plausible(u32 a)
{
    const struct bootinfo *bi = (const struct bootinfo *)0x1000;
    if (a >= 0xF00000u && a < 0x1000000u)           /* Kickstart ROM */
        return 1;
    if (a >= 0xE00000u && a < 0xE80000u)            /* extended ROM area */
        return 1;
    for (int i = 0; i < bi->nregions && i < 8; i++) {
        u32 lo = bi->r[i].lower & ~0x3Fu, hi = bi->r[i].upper;  /* lower is just past the MemHeader */
        if (lo < 0x400)
            lo = 0x400;
        if (a >= lo && a + 64 <= hi)
            return 1;
    }
    return 0;
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

/* Every wait on hardware has a timeout (CLAUDE.md): the longest blit (the copy-back, under 1024 rows of 80 bytes)
 * takes a few tens of ms; 500 ms (in E-clock ticks) means the blitter is stuck. Said once on the serial port and
 * the log, then the drawing goes on (it may be wrong; the machine keeps running). */
#define BLIT_TIMEOUT (ECLOCK_PAL / 2)
static int blit_stuck;

static void blit_wait(void)
{
    (void)DMACONR;
    if (!(DMACONR & DMAB_BLTBUSY))
        return;
    u32 t0 = hr_time(), n = 0;
    while (DMACONR & DMAB_BLTBUSY)
        if ((++n & 63) == 0 && (u32)(hr_time() - t0) > BLIT_TIMEOUT) {
            if (!blit_stuck++)
                kprintf("gfx: the blitter did not finish in 500 ms (DMACONR %x): drawing goes on without waiting\n",
                        (u32)DMACONR);
            return;
        }
}

static void blit_ptr(volatile u16 *hi, const void *p)
{
    hi[0] = (u16)((u32)p >> 16);
    hi[1] = (u16)(u32)p;
}

/* copy `rows` rows of 80 bytes, chip to chip; overlapping is fine (downwards it runs descending). A blit is at most
 * 1024 rows high (BLTSIZE's 10 bits, 0 meaning 1024). */
static void blit_copy(const u8 *src, u8 *dst, u32 rows)
{
    int desc = dst > src;
    while (rows) {
        u32 h = rows > 1024 ? 1024 : rows;
        blit_wait();
        BLTCON0 = 0x09F0;                       /* use A and D, D = A */
        BLTCON1 = desc ? 2 : 0;                 /* DESC: from the last word back */
        BLTAFWM = BLTALWM = 0xFFFF;
        BLTAMOD = BLTDMOD = 0;
        if (desc) {
            blit_ptr(&BLTAPTH, src + rows * STRIDE - 2);
            blit_ptr(&BLTDPTH, dst + rows * STRIDE - 2);
        } else {
            blit_ptr(&BLTAPTH, src);
            blit_ptr(&BLTDPTH, dst);
            src += h * STRIDE;
            dst += h * STRIDE;
        }
        BLTSIZE = (u16)((h << 6) | (STRIDE / 2));
        rows -= h;
    }
}

/* `lines` display lines from dst on: each plane filled with its bit of colour bg (all ones or all zeros) */
static void blit_fill(u8 *dst, u32 lines, u32 bg)
{
    for (int k = 0; k < gfx_depth; k++) {
        u8 *d = dst + k * STRIDE;
        for (u32 left = lines; left;) {
            u32 h = left > 1024 ? 1024 : left;
            blit_wait();
            BLTCON0 = (bg >> k & 1) ? 0x01FF : 0x0100;     /* D only, D = 1 or D = 0 */
            BLTCON1 = 0;
            BLTDMOD = (u16)(lstride - STRIDE);
            blit_ptr(&BLTDPTH, d);
            BLTSIZE = (u16)((h << 6) | (STRIDE / 2));
            d += h * lstride;
            left -= h;
        }
    }
}

/* ---- drawing ---- */

/* bit k of a colour index -> plane k's byte of a long (plane 0 in the top byte) */
static const u32 expand[16] = {
    0x00000000, 0xFF000000, 0x00FF0000, 0xFFFF0000, 0x0000FF00, 0xFF00FF00, 0x00FFFF00, 0xFFFFFF00,
    0x000000FF, 0xFF0000FF, 0x00FF00FF, 0xFFFF00FF, 0x0000FFFF, 0xFF00FFFF, 0x00FFFFFF, 0xFFFFFFFF,
};

/* a background colour as this depth shows it */
static u32 bg_of(u8 colour)
{
    return gfx_depth == 1 ? 0 : gfx_depth == 3 ? (colour >> 4) & 7u : colour >> 4;
}

/* attr bits: 1 inverse, 2 underline, 4 bold; colour: foreground in the low nibble, background in the high one. One
 * plane: the colours are not shown (grey on black, inverse is the glyph inverted, as before D14). Three planes: a
 * bright foreground is the colour plus bold (bright black, 8, is grey: black on black would hide it). If foreground
 * and background are the same, inverse (the cursor) uses the default colours so that it stays visible. With more
 * than one plane each glyph line is kept four times in a long (rglyph: the byte in every plane's lane), so a line is
 * one read, an AND, an XOR and a byte store per plane; one loop per depth keeps the masks in registers (the stack is
 * chip RAM on a stock A1200). Bold and underline, rarer, go through a copy of the glyph first. */
#define HOT __attribute__((optimize("O2")))     /* gfx.c is built -Os (cold set-up code); drawing is not */
static u32 *rglyph;                     /* [224][8], with more than one plane */

static HOT void draw1(u8 *p, const u8 *g, u32 attr, u32 x)
{
    for (int y = 0; y < 8; y++) {
        u32 b = g[y];
        if (attr & 4)
            b |= b >> 1;                        /* bold: smear one pixel */
        if ((attr & 2) && y == 7)
            b = 0xFF;                           /* underline */
        p[y * STRIDE] = (u8)(b ^ x);
    }
}

static HOT void draw4(u8 *p, const u32 *g, u32 m1, u32 m2)
{
    for (int y = 0; y < 8; y++) {
        u32 v = (g[y] & m1) ^ m2;
        p[3 * STRIDE] = (u8)v;
        v >>= 8;
        p[2 * STRIDE] = (u8)v;
        v >>= 8;
        p[STRIDE] = (u8)v;
        p[0] = (u8)(v >> 8);
        p += 4 * STRIDE;
    }
}

static HOT void draw3(u8 *p, const u32 *g, u32 m1, u32 m2)
{
    for (int y = 0; y < 8; y++) {
        u32 v = ((g[y] & m1) ^ m2) >> 8;
        p[2 * STRIDE] = (u8)v;
        v >>= 8;
        p[STRIDE] = (u8)v;
        p[0] = (u8)(v >> 8);
        p += 3 * STRIDE;
    }
}

static HOT void put_glyph(u8 *p, u8 ch, u32 attr, u32 colour)
{
    int gi = (ch < 32 ? '?' : ch) - 32;
    if (gfx_depth == 1) {
        draw1(p, glyphs[gi], attr, (attr & 1) ? 0xFF : 0);
        return;
    }
    u32 fg = colour & 15, bg = colour >> 4;
    if (gfx_depth == 3) {
        if (fg >= 8) {
            fg = fg == 8 ? 7 : fg & 7;
            attr |= 4;
        }
        bg &= 7;
    }
    if (attr & 1) {
        u32 t = fg;
        if (fg == bg)
            t = 7, bg = 0;
        fg = bg;
        bg = t;
    }
    const u32 *g = rglyph + gi * 8;
    u32 tmp[8];
    if (attr & 6) {
        for (int y = 0; y < 8; y++) {
            u32 v = g[y];
            if (attr & 4)
                v |= (v >> 1) & 0x7F7F7F7Fu;
            tmp[y] = v;
        }
        if (attr & 2)
            tmp[7] = ~0u;
        g = tmp;
    }
    if (gfx_depth == 4)
        draw4(p, g, expand[fg ^ bg], expand[bg]);
    else
        draw3(p, g, expand[fg ^ bg], expand[bg]);
}

static u8 *line_at(int l) { return tbuf + (u32)l * lstride; }

HOT void gfx_cell(int row, int col, u8 ch, u8 attr, u8 colour)
{
    if (up)
        put_glyph(line_at(off + row * 8) + col, ch, attr, colour);
}

/* rows [row, row + n) of the screen take the background colour of `colour` */
void gfx_fill_rows(int row, int n, u8 colour)
{
    if (!up || n <= 0)
        return;
    blit_fill(line_at(off + row * 8), (u32)n * 8, bg_of(colour));
    blit_wait();
}

/* n text rows from row src to row dst (they may overlap), by the blitter */
void gfx_move_rows(int dst, int src, int n)
{
    if (!up || n <= 0 || dst == src)
        return;
    blit_copy(line_at(off + src * 8), line_at(off + dst * 8), (u32)n * 8 * (u32)gfx_depth);
    blit_wait();
}

/* point the Copper list's plane pointers at display line o of the buffer */
static void cop_point(int o)
{
    const u8 *p = line_at(o);
    for (int k = 0; k < gfx_depth; k++, p += STRIDE) {
        cop[cop_ptr + 4 * k + 1] = (u16)((u32)p >> 16);
        cop[cop_ptr + 4 * k + 3] = (u16)(u32)p;
    }
}

/* vertical-blank interrupt: apply a scroll. The Copper moves the pointers after its WAIT for line COP_WAIT_LINE, so
 * they are written only while the beam is well before it (else at the next vertical blank): the planes, and the high
 * and low words of each pointer, always move together. */
void gfx_vblank(void)
{
    int o = off;
    if (up && o != hw_off && (int)((VPOSL >> 8) & 0x1FF) < COP_WAIT_LINE - 4) {
        cop_point(o);
        hw_off = o;
    }
}

void gfx_scroll(u8 colour)
{
    if (!up)
        return;
    if (off + 8 + dlines <= BUFL) {
        off += 8;
    } else {                            /* end of buffer: copy rows 1.. back to the top */
        blit_copy(line_at(off + 8), tbuf, (u32)(trows - 1) * 8 * (u32)gfx_depth);
        off = 0;
    }
    blit_fill(line_at(off + (trows - 1) * 8), 8, bg_of(colour));
    blit_wait();
}

void gfx_clear_text(u8 colour)
{
    if (!up)
        return;
    off = 0;
    blit_fill(tbuf, (u32)dlines, bg_of(colour));
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

/* AGA: Lisa's id register reads 0xF8 in its low byte (ECS Denise 0xFC); OCS has no such register and the bus floats,
 * so every one of 16 reads must agree */
static int lisa(void)
{
    u16 v = DENISEID & 0xFF;
    for (int i = 0; i < 16; i++)
        if ((DENISEID & 0xFF) != v)
            return 0;
    return v == 0xF8;
}

/* the depth for this machine (D14's default): AGA with fast RAM or 2 MB of chip RAM: 4; OCS/ECS with fast RAM: 3;
 * otherwise 1. GFX_DEPTH (make GFXDEPTH=n) asks for one: 4 only on AGA (else 3), 3 anywhere. */
static int pick_depth(void)
{
    const struct bootinfo *bi = (const struct bootinfo *)0x1000;
    int fast = 0, chip2m = 0;
    for (int i = 0; i < bi->nregions && i < 8; i++) {
        if (bi->r[i].attrs & MEMF_CHIP) {
            if (bi->r[i].upper >= 0x200000u)
                chip2m = 1;
        } else if (bi->r[i].attrs & MEMF_FAST)
            fast = 1;
    }
    if (GFX_DEPTH == 1 || GFX_DEPTH == 3)
        return GFX_DEPTH;
    if (GFX_DEPTH == 4)
        return aga ? 4 : 3;
    if (aga)
        return fast || chip2m ? 4 : 1;
    return fast ? 3 : 1;
}

int gfx_init(void)
{
    int fl = frame_lines();
    if (fl && fl < 280) {               /* NTSC: 200 lines */
        trows = 25;
        dlines = 200;
    }
    kprintf("display: beam's last line %d: %s, %d text rows\n", fl, trows == 25 ? "NTSC" : "PAL", trows);
    aga = lisa();
    cop = mem_alloc(160, MEM_CHIP, 0);
    if (!cop)
        return -1;
    for (gfx_depth = pick_depth(); ; gfx_depth = gfx_depth == 4 ? 3 : 1) {      /* too little chip RAM: fewer planes */
        lstride = (u32)STRIDE * (u32)gfx_depth;
        tbuf = mem_alloc((u32)BUFL * lstride, MEM_CHIP, 0);     /* 16-byte aligned: FMODE 3 needs 8 */
        if (tbuf || gfx_depth == 1)
            break;
    }
    if (!tbuf)
        return -1;
    if (gfx_depth > 1) {
        rglyph = kmalloc(sizeof glyphs * 4);
        if (!rglyph) {
            mem_free(tbuf);
            gfx_depth = 1;
            lstride = STRIDE;
            tbuf = mem_alloc((u32)BUFL * lstride, MEM_CHIP, 0);
            if (!tbuf)
                return -1;
        } else
            for (u32 i = 0; i < sizeof glyphs; i++)
                rglyph[i] = ((const u8 *)glyphs)[i] * 0x01010101u;
    }
    int f3 = gfx_depth == 4;            /* 4 planes only ever at FMODE 3 */
    kprintf("display: %s chips, %d bitplane%s%s\n", aga ? "AGA" : "OCS/ECS", gfx_depth, gfx_depth > 1 ? "s" : "",
            f3 ? " at 64-bit fetch (FMODE 3)" : "");
    u16 mod = (u16)(lstride - STRIDE + (f3 ? F3_MODADJ : 0));
    volatile u16 *c = cop;
#define MOVE(r, v) (*c++ = (u16)(r), *c++ = (u16)(v))
    if (aga) {                          /* written every frame: a previous program's values survive a warm reset */
        MOVE(0x1FC, f3 ? F3_FMODE : 0);       /* FMODE: 64-bit bitplane fetch */
        MOVE(0x106, 0x0C00);            /* BPLCON3: colour bank 0, LOCT 0 */
        MOVE(0x10C, 0x0011);            /* BPLCON4: no BPLAM colour-index XOR, sprite palettes as after reset */
    }
    MOVE(0x100, 0x8200 | (gfx_depth << 12));    /* BPLCON0: hires, N planes, colour burst */
    MOVE(0x102, f3 ? F3_BPLCON1 : 0);
    MOVE(0x104, 0);
    MOVE(0x108, mod);
    MOVE(0x10A, mod);
    MOVE(0x092, f3 ? F3_DDFSTRT : 0x3C);
    MOVE(0x094, f3 ? F3_DDFSTOP : 0xD4);
    cop_col = (int)(c - cop);
    for (int i = 0; i < (1 << gfx_depth); i++)
        MOVE(0x180 + 2 * i, gfx_depth == 1 ? (i ? gfx_palette[7] : 0) : gfx_palette[i]);
    *c++ = (u16)(COP_WAIT_LINE << 8 | 1);       /* WAIT for line COP_WAIT_LINE */
    *c++ = 0xFFFE;
    cop_ptr = (int)(c - cop);
    for (int k = 0; k < gfx_depth; k++)
        MOVE(0x0E0 + 4 * k, 0), MOVE(0x0E2 + 4 * k, 0);
    *c++ = 0xFFFF;
    *c++ = 0xFFFE;
#undef MOVE
    off = 0;
    cop_point(0);
    hw_off = 0;
    memset(tbuf, 0, (u32)BUFL * lstride);

    BPLCON0 = 0x0200;                   /* no planes until the Copper sets them */
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
 * vertical-blank interrupt that normally applies it no longer runs; the Copper reads the pointers at the next frame)
 * and turn the background dark red with white text, so the photo of the screen shows at once that it stopped; the
 * text stays */
void gfx_panic(void)
{
    if (!up)
        return;
    cop_point(off);
    cop[cop_col + 1] = 0x800;
    for (int i = 1; i < (1 << gfx_depth); i++)
        cop[cop_col + 2 * i + 1] = 0xFFF;
}
