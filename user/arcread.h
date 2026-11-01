// SPDX-License-Identifier: GPL-2.0-only
/* The LhA and LZX archive readers (DESIGN D33, docs/d33-archives.md): unlha and unlzx (user/unlha.c) and Ahiru's arcfs
 * (ports/ahiru) share them through the interface of ports/ahiru/upstream/core/arc.h. Written from the formats, not from
 * any other program: where each fact came from is in the design note. An archive is untrusted: every length is checked,
 * every loop ends with the input, an error is a return value. */
#ifndef ZENIX_ARCREAD_H
#define ZENIX_ARCREAD_H
#include "arc.h"

/* What arcent.method says. An LhA member: its method's number (-lh0- 0, -lh1- 1, -lh4- to -lh7- 4 to 7); an LZX member: 0
 * stored, 2 normal. Both: */
enum { AM_DIR = 8,          /* a directory (no data) */
       AM_OTHER = 9,        /* a method this reader does not unpack: extract says ARC_EUNSUP */
       AM_REFUSED = 10 };   /* a member that may not be written (a name leading out, a link, a name too long) */

/* A member with AM_REFUSED: in the default strict mode the list call stops with ARC_ECORRUPT, so a program opening an
 * archive as a folder (Ahiru) never sees it. A command that reports member by member sets this to 1 and gets it listed. */
extern int arc_list_refused;

int lha_list(const struct arcsrc *src, arc_addfn add, void *ctx);
int lha_extract(const struct arcsrc *src, const struct arcent *e, const struct arcsink *out);
int lzx_list(const struct arcsrc *src, arc_addfn add, void *ctx);
int lzx_extract(const struct arcsrc *src, const struct arcent *e, const struct arcsink *out);
void lzx_end(void);         /* the decoder kept between the members of a merged group goes back */

/* ---- shared by the two readers (user/arcread.c) ---- */
typedef unsigned char arc_u8;
typedef unsigned short arc_u16;
typedef unsigned int arc_u32;

/* A canonical Huffman decoder: codes up to 16 bits, shorter codes first, in symbol order. The first `kbits` bits go
 * through a table; a longer code is found from the counts. The caller gives the arrays. */
struct hf {
    arc_u16 *tab;           /* 1 << kbits entries: (length << 12) | symbol, 0 = a longer code or none */
    arc_u16 *syms;          /* the symbols by code */
    arc_u32 first[17];      /* the first code of each length */
    arc_u16 off[17], cnt[17];
    int kbits;
    int single;             /* >= 0: every code is this symbol, in no bits (LhA's one-symbol tables) */
    int rev;                /* the stream gives a code's first bit in the low bit of what it peeks (LZX) */
};
/* from lens[n] (0 = not used; every length at most 16). 0, or ARC_ECORRUPT for lengths no code can have */
int hf_build(struct hf *h, const arc_u8 *lens, int n);
/* the symbol of the code in the top bits of the 16-bit `v`, longer than kbits; *len is its length; -1: no such code */
int hf_long(const struct hf *h, unsigned v, int *len);
unsigned arc_rev16(unsigned v);

/* A member's name for Zenix: in[n] as stored, '/' between parts already: valid UTF-8 stays, anything else is Latin-1 (the
 * Amiga's), control bytes become '_', empty parts and "." go. Returns 0, or 1 when it may not be written: it starts with
 * '/', it has a ".." part, it does not fit out[cap] or nothing is left of it. `out` always ends in a 0. */
int arc_fixname(const unsigned char *in, int n, char *out, int cap);
#endif
