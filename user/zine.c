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

/* The volume's walk (5.6, 16.10): the program's directory and its subdirectories, to ZV_DEPTH levels. Pass 1 (W.vol
 * 0) counts the entries and the files' bytes, stopping once past 5.6's limits; pass 2 adds them to W.vol. Recursive,
 * a small frame a level (no path on the stack: W.path is the one being read). */
#define ZV_DEPTH 8
static struct {
    char path[400];                     /* the Zenix path being read */
    u32 top;                            /* bytes of the program's directory with its '/' (names shown from there) */
    u32 n, bytes;                       /* pass 1: entries (files and directories) and bytes found */
    int over;                           /* pass 1: past the limits, stopped */
    u8 *vol;                            /* pass 2: the volume being filled */
} W;

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
        if (de.name[0] == '.' || (!depth && !strcmp(de.name, name)))
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
        if (dir && depth + 1 >= ZV_DEPTH) {
            if (W.vol)
                dprintf(2, "zine: %s left out: deeper than %u directories\n", W.path + W.top, ZV_DEPTH);
            continue;
        }
        if (!W.vol) {
            W.n++;
            if (!dir && st.size > ZV_MAXBYTES - W.bytes)
                W.over = 1;
            else if (!dir)
                W.bytes += st.size;
            if (W.n > ZV_MAXFILES)
                W.over = 1;
            if (dir && !W.over)
                walk(plen + 1 + l, 0, depth + 1);
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
    /* memory (d29 16.6): during the session the whole file is in ZINE's volume (an overlaid program loads its
     * later parts from there) and its hunks are loaded beside it: the root's, and for an overlaid program at
     * least its largest part. Less than that free in all of Zenix's pools can never run. */
    u32 hunks = inf.bytes[0] + inf.bytes[1] + inf.bytes[2] + inf.node_max +
                (inf.ovl ? inf.ovl_words * 4 + (inf.tabsize + 1) * 4 : 0);
    u32 avail = info.chip_free + info.fast_free;
    if (len > avail || hunks > avail - len) {
        dprintf(2, "zine: %s does not fit in memory: its file (%u KB, all in memory during the session) and its "
                "hunks (%u KB) need more than the %u KB free\n", name, len >> 10, hunks >> 10, avail >> 10);
        return 1;
    }
    if (inf.ovl && inf.nodes)
        dprintf(1, "ZINE: %s is overlaid: %u later parts (%u KB together, the largest %u KB), loaded from memory "
                "when it asks for them\n", name, inf.nodes, (inf.node_all + 1023) >> 10, (inf.node_max + 1023) >> 10);
    else if (inf.ovl)                   /* a cruncher's data container (Spaceballs' Peek): d29 16.7 */
        dprintf(1, "ZINE: %s keeps %u KB of data after its overlay table and reads it itself through its overlay "
                "file, from memory\n", name, (len - inf.end + 1023) >> 10);
    /* the volume (5.6): the program's directory with its subdirectories (16.10), when it fits (at most 500 entries,
     * 16 MB and half the free fast RAM); else the program alone */
    u32 dl = (u32)(name - path);
    if (dl >= sizeof W.path - 64)
        dl = 0;
    memcpy(W.path, path, dl);
    W.path[dl] = 0;
    if (!dl)
        strcpy(W.path, "./");
    W.top = strlen(W.path);
    W.path[--W.top] = 0;                /* without its '/' (the walk puts one before each name) */
    walk(W.top, 0, 0);                  /* pass 1: count */
    W.top++;
    u32 nent = W.n;
    if (W.over || !zv_fits(nent + 1, W.bytes + len, info.fast_free)) {
        dprintf(2, "zine: only %s goes into ZINE's volume: its directory holds %s%u files and directories and %u KB "
                "(ZINE takes at most 500, 16 MB and half the free fast RAM, %u KB): a program that loads files may "
                "not find them; give it a directory of its own\n", name, W.over ? "at least " : "", nent,
                (W.bytes + 1023) >> 10, info.fast_free >> 11);
        nent = 0, W.bytes = 0;
    }
    u32 vsize = zv_need(nent + 2, W.bytes + len, nent + 1);
    u8 *data;
    W.vol = sys_memalloc(vsize);
    if (!W.vol || zv_init(W.vol, vsize, nent + 2, "ZINE") || zv_add(W.vol, 0, name, 0, st.mode, st.mtime, len, &data) < 0)
        die("zine: not enough memory for %s", name);
    memcpy(data, file, len);
    free(file);
    if (nent) {
        W.path[W.top - 1] = 0;
        walk(W.top - 1, 0, 0);          /* pass 2: fill */
    }
    u8 *vol = W.vol;
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
