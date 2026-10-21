#include "kernel.h"
#include "hw.h"

/* Framebuffer console: 640x256 hires, one bitplane, glyphs from the
 * Kickstart ROM's Topaz 8 font.
 *
 * The bootblock saves GfxBase->DefaultFont before the takeover; for
 * Topaz both the TextFont structure and its strike data live in the ROM
 * we keep mapped, so the kernel extracts the glyphs at leisure. If
 * validation fails the console stays serial-only.
 *
 * Framebuffer and Copper list sit in chip RAM (the DMA arena, mapped
 * cache-inhibited). Output is always mirrored to serial.
 *
 * Scrolling is pan-based (CCon's lesson, one machine deeper): the
 * buffer is two screens tall and the visible window slides down by
 * bumping the bitplane pointer in the Copper list - one 16-bit write,
 * since the high word never changes inside the buffer. Only when the
 * window hits the bottom does a 20KB jump-back copy occur, so the
 * per-scroll cost is ~80 bytes amortized instead of a 20KB memcpy. */

#define FB      ((u8 *)0x20000)         /* 80*512 = 40KB in chip RAM */
#define COPLIST ((volatile u16 *)0x1F000)
#define COLS 80
#define ROWS 32
#define FBLINES (ROWS * 8 * 2)          /* buffer scanlines: 2 screens */

static int fb_off;                      /* visible top scanline, 0..256 */

/* TextFont field offsets (graphics/text.h, RKRM includes) */
#define TF_YSIZE    20
#define TF_XSIZE    24
#define TF_LOCHAR   32
#define TF_HICHAR   33
#define TF_CHARDATA 34
#define TF_MODULO   38
#define TF_CHARLOC  40

static u8 glyphs[224][8];               /* Latin-1 32..255 (Topaz has all) */
static int fb_up;
static int cx, cy;

/* input ring shared by serial RX and keyboard ISRs */
static u8 inring[256];
static volatile u8 inhead, intail;

void con_input_push(u8 c)
{
    if (c == 3 && console_fg > 0) {     /* Ctrl-C -> SIGINT to foreground */
        for (int i = 0; i < NPROC; i++)
            if (procs[i].state != P_FREE && procs[i].pid == console_fg) {
                sig_post(&procs[i], SIGINT);
                return;
            }
        return;
    }
    u8 nh = (u8)(inhead + 1);
    if (nh != intail) {
        inring[inhead] = c;
        inhead = nh;
    }
    console_wake();
}

int con_getc_buffered(void)
{
    if (intail == inhead)
        return -1;
    int c = inring[intail];
    intail = (u8)(intail + 1);
    return c;
}

/* Anywhere the supervisor identity map covers: RAM regions or ROM.
 * (graphics.library copies the TextFont STRUCT to fast RAM at init;
 * only the glyph strike itself stays in ROM. The RAM copy is still
 * intact this early in boot - we extract before those pages get
 * reused, into our own table.) */
static int plausible(u32 a)
{
    return (a >= 0x400 && a < 0x08000000u) ||
           (a >= 0xF00000u && a < 0x1000000u);
}

#define GB_TEXTFONTS 140        /* struct List of all TextFonts */
#define GB_DEFAULTFONT 154

static int font_extract(u32 tf)
{
    if (!plausible(tf) || (tf & 1))
        return -1;
    u8 *f = (u8 *)tf;
    u16 ysize = *(u16 *)(f + TF_YSIZE);
    u16 xsize = *(u16 *)(f + TF_XSIZE);
    u8 lo = f[TF_LOCHAR], hi = f[TF_HICHAR];
    u32 data = *(u32 *)(f + TF_CHARDATA);
    u16 modulo = *(u16 *)(f + TF_MODULO);
    u32 charloc = *(u32 *)(f + TF_CHARLOC);
    if (ysize != 8 || xsize != 8 || lo > 32 || hi < 126 ||
        !plausible(data) || !plausible(charloc) || modulo > 512)
        return -1;
    for (int ch = 32; ch < 256; ch++) {
        if (ch > hi) {                  /* font ends early: '?' fill */
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

static void fb_init(void)
{
    memset(FB, 0, COLS * FBLINES);
    fb_off = 0;
    volatile u16 *cop = COPLIST;
    *cop++ = 0x00E0; *cop++ = (u16)((u32)FB >> 16);     /* BPL1PTH */
    *cop++ = 0x00E2; *cop++ = (u16)((u32)FB & 0xFFFF);  /* BPL1PTL */
    *cop++ = 0xFFFF; *cop++ = 0xFFFE;                   /* end */

    BPLCON0 = 0x9200;           /* HIRES, 1 bitplane, color burst */
    BPLCON1 = 0;
    BPLCON2 = 0;
    BPL1MOD = 0;
    DDFSTRT = 0x3C;
    DDFSTOP = 0xD4;
    DIWSTRT = 0x2C81;
    DIWSTOP = 0x2CC1;
    COLOR00 = 0x0000;
    COLOR01 = 0x0CCC;
    COP1LCH = (u16)(0x1F000 >> 16);
    COP1LCL = (u16)(0x1F000 & 0xFFFF);
    COPJMP1 = 0;
    DMACON = DMAF_SETCLR | DMAF_MASTER | DMAF_BPLEN | DMAF_COPEN;
}

/* scanline address helper: y is relative to the visible window top */
static u8 *fb_line(int y)
{
    return FB + (u32)(fb_off + y) * COLS;
}

static void fb_scroll(void)
{
    if (fb_off == ROWS * 8) {           /* window at buffer bottom: */
        memcpy(FB, fb_line(0), COLS * ROWS * 8);    /* jump back */
        fb_off = 0;
    }
    fb_off += 8;
    memset(fb_line((ROWS - 1) * 8), 0, COLS * 8);   /* fresh bottom row */
    COPLIST[3] = (u16)(((u32)FB + (u32)fb_off * COLS) & 0xFFFF);
}

static void fb_putc(char c)
{
    if (c == '\r') {
        cx = 0;
        return;
    }
    if (c == '\n') {
        cx = 0;
        if (++cy == ROWS) {
            cy--;
            fb_scroll();
        }
        return;
    }
    if (c == '\b') {
        if (cx) {
            cx--;
            for (int y = 0; y < 8; y++)
                fb_line(cy * 8 + y)[cx] = 0;
        }
        return;
    }
    if (c == '\t') {
        do
            fb_putc(' ');
        while (cx & 7);
        return;
    }
    u8 gc = (u8)c;
    if (gc < 32 || gc == 127)
        gc = '?';                       /* Latin-1 128-255 renders */
    const u8 *g = glyphs[gc - 32];
    for (int y = 0; y < 8; y++)
        fb_line(cy * 8 + y)[cx] = g[y];
    if (++cx == COLS)
        fb_putc('\n');
}

void con_putc(char c)
{
    serial_putc(c);             /* mirror: scripted tests + debug */
    if (fb_up)
        fb_putc(c);
}

/* Walk GfxBase->TextFonts for an 8x8 font (Topaz 8 lives there even
 * when the system default is Topaz 9, as on the A4000), falling back
 * to DefaultFont. The list nodes are in RAM the OS left behind —
 * still untouched this early - the glyph strike itself is in ROM. */
void con_init(u32 gfxbase)
{
    u32 got = 0;
    if (plausible(gfxbase)) {
        u32 node = *(u32 *)(gfxbase + GB_TEXTFONTS);    /* lh_Head */
        for (int i = 0; i < 16 && plausible(node); i++) {
            if (*(u32 *)node == 0)      /* ln_Succ == 0: list tail */
                break;
            if (font_extract(node) == 0) {
                got = node;
                break;
            }
            node = *(u32 *)node;
        }
        if (!got && font_extract(*(u32 *)(gfxbase + GB_DEFAULTFONT)) == 0)
            got = *(u32 *)(gfxbase + GB_DEFAULTFONT);
    }
    if (!got) {
        kprintf("console: no usable 8x8 ROM font - serial only\n");
        return;
    }
    fb_init();
    fb_up = 1;
    kprintf("console: 640x256, ROM Topaz 8 from %p\n", got);
}
