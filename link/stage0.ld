/* SPDX-License-Identifier: GPL-2.0-only */
/* stage0 is a position independent blob: entry first, no data, no relocations (see Makefile). */
OUTPUT_ARCH(m68k)
ENTRY(stage0_entry)
SECTIONS
{
    . = 0;
    .text : { *(.text.entry) *(.text*) *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    /DISCARD/ : { *(.comment*) *(.note*) *(.eh_frame*) }
}
ASSERT(SIZEOF(.data) == 0 && SIZEOF(.bss) == 0, "stage0 must not have writable data")
