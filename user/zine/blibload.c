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
 * ss_QueueCount 44). The counts kept as exec keeps them (QueueCount -1 when free). With more than one task (16.16)
 * Obtain waits while another task holds it: a SemaphoreRequest (a MinNode, sr_Waiter) on the wait queue and
 * SIGF_SINGLE, as exec's; Release hands it to the first waiter. The shared forms are the exclusive ones. ---- */
#define SS_Nest 14
#define SS_Wait 16
#define SS_Owner 40
#define SS_Queue 44
#define SIGF_SINGLE 0x10u
#define THIS_TASK EB(ZO_ExecBase_ThisTask, u32)
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

static int held_by_other(const u8 *ss)
{
    return zt_on && *(const s16 *)(ss + SS_Nest) > 0 && *(const u32 *)(ss + SS_Owner) != THIS_TASK;
}

void c_ObtainSemaphore(u8 *ss)
{
    if (!ss)
        return;
    (*(s16 *)(ss + SS_Queue))++;
    if (held_by_other(ss)) {
        u32 req[3] = { 0, 0, THIS_TASK };      /* the MinNode, sr_Waiter */
        c_SetSignal(0, SIGF_SINGLE);
        c_AddTail((struct zlist *)(ss + SS_Wait), (struct znode *)req);
        while (*(u32 *)(ss + SS_Owner) != THIS_TASK)
            c_Wait(SIGF_SINGLE);
        return;                         /* the releaser made it ours, NestCount 1 */
    }
    (*(s16 *)(ss + SS_Nest))++;
    *(u32 *)(ss + SS_Owner) = THIS_TASK;
}

u32 c_AttemptSemaphore(u8 *ss)
{
    if (!ss || held_by_other(ss))
        return 0;
    c_ObtainSemaphore(ss);
    return 1;
}

void c_ReleaseSemaphore(u8 *ss)
{
    if (!ss || *(s16 *)(ss + SS_Nest) <= 0)
        return;
    (*(s16 *)(ss + SS_Nest))--;
    (*(s16 *)(ss + SS_Queue))--;
    if (*(s16 *)(ss + SS_Nest))
        return;
    u32 *req = (u32 *)c_RemHead((struct zlist *)(ss + SS_Wait));
    *(u32 *)(ss + SS_Owner) = req ? req[2] : 0;
    if (req) {
        *(s16 *)(ss + SS_Nest) = 1;
        c_Signal(req[2], SIGF_SINGLE);
    }
}

/* ---- audio.device (devices/audio.h, NDK 3.2; the RKRM Devices manual and the audio.device autodocs): its
 * channels allocated and freed, and (16.16) CMD_WRITE played through Paula's DMA. IOAudio: io_Unit 24 (the channels,
 * a mask), io_Command 28, io_Flags 30, io_Error 31, ioa_AllocKey 32, ioa_Data 34, ioa_Length 38, ioa_Period 42,
 * ioa_Volume 44, ioa_Cycles 46, ioa_WriteMsg 48. A channel's writes queue on it (the request's own node); its audio
 * interrupt says the buffer in its registers has started, so the next one is loaded behind it (no gap) and the one
 * before is replied. A channel's interrupt is audio.device's only while it has writes there: a program's own level 4
 * handlers (SetIntVector) are called for the other channels as before. ---- */
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
#define ADIOF_PERVOL 0x10
#define ADIOF_WRITEMESSAGE 0x80
#define ADIOERR_NOALLOCATION (-10)
#define ADIOERR_ALLOCFAILED (-11)
#define IOERR_NOCMD (-3)
#define IOERR_ABORTED (-2)
#define FOREVER 0xFFFFFFFFu
u32 zb_audio_base;
u32 zb_aud_done, zb_aud_irqs;           /* writes played to their end, audio interrupts taken (the trace's line) */
static u8 aud_used;                     /* the channels given */
static u16 aud_key;
static u8 *aud_lock;                    /* an ADCMD_LOCK in progress: done when its channels are freed */
static struct ach {
    struct zlist q;                     /* the writes, in order: the first is playing or about to */
    u8 *play;                           /* the write whose cycle is playing (started by an interrupt) */
    u8 *regs;                           /* the write in the registers, to start at the next interrupt; 0: stop then */
    u8 *waitcycle;                      /* an ADCMD_WAITCYCLE, replied when the playing cycle ends */
    u32 left;                           /* cycles of play still to start (FOREVER: ioa_Cycles 0) */
    u8 on, stopped;                     /* the channel's DMA runs; CMD_STOP holds it */
} ach[4];

void zb_audio_init(u32 base)
{
    zb_audio_base = base;
    for (int c = 0; c < 4; c++) {
        struct zlist *l = &ach[c].q;
        l->head = (struct znode *)&l->tail;
        l->tail = 0;
        l->tailpred = (struct znode *)&l->head;
    }
}

int zb_audio_pending(u8 *io)
{
    if (!io)
        return 0;
    if (io == aud_lock)
        return 1;
    for (int c = 0; c < 4; c++) {
        if (ach[c].waitcycle == io)
            return 1;
        for (struct znode *n = ach[c].q.head; n->succ; n = n->succ)
            if ((u8 *)n == io)
                return 1;
    }
    return 0;
}

int zb_audio_busy(void)
{
    for (int c = 0; c < 4; c++)
        if (ach[c].q.head->succ || ach[c].waitcycle)
            return 1;
    return 0;
}

static int lowest(u32 m)
{
    for (int c = 0; c < 4; c++)
        if (m & 1u << c)
            return c;
    return -1;
}

static void load(int c, u8 *io)         /* a write's buffer into channel c's registers */
{
    u32 len = *(u32 *)(io + 38);
    len = len > 131072 ? 131072 : len < 2 ? 2 : len;
    CUSTOML(0x0A0 + 0x10 * c) = *(u32 *)(io + 34);
    CUSTOM(0x0A4 + 0x10 * c) = (u16)(len / 2);       /* words; 65,536 is written as 0 */
    if (io[30] & ADIOF_PERVOL) {
        CUSTOM(0x0A6 + 0x10 * c) = *(u16 *)(io + 42);
        CUSTOM(0x0A8 + 0x10 * c) = *(u16 *)(io + 44);
    }
}

static void off(int c)                  /* channel c silent: its DMA and interrupt off */
{
    CUSTOM(0x096) = (u16)(1u << c);
    CUSTOM(0x09A) = (u16)(0x80u << c);
    CUSTOM(0x09C) = (u16)(0x80u << c);
    ach[c].on = 0;
    ach[c].play = ach[c].regs = 0;
}

static void end_write(int c, u8 *io, s8 err)    /* a write off its queue, replied */
{
    c_Remove((struct znode *)io);
    io[31] = (u8)err;
    zb_io_done(io);
    (void)c;
}

/* the channels in m with writes and not held: their first write loaded, then their DMA on in one write (14-bit
 * pairs start together); the first interrupt of each says it started. Interrupts held off by the caller. */
static void kick(u32 m)
{
    u16 dma = 0;
    for (int c = 0; c < 4; c++) {
        struct ach *a = &ach[c];
        u8 *io = (u8 *)a->q.head;
        if (!(m & 1u << c) || a->on || a->stopped || !((struct znode *)io)->succ)
            continue;
        load(c, io);
        a->regs = io;
        a->play = 0;
        a->on = 1;
        CUSTOM(0x09C) = (u16)(0x80u << c);
        CUSTOM(0x09A) = (u16)(0x8000u | 0x80u << c);
        dma |= (u16)(1u << c);
    }
    if (dma)
        CUSTOM(0x096) = (u16)(0x8200u | dma);
}

/* what goes into channel c's registers behind the cycle that has just started */
static void behind(int c)
{
    struct ach *a = &ach[c];
    if (a->play && (a->left == FOREVER || a->left > 0)) {
        a->regs = a->play;              /* the same buffer again: Paula repeats it by itself */
        return;
    }
    struct znode *nx = a->play ? ((struct znode *)a->play)->succ : a->q.head;
    if (nx && nx->succ) {
        load(c, (u8 *)nx);
        a->regs = (u8 *)nx;
    } else
        a->regs = 0;
}

/* from zb_irq, level 4: the bits of channels whose writes are audio.device's taken and answered */
u32 zb_audio_irq(u32 pend)
{
    u32 mine = 0;
    for (int c = 0; c < 4; c++) {
        struct ach *a = &ach[c];
        u16 bit = (u16)(0x80u << c);
        if (!(pend & bit) || !a->on)
            continue;
        mine |= bit;
        CUSTOM(0x09C) = bit;
        CUSTOM(0x09C) = bit;
        zb_aud_irqs++;
        if (a->waitcycle) {
            u8 *w = a->waitcycle;
            a->waitcycle = 0;
            zb_io_done(w);
        }
        u8 *r = a->regs;
        if (r && r == a->play) {        /* the playing write's next cycle */
            if (a->left != FOREVER)
                a->left--;
        } else {
            if (a->play) {              /* the one before has played to its end */
                zb_aud_done++;
                end_write(c, a->play, 0);
            }
            a->play = r;
            if (!r) {
                off(c);
                continue;
            }
            u16 cy = *(u16 *)(r + 46);
            a->left = cy ? cy - 1u : FOREVER;
            if (r[30] & ADIOF_WRITEMESSAGE) {       /* ioa_WriteMsg replied as the write starts */
                u8 *wm = r + 48, *rp = *(u8 **)(wm + 14);
                if (rp && c_TypeOfMem((u32)rp))
                    zb_port_put(rp, (struct znode *)wm, 7);
            }
        }
        behind(c);
    }
    return mine;
}

/* every write of the channels in m ended (aborted), their DMA off */
static void flush(u32 m)
{
    u16 s = zt_dis();
    for (int c = 0; c < 4; c++)
        if (m & 1u << c) {
            off(c);
            struct znode *n;
            while ((n = ach[c].q.head)->succ)
                end_write(c, (u8 *)n, IOERR_ABORTED);
            if (ach[c].waitcycle) {
                u8 *w = ach[c].waitcycle;
                ach[c].waitcycle = 0;
                w[31] = (u8)IOERR_ABORTED;
                zb_io_done(w);
            }
        }
    zt_en(s);
}

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
    flush(m);                           /* the channels' writes end with them (their DMA off) */
    if (*(u16 *)(io + 32) == aud_key)
        aud_used &= (u8)~m;
    for (int c = 0; c < 4; c++)
        if (m & 1u << c)
            ach[c].stopped = 0;
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
    u16 s;
    switch (cmd) {
    case CMD_WRITE: {
        int c = lowest(m);
        if (c < 0 || !(aud_used & 1u << c) || *(u16 *)(io + 32) != aud_key) {
            io[31] = (u8)ADIOERR_NOALLOCATION;    /* not this request's channel (the autodoc's key check) */
            break;
        }
        *(u32 *)(io + 24) = 1u << c;
        io[30] &= (u8)~1;               /* asynchronous: replied when it has played */
        s = zt_dis();
        struct ach *a = &ach[c];
        c_AddTail(&a->q, (struct znode *)io);
        if (!a->on)
            kick(1u << c);
        else if (!a->regs)              /* the last cycle playing, nothing behind it: this one goes behind */
            behind(c);
        zt_en(s);
        return;
    }
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
    case CMD_FLUSH:
        flush(m);
        break;
    case CMD_RESET:
        flush(m);
        for (int c = 0; c < 4; c++)
            if (m & 1u << c)
                ach[c].stopped = 0;
        break;
    case CMD_STOP:                      /* held: the playing write starts again from its beginning at CMD_START */
        s = zt_dis();
        for (int c = 0; c < 4; c++)
            if (m & 1u << c) {
                off(c);
                ach[c].stopped = 1;
            }
        zt_en(s);
        break;
    case CMD_START:
        s = zt_dis();
        for (int c = 0; c < 4; c++)
            if (m & 1u << c)
                ach[c].stopped = 0;
        kick(m);
        zt_en(s);
        break;
    case ADCMD_FINISH:                  /* the playing write ends after its current cycle */
        s = zt_dis();
        for (int c = 0; c < 4; c++)
            if ((m & 1u << c) && ach[c].play) {
                ach[c].left = 0;
                behind(c);
            }
        zt_en(s);
        break;
    case ADCMD_WAITCYCLE: {
        int c = lowest(m);
        if (c >= 0 && ach[c].on && !ach[c].waitcycle) {
            io[30] &= (u8)~1;
            ach[c].waitcycle = io;
            return;
        }
        break;
    }
    case CMD_UPDATE:
    case CMD_CLEAR:
    case ADCMD_SETPREC:
        break;
    case CMD_READ: {                    /* the write playing on the channel, or 0 */
        int c = lowest(m);
        *(u32 *)(io + 34) = c >= 0 ? (u32)ach[c].play : 0;
        break;
    }
    default:
        io[31] = (u8)IOERR_NOCMD;
    }
    zb_io_done(io);
}

void zb_audio_abort(u8 *io)
{
    if (!io)
        return;
    if (io == aud_lock) {
        aud_lock = 0;
        io[31] = (u8)IOERR_ABORTED;
        zb_io_done(io);
        return;
    }
    u16 s = zt_dis();
    for (int c = 0; c < 4; c++) {
        struct ach *a = &ach[c];
        if (a->waitcycle == io) {
            a->waitcycle = 0;
            io[31] = (u8)IOERR_ABORTED;
            zb_io_done(io);
        }
        for (struct znode *n = a->q.head; n->succ; n = n->succ)
            if ((u8 *)n == io) {
                int live = io == a->play || io == a->regs;
                if (live)
                    off(c);
                end_write(c, io, IOERR_ABORTED);
                if (live)
                    kick(1u << c);
                break;
            }
    }
    zt_en(s);
}

/* ---- console.device (devices/console.h and conunit.h, NDK 3.2; the RKRM Devices manual, d29 16.14): a unit bound
 * to a window as old programs open it (The Juggler) for text and keys. ZINE draws no console text: CMD_WRITE's text
 * goes into the program's output (zine shows it), the control sequences left out; ZINE has no keyboard for programs,
 * so a CMD_READ stays in progress until AbortIO. io_Unit is a ConUnit of ZINE's (its window and an 8 x 8 grid, as a
 * program may read cu_XMax and cu_YMax); CONU_LIBRARY (-1) gives only io_Device, for RawKeyConvert. IOStdReq:
 * io_Unit 24, io_Command 28, io_Flags 30, io_Error 31, io_Actual 32, io_Length 36, io_Data 40. ConUnit: cu_Window 34,
 * cu_XMax 42, cu_YMax 44, cu_XRSize 46, cu_YRSize 48, cu_TabStops 98 (80 words), cu_FgPen 259, cu_TxHeight 282,
 * cu_TxWidth 284, cu_TxBaseline 286. ---- */
#define CON_UNITS 4
#define CU_SIZE 512                     /* more than struct ConUnit's 296 bytes */
u32 zb_con_base;
static u8 *con_read[CON_UNITS];         /* a CMD_READ in progress per unit (until AbortIO) */
static u8 con_said;

void zb_con_init(u32 base) { zb_con_base = base; }

int zb_con_pending(u8 *io)
{
    for (int i = 0; io && i < CON_UNITS; i++)
        if (con_read[i] == io)
            return 1;
    return 0;
}

u32 zb_con_open(u8 *io, u32 unit, u32 flags)
{
    (void)flags;
    if (!io)
        return (u32)-1;
    *(u32 *)(io + ZO_IORequest_io_Device) = zb_con_base;
    *(u32 *)(io + 24) = 0;
    io[31] = 0;
    if ((s32)unit == -1)                /* CONU_LIBRARY */
        return 0;
    u8 *cu = (u8 *)zx_AllocMem(CU_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (!cu) {
        *(u32 *)(io + ZO_IORequest_io_Device) = 0;
        io[31] = 0xFF;                  /* IOERR_OPENFAIL */
        return (u32)-1;
    }
    u8 *win = *(u8 **)(io + 40);
    u16 w = win ? *(u16 *)(win + 8) : 640, h = win ? *(u16 *)(win + 10) : 200;
    *(u8 **)(cu + 34) = win;
    *(u16 *)(cu + 42) = (u16)(w >= 16 ? w / 8 - 1 : 0);
    *(u16 *)(cu + 44) = (u16)(h >= 16 ? h / 8 - 1 : 0);
    *(u16 *)(cu + 46) = 8;
    *(u16 *)(cu + 48) = 8;
    for (int i = 0; i < 80; i++)
        *(u16 *)(cu + 98 + 2 * i) = i < 79 ? (u16)(8 * i) : 0xFFFF;
    cu[259] = 1;
    *(u16 *)(cu + 282) = 8;
    *(u16 *)(cu + 284) = 8;
    *(u16 *)(cu + 286) = 6;
    *(u8 **)(io + 24) = cu;
    return 0;
}

static int con_slot(const u8 *io)
{
    for (int i = 0; i < CON_UNITS; i++)
        if (con_read[i] == io)
            return i;
    return -1;
}

void zb_con_abort(u8 *io)
{
    int i = con_slot(io);
    if (io && i >= 0) {
        con_read[i] = 0;
        *(u32 *)(io + 32) = 0;
        io[31] = (u8)IOERR_ABORTED;
        zb_io_done(io);
    }
}

void zb_con_close(u8 *io)
{
    zb_con_abort(io);
    u8 *cu = *(u8 **)(io + 24);
    if (cu)
        zx_FreeMem((u32)cu, CU_SIZE);
    *(u32 *)(io + 24) = 0;
}

/* CMD_WRITE's text into the output: printable bytes, tabs and line ends; ESC and CSI sequences left out (their
 * parameters up to the final byte, 0x40-0x7E) */
static void con_text(const u8 *s, u32 n)
{
    static u8 seq;                      /* 1: after ESC, 2: in a CSI sequence (it may go on in the next write) */
    char buf[64];
    u32 k = 0;
    for (u32 i = 0; i < n; i++) {
        u8 c = s[i];
        if (seq == 1)
            seq = c == '[' ? 2 : 0;
        else if (seq == 2) {
            if (c >= 0x40 && c <= 0x7E)
                seq = 0;
        } else if (c == 0x1B)
            seq = 1;
        else if (c == 0x9B)
            seq = 2;
        else if (c == '\n' || c == '\t' || (c >= 0x20 && c != 0x7F && (c < 0x80 || c >= 0xA0)))
            buf[k++] = (char)c;
        if (k == sizeof buf || (i + 1 == n && k)) {
            zb_out(buf, k);
            k = 0;
        }
    }
}

void zb_con_beginio(u8 *io)
{
    u16 cmd = *(u16 *)(io + 28);
    io[8] = 5;                          /* NT_MESSAGE: in progress */
    io[31] = 0;
    *(u32 *)(io + 32) = 0;
    switch (cmd) {
    case CMD_WRITE: {
        const u8 *s = *(const u8 **)(io + 40);
        u32 n = *(u32 *)(io + 36);
        if (n == 0xFFFFFFFFu)           /* -1: up to its 0 byte */
            for (n = 0; s && s[n] && n < 65536; n++)
                ;
        if (s && n)
            con_text(s, n);
        *(u32 *)(io + 32) = s ? n : 0;
        break;
    }
    case CMD_READ: {                    /* no keys for programs: in progress until AbortIO */
        int i = con_slot(0);
        if (i >= 0) {
            con_read[i] = io;
            io[30] &= (u8)~1;           /* not quick */
            if (!con_said++)
                say("ZINE: console.device's CMD_READ waits for keys, which ZINE does not give programs", "", "\n");
            return;
        }
        break;                          /* more than four at once: done with no bytes */
    }
    case CMD_CLEAR:
    case CMD_RESET:
    case CMD_FLUSH:
    case CMD_START:
    case CMD_STOP:
    case CMD_UPDATE:
    case 9:                             /* CD_ASKKEYMAP, CD_SETKEYMAP, the default ones: taken, nothing changes */
    case 10:
    case 11:
    case 12:
        break;
    default:
        io[31] = (u8)IOERR_NOCMD;
    }
    zb_io_done(io);
}

/* input.device (devices/input.h, NDK 3.2; 16.14): opened by programs that add an input handler to see the keys and
 * the mouse first (TBL's demo system). ZINE gives programs no input events, so a handler is taken and never called;
 * every command (IND_ADDHANDLER 9, IND_REMHANDLER 10, IND_WRITEEVENT 11, the thresholds and the mouse port's ones)
 * is done at once. */
u32 zb_inp_base;
void zb_inp_init(u32 base) { zb_inp_base = base; }
u32 zb_inp_open(u8 *io)
{
    if (!io)
        return (u32)-1;
    *(u32 *)(io + ZO_IORequest_io_Device) = zb_inp_base;
    *(u32 *)(io + 24) = 0;
    io[31] = 0;
    return 0;
}
void zb_inp_beginio(u8 *io)
{
    io[8] = 5;
    io[31] = 0;
    *(u32 *)(io + 32) = 0;
    zb_io_done(io);
}

/* RawKeyConvert (-48): ZINE gives programs no keys, so no characters (0) */
u32 zb_con_rawkey(void) { return 0; }
