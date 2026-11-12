/* SPDX-License-Identifier: GPL-2.0-only */
/* zine PROGRAM [ARGS...]: run an AmigaOS program under ZINE, Zenix's own small AmigaOS (DESIGN D29,
 * docs/d29-zine.md section 7.1; docs/man/zine.txt). Checks the file, puts it into the in-memory volume, loads and
 * relocates /lib/zine/zine.blob, hands the machine over through SYS_zine and says how the program ended. */
#include "pw.h"
typedef unsigned short u16;
typedef unsigned char u8;
typedef int s32;
#include "../kernel/zine.h"
#include "zine/hunk.h"
#include "zine/zvol.h"
#include "zine/lvo.h"

#define BLOB "/lib/zine/zine.blob"
static const char *name, *blobpath = BLOB;

static int zine_sys(u32 op, void *a, u32 b) { return sys3(93, op, (u32)a, b); }

/* zine is set-user-id root (owner answer 1, 2026-10-30: root and the group zine). The membership is the caller's
 * groups as the kernel holds them (its gid and supplementary groups, kept across set-user-id), as sudo checks wheel.
 * What it reads for the program, it reads only where the user who ran it may (root may all), with the same groups, so
 * it shows nobody a file through a program's output */
static u32 ruid;
static int may_read(const struct ustat *st)
{
    if (ruid == 0)
        return 1;
    if (st->uid == ruid)
        return (st->mode & 0400) != 0;
    if (in_group(st->gid))
        return (st->mode & 0040) != 0;
    return (st->mode & 0004) != 0;
}

static int in_zine_group(void) { return in_group_named("zine"); }

static void die(const char *fmt, const char *arg)
{
    dprintf(2, fmt, arg);
    dprintf(2, "\n");
    _zexit(1);
}

/* the whole file into memory (malloc'd), its size and status */
static u8 *slurp(const char *path, u32 *len, struct ustat *st)
{
    int fd = sys_open(path, 0);
    if (fd < 0)
        return 0;
    u8 *b = 0;
    if (sys_fstat(fd, st) == 0 && !may_read(st)) {
        sys_close(fd);
        return 0;
    }
    if (sys_fstat(fd, st) == 0 && (b = malloc(st->size ? st->size : 1))) {
        u32 got = 0;
        int n;
        while (got < st->size && (n = sys_read(fd, b + got, st->size - got)) > 0)
            got += (u32)n;
        if (got != st->size) {
            free(b);
            b = 0;
        }
        *len = got;
    }
    sys_close(fd);
    return b;
}

static u32 crc32(const u8 *p, u32 n)
{
    u32 c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1));
    }
    return ~c;
}

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* /lib/zine/zine.blob: a ZEXE image with the "ZINB" header, checked by its CRC, loaded and relocated */
static u32 load_blob(u32 *size)
{
    struct ustat st;
    u32 len;
    u8 *f = slurp(blobpath, &len, &st);
    if (!f)
        die("zine: the blob %s (ZINE is not installed)", zwhy("cannot be read"));
    if (len < 48 || memcmp(f + 4, "ZEXE", 4) || be32(f + ZINB_OFF) != ZINB_MAGIC)
        die("zine: " BLOB " is damaged (%s)", "not ZINE's blob");
    u32 want = be32(f + ZINB_OFF + 8);
    f[ZINB_OFF + 8] = f[ZINB_OFF + 9] = f[ZINB_OFF + 10] = f[ZINB_OFF + 11] = 0;
    if (crc32(f, len) != want)
        die("zine: " BLOB " is damaged (%s)", "its checksum is wrong");
    if (be32(f + ZINB_OFF + 4) != ZINE_LAYOUT)
        die("zine: ZINE's files and %s are from different versions: finish the update first", "/bin/zine");
    u32 image = be32(f + 8), mem = be32(f + 12), nrel = be32(f + 16), roff = be32(f + 20);
    if (image > len || mem < image || roff > len || nrel > (len - roff) / 4)
        die("zine: " BLOB " is damaged (%s)", "its header");
    u8 *b = sys_memalloc(mem);
    if (!b)
        die("zine: not enough memory for %s", "ZINE");
    memcpy(b, f, image);
    memset(b + image, 0, mem - image);
    for (u32 i = 0; i < nrel; i++) {
        u32 o = be32(f + roff + 4 * i);
        if (o > image - 4 || (o & 1))
            die("zine: " BLOB " is damaged (%s)", "a relocation");
        *(u32 *)(b + o) += (u32)b;
    }
    free(f);
    *size = mem;
    return (u32)b;
}

/* the CPU support packages (d29 16.14): /lib/zine/sp060 and sp040, ZEXE images (user/zine/zsp060.S, zsp040.S) with
 * the "ZSPK" header at offset 32, loaded and relocated as the blob is; 0 when the file is not there or not right
 * (said: the programs then crash on the instructions the CPU lacks, as before) */
#define ZSPK_MAGIC 0x5A53504Bu
static u32 load_sp(const char *path, u32 cpu, zu32 *size)
{
    struct ustat st;
    u32 len;
    *size = 0;
    u8 *f = slurp(path, &len, &st);
    const char *why = 0;
    if (!f)
        why = "cannot be read";
    else if (len < 48 || memcmp(f + 4, "ZEXE", 4) || be32(f + 32) != ZSPK_MAGIC || be32(f + 36) != cpu)
        why = "is not ZINE's support package";
    u32 image = f ? be32(f + 8) : 0, mem = f ? be32(f + 12) : 0, nrel = f ? be32(f + 16) : 0,
        roff = f ? be32(f + 20) : 0;
    if (!why && (image > len || mem < image || image < 48 || roff > len || nrel > (len - roff) / 4))
        why = "is damaged (its header)";
    u8 *b = why ? 0 : sys_memalloc(mem);
    if (!why && !b)
        why = "does not fit in memory";
    if (!why) {
        memcpy(b, f, image);
        memset(b + image, 0, mem - image);
        for (u32 i = 0; i < nrel && !why; i++) {
            u32 o = be32(f + roff + 4 * i);
            if (o > image - 4 || (o & 1))
                why = "is damaged (a relocation)";
            else
                *(u32 *)(b + o) += (u32)b;
        }
    }
    free(f);
    if (why) {
        if (b)
            sys_memfree(b);
        dprintf(2, "zine: %s %s: the %u's missing instructions are not emulated\n", path, why, 68000 + cpu);
        return 0;
    }
    *size = mem;
    return (u32)b;
}

/* ZINE's own libraries by their number in the block's lib (kernel/zine.h): the four, then those of d29 16.12 */
#define NLIBS 14
static const char *const libs[NLIBS] = { "exec.library", "graphics.library", "dos.library", "intuition.library",
    "utility.library", "mathffp.library", "mathtrans.library", "mathieeedoubbas.library",
    "mathieeedoubtrans.library", "mathieeesingbas.library", "mathieeesingtrans.library", "diskfont.library",
    "keymap.library", "gadtools.library" };
const char *zl_name40(const char *lib, unsigned offset);    /* user/zine/lvo40.c: the NDK 3.2's names */

static const char *vecname(u32 v)
{
    static const char *const n[] = { 0, 0, "bus error", "address error", "illegal instruction", "zero divide",
                                     "CHK instruction", "TRAPV instruction", "privilege violation", "trace",
                                     "line A instruction", "line F instruction" };
    static const char *const f[] = { "FPU branch or set on unordered", "FPU inexact result", "FPU divide by zero",
                                     "FPU underflow", "FPU operand error", "FPU overflow", "FPU signalling NaN",
                                     "FPU unimplemented data type", 0, 0, 0, 0,
                                     "unimplemented effective address (68060)",
                                     "unimplemented integer instruction (68060: 64-bit MULU.L/DIVU.L, MOVEP, CAS2, "
                                     "CHK2, CMP2)" };
    return v < 12 ? n[v] : v >= 48 && v <= 61 ? f[v - 48] : 0;
}

/* a line-F crash: the FPU instructions a 68040 or 68060 does not have in hardware (AmigaOS's 68040.library and
 * 68060.library emulate them; ZINE does not), by the general form's opmode (coprocessor 1, ext 0x0000/0x4000 forms) */
static const char *fpu_missing(u32 w)
{
    static const struct { u8 op; char n[8]; } t[] = {
        { 0x01, "FINT" }, { 0x02, "FSINH" }, { 0x03, "FINTRZ" }, { 0x06, "FLOGNP1" }, { 0x08, "FETOXM1" },
        { 0x09, "FTANH" }, { 0x0A, "FATAN" }, { 0x0C, "FASIN" }, { 0x0D, "FATANH" }, { 0x0E, "FSIN" },
        { 0x0F, "FTAN" }, { 0x10, "FETOX" }, { 0x11, "FTWOTOX" }, { 0x12, "FTENTOX" }, { 0x14, "FLOGN" },
        { 0x15, "FLOG10" }, { 0x16, "FLOG2" }, { 0x19, "FCOSH" }, { 0x1C, "FACOS" }, { 0x1D, "FCOS" },
        { 0x1E, "FGETEXP" }, { 0x1F, "FGETMAN" }, { 0x21, "FMOD" }, { 0x25, "FREM" }, { 0x26, "FSCALE" },
    };
    if ((w >> 16 & 0xFFC0) != 0xF200 || (w & 0xA000))
        return 0;
    u32 op = w & 0x7F;
    if (op >= 0x30 && op <= 0x37)
        return "FSINCOS";
    for (u32 i = 0; i < sizeof t / sizeof t[0]; i++)
        if (t[i].op == op)
            return t[i].n;
    return 0;
}

/* The volume's walk (5.6, 16.10): the program's directory and its subdirectories, to ZV_DEPTH levels. Pass 1 (W.vol
 * 0) counts the entries and the files' bytes, stopping once past 5.6's limits; pass 2 adds them to W.vol. Recursive,
 * a small frame a level (no path on the stack: W.path is the one being read). */
#define ZV_DEPTH 8
static struct {
    char path[400];                     /* the Zenix path being read */
    u32 top;                            /* bytes of the program's directory with its '/' (names shown from there) */
    u32 n, bytes;                       /* pass 1: entries (files and directories) and bytes found (saturated) */
    int over;                           /* pass 1: more than ZV_MAXFILES entries, stopped */
    u32 *sz, nsz;                       /* pass 1: every file's size, ZA_ARCH set for an archive or a disk image */
    u8 *vol;                            /* pass 2: the volume being filled */
    int noarch;                         /* pass 2: archives and disk images left out */
    u32 thr;                            /* pass 2: files larger than this left out (16.12) */
    u32 left, leftbytes;                /* pass 2: files left out by the two rules above */
    int fonts;                          /* the walk of Zenix's Amiga fonts (16.13): at the top only NAME.font files and
                                         * folders; merged into the volume's FONTS, the program's own files first */
} W;
#define FONTDIR "/usr/share/fonts/amiga"
#define ZA_ARCH 0x80000000u

/* an archive or a disk image by its name's end: what a production's folder carries as it was downloaded, never a
 * file the program reads (16.12) */
static int is_archive(const char *n)
{
    static const char *const ext[] = { ".lha", ".lzh", ".lzx", ".zip", ".dms", ".adf", ".adz", ".hdf", ".7z", ".rar",
                                       ".tar", ".gz", ".tgz", ".bz2", ".xz", ".iso" };
    u32 l = strlen(n);
    for (u32 i = 0; i < sizeof ext / sizeof ext[0]; i++) {
        u32 k = strlen(ext[i]);
        if (l > k) {
            u32 j = 0;
            while (j < k && (n[l - k + j] | 0x20) == ext[i][j])
                j++;
            if (j == k)
                return 1;
        }
    }
    return 0;
}

/* a volume name (Latin-1) as UTF-8 into o (room for 2 * 107 + 1) */
static void utf8_of(char *o, const char *s)
{
    for (const u8 *p = (const u8 *)s; *p; p++)
        if (*p < 0x80)
            *o++ = (char)*p;
        else
            *o++ = (char)(0xC0 | *p >> 6), *o++ = (char)(0x80 | (*p & 0x3F));
    *o = 0;
}

static void walk(u32 plen, u32 parent, int depth)
{
    struct ustat st;
    struct udirent de;
    int dfd;
    if (sys_stat(W.path[0] ? W.path : "/", &st) || !may_read(&st) || (dfd = sys_open(W.path[0] ? W.path : "/", 0)) < 0)
        return;
    while (!W.over && sys_readdir(dfd, &de) > 0) {
        if (de.name[0] == '.' || (!depth && !W.fonts && !strcmp(de.name, name)))
            continue;
        u32 l = strlen(de.name);
        if (plen + 1 + l >= sizeof W.path) {
            if (W.vol)
                dprintf(2, "zine: %s/%s left out: its path is longer than %u bytes\n", W.path + W.top, de.name,
                        (u32)sizeof W.path - 1);
            continue;
        }
        W.path[plen] = '/';
        memcpy(W.path + plen + 1, de.name, l + 1);
        if (sys_stat(W.path, &st) || !may_read(&st))
            continue;
        int dir = (st.mode & S_IFMT) == S_IFDIR;
        if (!dir && (st.mode & S_IFMT) != S_IFREG)
            continue;
        if (W.fonts && !depth && !dir && (l < 6 || strcmp(de.name + l - 5, ".font")))
            continue;                   /* the fonts' folder: NAME.font and NAME/ only (not its README) */
        zu32 have;
        if (W.fonts && W.vol && !zv_find(W.vol, parent, de.name, &have)) {
            if (dir && zv_ent(W.vol, have)->dir)
                walk(plen + 1 + l, have, depth + 1);    /* the program's own folder of that font: merged */
            continue;                   /* a file the program brings itself comes first */
        }
        if (dir && depth + 1 >= ZV_DEPTH) {
            if (W.vol)
                dprintf(2, "zine: %s left out: deeper than %u directories\n", W.path + W.top, ZV_DEPTH);
            continue;
        }
        if (!W.vol) {
            W.n++;
            if (!dir) {
                W.bytes = st.size > 0xFFFFFFFFu - W.bytes ? 0xFFFFFFFFu : W.bytes + st.size;
                if (W.sz && W.nsz < ZV_MAXFILES)
                    W.sz[W.nsz++] = (st.size < ZA_ARCH ? st.size : ZA_ARCH - 1) | (is_archive(de.name) ? ZA_ARCH : 0);
            }
            if (W.n > ZV_MAXFILES)
                W.over = 1;
            if (dir && !W.over)
                walk(plen + 1 + l, 0, depth + 1);
            continue;
        }
        if (!dir && ((W.noarch && is_archive(de.name)) || st.size > W.thr)) {
            if (W.left < 5)
                dprintf(2, "zine: %s left out (%u KB)%s\n", W.path + W.top, (st.size + 1023) >> 10,
                        W.noarch && is_archive(de.name) ? ": an archive or a disk image" : "");
            W.left++;
            W.leftbytes += st.size;
            continue;
        }
        u8 *data;
        int e = zv_add(W.vol, parent, de.name, dir, st.mode, st.mtime, dir ? 0 : st.size, &data);
        if (e < 0) {
            dprintf(2, "zine: %s left out: %s\n", W.path + W.top, zv_reason(e));
            continue;
        }
        u32 t = zv_twin(W.vol, (u32)e);
        if (t) {                        /* AmigaDOS has no two names that differ only in case: said once, here */
            static char tw[2 * ZV_NAMEMAX + 2];
            utf8_of(tw, zv_ent(W.vol, t)->name);
            W.path[plen] = 0;
            const char *in = plen >= W.top ? W.path + W.top : "";
            const char *sl = *in ? "/" : "";
            dprintf(1, "ZINE: %s%s%s and %s%s%s differ only in case, which AmigaDOS does not tell apart: a name the "
                    "program uses finds the one spelt exactly so, else %s%s%s\n", in, sl, tw, in, sl, de.name, in, sl, tw);
            W.path[plen] = '/';
        }
        if (dir) {
            walk(plen + 1 + l, (u32)e, depth + 1);
            continue;
        }
        int ffd = sys_open(W.path, 0);
        u32 got = 0;
        int n;
        while (ffd >= 0 && got < st.size && (n = sys_read(ffd, data + got, st.size - got)) > 0)
            got += (u32)n;
        if (ffd >= 0)
            sys_close(ffd);
    }
    sys_close(dfd);
    W.path[plen] = 0;
}

/* the program's file, read where the check and the loader ask (zh_rd, 16.12) */
#define ZHEAD (16u << 10)               /* the header in memory: any header ZH_MAXHUNKS allows */
static int pfd = -1;
static u8 rdwin[4096];
static int rd_file(void *u, zu32 off, zu8 *buf, zu32 n)
{
    (void)u;
    if (sys_lseek(pfd, (int)off, 0) != (int)off)
        return -1;
    while (n) {
        int k = sys_read(pfd, buf, n > 0x100000u ? 0x100000u : n);
        if (k <= 0)
            return -1;
        buf += k, n -= (u32)k;
    }
    return 0;
}

/* path B's memory (16.12): at most ZV_NKEEP regions of zine's own (SYS_memalloc), planned before the load, the
 * largest hunks first: chip hunks in chip RAM, fast and any hunks in fast RAM; each region as large as what is left to
 * place of its kind when that can be had, else halved down to the largest hunk left, and filled largest first. The
 * loader then takes each hunk's planned place in turn. The volume names the regions to the blob, for which they are
 * the program's memory. */
static struct {
    struct { u32 base, size; } r[ZV_NKEEP];
    u32 n, *at, *sz, next, nh, chip_end;
} P;
static void *region(u32 want, int chip)
{
    void *m = chip ? sys_memalloc_chip(want) : sys_memalloc(want);
    if (m && !chip && (u32)m < P.chip_end) {    /* fast asked, chip given (the allocator's fall-back) */
        sys_memfree(m);
        m = 0;
    }
    return m;
}
static int pre_plan(const u8 *head, const struct zh_info *inf)
{
    u32 n = inf->nhunks;
    P.nh = n;
    P.at = malloc(n * 4 + 4), P.sz = malloc(n * 4 + 4);
    u8 *kind = malloc(n + 1);
    if (!P.at || !P.sz || !kind)
        return -ENOMEM;
    for (u32 i = 0; i < n; i++) {
        zu32 memf;
        P.sz[i] = (zh_hunk_size(head, inf, i, &memf) + 8 + 15) & ~15u;
        kind[i] = (memf & ZMEMF_CHIP) ? 1 : 0;
        P.at[i] = 0;
    }
    for (int k = 1; k >= 0; k--)
        for (;;) {
            u32 big = n, rem = 0;       /* the largest hunk of this kind not placed, and all of them */
            for (u32 i = 0; i < n; i++)
                if (kind[i] == k && !P.at[i]) {
                    rem += P.sz[i];
                    if (big == n || P.sz[i] > P.sz[big])
                        big = i;
                }
            if (big == n)
                break;
            if (P.n == ZV_NKEEP)
                return -ENOMEM;
            u32 want = rem;
            void *m;
            while (!(m = region(want, k)) && want > P.sz[big])
                want = want / 2 > P.sz[big] ? (want / 2 + 15) & ~15u : P.sz[big];
            if (!m)
                return -ENOMEM;
            P.r[P.n].base = (u32)m, P.r[P.n].size = want;
            P.n++;
            u32 used = 0;
            for (;;) {                  /* the largest that still fit, in turn */
                u32 b = n;
                for (u32 i = 0; i < n; i++)
                    if (kind[i] == k && !P.at[i] && P.sz[i] <= want - used && (b == n || P.sz[i] > P.sz[b]))
                        b = i;
                if (b == n)
                    break;
                P.at[b] = (u32)m + used;
                used += P.sz[b];
            }
        }
    free(kind);
    return 0;
}
static int pre_alloc(void *u, zu32 size, zu32 memf, struct zh_seg *out)
{
    (void)u, (void)memf;
    if (P.next >= P.nh || ((size + 15) & ~15u) != P.sz[P.next])
        return -ENOEXEC;                /* not the walk the plan was made from */
    out->p = (zu8 *)P.at[P.next];
    out->addr = P.at[P.next++];
    return 0;
}
static void pre_free(void *u, const struct zh_seg *s) { (void)u, (void)s; }   /* the regions go with zine */

int main(int argc, char **argv)
{
    if (argc > 3 && !strcmp(argv[1], "--blob")) {     /* the tests' other blobs (/tests/zine/null.blob): root only */
        if (sys_getuid() != 0)
            die("zine: --blob is for root%s", "");
        blobpath = argv[2];
        argv += 2;
        argc -= 2;
    }
    if (argc < 2) {
        dprintf(2, "usage: zine [--blob FILE] PROGRAM [ARGS...]\n");
        return 2;
    }
    ruid = (u32)sys_getuid();
    if (ruid != 0 && !in_zine_group())
        die("zine: not allowed: root and the members of the group zine may (root can add: gpasswd -a %s zine)",
            getenv("USER") ? getenv("USER") : "NAME");
    const char *path = argv[1];
    name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    /* the file is read as it is checked (16.12): only its first 16 KB (the header) are in zine's memory, the rest
     * through a window; the whole file goes into memory only when it is kept in the volume (path A below) */
    struct ustat st;
    pfd = sys_open(path, 0);
    if (pfd < 0 || sys_fstat(pfd, &st) || !may_read(&st) || (st.mode & S_IFMT) != S_IFREG)
        die("zine: %s", zwhy("cannot be read"));
    u32 len = st.size, hlen = len < ZHEAD ? len : ZHEAD;
    u8 *head = malloc(hlen ? hlen : 1);
    if (!head || rd_file(0, 0, head, hlen))
        die("zine: %s", zwhy("cannot be read"));
    struct zh_rd rd = { rd_file, 0, rdwin, sizeof rdwin };
    struct zh_info inf;
    char msg[128];
    if (zh_check_rd(head, hlen, len, &rd, name, &inf, msg, sizeof msg))
        die("zine: %s", msg);
    struct uzine_info info;
    int r = zine_sys(ZINE_INFO, &info, sizeof info);
    if (r < 0)
        die("zine: this Zenix kernel %s", "has no ZINE");
    if (!info.fast_total)
        die("ZINE needs fast RAM (this machine has only chip RAM)%s", "");
    if (info.layout != ZINE_LAYOUT)
        die("zine: ZINE's files and the kernel are from different versions: %s", "finish the update first");
    /* memory (d29 16.6, 16.12). Path A: the whole file in ZINE's volume (an overlaid program loads its later parts
     * from there, a program may read its own file) and its hunks loaded beside it in the session, when both fit in
     * all of Zenix's free memory. Path B, else: zine loads the hunks itself now, from the file, into memory of its
     * own that becomes the program's (fast and any hunks in fast RAM, chip ones in the chip RAM Zenix has free now),
     * and the file is not kept; an overlaid program cannot be loaded so. */
    u32 hunks = inf.bytes[0] + inf.bytes[1] + inf.bytes[2] + inf.node_max +
                (inf.ovl ? inf.ovl_words * 4 + (inf.tabsize + 1) * 4 : 0);
    u32 avail = info.chip_free + info.fast_free;
    int whole = len <= avail && hunks <= avail - len;
    if (!whole && inf.ovl) {
        dprintf(2, "zine: %s does not fit in memory: its file (%u KB, all in memory during the session: it is "
                "overlaid) and its hunks (%u KB) need more than the %u KB free\n", name, len >> 10, hunks >> 10,
                avail >> 10);
        return 1;
    }
    if (!whole && hunks > avail) {
        dprintf(2, "zine: %s does not fit in memory: its hunks need %u KB, more than the %u KB free\n", name,
                hunks >> 10, avail >> 10);
        return 1;
    }
    if (!whole && inf.bytes[1] > info.chip_free) {
        dprintf(2, "zine: %s does not fit in memory: it is loaded from its file before the hand-over (its file and "
                "its hunks do not fit together), and its chip RAM hunks (%u KB) need more than the %u KB of chip RAM "
                "Zenix has free now\n", name, inf.bytes[1] >> 10, info.chip_free >> 10);
        return 1;
    }
    /* the volume (5.6, 16.12): the program's directory with its subdirectories (16.10), at most ZV_MAXFILES (2000) entries and
     * 48 MB, in half the fast RAM the program leaves free (its hunks, and its file in path A: a program reads its
     * data files into memory of its own, so as much again stays for that). Over that: archives and disk images left
     * out first, then the largest files until the rest fits; each left out file is named (the first 5). */
    u32 dl = (u32)(name - path);
    if (dl >= sizeof W.path - 64)
        dl = 0;
    memcpy(W.path, path, dl);
    W.path[dl] = 0;
    if (!dl)
        strcpy(W.path, "./");
    W.top = strlen(W.path);
    W.path[--W.top] = 0;                /* without its '/' (the walk puts one before each name) */
    W.sz = malloc(ZV_MAXFILES * sizeof *W.sz);
    walk(W.top, 0, 0);                  /* pass 1: count */
    W.top++;
    u32 nent = W.n, fneed = inf.bytes[0] + inf.bytes[2] + (whole ? len : 0);
    u32 budget = info.fast_free > fneed ? (info.fast_free - fneed) / 2 : 0;
    W.thr = 0xFFFFFFFFu;
    u32 vbytes = W.bytes;
    if (!W.over && W.sz && !zv_fits(nent + 1, vbytes, budget)) {
        u32 na = 0;                     /* without the archives and disk images */
        for (u32 i = 0; i < W.nsz; i++)
            if (!(W.sz[i] & ZA_ARCH))
                na += W.sz[i];
        W.noarch = 1;
        vbytes = na;
        if (!zv_fits(nent + 1, na, budget)) {   /* the smallest files first: the largest size that still fits */
            for (u32 i = 1; i < W.nsz; i++)     /* (a few hundred at most: insertion sort) */
                for (u32 j = i; j && W.sz[j - 1] > W.sz[j]; j--) {
                    u32 t = W.sz[j];
                    W.sz[j] = W.sz[j - 1], W.sz[j - 1] = t;
                }
            u32 sum = 0, i = 0;
            W.thr = 0;
            while (i < W.nsz && !(W.sz[i] & ZA_ARCH)) {
                u32 k = i, add = 0;     /* all files of this size, or none */
                while (k < W.nsz && W.sz[k] == W.sz[i])
                    add += W.sz[k++];
                if (add > ZV_MAXBYTES || sum + add > budget || sum + add > ZV_MAXBYTES)
                    break;
                sum += add;
                W.thr = W.sz[i];
                i = k;
            }
            vbytes = sum;
        }
        dprintf(2, "zine: %s's directory holds %u KB, more than ZINE's volume takes on this machine now (%u KB: "
                "half the fast RAM the program leaves free); %s are left out, a program that loads them will not "
                "find them\n", name, (W.bytes + 1023) >> 10, budget >> 10,
                W.thr == 0xFFFFFFFFu ? "its archives and disk images" : "its archives, disk images and largest files");
    }
    if (W.over) {
        dprintf(2, "zine: only %s goes into ZINE's volume: its directory holds more than %u files and directories: "
                "a program that loads files may not find them; give it a directory of its own\n", name, ZV_MAXFILES);
        nent = 0, vbytes = 0;
    }
    /* Zenix's Amiga fonts (16.13): /usr/share/fonts/amiga goes into the volume's FONTS (diskfont's OpenDiskFont looks
     * there), when it fits in what the program's files leave of the budget and the ZV_MAXFILES entries; else left out, said */
    u32 fn = 0, fbytes = 0;
    static char wpath[sizeof W.path];
    {
        u32 n0 = W.n, b0 = W.bytes, *sz0 = W.sz, top0 = W.top;
        memcpy(wpath, W.path, sizeof wpath);
        int over0 = W.over;
        W.n = W.bytes = 0, W.over = 0, W.sz = 0, W.fonts = 1;
        strcpy(W.path, FONTDIR);
        W.top = sizeof FONTDIR;
        walk(sizeof FONTDIR - 1, 0, 0);
        fn = W.n, fbytes = W.bytes;
        if (fn && (W.over || !zv_fits(nent + fn + 2, vbytes + fbytes, budget))) {
            dprintf(2, "zine: the Amiga fonts in " FONTDIR " (%u KB) are left out: ZINE's volume has no room for them "
                    "on this machine now; a program that opens one of them will not find it\n", (fbytes + 1023) >> 10);
            fn = fbytes = 0;
        }
        W.n = n0, W.bytes = b0, W.sz = sz0, W.top = top0, W.over = over0, W.fonts = 0;
        memcpy(W.path, wpath, sizeof wpath);
    }
    u32 pf = whole ? len : 0;           /* the program's file in the volume */
    u32 vsize = zv_need(nent + fn + 3, vbytes + fbytes + pf, nent + fn + 1);
    u8 *data;
    W.vol = sys_memalloc(vsize);
    if (!W.vol || zv_init(W.vol, vsize, nent + fn + 3, "ZINE"))
        die("zine: not enough memory for %s", name);
    if (whole) {                        /* path A: the file read straight into the volume */
        if (zv_add(W.vol, 0, name, 0, st.mode, st.mtime, len, &data) < 0)
            die("zine: not enough memory for %s", name);
        if (rd_file(0, 0, data, len))
            die("zine: %s", zwhy("cannot be read"));
        if (inf.ovl) {                  /* checked again whole, its parts too (16.6) */
            if (zh_check(data, len, name, &inf, msg, sizeof msg))
                die("zine: %s", msg);
            hunks = inf.bytes[0] + inf.bytes[1] + inf.bytes[2] + inf.node_max + inf.ovl_words * 4 +
                    (inf.tabsize + 1) * 4;
            if (hunks > avail - len) {
                dprintf(2, "zine: %s does not fit in memory: its file (%u KB, all in memory during the session) and "
                        "its hunks (%u KB) need more than the %u KB free\n", name, len >> 10, hunks >> 10, avail >> 10);
                return 1;
            }
        }
    }
    if (inf.ovl && inf.nodes)
        dprintf(1, "ZINE: %s is overlaid: %u later parts (%u KB together, the largest %u KB), loaded from memory "
                "when it asks for them\n", name, inf.nodes, (inf.node_all + 1023) >> 10, (inf.node_max + 1023) >> 10);
    else if (inf.ovl)                   /* a cruncher's data container (Spaceballs' Peek): d29 16.7 */
        dprintf(1, "ZINE: %s keeps %u KB of data after its overlay table and reads it itself through its overlay "
                "file, from memory\n", name, (len - inf.end + 1023) >> 10);
    if (nent) {
        W.path[W.top - 1] = 0;
        walk(W.top - 1, 0, 0);          /* pass 2: fill */
        if (W.left > 5)
            dprintf(2, "zine: and %u more files left out (%u KB together)\n", W.left - 5, (W.leftbytes + 1023) >> 10);
    }
    if (fn) {                           /* the fonts into FONTS (the program's own, or one made for them) */
        zu32 fd;
        int e = zv_find(W.vol, 0, "FONTS:", &fd);
        if (e)
            e = zv_add(W.vol, 0, "Fonts", 1, S_IFDIR | 0755, sys_time(), 0, 0);
        else
            e = (int)fd;
        if (e > 0) {
            W.fonts = 1, W.thr = 0xFFFFFFFFu, W.noarch = 0, W.left = 0;
            strcpy(W.path, FONTDIR);
            W.top = sizeof FONTDIR;
            walk(sizeof FONTDIR - 1, (u32)e, 0);
            W.fonts = 0;
        }
    }
    u8 *vol = W.vol;
    ((struct zv_hdr *)vol)->now = sys_time();  /* dos DateStamp's clock (16.13) */
    /* a 68040 or 68060 (AttnFlags; a 68060 without 68060.library at boot says 68040, so then both): Motorola's
     * packages for what the CPU lacks (16.14); not on Emu68, which runs every instruction itself */
    if ((info.attn & 0x88) && !(info.size >= UZINE_INFO_MACHINE && (info.machine & UZI_EMU68))) {
        struct zv_hdr *vh = (struct zv_hdr *)vol;
        vh->sp[0] = load_sp("/lib/zine/sp060", 60, &vh->sp_size[0]);
        if (!(info.attn & 0x80))
            vh->sp[1] = load_sp("/lib/zine/sp040", 40, &vh->sp_size[1]);
    }
    u32 bsize, blob = load_blob(&bsize);
    /* the ZINE block: the saved chip RAM (Zenix's used chip blocks, low memory), the plan, the stack, the libraries */
    u32 ksize = info.chip_total - info.chip_free + (256u << 10);
    struct zine_blk *b = sys_memalloc(ksize);
    if (!b)
        die("zine: not enough memory for ZINE's own %s", "block");
    /* path B: the hunks loaded now, after the volume, the blob and the ZINE block have their fast RAM (else they
     * could only be had in chip RAM, which is the program's) */
    if (!whole) {
        zu32 preseg = 0;
        struct zh_seg *work = malloc(inf.nhunks * sizeof *work + 8);
        P.chip_end = info.chip_total;
        struct zh_ops ops = { pre_alloc, pre_free, 0 };
        int e = !work ? -ENOMEM : pre_plan(head, &inf);
        if (!e)
            e = zh_load_rd(head, hlen, len, &rd, &inf, &ops, work, &preseg);
        if (e) {
            dprintf(2, "zine: %s could not be loaded from its file: %s\n", name, e == -ENOMEM ?
                    "not enough memory in one piece for its hunks" : "its file changed, or a read failed");
            return 1;
        }
        free(work);
        dprintf(1, "ZINE: %s is loaded from its file now (its file, %u KB, and its hunks, %u KB, do not fit in memory "
                "together); its file is not kept during the session\n", name, len >> 10, hunks >> 10);
        struct zv_hdr *vh = (struct zv_hdr *)vol;
        vh->pre_seg = preseg;
        vh->nkeep = P.n;
        for (u32 i = 0; i < P.n; i++)
            vh->keep[i].addr = P.r[i].base, vh->keep[i].size = P.r[i].size;
    }
    sys_close(pfd);
    free(head);
    for (int tries = 0; tries < 2; tries++) {
        if (tries && !(b = sys_memalloc(ksize)))
            die("zine: not enough memory for ZINE's own %s", "block");
        memset(b, 0, sizeof *b);
        b->magic = ZB_MAGIC;
        b->size = ksize;
        strncpy(b->name, name, sizeof b->name - 1);
        u32 n = 0;
        for (int i = 2; i < argc; i++) {
            u32 l = strlen(argv[i]);
            if (n + l + 2 > ZB_ARGMAX)
                die("zine: the arguments are longer than %s bytes", "255");
            if (n)
                b->args[n++] = ' ';
            memcpy(b->args + n, argv[i], l);
            n += l;
        }
        b->args[n++] = '\n';
        b->arglen = n;
        if (!tries)
            dprintf(1, "ZINE: handing the machine to %s (Ctrl-Amiga-Amiga restarts the machine into Zenix)\n", name);
        struct uzine_go g = { sizeof g, blob, bsize, (u32)vol, vsize, (u32)b, ksize, 0 };
        r = zine_sys(ZINE_GO, &g, 0);
        if (r != -ENOMEM || !b->need || b->need <= ksize)
            break;
        ksize = b->need;                /* Zenix's memory changed since ZINE_INFO: once more with the size asked */
        sys_memfree(b);
    }
    switch (r) {
    case 0:
        break;
    case -EPERM:
        die("zine: not allowed here (only at the Amiga's own console)%s", "");
        break;
    case -EBUSY:
        die("zine: not now (another ZINE session, or an update %s)", "not yet confirmed");
        break;
    case -ENOEXEC:
        die("zine: ZINE's files and the kernel are from different versions: %s", "finish the update first");
        break;
    case -ENODEV:
        die("ZINE needs fast RAM (%s)", "this machine has only chip RAM, or ZINE's memory is not in fast RAM");
        break;
    case -ENOSPC:
        die("zine: Zenix holds more than half of the fast RAM at the moment: %s", "stop some programs first");
        break;
    case -EIO:
        die("zine: a filesystem did not answer the sync: %s", "not handing over");
        break;
    default:
        die("zine: the hand-over failed (%s)", zstrerror(r) ? zstrerror(r) : "unknown error");
    }
    if (b->outlen)
        sys_write(1, b->out, b->outlen);
    if (b->outlen && b->out[b->outlen - 1] != '\n')
        dprintf(1, "\n");
    if (b->outlost)
        dprintf(1, "ZINE: %u more bytes of output were not kept\n", b->outlost);
    dprintf(1, "ZINE: %s had %u KB chip and %u KB fast RAM\n", name, b->chip_free >> 10, b->fast_free >> 10);
    int rc = 1;
    switch (b->how) {
    case ZH_RETURN:
    case ZH_EXIT:
        dprintf(1, "ZINE: %s ended (return code %d) after %u s; Zenix is back\n", name, b->code, b->secs);
        rc = b->code & 255;
        break;
    case ZH_REBOOT:
        dprintf(1, "ZINE: %s asked to restart the machine; Zenix is back instead\n", name);
        break;
    case ZH_UNSUP: {
        const char *lib = b->lib < NLIBS ? libs[b->lib] : "a library";
        const char *fn = b->lib < NLIBS ? zl_name(lib, b->lvo) : 0;
        if (!fn && b->lib < NLIBS)
            fn = zl_name40(lib, b->lvo);
        dprintf(1, "ZINE: %s called %s %s%s(offset -%u, from %08x), which ZINE does not support yet: stopped\n",
                name, lib, fn ? fn : "", fn ? " " : "", b->lvo, b->pc);
        break;
    }
    case ZH_CRASH:
        if (b->vec == 11 && fpu_missing(b->addr))
            dprintf(1, "ZINE: %s crashed: line F instruction at %08x (instruction %04x %04x: %s, which this CPU "
                    "does not have in hardware; AmigaOS's 68040/68060.library emulates it, ZINE only on a 68040 or 68060 with an "
                    "FPU)\n", name, b->pc,
                    b->addr >> 16, b->addr & 0xFFFF, fpu_missing(b->addr));
        else if (b->vec == 4 || b->vec == 10 || b->vec == 11 || b->vec == 61)
            dprintf(1, "ZINE: %s crashed: %s at %08x (instruction %04x %04x)\n", name, vecname(b->vec), b->pc,
                    b->addr >> 16, b->addr & 0xFFFF);
        else if (vecname(b->vec))
            dprintf(1, "ZINE: %s crashed: %s at %08x (access %08x)\n", name, vecname(b->vec), b->pc, b->addr);
        else
            dprintf(1, "ZINE: %s crashed: exception %u at %08x\n", name, b->vec, b->pc);
        break;
    case ZH_ALERT:
        dprintf(1, "ZINE: %s crashed (alert %08x%s)\n", name, b->addr,
                (b->addr & 0x7FFFFFFFu) == 0x0700000Cu ? ": its overlay supervisor could not load a part" : "");
        break;
    case ZH_STOP:
        dprintf(1, b->why == ZS_WAIT ? "ZINE: %s waits for a signal nothing can send: stopped\n" :
                   "ZINE: %s uses graphics' own displays, which ZINE does not support yet: stopped\n", name);
        break;
    case ZH_LOAD:
        dprintf(1, "ZINE: %s could not be loaded (%s)\n", name, b->why == ZS_NOMEM ? "not enough memory" :
                b->why == ZS_NOFILE ? "not in ZINE's volume" : "a damaged file");
        break;
    default:
        dprintf(1, "ZINE: %s ended in an unknown way (%u)\n", name, b->how);
    }
    return rc;
}
