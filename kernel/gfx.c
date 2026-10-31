/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#include "hw.h"

/* Display: 640 pixels wide hires, the whole screen text: 32 rows of 8 lines on a PAL display (256 lines), 25 on NTSC
 * (200 lines); gfx_init measures which one the machine shows (the beam's last line), so no row is ever hidden below
 * the picture. How many bitplanes (DESIGN D14) depends on the machine, because the display takes chip-bus time from
 * the CPU, which shares chip RAM on a stock A1200, and takes chip RAM (docs/d14-colour-console.md section 11):
 *   - AGA (Lisa's id) with fast RAM: 4 planes, 16 colours, fetched 64 bits at a time (FMODE 3), which costs the same
 *     40 bus slots per line as one plane at the old 16-bit fetch. Never 4 planes at FMODE 0: that takes every slot
 *     of the visible lines from the CPU.
 *   - OCS/ECS with fast RAM, and every machine with 2 MB of chip RAM and no fast RAM: 3 planes, 8 colours (a bright
 *     colour is shown as the colour plus bold); on AGA at FMODE 3 too (30 slots), elsewhere at the 16-bit fetch.
 *   - otherwise (chip RAM only, less than 2 MB): 1 plane, grey on black, as before D14.
 * /etc/console's "colours N" (2, 4, 8 or 16) changes it once the root is up (init, SYS_console: gfx_set_colours).
 * 2 planes, 4 colours: black, red (for red, yellow, magenta), cyan (for green, blue, cyan) and grey, bright as bold.
 * One plane is always fetched 16 bits at a time (as verified on the A1200); 2 to 4 on AGA at FMODE 3.
 * make GFXDEPTH=1|2|3|4 builds a kernel that uses that depth wherever it can (the screenshot tests).
 * The planes are interleaved: the planes of a line lie one after another (80 bytes each), so one base pointer serves
 * all planes and a clear or copy of whole lines is one blit. Scrolling the text is a pointer bump in the Copper list,
 * applied at vertical blank; only when the window reaches the end of the buffer (twice the screen's height) does the
 * blitter copy the visible rows back to the top (the copy leaves identical pixels, so the switch cannot be seen).
 * Region scrolls (CSI S/T/L/M, a scroll region) are one blitter copy too, and erased rows a blitter fill with the
 * background colour. A glyph is drawn by the CPU, one byte per plane per glyph line: (glyph & m1) ^ m2 per plane, m1
 * set where the foreground and background bits differ, m2 where the background bit is set. Glyphs come from the
 * Kickstart ROM's Topaz 8, found through GfxBase before the OS memory is reused.
 * DESIGN D28: a cell above U+00FF (gfx_ucell) takes its glyph from the Unicode font (confont.c: the half-page index,
 * an entry, then Topaz's glyph of a Latin-1 byte for a stand-in, else the font's 8 bytes drawn byte-wise: each
 * plane's byte (b & m) ^ n, with no replicated table and no multiply), or the box built in here when the font has
 * none or there is no font. Outside 8-bit mode the bytes 0x80-0x9F (stray bytes: no text is stored there) are the
 * box: their glyph lines are swapped with Topaz's own when the shown console's mode changes (gfx_show), so the
 * Latin-1 path pays nothing for it. */

/* first display line; 0x2C is the standard start. Emulator windows
 * crop the top, so screenshot tests build with DIW_V=0x38 (make DIWV=0x38). */
#ifndef DIW_V
#define DIW_V 0x2C
#endif
#ifndef GFX_DEPTH
#define GFX_DEPTH 0                     /* 0: chosen by the machine (gfx_init); make GFXDEPTH=n sets 1, 2, 3 or 4 */
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
int gfx_depth;                          /* bitplanes: 1 to 4 (0: no display) */
static int dlines = 256;                /* display lines: 8 * trows */
static u32 lstride;                     /* bytes per display line: 80 per plane */
static int aga;
static u8 *tbuf;                        /* chip RAM */
static volatile u16 *cop;
static int cop_col, cop_ptr;            /* where the colours and the plane pointers are in the Copper list (words) */
static int off;                         /* drawing window top (lines) */
static volatile int hw_off = -1;        /* what the Copper list shows */
static u8 glyphs[224][8] __attribute__((aligned(4)));  /* Latin-1 32..255 */
static u8 c1alt[32][8];                 /* D28: the glyphs of 0x80-0x9F not in glyphs[] now (Topaz's, or the boxes) */
static int c1topaz;                     /* glyphs[] holds Topaz's own 0x80-0x9F (8-bit mode shown) */
/* D28: the box, and the wide box's left and right halves, when the Unicode font has none (or there is none) */
static const u8 boxes[3][8] = {
    { 0x00, 0x7E, 0x42, 0x42, 0x42, 0x42, 0x7E, 0x00 },
    { 0x00, 0x7F, 0x40, 0x40, 0x40, 0x40, 0x7F, 0x00 },
    { 0x00, 0xFE, 0x02, 0x02, 0x02, 0x02, 0xFE, 0x00 },
};
static int up;

/* The 16 ANSI colours, 12 bits (on AGA with LOCT 0 each value fills the low nibbles too). Grey on black stays the
 * default text (colour 7 on 0); blue is lighter than VGA's so that it reads on black in Topaz. */
const u16 gfx_palette[16] = {
    0x000, 0xC22, 0x2B2, 0xBB2, 0x45E, 0xB3B, 0x2BB, 0xBBB,
    0x666, 0xF66, 0x6F6, 0xFF6, 0x88F, 0xF7F, 0x6FF, 0xFFF,
};
/* the CGA/VGA palette in SGR order (DESIGN D20 4.3, CONR_CGAPAL: the palette BBS art is drawn in; brown, not yellow) */
static const u16 cga_palette[16] = {
    0x000, 0xA00, 0x0A0, 0xA50, 0x00A, 0xA0A, 0x0AA, 0xAAA,
    0x555, 0xF55, 0x5F5, 0xFF5, 0x55F, 0xF5F, 0x5FF, 0xFFF,
};
/* 2 planes: what each of the eight ANSI colours becomes (warm ones red, cool ones cyan), and the four colours shown */
static const u8 map2[8] = { 0, 1, 2, 1, 2, 1, 2, 3 };
const u16 gfx_palette2[4] = { 0x000, 0xC22, 0x2BB, 0xBBB };

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
    for (int i = 0; i < 32; i++) {      /* D28: outside 8-bit mode 0x80-0x9F draw the box; Topaz's are kept aside */
        memcpy(c1alt[i], glyphs[0x80 - 32 + i], 8);
        memcpy(glyphs[0x80 - 32 + i], boxes[0], 8);
    }
    c1topaz = 0;
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
    return gfx_depth == 1 ? 0 : gfx_depth == 2 ? map2[(colour >> 4) & 7u] : gfx_depth == 3 ? (colour >> 4) & 7u : colour >> 4;
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

/* A console's loaded 8-bit font (DESIGN D20 4.2: CON_FONT, each byte one of its 256 glyphs, indexed by the byte), with
 * its own four-bytes table while there is more than one plane. Only the shown console is drawn, so one pointer says
 * whose glyphs put_glyph takes (0: the ROM's Topaz, indexed byte - 32, a byte below 32 drawn as '?'). Every font is
 * on a list, so a depth change builds or frees their tables with Topaz's. */
struct gfont {
    struct gfont *next;
    u32 *r;                             /* [256][8], with more than one plane */
    u8 g[256][8];
};
static struct gfont *fonts;
static const struct gfont *sfont;
static int scga;                        /* the shown console asked for the CGA palette (CONR_CGAPAL) */
static volatile int pal_dirty;          /* the Copper list's colours are to be written at the next vertical blank */
static u32 dgen;                        /* CON_LINE's generation: raised when the depth, planes or palette change */

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

static HOT void draw2(u8 *p, const u32 *g, u32 m1, u32 m2)
{
    for (int y = 0; y < 8; y++) {
        u32 v = ((g[y] & m1) ^ m2) >> 16;
        p[STRIDE] = (u8)v;
        p[0] = (u8)(v >> 8);
        p += 2 * STRIDE;
    }
}

/* the colours a cell is drawn in at this depth (more than one plane): *fg, *bg; bold added for a bright colour at 2
 * or 3 planes, and inverse applied -> the attributes then */
static inline __attribute__((always_inline)) HOT u32 pens(u32 attr, u32 colour, u32 *fgp, u32 *bgp)
{
    u32 fg = colour & 15, bg = colour >> 4;
    if (gfx_depth < 4) {
        if (fg >= 8) {
            fg = fg == 8 ? 7 : fg & 7;
            attr |= 4;
        }
        bg &= 7;
        if (gfx_depth == 2)
            fg = map2[fg], bg = map2[bg];
    }
    if (attr & 1) {
        u32 t = fg;
        if (fg == bg)
            t = gfx_depth == 2 ? 3 : 7, bg = 0;
        fg = bg;
        bg = t;
    }
    *fgp = fg;
    *bgp = bg;
    return attr;
}

static inline __attribute__((always_inline)) HOT void put_glyph(u8 *p, u8 ch, u32 attr, u32 colour)
{
    const struct gfont *f = sfont;
    int gi = f ? ch : (ch < 32 ? '?' : ch) - 32;
    if (gfx_depth == 1) {
        draw1(p, f ? f->g[gi] : glyphs[gi], attr, (attr & 1) ? 0xFF : 0);
        return;
    }
    u32 fg, bg;
    attr = pens(attr, colour, &fg, &bg);
    const u32 *g = (f ? f->r : rglyph) + gi * 8;
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
    else if (gfx_depth == 3)
        draw3(p, g, expand[fg ^ bg], expand[bg]);
    else
        draw2(p, g, expand[fg ^ bg], expand[bg]);
}

static u8 *line_at(int l) { return tbuf + (u32)l * lstride; }
/* gfx_cell's addresses without a multiply (the 68020's muls.l costs up to 44 cycles, and gcc turns shift-and-add back
 * into one): `top` is the drawing window's first line, `rowoff` each text row's byte offset from it (DESIGN D28 15) */
static u8 *top;
static u32 rowoff[TROWS_MAX];
static void set_off(int o)              /* the drawing window moves (off), or tbuf/lstride changed */
{
    off = o;
    top = line_at(o);
}
static void set_rows(void)              /* lstride changed: each text row is 8 lines */
{
    u32 a = 0;
    for (int r = 0; r < TROWS_MAX; r++, a += 8 * lstride)
        rowoff[r] = a;
}

HOT __attribute__((noipa)) void gfx_cell(int row, int col, u8 ch, u8 attr, u8 colour)
{
    if (up)
        put_glyph(top + rowoff[row] + col, ch, attr, colour);
}

/* D28: 8 glyph bytes drawn byte-wise at any depth: each plane's byte is (b & m) ^ n, m set where the foreground and
 * background bits differ, n where the background bit is set (no replicated table: gcc turns every C replication into
 * a 44-cycle muls.l, DESIGN D28 8). Bold and underline, rarer, go through a copy first; one loop per depth keeps the
 * masks in registers. */
static HOT void put_bytes(u8 *p, const u8 *g, u32 attr, u32 colour)
{
    u8 tmp[8];
    if (attr & 6) {
        for (int y = 0; y < 8; y++) {
            u32 b = g[y];
            if (attr & 4)
                b |= b >> 1;                    /* bold: smear one pixel */
            tmp[y] = (u8)b;
        }
        if (attr & 2)
            tmp[7] = 0xFF;                      /* underline */
        g = tmp;
    }
    if (gfx_depth == 1) {
        u8 x = (attr & 1) ? 0xFF : 0;
        for (int y = 0; y < 8; y++, p += STRIDE)
            *p = (u8)(g[y] ^ x);
        return;
    }
    u32 fg, bg;
    pens(attr, colour, &fg, &bg);
    u8 m0 = (fg ^ bg) & 1 ? 0xFF : 0, n0 = bg & 1 ? 0xFF : 0;
    u8 m1 = (fg ^ bg) & 2 ? 0xFF : 0, n1 = bg & 2 ? 0xFF : 0;
    u8 m2 = (fg ^ bg) & 4 ? 0xFF : 0, n2 = bg & 4 ? 0xFF : 0;
    u8 m3 = (fg ^ bg) & 8 ? 0xFF : 0, n3 = bg & 8 ? 0xFF : 0;
    /* opaque to gcc: knowing a mask is 0 or 0xFF it would turn b & m into a muls.w (D28 8's trap) */
    __asm__("" : "+d"(m0), "+d"(n0), "+d"(m1), "+d"(n1));
    __asm__("" : "+d"(m2), "+d"(n2), "+d"(m3), "+d"(n3));
    const u8 *e = g + 8;
    if (gfx_depth == 3)
        for (; g < e; g++, p += 3 * STRIDE) {
            u8 b = *g;
            p[0] = (b & m0) ^ n0;
            p[STRIDE] = (b & m1) ^ n1;
            p[2 * STRIDE] = (b & m2) ^ n2;
        }
    else if (gfx_depth == 4)
        for (; g < e; g++, p += 4 * STRIDE) {
            u8 b = *g;
            p[0] = (b & m0) ^ n0;
            p[STRIDE] = (b & m1) ^ n1;
            p[2 * STRIDE] = (b & m2) ^ n2;
            p[3 * STRIDE] = (b & m3) ^ n3;
        }
    else
        for (; g < e; g++, p += 2 * STRIDE) {
            u8 b = *g;
            p[0] = (b & m0) ^ n0;
            p[STRIDE] = (b & m1) ^ n1;
        }
}

/* D28: a cell above U+00FF: part 0 one cell, 1 the left half of a wide character, 2 its right half (v the lead's).
 * The font's entry: 0 none (the box; a wide character's halves of the wide box), 32-255 Topaz's glyph of that byte
 * (a stand-in, drawn exactly as Topaz), 256 + n its glyph n. A lead with a glyph has a blank right half. */
HOT void gfx_ucell(int row, int col, u16 v, u8 attr, u8 colour, int part)
{
    if (!up)
        return;
    u8 *p = top + rowoff[row] + col;
    u32 e = 0;                          /* (Topaz's glyphs through gfx_cell: put_glyph stays inlined in that hot path) */
    if (v < 0xFFFE && cf_index) {
        u32 h = cf_index[v >> 7];
        if (h)
            e = cf_pages[(h - 1) * 128 + (v & 127)] & 0x1FFF;
    }
    if (part == 2) {
        if (e)
            gfx_cell(row, col, ' ', attr, colour);
        else
            put_bytes(p, cf_index ? cf_glyphs + 16 : boxes[2], attr, colour);
        return;
    }
    if (!e)
        put_bytes(p, cf_index ? cf_glyphs + 8 * (u32)part : boxes[part], attr, colour);
    else if (e < 256)
        gfx_cell(row, col, (u8)e, attr, colour);
    else
        put_bytes(p, cf_glyphs + 8 * (e - 256), attr, colour);
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
/* colour i of the Copper list at `depth` planes: one plane grey on black, two the four colours, more the shown
 * console's palette (Zenix's, or CGA's for a console with CONR_CGAPAL) */
static u16 colour_of(int i, int depth)
{
    return depth == 1 ? (i ? gfx_palette[7] : 0) : depth == 2 ? gfx_palette2[i] : (scga ? cga_palette : gfx_palette)[i];
}

void gfx_vblank(void)
{
    int o = off;
    if (up && (o != hw_off || pal_dirty) && (int)((VPOSL >> 8) & 0x1FF) < COP_WAIT_LINE - 4) {
        cop_point(o);
        hw_off = o;
        if (pal_dirty) {
            pal_dirty = 0;
            for (int i = 0; i < (1 << gfx_depth); i++)
                cop[cop_col + 2 * i + 1] = colour_of(i, gfx_depth);
        }
    }
}

void gfx_scroll(u8 colour)
{
    if (!up)
        return;
    if (off + 8 + dlines <= BUFL) {
        set_off(off + 8);
    } else {                            /* end of buffer: copy rows 1.. back to the top */
        blit_copy(line_at(off + 8), tbuf, (u32)(trows - 1) * 8 * (u32)gfx_depth);
        set_off(0);
    }
    blit_fill(line_at(off + (trows - 1) * 8), 8, bg_of(colour));
    blit_wait();
}

void gfx_clear_text(u8 colour)
{
    if (!up)
        return;
    set_off(0);
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

/* the depth for this machine (D14's default, section 11): AGA with fast RAM: 4; fast RAM on OCS/ECS, or 2 MB of chip
 * RAM and no fast RAM: 3; otherwise 1. GFX_DEPTH (make GFXDEPTH=n) asks for one: 4 only on AGA (else 3). */
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
    if (GFX_DEPTH >= 1 && GFX_DEPTH <= 3)
        return GFX_DEPTH;
    if (GFX_DEPTH == 4)
        return aga ? 4 : 3;
    if (fast)
        return aga ? 4 : 3;
    return chip2m ? 3 : 1;
}

/* the Copper list for `depth` planes into l (COP_BYTES of chip RAM); the indices of its colours and pointers */
#define COP_BYTES 160
static void build_cop(volatile u16 *l, int depth, int *col, int *ptr)
{
    int f3 = aga && depth > 1;          /* more than one plane on AGA: always at FMODE 3 */
    u16 mod = (u16)((u32)STRIDE * (u32)depth - STRIDE + (f3 ? F3_MODADJ : 0));
    volatile u16 *c = l;
#define MOVE(r, v) (*c++ = (u16)(r), *c++ = (u16)(v))
    if (aga) {                          /* written every frame: a previous program's values survive a warm reset */
        MOVE(0x1FC, f3 ? F3_FMODE : 0);       /* FMODE: 64-bit bitplane fetch */
        MOVE(0x106, 0x0C00);            /* BPLCON3: colour bank 0, LOCT 0 */
        MOVE(0x10C, 0x0011);            /* BPLCON4: no BPLAM colour-index XOR, sprite palettes as after reset */
    }
    MOVE(0x100, 0x8200 | (depth << 12));        /* BPLCON0: hires, N planes, colour burst */
    MOVE(0x102, f3 ? F3_BPLCON1 : 0);
    MOVE(0x104, 0);
    MOVE(0x108, mod);
    MOVE(0x10A, mod);
    MOVE(0x092, f3 ? F3_DDFSTRT : 0x3C);
    MOVE(0x094, f3 ? F3_DDFSTOP : 0xD4);
    *col = (int)(c - l);
    for (int i = 0; i < (1 << depth); i++)
        MOVE(0x180 + 2 * i, colour_of(i, depth));
    *c++ = (u16)(COP_WAIT_LINE << 8 | 1);       /* WAIT for line COP_WAIT_LINE */
    *c++ = 0xFFFE;
    *ptr = (int)(c - l);
    for (int k = 0; k < depth; k++)
        MOVE(0x0E0 + 4 * k, 0), MOVE(0x0E2 + 4 * k, 0);
    *c++ = 0xFFFF;
    *c++ = 0xFFFE;
#undef MOVE
}

/* glyph lines kept four times in a long, for more than one plane (Topaz's 7 KB, a loaded font's 8 KB): 0 when out
 * of memory */
static u32 *make_r(const u8 *g, u32 n)
{
    u32 *r = kmalloc(n * 4);
    if (r)
        for (u32 i = 0; i < n; i++)
            r[i] = g[i] * 0x01010101u;
    return r;
}
static u32 *make_rglyph(void) { return make_r(glyphs[0], sizeof glyphs); }

/* every loaded font's table freed (one plane draws from the plain glyphs) */
static void fonts_flat(void)
{
    for (struct gfont *f = fonts; f; f = f->next) {
        kfree(f->r);
        f->r = 0;
    }
}

struct gfont *gfx_font_new(const u8 *g)
{
    struct gfont *f = kmalloc(sizeof *f);
    if (!f)
        return 0;
    memcpy(f->g, g, sizeof f->g);
    f->r = 0;
    if (gfx_depth > 1 && !(f->r = make_r(f->g[0], sizeof f->g))) {
        kfree(f);
        return 0;
    }
    f->next = fonts;
    fonts = f;
    return f;
}

void gfx_font_free(struct gfont *f)
{
    if (!f)
        return;
    if (sfont == f)
        sfont = 0;
    for (struct gfont **pp = &fonts; *pp; pp = &(*pp)->next)
        if (*pp == f) {
            *pp = f->next;
            break;
        }
    kfree(f->r);
    kfree(f);
}

/* D28: Topaz's own glyphs of 0x80-0x9F (8-bit mode) or the boxes, swapped into glyphs[] and Topaz's table */
static void c1_swap(void)
{
    for (int i = 0; i < 32; i++) {
        u8 *a = glyphs[0x80 - 32 + i], *b = c1alt[i];
        for (int y = 0; y < 8; y++) {
            u8 t = a[y];
            a[y] = b[y];
            b[y] = t;
            if (rglyph)
                rglyph[(0x80 - 32 + i) * 8 + y] = a[y] * 0x01010101u;
        }
    }
    c1topaz = !c1topaz;
}

/* the console shown changed, or its font or rules did: its glyphs from now on, its palette at the next vertical
 * blank; eight: its 8-bit mode (D28), where Topaz draws 0x80-0x9F as its own glyphs */
void gfx_show(const struct gfont *f, int cga, int eight)
{
    if ((eight != 0) != c1topaz)
        c1_swap();
    sfont = f;
    cga = cga != 0;
    if (cga != scga) {
        scga = cga;
        pal_dirty = 1;
        dgen++;
    }
}

static void say_depth(void)
{
    kprintf("display: %s chips, %d bitplane%s%s\n", aga ? "AGA" : "OCS/ECS", gfx_depth, gfx_depth > 1 ? "s" : "",
            aga && gfx_depth > 1 ? " at 64-bit fetch (FMODE 3)" : "");
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
    cop = mem_alloc(COP_BYTES, MEM_CHIP, 0);
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
    if (gfx_depth > 1 && !(rglyph = make_rglyph())) {
        mem_free(tbuf);
        gfx_depth = 1;
        lstride = STRIDE;
        tbuf = mem_alloc((u32)BUFL * lstride, MEM_CHIP, 0);
        if (!tbuf)
            return -1;
    }
    say_depth();
    build_cop(cop, gfx_depth, &cop_col, &cop_ptr);
    set_rows();
    set_off(0);
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

/* DESIGN D29 9.2: what ZINE's way back writes into the display's write-only registers: the Copper list (returned)
 * and DIWSTRT, DIWSTOP as gfx_init wrote them */
u32 gfx_zine(u16 *diw)
{
    diw[0] = (u16)((DIW_V << 8) | 0x81);
    diw[1] = (u16)((((DIW_V + dlines) & 0xFF) << 8) | 0xC1);
    return (u32)cop;
}

/* /etc/console's setting (init, SYS_console; D14 section 11): show `colours` (2, 4, 8, 16) from now on. The new
 * buffer and Copper list come first (no memory: -ENOMEM, the display stays as it is); the switch is one register
 * write with interrupts masked, which the Copper takes at the next vertical blank; one frame later (a timeout of
 * 100 ms: the beam may not move) the old ones are freed and the console shown is drawn again from its grid. Returns
 * the colours shown now. 16 on OCS/ECS (no 64-bit fetch, never 4 planes at the 16-bit one) gives 8. */
int gfx_set_colours(int colours)
{
    int d = colours == 2 ? 1 : colours == 4 ? 2 : colours == 8 ? 3 : colours == 16 ? 4 : 0;
    if (!d)
        return colours ? -EINVAL : 1 << gfx_depth;
    if (!up)
        return -ENODEV;
    if (d == 4 && !aga)
        d = 3;
    if (d == gfx_depth)
        return 1 << d;
    u32 *rg = rglyph;
    u32 ls = (u32)STRIDE * (u32)d;
    volatile u16 *nc = mem_alloc(COP_BYTES, MEM_CHIP, 0);
    u8 *nb = mem_alloc((u32)BUFL * ls, MEM_CHIP, 0);
    if (d > 1 && !rg)
        rg = make_rglyph();
    int fok = 1;                        /* a loaded font's table too (D20), built before anything changes */
    for (struct gfont *f = fonts; d > 1 && f && fok; f = f->next)
        if (!f->r && !(f->r = make_r(f->g[0], sizeof f->g)))
            fok = 0;
    if (!nc || !nb || (d > 1 && !rg) || !fok) {
        mem_free((void *)nc);
        mem_free(nb);
        if (rg != rglyph)
            kfree(rg);
        if (gfx_depth == 1)
            fonts_flat();               /* the tables just built: one plane keeps none */
        return -ENOMEM;
    }
    int col, ptr;
    build_cop(nc, d, &col, &ptr);
    memset(nb, 0, (u32)BUFL * ls);
    blit_wait();
    volatile u16 *oc = cop;
    u8 *ob = tbuf;
    u16 s = irq_save();
    tbuf = nb;
    cop = nc;
    gfx_depth = d;
    lstride = ls;
    rglyph = rg;
    cop_col = col;
    cop_ptr = ptr;
    set_rows();
    set_off(0);
    cop_point(0);
    hw_off = 0;
    COP1LCH = (u16)((u32)nc >> 16);
    COP1LCL = (u16)(u32)nc;
    irq_restore(s);
    dgen++;
    u32 t0 = hr_time(), n = 0;          /* a vertical blank: the beam's line number goes down once */
    int last = (int)((VPOSL >> 8) & 0x1FF);
    while ((u32)(hr_time() - t0) < ECLOCK_PAL / 10 && ++n < 2000000u) {
        int v = (int)((VPOSL >> 8) & 0x1FF);
        if (v < last)
            break;
        last = v;
    }
    mem_free((void *)oc);
    mem_free(ob);
    if (d == 1) {                       /* one plane draws from the plain glyphs */
        kfree(rglyph);
        rglyph = 0;
        fonts_flat();
    }
    say_depth();
    term_redraw();
    return 1 << d;
}

int gfx_up(void) { return up; }

/* SYS_console CON_LINE (DESIGN D24 6.2), for the shown console (term_conop checked it): display line `line` of the
 * drawing window as one pen a pixel, and the display's facts, into the caller's struct uconline. Only "a line of
 * pens" is promised: the planes' layout stays the kernel's. -> the pens written */
long gfx_conline(u32 line, struct uconline *d)
{
    if (line >= (u32)dlines)
        return -EINVAL;
    u32 k = d->npens < 8 * STRIDE ? d->npens & ~7u : 8 * STRIDE;
    if (k && !user_ok_wr(current, d->pens, k))
        return -EFAULT;
    const u8 *p = line_at(off + (int)line);
    u8 *o = (u8 *)d->pens;
    for (u32 x = 0; x < k / 8; x++, p++)
        for (int b = 7; b >= 0; b--) {
            u32 pen = 0;
            for (int pl = gfx_depth - 1; pl >= 0; pl--)
                pen = pen << 1 | (p[pl * STRIDE] >> b & 1);
            *o++ = (u8)pen;
        }
    d->width = 8 * STRIDE;
    d->height = (u16)dlines;
    d->depth = (u16)gfx_depth;
    d->pad = 0;
    d->generation = dgen;
    d->size = UCONLINE_FIRST;                           /* the bytes of the struct the kernel filled */
    for (int i = 0; i < 16; i++)
        d->palette[i] = i < (1 << gfx_depth) ? colour_of(i, gfx_depth) : 0;
    return (long)k;
}

/* a kernel panic with the display up (interrupts are off): show the scroll position the text was drawn at (the
 * vertical-blank interrupt that normally applies it no longer runs; the Copper reads the pointers at the next frame)
 * and turn the background dark red with white text, so the photo of the screen shows at once that it stopped; the
 * text stays */
void gfx_panic(void)
{
    if (!up)
        return;
    sfont = 0;                          /* Topaz (D20 4.2: a panic is always readable); no table work here */
    top = line_at(off);                 /* a fault inside set_off may have left top behind off */
    cop_point(off);
    cop[cop_col + 1] = 0x800;
    for (int i = 1; i < (1 << gfx_depth); i++)
        cop[cop_col + 2 * i + 1] = 0xFFF;
}
