/* Programs built into the kernel image (ZEXE blobs with their own relocations). */
    .section .rodata
    .balign 4
    .globl  zfsd_blob, zfsd_blob_end, fsd_blob, fsd_blob_end, boot_blob, boot_blob_end, diskd_blob, diskd_blob_end
fsd_blob:
    .incbin "build/fsd.zx"
fsd_blob_end:
    .balign 4
boot_blob:
    .incbin "build/boot.zx"
boot_blob_end:
    .balign 4
diskd_blob:
    .incbin "build/diskd.zx"
diskd_blob_end:
    .balign 4
zfsd_blob:
    .incbin "build/mount.zenixfs.zx"      /* the ZenixFS server: serves the root if the root is ZenixFS */
zfsd_blob_end:
