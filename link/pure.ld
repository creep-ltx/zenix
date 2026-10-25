/* SPDX-License-Identifier: GPL-2.0-only */
/* A program with shared code (ZEX2, DESIGN D13, tools/mkzexe.py): the shared part - code and constants - first, then
 * from __text_end (16-aligned) the data part every process has its own copy of: the address table first (a5 points
 * at its start; the 16-bit table offsets count from there), .got.plt (12 zero bytes, placed so it is not an orphan
 * between .data and .bss), .data, .bss. */
OUTPUT_ARCH(m68k)
ENTRY(_start)
SECTIONS
{
    . = 0;
    .text : {
        KEEP(*(.text.start))
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    . = ALIGN(16);
    __text_end = .;
    .got : { *(.got) }
    .got.plt : { *(.got.plt) }
    .data : { *(.data*) }
    . = ALIGN(4);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(4);
    __bss_end = .;
    /DISCARD/ : { *(.comment*) *(.note*) *(.eh_frame*) }
}
