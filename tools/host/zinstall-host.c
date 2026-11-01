/* SPDX-License-Identifier: GPL-2.0-only */
/* The installer's table code and boot partition writer on image files, on the PC (tools/zinstall-host-test.py):
 *   zinstall-host list IMG                         what the disk holds, what could be done
 *   zinstall-host install IMG WHICH [MB] [zfs]     do option WHICH (a number as zinstall shows), the filesystems faked:
 *                                                  stage 1, marks in the root and /home, the boot partition, stage 2
 *   zinstall-host fuzz SEED ROUNDS IMG...          damaged tables: t_read, t_options and t_plan_make must end
 * Environment: ZI_CUT=N a power cut after N sector writes (exit 99); ZI_TABLE_ONLY=1 stage 1 only; ZI_CHANGE=LBA:BYTE:VALUE
 * a change on the disk after the plan was made, before the re-read; ZI_CUT_TABLE=1 ZI_CUT counts table writes only. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../../user/zinstall/table.h"
#include "../../user/zinstall/ffsw.h"

static int fd;
static long cut = -1, written;
static int rd(void *c, t_u32 lba, t_u32 n, void *buf)
{
    (void)c;
    if (pread(fd, buf, n * 512, (off_t)lba * 512) != (ssize_t)(n * 512)) return -5;
    return 0;
}
static int wr(void *c, t_u32 lba, t_u32 n, const void *buf)
{
    (void)c;
    for (t_u32 i = 0; i < n; i++) {
        if (cut >= 0 && written >= cut) exit(99);
        if (pwrite(fd, (const char *)buf + i * 512, 512, (off_t)(lba + i) * 512) != 512) return -5;
        written++;
    }
    return 0;
}
static int fl(void *c) { (void)c; return 0; }
static const struct tio io = { 0, rd, wr, fl };

static const char *hexdt(unsigned t)
{
    static char b[16];
    snprintf(b, sizeof b, "%08x", t);
    return b;
}
static void show(const struct tdisk *d)
{
    static const char *const kinds[] = { "blank", "rdb", "mbr", "bare", "unknown", "gpt", "hybrid" };
    printf("kind %s\nnoedit %s\nwhat %s\n", kinds[d->kind], d->noedit ? d->noedit : "-", d->what);
    printf("hasfat %d\n", d->hasfat);
    for (int i = 0; i < d->nparts; i++) {
        const struct t_part *p = &d->part[i];
        const char *why = t_part_refused(d, i);
        printf("part %s label=%s vol=%s start=%u sectors=%u type=%s mbr=%02x boot=%d nomount=%d pri=%d region=%d refused=%s\n", p->dev, p->label[0] ? p->label : "-",
               p->vol[0] ? p->vol : "-", p->start, p->sectors, t_typename(p), p->mbrtype, p->bootable, p->nomount, p->pri, p->region, why ? why : "-");
        (void)hexdt;
    }
    for (int r = 0; r < d->nregions; r++)
        printf("region %d base=%u limit=%u cyl=%u noedit=%s\n", r, d->region[r].base, d->region[r].limit, d->region[r].cyl, d->region[r].noedit ? d->region[r].noedit : "-");
}
static void show_opts(const struct tdisk *d, struct t_opt *o, int n)
{
    static const char *const k[] = { "newcont", "free", "part", "whole" };
    for (int i = 0; i < n; i++)
        printf("option %d %s region=%d part=%d start=%u sectors=%u\n", i + 1, k[o[i].kind], o[i].region, o[i].part, o[i].start, o[i].sectors);
    const char *w = t_whole_refused(d);
    printf("whole %s\n", w ? w : "allowed");
}

/* the boot partition's writer, through the whole disk's I/O, confined here as diskd confines it */
static t_u32 pstart, psect;
static int pwr(void *c, unsigned lba, unsigned n, const void *buf)
{
    if (lba + n > psect) { fprintf(stderr, "write outside the partition\n"); exit(3); }
    return wr(c, pstart + lba, n, buf);
}
static int prd(void *c, unsigned lba, unsigned n, void *buf)
{
    if (lba + n > psect) return -5;
    return rd(c, pstart + lba, n, buf);
}
static const struct ffsw_io fio = { 0, pwr, prd };
struct msrc { const unsigned char *p; };
static int src_mem(void *c, unsigned char *b, unsigned n) { struct msrc *m = c; memcpy(b, m->p, n); m->p += n; return 0; }

/* a filesystem faked: the first 64 KB of the partition, a pattern */
static int fake_fs(const struct t_newp *n, unsigned char mark)
{
    static unsigned char b[512];
    for (int i = 0; i < 128; i++) {
        memset(b, mark, 512);
        if ((t_u32)i >= n->sectors) break;
        if (wr(0, n->start + (t_u32)i, 1, b)) return -1;
    }
    return 0;
}
static int write_boot(const struct t_plan *pl)
{
    static unsigned char blob[300000];
    unsigned char h[32], h2[32];
    for (unsigned i = 0; i < sizeof blob; i++) blob[i] = (unsigned char)(i * 2654435761u >> 24);
    pstart = pl->nw[R_BOOT].start; psect = pl->nw[R_BOOT].sectors;
    if (ffsw_begin(&fio, psect, "ZenixBoot", 17000, 60)) { printf("ffsw: %s\n", ffsw_error()); return -1; }
    if (ffsw_mkdir("S") || ffsw_mkdir("Zenix")) goto bad;
    {
        struct msrc m = { (const unsigned char *)"SYS:bootmenu\n" };
        if (ffsw_file("S", "Startup-Sequence", 13, src_mem, &m, h)) goto bad;
    }
    {
        struct msrc m = { blob };
        if (ffsw_file("", "bootmenu", 14440, src_mem, &m, h)) goto bad;
    }
    {
        struct msrc m = { blob };
        if (ffsw_file("Zenix", "zenix", 165484, src_mem, &m, h)) goto bad;
    }
    {
        struct msrc m = { blob };
        if (ffsw_file("", "zero", 0, src_mem, &m, h2)) goto bad;
    }
    if (ffsw_end()) goto bad;
    if (ffsw_read_sha("Zenix", "zenix", 165484, h2) || memcmp(h, h2, 32)) { printf("ffsw: read back failed\n"); return -1; }
    return 0;
bad:
    printf("ffsw: %s\n", ffsw_error());
    return -1;
}

static struct tdisk d;
static struct t_plan pl;
static struct t_opt opts[T_MAXOPT];

static t_u32 size_of(const char *path)
{
    struct stat st;
    if (stat(path, &st)) { perror(path); exit(2); }
    return (t_u32)(st.st_size / 512);
}

static unsigned long long frnd(void)
{
    static unsigned long long s;
    if (!s) s = 88172645463325252ull ^ (unsigned long long)strtoul(getenv("ZI_SEED") ? getenv("ZI_SEED") : "1", 0, 10);
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}
/* the sectors a table lives in, damaged in a memory copy: the program reads through mrd */
static unsigned char *img;
static unsigned imgsect;
static int mrd(void *c, t_u32 lba, t_u32 n, void *buf)
{
    (void)c;
    if ((unsigned long long)lba + n > imgsect) return -5;
    memcpy(buf, img + (size_t)lba * 512, (size_t)n * 512);
    return 0;
}
static int mwr(void *c, t_u32 lba, t_u32 n, const void *buf)
{
    (void)c; (void)lba; (void)n; (void)buf;
    fprintf(stderr, "the fuzzer's plan wrote\n");
    exit(4);
}
static int fuzz(unsigned long rounds, int nimg, char **names)
{
    static const struct tio mio = { 0, mrd, mwr, fl };
    unsigned long okread = 0, plans = 0, refused = 0, opts_tot = 0;
    /* only the first 16 MB of each image are kept: where the tables are */
    unsigned maxs = 32768;
    unsigned char *orig[16];
    unsigned sects[16], real[16];
    if (nimg > 16) nimg = 16;
    img = malloc((size_t)maxs * 512);
    for (int i = 0; i < nimg; i++) {
        int f = open(names[i], O_RDONLY);
        if (f < 0) { perror(names[i]); return 2; }
        real[i] = size_of(names[i]);
        sects[i] = real[i] < maxs ? real[i] : maxs;
        orig[i] = malloc((size_t)sects[i] * 512);
        if (read(f, orig[i], (size_t)sects[i] * 512) != (ssize_t)((size_t)sects[i] * 512)) { perror("read"); return 2; }
        close(f);
    }
    for (unsigned long r = 0; r < rounds; r++) {
        int k = (int)(frnd() % (unsigned long long)nimg);
        imgsect = sects[k];
        memcpy(img, orig[k], (size_t)imgsect * 512);
        /* damage: a few sectors of the first ones that hold tables */
        int hits = 1 + (int)(frnd() % 8);
        for (int h = 0; h < hits; h++) {
            unsigned sec = (unsigned)(frnd() % 4 == 0 ? frnd() % imgsect : frnd() % (imgsect < 32 ? imgsect : 32));
            unsigned kind = (unsigned)(frnd() % 6);
            unsigned at = (unsigned)(frnd() % 512);
            unsigned char *p = img + (size_t)sec * 512;
            switch (kind) {
            case 0: p[at] ^= (unsigned char)(1u << (frnd() % 8)); break;
            case 1: p[at] = (unsigned char)frnd(); break;
            case 2: { unsigned a = at & ~3u; unsigned v = (unsigned)frnd(); p[a] = (unsigned char)(v >> 24); p[a + 1] = (unsigned char)(v >> 16); p[a + 2] = (unsigned char)(v >> 8); p[a + 3] = (unsigned char)v; break; }
            case 3: { unsigned a = at & ~3u; static const unsigned vv[] = { 0, 1, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 15, 16, 64, 128 }; unsigned v = vv[frnd() % 9]; p[a] = (unsigned char)(v >> 24); p[a + 1] = (unsigned char)(v >> 16); p[a + 2] = (unsigned char)(v >> 8); p[a + 3] = (unsigned char)v; break; }
            case 4: memset(p + (at & ~15u), (int)(frnd() & 0xFF), 16 < 512 - (at & ~15u) ? 16 : 512 - (at & ~15u)); break;
            default: { /* a chain or an entry at a small number */ unsigned a = at & ~3u; p[a] = 0; p[a + 1] = 0; p[a + 2] = 0; p[a + 3] = (unsigned char)(frnd() % 40); break; }
            }
        }
        /* the disk's size: the image's or a smaller one sometimes */
        t_u32 s = real[k];
        if (frnd() % 8 == 0) s = (t_u32)(frnd() % (real[k] + 1));
        if (s > imgsect && frnd() % 2) s = imgsect;
        if (t_read(&d, &mio, s, 0, 0)) { refused++; continue; }
        okread++;
        for (int i = 0; i < d.nparts; i++) (void)t_part_refused(&d, i);
        (void)t_whole_refused(&d);
        int n = t_options(&d, &mio, opts, T_MAXOPT);
        opts_tot += (unsigned long)n;
        for (int i = 0; i < n; i++) {
            const char *why;
            if (t_plan_make(&d, &mio, &opts[i], 0, (int)(frnd() & 1), &pl, &why)) { refused++; continue; }
            plans++;
            /* no plan may write outside the disk, or twice the same place with a different view */
            for (int w = 0; w < pl.nw1; w++) if (pl.w1[w].lba >= s) { printf("zinstall-fuzz: plan writes past the disk\n"); return 3; }
            /* nothing the table claims may be overlapped by what is new, or by a table sector, but what was chosen, and the container it lies in */
            if (opts[i].kind == TO_WHOLE) continue;
            for (int q = 0; q < d.nparts; q++) {
                const struct t_part *x = &d.part[q];
                if (q == opts[i].part || !x->sectors) continue;
                if (x->how == TP_MBR && x->mbrtype == 0x76 && opts[i].region >= 0 && x->holds == opts[i].region) continue;
                if (opts[i].part >= 0 && d.part[opts[i].part].how == TP_RDB && x->how == TP_MBR && x->holds == d.part[opts[i].part].region) continue;
                for (int w = 0; w < pl.nnew; w++) {
                    unsigned long long a = pl.nw[w].start, an = pl.nw[w].sectors;
                    if (a < (unsigned long long)x->start + x->sectors && x->start < a + an) { printf("zinstall-fuzz: a new partition over %s\n", x->dev); return 3; }
                }
                for (int w = 0; w < pl.nw1; w++)
                    if (pl.w1[w].lba >= x->start && pl.w1[w].lba - x->start < x->sectors) { printf("zinstall-fuzz: a table write into %s\n", x->dev); return 3; }
            }
        }
    }
    printf("zinstall-fuzz: %lu rounds, %lu tables read, %lu options, %lu plans made, %lu refused: no crash, no hang\n", rounds, okread, opts_tot, plans, refused);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: see the source\n"); return 2; }
    if (!strcmp(argv[1], "fuzz")) return fuzz(strtoul(argv[3], 0, 10), argc - 4, argv + 4);
    int ro = !strcmp(argv[1], "list");
    fd = open(argv[2], ro ? O_RDONLY : O_RDWR);
    if (fd < 0) { perror(argv[2]); return 2; }
    t_u32 sect = size_of(argv[2]);
    struct t_busy bz;
    int nbz = 0;
    if (getenv("ZI_BUSY") && sscanf(getenv("ZI_BUSY"), "%u:%u", &bz.start, &bz.sectors) == 2) nbz = 1;
    if (t_read(&d, &io, sect, &bz, nbz)) { printf("cannot read\n"); return 1; }
    int n = t_options(&d, &io, opts, T_MAXOPT);
    if (!strcmp(argv[1], "list")) { show(&d); show_opts(&d, opts, n); return 0; }
    if (!strcmp(argv[1], "install") && argc >= 4) {
        const char *why;
        int k = atoi(argv[3]);
        if (k < 1 || k > n) { printf("no option %d\n", k); return 1; }
        t_u32 maxs = argc > 4 ? (t_u32)strtoul(argv[4], 0, 10) * 2048 : 0;
        int zfs = argc > 5 && !strcmp(argv[5], "zfs");
        if (t_plan_make(&d, &io, &opts[k - 1], maxs, zfs, &pl, &why)) { printf("refused: %s\n", why); return 1; }
        for (int i = 0; i < pl.nnew; i++)
            printf("new %s start=%u sectors=%u blk=%u lo=%u hi=%u\n", pl.nw[i].label, pl.nw[i].start, pl.nw[i].sectors, pl.nw[i].blk, pl.nw[i].lo, pl.nw[i].hi);
        printf("atomic %d pri %d aos %s nw1 %d commit1 %d nw2 %d\n", pl.atomic, pl.pri, pl.aos[0] ? pl.aos : "-", pl.nw1, pl.commit1, pl.nw2);
        const char *ch = getenv("ZI_CHANGE");
        if (ch) {
            unsigned lba, off, val;
            if (sscanf(ch, "%u:%u:%u", &lba, &off, &val) == 3) { unsigned char c = (unsigned char)val; if (pwrite(fd, &c, 1, (off_t)lba * 512 + off) != 1) return 2; }
        }
        int same = t_same(&d, &io, sect, &bz, nbz);
        if (same != 1) { printf("changed: nothing written\n"); return 1; }
        if (getenv("ZI_CUT")) cut = atol(getenv("ZI_CUT"));
        if (t_commit(&io, pl.w1, pl.nw1, &why)) { printf("commit: %s\n", why); return 1; }
        printf("stage1 %ld\n", written);
        if (getenv("ZI_TABLE_ONLY")) return 0;
        if (fake_fs(&pl.nw[R_ROOT], 0xA5)) return 1;
        if (pl.nnew > 2 && fake_fs(&pl.nw[R_HOME], 0x5A)) return 1;
        if (write_boot(&pl)) return 1;
        printf("files %ld\n", written);
        if (t_commit(&io, pl.w2, pl.nw2, &why)) { printf("commit2: %s\n", why); return 1; }
        printf("done %ld\n", written);
        return 0;
    }
    return 2;
}
