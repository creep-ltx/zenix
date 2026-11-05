/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* A kept start-up partition (DESIGN D35 3b): Zenix's files added to an AmigaDOS FFS volume that stays as it is, through
 * zpkg's boot partition writer (user/zpkg/bootfs.c, its whole walk first, its safe order). zenixboot in the root (a
 * different one replaced), Zenix/ made if missing, Zenix/zenix written, zenixboot's old state and updates deleted.
 * Nothing else on the volume is written: no boot menu, no Startup-Sequence, no other file. No system calls. */
#include "../zpkg/zpkg.h"

struct bk_look {
    int zb_there, zb_same;              /* zenixboot in the root; the same bytes as the new one */
    int dir_there, kern_there;          /* Zenix/, Zenix/zenix */
    int starts_zb;                      /* S/Startup-Sequence or S/BootMenu.conf names zenixboot */
    int nold;                           /* zenixboot's old files in Zenix/ that go (state, zenix.old, updates...) */
    char old[200];                      /* their names, for the plan */
    zp_u32 nfree, need;                 /* blocks free; blocks the files need (after the old ones are given back) */
};
/* the volume walked and looked at (read only): 0, or a refusal with *why */
int bk_look(const struct zp_ffs *f, zp_u32 zbsize, zp_src zbsrc, void *zbu, zp_u32 ksize, struct bk_look *k, const char **why);
/* the writes, then the whole volume walked again and both files compared with their sources: 0, or a refusal */
int bk_write(const struct zp_ffs *f, zp_u32 zbsize, zp_src zbsrc, void *zbu, zp_u32 ksize, zp_src ksrc, void *ku,
             const zp_u32 date[3], const char **why);
