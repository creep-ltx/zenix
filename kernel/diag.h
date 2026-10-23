/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* Boot stage colours (audit A11). Until the display is up, the whole screen shows COLOR00 (the bootblock switched
 * copper and bitplane DMA off), and each boot step sets its own colour, so a machine that freezes before any text
 * appears is named by its colour on a phone photo. The table for the owner is in docs/pi-first-boot.md; stages 1-3
 * are set by boot/bootblock.S (same values), 4-5 by kernel/start.S, the rest by kmain.
 *
 * A halt on an error before the display is up (panic, exception in the kernel, a 68000) splits the screen: the
 * colour of the stage reached on top, red from line 150 down (diag_halt in start.S). After the display is up a panic
 * turns the top bar red and leaves its text on the screen. The whole screen red is the bootblock's own failure.
 *
 * Test builds: make DIAG=-DDIAG_STOP=N hangs at stage N, DIAG=-DDIAG_PANIC=N panics there (tools/diag-test.sh). */
#include "kernel.h"

#define DIAG_BLUE       0x00F   /* 1  bootblock: loading the kernel from the floppy (AmigaOS alive) */
#define DIAG_CYAN       0x0FF   /* 2  bootblock: loaded; stage0 reads the Pi tables, exec's memory list */
#define DIAG_MAGENTA    0xF0F   /* 3  bootblock: AmigaOS switched off, the copier moves the kernel */
#define DIAG_YELLOW     0xFF0   /* 4  start.S: the kernel runs, relocating itself */
#define DIAG_ORANGE     0xF80   /* 5  start.S: relocated, caches flushed, BSS cleared */
#define DIAG_GREEN      0x0F0   /* 6  kmain: log and serial port, hardware tables, ROM font */
#define DIAG_BROWN      0x630   /* 7  kmain: exception vectors, memory manager and its self-test */
#define DIAG_PINK       0xF9C   /* 8  kmain: memory ok, instruction cache on */
#define DIAG_WHITE      0xFFF   /* 9  kmain: starting the display */
                                /* 10 display up: black text area, blue bar, the boot log as text */
                                /* 11 boot finished: servers and the first programs started */
#define DIAG_GREY       0x888   /* no display (no ROM font, or no chip RAM): the system goes on, serial only */
#define DIAG_RED        0xF00

#define DIAG_DISPLAY_UP 10
#define DIAG_BOOTED     11

void diag_stage(int n, u16 colour);                     /* main.c: colour 0 = keep the current one */
void diag_halt(u16 top) __attribute__((noreturn));     /* start.S: split screen, top colour over red, forever */
extern u16 diag_colour;                                 /* the colour of the stage reached */
