/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's blob: libraries from files and what builds them (docs/d29-zine.md 16.8). OpenLibrary of a library ZINE has
 * not built in looks for it in LIBS: (as assigned: ZINE starts it as the program's volume's LIBS directory, the
 * program's own directory and RAM:), LoadSegs it, finds its RomTag and initialises it as InitResident does (auto-init:
 * MakeLibrary from its table, then AddLibrary; else its init code is called), then calls its Open vector. Written from
 * the RKRM Includes and Autodocs (InitResident, InitStruct, MakeLibrary, MakeFunctions, OpenLibrary, CloseLibrary,
 * the semaphores) and the NDK 3.2's include files for numbers; never from AmigaOS code.
 *
 * Also the exec calls a disk library uses (semaphores: one task, so they never wait), and audio.device's channel
 * allocation (programs that play on Paula themselves ask it for their channels first). */
#include "blob.h"

static u32 slen(const char *s)
{
    u32 n = 0;
    while (s && s[n])
        n++;
    return n;
}

static void say(const char *a, const char *b, const char *c)
{
    zb_out(a, slen(a));
    zb_out(b, slen(b));
    zb_out(c, slen(c));
}

static void say_num(const char *a, u32 v, const char *c)
{
    char n[11];
    int i = 10;
    n[10] = 0;
    do
        n[--i] = (char)('0' + v % 10), v /= 10;
    while (v && i);
    say(a, n + i, c);
}

#define LIBLIST ((struct zlist *)(ZEXEC + ZO_ExecBase_LibList))
#define RESLIST ((struct zlist *)(ZEXEC + ZO_ExecBase_ResourceList))
#define LIBF_CHANGED 2
#define RTC_MATCHWORD 0x4AFC
#define RTF_AUTOINIT 0x80
#define NT_DEVICE 3

/* ---- InitStruct (-78): clear size bytes, then the table's byte commands ddssnnnn: dd 00 the next destination, 01
 * the same, repeated, 10 a byte offset follows, 11 a 24-bit offset follows; ss 00 long, 01 word, 10 byte; nnnn the
 * count - 1. Commands are read from the next even byte, words and longs from the next even byte; 0 ends it ---- */
void c_InitStruct(const u8 *t, u8 *mem, u32 size)
{
    if (size)
        zmemset(mem, 0, size & ~1u);
    u8 *d = mem;
    for (u32 guard = 0; t && guard < 65536; guard++) {
        t = (const u8 *)(((u32)t + 1) & ~1u);
        u32 cmd = *t++;
        if (!cmd)
            return;
        u32 dd = cmd >> 6, ss = cmd >> 4 & 3, cnt = (cmd & 15) + 1;
        if (dd == 2)
            d = mem + *t++;
        else if (dd == 3)
            d = mem + ((u32)t[0] << 16 | (u32)t[1] << 8 | t[2]), t += 3;
        if (ss == 3) {
            say("ZINE: ", Z.blk->name, " gave InitStruct a table with a command of size 11 (an error): stopped "
                "there\n");
            return;
        }
        u32 w = ss == 0 ? 4 : ss == 1 ? 2 : 1;
        if (w > 1)
            t = (const u8 *)(((u32)t + 1) & ~1u);
        for (u32 i = 0; i < cnt; i++) {
            zmemcpy(d, t, w);           /* bytewise: d may be odd for a byte, the table's data even */
            d += w;
            if (dd != 1)
                t += w;
        }
        if (dd == 1)
            t += w;
    }
}

/* ---- MakeFunctions (-90): the jump table below target; funcs absolute longs, or (disp) words added to disp;
 * either ends at -1. The table's bytes */
u32 c_MakeFunctions(u8 *target, const u8 *funcs, u32 disp)
{
    u32 n = 0;
    for (;; n++) {
        u32 f;
        if (disp) {
            s16 v = *(const s16 *)(funcs + 2 * n);
            if (v == -1)
                break;
            f = disp + (u32)(s32)v;
        } else {
            f = *(const u32 *)(funcs + 4 * n);
            if (f == 0xFFFFFFFFu)
                break;
        }
        u8 *slot = target - 6 * (n + 1);
        *(u16 *)slot = 0x4EF9;          /* jmp abs.l */
        *(u32 *)(slot + 2) = f;
        if (n == 1023)                  /* far more than any library's; a table without its end stops here */
            break;
    }
    extern void zx_CacheClearU(void);
    zx_CacheClearU();                   /* code was written (the autodoc: the instruction cache is cleared) */
    return n * 6;
}

/* ---- MakeLibrary (-84): the vectors (first word -1: word displacements from vectors after it), the base data
 * (dsize bytes, the Library at its start) cleared and InitStruct'd, the base longword aligned (V36), then init called
 * with d0 the base, a0 segList, a6 ExecBase: its answer is the library (0: it failed and freed itself) ---- */
u32 c_MakeLibrary(const u8 *vec, const u8 *st, u32 init, u32 dsize, u32 seg)
{
    u32 n = 0, rel = *(const s16 *)vec == -1;
    const u8 *arr = rel ? vec + 2 : vec;
    while (n < 1024 && (rel ? *(const s16 *)(arr + 2 * n) != -1 : *(const u32 *)(arr + 4 * n) != 0xFFFFFFFFu))
        n++;
    u32 neg = (n * 6 + 3) & ~3u, pos = (dsize + 3) & ~3u;
    if (pos < ZO_Library_SIZEOF)
        pos = (ZO_Library_SIZEOF + 3) & ~3u;
    u32 mem = zx_AllocMem(neg + pos, MEMF_PUBLIC | MEMF_CLEAR);
    if (!mem)
        return 0;
    u8 *lib = (u8 *)mem + neg;
    c_MakeFunctions(lib, arr, rel ? (u32)vec : 0);
    struct zlib *l = (struct zlib *)lib;
    l->negsize = (u16)neg;
    l->possize = (u16)dsize;
    if (st)
        c_InitStruct(st, lib, 0);
    if (init)
        return zb_libcall(init, ZEXEC, (u32)lib, seg, 0, 0);
    return (u32)lib;
}

void c_AddLibrary(struct zlib *l)
{
    l->node.type = NT_LIBRARY;
    l->flags |= LIBF_CHANGED;
    c_Enqueue(LIBLIST, &l->node);
}

/* RemLibrary (-402): the library's own Expunge, whose answer (its segment list, or 0) is the answer */
u32 c_RemLibrary(u32 lib)
{
    return lib ? zb_libcall(lib - 18, lib, 0, 0, 0, 0) : 0;
}

void c_SumLibrary(u32 lib) { (void)lib; }   /* ZINE does not check libraries' sums */

void c_AddResource(struct znode *r) { if (r) c_Enqueue(RESLIST, r); }

/* FindResident (-96): ZINE has no resident modules (ResModules is empty) */
u32 c_FindResident(const char *name) { (void)name; return 0; }

/* InitResident (-102): a RomTag checked (its match word and the pointer to itself); auto-init: MakeLibrary from the
 * four longwords rt_Init points to (the base's size, the vectors, the InitStruct table, the init code), and a
 * library added to the library list, a resource to the resources; else rt_Init called with d0 0, a0 the segment
 * list, a6 ExecBase. The answer: what was made (0: nothing) */
u32 c_InitResident(const u8 *rt, u32 seg)
{
    if (!rt || *(const u16 *)rt != RTC_MATCHWORD || *(const u32 *)(rt + 2) != (u32)rt)
        return 0;
    u32 init = *(const u32 *)(rt + 22);
    if (!(rt[10] & RTF_AUTOINIT))
        return init ? zb_libcall(init, ZEXEC, 0, seg, seg, 0) : 0;
    const u32 *ai = (const u32 *)init;
    if (!ai)
        return 0;
    u32 lib = c_MakeLibrary((const u8 *)ai[1], (const u8 *)ai[2], ai[3], ai[0], seg);
    if (lib) {
        if (rt[12] == NT_LIBRARY)
            c_AddLibrary((struct zlib *)lib);
        else if (rt[12] == NT_RESOURCE)
            c_AddResource((struct znode *)lib);
    }
    return lib;
}

/* ---- libraries from files ---- */
static int on_list(u32 lib)
{
    for (struct znode *n = LIBLIST->head; n->succ; n = n->succ)
        if ((u32)n == lib)
            return 1;
    return 0;
}

/* the first RomTag in the segment list: a match word whose next longword points to it (the autodoc: the first code
 * hunk holds it; every hunk is searched) */
static const u8 *romtag(u32 seg)
{
    for (u32 n = 0; seg && n < 1024; n++) {
        u32 a = (seg << 2) - 4, size = *(const u32 *)a;
        const u8 *p = (const u8 *)(a + 8);
        for (u32 o = 0; size >= 8 && o + 26 <= size - 8; o += 2)
            if (*(const u16 *)(p + o) == RTC_MATCHWORD && *(const u32 *)(p + o + 2) == (u32)(p + o))
                return p + o;
        seg = *(const u32 *)(a + 4);
    }
    return 0;
}

/* the library's file loaded and initialised: its base, or 0 with the reason in the output */
static u32 lib_load(const char *name, const char *path)
{
    u32 seg = c_LoadSeg(path, 0, 0);
    if (!seg)
        return 0;                       /* LoadSeg said why */
    const u8 *rt = romtag(seg);
    if (!rt) {
        say("ZINE: ", path, " holds no RomTag: not a library\n");
        c_UnLoadSeg(seg);
        return 0;
    }
    u32 lib = c_InitResident(rt, seg);
    if (!lib) {
        say("ZINE: ", path, "'s initialisation failed (its init code answered 0)\n");
        c_UnLoadSeg(seg);
        return 0;
    }
    if (!on_list(lib))                  /* an init that made the library without adding it */
        c_AddLibrary((struct zlib *)lib);
    struct zlib *l = (struct zlib *)lib;
    say("ZINE: ", name, "");
    say_num(" (version ", l->version, "");
    say(") loaded from ", path, "\n");
    return lib;
}

/* OpenLibrary (-552) of a library not built in: one on the library list, or loaded from LIBS: (a name with ':' or
 * '/': from there); the version checked, then its Open vector (a6 the base, d0 the version): its answer */
u32 zb_lib_open(const char *name, u32 version)
{
    const char *bn = name;
    for (const char *s = name; *s; s++)
        if (*s == ':' || *s == '/')
            bn = s + 1;
    u32 lib = c_FindName(LIBLIST, bn);
    if (!lib) {
        char path[200], where[200];
        u32 n = slen(name);
        if (bn == name) {               /* LIBS:name */
            if (n + 6 > sizeof path)
                n = sizeof path - 6;
            zmemcpy(path, "LIBS:", 5);
            zmemcpy(path + 5, name, n);
            path[5 + n] = 0;
        } else {
            if (n + 1 > sizeof path)
                n = sizeof path - 1;
            zmemcpy(path, name, n);
            path[n] = 0;
        }
        if (!zb_file_exists(path, where, sizeof where)) {
            say("ZINE: no library ", name, "\n");
            return 0;
        }
        lib = lib_load(name, where);
        if (!lib)
            return 0;
    }
    struct zlib *l = (struct zlib *)lib;
    if (l->version < version) {
        say("ZINE: ", bn, " is ");
        say_num("version ", l->version, "");
        say_num(", the program asks for version ", version, "\n");
        return 0;
    }
    return zb_libcall(lib - 6, lib, version, 0, 0, 0);
}

/* CloseLibrary (-414) of a library not built in: its Close vector; an answer is its segment list (it expunged
 * itself): unloaded. Libraries stay loaded while unused, as on AmigaOS, until memory runs short (zb_lib_flush). */
void zb_lib_close(u32 lib)
{
    if (!lib || !on_list(lib))
        return;
    u32 seg = zb_libcall(lib - 12, lib, 0, 0, 0, 0);
    if (seg)
        c_UnLoadSeg(seg);
}

/* AllocMem found no room: every library not built in and not open is asked to expunge itself (its Expunge vector;
 * an answer is its segment list, unloaded), as AmigaOS does when memory runs short. 1 when any went. */
int zb_lib_flush(void)
{
    int any = 0;
    for (int again = 1; again;) {
        again = 0;
        for (struct znode *n = LIBLIST->head; n->succ; n = n->succ) {
            struct zlib *l = (struct zlib *)n;
            int builtin = 0;
            for (int i = 0; i < 4; i++)
                builtin |= (u32)l == Z.lib[i];
            if (builtin || l->opencnt)
                continue;
            u32 seg = c_RemLibrary((u32)l);
            if (seg) {
                c_UnLoadSeg(seg);
                any = again = 1;
                break;                  /* the list changed under the walk */
            }
        }
    }
    return any;
}

/* ---- signal semaphores (exec/semaphores.h, NDK 3.2: ss_NestCount 14, ss_WaitQueue 16, ss_Owner 40,
 * ss_QueueCount 44). One task: nobody else can hold one, so Obtain never waits; the counts kept as exec keeps them
 * (QueueCount -1 when free) for programs that look. ---- */
#define SS_Nest 14
#define SS_Wait 16
#define SS_Owner 40
#define SS_Queue 44
void c_InitSemaphore(u8 *ss)
{
    struct zlist *l = (struct zlist *)(ss + SS_Wait);
    l->head = (struct znode *)&l->tail;
    l->tail = 0;
    l->tailpred = (struct znode *)&l->head;
    ss[8] = 15;                         /* NT_SIGNALSEM */
    *(s16 *)(ss + SS_Nest) = 0;
    *(u32 *)(ss + SS_Owner) = 0;
    *(s16 *)(ss + SS_Queue) = -1;
}

void c_ObtainSemaphore(u8 *ss)
{
    if (!ss)
        return;
    (*(s16 *)(ss + SS_Nest))++;
    (*(s16 *)(ss + SS_Queue))++;
    *(u32 *)(ss + SS_Owner) = Z.proc;
}

u32 c_AttemptSemaphore(u8 *ss)
{
    c_ObtainSemaphore(ss);
    return ss ? 1 : 0;
}

void c_ReleaseSemaphore(u8 *ss)
{
    if (!ss || *(s16 *)(ss + SS_Nest) <= 0)
        return;
    (*(s16 *)(ss + SS_Nest))--;
    (*(s16 *)(ss + SS_Queue))--;
    if (!*(s16 *)(ss + SS_Nest))
        *(u32 *)(ss + SS_Owner) = 0;
}

/* ---- audio.device (devices/audio.h, NDK 3.2; the RKRM Devices manual): its channels allocated and freed, as
 * programs that play on Paula themselves ask for them first (SID and module players). Nothing is played through it:
 * CMD_WRITE is refused (IOERR_NOCMD) and named in the output; the program's own Paula writes and its level 4 handlers
 * (SetIntVector) play. IOAudio: io_Unit 24 (the channels, a mask), io_Command 28, io_Flags 30, io_Error 31,
 * ioa_AllocKey 32, ioa_Data 34 (the masks wanted), ioa_Length 38 (their count), ioa_Period 42, ioa_Volume 44. ---- */
#define CMD_RESET 1
#define CMD_READ 2
#define CMD_WRITE 3
#define CMD_UPDATE 4
#define CMD_CLEAR 5
#define CMD_STOP 6
#define CMD_START 7
#define CMD_FLUSH 8
#define ADCMD_FREE 9
#define ADCMD_SETPREC 10
#define ADCMD_FINISH 11
#define ADCMD_PERVOL 12
#define ADCMD_LOCK 13
#define ADCMD_WAITCYCLE 14
#define ADCMD_ALLOCATE 32
#define ADIOERR_ALLOCFAILED (-11)
#define IOERR_NOCMD (-3)
#define IOERR_ABORTED (-2)
u32 zb_audio_base;
static u8 aud_used;                     /* the channels given */
static u16 aud_key;
static u8 *aud_lock;                    /* an ADCMD_LOCK in progress: done when its channels are freed */
static u8 aud_said;

void zb_audio_init(u32 base) { zb_audio_base = base; }

int zb_audio_pending(u8 *io) { return io && io == aud_lock; }

/* ADCMD_ALLOCATE: the first mask in ioa_Data whose channels are all free (one program: none is stolen). 0 or the
 * error */
static s32 allocate(u8 *io)
{
    const u8 *masks = *(const u8 **)(io + 34);
    u32 n = *(u32 *)(io + 38);
    if (!aud_key)
        aud_key = 1;
    for (u32 i = 0; masks && i < n && i < 16; i++) {
        u8 m = masks[i] & 15;
        if (m && !(m & aud_used)) {
            aud_used |= m;
            *(u32 *)(io + 24) = m;
            *(u16 *)(io + 32) = aud_key;
            return 0;
        }
    }
    *(u32 *)(io + 24) = 0;
    return ADIOERR_ALLOCFAILED;
}

static void release(u8 *io)
{
    u32 m = *(u32 *)(io + 24) & 15;
    if (*(u16 *)(io + 32) == aud_key)
        aud_used &= (u8)~m;
    CUSTOM(0x096) = (u16)m;             /* DMACON: those channels' DMA off */
    *(u32 *)(io + 24) = 0;
    if (aud_lock && !(*(u32 *)(aud_lock + 24) & aud_used)) {
        u8 *l = aud_lock;
        aud_lock = 0;
        zb_io_done(l);
    }
}

u32 zb_audio_open(u8 *io, u32 unit, u32 flags)
{
    (void)unit, (void)flags;
    if (!io)
        return (u32)-1;
    *(u32 *)(io + ZO_IORequest_io_Device) = zb_audio_base;
    io[31] = 0;
    if (*(u32 *)(io + 38)) {            /* ioa_Length: allocate on open */
        s32 e = allocate(io);
        if (e) {
            io[31] = (u8)e;
            *(u32 *)(io + ZO_IORequest_io_Device) = 0;
            return (u32)e;
        }
    } else
        *(u32 *)(io + 24) = 0;
    return 0;
}

void zb_audio_close(u8 *io)
{
    if (*(u32 *)(io + 24))
        release(io);
}

void zb_audio_beginio(u8 *io)
{
    u16 cmd = *(u16 *)(io + 28);
    u32 m = *(u32 *)(io + 24) & 15;
    io[8] = 5;                          /* NT_MESSAGE: in progress */
    io[31] = 0;
    switch (cmd) {
    case ADCMD_ALLOCATE:
        io[31] = (u8)allocate(io);
        break;
    case ADCMD_FREE:
        release(io);
        break;
    case ADCMD_LOCK:                    /* in progress until the channels are freed */
        aud_lock = io;
        io[30] &= (u8)~1;
        return;
    case ADCMD_PERVOL:
        for (int c = 0; c < 4; c++)
            if (m & 1u << c) {
                CUSTOM(0x0A6 + 0x10 * c) = *(u16 *)(io + 42);
                CUSTOM(0x0A8 + 0x10 * c) = *(u16 *)(io + 44);
            }
        break;
    case CMD_RESET:
    case CMD_FLUSH:
    case CMD_STOP:
        CUSTOM(0x096) = (u16)m;         /* those channels' DMA off */
        break;
    case CMD_START:
    case CMD_UPDATE:
    case CMD_CLEAR:
    case ADCMD_SETPREC:
    case ADCMD_FINISH:
    case ADCMD_WAITCYCLE:
        break;
    case CMD_READ:
        *(u32 *)(io + 34) = 0;          /* no CMD_WRITE is playing */
        break;
    default:                            /* CMD_WRITE: ZINE's audio.device plays nothing */
        io[31] = (u8)IOERR_NOCMD;
        if (!aud_said++)
            say_num("ZINE: audio.device's command ", cmd, cmd == CMD_WRITE ? " (CMD_WRITE): ZINE's audio.device "
                    "only gives channels, it plays nothing; answered with an error\n" : ": not known, answered with an "
                    "error\n");
    }
    zb_io_done(io);
}

void zb_audio_abort(u8 *io)
{
    if (io && io == aud_lock) {
        aud_lock = 0;
        zb_io_done(io);
        io[31] = (u8)IOERR_ABORTED;
    }
}
