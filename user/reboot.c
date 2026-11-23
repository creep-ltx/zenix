/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "randseed.h"
#include "clockfile.h"
#include "builddate.h"
#include "../crypto/zc/zc_clock.h"
/* reboot [amigaos|zenix]: save the disks and reset the Amiga (root only). As halt, poweroff or shutdown it saves the
 * disks the same way and then halts with a line saying the Amiga may be switched off (an Amiga cannot switch itself
 * off, so poweroff is halt); shutdown takes -h (the default), -r (reboot) and "now" as Linux's does, no delay. With a system named - or started as
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
    const char *USAGE = !strcmp(me, "shutdown") ? "usage: shutdown [-h | -r] [now]   (no delay: it happens at once)\n"
                      : !strcmp(me, "halt") ? "usage: halt   (saves the disks, then halts: switch the Amiga off)\n"
                      : !strcmp(me, "poweroff") ? "usage: poweroff   (saves the disks, then halts: switch the Amiga off)\n"
                      : "usage: reboot [amigaos|zenix]   (bootamigaos = reboot amigaos)\n";
    zopt(argc, argv, USAGE);
    u32 note = 0;
    if (!strcmp(me, "halt") || !strcmp(me, "poweroff"))
        note = REBOOT_HALT;
    if (!strcmp(me, "shutdown")) {
        note = REBOOT_HALT;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-r"))
                note = 0;
            else if (argv[i][0] == '-' && strcmp(argv[i], "-h") && strcmp(argv[i], "-P")) zbadopt(argv[i], USAGE);
            else if (strcmp(argv[i], "-h") && strcmp(argv[i], "-P") && strcmp(argv[i], "now")) zusage(USAGE);
        }
        argc = 1;
    }
    if (note == REBOOT_HALT && argc > 1) {
        if (argv[1][0] == '-') zbadopt(argv[1], USAGE);
        zusage(USAGE);
    }
    if (!strcmp(me, "bootamigaos"))
        note = BOOTNOTE_AMIGAOS;
    if (argc > 1 && argv[1][0] == '-') zbadopt(argv[1], USAGE);
    if (argc > 2 || (argc == 2 && note)) zusage(USAGE);
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
    dprintf(2, "%s: %s\n", me, r == -EPERM ? "only root may do this (try sudo)" : t ? t : "failed");
    return 1;
}
