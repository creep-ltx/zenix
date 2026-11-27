/* SPDX-License-Identifier: GPL-2.0-only */
/* The Raspberry Pi's hardware video scaler (HVS) as Zenix's own graphics on the PiStorm (DESIGN D63,
 * docs/d63-pistorm-gfx.md): the Pi in the hardware table, the monitor's EDID, the mode list, the picture's place and
 * scaling, the display list's words, the register access. No system calls and no libz: displayd, pidisplay and the
 * PC's test (tools/hvs-host-test.c) build the same file. The words are the BCM2711's (the Pi 4 and CM4), as
 * VideoCore.card's vc6.c and Linux's vc4 driver write them. */
#pragma once

/* the HVS's window (bus 0x7E400000: the hardware table's soc_base + HVS_OFF) */
#define HVS_OFF         0x400000u
#define HVS_DISPLIST1   0x24u           /* the HDMI's channel: the word offset of the list it shows */
#define HVS_DISPSTAT1   0x58u           /* its line (bits 11-0) and frame count (bits 17-12) */
#define HVS_DLIST       0x4000u         /* the list memory: 4,096 words */
#define HVS_DLIST_WORDS 4096u
#define HVS_WINDOW      0x8000u         /* the registers and the list memory: what --pretend's window holds */
/* Zenix's words of the list memory (d63 4): clear of the firmware's (0-31) and of what AmigaOS left (0-0x3FF) */
#define HVS_DL_BLANK    0x800u          /* the end word alone: the background */
#define HVS_DL_KERNEL   0x808u          /* the nearest-neighbour kernel, 11 words */
#define HVS_DL_A        0x820u          /* list A, at most 32 words */
#define HVS_DL_B        0x840u          /* list B */
#define HVS_DL_PAL      0x900u          /* an 8-bit picture's palette, 256 words 0xFFRRGGBB */
#define HVS_DL_END      0x80000000u
#define HVS_VC4_SPARE   4096u           /* left free at vc4-mem's end (room for the pointer's picture later, d63 13) */

/* Picasso96's RGBFormat numbers of the three depths (fb.c's table; d63 5) */
#define HVS_FMT8        1               /* CLUT */
#define HVS_FMT16       4               /* R5G6B5PC */
#define HVS_FMT32       9               /* B8G8R8A8 */

#define HVS_MAXMODES    32
#define HVS_MINW        320
#define HVS_MINH        256

struct hvsmode {
    unsigned short w, h;
    unsigned char bits;                 /* HVSB_8 1 | HVSB_16 2 | HVSB_32 4: the depths that fit the memory */
    unsigned char flags;                /* HVSM_NATIVE 1 (the output's size), HVSM_PREF 2 (the monitor's preferred) */
};
#define HVSB_8   1
#define HVSB_16  2
#define HVSB_32  4
#define HVSM_NATIVE 1
#define HVSM_PREF   2

#define HVS_EDIDMAX 24
struct hvsedid {
    char name[14];                      /* the monitor's name ("" none) */
    char maker[4];                      /* the manufacturer's three letters */
    int n;                              /* sizes found */
    unsigned short w[HVS_EDIDMAX], h[HVS_EDIDMAX];
    int pref;                           /* the index of the preferred size, -1 none */
};
/* one EDID block (128 bytes) -> 0, or -1 (a bad header or checksum: e left empty) */
int hvs_edid(const unsigned char *b, unsigned n, struct hvsedid *e);

/* the modes for an output of ow x oh and vc4 bytes of picture memory (its last 4 KB left free), with the EDID's
 * sizes (e may be 0) -> how many (at most max), sorted by area, then width */
int hvs_modes(unsigned ow, unsigned oh, unsigned vc4, const struct hvsedid *e, struct hvsmode *m, int max);

/* bytes a row of a picture w pixels wide at bits (8, 16, 32): rounded up to 16 */
unsigned hvs_bpr(unsigned w, unsigned bits);
/* the format number for bits, 0 for none */
int hvs_format(unsigned bits);
/* 1 if w x h x bits is a picture the driver makes on that output with that memory */
int hvs_size_ok(unsigned w, unsigned h, unsigned bits, unsigned ow, unsigned oh, unsigned vc4);

/* where the picture goes on the output: one factor for both axes (16.16, source over output), centred */
struct hvsgeo { unsigned scale, dw, dh, x, y, unity; };
void hvs_fit(unsigned w, unsigned h, unsigned ow, unsigned oh, struct hvsgeo *g);

/* the list's words (in the CPU's order; the driver writes them little-endian) for a picture at addr (the 68k address,
 * which is the ARM's: under 1 GB) -> how many, the end word included (at most 32) */
struct hvsplane { unsigned w, h, bits, bpr, addr, ow, oh; };
int hvs_list(const struct hvsplane *p, unsigned *out);
/* the nearest-neighbour kernel's 11 words */
void hvs_kernel(unsigned out[11]);
/* a 12-bit colour 0x0RGB as a palette word */
unsigned hvs_argb(unsigned rgb12);

/* "WxHxBITS" -> 0 (bits 8, 16, 32; *bits 0 when left out and none is allowed), -1 */
int hvs_parse_size(const char *s, unsigned *w, unsigned *h, unsigned *bits);

/* ---- the Pi (hvslist.c touches the hardware only through these, a word at a time, little-endian) ---- */
#define HVS_CON_MAGIC   0x5A434F4Eu     /* "ZCON" at list word 0x801: word 0x802 is the console's list (d63 8) */
#define HVS_DL_CONMAGIC 0x801u
#define HVS_DL_CON      0x802u
struct hvspi {
    unsigned long hvs;                  /* the HVS's window as the 68k sees it (long: the PC's test) */
    unsigned long soc;                       /* the peripherals (the 1 MHz timer at +0x3004); 0: a pretend Pi */
    unsigned long vc4;                  /* the picture memory */
    unsigned vc4size;
    unsigned ow, oh;                    /* the output */
    const unsigned char *edid;          /* the monitor's EDID block, 0 none */
};
/* the Pi from the hardware table and the kernel's record after it (kernel/hwinfo.h) -> 0, or the reason it is none */
const char *hvs_find(const void *hwinfo, const void *hwdisp, struct hvspi *p);
unsigned hvs_rd(unsigned long addr);
void hvs_wr(unsigned long addr, unsigned v);
static inline unsigned long hvs_dl(const struct hvspi *p, unsigned word) { return p->hvs + HVS_DLIST + 4 * word; }
/* the channel runs (DISPSTAT1's mode RUN or EOF) */
int hvs_running(const struct hvspi *p);
/* the frame counter (DISPSTAT1 bits 17-12) */
static inline unsigned hvs_frame(const struct hvspi *p) { return hvs_rd(p->hvs + HVS_DISPSTAT1) >> 12 & 63; }
/* the kernel, the blank list and the list for pl into word at, every word read back -> 0, or -1 (the memory did not
 * keep them) */
int hvs_put(const struct hvspi *p, const struct hvsplane *pl, unsigned at);
/* the console's list: the one stored at 0x801-0x802 (0: none); hvs_con_keep stores v unless it is 0 or Zenix's own */
unsigned hvs_con(const struct hvspi *p);
void hvs_con_keep(const struct hvspi *p, unsigned v);
static inline int hvs_ours(unsigned v) { return v >= HVS_DL_BLANK && v < HVS_DL_PAL + 256; }
