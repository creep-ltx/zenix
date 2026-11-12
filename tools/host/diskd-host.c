/* SPDX-License-Identifier: GPL-2.0-only */
/* The disk server (user/diskd.c) on the host, over an image file (HOSTDISK) as the whole disk "ide0", driven by
 * commands on stdin, one per line (tools/disk-host-test.sh):
 *   list                    DISK_LIST: every device with its handle (slot), start, sectors, mounted pid
 *   open NAME plain|raw|mount PID     DISK_OPEN from process PID
 *   write H LBA PID | read H LBA [PID]   one sector through handle H (a read from process PID, default 1)
 *   unmount H PID | rescan | flush H
 *   dead PID                that process no longer exists (sys_procinfo stops listing it)
 *   restart                 diskd ends and is started again (DESIGN D11): a fresh state, then its saved table
 *   damage                  flip a byte of the saved table (the next restart must refuse it)
 *   as UID                  later requests come from this user id (0 = root, the default)
 *   sh COMMAND              run a host command (to change the image between steps)
 *   readx H LBA N FILE      N sectors through handle H into the host file FILE
 * A CD drive (DESIGN D44) when HOSTCD names an image: a model of the ATAPI drive behind SYS_atapi (TEST UNIT READY,
 * REQUEST SENSE, READ CAPACITY, READ (10), with the kernel's window of kept bytes), and commands that change its state:
 *   cd change               a disc changed: the next command answers UNIT ATTENTION
 *   cd eject | cd insert    no disc (NOT READY, medium not present) | the disc back, with a unit attention
 *   cd becoming N           the next N commands answer NOT READY, becoming ready
 * A USB stick (DESIGN D46) when HOSTUSB names an image: a stub of the USB server behind the posts of kernel/diskproto.h
 * (it reads the sectors asked into its bounce buffer and posts UR_DONE), and commands for it:
 *   usb hello | usb hello-other   the server says hello (DISK_USBHELLO) | a process that does not own port usb does
 *   usb add | usb del             the stick plugged in (UR_ADD, labelled "Corsair GTX") | pulled (UR_DEL)
 *   usb dead | usb alive          the server stops answering requests | answers again
 *   usb fail N                    the next N requests answer -EIO
 *   usb late N                    answers come only at diskd's N-th wait for them (0: at once)
 *   usb flood N                   N records of news posted at once (more than the window holds: some are lost)
 *   usb beat                      the server's heartbeat: a diskd without our window (it started again) gets the hello
 * Every command prints one line "COMMAND LINE -> RESULT". */
#include "zhost.h"
#include "../../kernel/diskproto.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int fd = -1;
static u32 nsec;
static u32 deadpid[16]; static int ndead;
static int flush_result;
/* the disk is read as the 68k sees it: longs are big-endian (the RDB code reads longs) */
static void swap(void *buf, u32 n) { u32 *w = buf; for (u32 i = 0; i < n * 128; i++) w[i] = __builtin_bswap32(w[i]); }

/* ---- the USB server's stub (D46): the posts both ways, the sectors from HOSTUSB ---- */
#include <poll.h>
#define POLLPORT 0x4000
#define POST_BUF 1
#define POST_WRITE 2
#define PEER_PORT 4
struct upostbuf { u32 addr, size; int from; };
struct upost { u32 off, src, len; };
#define USBPORT 77
static int usbfd = -1, usb_dead, usb_fail, usb_other, usb_reads, usb_late, usb_waits;
static struct urec pending;             /* a request whose answer is held back (usb late) */
static int npending;
static u32 usb_nsec, usb_seq;
static struct urec *dwin;               /* diskd's window (its POST_BUF) */
static u8 bounce[512 * 128];
static const char label[32] = "Corsair GTX";
static void to_diskd(struct urec r)
{
    if (!dwin) return;
    r.seq = ++usb_seq;
    dwin[r.seq % UREC_N] = r;
}
static int usb_answer(const struct urec *q);
int sys_post(int op, int port, void *arg)
{
    if (op == POST_BUF) { const struct upostbuf *b = arg; if (b->from != USBPORT) return -1; dwin = (struct urec *)b->addr; return 0; }
    if (op != POST_WRITE || port != USBPORT || !dwin) return -104;
    const struct upost *w = arg;
    const struct urec *q = (const struct urec *)w->src;
    if (q->kind != UR_READ || usb_dead) return 0;               /* a dead server takes the post and never answers */
    if (usb_late) { pending = *q; npending = 1; usb_waits = 0; return 0; }
    return usb_answer(q);
}
static int usb_answer(const struct urec *q)
{
    usb_reads++;
    int res = 0;
    if (usb_fail) { usb_fail--; res = -5; }
    else if (q->unit || q->count > 128 || q->lba + q->count > usb_nsec ||
             pread(usbfd, bounce, q->count * 512, (off_t)q->lba * 512) != (ssize_t)(q->count * 512)) res = -5;
    /* the bytes as they are on the stick (no swap as sys_blk_read's): the tables read from it here are MBRs, read bytewise */
    to_diskd((struct urec){ UR_DONE, 0, 0, 0, 0, res, q->seq, 0 });
    return 0;
}
int sys_peer(void *m, int op, void *arg) { (void)m; return op == PEER_PORT && (int)(long)arg == USBPORT && !usb_other ? 0 : -1; }
int sys_evwait(struct pollfd *f, u32 n, int ticks)
{
    (void)f; (void)n; (void)ticks;
    if (npending && ++usb_waits >= usb_late) { npending = 0; usb_answer(&pending); }
    return 1;
}
static int port_find(const char *name) { return !strcmp(name, "usb") ? USBPORT : 1; }
#define sys_port_find port_find
int sys_blk_read(u32 lba, u32 n, void *buf)
{
    if (lba + n > nsec || pread(fd, buf, n * 512, (off_t)lba * 512) != (ssize_t)n * 512) return -1;
    swap(buf, n);
    return 0;
}
int sys_blk_write(u32 lba, u32 n, const void *buf)
{
    static u8 t[512 * 64];
    if (lba + n > nsec || n > 64) return -1;
    memcpy(t, buf, n * 512); swap(t, n);
    return pwrite(fd, t, n * 512, (off_t)lba * 512) != (ssize_t)n * 512 ? -1 : 0;
}
int sys_blk_size(void) { return (int)nsec; }
int sys_blk_ram(u32 *r) { r[0] = r[1] = 0; return -1; }
int sys_blk_flush(void) { return flush_result; }
int sys_fd_info(u32 d) { (void)d; return 0; }
int sys_fd_rw(u32 d, u32 lba, u32 n, void *buf, int w) { (void)d; (void)lba; (void)n; (void)buf; (void)w; return -1; }
int sys_procinfo(u32 i, struct uproc *p)                  /* processes 1..40 exist unless declared dead */
{
    if (i >= 40) return -1;
    memset(p, 0, sizeof *p);
    p->pid = i + 1; p->state = 1;
    for (int k = 0; k < ndead; k++) if (deadpid[k] == p->pid) p->state = 0;
    return 0;
}

static u8 saved[512]; static u32 nsaved, starts = 1;
int sys_svc_self(void) { return (int)starts; }

/* the CD drive's model (D44) */
static int cdfd = -1, cd_ua, cd_out, cd_becoming;
static u32 cd_blocks, cd_cmds;
int sys_atapi(void *qv)
{
    struct uatapi *q = qv;
    static u8 blk[2048 * 40];
    u32 n = 0, key = 0, asc = 0;
    static u32 last_key, last_asc;
    q->err = 0;
    if (cdfd < 0) return -ENODEV;
    if ((q->len | q->skip) & 1) return -EINVAL;
    cd_cmds++;
    const u8 *c = q->cdb;
    if (c[0] == 0x03) {                                   /* REQUEST SENSE: the last error's, then cleared */
        memset(blk, 0, 18); blk[0] = 0x70; blk[2] = (u8)last_key; blk[7] = 10; blk[12] = (u8)last_asc;
        n = c[4] < 18 ? c[4] : 18;
        last_key = last_asc = 0;
    } else if (cd_ua) { key = 6; asc = 0x28; cd_ua = 0; }
    else if (cd_out) { key = 2; asc = 0x3A; }
    else if (cd_becoming) { key = 2; asc = 4; cd_becoming--; }
    else if (c[0] == 0x00) n = 0;
    else if (c[0] == 0x25) {
        u32 last = cd_blocks - 1;
        blk[0] = (u8)(last >> 24); blk[1] = (u8)(last >> 16); blk[2] = (u8)(last >> 8); blk[3] = (u8)last;
        blk[4] = 0; blk[5] = 0; blk[6] = 8; blk[7] = 0;
        n = 8;
    } else if (c[0] == 0x28) {
        u32 lba = (u32)c[2] << 24 | (u32)c[3] << 16 | (u32)c[4] << 8 | c[5], cnt = (u32)c[7] << 8 | c[8];
        if (lba + cnt > cd_blocks || cnt > 40) { key = 5; asc = 0x21; }
        else if (pread(cdfd, blk, cnt * 2048, (off_t)lba * 2048) != (ssize_t)(cnt * 2048)) { key = 3; asc = 0x11; }
        else n = cnt * 2048;
    } else { key = 5; asc = 0x20; }
    if (key) { last_key = key; last_asc = asc; q->err = key << 4 | 4; return -EIO; }
    for (u32 i = 0; i < n; i++)                           /* the kernel's window: bytes [skip, skip + len) kept */
        if (i >= q->skip && i - q->skip < q->len) ((u8 *)q->buf)[i - q->skip] = blk[i];
    return (int)n;
}
int sys_svc_save(const void *buf, u32 n) { if (n > sizeof saved) return -7; memcpy(saved, buf, n); nsaved = n; return 0; }
int sys_svc_load(void *buf, u32 n) { if (n > nsaved) n = nsaved; memcpy(buf, saved, n); return (int)n; }

#define main diskd_main
#include "../../user/diskd.c"
#undef main
#undef sys_port_find
/* what diskd's main loop does with the server's news */
static void usb_loop(void) { usb_take(0); if (usb_news) { usb_news = 0; scan(); } }

static unsigned cur_uid;
static int req(u32 type, u32 a0, u32 a1, u32 a2, u32 a3, int from)
{
    struct umsg m;
    memset(&m, 0, sizeof m);
    m.type = type; m.a[0] = a0; m.a[1] = a1; m.a[2] = a2; m.a[3] = a3; m.from = from; m.uid = (unsigned short)cur_uid;
    return handle(&m);
}

int main(void)
{
    const char *cp = getenv("HOSTCD");
    if (cp && (cdfd = open(cp, O_RDONLY)) >= 0) { struct stat cs; fstat(cdfd, &cs); cd_blocks = (u32)(cs.st_size / 2048); cd_ua = 1; }
    const char *up = getenv("HOSTUSB");
    if (up && (usbfd = open(up, O_RDONLY)) >= 0) { struct stat us; fstat(usbfd, &us); usb_nsec = (u32)(us.st_size / 512); }
    const char *p = getenv("HOSTDISK");
    if (!p || (fd = open(p, O_RDWR)) < 0) { fprintf(stderr, "set HOSTDISK\n"); return 9; }
    struct stat st; fstat(fd, &st); nsec = (u32)(st.st_size / 512);
    if (getenv("NOFLUSH")) flush_result = -95;
    disk_sectors = nsec;
    disk_caps = sys_blk_flush() == 0 ? DISKCAP_FLUSH : 0;
    scan();
    char line[512];
    static u8 sec[512];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        char whole[512];
        strcpy(whole, line);
        if (!strncmp(line, "sh ", 3)) { int r = system(line + 3); printf("sh -> %d\n", r); fflush(stdout); continue; }
        char *cmd = strtok(line, " "), *a = strtok(0, " "), *b = strtok(0, " "), *c = strtok(0, " ");
        if (!cmd || cmd[0] == '#') continue;
        int r;
        if (!strcmp(cmd, "list")) {
            struct diskinfo d;
            for (u32 i = 0; req(DISK_LIST, i, (u32)&d, 0, 0, 1) == 0; i++)
                printf("list -> %s h%d start %u sectors %u type %08x mounted %u caps %u\n", d.name, find(d.name), d.start, d.sectors, d.dostype, d.mounted, d.caps);
            continue;
        }
        if (!strcmp(cmd, "open")) r = req(DISK_OPEN, (u32)a, !strcmp(b, "raw") ? DISK_RAW : !strcmp(b, "mount") ? DISK_MOUNT : 0, 0, 0, atoi(c));
        else if (!strcmp(cmd, "write")) { memset(sec, 0x5A, 512); r = req(DISK_WRITE, (u32)atoi(a), (u32)atoi(b), 1, (u32)sec, atoi(c)); }
        else if (!strcmp(cmd, "read")) r = req(DISK_READ, (u32)atoi(a), (u32)atoi(b), 1, (u32)sec, c ? atoi(c) : 1);
        else if (!strcmp(cmd, "readx")) {
            static u8 big[512 * 256];
            char *f = strtok(0, " ");
            u32 n = (u32)atoi(c);
            r = n > 255 ? -1 : req(DISK_READ, (u32)atoi(a), (u32)atoi(b), n, (u32)big + 1, 1);   /* an odd buffer, as a
                                                                                                    * program's may be */
            FILE *o = f ? fopen(f, "wb") : 0;
            if (o) { if (!r) fwrite(big + 1, 512, n, o); fclose(o); }
        }
        else if (!strcmp(cmd, "cd")) {
            r = 0;
            if (!strcmp(a, "change")) cd_ua = 1;
            else if (!strcmp(a, "eject")) cd_out = 1;
            else if (!strcmp(a, "insert")) { cd_out = 0; cd_ua = 1; }
            else if (!strcmp(a, "becoming")) cd_becoming = atoi(b);
            else r = -1;
        }
        else if (!strcmp(cmd, "usb")) {
            r = 0;
            if (!strcmp(a, "hello") || !strcmp(a, "hello-other")) {
                usb_other = a[5] == '-';
                usb_seq = 0;
                r = req(DISK_USBHELLO, USBPORT, 0, 0, 0, 2);
            } else if (!strcmp(a, "add")) { to_diskd((struct urec){ UR_ADD, 0, 0, usb_nsec, 128, 0, (u32)bounce, (u32)label }); usb_loop(); }
            else if (!strcmp(a, "del")) { to_diskd((struct urec){ UR_DEL, 0, 0, 0, 0, 0, 0, 0 }); usb_loop(); }
            else if (!strcmp(a, "dead")) usb_dead = 1;
            else if (!strcmp(a, "alive")) usb_dead = 0;
            else if (!strcmp(a, "fail")) usb_fail = atoi(b);
            else if (!strcmp(a, "reads")) r = usb_reads;
            else if (!strcmp(a, "late")) usb_late = atoi(b);
            else if (!strcmp(a, "flood")) {
                for (int k = 0; k < atoi(b); k++)
                    to_diskd((struct urec){ k & 1 ? UR_ADD : UR_DEL, 0, 0, usb_nsec, 128, 0, (u32)bounce, (u32)label });
                usb_loop();
            }
            else if (!strcmp(a, "beat")) {                       /* usbd's heartbeat: a len-0 post fails without a window */
                if (!dwin) {
                    usb_seq = 0;
                    r = req(DISK_USBHELLO, USBPORT, 0, 0, 0, 2);
                    to_diskd((struct urec){ UR_ADD, 0, 0, usb_nsec, 128, 0, (u32)bounce, (u32)label });
                    usb_loop();
                }
            }
            else r = -1;
        }
        else if (!strcmp(cmd, "unmount")) r = req(DISK_UNMOUNT, (u32)atoi(a), 0, 0, 0, atoi(b));
        else if (!strcmp(cmd, "rescan")) r = req(DISK_RESCAN, 0, 0, 0, 0, 1);
        else if (!strcmp(cmd, "flush")) r = req(DISK_FLUSH, (u32)atoi(a), 0, 0, 0, 1);
        else if (!strcmp(cmd, "as")) { cur_uid = (unsigned)atoi(a); r = 0; }
        else if (!strcmp(cmd, "dead")) { if (ndead < 16) deadpid[ndead++] = (u32)atoi(a); r = 0; }
        else if (!strcmp(cmd, "restart")) {           /* a new diskd: nothing but what it saved */
            memset(dev, 0, sizeof dev); memset(used, 0, sizeof used); memset(phys, 0, sizeof phys);
            memset(order, 0, sizeof order); norder = 0; fd_probe = 1; starts++;
            usb_port = 0; usb_sectors = 0; dwin = 0;                /* D46: a new diskd knows no USB server */
            for (int s = 0; s < MAXDEV; s++) fdrive[s] = -1;
            r = restore();
            if (!r) scan();
        }
        else if (!strcmp(cmd, "damage")) { saved[nsaved / 2] ^= 0x5A; r = 0; }
        else { printf("%s -> unknown\n", cmd); continue; }
        printf("%s -> %d\n", whole, r);
        fflush(stdout);
    }
    return 0;
}
