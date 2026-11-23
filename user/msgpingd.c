/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "../kernel/netproto.h"
/* test server: port "ping" (or -port NAME); type 1 = add a[0]+a[1] into a[2]; type 2 = copy the client's string a[0]
 * (pointer into the client's memory) to upper case in place; type 3 = answer with its sys_svc_self() (the start count and
 * SVC_MIDREQ: audit A8's test); type 7 = wait a second, then crash (an illegal
 * instruction: the service manager's tests, DESIGN D11); type 9 = quit. Exits after serving COUNT requests if given.
 * -call NAME: call port NAME once before creating its own (a start-up that needs another service: D11's tests).
 * -fs: a filesystem server that hangs on sync: FS_ROOT answers 2, FS_SYNC never returns, FS_QUIT ends it, anything
 * else fails (a reboot must not wait for it for ever: audit A7, tools/svc-test.sh). -fsstall: the same, but every
 * request other than FS_ROOT, FS_CLOSE and FS_QUIT never returns (a program that opens a file there is held in the
 * kernel until the server ends: wasabid's "freed", tools/remote-test.sh). -fsbad: the same, but FS_ROOT
 * fails too (a mount that fails must end its server: audit A9). -fslinked: a filesystem server that answers DESIGN D26's
 * FS_LINKED wrongly: FS_OPEN of anything but its root and /f -1025 (FSL_A1 for a one-path request: outside the table),
 * FS_READ and FS_READDIR -1024 (requests that never ask for it), anything else -1024 with the path left as it is (a
 * valid answer: the kernel goes on at that text, from /); its root opens as a directory, /f as a file; FS_ROOT 2,
 * FS_SYNC and FS_CLOSE 0, FS_QUIT ends it. -fsdir: an older filesystem server (DESIGN D49's fallback): its root lists
 * ".", ".." and n00 .. n49 (inodes 10 .. 59, files) through FS_READDIR only, FS_READDIRS is -ENOSYS as every
 * request it does not know; FS_OPEN, FS_STAT, FS_LSTAT of the root or of a name, FS_FSTAT, FS_CLOSE, FS_SYNC, FS_QUIT.
 * -fswin (DESIGN D60 stage 2, tools/mmu-test.sh): a filesystem server whose files are WIN_SIZE bytes of the pattern
 * (i * 7 + i / 256) & 255, to test the kernel's copies and windows: f is read as asked; fwd is read by this server
 * from /tmp/wsrc straight into its client's buffer (a window passed on to the kernel and the root's server); over
 * writes one byte past the buffer, under one before it, after keeps the buffer and writes into it at its next
 * request (each must end this server with the protection line); a stat of forge tries forged and second replies and
 * says what they got; a stat of where says whether its path argument is a copy in its own message (the kernel's
 * stack never shown). -uid N: serve as user N (a server that may not see its root clients: refused, D60 5.8).
 * Type 10: the client's message, at a[0], rewritten while it is served (next, state, client, kflags), then a[2] 4242
 * and the result 7 (the kernel must not use what the client's memory says: D60 5.1).
 * DESIGN D60 stage 3 (tools/mmu-test.sh): its port is marked PEER_EXACT (usys.h), so with full protection it sees only
 * what its clients declare (SYS_callv); -old makes the port without the mark (an old server: D41's lend of the whole
 * client when it may debug it, 5.8). Type 11: writes the byte a[1] bytes after a[0] (one past a declared buffer);
 * type 13: SYS_peer PEER_RD of 4 bytes at a[0], the answer in a[2]; type 14: writes a[0] without its client having
 * declared it (a server touching memory it was not given).
 * DESIGN D60 stage 4 (tools/mmu-test.sh): -paula, registered as the service "audio" (svctest reg), claims Paula and
 * tries DRV_AUDIO with a channel in the kernel's memory, in its own chip RAM, past its end, in its own fast RAM, with
 * DMAEN, with an AUDxLCH alone; plays from its own chip RAM and frees it (the channel must stop) and turns the channel
 * on again without a new pointer; says every answer in one line, then writes Paula itself (which must end it).
 * DESIGN D60 stage 5: -paula -old, an old sound server (its port not marked): claims Paula and writes it itself, which
 * the custom chips' page granted at its claim allows (5.8), and says so; with MMU=STRICT it gets no page and ends. */
#define FS_OPEN 1
#define FS_READDIR 6
#define FS_CLOSE 24
#define FS_SYNC 13
#define FS_ROOT 22
#define FS_QUIT 23
#define FS_STAT 4
#define FS_FSTAT 5
#define FS_LSTAT 19
#define FS_READ 2
#define WIN_SIZE 20000u
static const char *winname[] = { "", "", "", "f", "fwd", "over", "under", "after", "forge", "where" };
static u32 win_ino(const char *p)               /* the root 2, the files 3 .. 9 */
{
    while (*p == '/') p++;
    if (!*p || !strcmp(p, ".")) return 2;
    for (u32 i = 3; i < 10; i++) if (!strcmp(p, winname[i])) return i;
    return 0;
}
static void win_fill(unsigned char *b, u32 off, u32 n) { for (u32 i = 0; i < n; i++) b[i] = (unsigned char)((off + i) * 7 + (off + i) / 256); }
static unsigned char *win_kept;                            /* "after": the buffer of its last read */
static int fswin(struct umsg *m, int port)
{
    u32 ino;
    if (win_kept && m->type != FS_READ) { win_kept[0] = 1; win_kept = 0; }     /* (the protection line ends it) */
    switch (m->type) {
    case FS_ROOT: return 2;
    case FS_SYNC: case FS_CLOSE: return 0;
    case FS_OPEN: case FS_STAT: case FS_LSTAT: {
        const char *p = (const char *)m->a[0];
        if (!(ino = win_ino(p))) return -2;
        if (ino == 8 && m->type != FS_OPEN) {   /* forge: forged pointers into the message, a reply, a second */
            m->next = (struct umsg *)0x1234; m->state = 3; m->client_ = 8; m->kflags |= 1; m->server = 99;
            int f1 = sys_reply((struct umsg *)((u32)m + 4), 0), f2 = sys_reply((struct umsg *)0x1000, 0);
            int r1 = sys_reply(m, -2), r2 = sys_reply(m, 0);
            dprintf(1, "msgpingd: forged replies %d %d, reply %d, second reply %d\n", f1, f2, r1, r2);
            return 1000000;                     /* (main takes no reply for this one) */
        }
        if (ino == 9 && m->type != FS_OPEN)
            dprintf(1, "msgpingd: the path is %s\n", m->a[0] - (u32)m < 1024 ? "a copy in my own message" : "elsewhere");
        u32 *st = (u32 *)(m->type == FS_OPEN ? m->a[3] : m->a[2]);
        memset(st, 0, 28);
        st[0] = ino; st[1] = ino == 2 ? 040755 : 0100644; st[2] = ino == 2 ? 0 : WIN_SIZE; st[6] = 1;
        return m->type == FS_OPEN ? (int)ino : 0;
    }
    case FS_FSTAT: {
        u32 *st = (u32 *)m->a[1];
        memset(st, 0, 28);
        st[0] = m->a[0]; st[1] = m->a[0] == 2 ? 040755 : 0100644; st[2] = m->a[0] == 2 ? 0 : WIN_SIZE; st[6] = 1;
        return 0;
    }
    case FS_READDIR: {
        struct udirent *d = (struct udirent *)m->a[2];
        u32 c = m->a[1];
        if (m->a[0] != 2) return -20;
        if (c >= 9) { *(u32 *)m->a[3] = 9; return 0; }
        memset(d, 0, sizeof *d);
        if (c < 2) { strcpy(d->name, c ? ".." : "."); d->ino = 2; d->type = 2; }
        else { strcpy(d->name, winname[c + 1]); d->ino = c + 1; d->type = 1; }
        *(u32 *)m->a[3] = c + 1;
        return 1;
    }
    case FS_READ: {
        unsigned char *b = (unsigned char *)m->a[2];
        u32 off = m->a[1], n = m->a[3];
        if (m->a[0] < 3 || m->a[0] > 9) return -21;
        if (off >= WIN_SIZE) return 0;
        if (n > WIN_SIZE - off) n = WIN_SIZE - off;
        if (m->a[0] == 4) {                     /* fwd: the root's server writes into the window */
            int fd = sys_open("/tmp/wsrc", 0);
            if (fd < 0) return fd;
            sys_lseek(fd, (int)off, 0);
            int r = sys_read(fd, b, n);
            sys_close(fd);
            return r;
        }
        win_fill(b, off, n);
        if (m->a[0] == 5) b[n] = 0x55;          /* over */
        if (m->a[0] == 6) b[-1] = 0x55;         /* under */
        if (m->a[0] == 7) win_kept = b;         /* after: written at the next request that is not a read (the close) */
        return (int)n;
    }
    }
    (void)port;
    return -38;
}
/* -fsdir's inode of PATH (from the root: "/", "", "." are the root, "nNN" and "/nNN" a file), 0 for none */
static u32 fsdir_ino(const char *p)
{
    while (*p == '/') p++;
    if (!*p || !strcmp(p, ".")) return 2;
    if (p[0] == 'n' && p[1] >= '0' && p[1] <= '4' && p[2] >= '0' && p[2] <= '9' && !p[3]) return 10 + (u32)((p[1] - '0') * 10 + p[2] - '0');
    return 0;
}
static void fsdir_stat(u32 ino, u32 *st)
{
    memset(st, 0, 28);                          /* struct ustat: ino, mode, size, mtime, uid, gid, links */
    st[0] = ino; st[1] = ino == 2 ? 040755 : 0100644; st[2] = ino == 2 ? 0 : 3; st[3] = 1700000000; st[6] = 1;
}
static int fsdir(struct umsg *m)
{
    u32 ino;
    switch (m->type) {
    case FS_ROOT: return 2;
    case FS_SYNC: case FS_CLOSE: return 0;
    case FS_OPEN:
        if (!(ino = fsdir_ino((const char *)m->a[0]))) return -2;
        fsdir_stat(ino, (u32 *)m->a[3]);
        return (int)ino;
    case FS_STAT: case FS_LSTAT:
        if (!(ino = fsdir_ino((const char *)m->a[0]))) return -2;
        fsdir_stat(ino, (u32 *)m->a[2]);
        return 0;
    case FS_FSTAT:
        if (m->a[0] != 2 && (m->a[0] < 10 || m->a[0] > 59)) return -116;
        fsdir_stat(m->a[0], (u32 *)m->a[1]);
        return 0;
    case FS_READDIR: {
        struct udirent *d = (struct udirent *)m->a[2];
        u32 c = m->a[1];
        if (m->a[0] != 2) return -20;
        if (c >= 52) { *(u32 *)m->a[3] = 52; return 0; }
        memset(d, 0, sizeof *d);
        if (c < 2) { strcpy(d->name, c ? ".." : "."); d->ino = 2; d->type = 2; }
        else { d->name[0] = 'n'; d->name[1] = (char)('0' + (c - 2) / 10); d->name[2] = (char)('0' + (c - 2) % 10); d->ino = 8 + c; d->type = 1; }
        *(u32 *)m->a[3] = c + 1;
        return 1;
    }
    }
    return -38;                                 /* FS_READDIRS too: the fallback is tested with this */
}
/* -paula: the hostile checks of DRV_AUDIO (DESIGN D60 6.2) */
static int aud(int claim, int n, const unsigned short *w)
{
    struct udrvaudio q;
    memset(&q, 0, sizeof q);
    q.n = (u32)n;
    for (int i = 0; i < n; i++) { q.w[i].reg = w[2 * i]; q.w[i].val = w[2 * i + 1]; }
    int r = sys_drv(DRV_AUDIO, (u32)claim, (u32)&q);
    return n && r > 0 ? 0 : r;                      /* (a batch's answer: its registers; 0 for the line) */
}
static void paula(int old)
{
    int c = sys_drv(DRV_CLAIM, DRV_PAULA, 0);
    if (old) {                                      /* an old sndd writes Paula itself */
        if (c < 0) { dprintf(1, "msgpingd: paula (old): claim %d\n", c); return; }
        *(volatile unsigned short *)0xDFF0A8 = 0;
        dprintf(1, "msgpingd: paula (old): wrote Paula myself\n");
        return;
    }
    char *chip = sys_memalloc_chip(8192), *fast = sys_memalloc(8192);
    if (c < 0 || !chip || !fast) { dprintf(1, "msgpingd: paula: claim %d, memory %d\n", c, chip && fast); return; }
    u32 k = 0x2000, a = (u32)chip, f = (u32)fast;      /* 0x2000: low chip RAM, the kernel's */
#define LC(x) 0xA0, (unsigned short)((x) >> 16), 0xA2, (unsigned short)(x), 0xA4
    unsigned short w1[] = { LC(k), 64, 0x96, 0x8001 }, w2[] = { LC(a), 64, 0xA6, 400, 0xA8, 0 },
                   w3[] = { LC(a + 8150), 64 }, w4[] = { LC(f), 64 }, w5[] = { 0x96, 0x8201 }, w6[] = { 0xA0, 0 },
                   w7[] = { 0x96, 0x8001 }, w8[] = { 0x96, 0x0001 };
#define N(w) (int)(sizeof w / 4)
    int r1 = aud(c, N(w1), w1), r2 = aud(c, N(w2), w2), r3 = aud(c, N(w3), w3), r4 = aud(c, N(w4), w4),
        r5 = aud(c, N(w5), w5), r6 = aud(c, N(w6), w6), r7 = aud(c, N(w7), w7);
    int on = aud(c, 0, 0) & 1;                      /* channel 0 plays from our chip RAM */
    sys_memfree(chip);
    int off = !(aud(c, 0, 0) & 1), r8 = aud(c, N(w7), w7);     /* stopped by the free; on again: no pointer checked */
    aud(c, N(w8), w8);
    dprintf(1, "msgpingd: paula kernel %d own %d past %d fast %d dmaen %d lch %d play %d on %d freed-off %d again %d\n",
            r1, r2, r3, r4, r5, r6, r7, on, off, r8);
    *(volatile unsigned short *)0xDFF0A8 = 0;       /* the chips' page is not ours: this ends us */
    dprintf(1, "msgpingd: paula: wrote Paula myself\n");
}

static const char USAGE[] = "usage: msgpingd [-port NAME] [-call NAME] [-uid N] [-old] [-paula]\n       [-fs | -fsstall | -fsbad | -fslinked | -fsdir | -fswin]\n       [count]\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    const char *pname = "ping";
    int left = -1, fs = 0, uid = -1, old = 0, pau = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-port") && i + 1 < argc) pname = argv[++i];
        else if (!strcmp(argv[i], "-fs")) fs = 1;
        else if (!strcmp(argv[i], "-fsbad")) fs = 2;
        else if (!strcmp(argv[i], "-fsstall")) fs = 4;
        else if (!strcmp(argv[i], "-fslinked")) fs = 3;
        else if (!strcmp(argv[i], "-fsdir")) fs = 5;
        else if (!strcmp(argv[i], "-fswin")) fs = 6;
        else if (!strcmp(argv[i], "-uid") && i + 1 < argc) uid = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-old")) old = 1;
        else if (!strcmp(argv[i], "-paula")) pau = 1;
        else if (!strcmp(argv[i], "-call") && i + 1 < argc) {
            struct umsg c;
            memset(&c, 0, sizeof c);
            c.type = 1;
            int r = sys_call(sys_port_find(argv[++i]), &c);
            if (r < 0) { dprintf(1, "msgpingd: %s: the call to %s failed (%d)\n", pname, argv[i], r); return 1; }
        } else left = atoi(argv[i]);
    }
    int port = old ? sys3(29, (u32)pname, 0, 0) : sys_port_create(pname);    /* (-old: not marked PEER_EXACT) */
    if (port < 0) { dprintf(1, "msgpingd: port busy\n"); return 1; }
    if (pau)
        paula(old);
    if (uid >= 0 && sys_setuid((u32)uid) < 0) { dprintf(1, "msgpingd: setuid %d failed\n", uid); return 1; }
    struct umsg *m = sys_recv(port, 1);
    for (;;) {
        if ((int)m == -1) return 1;
        int r = 0;
        if (fs == 5 && m->type == FS_QUIT) { sys_reply(m, 0); return 0; }
        if (fs == 5) { m = sys_replyrecv(m, fsdir(m), port); continue; }
        if (fs == 6 && m->type == FS_QUIT) { sys_reply(m, 0); return 0; }
        if (fs == 6) {
            int w = fswin(m, port);
            m = w == 1000000 ? sys_recv(port, 1) : sys_replyrecv(m, w, port);
            continue;
        }
        if (fs) {
            if (m->type == FS_SYNC && fs != 3) for (;;) sys_sleep(100);
            if (fs == 4 && m->type != FS_ROOT && m->type != FS_CLOSE && m->type != FS_QUIT) for (;;) sys_sleep(100);
            if (m->type == FS_QUIT) { sys_reply(m, 0); return 0; }
            int root = fs == 3 && m->type == FS_OPEN && !strcmp((const char *)m->a[0], "/");
            if (root || (fs == 3 && m->type == FS_OPEN && !strcmp((const char *)m->a[0], "/f"))) {
                u32 *st = (u32 *)m->a[3];               /* struct ustat: ino, mode, size, mtime, uid, gid, links */
                memset(st, 0, 28);
                st[0] = root ? 2 : 3; st[1] = root ? 040755 : 0100644; st[2] = root ? 0 : 10; st[6] = 1;
                r = (int)st[0];
            } else if (fs == 3)
                r = m->type == FS_ROOT ? 2 : m->type == FS_SYNC || m->type == FS_CLOSE ? 0 : m->type == FS_OPEN ? -1025 : -1024;
            else
                r = m->type == FS_ROOT && (fs == 1 || fs == 4) ? 2 : -38;
            m = sys_replyrecv(m, r, port);
            continue;
        }
        if (m->type == 1) m->a[2] = m->a[0] + m->a[1];
        else if (m->type == 2) { char *s = (char *)m->a[0]; for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; }
        else if (m->type == 3) r = sys_svc_self();
        else if (m->type == 7) { sys_sleep(100); __asm__ volatile("illegal"); }
        else if (m->type == 9) { sys_reply(m, 0); return 0; }
        else if (m->type == 10) {               /* the client's own message rewritten in flight */
            struct umsg *c = (struct umsg *)m->a[0];
            c->next = (struct umsg *)0x1234; c->state = 3; c->client_ = 0; c->kflags = 1; c->server = 0; c->from = 1;
            m->a[2] = 4242;
            r = 7;
        }
        else if (m->type == 11) ((volatile char *)m->a[0])[m->a[1]] = 1;
        else if (m->type == 13) {
            struct upeermem pm = { m->a[0], 4 };
            m->a[2] = (u32)sys_peer(m, PEER_RD, &pm);
        } else if (m->type == 14) *(volatile char *)m->a[0] = 1;
        else r = -1;
        if (left > 0 && --left == 0) { sys_reply(m, r); return 0; }
        m = sys_replyrecv(m, r, port);
    }
}
