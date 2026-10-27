/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "randseed.h"
/* reboot [amigaos|zenix]: save the disks and reset the Amiga (root only). With a system named - or started as
 * "bootamigaos" - the boot menu (boot/bootmenu.c) starts that system at once instead of showing the menu. First a
 * new random seed goes into /var/lib/random-seed (DESIGN D19 9.3, randseed.h): what this session gathered is in the
 * next start's pool. */
int main(int argc, char **argv)
{
    const char *me = argv[0], *p;
    for (p = argv[0]; *p; p++)
        if (*p == '/')
            me = p + 1;
    u32 note = 0;
    if (!strcmp(me, "bootamigaos"))
        note = BOOTNOTE_AMIGAOS;
    if (argc > 2 || (argc == 2 && note)) {
        fprintf(2, "usage: reboot [amigaos|zenix]   (bootamigaos = reboot amigaos)\n");
        return 2;
    }
    if (argc == 2) {
        if (!strcmp(argv[1], "amigaos"))
            note = BOOTNOTE_AMIGAOS;
        else if (!strcmp(argv[1], "zenix"))
            note = BOOTNOTE_ZENIX;
        else {
            fprintf(2, "reboot: %s: not a system (amigaos or zenix)\n", argv[1]);
            return 2;
        }
    }
    if (sys_geteuid() == 0)
        seed_store(seed_pool_ready() ? SEED_F_SEEDED : 0, 0);     /* a read-only root: nothing written, no harm */
    int r = sys_reboot(note);
    const char *t = zstrerror(r);
    fprintf(2, "reboot: %s\n", r == -EPERM ? "only root may reboot" : t ? t : "failed");
    return 1;
}
