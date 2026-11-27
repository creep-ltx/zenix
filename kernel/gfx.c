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
 * all planes and a clear or copy of whole lines is one blit. The buffer is a ring of text rows (the screen's rows
 * and some spare ones, below): display row r is the ring's row (base + r) mod nring, so scrolling the whole screen
 * is base + 1 and one blitter fill of the row that comes in, never a copy. The Copper list shows it with two sets of
 * plane pointers: the first at the top (the ring's row base), the second where the ring wraps (its row 0), after a
 * WAIT for that line (PAL's lines from 256 on through the usual WAIT for line 255's end first), both written at
 * vertical blank. The spare rows keep the text the CPU and blitter draw out of sight while the display still shows
 * an older base; the terminal moves the screen by all of a write's scrolls at once (gfx_jump, DESIGN D28 21), and
 * shows what it drew when it is done (gfx_done). Region scrolls (CSI S/T/L/M, a scroll region) are blitter copies
 * too, a copy per run of rows that lie in one piece of the ring, and erased rows blitter fills with the background
 * colour. A glyph is drawn by the CPU from its 8 bytes: (glyph & m1) ^ m2 per plane, m1 set where the foreground and
 * background bits differ, m2 where the background bit is set; four cells in one colour a long word per plane and
 * line (gfx_cells). The CPU never reads chip RAM to draw. Glyphs come from the Kickstart ROM's Topaz 8, found through
 * GfxBase before the OS memory is reused.
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
#define BEAMCON0 REG16(0xDFF1DC)        /* D62: ECS/AGA Agnus: bit 5 PAL (the other bits 0: the standard beam) */
#define COP_WAIT_LINE 0x20              /* the Copper waits for this line before it moves the plane pointers */
#define STRIDE  80                      /* bytes per plane per line */

int trows = TROWS_MAX / 2;              /* text rows on this display: 32 (PAL) or 25 (NTSC), twice that interlaced */
int gfx_depth;                          /* bitplanes: 1 to 4 (0: no display) */
static int dlines = 256;                /* display lines: 8 * trows */
static int vlines = 256;                /* the beam's lines of the picture: 256 PAL, 200 NTSC (dlines halved in lace) */
static int lace;                        /* D28 22, CON_MODE: interlaced (two fields, two Copper lists) */
static int ntsc;                        /* D62: the video standard shown is NTSC (200 lines; CON_MODE's CONM_NTSC) */
static int lastl = 312;                 /* the line only a long field has: 312 PAL, 262 NTSC */
static u32 lstride;                     /* bytes per display line: 80 per plane */
static u32 rstride;                     /* bytes per text row: 8 lines */
static int aga;
static u8 *tbuf;                        /* chip RAM: nring rows */
static int nring;                       /* the ring's rows: trows + spare (gfx_init) */
static volatile u16 *cop;               /* the Copper lists: the long field's (the only one without lace), the short's
                                         * COP_WORDS further on */
static int cop_col, cop_ptr, cop_ptr2, cop_end; /* where the colours, the two sets of plane pointers and the field's
                                                 * end are (words, the same in both lists) */
static int base;                        /* the ring's row to show as display row 0 (the vertical blank applies it) */
static int dbase;                       /* D28 21: the ring's row drawn as display row 0 (ahead of base inside a flush) */
static volatile int hw_base = -1;       /* what the Copper list shows (-1: not known yet) */
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
static int held;                        /* D38: a console is shown in graphics mode: its server's Copper list runs */
#define DMAF_SPREN 0x0020               /* sprite DMA: on only while a server's Copper list (and its pointer) is shown */
#define SPRCTL(i) REG16(0xDFF142 + 8 * (i))

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
KINIT static int plausible(u32 a)
{
    const struct bootinfo *bi = BOOTINFO_K;
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

KINIT static int font_extract(u32 tf)
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
KINIT int gfx_font_init(u32 gfxbase)
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
static int blit_cpu;                    /* D38: a display server was switched away from without its answer: it may still be
                                         * writing the blitter's registers, so the kernel copies and fills with the CPU
                                         * until it answers or ends (gfx_blit_cpu) */
static void blit_copy(const u8 *src, u8 *dst, u32 rows)
{
    if (blit_cpu) {
        memmove(dst, src, rows * STRIDE);
        return;
    }
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
    if (blit_cpu) {
        for (u32 l = 0; l < lines; l++)
            for (int k = 0; k < gfx_depth; k++)
                memset(dst + l * lstride + (u32)k * STRIDE, (bg >> k & 1) ? 0xFF : 0, STRIDE);
        return;
    }
    int one = !bg || bg == (1u << gfx_depth) - 1u;     /* D28 21: every plane the same bit: one blit of all of them */
    if (one)
        lines *= (u32)gfx_depth;
    for (int k = 0; k < (one ? 1 : gfx_depth); k++) {
        u8 *d = dst + k * STRIDE;
        for (u32 left = lines; left;) {
            u32 h = left > 1024 ? 1024 : left;
            blit_wait();
            BLTCON0 = (bg >> k & 1) ? 0x01FF : 0x0100;     /* D only, D = 1 or D = 0 */
            BLTCON1 = 0;
            BLTDMOD = one ? 0 : (u16)(lstride - STRIDE);
            blit_ptr(&BLTDPTH, d);
            BLTSIZE = (u16)((h << 6) | (STRIDE / 2));
            d += h * (one ? STRIDE : lstride);
            left -= h;
        }
    }
}

/* ---- drawing ---- */

/* a background colour as this depth shows it */
static u32 bg_of(u8 colour)
{
    return gfx_depth == 1 ? 0 : gfx_depth == 2 ? map2[(colour >> 4) & 7u] : gfx_depth == 3 ? (colour >> 4) & 7u : colour >> 4;
}

/* attr bits: 1 inverse, 2 underline, 4 bold; colour: foreground in the low nibble, background in the high one. One
 * plane: the colours are not shown (grey on black, inverse is the glyph inverted, as before D14). Three planes: a
 * bright foreground is the colour plus bold (bright black, 8, is grey: black on black would hide it). If foreground
 * and background are the same, inverse (the cursor) uses the default colours so that it stays visible. DESIGN
 * D28 21: every glyph is drawn from its 8 bytes (gfx_cells: four cells a long word at a time, others put_bytes);
 * the table of each glyph line kept four times in a long (7 KB for Topaz, 8 KB for each loaded font) is gone. */
#define HOT __attribute__((optimize("O2")))     /* gfx.c is built -Os (cold set-up code); drawing is not */

/* A console's loaded 8-bit font (DESIGN D20 4.2: CON_FONT, each byte one of its 256 glyphs, indexed by the byte).
 * Only the shown console is drawn, so one pointer says whose glyphs are drawn (0: the ROM's Topaz, indexed byte - 32,
 * a byte below 32 drawn as '?'). */
struct gfont {
    u8 g[256][8];
};
static const struct gfont *sfont;
static int scga;                        /* the shown console asked for the CGA palette (CONR_CGAPAL) */
static volatile int pal_dirty;          /* the Copper list's colours are to be written at the next vertical blank */
static u32 dgen;                        /* CON_LINE's generation: raised when the depth, planes or palette change */

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

/* gfx_cell's addresses without a multiply (the 68020's muls.l costs up to 44 cycles, and gcc turns shift-and-add back
 * into one): `top` is the ring's first byte, `rowoff` each display row's byte offset in the ring (DESIGN D28 15, 20) */
static u8 *top;
static u32 rowoff[TROWS_MAX];
static void set_rows(int b)             /* the ring's row b is display row 0 of the drawing, or tbuf/lstride changed */
{
    dbase = b;
    top = tbuf;
    u32 a = (u32)b * rstride, end = (u32)nring * rstride;
    for (int r = 0; r < trows; r++, a += rstride) {
        if (a >= end)
            a -= end;
        rowoff[r] = a;
    }
}
static void set_base(int b) { set_rows(b); base = b; }   /* ... and shown from the next vertical blank */
static u8 *row_at(int r) { return tbuf + rowoff[r]; }
static u8 *line_at(int l) { return row_at(l >> 3) + (u32)(l & 7) * lstride; }     /* display line l */

/* how many of the display rows a.., and b.. (n at most) lie in one piece of the ring each */
static int run_up(int a, int b, int n)
{
    int k = 1;
    while (k < n && rowoff[a + k] == rowoff[a + k - 1] + rstride && rowoff[b + k] == rowoff[b + k - 1] + rstride)
        k++;
    return k;
}
/* the same, the rows ..a and ..b counted backwards (a and b the last ones) */
static int run_down(int a, int b, int n)
{
    int k = 1;
    while (k < n && rowoff[a - k] + rstride == rowoff[a - k + 1] && rowoff[b - k] + rstride == rowoff[b - k + 1])
        k++;
    return k;
}


/* D28: 8 glyph bytes drawn byte-wise at any depth: each plane's byte is (b & m) ^ n, m set where the foreground and
 * background bits differ, n where the background bit is set (no replicated table: gcc turns every C replication into
 * a 44-cycle muls.l, DESIGN D28 8); bold smears a pixel, underline fills the last line. D28 21: one cell (gfx_cells
 * draws runs four at a time), so one small loop over the planes (-Os) */
static void put_bytes(u8 *p, const u8 *g, u32 attr, u32 colour)
{
    u32 fg = 7, bg = 0;
    int d = gfx_depth;
    if (d > 1)
        attr = pens(attr, colour, &fg, &bg);
    else if (attr & 1)
        fg = 0, bg = 7;                                 /* one plane: inverse is the glyph inverted */
    u8 m[4], n[4];
    for (int k = 0; k < d; k++) {
        m[k] = ((fg ^ bg) >> k & 1) ? 0xFF : 0;
        n[k] = (bg >> k & 1) ? 0xFF : 0;
    }
    for (int y = 0; y < 8; y++, p += lstride) {
        u32 b = g[y];
        if (attr & 4)
            b |= b >> 1;
        if ((attr & 2) && y == 7)
            b = 0xFF;
        for (int k = 0; k < d; k++)
            p[k * STRIDE] = (u8)((b & m[k]) ^ n[k]);
    }
}

/* DESIGN D28 21: cells x0..x1-1 of display row `row` from a console's grids (ch, at, co: the cells' bytes, cell x0
 * first; none above U+00FF). Four cells in one attribute and colour that start at a multiple of 4 are drawn a long
 * word at a time: each glyph line's four bytes gathered in a register, each plane one long-word store (G & m) ^ n,
 * m set where the foreground and background bits differ, n where the background bit is set: a quarter of the chip
 * RAM writes of drawing them one by one, the CPU never reads chip RAM. Others, bold and underline, one by one. */
__attribute__((optimize("O2", "no-unswitch-loops"))) void gfx_cells(int row, int x0, int x1, const u8 *ch, const u8 *at, const u8 *co)
{
    if (!up)
        return;
    u8 *p = top + rowoff[row];
    const struct gfont *f = sfont;
    for (int x = x0; x < x1;) {
        int i = x - x0;
        u32 a4 = 0, c4 = 0;
        if (!(x & 3) && x + 4 <= x1) {
            a4 = *(const u32 *)(at + i);
            c4 = *(const u32 *)(co + i);
        }
        u32 fg = 0, bg = 0, a = at[i];
        if ((x & 3) || x + 4 > x1 || ((a4 ^ (a4 >> 8)) & 0xFFFFFF) || ((c4 ^ (c4 >> 8)) & 0xFFFFFF) ||
            (gfx_depth == 1 ? (a & 6) : (pens(a, co[i], &fg, &bg) & 6))) {
            u32 c = ch[i];
            put_bytes(p + x, f ? f->g[c] : glyphs[(c < 32 ? '?' : c) - 32], a, co[i]);
            x++;
            continue;
        }
        const u8 *g[4];
        int sp = !f && *(const u32 *)(ch + i) == 0x20202020u;  /* four spaces in Topaz: no glyph read (a cleared row) */
        for (int j = 0; j < 4; j++) {
            u32 c = ch[i + j];
            g[j] = f ? f->g[c] : glyphs[(c < 32 ? '?' : c) - 32];
        }
        u32 m0 = ~0u, n0 = (a & 1) ? ~0u : 0, m1 = 0, n1 = 0, m2 = 0, n2 = 0, m3 = 0, n3 = 0;
        if (gfx_depth > 1) {
            u32 d = fg ^ bg;
            m0 = -(d & 1); n0 = -(bg & 1);
            m1 = -(d >> 1 & 1); n1 = -(bg >> 1 & 1);
            m2 = -(d >> 2 & 1); n2 = -(bg >> 2 & 1);
            m3 = -(d >> 3 & 1); n3 = -(bg >> 3 & 1);
        }
        __asm__("" : "+d"(m0), "+d"(n0), "+d"(m1), "+d"(n1));       /* opaque: no muls for a 0/~0 mask (D28 8) */
        __asm__("" : "+d"(m2), "+d"(n2), "+d"(m3), "+d"(n3));
        u8 *q = p + x;
        for (int y = 0; y < 8; y++, q += lstride) {
            u32 v = sp ? 0 : (u32)g[0][y] << 24 | (u32)g[1][y] << 16 | (u32)g[2][y] << 8 | g[3][y];
            *(u32 *)q = (v & m0) ^ n0;
            if (gfx_depth > 1) {
                *(u32 *)(q + STRIDE) = (v & m1) ^ n1;
                if (gfx_depth > 2) {
                    *(u32 *)(q + 2 * STRIDE) = (v & m2) ^ n2;
                    if (gfx_depth > 3)
                        *(u32 *)(q + 3 * STRIDE) = (v & m3) ^ n3;
                }
            }
        }
        x += 4;
    }
}

/* one cell (the cursor, a cell above U+00FF's stand-in): gfx_cells' one-by-one path */
void gfx_cell(int row, int col, u8 ch, u8 attr, u8 colour) { gfx_cells(row, col, col + 1, &ch, &attr, &colour); }

/* D28: a cell above U+00FF: part 0 one cell, 1 the left half of a wide character, 2 its right half (v the lead's).
 * The font's entry: 0 none (the box; a wide character's halves of the wide box), 32-255 Topaz's glyph of that byte
 * (a stand-in, drawn exactly as Topaz), 256 + n its glyph n. A lead with a glyph has a blank right half. */
HOT void gfx_ucell(int row, int col, u16 v, u8 attr, u8 colour, int part)
{
    if (!up)
        return;
    u8 *p = top + rowoff[row] + col;
    u32 e = 0;
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
    u32 bg = bg_of(colour);
    for (int k; n > 0; row += k, n -= k) {     /* a fill per piece of the ring */
        k = run_up(row, row, n);
        blit_fill(row_at(row), (u32)k * 8, bg);
    }
    blit_wait();
}

/* n text rows from row src to row dst (they may overlap), by the blitter: a copy per run of rows that lie in one piece
 * of the ring at both ends, upwards from the first row (dst above src) or downwards from the last, so a row is read
 * before it is written over (a run itself copies as memmove does) */
void gfx_move_rows(int dst, int src, int n)
{
    if (!up || n <= 0 || dst == src)
        return;
    u32 pl = 8 * (u32)gfx_depth;
    if (dst < src)
        for (int i = 0, k; i < n; i += k) {
            k = run_up(dst + i, src + i, n - i);
            blit_copy(row_at(src + i), row_at(dst + i), (u32)k * pl);
        }
    else
        for (int i = n, k; i > 0; i -= k) {
            k = run_down(dst + i - 1, src + i - 1, i);
            blit_copy(row_at(src + i - k), row_at(dst + i - k), (u32)k * pl);
        }
    blit_wait();
}

/* the Copper list shows the ring from its row b: the first pointers at row b, a WAIT for the line where the ring
 * wraps (after the picture when it does not wrap on the screen), the second pointers at its row 0. A line from 256
 * on is waited for in two steps: line 255's end, then the line's low 8 bits (the Copper compares 8 bits of the line).
 * Written at vertical blank, or with the interrupts off (a panic). */
#define COP_WORDS 120                   /* a field's list at most (build_cop checks it) */
static void cop_point(int b)
{
    int s = (nring - b) * 8;            /* the display line where the ring wraps */
    if (s > dlines)
        s = dlines;
    u32 l = (u32)(DIW_V + (s >> lace));         /* (interlaced: each field shows every other line) */
    if (lace && l >= (u32)lastl)
        l = (u32)lastl - 1;             /* a short field never reaches lastl (DIWV 0x38: 312); past the picture anyway */
    for (int f = 0; f <= lace; f++) {           /* D28 22: the short field's planes start a line further in */
        volatile u16 *c = cop + f * COP_WORDS;
        const u8 *p = tbuf + (u32)b * rstride + (u32)f * lstride, *q = tbuf + (u32)f * lstride;
        for (int k = 0; k < gfx_depth; k++, p += STRIDE, q += STRIDE) {
            c[cop_ptr + 4 * k + 1] = (u16)((u32)p >> 16);
            c[cop_ptr + 4 * k + 3] = (u16)(u32)p;
            c[cop_ptr2 + 4 * k + 1] = (u16)((u32)q >> 16);
            c[cop_ptr2 + 4 * k + 3] = (u16)(u32)q;
        }
        c[cop_ptr2 - 4] = l >= 256 ? 0xFFDF : (u16)(l << 8 | 1);
        c[cop_ptr2 - 2] = (u16)((l & 0xFF) << 8 | 1);
        if (lace)                       /* the wait for lastl: past line 255 already, or first to it */
            c[cop_end + 4] = l >= 256 ? (u16)((lastl & 0xFF) << 8 | 1) : 0xFFDF;
    }
}

/* interlaced: the lists l (both fields', end at word e) name a for the next field, and b at lastl (the kernel's own:
 * l and l + COP_WORDS; a display server's c for both while it has the screen, or the new lists at a switch, so the
 * Copper never follows lists that are given back or not shown) */
static void cop_next(volatile u16 *l, int e, u32 a, u32 b)
{
    for (int f = 0; f < 2; f++, l += COP_WORDS) {
        l[e + 1] = (u16)(a >> 16);
        l[e + 3] = (u16)a;
        l[e + 9] = (u16)(b >> 16);
        l[e + 11] = (u16)b;
    }
}

/* colour i in the lists (both fields') */
static void set_colour(int i, u16 v)
{
    cop[cop_col + 2 * i + 1] = v;
    cop[COP_WORDS + cop_col + 2 * i + 1] = v;
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

/* DESIGN D55 6: the vertical blanks counted; the interrupt only counts and asks kwork to wake the waiters (D16:
 * producers run in process context) */
static volatile u32 vbl_n;
static u32 vbl_wait;                    /* the slots waiting in CON_VBLANK */
void gfx_vwake(void)
{
    if (vbl_wait)
        ev_wake(&vbl_wait);
}
/* CON_VBLANK: wait until the count reaches want (or 100 ms passed) -> the count now; CONV_ASK only asks */
long gfx_vwait(u32 want)
{
    if (!up)
        return -ENODEV;
    if (want & CONV_ASK)
        return (long)(vbl_n & 0x7FFFFFFFu);
    u32 until = ticks + 10;
    for (;;) {
        ev_arm(&vbl_wait);              /* registered first: a count after the test still wakes us (ev_block) */
        u32 n = vbl_n & 0x7FFFFFFFu;
        if ((s32)((n - want) << 1) >= 0 || (s32)(ticks - until) >= 0)
            return (long)n;
        long r = ev_block(EV_IO, 1, until, 0);
        if (r < 0)
            return r;
    }
}

void gfx_vblank(void)
{
    vbl_n++;
    if (vbl_wait)
        kwork_pending = 1;
    int o = base;
    if (up && (o != hw_base || pal_dirty) && (int)((VPOSL >> 8) & 0x1FF) < COP_WAIT_LINE - 4) {
        cop_point(o);
        hw_base = o;
        if (pal_dirty) {
            pal_dirty = 0;
            for (int i = 0; i < (1 << gfx_depth); i++)
                set_colour(i, colour_of(i, gfx_depth));
        }
    }
    gcon_vblank();                      /* D38: the pointer's new place, before sprite DMA reads it */
}

/* DESIGN D28 20, 21: the rows a scroll brings in must not be on show. The display shows the ring from hw_base (set at
 * vertical blank); the drawing (dbase) may run ahead of it by up to the spare rows (nring - trows). gfx_jump moves the
 * drawing k rows on: within the spare rows nothing waits; beyond them, with fast RAM (as many spare rows as the
 * screen's) the whole screen is drawn afresh in the half not on show, so nothing ever waits either; without fast RAM
 * (CHIP_SPARE) it waits for the vertical blank, and a jump of more rows than the spare ones draws the whole screen
 * as far on as the spare rows allow (the rows it shares with the shown picture are drawn while on show, until the
 * next frame). What is drawn is shown once the caller says it is done (gfx_done): the vertical blank never shows a
 * row half drawn. No waiting with the interrupts masked (a panic, a kprintf inside a masked part: no vertical blank
 * comes), and every wait has a timeout: 100 ms, said once on the log, then no more waits. -> the rows at the bottom
 * of the screen to fill and draw: k, or trows (all of them). */
static int vb_dead;
int gfx_jump(int k)
{
    if (k > trows)
        k = trows;
    int h = hw_base, sp = nring - trows, ahead = dbase - h;
    if (ahead < 0)
        ahead += nring;
    u16 sr;
    __asm__ volatile("move.w %%sr,%0" : "=d"(sr));
    if (up && h >= 0 && !vb_dead && (sr & 0x0700) < 0x0300 && ahead + k > sp) {
        if (sp >= trows) {                              /* the half not on show */
            set_rows(h + trows >= nring ? h + trows - nring : h + trows);
            return trows;
        }
        u32 t0 = hr_time(), n = 0;
        while (base != h && hw_base == h)               /* (nothing new to show: no vertical blank to wait for) */
            if ((++n & 63) == 0 && (u32)(hr_time() - t0) > eclock_hz / 10) {
                vb_dead = 1;
                kprintf("gfx: no vertical blank in 100 ms: scrolling goes on without waiting for it\n");
                break;
            }
        if (k > sp) {                                   /* (dbase is shown now: gfx_done came before) */
            set_rows(dbase + sp >= nring ? dbase + sp - nring : dbase + sp);
            return trows;
        }
    }
    int nb = dbase + k;
    set_rows(nb >= nring ? nb - nring : nb);
    return k;
}

void gfx_done(void) { base = dbase; }

/* the whole screen takes the background of `colour`; the ring keeps its place. The display may still show an older
 * base until the vertical blank: its rows are cleared too when they are others */
void gfx_clear_text(u8 colour)
{
    if (!up)
        return;
    int h = hw_base;
    if (h >= 0 && h != dbase) {
        int b = dbase;
        set_rows(h);
        gfx_fill_rows(0, trows, colour);
        set_rows(b);
    }
    gfx_fill_rows(0, trows, colour);
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
 * so every one of 16 reads must agree. D62: an Agnus or Alice with bit 5 in its id (VPOSR bits 14-8: ECS $20/$30, AGA
 * $22/$23/$32/$33; OCS $00/$10) has BEAMCON0 and switches between PAL and NTSC */
static int agnus(void) { return (int)(VPOSR >> 8) & 0x7F; }
static int denise(void)                 /* D62: 0xF8 Lisa, 0xFC ECS Denise (super-hires), 0 OCS */
{
    u16 v = DENISEID & 0xFF;
    for (int i = 0; i < 16; i++)
        if ((DENISEID & 0xFF) != v)
            return 0;
    return v == 0xF8 || v == 0xFC ? v : 0;
}

/* DESIGN D28 20: the ring's chip RAM at ls bytes a line. Its spare rows (gfx_init): as many as the screen's with fast
 * RAM (a whole screen drawn out of sight: no scroll ever waits, D28 21), CHIP_SPARE on a machine with chip RAM only
 * (audit OS-3: 8, 400 lines a second before a scroll waits, about what its CPU draws; measured in FS-UAE on the stock
 * A1200: 16 -> 8 rows cost ls -laR / 8% on the screen and nothing else; 4 rows doubled it, none made it 2.6 times) */
#define CHIP_SPARE 8
static u32 ring_bytes(u32 ls) { return (u32)nring * 8 * ls; }

/* the depth for this machine (D14's default, section 11): AGA with fast RAM: 4; fast RAM on OCS/ECS, or 2 MB of chip
 * RAM and no fast RAM: 3; otherwise 1. GFX_DEPTH (make GFXDEPTH=n) asks for one: 4 only on AGA (else 3). */
static int fast;                        /* the machine has fast RAM (pick_depth) */
static int pick_depth(void)
{
    const struct bootinfo *bi = BOOTINFO_K;
    int chip2m = 0;
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

/* the Copper lists for `depth` planes, interlaced or not (lc), into l (2 x COP_WORDS of chip RAM); the indices of
 * the colours, the two sets of pointers (cop_point fills them and the WAITs before the second) and the field's end.
 * Interlaced (D28 22, the same scheme as displayd's, docs/d38-desktop.md "Interlace on the chips"): each field's
 * list skips the other field's lines (the modulos one line more) and ends naming the long field's list for the next
 * field; only a long field reaches line lastl, and there names the short field's instead. */
#define DIWSTOP_OF(vl) ((((DIW_V + (vl)) & 0xFF) << 8) | 0xC1)   /* the stop line's bit 8 is implied (not bit 7) */
#define DIWSTOP_V DIWSTOP_OF(vlines)
/* (vl, ll: the picture's lines and the long field's last line of the video standard the list is for, D62) */
static void build_cop(volatile u16 *l, int depth, int lc, int vl, int ll, int *col, int *ptr, int *ptr2, int *end)
{
    int f3 = aga && depth > 1;          /* more than one plane on AGA: always at FMODE 3 */
    u16 mod = (u16)((u32)STRIDE * (u32)depth * (u32)(lc + 1) - STRIDE + (f3 ? F3_MODADJ : 0));
    volatile u16 *c = l;
#define MOVE(r, v) (*c++ = (u16)(r), *c++ = (u16)(v))
    if (aga) {                          /* written every frame: a previous program's values survive a warm reset */
        MOVE(0x1FC, f3 ? F3_FMODE : 0);       /* FMODE: 64-bit bitplane fetch */
        MOVE(0x106, 0x0C00);            /* BPLCON3: colour bank 0, LOCT 0 */
        MOVE(0x10C, 0x0011);            /* BPLCON4: no BPLAM colour-index XOR, sprite palettes as after reset */
    }
    MOVE(0x08E, (DIW_V << 8) | 0x81);           /* DIWSTRT, DIWSTOP: every frame, so a display server's (D38) */
    MOVE(0x090, DIWSTOP_OF(vl));                /* window never outlives it */
    MOVE(0x100, 0x8200 | (depth << 12) | (lc << 2));    /* BPLCON0: hires, N planes, colour burst, LACE */
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
    *c++ = 0xFFDF;                      /* the ring's wrap: two WAITs (cop_point sets their lines), the pointers */
    *c++ = 0xFFFE;
    *c++ = 0xFFDF;
    *c++ = 0xFFFE;
    *ptr2 = (int)(c - l);
    for (int k = 0; k < depth; k++)
        MOVE(0x0E0 + 4 * k, 0), MOVE(0x0E2 + 4 * k, 0);
    *end = (int)(c - l);
    if (lc) {                           /* the next field's list: the long one, or at lastl the short one */
        MOVE(0x080, (u32)l >> 16);
        MOVE(0x082, (u32)l);
        *c++ = 0xFFDF;                  /* (cop_point: lastl's low bits when the wrap waited past 255) */
        *c++ = 0xFFFE;
        *c++ = (u16)((ll & 0xFF) << 8 | 1);
        *c++ = 0xFFFE;
        MOVE(0x080, (u32)(l + COP_WORDS) >> 16);
        MOVE(0x082, (u32)(l + COP_WORDS));
    }
    *c++ = 0xFFFF;
    *c++ = 0xFFFE;
    if (c - l > COP_WORDS)
        panic("gfx: the Copper list is longer than COP_WORDS");
    if (lc)                             /* the short field's list: the same words (cop_point sets its pointers) */
        memcpy((void *)(l + COP_WORDS), (const void *)l, 2 * COP_WORDS);
#undef MOVE
}

/* glyph lines kept four times in a long, for more than one plane (Topaz's 7 KB, a loaded font's 8 KB): 0 when out
 * of memory */
struct gfont *gfx_font_new(const u8 *g)
{
    struct gfont *f = kmalloc(sizeof *f);
    if (!f)
        return 0;
    memcpy(f->g, g, sizeof f->g);
    return f;
}

void gfx_font_free(struct gfont *f)
{
    if (!f)
        return;
    if (sfont == f)
        sfont = 0;
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
    kprintf("display: %s chips, %d bitplane%s%s%s\n", aga ? "AGA" : "OCS/ECS", gfx_depth, gfx_depth > 1 ? "s" : "",
            aga && gfx_depth > 1 ? " at 64-bit fetch (FMODE 3)" : "", lace ? ", interlaced" : "");
}

KINIT int gfx_init(void)
{
    int fl = frame_lines();
    if (fl && fl < 280) {               /* NTSC: 200 lines */
        trows = 25;
        dlines = vlines = 200;
        lastl = 262;
    }
    ntsc = trows == 25;
    kprintf("display: beam's last line %d: %s, %d text rows\n", fl, ntsc ? "NTSC" : "PAL", trows);
    aga = denise() == 0xF8;
    cop = mem_alloc(4 * COP_WORDS, MEM_CHIP, 0);
    if (!cop)
        return -1;
    int d = pick_depth();
    nring = trows + (fast ? trows : CHIP_SPARE);        /* D28 20: the spare rows */
    for (gfx_depth = d; ; gfx_depth = gfx_depth == 4 ? 3 : 1) {      /* too little chip RAM: fewer planes */
        lstride = (u32)STRIDE * (u32)gfx_depth;
        tbuf = mem_alloc(ring_bytes(lstride), MEM_CHIP, 0);     /* 16-byte aligned: FMODE 3 needs 8 */
        if (tbuf || gfx_depth == 1)
            break;
    }
    if (!tbuf)
        return -1;
    say_depth();
    build_cop(cop, gfx_depth, 0, vlines, lastl, &cop_col, &cop_ptr, &cop_ptr2, &cop_end);
    rstride = 8 * lstride;
    set_base(0);
    cop_point(0);
    hw_base = 0;
    memset(tbuf, 0, ring_bytes(lstride));

    BPLCON0 = 0x0200;                   /* no planes until the Copper sets them */
    DIWSTRT = (u16)((DIW_V << 8) | 0x81);
    DIWSTOP = (u16)DIWSTOP_V;
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
/* D62: ZINE's way back sets the machine's own standard (the blob's BEAMCON0); the one shown now again */
void gfx_zine_back(void)
{
    if (agnus() & 0x20)
        BEAMCON0 = ntsc ? 0 : 0x20;
}
u32 gfx_zine(u16 *diw)
{
    diw[0] = (u16)((DIW_V << 8) | 0x81);
    diw[1] = (u16)DIWSTOP_V;
    return (u32)cop;
}

/* /etc/console's settings (init, SYS_console; D14 section 11, D28 22): show d planes (1-4), interlaced or not (lc),
 * in NTSC or PAL (n, D62: only an ECS/AGA Agnus switches, gfx_set_mode checks; BEAMCON0 is written with the switch)
 * from now on. The new buffer and Copper lists come first, then (a new number of rows) the consoles' new grids (no
 * memory for any: -ENOMEM, the display stays as it is); the switch is one register write with interrupts masked,
 * which the Copper takes at the next vertical blank; the consoles' text moves into the new grids at once (nothing
 * draws between: no kprintf). One frame later (a timeout of 100 ms: the beam may not move) the old ones are freed and
 * the console shown is drawn again. -> 0 or -errno */
static int screen(int d, int lc, int n)
{
    if (!up)
        return -ENODEV;
    if (held)
        return -EBUSY;                  /* D38: a display server has the screen; its Copper list stays */
    if (d == gfx_depth && lc == lace && n == ntsc)
        return 0;
    int vl = n ? 200 : 256, ll = n ? 262 : 312;
    int rows = (vl >> 3) << lc, nr = rows + (fast ? rows : CHIP_SPARE);
    u32 ls = (u32)STRIDE * (u32)d, rb = (u32)nr * 8 * ls;
    blit_wait();                        /* first: its timeout's kprintf draws (nothing may draw between the grids' steps) */
    volatile u16 *nc = mem_alloc(4 * COP_WORDS, MEM_CHIP, 0);
    u8 *nb = mem_alloc(rb, MEM_CHIP, 0);
    if (!nc || !nb || (rows != trows && term_regrid(rows) < 0)) {
        mem_free((void *)nc);
        mem_free(nb);
        return -ENOMEM;
    }
    int col, ptr, ptr2, end;
    build_cop(nc, d, lc, vl, ll, &col, &ptr, &ptr2, &end);
    memset(nb, 0, rb);
    volatile u16 *oc = cop;
    u8 *ob = tbuf;
    u16 s;
    /* D62: to NTSC, BEAMCON0 is written while the beam is above line 240 (22 lines before NTSC's last): a PAL beam
     * already past 262 would run on to its counter's wrap, a frame without a vertical blank. At most a frame of
     * waiting (100 ms: a beam that does not move, no display to spoil) */
    for (u32 t0 = hr_time(), k = 0; n && !ntsc && (int)((VPOSL >> 8) & 0x1FF) >= 240 &&
                                    (u32)(hr_time() - t0) < eclock_hz / 10 && ++k < 2000000u;)
        ;
    s = irq_save();
    if (lace)                           /* the old lists name the new one for the next field, not themselves */
        cop_next(oc, cop_end, (u32)nc, (u32)nc);
    if (n != ntsc)
        BEAMCON0 = n ? 0 : 0x20;        /* D62: from the next frame on, 262 or 312 lines */
    ntsc = n;
    vlines = vl;
    lastl = ll;
    tbuf = nb;
    cop = nc;
    gfx_depth = d;
    lace = lc;
    lstride = ls;
    cop_col = col;
    cop_ptr = ptr;
    cop_ptr2 = ptr2;
    cop_end = end;
    rstride = 8 * ls;
    nring = nr;
    int moved = rows != trows;
    trows = rows;
    dlines = rows * 8;
    set_base(0);
    cop_point(0);
    hw_base = 0;
    COP1LCH = (u16)((u32)nc >> 16);
    COP1LCL = (u16)(u32)nc;
    irq_restore(s);
    if (moved)
        term_regrid(0);                 /* the text into the new grids, before anything draws (interrupts only queue) */
    dgen++;
    u32 t0 = hr_time(), k = 0;          /* a vertical blank: the beam's line number goes down once */
    int last = (int)((VPOSL >> 8) & 0x1FF);
    while ((u32)(hr_time() - t0) < eclock_hz / 10 && ++k < 2000000u) {
        int v = (int)((VPOSL >> 8) & 0x1FF);
        if (v < last)
            break;
        last = v;
    }
    mem_free((void *)oc);
    mem_free(ob);
    say_depth();
    term_redraw();
    return 0;
}

/* CON_COLOURS: 2, 4, 8, 16 (0 asks) -> the colours shown now. 16 on OCS/ECS (no 64-bit fetch, never 4 planes at the
 * 16-bit one) gives 8. */
int gfx_set_colours(int colours)
{
    int d = colours == 2 ? 1 : colours == 4 ? 2 : colours == 8 ? 3 : colours == 16 ? 4 : 0;
    if (!d)
        return colours ? -EINVAL : 1 << gfx_depth;
    if (d == 4 && !aga)
        d = 3;
    int r = screen(d, lace, ntsc);
    return r ? r : 1 << d;
}

/* CON_MODE (D28 22): CONM_HIRES or CONM_LACE (CONM_ASK asks) -> the mode shown now. D62: CONM_PAL or CONM_NTSC
 * added chooses the video standard too (the answer then has the standard's bit); CONM_INFO asks for the mode, the
 * standard and what the chips can show (CONMI_*) */
int gfx_set_mode(int m)
{
    if (m == CONM_ASK)
        return lace;
    int ag = agnus(), de = denise();
    if (m == CONM_INFO)
        return lace | (ntsc ? CONM_NTSC : CONM_PAL) | (de == 0xF8 ? CONMI_AGA : 0) | (de ? CONMI_SHRES : 0) |
               (ag & 0x20 ? CONMI_BEAM : 0) | (ag & 0x10 ? CONMI_NTSCX : 0);
    int st = m & (CONM_PAL | CONM_NTSC), n = st ? st == CONM_NTSC : ntsc;
    if ((m & ~(CONM_LACE | CONM_PAL | CONM_NTSC)) || st == (CONM_PAL | CONM_NTSC))
        return -EINVAL;
    if (n != ntsc && !(ag & 0x20))
        return -ENODEV;                 /* an OCS Agnus shows its crystal's standard only */
    int r = screen(gfx_depth, m & CONM_LACE, n);
    return r ? r : st ? (m & CONM_LACE) | (ntsc ? CONM_NTSC : CONM_PAL) : m;
}

int gfx_up(void) { return up && !held; }
int gfx_present(void) { return up; }
void gfx_blit_cpu(int on) { blit_wait(); blit_cpu = on; }

/* the kernel's display back from a display server at once (gfx_hand(0), a panic): its Copper list from now on
 * (COPJMP1: the server's list stops mid-frame, its memory may be going), sprites off, the DMA the console needs */
static void reclaim(void)
{
    held = 0;
    if (lace)
        cop_next(cop, cop_end, (u32)cop, (u32)(cop + COP_WORDS));
    COP1LCH = (u16)((u32)cop >> 16);
    COP1LCL = (u16)(u32)cop;
    COPJMP1 = 0;
    DMACON = DMAF_SPREN;
    DMACON = DMAF_SETCLR | DMAF_MASTER | DMAF_BPLEN | DMAF_COPEN | DMAF_BLTEN;
    for (int i = 0; i < 8; i++)
        SPRCTL(i) = 0;
    DIWSTRT = (u16)((DIW_V << 8) | 0x81);
    DIWSTOP = (u16)DIWSTOP_V;
    hw_base = -1;
    pal_dirty = 1;
}
void gfx_reclaim(void) { if (up && held) reclaim(); }
int gfx_diwv(void) { return DIW_V; }
int gfx_aga(void) { return aga; }
void gfx_topaz(u8 *dst) { memcpy(dst, glyphs, sizeof glyphs); }
void gfx_geom(u16 *w, u16 *h) { *w = 8 * STRIDE; *h = (u16)vlines; }   /* (CON_GRAB: the frame's lines) */

/* DESIGN D38 step 2 (gcon.c): show a display server's Copper list (cop), or the kernel's again (0). The kernel's own
 * blit ends first (the server may use the blitter from here); the Copper takes the new list at the next vertical
 * blank (COP1LC is where it starts each frame). Back to the kernel: sprite DMA off and the eight sprites switched off
 * (a sprite keeps showing its last data words otherwise), the console's DMA on again, the Copper restarted on the
 * kernel's list at once, the display window as gfx_init set it (the kernel's list sets it every frame too), the
 * plane pointers and colours written again at the next vertical blank; the caller draws the text again. */
void gfx_hand(u32 c)
{
    if (!up)
        return;
    blit_wait();
    u16 s = irq_save();
    if (c) {
        held = 1;
        if (lace)                       /* the kernel's lists would name themselves again at the field's end */
            cop_next(cop, cop_end, c, c);
        COP1LCH = (u16)(c >> 16);
        COP1LCL = (u16)c;
        DMACON = DMAF_SETCLR | DMAF_SPREN;
    } else
        reclaim();
    irq_restore(s);
}

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
    const u8 *p = line_at((int)line);
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
    if (held)
        reclaim();                      /* D38: a display server had the screen (panic_begin did it already) */
    sfont = 0;                          /* Topaz (D20 4.2: a panic is always readable); no table work here */
    set_base(dbase);                    /* a fault inside set_rows may have left rowoff half done */
    cop_point(dbase);
    set_colour(0, 0x800);
    for (int i = 1; i < (1 << gfx_depth); i++)
        set_colour(i, 0xFFF);
}
