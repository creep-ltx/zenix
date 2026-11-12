/* SPDX-License-Identifier: GPL-2.0-only */
/* Programs built into the kernel image (ZEXE blobs with their own relocations), found through -I$(BUILD).
 * The ZenixFS server is not built in (DESIGN D19 step 1): mount starts /sbin/zfsd as a file service. Only the
 * kernel for a ZenixFS root, kernel-zfs.bin (KERNEL_ZFSD, DESIGN D34), carries it. */
    .section .rodata
    .balign 4
    .globl  fsd_blob, fsd_blob_end, boot_blob, boot_blob_end, diskd_blob, diskd_blob_end
fsd_blob:
    .incbin "fsd.zx"
fsd_blob_end:
    .balign 4
    .section .zinit.rodata,"a"         /* DESIGN D45: run once (pid 1's copy is made at boot), then given back */
    .balign 4
boot_blob:
#ifdef KERNEL_ZFSD
    .incbin "bootz.zx"               /* boot with the choices for a damaged ZenixFS root (DESIGN D34) */
#else
    .incbin "boot.zx"
#endif
boot_blob_end:
    .section .rodata
    .balign 4
diskd_blob:
    .incbin "diskd.zx"
diskd_blob_end:
#ifdef KERNEL_ZFSD
    .balign 4
    .globl  zfsd_blob, zfsd_blob_end
zfsd_blob:
    .incbin "zfsd.zx"                /* the ZenixFS server (@zfsd): the root's, or a mount's (svc.c) */
zfsd_blob_end:
#endif
