/* SPDX-License-Identifier: GPL-2.0-only */
OUTPUT_ARCH(m68k)
ENTRY(_start)
SECTIONS
{
    /* ZEXE: linked at 0, relocated by the loader (see tools/mkzexe.py) */
    . = 0;
    .text : {
        KEEP(*(.text.start))
        *(.text*)
    }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    /* DESIGN D45 (docs/d45-boot-memory.md): the spans given back to the pool after the boot. Each starts with 32 bytes
     * for the pool's block header; tools/check-init.py checks that nothing outside reaches into them. The boot's: its
     * code, stage0 (called on the loaded file under AmigaOS, found by the ZEXE header's +28), then the embedded boot
     * program as data. The CD's (`.zhwcd`): kept only when a CD drive answered at boot. Memory
     * protection's (`.zhwmmu`, section 13): kept only while it is on. */
    .zinit ALIGN(16) : {
        __zinit_start = .;
        . += 32;
        *(.zinit.text*)
        KEEP(*(.zinit.stage0))
    }
    .zinitd : {
        *(.zinit.rodata*)
        . = ALIGN(16);
        __zinit_end = .;
    }
    .zhwcd ALIGN(16) : {
        __zhwcd_start = .;
        . += 32;
        *(.zhwcd.text*)
        . = ALIGN(16);
        __zhwcd_end = .;
    }
    .zhwmmu ALIGN(16) : {
        __zhwmmu_start = .;
        . += 32;
        *(.zhwmmu.text*)
    }
    .zhwmmud : {
        *(.zhwmmu.rodata*)
        . = ALIGN(16);
        __zhwmmu_end = .;
    }
    . = ALIGN(4);
    __bss_start = .;
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(4);
    __bss_end = .;
    _end = .;
    /DISCARD/ : { *(.comment*) *(.note*) }
}
