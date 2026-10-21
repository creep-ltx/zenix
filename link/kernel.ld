OUTPUT_ARCH(m68k)
ENTRY(_start)
SECTIONS
{
    . = 0x80000000;
    .text : {
        KEEP(*(.text.start))
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    . = ALIGN(4096);
    __ro_end = .;
    .data : { *(.data*) }
    . = ALIGN(4);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(4);
    __bss_end = .;
    _end = .;
    /DISCARD/ : { *(.comment*) *(.note*) }
}
