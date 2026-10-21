OUTPUT_ARCH(m68k)
ENTRY(_start)
SECTIONS
{
    /* M0: the bootblock puts the kernel at 0x40000 in chip RAM */
    . = 0x40000;
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
    _end = .;
    /DISCARD/ : { *(.comment*) *(.note*) }
}
