/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the frozen first sizes (CLAUDE.md "Frozen interfaces", audit3 B3): a caller passing size 0 (every
 * program built before D15) gets each struct's first size, also when the struct has grown at its end; a size below
 * the first is refused; a bigger one gets at most the kernel's size. The first sizes match the structs' first fields
 * as they were frozen. tools/usize-test.sh also checks that every system call passes the frozen constant. */
#include <stdio.h>
#include <stddef.h>
#include "../kernel/kernel.h"

static int bad;
#define EXPECT(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); bad++; } else printf("ok:   %s\n", msg); } while (0)

static void one(const char *name, u32 first, u32 now)
{
    char m[160];
    snprintf(m, sizeof m, "%s: size 0 gets the first size %u (the struct is %u bytes now)", name, first, now);
    EXPECT(usize_n(0, first, now) == first, m);
    snprintf(m, sizeof m, "%s: grown by 12 bytes, size 0 still gets %u", name, first);
    EXPECT(usize_n(0, first, now + 12) == first, m);
    snprintf(m, sizeof m, "%s: a size below %u is refused", name, first);
    EXPECT(usize_n(first - 1, first, now) == 0 && usize_n(1, first, now) == 0, m);
    snprintf(m, sizeof m, "%s: a bigger size gets at most the kernel's", name);
    EXPECT(usize_n(now + 100, first, now) == now && usize_n(first, first, now + 12) == first &&
           usize_n(first + 4, first, now + 12) == first + 4, m);
}

int main(void)
{
    EXPECT(USTAT_FIRST == 28 && offsetof(struct ustat, links) + 4 == 28, "ustat: first size 28 (ino .. links)");
    EXPECT(UDIRENT_FIRST == 72 && offsetof(struct udirent, name) + 64 == 72, "udirent: first size 72");
    EXPECT(UMOUNTINFO_FIRST == 88 && offsetof(struct umountinfo, type) + 8 == 88, "umountinfo: first size 88");
    EXPECT(UUPDATE_FIRST == 32 && offsetof(struct uupdate, kver) + 16 == 32, "uupdate: first size 32");
    EXPECT(UPROC_FIRST == 32 && offsetof(struct uproc, name) + 16 == 32, "uproc: first size 32");
    EXPECT(USTATFS_FIRST == 20 && UMEMINFO_FIRST == 16, "ustatfs 20, meminfo 16");
    one("ustat", USTAT_FIRST, USTAT_SIZE);             /* D49 8: and the device the kernel writes at 28 */
    EXPECT(USTAT_SIZE == 32 && usize_n(29, USTAT_FIRST, USTAT_SIZE) == 29 && usize_n(100, USTAT_FIRST, USTAT_SIZE) == 32,
           "ustat: 32 with the device, a bigger buffer gets 32");
    one("udirent", UDIRENT_FIRST, sizeof(struct udirent));
    one("umountinfo", UMOUNTINFO_FIRST, sizeof(struct umountinfo));
    one("uupdate", UUPDATE_FIRST, sizeof(struct uupdate));
    one("uproc", UPROC_FIRST, sizeof(struct uproc));
    one("ustatfs", USTATFS_FIRST, 20);
    one("meminfo", UMEMINFO_FIRST, 16);
    printf(bad ? "usize-test: %d FAILED\n" : "usize-test: all passed\n", bad);
    return bad != 0;
}
