OUTPUT_ARCH(m68k)
ENTRY(_ustart)
PHDRS
{
    text PT_LOAD FLAGS(5);  /* R+X */
    data PT_LOAD FLAGS(6);  /* R+W */
}
SECTIONS
{
    . = 0x400000;
    .text : {
        *(.text.ustart)
        *(.text*)
        *(.rodata*)
    } :text
    . = ALIGN(4096);
    .data : { *(.data*) } :data
    . = ALIGN(4);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) } :data
    . = ALIGN(4);
    __bss_end = .;
    /DISCARD/ : { *(.comment*) *(.note*) }
}
