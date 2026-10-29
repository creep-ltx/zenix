/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "randseed.h"
#include "clockfile.h"
#include "builddate.h"
#include "../crypto/zc/zc_clock.h"
/* reboot [amigaos|zenix]: save the disks and reset the Amiga (root only). With a system named - or started as
 * "bootamigaos" - the boot menu (boot/bootmenu.c) starts that system at once instead of showing the menu. First a
 * new random seed goes into /var/lib/random-seed (DESIGN D19 9.3, randseed.h): what this session gathered is in the
 * next start's pool; and a trusted clock (the battery clock or sntp's guarded answer this boot) raises the
 * certificate time's floor in /var/lib/clock (DESIGN D21 8.3; a hand-set time never does). */
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
        dprintf(2, "usage: reboot [amigaos|zenix]   (bootamigaos = reboot amigaos)\n");
        return 2;
    }
    if (argc == 2) {
        if (!strcmp(argv[1], "amigaos"))
            note = BOOTNOTE_AMIGAOS;
        else if (!strcmp(argv[1], "zenix"))
            note = BOOTNOTE_ZENIX;
        else {
            dprintf(2, "reboot: %s: not a system (amigaos or zenix)\n", argv[1]);
            return 2;
        }
    }
    if (sys_geteuid() == 0) {
        seed_store(seed_pool_ready() ? SEED_F_SEEDED : 0, 0);     /* a read-only root: nothing written, no harm */
        int b = sys_clockbits();
        uint32_t now = sys_time(), file;
        uint32_t floor = zc_clock_floor(ZENIX_BUILD_DATE, clockfile_read(&file) == 0, file);   /* as sntp and wget */
        if (b > 0 && clockfile_rises(b & CLOCK_RTC, b & CLOCK_NET, 1, floor, now))
            clockfile_write(now);                                   /* the floor only rises; a failure is no harm */
    }
    int r = sys_reboot(note);
    const char *t = zstrerror(r);
    dprintf(2, "reboot: %s\n", r == -EPERM ? "only root may reboot" : t ? t : "failed");
    return 1;
}
