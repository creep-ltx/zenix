/* SPDX-License-Identifier: GPL-2.0-only */
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
    .data : {
        /* the C library's constructors and destructors (DESIGN D27 5.3: picolibc's atexit functions run from
         * .fini_array_onexit, last in the table, so first at exit); PROVIDE
         * defines a symbol only when something refers to it, and no libz program has these sections */
        PROVIDE(__bothinit_array_start = .); PROVIDE(__init_array_start = .);
        KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array))
        PROVIDE(__init_array_end = .); PROVIDE(__bothinit_array_end = .);
        PROVIDE(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array)) KEEP(*(.fini_array*))   /* the last: .fini_array_onexit */
        PROVIDE(__fini_array_end = .);
        *(.data*)
    }
    . = ALIGN(4);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(4);
    __bss_end = .;
    /DISCARD/ : { *(.comment*) *(.note*) }
}
