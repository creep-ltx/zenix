#include "kernel.h"

/* ELF32 big-endian m68k ET_EXEC loader. Segments must be page-aligned
 * (link/user.ld guarantees this) and live in the user window. Read-only
 * segments get PD_WP. The kernel is big-endian, so ELF fields read
 * directly. */

struct ehdr {
    u8 ident[16];
    u16 type, machine;
    u32 version, entry, phoff, shoff, flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct phdr {
    u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

#define PT_LOAD 1
#define PF_W 2

int elf_load(u32 *uroot, const u8 *img, u32 len, u32 *entry_out,
             u32 *brk_out)
{
    u32 brk = UVA_BASE;
    const struct ehdr *eh = (const struct ehdr *)img;
    if (len < sizeof(*eh) ||
        img[0] != 0x7F || img[1] != 'E' || img[2] != 'L' || img[3] != 'F' ||
        img[4] != 1 /* 32-bit */ || img[5] != 2 /* big-endian */ ||
        eh->type != 2 /* ET_EXEC */ || eh->machine != 4 /* EM_68K */) {
        kprintf("elf: not a m68k executable\n");
        return -1;
    }
    for (u32 i = 0; i < eh->phnum; i++) {
        const struct phdr *ph =
            (const struct phdr *)(img + eh->phoff + i * eh->phentsize);
        if (ph->type != PT_LOAD || ph->memsz == 0)
            continue;
        if (ph->vaddr < UVA_BASE || ph->vaddr + ph->memsz > USTACK_TOP ||
            (ph->vaddr & (PAGE_SIZE - 1)) ||
            ph->offset + ph->filesz > len) {
            kprintf("elf: bad segment %p+%x\n", ph->vaddr, ph->memsz);
            return -1;
        }
        u32 wp = (ph->flags & PF_W) ? 0 : PD_WP;
        for (u32 off = 0; off < ph->memsz; off += PAGE_SIZE) {
            u32 pa = pmm_alloc();
            if (off < ph->filesz) {
                u32 n = ph->filesz - off;
                if (n > PAGE_SIZE)
                    n = PAGE_SIZE;
                memcpy((void *)pa, img + ph->offset + off, n);
            }
            umem_map(uroot, ph->vaddr + off, pa, wp);
        }
        u32 end = (ph->vaddr + ph->memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (end > brk)
            brk = end;
    }
    *entry_out = eh->entry;
    *brk_out = brk;
    return 0;
}
