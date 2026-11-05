/* SPDX-License-Identifier: GPL-2.0-only */
/* netd, Zenix's network server (DESIGN D19, docs/d19-network.md; docs/man/netd.txt): the service "net" that boot
 * registers as /sbin/netd and the kernel starts at the first socket call. This file is netd's main program on Zenix:
 * the operating system layer the core asks for (netd.h: the clocks, the serial port, the log) and the message loop
 * that turns the kernel's NET_* requests (kernel/netproto.h) into the core's calls (net/netd: sockets, interfaces,
 * names over lwIP's raw API). The rules of 4.3 it keeps: every kernel request is answered at once with NETSRV_REPLY
 * (the result and the socket's new ready bits in one trap); the bits that changed otherwise are pushed with
 * NETSRV_READY once a turn; the only waits are in the loop's evwait (the port, the serial lines, the timers). */
#include "../../user/libz.h"
#include "../../kernel/netproto.h"
#include "netd.h"
#include "../port/netd_port.h"
#define NAME_IDX(n) ((n) & 0xFF)
#define NAME_GEN(n) (((n) >> 8) & 0xFFFF)

/* ---- the operating system layer (netd.h, netd_port.h) ---- */
unsigned long netd_os_now_ms(void) { return (unsigned long)sys_ticks() * 10; }
/* RFC 6528's clock: the E-clock (709 or 716 kHz) counts 1.4 us; a quarter of it is near enough to 4 us */
unsigned long netd_os_clock4us(void) { return (unsigned long)sys_hrtime() >> 2; }

void netd_os_log(const char *line)
{
    char b[200];
    u32 n = strlen(line);
    if (n > sizeof b - 1) n = sizeof b - 1;
    memcpy(b, line, n);
    b[n++] = '\n';
    sys_write(1, b, n);                 /* a service's output is the kernel log (dmesg) */
}
void netd_os_fatal(void) { sys_exit(70); }

/* a serial line for SLIP (D19 5.3): /dev/ttyS0 at 115,200 baud 8N1, never waiting */
int netd_os_serial_open(const char *dev, unsigned long baud)
{
    if (baud != 115200) return -EINVAL;         /* Zenix's serial port runs at 115,200 only (D12) */
    int fd = sys_open(dev, O_RDWR | O_NONBLOCK);
    return fd;
}
long netd_os_read(int h, void *buf, unsigned long n)
{
    int r = sys_read(h, buf, (u32)n);
    if (r == -EAGAIN || r == -EINTR) return 0;
    return r ? r : -EIO;
}
long netd_os_write(int h, const void *buf, unsigned long n)
{
    int r = sys_write(h, buf, (u32)n);          /* O_NONBLOCK: what fits in the send ring now */
    return r == -EAGAIN || r == -EINTR ? 0 : r;
}
void netd_os_close(int h) { sys_close(h); }
/* a driver's buffers: a region of their own (freed whole when the interface and the stack are done with them) */
void *netd_os_alloc(unsigned long n) { return sys_memalloc((u32)n); }
void netd_os_free(void *p) { sys_memfree(p); }

/* ---- netd's heap (net/port/port.c counts and caps it): first fit in a few regions of its own, each a quarter of the
 * limit (at least 8 KB), taken when needed: libz's malloc would round every block up to a power of two and grow in
 * chunks of 4, 16, 64 KB. Free blocks are kept in address order and joined with their neighbours. ---- */
struct hblk { u32 size; struct hblk *next; };  /* size with this header; next only while free */
#define HB_USED ((struct hblk *)0x5a4e4855)      /* "ZNHU": in use (a stray free is refused) */
#define NREG 5
static struct { char *base; u32 size; struct hblk *free; } reg[NREG];
static int nreg;
static u32 reg_chunk = 8192;

static int new_region(u32 need)
{
    u32 sz = reg_chunk > need ? reg_chunk : (need + 4095) & ~4095u;
    char *b;
    if (nreg == NREG || !(b = sys_memalloc(sz + 8))) return -1;
    char *a = (char *)(((u32)b + 7) & ~7u);
    reg[nreg].base = a;
    reg[nreg].size = sz;
    reg[nreg].free = (struct hblk *)a;
    reg[nreg].free->size = sz;
    reg[nreg].free->next = 0;
    nreg++;
    return 0;
}
void *netd_heap_alloc(unsigned long n)
{
    u32 need = ((u32)n + sizeof(struct hblk) + 7) & ~7u;
    if (n > 0x100000) return 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int r = 0; r < nreg; r++)
            for (struct hblk **pp = &reg[r].free, *b; (b = *pp); pp = &b->next) {
                if (b->size < need) continue;
                if (b->size - need >= 32) {             /* split: the rest stays free, in place */
                    struct hblk *rest = (struct hblk *)((char *)b + need);
                    rest->size = b->size - need;
                    rest->next = b->next;
                    *pp = rest;
                    b->size = need;
                } else
                    *pp = b->next;
                b->next = HB_USED;
                return b + 1;
            }
        if (pass || new_region(need) < 0) break;
    }
    return 0;
}
void netd_heap_free(void *p)
{
    struct hblk *b = (struct hblk *)p - 1;
    int r;
    if (!p) return;
    for (r = 0; r < nreg; r++)
        if ((char *)b >= reg[r].base && (char *)b < reg[r].base + reg[r].size) break;
    if (r == nreg || b->next != HB_USED) {
        dprintf(1, "netd: a free of memory netd's heap did not give (%x): ignored\n", (u32)p);
        return;
    }
    struct hblk **pp = &reg[r].free, *prev = 0;
    while (*pp && *pp < b) { prev = *pp; pp = &(*pp)->next; }
    b->next = *pp;
    *pp = b;
    if (b->next && (char *)b + b->size == (char *)b->next) {    /* join the next */
        b->size += b->next->size;
        b->next = b->next->next;
    }
    if (prev && (char *)prev + prev->size == (char *)b) {        /* and the one before */
        prev->size += b->size;
        prev->next = b->next;
    }
}

/* ---- the machine's profile (D19 7.3): small on chip RAM alone, large with enough fast RAM ---- */
#define LARGE_FAST (2u << 20)                   /* free fast RAM for the large profile (measured: section 14) */
static int profile(void)
{
    u32 m[4];
    if (sys_meminfo(m) < 0) return NETD_PROFILE_SMALL;
    return m[1] >= LARGE_FAST ? NETD_PROFILE_LARGE : NETD_PROFILE_SMALL;
}

/* ---- random keys (D19 9.3): from /dev/urandom before lwIP starts; fresh ones when /dev/random says the pool is
 * seeded, and every RESEED ticks after ---- */
#define RESEED (5 * 60 * 100)
static int rnd_fd = -1;                         /* /dev/random until the pool is seeded, then closed */
static u32 last_seed;
static int seed(unsigned char s[48], const char *dev)
{
    int fd = sys_open(dev, O_RDONLY), n = fd < 0 ? fd : sys_read(fd, s, 48);
    if (fd >= 0) sys_close(fd);
    return n == 48 ? 0 : -1;
}
static void reseed(const char *dev)
{
    unsigned char s[48];
    if (!seed(s, dev)) netd_reseed(s);
    memset(s, 0, sizeof s);
    last_seed = sys_ticks();
}

/* ---- drivers (D19 2.3): each a child of netd, /sbin/net/NAME IFACE [ARGS], with a pipe as its standard input: the
 * doorbell (one byte when netd has work for it and has not rung since its last DRV_XFER; netd ignores SIGPIPE, and
 * the driver takes POLLHUP on the pipe as netd's end). netd supervises them as init does a console's login, with
 * D11's numbers: an end is logged, the interface goes down, the driver starts again after 0, 0, 1, 2, 4 s, and the
 * fifth end in 60 s gives it up until ifup ---- */
#define NDRV NETD_IFMAX
#define DRV_WINDOW 6000u                /* ends are counted in 60 s (ticks) */
static struct drv {
    int used;                           /* a stanza with a driver other than slip, brought up */
    struct netd_ifcfg cfg;
    int pid;                            /* the running driver, 0: none */
    int bell, rung;                     /* the doorbell's write end; rung since the driver's last DRV_XFER */
    int h;                              /* its interface (netd_drv_attach), -1 before the attach */
    int ends, givenup, stopping;
    int probe, attached;                /* started by the card probe (no stanza); its driver has attached once */
    u32 win, start_at;                  /* the 60 s window; when to start it again (0: not waiting) */
} drvs[NDRV];
static volatile int child_ended;
static int probe_spawning;              /* drv_up is starting a card probe's driver (quiet: see drv_spawn) */
static int stop_idle;                   /* NETIFREQ_BOOT: decide once the probe is done: stop if nothing is left to serve */
static u32 ifset_sock, boot_sock;       /* the socket of the request being served; of the NETIFREQ_BOOT (ifup's own) */
static void on_child(int sig) { (void)sig; child_ended = 1; }

static struct drv *drv_named(const char *name)
{
    for (int i = 0; i < NDRV; i++) if (drvs[i].used && !strcmp(drvs[i].cfg.name, name)) return &drvs[i];
    return 0;
}
static struct drv *drv_of_pid(int pid)
{
    for (int i = 0; i < NDRV; i++) if (drvs[i].used && drvs[i].pid == pid && pid > 0) return &drvs[i];
    return 0;
}
static int drv_spawn(struct drv *d)
{
    char path[48], args[64], *argv[8];
    int p[2], n = 0;
    snprintf(path, sizeof path, "/sbin/net/%s", d->cfg.driver);
    strncpy(args, d->cfg.args, sizeof args - 1);
    args[sizeof args - 1] = 0;
    argv[n++] = path;
    argv[n++] = d->cfg.name;
    for (char *w = args; *w && n < 7;) {        /* the rest of the driver line, word by word */
        while (*w == ' ') *w++ = 0;
        if (!*w) break;
        argv[n++] = w;
        while (*w && *w != ' ') w++;
    }
    argv[n] = 0;
    /* a card probe's driver (D19 6.3) that has not found its card yet gets the word "probe": it says nothing until the
     * card is there (a quiet console, as a Linux driver that finds no device says nothing); netd's own lines about it
     * wait too. A stanza naming the driver shows its reasons as always */
    int quiet = probe_spawning || (d->probe && !d->attached);
    if (quiet && n < 7) argv[n++] = "probe";
    argv[n] = 0;
    if (sys_pipe(p) < 0) return -EMFILE;
    int io[3] = { p[0], 1, 2 };                 /* its output is netd's: the kernel log */
    int pid = sys_spawn(path, argv, io);
    sys_close(p[0]);
    if (pid < 0) { sys_close(p[1]); return pid; }
    d->pid = pid;
    d->bell = p[1];
    d->rung = 0;
    d->h = -1;
    d->start_at = 0;
    if (!quiet) dprintf(1, "net: %s: started %s (pid %d)\n", d->cfg.name, path, pid);
    return 0;
}
/* ifup of a stanza with a driver: its process starts; the interface appears when it attaches */
static long drv_up(const struct netd_ifcfg *c)
{
    struct drv *d = drv_named(c->name);
    if (strchr(c->driver, '/') || !c->driver[0]) return -EINVAL;   /* NAME means /sbin/net/NAME (6.1) */
    if (d && (d->pid || d->start_at) && !d->givenup) return -EEXIST;
    if (!d)
        for (int i = 0; i < NDRV && !d; i++) if (!drvs[i].used) d = &drvs[i];
    if (!d) return -ENOSPC;
    memset(d, 0, sizeof *d);
    d->used = 1;
    d->cfg = *c;
    d->h = -1;
    long r = drv_spawn(d);
    if (r < 0) d->used = 0;
    return r;
}
/* the driver's interface goes and the driver stops: its doorbell closes (POLLHUP: it stops its device and ends; the
 * kernel stops the device anyway when it ends, D19 2.4) */
static void drv_down(struct drv *d)
{
    if (d->h >= 0) netd_drv_gone(d->h);
    d->h = -1;
    if (d->bell > 0) sys_close(d->bell);
    d->bell = 0;
    d->stopping = d->pid != 0;
    d->start_at = 0;
    if (!d->pid) d->used = 0;
}
static const char *const probe_drv[] = NETD_PROBE_DRIVERS;
#define NPROBE ((int)(sizeof probe_drv / sizeof probe_drv[0]))
static int probe_at = NPROBE;           /* the next driver to try; NPROBE: no probe going on */
static int probe_busy;                  /* a probe's driver is running and has not attached yet */
static char probe_none[48], probe_got[48];  /* the drivers this probe found no card for, and a card for; ", " between */
static void probe_note(char *b, u32 n, const char *drv)
{
    u32 l = strlen(b);
    if (l + strlen(drv) + 3 < n) snprintf(b + l, n - l, "%s%s", l ? ", " : "", drv);
}
static void probe_next(void);
static int idle_cond(void);
/* SIGCHLD came: which drivers ended (the handler only sets the flag, D19 2.3) */
static void drv_reap(void)
{
    child_ended = 0;
    for (int i = 0; i < NDRV; i++) {
        struct drv *d = &drvs[i];
        if (!d->used || !d->pid) continue;
        int st = sys_waitnb(d->pid);
        if (st == -2) continue;
        if (!d->probe || d->attached || d->stopping)
            dprintf(1, "net: %s: /sbin/net/%s (pid %d) ended, status %d\n", d->cfg.name, d->cfg.driver, d->pid, st);
        d->pid = 0;
        if (d->h >= 0) netd_drv_gone(d->h);
        d->h = -1;
        if (d->bell > 0) sys_close(d->bell);
        d->bell = 0;
        if (d->stopping) {
            if (d->probe && !d->attached) { probe_busy = 0; probe_at = NPROBE; }   /* ifdown in the probe: it ends */
            d->used = 0;
            continue;
        }
        if (d->probe && !d->attached) {         /* the probe found no card for it: noted for the probe's one line */
            probe_note(probe_none, sizeof probe_none, d->cfg.driver);
            d->used = 0;
            probe_next();
            continue;
        }
        u32 now = sys_ticks();
        if (!d->ends || now - d->win >= DRV_WINDOW) { d->win = now; d->ends = 0; }
        if (++d->ends >= 5) {
            dprintf(1, "net: %s: /sbin/net/%s given up after 5 ends in 60 s (ifup %s starts it again)\n", d->cfg.name,
                   d->cfg.driver, d->cfg.name);
            d->givenup = 1;
            continue;
        }
        u32 back = d->ends >= 2 ? 100u << (d->ends - 2) : 0;
        d->start_at = (now + back) | 1;
        if (back) dprintf(1, "net: %s: start %d in %u s\n", d->cfg.name, d->ends + 1, back / 100);
    }
}
/* drivers due to start again; the ticks until the next one (0x7fffffff: none) */
static u32 drv_starts(void)
{
    u32 now = sys_ticks(), next = 0x7fffffff;
    for (int i = 0; i < NDRV; i++) {
        struct drv *d = &drvs[i];
        if (!d->used || !d->start_at) continue;
        u32 left = d->start_at - now;
        if ((int)left <= 0) {
            d->start_at = 0;
            if (drv_spawn(d) < 0) {
                dprintf(1, "net: %s: cannot start /sbin/net/%s\n", d->cfg.name, d->cfg.driver);
                d->used = 0;
            }
        } else if (left < next) next = left;
    }
    return next;
}
static void drv_bells(void)
{
    for (int i = 0; i < NDRV; i++) {
        struct drv *d = &drvs[i];
        if (d->used && d->pid && d->h >= 0 && !d->rung && d->bell > 0 && netd_drv_pending(d->h)) {
            sys_write(d->bell, "!", 1);         /* never blocks: at most a byte or two in the pipe */
            d->rung = 1;
        }
    }
}

/* NETDRV_ATTACH: the driver of one of netd's stanzas says who it is (the kernel checked the block's place and size) */
static long do_attach(struct umsg *m)
{
    struct drv *d = drv_of_pid((int)m->a[0]);
    struct udrvattach *a = (struct udrvattach *)m->a[1];
    struct netd_drvattach na;
    void *bufs[NETD_DRV_MAXBUF];
    unsigned bs, nb;
    if (!d) return -EACCES;                     /* not one of netd's drivers (D19 9.1) */
    if (d->h >= 0) return -EEXIST;
    if (strncmp(a->name, d->cfg.name, sizeof a->name)) return -EINVAL;
    memset(&na, 0, sizeof na);
    memcpy(na.mac, a->mac, 6);
    na.mtu = a->mtu;
    na.nrx = a->nrx;
    na.dma = (a->flags & DRVA_DMA) != 0;
    na.link = (a->flags & DRVA_LINK) != 0;
    na.dmamem = (void *)a->dma;
    na.dmalen = a->dmalen;
    int h = netd_drv_attach(&d->cfg, &na, &bs, &nb, bufs);
    if (h < 0) return h;
    d->h = h;                                   /* a good start does not clear the end count: the window does (D11) */
    stop_idle = 0;                              /* an interface: netd stays */
    if (d->probe && !d->attached) {
        dprintf(1, "netd: %s: /sbin/net/%s found a card: up with DHCP (no stanza in /etc/network/interfaces names it)\n",
                d->cfg.name, d->cfg.driver);
        d->attached = 1;
        probe_note(probe_got, sizeof probe_got, d->cfg.driver);
        probe_next();
    }
    d->attached = 1;
    a->ifindex = (u32)h + 1;
    a->bufsize = bs;
    a->nbuf = nb;
    for (u32 k = 0; k < nb; k++) a->buf[k] = (u32)bufs[k];
    return (long)a->ifindex;
}
/* NETDRV_XFER: one batch each way; every number in it is checked by the core (a wrong one fails that entry) */
static long do_xfer(struct umsg *m)
{
    struct drv *d = drv_of_pid((int)m->a[0]);
    struct udrvxfer *x = (struct udrvxfer *)m->a[1];
    static struct netd_frame fr[DRV_BATCH];
    if (!d) return -EACCES;
    if (d->h < 0) return -ENOTCONN;
    if (x->nrx > DRV_BATCH || x->ndone > DRV_BATCH) return -EINVAL;
    d->rung = 0;
    netd_drv_link(d->h, x->link != 0);
    netd_drv_count(d->h, x->rx_bad, x->rx_drop, x->tx_err);
    for (u32 k = 0; k < x->ndone; k++) netd_drv_done(d->h, x->done[k]);
    for (u32 k = 0; k < x->nrx; k++) netd_drv_rx(d->h, x->rx[k].num, x->rx[k].len);
    x->nfree = (u32)netd_drv_freed(d->h, x->freed, DRV_BATCH);
    x->ntx = (u32)netd_drv_tx(d->h, fr, DRV_BATCH);
    for (u32 k = 0; k < x->ntx; k++) {
        struct udrvframe *o = &x->tx[k];
        memset(o, 0, sizeof *o);
        o->tag = fr[k].tag;
        o->npieces = fr[k].npieces;
        for (int j = 0; j < fr[k].npieces; j++) { o->piece[j].addr = (u32)fr[k].piece[j].addr; o->piece[j].len = (u32)fr[k].piece[j].len; }
    }
    x->more = (u32)netd_drv_pending(d->h);
    return 0;
}

/* ---- the configuration files (D19 6.1): read at the start (and on a reload), never per request ---- */
#define CONF_MAX 16384
static char *slurp(const char *path, unsigned long *len)
{
    struct ustat st;
    int fd = sys_open(path, O_RDONLY);
    char *b = 0;
    *len = 0;
    if (fd < 0) return 0;
    if (sys_fstat(fd, &st) >= 0 && st.size <= CONF_MAX && (b = sys_memalloc(st.size + 1))) {   /* given back at once */
        int n = sys_read(fd, b, st.size);
        *len = n > 0 ? (unsigned long)n : 0;
        b[*len] = 0;
    } else if (st.size > CONF_MAX)
        dprintf(1, "netd: %s: over %u bytes, not read\n", path, CONF_MAX);
    sys_close(fd);
    return b;
}
static struct netd_config conf;         /* /etc/resolv.conf's name servers and /etc/network/interfaces' stanzas */
/* 0, -ENOENT (no file) or -EACCES (not root's alone: netd starts programs as root from it, D19 6.1) */
static int ifs_state;
static void load_config(void)
{
    unsigned long n;
    struct ustat st;
    char *t = slurp("/etc/hosts", &n);
    netd_hosts_load(t ? t : "", n);
    if (t) sys_memfree(t);
    memset(&conf, 0, sizeof conf);
    if ((t = slurp("/etc/resolv.conf", &n))) { netd_parse_resolv(t, n, &conf); sys_memfree(t); }
    netd_set_dns(conf.dns, conf.ndns);
    netd_set_search(conf.search);
    ifs_state = -ENOENT;
    if (sys_stat("/etc/network/interfaces", &st) < 0) return;
    if (!netd_config_trusted(st.mode, st.uid)) {
        dprintf(1, "netd: /etc/network/interfaces must belong to root and be writable by root alone: not used\n");
        ifs_state = -EACCES;
        return;
    }
    if ((t = slurp("/etc/network/interfaces", &n))) {
        netd_parse_interfaces(t, n, &conf);
        ifs_state = 0;
        sys_memfree(t);
    }
}

/* ---- the card probe (D19 6.3): at netd's start and at ifup -a, each shipped driver no stanza (and no noprobe line)
 * names is started in turn as ethN with DHCP; one that ends before it attaches found no card (it said why) and is
 * forgotten, not started again; one that attaches is an interface like a stanza's, supervised the same way ---- */
static int name_free(const char *n)
{
    struct netd_ifinfo f;
    if (drv_named(n)) return 0;
    for (int i = 0; i < conf.nifs; i++) if (!strcmp(conf.ifs[i].name, n)) return 0;
    for (int i = 0; netd_if_info(i, &f) == 0; i++) if (!strcmp(f.name, n)) return 0;
    return 1;
}
static void probe_next(void)
{
    probe_busy = 0;
    while (probe_at < NPROBE) {
        const char *dn = probe_drv[probe_at++];
        char path[32];
        struct ustat st;
        int used = 0;
        for (int i = 0; i < NDRV; i++) if (drvs[i].used && !strcmp(drvs[i].cfg.driver, dn)) used = 1;
        snprintf(path, sizeof path, "/sbin/net/%s", dn);
        if (used || !netd_probe_wanted(&conf, dn) || sys_stat(path, &st) < 0) continue;
        struct netd_ifcfg c;
        memset(&c, 0, sizeof c);
        for (int k = 0; k < 10 && !c.name[0]; k++) {
            snprintf(c.name, sizeof c.name, "eth%d", k);
            if (!name_free(c.name)) c.name[0] = 0;
        }
        if (!c.name[0]) { dprintf(1, "netd: no free name ethN for the card probe of %s\n", dn); continue; }
        c.is_auto = 1;
        c.method = NETD_INET_DHCP;
        strcpy(c.driver, dn);
        probe_spawning = 1;
        long r = drv_up(&c);
        probe_spawning = 0;
        if (r < 0) { dprintf(1, "netd: %s: the card probe could not start %s: %s\n", c.name, path, zstrerror((int)r)); continue; }
        struct drv *d = drv_named(c.name);
        if (d) d->probe = 1;
        probe_busy = 1;
        return;
    }
    /* the probe is done: one line in the log (when netd is about to stop for want of anything to serve, its line says
     * it instead) */
    if ((probe_got[0] || probe_none[0]) && !idle_cond())
        dprintf(1, "netd: card probe done: %s%s%s%s%s\n", probe_got[0] ? "found " : "", probe_got,
                probe_got[0] && probe_none[0] ? "; " : "", probe_none[0] ? "no card for " : "", probe_none);
}
static void probe_start(void)
{
    if (ifs_state == -EACCES || probe_busy || probe_at < NPROBE) return;    /* a file netd may not use: no guessing */
    probe_at = 0;
    probe_none[0] = 0;
    probe_next();
}
/* NETIFREQ_BOOT, decided once when the probe is done: stop if there is nothing to serve: no interface but the
 * loopback, no driver, no socket but ifup's own (it closes it after the answer) */
static int idle_cond(void)
{
    struct netd_ifinfo f;
    if (!stop_idle) return 0;
    for (int i = 0; i < NDRV; i++) if (drvs[i].used) return 0;
    for (int i = 0; netd_if_info(i, &f) == 0; i++) if (strcmp(f.name, "lo")) return 0;
    return !netd_nsockets(NAME_IDX(boot_sock), NAME_GEN(boot_sock));
}
static int idle_now(void)
{
    if (!stop_idle || probe_busy || probe_at < NPROBE) return 0;
    int r = idle_cond();
    stop_idle = 0;
    return r;
}

/* ifup: the stanza of name ("": every "auto" one); an interface already up is no error for "" */
static long if_up(const char *name)
{
    long r = 0, e;
    if (ifs_state) return name[0] ? ifs_state : ifs_state == -EACCES ? -EACCES : 0;
    for (int i = 0; i < conf.nifs; i++) {
        struct netd_ifcfg *c = &conf.ifs[i];
        if (name[0] ? strcmp(c->name, name) : !c->is_auto) continue;
        e = strcmp(c->driver, "slip") ? drv_up(c) : netd_if_up(c);
        if (e < 0 && !(e == -EEXIST && !name[0])) {
            dprintf(1, "netd: %s would not come up: %s\n", c->name, zstrerror((int)e));
            if (!r) r = e;
        }
        if (name[0]) return e;
    }
    return name[0] ? -ENOENT : r;
}
static long if_down(const char *name)
{
    struct netd_ifinfo f;
    struct drv *d;
    if (name[0]) {
        if ((d = drv_named(name)) && (d->pid || d->start_at || d->givenup)) { drv_down(d); return 0; }
        return netd_if_down(name);
    }
    for (int i = 0; i < NDRV; i++) if (drvs[i].used && !drvs[i].stopping) drv_down(&drvs[i]);
    for (int i = 0; netd_if_info(i, &f) == 0;)
        if (strcmp(f.name, "lo") && netd_if_down(f.name) == 0) i = 0;    /* the list changed: from the start */
        else i++;
    return 0;
}

static long do_ifset(struct netreq *q)
{
    struct unetifreq rq;
    if (q->uid) return -EPERM;                  /* the network's configuration is root's (D19 9.1) */
    if (q->len < UNETIFREQ_FIRST) return -EINVAL;
    memcpy(&rq, (const void *)q->r.buf, sizeof rq);
    rq.name[sizeof rq.name - 1] = 0;
    switch (rq.op) {
    case NETIFREQ_BOOT:
        rq.name[0] = 0;
        stop_idle = 1;
        boot_sock = ifset_sock;
        /* fall through */
    case NETIFREQ_UP: {
        load_config();                          /* read again: ifup is the reload (D19 6.1) */
        long r = if_up(rq.name);
        if (!rq.name[0]) probe_start();         /* ifup -a: the cards no stanza names (D19 6.3) */
        return r;
    }
    case NETIFREQ_DOWN:
        return if_down(rq.name);
    case NETIFREQ_ADDR:
        return rq.name[0] && rq.addr ? netd_if_addr(rq.name, rq.addr, rq.mask) : -EINVAL;
    }
    return -EINVAL;
}

static long do_iflist(struct netreq *q)
{
    u32 rs = q->r.flags ? q->r.flags : UNETIF_FIRST, n = 0;
    struct netd_ifinfo f;
    unsigned long gw;
    char dn[NETD_IFNAME];
    unsigned long dns_now[2];
    if (rs < UNETIF_FIRST || rs > 4096) return -EINVAL;
    netd_dns_servers(dns_now);
    netd_route_get(&gw, dn);
    for (int i = 0; (n + 1) * rs <= q->len && netd_if_info(i, &f) == 0; i++, n++) {
        struct unetif u;
        unsigned char *o = (unsigned char *)q->r.buf + n * rs;
        memset(&u, 0, sizeof u);
        memcpy(u.name, f.name, sizeof u.name - 1);
        u.addr = f.addr;
        u.mask = f.mask;
        u.gw = f.gateway;
        u.mtu = f.mtu;
        u.flags = (unsigned short)((f.up ? NETIF_UP : 0) | (f.link ? NETIF_LINK : 0) |
                                   (!strcmp(f.name, "lo") ? NETIF_LOOPBACK : 0) | (dn[0] && !strcmp(dn, f.name) ? NETIF_DEFAULT : 0));
        u.rx_frames = f.rx_frames; u.tx_frames = f.tx_frames;
        u.rx_bytes = f.rx_bytes; u.tx_bytes = f.tx_bytes;
        u.rx_bad = f.rx_bad; u.tx_drop = f.tx_drop;
        u.flags |= (unsigned short)((f.ether ? NETIF_ETHER : 0) | (f.dhcp ? NETIF_DHCP : 0));
        u.dns[0] = dns_now[0];
        u.dns[1] = dns_now[1];
        memcpy(u.hwaddr, f.hwaddr, 6);
        u.lease = f.lease;
        u.dhcp_server = f.dhcp_server;
        u.rx_drop = f.rx_drop;
        memset(o, 0, rs);
        memcpy(o, &u, rs < sizeof u ? rs : sizeof u);
    }
    return (long)n;
}

static long do_route(struct netreq *q)
{
    struct uroute *rr = (struct uroute *)q->r.buf;
    unsigned long gw;
    if (q->len < UROUTE_FIRST) return -EINVAL;
    switch (rr->op) {
    case ROUTE_GET:
        netd_route_get(&gw, rr->name);
        rr->gw = gw;
        return 0;
    case ROUTE_ADD:
    case ROUTE_DEL:
        if (q->uid) return -EPERM;
        if (rr->op == ROUTE_ADD && !rr->gw) return -EINVAL;
        return netd_route_default(rr->op == ROUTE_ADD ? rr->gw : 0);
    }
    return -EINVAL;
}

/* ---- the message loop ---- */
static int port;
static u32 epoch;

/* an address length the program gets back: the true one (BSD's in-out socklen_t) */
static void put_len(u32 p, unsigned v) { if (p) *(u32 *)p = v; }

static struct netd_connrow rows[64];
static long do_netstat(struct netreq *q)
{
    u32 rs = q->r.flags ? q->r.flags : UNETSTAT_FIRST, max, i, n;
    if (rs < UNETSTAT_FIRST || rs > 4096) return -EINVAL;
    max = q->len / rs;
    n = (u32)netd_netstat(rows, max < 64 ? (int)max : 64);
    for (i = 0; i < n; i++) {
        struct unetstat u;
        unsigned char *o = (unsigned char *)q->r.buf + i * rs;
        memset(&u, 0, sizeof u);
        u.proto = rows[i].proto;
        u.state = rows[i].state;
        u.lport = rows[i].lport;
        u.laddr = rows[i].laddr;
        u.raddr = rows[i].raddr;
        u.rport = rows[i].rport;
        u.rxq = rows[i].rxq;
        u.txq = rows[i].txq;
        u.uid = rows[i].uid;
        memset(o, 0, rs);
        memcpy(o, &u, rs < sizeof u ? rs : sizeof u);
    }
    return (long)n;
}

/* one kernel request: the result (>= 0 or -errno); the core writes into the program's memory in place, never more
 * than the lengths the kernel checked (netreq's len, alen, vlen) */
static long serve(struct umsg *m)
{
    if (m->type == NETDRV_ATTACH) return do_attach(m);
    if (m->type == NETDRV_XFER) return do_xfer(m);
    struct netreq *q = (struct netreq *)m->a[1];
    struct usockreq *r = &q->r;
    unsigned i = NAME_IDX(m->a[0]), g = NAME_GEN(m->a[0]), al = q->alen;
    void *addr = r->addr ? (void *)r->addr : 0;
    long v;
    switch (m->type) {
    case SOCK_SOCKET:
        return netd_socket(i, g, r->level, (int)r->flags, r->option, q->uid);
    case SOCK_BIND:
        return netd_bind(i, g, addr, al, q->uid);
    case SOCK_CONNECT:
        return netd_connect(i, g, addr, al);
    case SOCK_LISTEN:
        return netd_listen(i, g, (int)(r->len > 255 ? 255 : r->len));
    case SOCK_ACCEPT:
        v = netd_accept(i, g, NAME_IDX(q->newsock), NAME_GEN(q->newsock), addr, addr ? &al : 0);
        if (v >= 0 && addr) put_len(r->alen, al);
        return v;
    case SOCK_SENDTO:
        return netd_send(i, g, (const void *)r->buf, q->len, (int)r->flags, addr, al);
    case SOCK_RECVFROM:
        v = netd_recv(i, g, (void *)r->buf, q->len, (int)r->flags, addr, addr ? &al : 0);
        if (v >= 0 && addr) put_len(r->alen, al);
        return v;
    case SOCK_SHUTDOWN:
        return netd_shutdown(i, g, (int)r->flags);
    case SOCK_GETSOCKOPT: {
        unsigned vl = q->vlen;
        v = netd_getsockopt(i, g, r->level, r->option, (void *)r->val, &vl);
        if (!v) put_len(r->vlen, vl);
        return v;
    }
    case SOCK_SETSOCKOPT:
        return netd_setsockopt(i, g, r->level, r->option, (const void *)r->val, q->vlen);
    case SOCK_GETSOCKNAME:
    case SOCK_GETPEERNAME:
        v = netd_getname(i, g, m->type == SOCK_GETPEERNAME, addr, &al);
        if (!v) put_len(r->alen, al);
        return v;
    case SOCK_IOCTL: {
        long n = 0;
        v = netd_ioctl(i, g, r->option, &n);    /* FIONREAD: the core's long, the program's int */
        if (!v) *(int *)r->val = (int)n;
        return v;
    }
    case SOCK_RESOLVE: {
        /* the name in buf (not NUL-ended); the socket becomes the lookup (D19 4.5): the one the program opened is
         * given up first, the same (index, generation) names the lookup */
        static char name[256];
        u32 n = q->len < sizeof name - 1 ? q->len : sizeof name - 1;
        memcpy(name, (const void *)r->buf, n);
        name[n] = 0;
        if (!name[0] || strlen(name) != n) return -EINVAL;
        if ((v = netd_close(i, g)) < 0) return v;
        return netd_resolve(i, g, name, q->uid);
    }
    case SOCK_NETSTAT:                          /* asked on any socket of the caller's: it must be one netd knows */
    case SOCK_IFLIST:
    case SOCK_IFSET:
    case SOCK_ROUTE: {
        long n;
        if ((v = netd_ioctl(i, g, NETD_FIONREAD, &n)) == -ENETDOWN) return v;
        ifset_sock = m->a[0];
        return m->type == SOCK_NETSTAT ? do_netstat(q) : m->type == SOCK_IFLIST ? do_iflist(q) :
               m->type == SOCK_IFSET ? do_ifset(q) : do_route(q);
    }
    case NET_CLOSE:
        return netd_close(i, g);
    }
    return -EOPNOTSUPP;
}

static void answer(struct umsg *m)
{
    long r;
    u32 b;
    if (!(m->kflags & UMSG_KERNEL)) {           /* only the kernel's requests (D19 9.1) */
        sys_netsrv(NETSRV_REPLY, (u32)m, (u32)-EACCES);
        return;
    }
    r = serve(m);
    if (m->type >= NETDRV_ATTACH) b = 0;        /* a driver's block: no socket */
    else if (m->type == SOCK_SOCKET) b = netd_bits(NAME_IDX(m->a[0]), NAME_GEN(m->a[0]));
    else {
        b = m->type == NET_CLOSE ? 0 : netd_bits(NAME_IDX(m->a[0]), NAME_GEN(m->a[0]));
        if (m->type == SOCK_ACCEPT && r >= 0) {
            struct netreq *q = (struct netreq *)m->a[1];
            b |= netd_bits(NAME_IDX(q->newsock), NAME_GEN(q->newsock)) << 16;
        }
    }
    m->a[2] = b;                                /* the truth with the reply (rule 1) */
    m->a[3] = epoch;
    sys_netsrv(NETSRV_REPLY, (u32)m, (u32)r);
}

static void push(void)
{
    static struct netd_ready rd[NETD_NSOCK];
    static struct unetready u[NETD_NSOCK];
    int n = netd_changed(rd, NETD_NSOCK);
    for (int k = 0; k < n; k++) {
        u[k].sock = (u32)rd[k].idx | (u32)rd[k].gen << 8;
        u[k].bits = rd[k].bits;
    }
    if (n) sys_netsrv(NETSRV_READY, (u32)u, (u32)n);
}

#define TURN_MSGS 8                             /* requests served a turn before lwIP's timers and the lines again */

int main(void)
{
    struct netd_limits lim;
    unsigned char s[48];
    signal(SIGPIPE, SIG_IGN);                   /* a dead driver's pipe must not end netd (D19 2.3) */
    signal(SIGCHLD, on_child);                  /* a driver ended: the handler only sets a flag */
    port = sys_port_create("net");              /* the port first: the kernel's start deadline (D19 6.3) */
    if (port < 0) {
        dprintf(1, "netd: port net: %s\n", zstrerror(port));
        return 1;
    }
    epoch = (u32)sys_netsrv(NETSRV_HELLO, 0, 0);
    int prof = profile();
    netd_limits_profile(&lim, prof);
    reg_chunk = (u32)(lim.heap / 4 + 4095) & ~4095u;    /* the heap's regions: a quarter of the limit each */
    if (reg_chunk < 8192) reg_chunk = 8192;
    if (seed(s, "/dev/urandom")) {
        dprintf(1, "netd: cannot read /dev/urandom\n");
        return 1;
    }
    if (netd_init(&lim, s)) {
        dprintf(1, "netd: the limits are wrong\n");
        return 1;
    }
    memset(s, 0, sizeof s);
    last_seed = sys_ticks();
    rnd_fd = sys_open("/dev/random", O_RDONLY | O_NONBLOCK);
    load_config();
    if_up("");                                  /* the "auto" interfaces (D19 6.3: after the port is open) */
    probe_start();                              /* and the cards no stanza names */
    dprintf(1, "netd: serving net, epoch %u, %s profile (%u KB of buffers)\n", epoch,
           prof == NETD_PROFILE_LARGE ? "large" : "small", (u32)(lim.heap / 1024));
    for (;;) {
        unsigned long ms = netd_turn();
        if (child_ended) drv_reap();
        if (idle_now()) {                       /* a clean stop: the next socket call starts netd again (D11) */
            dprintf(1, "netd: no network card found%s%s%s and none configured: netd stops (it starts again when a program "
                    "needs the network)\n", probe_none[0] ? " (no card for " : "", probe_none, probe_none[0] ? ")" : "");
            return 0;
        }
        u32 dn = drv_starts();
        if (dn != 0x7fffffff && ms > dn * 10) ms = dn * 10;
        drv_bells();
        push();
        struct pollfd pf[2 + NETD_IFMAX];
        int h[NETD_IFMAX], ww[NETD_IFMAX], nh = netd_wait_handles(h, ww, NETD_IFMAX), np = 0;
        pf[np].fd = port; pf[np].events = POLLPORT | POLLIN; pf[np].revents = 0; np++;
        for (int k = 0; k < nh; k++) {
            pf[np].fd = h[k];
            pf[np].events = (short)(POLLIN | (ww[k] ? POLLOUT : 0));
            pf[np].revents = 0;
            np++;
        }
        if (rnd_fd >= 0) { pf[np].fd = rnd_fd; pf[np].events = POLLIN; pf[np].revents = 0; np++; }
        u32 since = sys_ticks() - last_seed;
        if (since >= RESEED) { reseed("/dev/urandom"); since = 0; }
        if (ms > (RESEED - since) * 10) ms = (RESEED - since) * 10;
        sys_evwait(pf, (u32)np, (int)((ms + 9) / 10));
        if (rnd_fd >= 0 && (pf[np - 1].revents & POLLIN)) {    /* the pool is seeded: keys from it now */
            sys_close(rnd_fd);
            rnd_fd = -1;
            reseed("/dev/random");
        }
        for (int k = 0; k < TURN_MSGS; k++) {
            struct umsg *m = sys_recv(port, 0);
            if ((int)m < 0 && (int)m > -4096) return 0;         /* the port is gone: a kill between requests */
            if (!m) break;
            answer(m);
        }
    }
}
