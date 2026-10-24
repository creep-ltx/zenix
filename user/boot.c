/* SPDX-License-Identifier: GPL-2.0-only */
#pragma GCC optimize("Os")              /* embedded in the kernel image: size first */
#include "libz.h"
#include "../kernel/diskproto.h"
#include "rdb.h"
#include "rootpick.h"
#include "volid.h"
/* Built into the kernel image: the starter, pid 1 (DESIGN D11). It registers the disk server and the root filesystem
 * server as services (the kernel starts a service when it is first called, and again after it died); its own first
 * calls start them: rootpick's DISK_LIST starts diskd, the root inode (SYS_mountinfo) starts the root server. Then it
 * starts /bin/init on console 1 and waits for it; should init ever end, it says so and starts it again. */

static int dport;
static void say(const char *s) { sys_write(1, s, strlen(s)); }   /* no printf: it would double boot's size */
static char *hex(char *p, const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++) {
        *p++ = "0123456789abcdef"[b[i] >> 4];
        *p++ = "0123456789abcdef"[b[i] & 15];
    }
    return p;
}
static char *hex32(char *p, u32 v)
{
    unsigned char b[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };
    return hex(p, b, 4);
}

/* Which server serves DEV (its superblock: volid.h, so a bare disk needs no server started on trial), and its
 * volume id "START,SIZE,UUID" in hex. 1 ext2, 2 ZenixFS, 0 neither. */
static int look(const char *dev, char *id)
{
    struct diskinfo di;
    unsigned char uuid[16];
    int type = vol_look(dport, dev, &di, uuid);
    *id = 0;
    if (type >= 0) {
        char *p = hex32(id, di.start);
        *p++ = ',';
        p = hex32(p, di.sectors);
        *p++ = ',';
        *hex(p, uuid, 16) = 0;
    }
    return type < 0 ? 0 : type;
}

int main(void)
{
    sys_svc_register("disk", "@diskd", -10);
    dport = sys_port_find("disk");
    struct rootpick rp;
    rootpick(dport, &rp);                       /* its first DISK_LIST starts diskd */
    const char *dev = rp.have_ram ? "ram0" : rp.found ? rp.name : "ide0";
    char id[64], prog[SVC_TEXT_MAX];
    int type = look(dev, id);
    strcpy(prog, type == 2 ? "@zfsd " : "@fsd ");
    strcat(prog, dev);
    strcat(prog, " fs -root -id ");
    strcat(prog, id);
    sys_svc_register("fs", prog, -10);
    struct umountinfo mi;
    sys_mountinfo(0, &mi);                      /* the root inode: the first call to "fs" starts the root server */
    char *argv[] = { "/bin/init", 0 };
    for (;;) {
        int pid = sys_spawn("/bin/init", argv, 0);
        if (pid < 0) { say("boot: cannot start /bin/init\n"); sys_sleep(500); continue; }
        int st = sys_wait(pid);
        char n[12], *p = n + sizeof n;
        *--p = 0;
        u32 v = (u32)st;
        do *--p = (char)('0' + v % 10); while ((v /= 10) > 0);
        say("boot: /bin/init ended (status ");
        say(p);
        say("), starting it again\n");
        sys_sleep(100);
    }
}
