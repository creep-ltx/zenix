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
    .data : { *(.data*) }
    . = ALIGN(4);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(4);
    __bss_end = .;
    /DISCARD/ : { *(.comment*) *(.note*) }
}
