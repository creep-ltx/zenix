/* SPDX-License-Identifier: GPL-2.0-only */
/* zinstall: put Zenix on a hard disk, CF or SD card, from the install floppy or an existing Zenix (DESIGN D35,
 * docs/d35-installer.md, docs/man/zinstall.txt). This file is the dialogue and the running of it; user/zinstall/table.c
 * decides what may be done to the tables and writes them, user/zinstall/ffsw.c writes the boot partition. Nothing here
 * writes a disk but through those two and the programs mkfs.ext2, mkfs.zenixfs and the filesystem servers, on the
 * partitions found by their exact place after the table was changed. */
#include "libz.h"
#include "pw.h"
#include "../kernel/diskproto.h"
#include "zpkgfmt.h"
#include "sha256.h"
#include "zinstall/table.h"
#include "zinstall/ffsw.h"
#include "miniz.h"

typedef unsigned char u8;

#define SETFILE "/etc/zinstall.set"
#define ZI "/mnt/zi"                    /* the new root while it is written */
#define ZH "/mnt/zh"                    /* its /home */
#define BEFORE "/var/lib/zinstall/table-before"     /* in the new root: how to take its table entries out again (--undo) */
#define KFILE "/lib/zinstall/zenix" /* the kernel, in the new root until the boot partition is written */
#define BFILES "/lib/zinstall/"     /* bootmenu, zenixboot */
#define ESKIP 1001                      /* the person said skip: optional packages never span two disks (tools/mkinstall.py), so nothing of it was written */
#define MINKB 350                       /* free memory it wants before it writes anything (measured: docs/d35-installer.md section 10) */
#define MINKB_ZFS 500

static int disk_port = -1;

/* ---- words ---- */
static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vdprintf(1, fmt, ap);
    va_end(ap);
}
void zf_say(const char *fmt, ...)       /* user/zpkgfmt.c's reasons */
{
    va_list ap;
    va_start(ap, fmt);
    say("zinstall: ");
    vdprintf(1, fmt, ap);
    va_end(ap);
    say("\n");
}
static char line[128];
static int ask(const char *prompt)      /* a line into line[], without its end; -1 at the end of input */
{
    say("%s", prompt);
    unsigned n = 0;
    for (;;) {
        char c;
        int r = sys_read(0, &c, 1);
        if (r <= 0) return -1;
        if (c == '\n' || c == '\r') break;
        if (n + 1 < sizeof line) line[n++] = c;
    }
    line[n] = 0;
    while (n && line[n - 1] == ' ') line[--n] = 0;
    char *s = line;
    while (*s == ' ') s++;
    if (s != line) memmove(line, s, strlen(s) + 1);
    return 0;
}
static const char *mbtext(u32 sectors)
{
    static char b[4][16];
    static int at;
    char *o = b[at++ & 3];
    if (sectors < 2048) snprintf(o, 16, "%u KB", sectors / 2);
    else if (sectors / 2048 < 10000) snprintf(o, 16, "%u MB", sectors / 2048);
    else snprintf(o, 16, "%u GB", sectors / 2097152);
    return o;
}
static u32 lowest = 0xFFFFFFFFu;
static u32 mem_now(void)
{
    u32 m[4];
    if (sys_meminfo(m)) return 0;
    u32 f = m[0] + m[1];
    if (f < lowest) lowest = f;
    return f;
}

/* ---- the disk server ---- */
static int dcall(u32 type, u32 a0, u32 a1, u32 a2, u32 a3)
{
    struct umsg m;
    m.type = type; m.a[0] = a0; m.a[1] = a1; m.a[2] = a2; m.a[3] = a3;
    return sys_call(disk_port, &m);
}
static int dlist(u32 i, struct diskinfo *d) { return dcall(DISK_LISTSZ, i, (u32)d, sizeof *d, 0); }
static int dinfo(int h, struct diskinfo *d) { return dcall(DISK_INFOSZ, (u32)h, (u32)d, sizeof *d, 0); }
static int dopen(const char *name, u32 flags) { return dcall(DISK_OPEN, (u32)name, flags, 0, 0); }
static void rescan(void) { dcall(DISK_RESCAN, 0, 0, 0, 0); }

static int diskh;                       /* the whole disk */
static int noflush;                     /* said once */
static int tio_rd(void *c, t_u32 lba, t_u32 n, void *buf) { (void)c; return dcall(DISK_READ, (u32)diskh, lba, n, (u32)buf); }
static int tio_wr(void *c, t_u32 lba, t_u32 n, const void *buf) { (void)c; return dcall(DISK_WRITE, (u32)diskh, lba, n, (u32)buf); }
static int tio_fl(void *c)
{
    (void)c;
    int r = dcall(DISK_FLUSH, (u32)diskh, 0, 0, 0);
    if (r == -EOPNOTSUPP || r == -95) {
        if (!noflush) say("Note: this disk cannot promise that a write has reached it; a power cut now could lose the last writes.\n");
        noflush = 1;
        return 0;
    }
    return r;
}
static const struct tio io = { 0, tio_rd, tio_wr, tio_fl };

static struct t_busy busy[20];
static int nbusy;
static void find_busy(void)
{
    struct diskinfo di;
    nbusy = 0;
    for (u32 i = 0; dlist(i, &di) == 0 && nbusy < (int)(sizeof busy / sizeof busy[0]); i++) {
        if (strncmp(di.name, "ide", 3) || !di.mounted) continue;
        busy[nbusy].start = di.start; busy[nbusy].sectors = di.sectors;
        nbusy++;
    }
}
/* the device at exactly this place: its name, or 0 */
static const char *dev_at(u32 start, u32 sectors)
{
    static struct diskinfo di;
    for (u32 i = 0; dlist(i, &di) == 0; i++)
        if (!strncmp(di.name, "ide", 3) && di.start == start && di.sectors == sectors && strchr(di.name, 'p')) return di.name;
    return 0;
}

/* ---- running programs ---- */
static int run(const char *path, char *a1, char *a2, char *a3)
{
    char *argv[5] = { (char *)path, a1, a2, a3, 0 };
    int pid = sys_spawn(path, argv, 0);
    if (pid < 0) { say("zinstall: cannot start %s (%d)\n", path, pid); return -1; }
    int st = sys_wait(pid);
    mem_now();
    return st;
}

/* ---- the source: a stream of bytes ---- */
#define MAXITEM 64
struct item { char *name; u32 off, size; u8 sha[32]; int extra; };
static struct item items[MAXITEM];
static int nitems, ndisks;
static u32 setid, data1, kern_off, kern_size;
static u8 kern_sha[32];
static char *settext;

static int read_set(void)
{
    struct ustat st;
    if (sys_stat(SETFILE, &st) < 0 || st.size > 16384) return -1;
    settext = xmalloc(st.size + 1);
    int fd = sys_open(SETFILE, O_RDONLY);
    if (fd < 0 || sys_read(fd, settext, st.size) != (int)st.size) { if (fd >= 0) sys_close(fd); return -1; }
    sys_close(fd);
    settext[st.size] = 0;
    for (char *l = settext, *e; *l; l = e) {
        for (e = l; *e && *e != '\n'; e++) ;
        if (*e) *e++ = 0;
        char *w[6];
        int n = 0;
        for (char *p = l; *p && n < 6;) {
            while (*p == ' ') p++;
            if (!*p) break;
            w[n++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = 0;
        }
        if (n == 2 && !strcmp(w[0], "id")) setid = (u32)strtoul(w[1], 0, 16);
        else if (n == 2 && !strcmp(w[0], "disks")) ndisks = atoi(w[1]);
        else if (n == 2 && !strcmp(w[0], "data1")) data1 = (u32)strtoul(w[1], 0, 10);
        else if (n == 4 && !strcmp(w[0], "kernel")) {
            kern_off = (u32)strtoul(w[1], 0, 10); kern_size = (u32)strtoul(w[2], 0, 10);
            if (zf_parse_sha(w[3], kern_sha)) return -1;
        } else if (n == 6 && !strcmp(w[0], "item")) {
            if (nitems == MAXITEM) { say("zinstall: more than %d items on the set\n", MAXITEM); return -1; }
            struct item *it = &items[nitems++];
            it->name = w[1]; it->off = (u32)strtoul(w[2], 0, 10); it->size = (u32)strtoul(w[3], 0, 10);
            if (zf_parse_sha(w[4], it->sha)) return -1;
            it->extra = !strcmp(w[5], "extra");
        }
    }
    if (!ndisks || !data1 || !kern_size || !nitems) { say("zinstall: %s is not whole\n", SETFILE); return -1; }
    return 0;
}

/* the floppies: the stream's bytes on each disk; disk 1's start at data1 (after its header sector) */
#define FLOPPY 901120u
#define HDR_OFF(n) ((n) == 1 ? data1 - 512 : 0u)
#define STREAM_OFF(n) ((n) == 1 ? data1 : 512u)
#define TRACKB (512u * 11)
static int fl_h = -1, fl_disk = 0;
static int skip_extras;
static u8 *trk;
static u32 trk_at = 0xFFFFFFFFu;

static int probe_drive(int drv, int want)
{
    static u8 h[512];
    char name[4] = { 'd', 'f', (char)('0' + drv), 0 };
    int hd = dopen(name, 0);
    if (hd < 0) return -1;
    if (dcall(DISK_READ, (u32)hd, HDR_OFF(want) / 512, 1, (u32)h)) return -1;
    if (memcmp(h, "ZSET", 4)) return -1;
    u32 id = (u32)h[4] << 24 | (u32)h[5] << 16 | (u32)h[6] << 8 | h[7];
    u32 n = (u32)h[8] << 24 | (u32)h[9] << 16 | (u32)h[10] << 8 | h[11];
    if (id != setid || (int)n != want) return -1;
    return hd;
}
/* the wanted disk in some drive: 0; -1 the person said skip (optional), or stop */
static int need_disk(int want, int optional)
{
    if (fl_disk == want && fl_h >= 0) return 0;
    trk_at = 0xFFFFFFFFu;
    for (;;) {
        for (int d = 0; d < 4; d++) {
            int h = probe_drive(d, want);
            if (h >= 0) { fl_h = h; fl_disk = want; return 0; }
        }
        say("\nPut install disk %d in a drive and press Return.%s\n", want, optional ? " Type skip to leave the rest out:\n(the rest can come later with zpkg -S)" : " Type stop to stop.");
        if (ask("> ")) return -1;
        if (optional && !strcmp(line, "skip")) { skip_extras = 1; return -1; }
        if (!optional && !strcmp(line, "stop")) return -1;
    }
}
static int fl_read_at(int disk, u32 off, u8 *buf, u32 n, int optional)
{
    if (!trk) trk = xmalloc(TRACKB);
    while (n) {
        if (need_disk(disk, optional)) return -1;
        u32 t = off / TRACKB * TRACKB;
        if (t != trk_at) {
            int tries = 0;
            for (;;) {
                if (dcall(DISK_READ, (u32)fl_h, t / 512, TRACKB / 512, (u32)trk) == 0) break;
                if (++tries < 3) continue;
                say("\nDisk %d cannot be read there (byte %u). Try the disk again and press Return, or type stop:\n", disk, off);
                if (ask("> ") || !strcmp(line, "stop")) return -1;
                fl_disk = 0; fl_h = -1;
                if (need_disk(disk, optional)) return -1;
                tries = 0;
            }
            trk_at = t;
        }
        u32 k = TRACKB - (off - t);
        if (k > n) k = n;
        memcpy(buf, trk + (off - t), k);
        buf += k; off += k; n -= k;
    }
    return 0;
}
/* a position of the stream: which disk, which byte */
static void stream_place(u32 pos, int *disk, u32 *off, u32 *room)
{
    u32 cap1 = FLOPPY - data1, cap = FLOPPY - 512;
    if (pos < cap1) { *disk = 1; *off = data1 + pos; *room = cap1 - pos; return; }
    pos -= cap1;
    *disk = (int)(pos / cap) + 2;
    *off = 512 + pos % cap;
    *room = cap - pos % cap;
}

/* where a bundle comes from: a piece of the floppy stream, or a file */
static int src_fd = -1;
static u32 src_pos, src_left;
static int src_optional;
static int src_read(u8 *buf, u32 n)
{
    if (n > src_left) return -EINVAL;
    src_left -= n;
    if (src_fd >= 0) {
        while (n) {
            int r = sys_read(src_fd, buf, n);
            if (r <= 0) return -EIO;
            buf += r; n -= (u32)r;
        }
        return 0;
    }
    while (n) {
        int d; u32 off, room;
        stream_place(src_pos, &d, &off, &room);
        u32 k = n < room ? n : room;
        if (fl_read_at(d, off, buf, k, src_optional)) return skip_extras ? ESKIP : -EIO;
        buf += k; n -= k; src_pos += k;
    }
    return 0;
}

/* ---- files in the new root ---- */
static char pbuf[ZPATH_MAX];
static const char *in_root(const char *p) { snprintf(pbuf, sizeof pbuf, ZI "%s", p); return pbuf; }
static int mkdir_p(const char *path)
{
    static char b[ZPATH_MAX];
    strncpy(b, path, sizeof b - 1); b[sizeof b - 1] = 0;
    for (char *s = b + 1; *s; s++) if (*s == '/') { *s = 0; sys_mkdir(b); *s = '/'; }
    int r = sys_mkdir(b);
    return r == -17 || r == 0 ? 0 : r;
}
static int write_all(int fd, const void *p, u32 n)
{
    while (n) {
        int w = sys_write(fd, p, n > 4096 ? 4096 : n);
        if (w <= 0) return w < 0 ? w : -EIO;
        p = (const u8 *)p + w; n -= (u32)w;
    }
    return 0;
}
static int put_file(const char *rootpath, const void *data, u32 n, u32 mode)
{
    const char *p = in_root(rootpath);
    sys_unlink(p);
    int fd = sys_open(p, O_WRONLY | O_CREAT | O_EXCL);
    if (fd < 0) return fd;
    int r = write_all(fd, data, n);
    sys_close(fd);
    if (!r) r = sys_chmod(p, mode);
    return r;
}

/* ---- a package's tar.gz, streamed ---- */
static u8 inbuf[4096];
struct tarst { int fd; u32 left, pad, hdr; struct zf_ent *e; struct sha256 c; u8 h[512]; int err; struct zf_pkg *k; };
static struct tarst tst;
static u32 octal(const u8 *p, int n, int *bad)
{
    u32 v = 0;
    int i = 0;
    while (i < n && p[i] == ' ') i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
        if (v >> 29) { *bad = 1; return 0; }
        v = v << 3 | (u32)(p[i] - '0');
    }
    return v;
}
static int ent_done(struct tarst *t)
{
    u8 h[32];
    sys_close(t->fd);
    t->fd = -1;
    sha256_final(&t->c, h);
    if (memcmp(h, t->e->sha, 32)) { say("zinstall: %s: its SHA-256 is not its manifest's\n", t->e->path); return -EINVAL; }
    const char *p = in_root(t->e->path);
    int r;
    if ((r = sys_chown(p, t->e->uid, t->e->gid)) < 0 || (r = sys_chmod(p, t->e->mode)) < 0) { say("zinstall: %s: owner or mode not set (%d)\n", t->e->path, r); return r; }
    t->e->kind = 'x';                   /* done */
    return 0;
}
static int tar_feed(struct tarst *t, const u8 *p, u32 n)
{
    while (n && !t->err) {
        if (t->left) {
            u32 k = n < t->left ? n : t->left;
            if (t->fd >= 0) {
                int r = write_all(t->fd, p, k);
                if (r) { say("zinstall: %s: cannot write it (%d)\n", t->e->path, r); return t->err = r; }
                sha256_update(&t->c, p, k);
            }
            t->left -= k; p += k; n -= k;
            if (!t->left && t->fd >= 0 && (t->err = ent_done(t))) return t->err;
            continue;
        }
        if (t->pad) { u32 k = n < t->pad ? n : t->pad; t->pad -= k; p += k; n -= k; continue; }
        u32 k = 512 - t->hdr;
        if (k > n) k = n;
        memcpy(t->h + t->hdr, p, k);
        t->hdr += k; p += k; n -= k;
        if (t->hdr < 512) continue;
        t->hdr = 0;
        if (!t->h[0]) continue;
        int bad = 0;
        u32 size = octal(t->h + 124, 12, &bad);
        if (bad || memcmp(t->h + 257, "ustar", 5)) { say("zinstall: package %s: a damaged tar header\n", t->k->name); return t->err = -EINVAL; }
        char name[ZF_PMAX + 1];
        u32 nl = 0;
        while (nl < 100 && t->h[nl]) nl++;
        if (t->h[345] || nl >= ZF_PMAX) { say("zinstall: package %s: a name too long\n", t->k->name); return t->err = -EINVAL; }
        name[0] = '/';
        memcpy(name + 1, t->h, nl); name[nl + 1] = 0;
        t->left = size;
        t->pad = (512 - size % 512) % 512;
        t->e = (t->h[156] == '0' || t->h[156] == 0) ? zf_find_ent(t->k, name) : 0;
        if (t->e && t->e->kind != 'f' && t->e->kind != 'c') t->e = 0;
        t->fd = -1;
        if (t->e) {
            if (size != t->e->size) { say("zinstall: %s: its archive's size is not its manifest's\n", t->e->path); return t->err = -EINVAL; }
            const char *pth = in_root(t->e->path);
            sys_unlink(pth);
            t->fd = sys_open(pth, O_WRONLY | O_CREAT | O_EXCL);
            if (t->fd < 0) { say("zinstall: %s: cannot create it (%d)\n", t->e->path, t->fd); return t->err = t->fd; }
            sha256_init(&t->c);
            if (!size && (t->err = ent_done(t))) return t->err;
        }
    }
    return t->err;
}
/* the tgz part (len bytes from the source) into the new root; its SHA-256 checked against want */
static int unpack(struct zf_pkg *k, u32 len, const u8 *want)
{
    static u8 *dict;
    static tinfl_decompressor *dec;
    if (!dict) dict = malloc(TINFL_LZ_DICT_SIZE);
    if (!dec) dec = malloc(sizeof *dec);
    if (!dict || !dec) { say("zinstall: not enough memory to unpack\n"); return -ENOMEM; }
    struct sha256 hs;
    sha256_init(&hs);
    u32 left = len, inn = 0, pos = 10;
    if (len < 18) return -EINVAL;
    u32 first = len < sizeof inbuf ? len : sizeof inbuf;
    int r = src_read(inbuf, first);
    if (r) return r;
    sha256_update(&hs, inbuf, first);
    left -= first; inn = first;
    const u8 *in = inbuf;
    if (in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) { say("zinstall: package %s: not gzip data\n", k->name); return -EINVAL; }
    u32 flg = in[3];
    if (flg & 4) { if (pos + 2 > inn) return -EINVAL; pos += 2 + (in[pos] | (u32)in[pos + 1] << 8); }
    if (flg & 8) while (pos < inn && in[pos++]) ;
    if (flg & 16) while (pos < inn && in[pos++]) ;
    if (flg & 2) pos += 2;
    if (pos >= inn) { say("zinstall: package %s: a damaged gzip header\n", k->name); return -EINVAL; }
    in += pos; inn -= pos;
    struct tarst *t = &tst;
    memset(t, 0, sizeof *t);
    t->fd = -1; t->k = k;
    tinfl_init(dec);
    size_t dofs = 0;
    for (;;) {
        if (!inn && left) {
            u32 want_n = left < sizeof inbuf ? left : sizeof inbuf;
            if ((r = src_read(inbuf, want_n))) { t->err = r; break; }
            sha256_update(&hs, inbuf, want_n);
            left -= want_n; inn = want_n; in = inbuf;
        }
        size_t in_n = inn, out_n = TINFL_LZ_DICT_SIZE - dofs;
        tinfl_status st = tinfl_decompress(dec, in, &in_n, dict, dict + dofs, &out_n, left ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        in += in_n; inn -= (u32)in_n;
        if (out_n && tar_feed(t, dict + dofs, (u32)out_n)) break;
        dofs = (dofs + out_n) & (TINFL_LZ_DICT_SIZE - 1);
        if (st == TINFL_STATUS_DONE) break;
        if (st < 0 || (st == TINFL_STATUS_NEEDS_MORE_INPUT && !left && !inn)) { say("zinstall: package %s: damaged compressed data\n", k->name); t->err = -EINVAL; break; }
    }
    if (t->fd >= 0) sys_close(t->fd);
    if (t->err) return t->err;
    while (left) {                      /* what remains of the part: gzip's end */
        u32 want_n = left < sizeof inbuf ? left : sizeof inbuf;
        if ((r = src_read(inbuf, want_n))) return r;
        sha256_update(&hs, inbuf, want_n);
        left -= want_n;
    }
    u8 got[32];
    sha256_final(&hs, got);
    if (memcmp(got, want, 32)) { say("zinstall: package %s: its archive's SHA-256 is not its table's\n", k->name); return -EINVAL; }
    for (int i = 0; i < k->nent; i++)
        if (k->ent[i].kind == 'f' || k->ent[i].kind == 'c') { say("zinstall: package %s: %s is not in its archive\n", k->name, k->ent[i].path); return -EINVAL; }
    return 0;
}

/* the directories and links of a manifest, before its files */
static int make_tree(struct zf_pkg *k)
{
    for (int i = 0; i < k->nent; i++) {
        struct zf_ent *e = &k->ent[i];
        if (e->kind == 'd') {
            const char *p = in_root(e->path);
            int r = mkdir_p(p);
            if (r) { say("zinstall: %s: cannot make it (%d)\n", e->path, r); return r; }
            sys_chown(p, e->uid, e->gid);
            sys_chmod(p, e->mode);
        } else if (e->kind == 'l') {
            const char *p = in_root(e->path);
            sys_unlink(p);
            int r = sys_symlink(e->target, p);
            if (r) { say("zinstall: %s: cannot make the link (%d)\n", e->path, r); return r; }
        } else {
            /* a file's folder exists? */
            static char dir[ZPATH_MAX];
            snprintf(dir, sizeof dir, ZI "%s", e->path);
            char *s = strrchr(dir, '/');
            if (s && s != dir) { *s = 0; int r = mkdir_p(dir); if (r) return r; }
        }
    }
    return 0;
}

/* one bundle from the source: its parts in the order of the bundle */
static u8 hdrbuf[ZF_HDR + ZF_MAXTOC * ZF_TOCE];
static int install_bundle(u32 total, const char *what)
{
    struct zf_bundle b;
    unsigned need = ZF_HDR, have = 0;
    int r;
    for (;;) {
        if (need > sizeof hdrbuf) return -EINVAL;
        if ((r = src_read(hdrbuf + have, need - have))) return r;
        have = need;
        if ((r = zf_bundle_head(&b, hdrbuf, have, total, &need)) != -EAGAIN) break;
    }
    if (r) return r;
    u32 at = have;
    struct zf_pkg pk;
    int havepk = 0;
    static u8 *scratch;
    if (!scratch) scratch = xmalloc(2048);
    /* the table first: bundle parts read in place below would overwrite hdrbuf only by the next bundle */
    for (unsigned i = 0; i < b.ntoc; i++) {
        char name[41], pname[25];
        unsigned off, size;
        const unsigned char *sha;
        if (zf_bundle_entry(&b, i, name, &off, &size, &sha)) return -EINVAL;
        if (off < at) { say("zinstall: %s: its parts are not in order\n", what); return -EINVAL; }
        while (at < off) {              /* padding */
            u32 k = off - at < 2048 ? off - at : 2048;
            if ((r = src_read(scratch, k))) return r;
            at += k;
        }
        int kind = zf_part_kind(name, pname);
        if (kind < 0) return -EINVAL;
        if (kind == 1) {                /* a manifest */
            if (size > ZF_MANMAX) { say("zinstall: %s: too big a manifest\n", pname); return -EINVAL; }
            char *t = malloc(size + 1);
            if (!t) { say("zinstall: not enough memory for the manifest of %s\n", pname); return -ENOMEM; }
            if ((r = src_read((u8 *)t, size))) { free(t); return r; }
            u8 h[32];
            sha256(t, size, h);
            if (memcmp(h, sha, 32)) { say("zinstall: %s: its manifest's SHA-256 is not its table's\n", pname); free(t); return -EINVAL; }
            t[size] = 0;
            /* the manifest as text goes to the new root's database (kept before it is cut up by the parser) */
            if (strncmp(pname, "kernel-", 7)) {
                static char mp[64];
                snprintf(mp, sizeof mp, "/var/lib/zpkg/%s.manifest", pname);
                mkdir_p(ZI "/var/lib/zpkg");
                if ((r = put_file(mp, t, size, 0644))) { say("zinstall: %s: cannot write it (%d)\n", mp, r); free(t); return r; }
            }
            if ((r = zf_manifest_parse(&pk, t, size, pname))) return r;
            havepk = 1;
            if ((r = make_tree(&pk))) { zf_pkg_free(&pk); return r; }
        } else if (kind == 2) {         /* its files */
            if (!havepk || strcmp(pk.name, pname)) { say("zinstall: %s: its files come before its manifest\n", pname); return -EINVAL; }
            say("  %s\n", pname);
            r = unpack(&pk, size, sha);
            zf_pkg_free(&pk);
            havepk = 0;
            if (r) return r;
        } else if (!strcmp(name, "boot/zenix")) {   /* a system bundle's kernel */
            mkdir_p(ZI "/lib/zinstall");
            const char *p = in_root(KFILE);
            sys_unlink(p);
            int fd = sys_open(p, O_WRONLY | O_CREAT | O_EXCL);
            if (fd < 0) { say("zinstall: cannot write the kernel (%d)\n", fd); return fd; }
            struct sha256 c;
            sha256_init(&c);
            u32 left = size;
            while (left && !r) {
                u32 k = left < sizeof inbuf ? left : sizeof inbuf;
                if (!(r = src_read(inbuf, k))) { sha256_update(&c, inbuf, k); r = write_all(fd, inbuf, k); left -= k; }
            }
            sys_close(fd);
            if (r) return r;
            u8 h[32];
            sha256_final(&c, h);
            if (memcmp(h, sha, 32)) { say("zinstall: the kernel's SHA-256 is not its table's\n"); return -EINVAL; }
        } else {                        /* something this version does not know: left out */
            u32 left = size;
            while (left) {
                u32 k = left < 2048 ? left : 2048;
                if ((r = src_read(scratch, k))) return r;
                left -= k;
            }
        }
        at = off + size;
        mem_now();
    }
    return 0;
}

/* the kernel lying on disk 1 (a raw image behind the boot block) into the new root */
static int floppy_kernel(void)
{
    say("  the kernel\n");
    mkdir_p(ZI "/lib/zinstall");
    const char *p = in_root(KFILE);
    sys_unlink(p);
    int fd = sys_open(p, O_WRONLY | O_CREAT | O_EXCL);
    if (fd < 0) { say("zinstall: cannot write the kernel (%d)\n", fd); return fd; }
    struct sha256 c;
    sha256_init(&c);
    u32 left = kern_size, off = kern_off;
    int r = 0;
    while (left && !r) {
        u32 k = left < sizeof inbuf ? left : sizeof inbuf;
        if (fl_read_at(1, off, inbuf, k, 0)) { r = -EIO; break; }
        sha256_update(&c, inbuf, k);
        r = write_all(fd, inbuf, k);
        off += k; left -= k;
    }
    sys_close(fd);
    if (r) { say("zinstall: cannot copy the kernel (%d)\n", r); return r; }
    u8 h[32];
    sha256_final(&c, h);
    if (memcmp(h, kern_sha, 32)) { say("zinstall: the kernel on disk 1 is not what the set says\n"); return -EINVAL; }
    return 0;
}

static int copy_from_floppies(int zenixfs)
{
    if (!zenixfs && floppy_kernel()) return -1;
    int notes = 0;
    for (int i = 0; i < nitems; i++) {
        struct item *it = &items[i];
        int zk = !strcmp(it->name, "kernel-zfs");
        if (zk && !zenixfs) continue;
        if (it->extra && skip_extras) { notes++; continue; }
        src_fd = -1; src_pos = it->off; src_left = it->size; src_optional = it->extra;
        /* the item's whole checksum needs its bytes once more; each part of a bundle has its own: those are checked */
        int r = install_bundle(it->size, it->name);
        if (r == ESKIP) { notes++; continue; }
        if (r) { say("zinstall: %s: failed (%d)\n", it->name, r); return r; }
    }
    if (notes) say("%d optional packages left out: zpkg -S brings them later.\n", notes);
    return 0;
}

static int copy_from_network(int zenixfs)
{
    static char dir[64];
    snprintf(dir, sizeof dir, ZI "/var/cache/zpkg/pkg");
    int r = mkdir_p(dir);
    if (r) { say("zinstall: cannot make %s (%d)\n", dir, r); return r; }
    say("Fetching the system and packages with zpkg...\n");
    r = run("/bin/zpkg", "--fetch", dir, zenixfs ? "--zfs" : 0);
    if (r) { say("zinstall: zpkg could not fetch them (%d)\n", r); return -EIO; }
    char **names;
    int n = dir_list(dir, &names);
    if (n < 0) return -EIO;
    /* the system first, then the rest in order: a manifest's files need its folders, and the base package has the common ones */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n; i++) {
            const char *f = names[i];
            int l = (int)strlen(f);
            if (l < 5 || strcmp(f + l - 4, ".zup")) continue;
            int sys = (!strncmp(f, "zenix-", 6) && isdigit((unsigned char)f[6])) || (!strncmp(f, "zenixz-", 7) && isdigit((unsigned char)f[7]));
            if ((pass == 0) != (sys == 1)) continue;
            static char path[128];
            snprintf(path, sizeof path, "%s/%s", dir, f);
            struct ustat st;
            int fd = sys_open(path, O_RDONLY);
            if (fd < 0 || sys_fstat(fd, &st) < 0) { dir_free(names, n); return -EIO; }
            src_fd = fd; src_left = st.size;
            r = install_bundle(st.size, f);
            sys_close(fd);
            src_fd = -1;
            if (r) { say("zinstall: %s: failed (%d)\n", f, r); dir_free(names, n); return r; }
        }
    dir_free(names, n);
    return 0;
}

/* ---- checking what is on the disk ---- */
static int sha_file(const char *path, u8 out[32], u32 *size)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return fd;
    struct sha256 c;
    sha256_init(&c);
    u32 tot = 0;
    for (;;) {
        int n = sys_read(fd, inbuf, sizeof inbuf);
        if (n < 0) { sys_close(fd); return n; }
        if (!n) break;
        sha256_update(&c, inbuf, (u32)n);
        tot += (u32)n;
    }
    sys_close(fd);
    sha256_final(&c, out);
    if (size) *size = tot;
    return 0;
}
/* every file of every installed manifest, read back from the disk. 0, or the number of problems */
static int verify_root(u32 *nfiles)
{
    char **names;
    static char mp[64];
    snprintf(mp, sizeof mp, ZI "/var/lib/zpkg");
    int n = dir_list(mp, &names), bad = 0;
    if (n < 0) { say("zinstall: cannot read the new root's package database\n"); return 1; }
    for (int i = 0; i < n; i++) {
        int l = (int)strlen(names[i]);
        if (l < 10 || strcmp(names[i] + l - 9, ".manifest")) continue;
        snprintf(pbuf, sizeof pbuf, ZI "/var/lib/zpkg/%s", names[i]);
        struct ustat st;
        if (sys_stat(pbuf, &st) < 0 || st.size > ZF_MANMAX) { bad++; continue; }
        char *t = malloc(st.size + 1);
        if (!t) { say("zinstall: not enough memory to read a manifest\n"); bad++; break; }
        int fd = sys_open(pbuf, O_RDONLY);
        if (fd < 0 || sys_read(fd, t, st.size) != (int)st.size) { if (fd >= 0) sys_close(fd); free(t); bad++; continue; }
        sys_close(fd);
        t[st.size] = 0;
        struct zf_pkg pk;
        if (zf_manifest_parse(&pk, t, st.size, names[i])) { bad++; continue; }
        for (int k = 0; k < pk.nent; k++) {
            struct zf_ent *e = &pk.ent[k];
            static char p[ZPATH_MAX];
            snprintf(p, sizeof p, ZI "%s", e->path);
            struct ustat s2;
            if (sys_lstat(p, &s2) < 0) { say("zinstall: check: %s is missing\n", e->path); bad++; continue; }
            if (e->kind == 'f' || e->kind == 'c') {
                u8 h[32];
                u32 sz;
                if (sha_file(p, h, &sz) || sz != e->size || memcmp(h, e->sha, 32)) { say("zinstall: check: %s does not read back as it was written\n", e->path); bad++; }
                (*nfiles)++;
            }
            if (e->kind != 'l' && (s2.mode & 07777) != (u32)e->mode) { say("zinstall: check: %s has the wrong mode\n", e->path); bad++; }
        }
        zf_pkg_free(&pk);
        mem_now();
    }
    dir_free(names, n);
    return bad;
}

/* ---- the boot partition ---- */
static int bp_h;
static int bp_wr(void *c, unsigned lba, unsigned n, const void *buf) { (void)c; return dcall(DISK_WRITE, (u32)bp_h, lba, n, (u32)buf); }
static int bp_rd(void *c, unsigned lba, unsigned n, void *buf) { (void)c; return dcall(DISK_READ, (u32)bp_h, lba, n, (u32)buf); }
static const struct ffsw_io bpio = { 0, bp_wr, bp_rd };
struct mem_src { const u8 *p; };
static int src_mem(void *c, unsigned char *b, unsigned n) { struct mem_src *m = c; memcpy(b, m->p, n); m->p += n; return 0; }
static int src_file(void *c, unsigned char *b, unsigned n)
{
    int fd = *(int *)c;
    while (n) {
        int r = sys_read(fd, b, n);
        if (r <= 0) return -1;
        b += r; n -= (u32)r;
    }
    return 0;
}
struct wrote { const char *dir, *name; unsigned size; u8 sha[32]; };
static struct wrote wrote[FFSW_MAXFILES];
static int nwrote;
static int add_text(const char *dir, const char *name, const char *text)
{
    struct wrote *w = &wrote[nwrote++];
    struct mem_src m = { (const u8 *)text };
    w->dir = dir; w->name = name; w->size = (unsigned)strlen(text);
    return ffsw_file(dir, name, w->size, src_mem, &m, w->sha);
}
static int add_host(const char *dir, const char *name, const char *rootpath)
{
    const char *p = in_root(rootpath);
    struct ustat st;
    if (sys_stat(p, &st) < 0) { say("zinstall: %s is not in the new root\n", rootpath); return -1; }
    int fd = sys_open(p, O_RDONLY);
    if (fd < 0) return -1;
    struct wrote *w = &wrote[nwrote++];
    w->dir = dir; w->name = name; w->size = st.size;
    int r = ffsw_file(dir, name, st.size, src_file, &fd, w->sha);
    sys_close(fd);
    return r;
}
static int write_boot(const struct t_plan *pl, const char *dev)
{
    bp_h = dopen(dev, DISK_RAW);
    if (bp_h < 0) { say("zinstall: cannot open %s (%d)\n", dev, bp_h); return -1; }
    u32 t = sys_time();
    u32 days = t > 252460800u ? (t - 252460800u) / 86400 : 0, mins = t > 252460800u ? (t - 252460800u) % 86400 / 60 : 0;
    if (ffsw_begin(&bpio, pl->nw[R_BOOT].sectors, "ZenixBoot", days, mins)) goto bad;
    static char conf[512];
    int dual = pl->aos[0] != 0;
    if (dual)
        snprintf(conf, sizeof conf,
                 "# S:BootMenu.conf - the boot menu (man bootmenu), written by zinstall.\n"
                 "# default 1: AmigaOS starts by itself after the countdown. default 2: Zenix.\n"
                 "timeout 5\ndefault 1\nAmigaOS = partition %s\nZenix = run SYS:zenixboot\n"
                 "Zenix (previous) = run SYS:zenixboot PREVIOUS\nKickstart shell = shell\n", pl->aos);
    else
        snprintf(conf, sizeof conf,
                 "# S:BootMenu.conf - the boot menu (man bootmenu), written by zinstall.\n"
                 "timeout 3\ndefault 1\nZenix = run SYS:zenixboot\n"
                 "Zenix (previous) = run SYS:zenixboot PREVIOUS\nKickstart shell = shell\n");
    if (ffsw_mkdir("S") || ffsw_mkdir("Zenix")) goto bad;
    if (add_text("S", "Startup-Sequence", "SYS:bootmenu\n") || add_text("S", "BootMenu.conf", conf)) goto bad;
    if (add_host("", "bootmenu", BFILES "bootmenu")) goto bad;
    if (dual && add_host("", "bootzenix", BFILES "bootmenu")) goto bad;
    if (add_host("", "zenixboot", BFILES "zenixboot")) goto bad;
    if (add_host("Zenix", "zenix", KFILE)) goto bad;
    if (ffsw_end()) goto bad;
    dcall(DISK_FLUSH, (u32)bp_h, 0, 0, 0);
    for (int i = 0; i < nwrote; i++) {
        u8 h[32];
        if (ffsw_read_sha(wrote[i].dir, wrote[i].name, wrote[i].size, h) || memcmp(h, wrote[i].sha, 32)) {
            say("zinstall: %s on the boot partition does not read back\n", wrote[i].name);
            return -1;
        }
    }
    return 0;
bad:
    say("zinstall: the boot partition: %s\n", ffsw_error());
    return -1;
}

/* ---- the machine's name and its accounts (after the copy and the check; DESIGN D35 section 7) ---- */
static const char *opt_host, *opt_rootpw, *opt_user, *opt_userpw, *opt_username;
static int opt_nouser;
static char hostname[64] = "zenix", username[32];

static int host_ok(const char *h)       /* 1-63 of a-z 0-9 -, not starting or ending with - */
{
    size_t l = strlen(h);
    if (!l || l > 63 || h[0] == '-' || h[l - 1] == '-') return 0;
    for (size_t i = 0; i < l; i++) if (!((h[i] >= 'a' && h[i] <= 'z') || (h[i] >= '0' && h[i] <= '9') || h[i] == '-')) return 0;
    return 1;
}
/* a password: the first line of FILE, or typed twice with no echo (read_secret, as passwd); 0, -1 stop */
static int get_password(const char *what, const char *file, char out[100])
{
    if (file) {
        int fd = sys_open(file, O_RDONLY), n = fd >= 0 ? sys_read(fd, out, 99) : -1;
        if (fd >= 0) sys_close(fd);
        if (n <= 0) { say("zinstall: cannot read the password file %s\n", file); return -1; }
        out[n] = 0;
        char *e = strchr(out, '\n'); if (e) *e = 0;
        if (!out[0]) { say("zinstall: %s is empty\n", file); return -1; }
        return 0;
    }
    static char again[100], prompt[80];
    for (;;) {
        snprintf(prompt, sizeof prompt, "%s (nothing shows as you type): ", what);
        sys_ttymode(TTY_RAW | TTY_NOSIG);   /* before the prompt shows: a key typed at once is not echoed by the cooked line (seen) */
        if (read_secret(prompt, out, 100)) return -1;
        if (!out[0]) { say("A password is needed here.\n"); continue; }
        sys_ttymode(TTY_RAW | TTY_NOSIG);
        if (read_secret("Again: ", again, sizeof again)) return -1;
        if (!strcmp(out, again)) return 0;
        say("The two were not the same. Once more.\n");
    }
}
static u32 hu_uid, hu_gid;              /* has_user's find: its numbers */
static int has_user(const char *path, const char *name)
{
    int n = 0;
    char *t = pw_slurp(path, &n);
    if (!t) return 0;
    u32 nl = strlen(name);
    int found = 0;
    for (char *l = t; *l && !found; ) {
        if (!strncmp(l, name, nl) && l[nl] == ':') {
            found = 1;
            char *f = l + nl + 1;                        /* name:hash:uid:gid */
            f = strchr(f, ':');
            hu_uid = f ? (u32)strtoul(f + 1, &f, 10) : 0;
            hu_gid = f && *f == ':' ? (u32)strtoul(f + 1, 0, 10) : 100;
        }
        char *e = strchr(l, '\n');
        if (!e) break;
        l = e + 1;
    }
    free(t);
    return found;
}
/* the questions; homedir: where the new /home is now (ZH, or the root's /home) */
static int accounts(const char *homedir)
{
    static char pw1[100], hash[100], comment[48];
    say("\nThe machine and its accounts.\n");
    if (opt_host) {
        if (!host_ok(opt_host)) { say("zinstall: %s: a name is 1-63 of a-z 0-9 -\n", opt_host); return -1; }
        strcpy(hostname, opt_host);
    } else for (;;) {
        if (ask("The machine's name (Return = zenix): ")) return -1;
        if (!line[0]) break;
        if (host_ok(line)) { strcpy(hostname, line); break; }
        say("1-63 of a-z, 0-9 and -, please (not starting or ending with -).\n");
    }
    static char hl[80];
    snprintf(hl, sizeof hl, "%s\n", hostname);
    if (put_file("/etc/hostname", hl, (u32)strlen(hl), 0644)) { say("zinstall: cannot write /etc/hostname\n"); return -1; }
    if (get_password("root's password", opt_rootpw, pw1)) return -1;
    pw_new_hash(pw1, hash);
    memset(pw1, 0, sizeof pw1);
    if (pw_set_hash_at("zinstall", ZI "/etc/passwd", "root", hash)) return -1;
    say("  root's password is set.\n");
    /* the first user */
    username[0] = 0;
    if (opt_user) {
        if (!pw_name_ok(opt_user)) { say("zinstall: %s: a name is 1-31 of a-z 0-9 _ - and starts with a letter or _\n", opt_user); return -1; }
        strcpy(username, opt_user);
    } else if (!opt_nouser) for (;;) {
        if (ask("Your own login name (Return = none now; useradd later): ")) return -1;
        if (!line[0]) break;
        if (pw_name_ok(line) && strcmp(line, "root")) { strcpy(username, line); break; }
        say("1-31 of a-z, 0-9, _ and -, starting with a letter or _, please.\n");
    }
    comment[0] = 0;
    if (username[0]) {
        if (opt_username) { strncpy(comment, opt_username, sizeof comment - 1); }
        else if (!opt_user) {
            for (;;) {
                if (ask("Your full name (Return = none): ")) return -1;
                if (pw_field_ok(line) && strlen(line) < sizeof comment) { strcpy(comment, line); break; }
                say("At most 47 characters, and no colon.\n");
            }
        }
        if (!pw_field_ok(comment)) { say("zinstall: a full name may not hold a colon\n"); return -1; }
        if (get_password(username, opt_userpw, pw1)) return -1;
        pw_new_hash(pw1, hash);
        memset(pw1, 0, sizeof pw1);
        static char home[64];
        snprintf(home, sizeof home, "/home/%s", username);
        u32 uid, gid;
        if (has_user(ZI "/etc/passwd", username)) {          /* one of the shipped accounts: it becomes yours */
            if (pw_set_hash_at("zinstall", ZI "/etc/passwd", username, hash)) return -1;
            uid = hu_uid; gid = hu_gid;
            static char hp[96];
            snprintf(hp, sizeof hp, "%s/%s", homedir, username);
            if (mkdir_p(hp) || sys_chown(hp, uid, gid) || sys_chmod(hp, 0700)) { say("zinstall: cannot make %s's home\n", username); return -1; }
            say("  %s was there already: its password is set, its home made.\n", username);
        } else {
            struct pw_add na = { username, "users", home, "/bin/sh", comment, "wheel,zine", hash, -1 };
            if (pw_add_at("zinstall", ZI "/etc/passwd", ZI "/etc/group", &na, &uid, &gid)) return -1;
            static char hp[96];
            snprintf(hp, sizeof hp, "%s/%s", homedir, username);
            if (mkdir_p(hp) || sys_chown(hp, uid, gid) || sys_chmod(hp, 0700)) { say("zinstall: cannot make %s's home\n", username); return -1; }
            say("  %s added: groups users, wheel (sudo) and zine (demos), home %s.\n", username, home);
        }
    }
    /* the shipped accounts without a password (user, guest) are locked, not left open to anyone at the console */
    static const char *const shipped[] = { "user", "guest" };
    for (int i = 0; i < 2; i++)
        if (strcmp(shipped[i], username) && has_user(ZI "/etc/passwd", shipped[i])) {
            if (pw_set_hash_at("zinstall", ZI "/etc/passwd", shipped[i], "*")) return -1;
            say("  the shipped account %s is locked (passwd %s opens it again).\n", shipped[i], shipped[i]);
        }
    return 0;
}

/* ---- the dialogue ---- */
static void show_disk(const struct tdisk *d)
{
    say("\nThe disk ide0: %s. It holds %s.\n", mbtext(d->sectors), d->what);
    if (d->noedit) say("Its table cannot be changed: %s.\n", d->noedit);
    if (d->nparts) say("  %-8s %-11s %9s  %-18s %s\n", "DEVICE", "NAME", "SIZE", "TYPE", "");
    for (int i = 0; i < d->nparts; i++) {
        const struct t_part *p = &d->part[i];
        say("  %-8s %-11s %9s  %-18s %s%s%s%s\n", p->dev, p->label[0] ? p->label : "-", mbtext(p->sectors), t_typename(p),
            p->vol[0] ? p->vol : "", p->bootable ? " boot" : "", p->busy ? " IN USE" : "", p->how == TP_RDB && p->nomount ? " nomount" : "");
    }
}
static void show_opt(int n, const struct tdisk *d, const struct t_opt *o)
{
    switch (o->kind) {
    case TO_NEWCONT: say("  %d  the free space, %s, in a new partition of the PC table (nothing else changes)\n", n, mbtext(o->sectors)); break;
    case TO_FREE: say("  %d  the free space, %s, in the Amiga table%s (nothing else changes)\n", n, mbtext(o->sectors), d->region[o->region].base ? " inside the card's container" : ""); break;
    case TO_PART: {
        const struct t_part *p = &d->part[o->part];
        say("  %d  REPLACE partition %s %s, %s, %s: what is on it is LOST\n", n, p->dev, p->label, mbtext(p->sectors), t_typename(p));
        break;
    }
    case TO_WHOLE: say("  %d  ERASE THE WHOLE DISK, %s: everything on it is LOST\n", n, mbtext(o->sectors)); break;
    }
}
static int has_cont(const struct tdisk *d)
{
    for (int i = 0; i < d->nparts; i++) if (d->part[i].how == TP_MBR && d->part[i].mbrtype == 0x76) return 1;
    return 0;
}
static void show_plan(const struct tdisk *d, const struct t_plan *p)
{
    say("\nWhat will change on ide0:\n");
    if (p->opt.kind == TO_PART) {
        const struct t_part *q = &d->part[p->opt.part];
        say("  Partition %s %s (%s, %s) is REPLACED: what is on it is LOST.\n", q->dev, q->label, mbtext(q->sectors), t_typename(q));
        if (q->vol[0]) say("  Its volume is called %s.\n", q->vol);
        if (q->bootable) say("  It is marked bootable: a system may be starting from it.\n");
    }
    if (p->opt.kind == TO_WHOLE) say("  THE WHOLE DISK IS ERASED: every partition and everything on them is LOST.\n");
    if (p->opt.kind == TO_NEWCONT) say("  A new partition of type 0x76 in the PC table, %s, holding an Amiga table with:\n", mbtext(p->opt.sectors));
    static const char *const what[3] = { "the boot menu and the kernel (AmigaDOS FFS)", 0, "/home (ext2)" };
    for (int i = 0; i < p->nnew; i++)
        say("  New %-4s %8s  %s\n", p->nw[i].label, mbtext(p->nw[i].sectors),
            i == R_ROOT ? (p->zenixfs ? "the Zenix root (ZenixFS)" : "the Zenix root (ext2)") : what[i]);
    say("  Sectors of the table written: ");
    for (int i = 0; i < p->nw1; i++) say("%u ", p->w1[i].lba);
    say("(then %u %u when all is checked)\n", p->w2[0].lba, p->w2[1].lba);
    if (p->opt.kind != TO_WHOLE) {
        int any = 0;
        for (int i = 0; i < d->nparts; i++)
            if (i != p->opt.part) {
                if (!any) say("  Not touched: ");
                else say(", ");
                say("%s", d->part[i].label[0] ? d->part[i].label : d->part[i].dev);
                any = 1;
            }
        if (any) say("\n");
        if (d->hasfat) say("  The FAT partition (Emu68, the Kickstart) is not touched.\n");
    }
    if (p->aos[0]) say("  At power-on a boot menu comes first: %s starts by itself after 5 seconds.\n", p->aos);
    if (d->kind == TD_MBR)
        say("  A PC table: only Emu68's SD driver starts from it. On any other Amiga start\n  Zenix from a boot floppy (%s).\n", p->zenixfs ? "zenix-zfs.adf" : "zenix.adf");
    if (p->opt.kind == TO_NEWCONT && has_cont(d))
        say("  NOT KNOWN: a second 0x76 next to the first has never been tried under Emu68.\n  If it is not seen, the first goes on as before; start Zenix from a floppy.\n");
    if (!p->aos[0] && p->opt.kind != TO_WHOLE)
        say("  No system that starts by itself was found on this disk: the menu has no AmigaOS\n  line. A system on another disk with a lower boot priority will not start first.\n");
    if (!p->atomic)
        say("  A cut power during the table writes can damage the old table before the new\n  one is whole here: the disk then holds neither.\n");
}

static int gone;                        /* what was chosen is lost: a partition, or the disk */
static void fail_note(int stage)
{
    say("\nStopped. ");
    if (!stage) say("Nothing was changed on the disk.\n");
    else if (gone) say("The new partitions are in the table, not bootable. What you chose to replace is\nalready lost. Run zinstall again and choose them.\n");
    else say("The new partitions are in the table, not bootable: the machine starts as before.\nRun zinstall again and choose them, or remove them (fdisk del on an Amiga table).\n");
}

static void umount_all(void)
{
    sys_sync();
    sys_umount(ZH);
    sys_umount(ZI);
}

static int list_only(void)
{
    static struct tdisk d;
    static struct t_opt o[T_MAXOPT];
    struct diskinfo di;
    int disk = dopen("ide0", 0);
    if (disk < 0 || dinfo(disk, &di) < 0) { say("zinstall: no hard disk found\n"); return 1; }
    diskh = disk;
    find_busy();
    if (t_read(&d, &io, di.sectors, busy, nbusy)) { say("zinstall: cannot read the disk\n"); return 1; }
    show_disk(&d);
    int n = t_options(&d, &io, o, T_MAXOPT);
    say("\nWhat could be done:\n");
    for (int i = 0; i < n; i++) show_opt(i + 1, &d, &o[i]);
    if (!n) say("  nothing\n");
    for (int i = 0; i < d.nparts; i++) {
        const char *w = t_part_refused(&d, i);
        if (w) say("  %s is not offered: %s\n", d.part[i].dev, w);
    }
    if (t_whole_refused(&d)) say("  erasing the disk is not offered: %s\n", t_whole_refused(&d));
    return 0;
}

/* The table as it was: the file: "ZTB1", then long: what was chosen (0 free space: undo possible), the commit's record, the records;
 * each a long, the sector's number, and the old bytes, the bytes stage 1 wrote and the bytes after stage 2 (the same if it did not change). */
struct tb_rec { u8 at[4]; u8 old[512], w1[512], w2[512]; };
static struct tb_rec tbr;
static int save_before(const struct t_plan *pl, int free_space)
{
    static char p[64];
    snprintf(p, sizeof p, ZI "%s", BEFORE);
    sys_unlink(p);
    int fd = sys_open(p, O_WRONLY | O_CREAT | O_EXCL), r;
    if (fd < 0) return fd;
    u8 h[16] = { 'Z', 'T', 'B', '1', 0, 0, 0, (u8)!free_space, 0, 0, 0, (u8)pl->commit1, 0, 0, 0, (u8)pl->nw1 };
    r = write_all(fd, h, sizeof h);
    for (int i = 0; i < pl->nw1 && !r; i++) {
        u32 l = pl->w1[i].lba;
        tbr.at[0] = (u8)(l >> 24); tbr.at[1] = (u8)(l >> 16); tbr.at[2] = (u8)(l >> 8); tbr.at[3] = (u8)l;
        memcpy(tbr.old, pl->w1[i].old, 512);
        memcpy(tbr.w1, pl->w1[i].data, 512);
        memcpy(tbr.w2, pl->w1[i].data, 512);
        for (int k = 0; k < pl->nw2; k++) if (pl->w2[k].lba == l) memcpy(tbr.w2, pl->w2[k].data, 512);
        r = write_all(fd, &tbr, sizeof tbr);
    }
    sys_close(fd);
    if (!r) r = sys_chmod(p, 0600);
    return r;
}
/* take the install out of the table again: every sector as it was, the one that made it real first; only if the table is still what
 * zinstall left. The new partitions' space then holds old data; nothing else changes. */
static int undo(const char *path)
{
    static u8 cur[512];
    u8 h[16];
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) { say("zinstall: cannot read %s (%d)\n", path, fd); return 1; }
    if (sys_read(fd, h, sizeof h) != (int)sizeof h || memcmp(h, "ZTB1", 4)) { say("zinstall: %s is not a table copy\n", path); sys_close(fd); return 1; }
    u32 chosen = h[7], commit = h[11], n = h[15];
    if (chosen) { say("zinstall: this install replaced a partition or the whole disk: what was there is gone, the old table would point at nothing. Nothing changed.\n"); sys_close(fd); return 1; }
    if (!n || n > T_MAXW || commit >= n) { say("zinstall: %s is damaged\n", path); sys_close(fd); return 1; }
    diskh = dopen("ide0", 0);
    if (diskh < 0) { say("zinstall: no hard disk found\n"); sys_close(fd); return 1; }
    static struct tb_rec *rec;
    if (!rec) rec = xmalloc(sizeof *rec * T_MAXW);
    for (u32 i = 0; i < n; i++) {
        if (sys_read(fd, &rec[i], sizeof *rec) != (int)sizeof *rec) { say("zinstall: %s is cut short\n", path); sys_close(fd); return 1; }
        u32 l = (u32)rec[i].at[0] << 24 | (u32)rec[i].at[1] << 16 | (u32)rec[i].at[2] << 8 | rec[i].at[3];
        if (tio_rd(0, l, 1, cur) || (memcmp(cur, rec[i].w1, 512) && memcmp(cur, rec[i].w2, 512))) {
            say("zinstall: sector %u of the table is not what zinstall left: it has changed since. Nothing changed.\n", l);
            sys_close(fd);
            return 1;
        }
    }
    sys_close(fd);
    say("This takes Zenix's new partitions out of the table of ide0; the rest is as before the install.\nType undo to go on. Anything else stops here.\n");
    if (ask("> ") || strcmp(line, "undo")) { say("Nothing was changed.\n"); return 0; }
    for (u32 pass = 0; pass < n; pass++) {          /* the one that made it real first: after it the rest is unreferenced */
        u32 i = pass == 0 ? commit : (pass <= commit ? pass - 1 : pass);
        u32 l = (u32)rec[i].at[0] << 24 | (u32)rec[i].at[1] << 16 | (u32)rec[i].at[2] << 8 | rec[i].at[3];
        if (tio_wr(0, l, 1, rec[i].old)) { say("zinstall: cannot write sector %u\n", l); return 1; }
        if (pass == 0) tio_fl(0);
    }
    tio_fl(0);
    rescan();
    say("Done: the table is as it was. Restart: the disk starts what it started before.\nThe space of the new partitions is free again and holds old data.\n");
    return 0;
}

int main(int argc, char **argv)
{
    static struct tdisk d;
    static struct t_plan pl;
    static struct t_opt opts[T_MAXOPT];
    const char *why;
    disk_port = sys_port_find("disk");
    if (disk_port < 0) { say("zinstall: no disk server\n"); return 1; }
    if (sys_getuid() != 0) { say("zinstall: only root installs (log in as root)\n"); return 1; }
    if (argc > 1 && !strcmp(argv[1], "--list")) return list_only();
    if (argc > 1 && !strcmp(argv[1], "--undo")) return undo(argc > 2 ? argv[2] : BEFORE);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : 0;
        if (!strcmp(a, "--no-user")) { opt_nouser = 1; continue; }
        if (!v) goto usage;
        if (!strcmp(a, "--hostname")) opt_host = v;
        else if (!strcmp(a, "--root-password-file")) opt_rootpw = v;
        else if (!strcmp(a, "--user")) opt_user = v;
        else if (!strcmp(a, "--user-password-file")) opt_userpw = v;
        else if (!strcmp(a, "--user-name")) opt_username = v;
        else goto usage;
        i++;
    }
    if (0) {
usage:
        say("usage: zinstall [--hostname NAME] [--root-password-file FILE] [--user NAME [--user-name TEXT]\n"
            "       [--user-password-file FILE] | --no-user]     zinstall --list     zinstall --undo [FILE]\n");
        return 2;
    }
    {   /* one at a time: two would plan on the same blocks */
        struct uproc pr;
        int me = sys_getpid();
        for (u32 i = 0; i < 64 && sys_procinfo(i, &pr) == 0; i++)
            if ((int)pr.pid != me && pr.state != 0 && pr.state != 5 && !strncmp(pr.name, "zinstall", 8)) {
                say("zinstall: another zinstall is running (pid %u): one at a time.\n", pr.pid);
                return 1;
            }
    }

    say("\nThe Zenix installer. Nothing is written until you have seen exactly what will\nchange and said yes. Type q at any question to stop.\n");
    int floppy = read_set() == 0;
    struct ustat st;
    if (!floppy && sys_stat("/bin/zpkg", &st) < 0) {
        say("zinstall: this is neither the install floppy (no %s) nor a Zenix with zpkg:\nthere is nothing to install from.\n", SETFILE);
        return 1;
    }
    say("Files come from: %s.\n", floppy ? "the install floppies" : "the package server, with zpkg");

    u32 free0 = mem_now();
    struct diskinfo di;
    diskh = dopen("ide0", 0);
    if (diskh < 0 || dinfo(diskh, &di) < 0) { say("zinstall: no hard disk found\n"); return 1; }
    find_busy();
    if (t_read(&d, &io, di.sectors, busy, nbusy)) { say("zinstall: cannot read the disk\n"); return 1; }
    show_disk(&d);
    int nopt = t_options(&d, &io, opts, T_MAXOPT);
    if (!nopt) {
        say("\nThere is nowhere on this disk to put Zenix:\n");
        if (d.kind == TD_RDB || d.kind == TD_MBR) say("  no free space of at least 28 MB.\n");
        for (int i = 0; i < d.nparts; i++) {
            const char *w = t_part_refused(&d, i);
            if (w) say("  %s: %s.\n", d.part[i].dev, w);
        }
        if (t_whole_refused(&d)) say("  Erasing it: %s.\n", t_whole_refused(&d));
        say("Nothing was changed.\n");
        return 1;
    }
    say("\nWhere should Zenix go?\n");
    for (int i = 0; i < nopt; i++) show_opt(i + 1, &d, &opts[i]);
    if (d.noedit && d.kind == TD_RDB)
        say("\nThis disk's table looks damaged. The partitions may still be recoverable with a\ntool that repairs tables; erasing the disk ends that.\n");
    int dflt = opts[0].kind == TO_FREE || opts[0].kind == TO_NEWCONT;
    struct t_opt *o = 0;
    while (!o) {
        if (ask(dflt ? "Choose a number (Return = 1, the free space): " : "Choose a number (there is no default here): ")) return 1;
        if (!strcmp(line, "q")) { say("Nothing was changed.\n"); return 0; }
        if (!line[0]) { if (dflt) o = &opts[0]; continue; }
        int k = atoi(line);
        if (k >= 1 && k <= nopt) o = &opts[k - 1];
    }

    /* how much */
    u32 maxsect = 0;
    if (o->kind != TO_WHOLE && o->kind != TO_PART) {
        for (;;) {
            say("\nThere are %s here. How many MB may Zenix use?\n", mbtext(o->sectors));
            if (ask("MB (Return = what it decides: boot, root, /home, some left free): ")) return 1;
            if (!strcmp(line, "q")) { say("Nothing was changed.\n"); return 0; }
            if (!line[0]) break;
            u32 mb = (u32)strtoul(line, 0, 10);
            if (mb >= 28 && mb <= o->sectors / 2048) { maxsect = mb * 2048; break; }
            say("Between 28 and %u, please.\n", o->sectors / 2048);
        }
    }
    /* the root's filesystem */
    say("\nThe root's filesystem:\n"
        "  1  ext2: proven on real hardware (the owner's PiStorm A1200). The default.\n"
        "  2  ZenixFS: survives a power cut with no check at the next start, but costs\n"
        "     about 188-252 KB more memory on a 2 MB machine, and it has NOT yet run on\n"
        "     real hardware, only in an emulator.\n");
    int zenixfs = -1;
    while (zenixfs < 0) {
        if (ask("Choose (Return = 1, ext2): ")) return 1;
        if (!strcmp(line, "q")) { say("Nothing was changed.\n"); return 0; }
        if (!line[0] || !strcmp(line, "1")) zenixfs = 0;
        else if (!strcmp(line, "2")) zenixfs = 1;
    }
    if (t_plan_make(&d, &io, o, maxsect, zenixfs, &pl, &why)) { say("zinstall: this cannot be done: %s.\nNothing was changed.\n", why); return 1; }
    /* memory, before anything is written */
    u32 need = (zenixfs ? MINKB_ZFS : MINKB) * 1024;
    if (free0 < need) {
        say("zinstall: only %u KB of memory are free; installing needs about %u KB.\nNothing was changed.\n", free0 / 1024, need / 1024);
        return 1;
    }
    show_plan(&d, &pl);
    if (pl.opt.kind == TO_WHOLE) {
        say("\nTo erase the whole disk type exactly: ERASE ide0\nAnything else stops here.\n");
        if (ask("> ") || strcmp(line, "ERASE ide0")) { say("Nothing was changed.\n"); return 0; }
    } else if (pl.opt.kind == TO_PART) {
        const struct t_part *q = &d.part[pl.opt.part];
        const char *word = q->label[0] ? q->label : q->dev;
        say("\nTo replace this partition type its name exactly: %s\nAnything else stops here.\n", word);
        if (ask("> ") || strcmp(line, word)) { say("Nothing was changed.\n"); return 0; }
    } else {
        say("\nType yes to go on. Anything else stops here.\n");
        if (ask("> ") || strcmp(line, "yes")) { say("Nothing was changed.\n"); return 0; }
    }

    /* the tables once more, right before the first write */
    find_busy();
    int same = t_same(&d, &io, di.sectors, busy, nbusy);
    if (same != 1) { say("\nThe disk changed since it was shown%s: nothing was written. Run zinstall again.\n", same < 0 ? " (or cannot be read)" : ""); return 1; }
    if (nbusy == 0) {
        int h = dopen("ide0", DISK_RAW);        /* nothing mounted: and so it stays while we work on the table */
        if (h < 0) { say("zinstall: the disk is in use now (%d): nothing was written.\n", h); return 1; }
        diskh = h;
    }

    gone = pl.opt.kind == TO_PART || pl.opt.kind == TO_WHOLE;
    say("\nWriting the table.\n");
    if (t_commit(&io, pl.w1, pl.nw1, &why)) {
        say("zinstall: %s.\nStopped. Whether the new table is whole: run zinstall --list.\n", why);
        return 1;
    }
    rescan();
    const char *names[3];
    static char nb[3][16];
    for (int i = 0; i < pl.nnew; i++) {
        const char *n = dev_at(pl.nw[i].start, pl.nw[i].sectors);
        if (!n) { say("zinstall: the disk server does not show the new partition %s: stopping.\n", pl.nw[i].label); fail_note(1); return 1; }
        strncpy(nb[i], n, 15);
        names[i] = nb[i];
        say("  %s is %s\n", pl.nw[i].label, names[i]);
    }
    /* filesystems */
    say("\nMaking the filesystems.\n");
    sys_mkdir("/mnt"); sys_mkdir(ZI); sys_mkdir(ZH);
    /* one inode for 16 KB: the system takes under 500 of the 8,000 on 128 MB, and fsck.ext2 needs about 12 bytes an inode: at mkfs's default of 4 KB a
     * 128 MB root ran it out of memory on 2 MB of chip RAM (measured), here and in any later check */
    int r = zenixfs ? run("/bin/mkfs.zenixfs", (char *)names[R_ROOT], 0, 0) : run("/bin/mkfs.ext2", "-i", "16384", (char *)names[R_ROOT]);
    if (r) { say("zinstall: mkfs failed (%d)\n", r); fail_note(1); return 1; }
    if (pl.nnew > 2) {
        r = run("/bin/mkfs.ext2", "-i", "16384", (char *)names[R_HOME]);
        if (r) { say("zinstall: mkfs of /home failed (%d)\n", r); fail_note(1); return 1; }
    }
    char *margv[] = { "/bin/mount", "-t", zenixfs ? "zenixfs" : "ext2", (char *)names[R_ROOT], ZI, 0 };
    int pid = sys_spawn(margv[0], margv, 0);
    r = pid < 0 ? pid : sys_wait(pid);
    if (r) { say("zinstall: cannot mount the new root (%d)\n", r); fail_note(1); return 1; }
    /* copy */
    say("\nCopying the system.\n");
    r = floppy ? copy_from_floppies(zenixfs) : copy_from_network(zenixfs);
    if (r) { umount_all(); fail_note(1); return 1; }
    /* the places every root has that are in no package */
    sys_mkdir(ZI "/tmp"); sys_chmod(ZI "/tmp", 01777);
    sys_mkdir(ZI "/home"); sys_mkdir(ZI "/mnt"); sys_mkdir(ZI "/bin");
    sys_sync();
    /* read everything back from the disk, the server's cache gone with its unmount */
    say("\nChecking every file on the disk against its checksum.\n");
    sys_umount(ZI);
    pid = sys_spawn(margv[0], margv, 0);
    r = pid < 0 ? pid : sys_wait(pid);
    if (r) { say("zinstall: cannot mount the new root again (%d)\n", r); fail_note(1); return 1; }
    u32 nfiles = 0;
    int bad = verify_root(&nfiles);
    if (bad) { say("zinstall: %d problem(s) on the disk: stopping.\n", bad); umount_all(); fail_note(1); return 1; }
    say("  %u files are right.\n", nfiles);
    /* the machine's own */
    if (!floppy) {                      /* the new machine knows the same servers */
        struct ustat cst;
        if (sys_stat("/etc/zpkg.conf", &cst) == 0 && cst.size < 4096) {
            char *t = xmalloc(cst.size);
            int fd = sys_open("/etc/zpkg.conf", O_RDONLY);
            if (fd >= 0 && sys_read(fd, t, cst.size) == (int)cst.size) put_file("/etc/zpkg.conf", t, cst.size, 0644);
            if (fd >= 0) sys_close(fd);
            free(t);
        }
    }
    static char fst[1024];
    struct ustat fs;
    if (sys_stat(ZI "/etc/fstab", &fs) == 0 && fs.size < sizeof fst - 64) {
        int fd = sys_open(ZI "/etc/fstab", O_RDONLY);
        int n = fd >= 0 ? sys_read(fd, fst, fs.size) : -1;
        if (fd >= 0) sys_close(fd);
        if (n == (int)fs.size) {
            if (pl.nnew > 2) snprintf(fst + n, sizeof fst - (u32)n, "%s    /home   ext2     defaults\n", names[R_HOME]);
            put_file("/etc/fstab", fst, (u32)strlen(fst), 0644);
        }
    }
    /* the table as it was (--undo) */
    mkdir_p(ZI "/var/lib/zinstall");
    r = save_before(&pl, pl.opt.kind == TO_FREE || pl.opt.kind == TO_NEWCONT);
    if (r) { say("zinstall: cannot save the copy of the table (%d)\n", r); umount_all(); fail_note(1); return 1; }
    /* /home, then the machine's name and its accounts */
    if (pl.nnew > 2) {
        char *h[] = { "/bin/mount", "-t", "ext2", (char *)names[R_HOME], ZH, 0 };
        pid = sys_spawn(h[0], h, 0);
        r = pid < 0 ? pid : sys_wait(pid);
        if (r) { say("zinstall: cannot mount the new /home (%d)\n", r); umount_all(); fail_note(1); return 1; }
    }
    if (accounts(pl.nnew > 2 ? ZH : ZI "/home")) { umount_all(); fail_note(1); return 1; }
    sys_sync();
    if (pl.nnew > 2) sys_umount(ZH);
    /* the boot partition */
    say("\nWriting the boot partition (%s).\n", names[R_BOOT]);
    if (write_boot(&pl, names[R_BOOT])) { umount_all(); fail_note(1); return 1; }
    sys_unlink(ZI KFILE);
    sys_sync();
    r = sys_umount(ZI);
    if (r) { say("zinstall: cannot unmount the new root (%d)\n", r); fail_note(1); return 1; }
    /* a read-only check of the filesystems */
    say("\nChecking the filesystems.\n");
    r = zenixfs ? run("/bin/fsck.zenixfs", (char *)names[R_ROOT], 0, 0) : run("/bin/fsck.ext2", "-n", (char *)names[R_ROOT], 0);
    if (r) { say("zinstall: the check of the new root found a problem (%d): stopping.\n", r); fail_note(1); return 1; }
    /* last of all: bootable */
    if (t_commit(&io, pl.w2, pl.nw2, &why)) { say("zinstall: %s\n", why); fail_note(1); return 1; }
    rescan();
    say("\nDone. Zenix is on this disk.\n"
        "Lowest free memory seen while installing: %u KB.\n\n"
        "What to do next:\n", lowest / 1024);
    say("  1. Take the floppy out and restart the Amiga (Ctrl-Amiga-Amiga, or type reboot).\n");
    if (pl.aos[0]) {
        say("  2. A boot menu comes first. AmigaOS (%s) starts by itself after 5 seconds; press\n"
            "     a key and choose Zenix for Zenix.\n", pl.aos);
        if (username[0]) say("  3. Log in as %s on %s; sudo for root's work.\n\n", username, hostname);
        else say("  3. Log in as root on %s; useradd NAME and passwd NAME make your account.\n\n", hostname);
        say("The way back to only %s: in Zenix type zinstall --undo, then restart: the table is\n"
            "as it was. Or at power-on hold both mouse buttons for Kickstart's own menu.\n", pl.aos);
    } else {
        if (username[0]) say("  2. Log in as %s on %s; sudo for root's work.\n", username, hostname);
        else say("  2. Log in as root on %s; useradd NAME and passwd NAME make your account.\n", hostname);
    }
    if (d.kind == TD_MBR) say("%s; on another Amiga start it from the floppy zenix%s.adf.\n", has_cont(&d) ? "Under Emu68 a second 0x76 is not known to start" : "Under Emu68 it should start by itself", zenixfs ? "-zfs" : "");
    say("Later: zpkg -Syu brings what is newer, zpkg -S NAME more programs.\n");
    return 0;
}
