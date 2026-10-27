/* SPDX-License-Identifier: GPL-2.0-only */
#pragma GCC optimize("Os")              /* embedded in the kernel image: size first */
#include "libz.h"
#include "../kernel/diskproto.h"
#include "../kernel/fsproto.h"
#include "rdb.h"
#include "rootpick.h"
#include "volid.h"
#include "randseed.h"
/* Built into the kernel image: the starter, pid 1 (DESIGN D11). It registers the disk server and the root filesystem
 * server as services (the kernel starts a service when it is first called, and again after it died); its own first
 * calls start them: rootpick's DISK_LIST starts diskd, the root inode (SYS_mountinfo) starts the root server. An ext2
 * root not marked clean is checked first (DESIGN D17): served read-only, /bin/fsck.ext2 -y on it, then the writable
 * server; a root the check could not repair stays read-only unless the owner chooses otherwise (rescue(), man
 * rescue). Then it runs /sbin/zup (DESIGN D15: an update's root half, or what a power cut left), gives the update bundle's memory back,
 * takes the random seed (DESIGN D19 9.3, randseed.h), registers the services listed in /etc/svctab and starts /bin/init
 * on console 1 and waits for it; should init ever end, it says so and starts it again. */

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
 * journal finishes its work at the next start). "A zup" is every process named zup: stage 1 and the new zup it
 * started for the rest (stage 2), which outlives a stage 1 that crashed or was stopped (audit3 B4); the bundle's
 * memory is given back (RELEASE) only when none runs any more, whatever zup did. With a bundle, a missing /sbin/zup
 * or one that did not end well (crashed, stopped, failed) leaves the update note FAILED for the bundle's version
 * (audit3 B1): login's confirmation then keeps zenixboot from retiring the bundle */
static void num(u32 v) { char n[12], *p = n + sizeof n; *--p = 0; do *--p = (char)('0' + v % 10); while ((v /= 10) > 0); say(p); }
#ifdef DIAG_SLOWZUP                     /* test builds (tools/update-test.sh slowzup): shorter limits */
#define ZUP_SAY  50
#define ZUP_STOP 2400                   /* 4 min: zup checks the bundle first (a minute in FS-UAE) */
#else
#define ZUP_SAY  600                    /* tenths of a second: 1 min */
#define ZUP_STOP 12000                  /* 20 min */
#endif
static int zups(int kill)               /* how many processes named zup are alive; with kill, each is stopped */
{
    struct uproc p;
    int n = 0;
    for (u32 i = 1; sys_procinfo(i, &p) >= 0; i++)
        if (p.state && p.state != 5 && p.name[0] == 'z' && p.name[1] == 'u' && p.name[2] == 'p' && !p.name[3]) {   /* 5: a zombie */
            n++;
            if (kill) sys_kill((int)p.pid);
        }
    return n;
}
static void update(int ro)
{
    char *argv[] = { "/sbin/zup", 0 };
    int pid = ro ? -EROFS : sys_spawn(argv[0], argv, 0), st = 0, done = pid < 0;
    for (u32 t = 1; ; t++) {            /* t: tenths of a second */
        if (!done && (st = sys_waitnb(pid)) != -2) done = 1;
        if (done && !zups(0)) break;
        if (t == ZUP_STOP + 100) { say("boot: a zup does not stop: the update bundle's memory stays reserved\n"); return; }
        sys_sleep(10);
        if (t == ZUP_SAY) say("boot: /sbin/zup is still working (an update)\n");
        if (t == ZUP_STOP) {
            say("boot: /sbin/zup stopped after 20 min; the next start finishes its work\n");
            zups(1);
            if (!done) { st = sys_wait(pid); done = 1; }
            if (!st) st = -1;           /* stopped: not a success */
        }
    }
    struct uupdate u;
    int have = sys_update(UPD_INFO, (u32)&u, sizeof u) == 0;
    if (have && pid < 0 && (u.size || u.flags))
        say(ro ? "boot: the root is read-only: the update is not installed\n" : "boot: no /sbin/zup: the update is not installed\n");
    if (have && u.addr && u.size >= 16 && (pid < 0 || st != 0)) {
        const unsigned char *h = (const unsigned char *)u.addr;       /* the bundle header's version (big-endian, +12): read, never passed */
        sys_update(UPD_NOTE, UPDNOTE_FAILED, (u32)h[12] << 24 | (u32)h[13] << 16 | (u32)h[14] << 8 | h[15]);
    }
    sys_update(UPD_RELEASE, 0, 0);
}

/* fsck.ext2 at boot (D17): its status; a word after 1 minute, stopped after 20 (a cut check is safe: the root stays
 * marked and is checked again at the next start) */
static int wait_limited(int pid)
{
    if (pid < 0) { say("boot: cannot start /bin/fsck.ext2: the root is not checked\n"); return -1; }
    for (u32 t = 1; ; t++) {
        int st = sys_waitnb(pid);
        if (st != -2) return st;
        sys_sleep(10);
        if (t == ZUP_SAY) say("boot: fsck.ext2 is still working\n");
        if (t == ZUP_STOP) { sys_kill(pid); return sys_wait(pid); }
    }
}

/* D17's owner decision (2026-10-26): a root fsck.ext2 could not repair (or could not check) stays served read-only by
 * the check server, and the console offers what AmigaOS offers for a disk it cannot validate; a key, or 1 after 30 s
 * (an unattended machine still comes up). 1 if the root stays read-only, 0 for the writable server (choice 2, or a root
 * the rescue shell left marked clean). man rescue explains the steps. */
static int rescue(const char *dev)
{
    for (;;) {
        say("boot: the root could not be repaired and is mounted read-only. Choose (man rescue):\n"
            "  1  go on read-only (the default in 30 seconds)\n"
            "  2  go on read-write anyway (risky: the damage can grow)\n"
            "  3  a rescue shell as root\n"
            "  4  reboot\n");
        sys_ttymode(TTY_RAW);                   /* one key, no Return */
        char k = '1';
        for (u32 end = sys_ticks() + 3000;;) {
            int left = (int)(end - sys_ticks());
            struct pollfd pf = { 0, POLLIN, 0 };
            if (left <= 0) break;
            if (sys_evwait(&pf, 1, left) > 0 && sys_read(0, &k, 1) == 1 && k >= '1' && k <= '4') break;
            k = '1';
        }
        sys_ttymode(0);
        char e[2] = { k, '\n' };
        sys_write(1, e, 2);
        if (k == '1') { say("boot: going on with the root read-only\n"); return 1; }
        if (k == '2') { say("boot: going on with the root read-write\n"); return 0; }
        if (k == '4') { sys_reboot(0); continue; }
        say("boot: a shell as root; the root stays read-only. Repair commands:\n  fsck.ext2 ");
        say(dev);
        say("      check again, changing nothing\n  fsck.ext2 -y ");
        say(dev);
        say("   repair what it can\n  mount, cp          copy files to safety (another disk) first\n"
            "  exit               back to these choices\n");
        static const char env[] = "PATH=/bin\0HOME=/\0USER=root\0LOGNAME=root\0SHELL=/bin/sh\0";
        sys_envset(env, sizeof env);            /* for the shell; boot's own stays empty after it */
        char *sa[] = { "/bin/sh", 0 };
        int pid = sys_spawn(sa[0], sa, 0);
        sys_envset(0, 0);
        if (pid < 0) say("boot: cannot start /bin/sh\n");
        else sys_wait(pid);
        char id[64];
        if (vol_id(dport, dev, id) == 1 && (vol_state & 1)) {   /* repaired in the shell: no question */
            say("boot: the root is marked clean now: going on read-write\n");
            return 0;
        }
    }
}

/* the seed file at the start (randseed.h): the old seed into the pool, a new one into the file, the old one credited
 * only by the rule. persistent: the root is a writable disk (not a RAM disk, not read-only or a rescue root) */
static int seeded_file;                 /* the file already holds a seed from a seeded pool */
static void seed_start(int persistent)
{
    unsigned char old[SEED_BYTES];
    int fl = 0, ok = seed_load(old, &fl);
    if (ok) {
        int u = sys_open("/dev/urandom", O_WRONLY);
        if (u >= 0) {
            sys_write(u, old, SEED_BYTES);      /* mixed, uncredited */
            sys_close(u);
        }
    }
    if (!persistent) {
        if (ok)
            say("boot: random seed not credited (the root is a RAM disk or read-only)\n");
        memset(old, 0, sizeof old);
        return;
    }
    int nf = seed_flags(seed_pool_ready(), ok, fl, persistent);
    int wrote = seed_store(nf) == 0;
    if (!wrote)
        say("boot: cannot rewrite " SEED_PATH "\n");
    seeded_file = wrote && nf;
    if (seed_credit(ok, fl, persistent, wrote)) {
        int r = sys_open("/dev/random", O_WRONLY);      /* root's write to /dev/random credits it */
        if (r >= 0) {
            sys_write(r, old, SEED_BYTES);
            sys_close(r);
        }
    } else if (ok)
        say("boot: random seed mixed, not credited (from an unseeded boot)\n");
    memset(old, 0, sizeof old);
}

/* wait for init, and meanwhile for the pool's first seeding: then the file gets a seed from a seeded pool (a machine
 * switched off without reboot still has one at its next start) */
static int wait_init(int pid, int persistent)
{
    if (persistent && !seeded_file) {
        struct pollfd p;
        p.fd = sys_open("/dev/random", O_RDONLY);
        while (p.fd >= 0) {
            p.events = POLLIN;
            p.revents = 0;
            if (sys_evwait(&p, 1, 6000) == 1) {         /* a minute at most, then a look at init */
                if (seed_store(SEED_F_SEEDED) == 0)
                    seeded_file = 1;
                break;
            }
            int st = sys_waitnb(pid);
            if (st != -2) {
                sys_close(p.fd);
                return st;
            }
        }
        if (p.fd >= 0)
            sys_close(p.fd);
    }
    return sys_wait(pid);
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
    int check = type == 1 && !rp.have_ram && !(vol_state & 1);   /* ext2 not marked clean: a cut while writing (D17) */
    strcpy(prog, type == 2 ? "@zfsd " : "@fsd ");
    strcat(prog, dev);
    char *w = prog + strlen(prog) + 4;          /* the -root word, or -fsck for the check (the same length) */
    strcat(prog, check ? " fs -fsck -id " : " fs -root -id ");
    strcat(prog, id);
    sys_svc_register("fs", prog, -10);
    struct umountinfo mi;
    sys_mountinfo(0, &mi);                      /* the root inode: the first call to "fs" starts the root server */
    int ro = 0;
    if (check) {                                /* served read-only, the device shared: fsck.ext2 -y on it */
        say("boot: the root was not shut down cleanly: fsck.ext2 -y ");
        say(dev);
        say("\n");
        char *fa[] = { "/bin/fsck.ext2", "-y", (char *)dev, 0 };
        int st = wait_limited(sys_spawn(fa[0], fa, 0));
        if (st > 1) {                           /* 0 clean, 1 repaired: fsck.ext2 said so itself */
            say("boot: fsck.ext2 ended with status ");
            num((u32)st);
            say("\n");
        }
        if (st < 0 || st > 1)
            ro = rescue(dev);
        if (!ro) {
            struct umsg q;
            q.type = FS_QUIT;                   /* the check server lets the device go and ends (a stop, not a crash) */
            sys_call(sys_port_find("fs"), &q);
            memcpy(w, "-root", 5);              /* the writable server from now on (a new text: a first start) */
            sys_svc_register("fs", prog, -10);
        }
    }
    update(ro);                                 /* an update's root half (D15), before the root is used */
    int persistent = !rp.have_ram && !ro;
    seed_start(persistent);
    svctab();                                   /* the services that live on the disk: started when first called */
    char *argv[] = { "/bin/init", 0 };
    for (;;) {
        int pid = sys_spawn("/bin/init", argv, 0);
        if (pid < 0) { say("boot: cannot start /bin/init\n"); sys_sleep(500); continue; }
        int st = wait_init(pid, persistent);
        say("boot: /bin/init ended (status ");
        num((u32)st);
        say("), starting it again\n");
        sys_sleep(100);
    }
}
