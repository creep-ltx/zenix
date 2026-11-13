/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *  m68k ELF relocations for TCC (Zenix: 68020+, gcc's m68k-elf rules)
 *
 *  Copyright (c) 2026 the Zenix authors
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  Zenix links tcc's objects with its own linker, zld; this file gives
 *  tcc's ELF code what it needs to write objects (and to relocate in
 *  memory): absolute and PC-relative relocations of 8, 16 and 32 bits,
 *  stored big-endian. No GOT, no PLT, no shared libraries.
 */

#ifdef TARGET_DEFS_ONLY

#define EM_TCC_TARGET EM_68K

/* relocation type for 32 bit data relocation */
#define R_DATA_32   R_68K_32
#define R_DATA_PTR  R_68K_32
#define R_JMP_SLOT  R_68K_JMP_SLOT
#define R_GLOB_DAT  R_68K_GLOB_DAT
#define R_COPY      R_68K_COPY
#define R_RELATIVE  R_68K_RELATIVE

#define R_NUM       R_68K_NUM

#define ELF_START_ADDR 0x00000000
#define ELF_PAGE_SIZE  0x1000

#define PCRELATIVE_DLLPLT 0
#define RELOCATE_DLLPLT 0

#else /* !TARGET_DEFS_ONLY */

#include "tcc.h"

#ifdef NEED_RELOC_TYPE
/* Returns 1 for a code relocation, 0 for a data relocation. For unknown
   relocations, returns -1. */
ST_FUNC int code_reloc (int reloc_type)
{
    switch (reloc_type) {
    case R_68K_32:
    case R_68K_16:
    case R_68K_8:
    case R_68K_GLOB_DAT:
    case R_68K_COPY:
    case R_68K_RELATIVE:
    case R_68K_NONE:
        return 0;
    case R_68K_PC32:
    case R_68K_PC16:
    case R_68K_PC8:
    case R_68K_PLT32:
    case R_68K_JMP_SLOT:
        return 1;
    }
    return -1;
}

/* Whether and when the relocation needs a GOT and/or PLT entry: never, as
   Zenix programs are linked statically. */
ST_FUNC int gotplt_entry_type (int reloc_type)
{
    switch (reloc_type) {
    case R_68K_NONE:
    case R_68K_32:
    case R_68K_16:
    case R_68K_8:
    case R_68K_PC32:
    case R_68K_PC16:
    case R_68K_PC8:
    case R_68K_GLOB_DAT:
    case R_68K_JMP_SLOT:
    case R_68K_COPY:
    case R_68K_RELATIVE:
        return NO_GOTPLT_ENTRY;
    }
    return -1;
}

#ifdef NEED_BUILD_GOT
ST_FUNC unsigned create_plt_entry(TCCState *s1, unsigned got_offset, struct sym_attr *attr)
{
    tcc_error_noabort("m68k: no PLT (no shared libraries)");
    return 0;
}

ST_FUNC void relocate_plt(TCCState *s1)
{
}
#endif
#endif

static void put_be16(unsigned char *p, unsigned v)
{
    p[0] = v >> 8, p[1] = v;
}

static void put_be32(unsigned char *p, unsigned v)
{
    p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v;
}

ST_FUNC void relocate(TCCState *s1, ElfW_Rel *rel, int type, unsigned char *ptr, addr_t addr, addr_t val)
{
    int32_t x;

    switch (type) {
    case R_68K_32:
        put_be32(ptr, val);
        return;
    case R_68K_16:
        put_be16(ptr, val);
        return;
    case R_68K_8:
        *ptr = val;
        return;
    case R_68K_PC32:
    case R_68K_PLT32:
        put_be32(ptr, val - addr);
        return;
    case R_68K_PC16:
        x = val - addr;
        if (x != (int16_t)x)
            tcc_error_noabort("m68k: 16-bit PC-relative relocation out of range");
        put_be16(ptr, x);
        return;
    case R_68K_PC8:
        x = val - addr;
        if (x != (int8_t)x)
            tcc_error_noabort("m68k: 8-bit PC-relative relocation out of range");
        *ptr = x;
        return;
    case R_68K_NONE:
    case R_68K_COPY:
    case R_68K_GLOB_DAT:
    case R_68K_JMP_SLOT:
    case R_68K_RELATIVE:
        return;
    default:
        tcc_error_noabort("m68k: relocation type %d not supported", type);
        return;
    }
}

#endif /* !TARGET_DEFS_ONLY */
