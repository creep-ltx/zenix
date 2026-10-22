/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* Which root filesystem server serves the root? Two root servers are built into the kernel (the ext2 fsd and ZenixFS's
 * mount.zenixfs started without arguments) and both run at boot. Both apply this same rule to the same list of
 * devices, so exactly one of them finds that the root is its own and serves it; the other leaves quietly.
 *   1. A RAM disk (ram0, from the boot floppy) is the root. Today it is always ext2.
 *   2. Otherwise the first partition of type ext2 or zenixfs that is marked bootable (fdisk boot ...),
 *   3. else the first partition of those types.
 *   4. No such partition: the bare disk ide0 (whoever finds its own filesystem on it).
 * Needs libz.h, diskproto.h and rdb.h before it. */
struct rootpick { int have_ram; int found; char name[16]; unsigned dostype; };

static void rootpick(int disk_port, struct rootpick *r)
{
    struct umsg m;
    struct diskinfo di;
    int first = 0;
    memset(r, 0, sizeof *r);
    for (u32 i = 1; i < 16; i++) {
        m.type = DISK_LIST; m.a[0] = i; m.a[1] = (u32)&di;
        if (sys_call(disk_port, &m) < 0) break;
        if (!strcmp(di.name, "ram0")) { r->have_ram = 1; continue; }
        if (di.dostype != DT_EXT2 && di.dostype != DT_ZENIXFS) continue;
        if ((di.flags & 1) || !first) {
            strncpy(r->name, di.name, sizeof r->name - 1);
            r->dostype = di.dostype;
            r->found = 1;
            first = 1;
            if (di.flags & 1) return;                    /* a bootable one wins at once */
        }
    }
}
