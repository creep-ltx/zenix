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

static const char *const libs[4] = { "exec.library", "graphics.library", "dos.library", "intuition.library" };

static const char *vecname(u32 v)
{
    static const char *const n[] = { 0, 0, "bus error", "address error", "illegal instruction", "zero divide",
                                     "CHK instruction", "TRAPV instruction", "privilege violation", "trace",
                                     "line A instruction", "line F instruction" };
    return v < 12 ? n[v] : 0;
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
    struct ustat st;
    u32 len;
    u8 *file = slurp(path, &len, &st);
    if (!file)
        die("zine: %s", zwhy("cannot be read"));
    struct zh_info inf;
    char msg[128];
    if (zh_check(file, len, name, &inf, msg, sizeof msg))
        die("zine: %s", msg);
    struct uzine_info info;
    int r = zine_sys(ZINE_INFO, &info, sizeof info);
    if (r < 0)
        die("zine: this Zenix kernel %s", "has no ZINE");
    if (!info.fast_total)
        die("ZINE needs fast RAM (this machine has only chip RAM)%s", "");
    if (info.layout != ZINE_LAYOUT)
        die("zine: ZINE's files and the kernel are from different versions: %s", "finish the update first");
    /* the volume (5.6): the program's directory, its plain files (not its subdirectories yet), when they fit (at
     * most 500 files, 16 MB and half the free fast RAM); else the program alone */
    char dir[256];
    u32 dl = (u32)(name - path);
    if (dl >= sizeof dir)
        dl = 0;
    memcpy(dir, path, dl);
    dir[dl] = 0;
    if (!dl)
        strcpy(dir, ".");
    u32 nfiles = 0, bytes = 0;
    struct udirent de;
    struct ustat fst;
    int dfd = sys_stat(dir, &fst) == 0 && may_read(&fst) ? sys_open(dir, 0) : -1;
    char fp[320];
    if (dfd >= 0) {
        while (sys_readdir(dfd, &de) > 0) {
            if (de.name[0] == '.' || !strcmp(de.name, name))
                continue;
            snprintf(fp, sizeof fp, "%s/%s", dir, de.name);
            if (sys_stat(fp, &fst) == 0 && (fst.mode & S_IFMT) == S_IFREG && may_read(&fst))
                nfiles++, bytes += fst.size;
        }
        sys_close(dfd);
    }
    if (!zv_fits(nfiles + 1, bytes + len, info.fast_free))
        nfiles = 0, bytes = 0;
    u32 vsize = zv_need(nfiles + 2, bytes + len, nfiles + 1);
    u8 *vol = sys_memalloc(vsize), *data;
    if (!vol || zv_init(vol, vsize, nfiles + 2, "ZINE") || zv_add(vol, 0, name, 0, st.mode, st.mtime, len, &data) < 0)
        die("zine: not enough memory for %s", name);
    memcpy(data, file, len);
    free(file);
    if (nfiles && (dfd = sys_open(dir, 0)) >= 0) {
        u32 added = 0;
        while (added < nfiles && sys_readdir(dfd, &de) > 0) {
            if (de.name[0] == '.' || !strcmp(de.name, name))
                continue;
            snprintf(fp, sizeof fp, "%s/%s", dir, de.name);
            if (sys_stat(fp, &fst) || (fst.mode & S_IFMT) != S_IFREG || !may_read(&fst))
                continue;
            int e = zv_add(vol, 0, de.name, 0, fst.mode, fst.mtime, fst.size, &data);
            if (e < 0) {
                dprintf(2, "zine: %s left out: %s\n", de.name, zv_reason(e));
                continue;
            }
            int ffd = sys_open(fp, 0);
            u32 got = 0;
            int n;
            while (ffd >= 0 && got < fst.size && (n = sys_read(ffd, data + got, fst.size - got)) > 0)
                got += (u32)n;
            if (ffd >= 0)
                sys_close(ffd);
            added++;
        }
        sys_close(dfd);
    }
    u32 bsize, blob = load_blob(&bsize);
    /* the ZINE block: the saved chip RAM (Zenix's used chip blocks, low memory), the plan, the stack, the libraries */
    u32 ksize = info.chip_total - info.chip_free + (256u << 10);
    struct zine_blk *b = 0;
    for (int tries = 0; tries < 2; tries++) {
        b = sys_memalloc(ksize);
        if (!b)
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
        const char *lib = b->lib < 4 ? libs[b->lib] : "a library";
        const char *fn = b->lib < 4 ? zl_name(lib, b->lvo) : 0;
        dprintf(1, "ZINE: %s called %s %s%s(offset -%u, from %08x), which ZINE does not support yet: stopped\n",
                name, lib, fn ? fn : "", fn ? " " : "", b->lvo, b->pc);
        break;
    }
    case ZH_CRASH:
        if (b->vec == 11 && fpu_missing(b->addr))
            dprintf(1, "ZINE: %s crashed: line F instruction at %08x (instruction %04x %04x: %s, which this CPU "
                    "does not have in hardware; AmigaOS's 68040/68060.library emulates it, ZINE does not)\n", name, b->pc,
                    b->addr >> 16, b->addr & 0xFFFF, fpu_missing(b->addr));
        else if (b->vec == 4 || b->vec == 10 || b->vec == 11)
            dprintf(1, "ZINE: %s crashed: %s at %08x (instruction %04x %04x)\n", name, vecname(b->vec), b->pc,
                    b->addr >> 16, b->addr & 0xFFFF);
        else if (vecname(b->vec))
            dprintf(1, "ZINE: %s crashed: %s at %08x (access %08x)\n", name, vecname(b->vec), b->pc, b->addr);
        else
            dprintf(1, "ZINE: %s crashed: exception %u at %08x\n", name, b->vec, b->pc);
        break;
    case ZH_ALERT:
        dprintf(1, "ZINE: %s crashed (alert %08x)\n", name, b->addr);
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
