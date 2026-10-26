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
 * runs /sbin/zup (DESIGN D15: an update's root half, or what a power cut left), gives the update bundle's memory back,
 * registers the services listed in /etc/svctab and starts /bin/init on console 1 and waits for it; should init ever
 * end, it says so and starts it again. */

static int dport;
static void say(const char *s) { sys_write(1, s, strlen(s)); }   /* no printf: it would double boot's size */
/* /etc/svctab (DESIGN D11 4.4): "PORT NICE PROGRAM [ARGUMENTS]" a line, # comments and blank lines skipped; read from
 * the root once it is up. A bad line (or one the kernel refuses) is reported and skipped, never stops the boot. */
static void svctab(void)
{
    static char buf[2048];
    int fd = sys_open("/etc/svctab", O_RDONLY), n = fd < 0 ? 0 : sys_read(fd, buf, sizeof buf - 1);
    if (fd >= 0) sys_close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    for (char *t = buf; *t; t++)
        if (*t == '\t') *t = ' ';                  /* the kernel splits a program's words at spaces */
    for (char *l = buf, *e; *l; l = e) {
        for (e = l; *e && *e != '\n'; e++)
            ;
        if (*e) *e++ = 0;
        char *p = l, *name, *nv;
        while (*p == ' ') p++;
        if (!*p || *p == '#') continue;
        name = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
        while (*p == ' ') p++;
        nv = p;
        if (*p == '-') p++;
        int digits = 0;
        while (*p >= '0' && *p <= '9') { p++; digits++; }
        int sep = *p == ' ';
        if (*p) *p++ = 0;
        while (*p == ' ') p++;
        if (!digits || !sep || sys_svc_register(name, p, atoi(nv)) < 0) {
            say("boot: /etc/svctab: line skipped: ");
            say(name);
            say("\n");
        }
    }
}

/* /sbin/zup at every start, before anything else uses the root (it does nothing without a bundle, a journal or a
 * way back to take). A zup still working after a minute is reported, one still working after 20 is stopped (the
 * journal finishes its work at the next start); the bundle's memory is given back whatever zup did (RELEASE) */
static void num(u32 v) { char n[12], *p = n + sizeof n; *--p = 0; do *--p = (char)('0' + v % 10); while ((v /= 10) > 0); say(p); }
static void update(void)
{
    char *argv[] = { "/sbin/zup", 0 };
    int pid = sys_spawn(argv[0], argv, 0);
    for (u32 t = 1; pid >= 0 && sys_waitnb(pid) == -2; t++) {     /* t: tenths of a second */
        sys_sleep(10);
        if (t == 600) say("boot: /sbin/zup is still working (an update)\n");
        if (t == 12000) { say("boot: /sbin/zup stopped after 20 min; the next start finishes its work\n"); sys_kill(pid); sys_wait(pid); }
    }
    if (pid < 0) {
        struct uupdate u;
        if (sys_update(UPD_INFO, (u32)&u, sizeof u) == 0 && (u.size || u.flags))
            say("boot: no /sbin/zup: the update is not installed\n");
    }
    sys_update(UPD_RELEASE, 0, 0);
}

int main(void)
{
    sys_svc_register("disk", "@diskd", -10);
    dport = sys_port_find("disk");
    struct rootpick rp;
    rootpick(dport, &rp);                       /* its first DISK_LIST starts diskd */
    const char *dev = rp.have_ram ? "ram0" : rp.found ? rp.name : "ide0";
    char id[64], prog[SVC_TEXT_MAX];
    int type = vol_id(dport, dev, id);   /* which server, from the superblock: a bare disk needs no trial start */
    strcpy(prog, type == 2 ? "@zfsd " : "@fsd ");
    strcat(prog, dev);
    strcat(prog, " fs -root -id ");
    strcat(prog, id);
    sys_svc_register("fs", prog, -10);
    struct umountinfo mi;
    sys_mountinfo(0, &mi);                      /* the root inode: the first call to "fs" starts the root server */
    update();                                   /* an update's root half (D15), before the root is used */
    svctab();                                   /* the services that live on the disk: started when first called */
    char *argv[] = { "/bin/init", 0 };
    for (;;) {
        int pid = sys_spawn("/bin/init", argv, 0);
        if (pid < 0) { say("boot: cannot start /bin/init\n"); sys_sleep(500); continue; }
        int st = sys_wait(pid);
        say("boot: /bin/init ended (status ");
        num((u32)st);
        say("), starting it again\n");
        sys_sleep(100);
    }
}
