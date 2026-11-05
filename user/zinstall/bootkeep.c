/* SPDX-License-Identifier: GPL-2.0-only */
/* DESIGN D35 3b: see bootkeep.h */
#ifdef ZHOST
#include <string.h>
#include <stdlib.h>
#else
#include "../libz.h"
#endif
#include "bootkeep.h"

/* zenixboot's own files of an older system (docs/man/zenixboot.txt): gone, so nothing of it is started, rolled back
 * to or installed over the new one */
static const char *const oldf[] = { "state", "state.new", "zenix.new", "zenix.old", "zenix.rejected", "zenix-update.zup",
                                    "zenix-update.zup.sig", "zenix-update.zup.part", "zenix-update.zup.sig.part",
                                    "zenix-update.bad", "zenix-update.bad.sig", "zenix-update.done", "zenix-update.done.sig",
                                    "zenix-update.rejected", "zenix-update.rejected.sig" };
#define NOLD (int)(sizeof oldf / sizeof oldf[0])

static unsigned char cbuf[2][512];

/* the file in v's directory the same as src's size bytes? 1 yes, 0 no, <0 a read error */
static int same(struct zp_bootvol *v, const struct zp_bootfile *bf, zp_u32 size, zp_src src, void *u)
{
    if (bf->size != size) return 0;
    for (zp_u32 off = 0; off < size; off += 512) {
        zp_u32 n = size - off < 512 ? size - off : 512;
        if (zp_boot_read(v, bf, off, cbuf[0], n) || src(u, off, cbuf[1], n) < 0) return -1;
        if (memcmp(cbuf[0], cbuf[1], n)) return 0;
    }
    return 1;
}

/* the text of S/NAME names zenixboot (any case) */
static int names_zb(struct zp_bootvol *v, const char *name)
{
    struct zp_bootfile bf;
    static char t[2049];
    int hit = 0;
    if (zp_boot_find(v, name, &bf, 0)) return 0;
    zp_u32 n = bf.size < 2048 ? bf.size : 2048;
    if (!zp_boot_read(v, &bf, 0, (unsigned char *)t, n)) {
        t[n] = 0;
        for (char *p = t; *p; p++) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        hit = strstr(t, "zenixboot") != 0;
    }
    zp_boot_file_free(&bf);
    return hit;
}

int bk_look(const struct zp_ffs *f, zp_u32 zbsize, zp_src zbsrc, void *zbu, zp_u32 ksize, struct bk_look *k, const char **why)
{
    struct zp_bootvol v;
    struct zp_bootfile bf;
    zp_u32 freed = 0;
    int rc;
    memset(k, 0, sizeof *k);
    if ((rc = zp_boot_open(f, "Zenix", &v, why)) != 0) return rc;
    k->nfree = v.nfree + v.nleak;
    k->dir_there = v.dir != 0;
    if (v.dir) {
        if (zp_boot_find(&v, "zenix", &bf, 0) == 0) { k->kern_there = 1; zp_boot_file_free(&bf); }
        for (int i = 0; i < NOLD; i++) {
            rc = zp_boot_find(&v, oldf[i], &bf, why);
            if (rc == ZP_ENOENT) continue;
            if (rc) goto out;                          /* a directory or a link of that name: not touched, refused */
            freed += zp_boot_blocks(bf.size);
            zp_boot_file_free(&bf);
            if (strlen(k->old) + strlen(oldf[i]) + 3 < sizeof k->old) {
                if (k->nold) strcat(k->old, ", ");
                strcat(k->old, oldf[i]);
            }
            k->nold++;
        }
    }
    zp_boot_cd(&v, "", 0);
    rc = zp_boot_find(&v, "zenixboot", &bf, why);
    if (rc && rc != ZP_ENOENT) goto out;
    if (!rc) {
        k->zb_there = 1;
        rc = same(&v, &bf, zbsize, zbsrc, zbu);
        zp_boot_file_free(&bf);
        if (rc < 0) { *why = "a read error on the start-up partition"; goto out; }
        k->zb_same = rc;
    }
    if (zp_boot_cd(&v, "S", 0) == 0) k->starts_zb = names_zb(&v, "Startup-Sequence") || names_zb(&v, "BootMenu.conf");
    k->need = (k->zb_same ? 0 : zp_boot_blocks(zbsize)) + zp_boot_blocks(ksize) + (k->dir_there ? 0 : 1) + 16;
    rc = 0;
    if (k->nfree + freed < k->need) { *why = "not enough free space on it"; rc = ZP_ENOSPC; }
out:
    zp_boot_close(&v);
    return rc;
}

int bk_write(const struct zp_ffs *f, zp_u32 zbsize, zp_src zbsrc, void *zbu, zp_u32 ksize, zp_src ksrc, void *ku,
             const zp_u32 date[3], const char **why)
{
    struct zp_bootvol v;
    struct zp_bootfile bf;
    int rc;
    if ((rc = zp_boot_open(f, "Zenix", &v, why)) != 0) return rc;
    /* the folder; the old system's state and updates first, then zenixboot, the kernel last */
    if (!v.dir && (rc = zp_boot_mkdir(&v, "Zenix", date, why)) != 0) goto out;
    for (int i = 0; i < NOLD; i++)
        if ((rc = zp_boot_delete(&v, oldf[i], why)) != 0 && rc != ZP_ENOENT) goto out;
    zp_boot_cd(&v, "", 0);
    rc = zp_boot_find(&v, "zenixboot", &bf, why);
    int keep = rc == 0 && same(&v, &bf, zbsize, zbsrc, zbu) == 1;
    if (!rc) zp_boot_file_free(&bf);
    if (!keep && (rc = zp_boot_replace(&v, "zenixboot", zbsize, zbsrc, zbu, date, why)) != 0) goto out;
    if ((rc = zp_boot_cd(&v, "Zenix", why)) != 0 || (rc = zp_boot_replace(&v, "zenix", ksize, ksrc, ku, date, why)) != 0) goto out;
    zp_boot_close(&v);
    /* read back: the whole volume walked again, both files as their sources */
    struct zp_ffs ro = *f;
    ro.write = 0;
    if ((rc = zp_boot_open(&ro, "Zenix", &v, why)) != 0) return rc;
    rc = zp_boot_find(&v, "zenix", &bf, why);
    if (!rc) { rc = same(&v, &bf, ksize, ksrc, ku) == 1 ? 0 : ZP_EIO; zp_boot_file_free(&bf); }
    if (!rc) {
        zp_boot_cd(&v, "", 0);
        rc = zp_boot_find(&v, "zenixboot", &bf, why);
        if (!rc) { rc = same(&v, &bf, zbsize, zbsrc, zbu) == 1 ? 0 : ZP_EIO; zp_boot_file_free(&bf); }
    }
    if (rc == ZP_EIO) *why = "the files do not read back as they were written";
out:
    zp_boot_close(&v);
    return rc;
}
