/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's blob: the small AmigaOS's libraries (DESIGN D29 section 5; the first set of build step 3): ExecBase and the
 * other bases with their jump tables, the memory lists, the process and its CLI, and the functions beyond memory
 * (blob.c). Every vector not listed here is "jsr zx_unsup": the program stops cleanly and `zine` names the call. */
#include "blob.h"

/* ---- lists (exec/lists.h) ---- */
static void newlist(struct zlist *l, u8 type)
{
    l->head = (struct znode *)&l->tail;
    l->tail = 0;
    l->tailpred = (struct znode *)&l->head;
    l->type = type;
}

void c_AddHead(struct zlist *l, struct znode *n)
{
    n->succ = l->head;
    n->pred = (struct znode *)&l->head;
    l->head->pred = n;
    l->head = n;
}

void c_AddTail(struct zlist *l, struct znode *n)
{
    n->succ = (struct znode *)&l->tail;
    n->pred = l->tailpred;
    l->tailpred->succ = n;
    l->tailpred = n;
}

void c_Insert(struct zlist *l, struct znode *n, struct znode *after)
{
    if (!after) {
        c_AddHead(l, n);
        return;
    }
    n->succ = after->succ;
    n->pred = after;
    after->succ->pred = n;
    after->succ = n;
}

void c_Remove(struct znode *n)
{
    n->pred->succ = n->succ;
    n->succ->pred = n->pred;
}

u32 c_RemHead(struct zlist *l)
{
    struct znode *n = l->head;
    if (!n->succ)
        return 0;
    c_Remove(n);
    return (u32)n;
}

u32 c_RemTail(struct zlist *l)
{
    struct znode *n = l->tailpred;
    if (!n->pred)
        return 0;
    c_Remove(n);
    return (u32)n;
}

void c_Enqueue(struct zlist *l, struct znode *n)
{
    struct znode *s = l->head;
    while (s->succ && s->pri >= n->pri)
        s = s->succ;
    n->succ = s;
    n->pred = s->pred;
    s->pred->succ = n;
    s->pred = n;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

u32 c_FindName(struct zlist *l, const char *name)
{
    struct znode *n = (struct znode *)l;     /* from a list, or from a node: the search starts after it */
    for (n = n->succ; n && n->succ; n = n->succ)
        if (n->name && streq(n->name, name))
            return (u32)n;
    return 0;
}

/* ---- the task (one process) and its signals ---- */
#define TASK(o, t) (*(volatile t *)(Z.proc + ZO_Task_##o))

u32 c_FindTask(const char *name)
{
    return !name || streq(name, (const char *)TASK(tc_Node + ZO_Node_ln_Name, u32)) ? Z.proc : 0;
}

u32 c_SetTaskPri(u32 task, u32 pri)
{
    (void)task;
    (void)pri;
    return 0;
}

u32 c_SetSignal(u32 sigs, u32 mask)
{
    u32 old = TASK(tc_SigRecvd, u32);
    TASK(tc_SigRecvd, u32) = (old & ~mask) | (sigs & mask);
    return old;
}

void c_Signal(u32 task, u32 sigs)
{
    if (task == Z.proc)
        TASK(tc_SigRecvd, u32) |= sigs;
}

/* Wait: one process, so only an interrupt server can send the signal; with interrupts disabled nothing can (5.2) */
u32 c_Wait(u32 sigs)
{
    for (;;) {
        u32 got = TASK(tc_SigRecvd, u32) & sigs;
        if (got) {
            TASK(tc_SigRecvd, u32) &= ~got;
            return got;
        }
        if (!sigs || EB(ZO_ExecBase_IDNestCnt, s8) >= 0)
            zb_stop(ZH_STOP, ZS_WAIT);
    }
}

u32 c_AllocSignal(u32 n)
{
    s32 want = (s8)n;
    u32 alloc = TASK(tc_SigAlloc, u32);
    for (s32 i = want < 0 ? 31 : want; i >= (want < 0 ? 16 : want); i--)
        if (!(alloc & (1u << i))) {
            TASK(tc_SigAlloc, u32) = alloc | 1u << i;
            TASK(tc_SigRecvd, u32) &= ~(1u << i);
            return (u32)i;
        }
    return (u32)-1;
}

void c_FreeSignal(u32 n)
{
    if ((s32)n >= 0 && n < 32)
        TASK(tc_SigAlloc, u32) &= ~(1u << n);
}

/* ---- interrupts (8.3) ---- */
extern void zx_chain(void);
#define IV(n) ((struct ziv *)(ZEXEC + ZO_ExecBase_IntVects + 12 * (n)))

u32 c_SetIntVector(u32 n, struct zint *is)
{
    if (n > 15)
        return 0;
    struct ziv *iv = IV(n);
    u32 old = (u32)iv->node;
    iv->node = (struct znode *)is;
    iv->code = is ? is->code : 0;
    iv->data = is ? is->data : 0;
    return old;
}

static struct zlist chains[16];
void c_AddIntServer(u32 n, struct zint *is)
{
    if (n < 16 && chains[n].tailpred) {
        u16 s = INTENAR & 0x4000;
        INTENA = 0x4000;
        c_Enqueue(&chains[n], &is->node);
        if (s)
            INTENA = 0xC000;
    }
}

void c_RemIntServer(u32 n, struct zint *is)
{
    if (n < 16 && chains[n].tailpred) {
        u16 s = INTENAR & 0x4000;
        INTENA = 0x4000;
        c_Remove(&is->node);
        if (s)
            INTENA = 0xC000;
    }
}

/* ---- the rest of exec ---- */
void c_Alert(u32 code)
{
    if (code & AT_DeadEnd) {
        Z.blk->addr = code;
        zb_stop(ZH_ALERT, 0);
    }
    static const char hx[] = "0123456789ABCDEF";
    char t[] = "ZINE: a recoverable alert 00000000\n";
    for (int i = 0; i < 8; i++)
        t[26 + i] = hx[(code >> (28 - 4 * i)) & 15];
    zb_out(t, sizeof t - 1);
}

static const char *const libname[4] = { "exec.library", "graphics.library", "dos.library", "intuition.library" };

u32 c_OpenLibrary(const char *name, u32 version)
{
    for (int i = 0; i < 4; i++)
        if (name && streq(name, libname[i])) {
            struct zlib *l = (struct zlib *)Z.lib[i];
            if (version > l->version)
                return 0;
            l->opencnt++;
            return (u32)l;
        }
    return 0;                           /* a library ZINE has not: 0, as AmigaOS answers when it has none */
}

void c_CloseLibrary(struct zlib *l)
{
    if (l && l->opencnt)
        l->opencnt--;
}

u32 c_OpenResource(const char *name)
{
    (void)name;
    return 0;                           /* the resources come with build step 4 */
}

/* no devices in the first version (5.5): the request's io_Error and the answer are IOERR_OPENFAIL */
u32 c_OpenDevice(const char *name, u32 unit, u8 *io, u32 flags)
{
    (void)name, (void)unit, (void)flags;
    if (io) {
        *(u32 *)(io + ZO_IORequest_io_Device) = 0;
        io[ZO_IORequest_io_Error] = 0xFF;
    }
    return (u32)-1;
}

void c_CloseDevice(u8 *io)
{
    (void)io;
}

u32 c_SetFunction(u8 *lib, s32 off, u32 fn)
{
    u8 *slot = lib + (s16)off;
    u32 old = *(u32 *)(slot + 2);
    *(u16 *)slot = 0x4EF9;
    *(u32 *)(slot + 2) = fn;
    extern void zx_CacheClearU(void);
    zx_CacheClearU();
    return old;
}

/* the instruction cache stays as Zenix had it, the data cache off (10.1): the answer says so; changes are not made */
u32 c_CacheControl(u32 bits, u32 mask)
{
    (void)bits, (void)mask;
    extern void zx_CacheClearU(void);
    zx_CacheClearU();
    return 1;                           /* CACRF_EnableI */
}

/* RawDoFmt (exec): %[-][0][width][.limit][l](d u x X s b c), %% ; without l a number is a 16-bit word */
typedef void (*zput)(void *u, u32 c);
static const u8 *fmt(const char *f, const u8 *d, zput put, void *u)
{
    for (; *f; f++) {
        if (*f != '%') {
            put(u, (u8)*f);
            continue;
        }
        f++;
        int left = 0, zero = 0, lng = 0;
        u32 width = 0, limit = 0xFFFFFFFFu;
        if (*f == '-')
            left = 1, f++;
        if (*f == '0')
            zero = 1, f++;
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (u32)(*f++ - '0');
        if (*f == '.') {
            limit = 0;
            for (f++; *f >= '0' && *f <= '9'; f++)
                limit = limit * 10 + (u32)(*f - '0');
        }
        if (*f == 'l')
            lng = 1, f++;
        char buf[12], *s = buf + sizeof buf, *str = 0;
        u32 len = 0, v = 0;
        switch (*f) {
        case 'd': case 'D': case 'u': case 'U': case 'x': case 'X': case 'c':
            if (lng) {
                v = (u32)d[0] << 24 | (u32)d[1] << 16 | (u32)d[2] << 8 | d[3];
                d += 4;
            } else {
                v = (u32)d[0] << 8 | d[1];
                if (*f == 'd' || *f == 'D')
                    v = (u32)(s32)(int16_t)v;
                d += 2;
            }
            break;
        case 's': case 'b':
            v = (u32)d[0] << 24 | (u32)d[1] << 16 | (u32)d[2] << 8 | d[3];
            d += 4;
            break;
        case 0:
            return d;
        default:
            put(u, (u8)*f);
            continue;
        }
        int neg = 0;
        switch (*f) {
        case 'd': case 'D':
            if ((s32)v < 0)
                neg = 1, v = -v;
            /* fall through */
        case 'u': case 'U':
            do
                *--s = (char)('0' + v % 10);
            while (v /= 10);
            if (neg)
                *--s = '-';
            str = s, len = (u32)(buf + sizeof buf - s);
            break;
        case 'x': case 'X':
            do
                *--s = "0123456789ABCDEF"[v & 15] | (*f == 'x' ? 0x20 : 0);
            while (v >>= 4);
            str = s, len = (u32)(buf + sizeof buf - s);
            break;
        case 'c':
            buf[0] = (char)v, str = buf, len = 1;
            break;
        case 's':
            str = (char *)v;
            for (len = 0; str && str[len] && len < limit; len++)
                ;
            break;
        case 'b':
            str = v ? (char *)(v << 2) + 1 : 0;
            len = v ? *(u8 *)(v << 2) : 0;
            if (len > limit)
                len = limit;
            break;
        }
        for (u32 i = len; !left && i < width; i++)
            put(u, zero ? '0' : ' ');
        for (u32 i = 0; i < len; i++)
            put(u, (u8)str[i]);
        for (u32 i = len; left && i < width; i++)
            put(u, ' ');
    }
    return d;
}

struct rawput { u32 proc, a3; };
static void raw_put(void *u, u32 c)
{
    struct rawput *r = u;
    if (!r->proc)
        return;
    if (*(const u32 *)r->proc == 0x16C04E75u) {     /* the usual "move.b d0,(a3)+; rts": done here */
        *(u8 *)r->a3 = (u8)c;
        r->a3++;
    } else
        zb_putch(r->proc, &r->a3, c);
}

u32 c_RawDoFmt(const char *f, const u8 *data, u32 proc, u32 a3)
{
    struct rawput r = { proc, a3 };
    const u8 *e = fmt(f, data, raw_put, &r);
    raw_put(&r, 0);
    return (u32)e;
}

/* ---- dos (5.4, the first set): the output into the ring `zine` shows after the session ---- */
u32 zb_out(const char *s, u32 n)
{
    struct zine_blk *b = Z.blk;
    u32 k = n < ZB_OUTMAX - b->outlen ? n : ZB_OUTMAX - b->outlen;
    zmemcpy(b->out + b->outlen, s, k);
    b->outlen += k;
    b->outlost += n - k;
    return n;
}

static void out_put(void *u, u32 c)
{
    char ch = (char)c;
    (void)u;
    zb_out(&ch, 1);
}

u32 c_Output(void) { return Z.out_fh; }
u32 c_Input(void) { return Z.in_fh; }
u32 c_IoErr(void) { return Z.ioerr; }

u32 c_SetIoErr(u32 e)
{
    u32 o = Z.ioerr;
    Z.ioerr = e;
    return o;
}

u32 c_Write(u32 fh, const char *buf, s32 n)
{
    if (fh != Z.out_fh || n < 0) {
        Z.ioerr = 114;                  /* ERROR_OBJECT_WRONG_TYPE: not a handle ZINE gave */
        return (u32)-1;
    }
    return zb_out(buf, (u32)n);
}

u32 c_Read(u32 fh, char *buf, s32 n)
{
    (void)buf, (void)n;
    if (fh != Z.in_fh) {
        Z.ioerr = 114;
        return (u32)-1;
    }
    return 0;                           /* the input is empty: end of file */
}

static u32 slen(const char *s)
{
    u32 n = 0;
    while (s && s[n])
        n++;
    return n;
}

u32 c_PutStr(const char *s)
{
    zb_out(s, slen(s));
    return 0;
}

u32 c_FPuts(u32 fh, const char *s)
{
    return c_Write(fh, s, (s32)slen(s)) == (u32)-1 ? (u32)-1 : 0;
}

u32 c_FPutC(u32 fh, u32 c)
{
    char ch = (char)c;
    return c_Write(fh, &ch, 1) == (u32)-1 ? (u32)-1 : (c & 0xFF);
}

u32 c_VPrintf(const char *f, const u8 *argv)
{
    u32 before = Z.blk->outlen + Z.blk->outlost;
    fmt(f, argv, out_put, 0);
    return Z.blk->outlen + Z.blk->outlost - before;
}

u32 c_VFPrintf(u32 fh, const char *f, const u8 *argv)
{
    if (fh != Z.out_fh) {
        Z.ioerr = 114;
        return (u32)-1;
    }
    return c_VPrintf(f, argv);
}

u32 c_Flush(u32 fh) { (void)fh; return 1; }

/* Delay: 50 ticks a second, counted in beam lines (313 a tick: 15,625 lines a second on PAL, 15,734 on NTSC) */
void c_Delay(u32 ticks)
{
    zb_wait_lines(ticks * 313);
}

/* ---- graphics (5.3, the first set) ---- */
#define IB_VIEWLORD(ib) ((ib) + ZO_IntuitionBase_ViewLord)
void c_WaitBlit(void) { zb_waitblit(); }

void c_OwnBlitter(void)
{
    zb_waitblit();
    Z.blitter = 1;
}

void c_DisownBlitter(void) { Z.blitter = 0; }

u32 c_VBeamPos(void)
{
    return ((u32)(VPOSR & 1) << 8) | (VHPOSR >> 8);
}

/* the next vertical blank, by the beam (works with interrupts off): the line number goes down; 100 ms at most */
void c_WaitTOF(void)
{
    u32 last = c_VBeamPos(), seen = 0;
    for (u32 it = 0; it < 4000000u && seen < 1600; it++) {
        u32 l = c_VBeamPos();
        if (l < last)
            return;
        if (l != last)
            seen++;
        last = l;
    }
}

void c_LoadView(u32 view)
{
    if (view && view != IB_VIEWLORD(Z.lib[3]))
        zb_stop(ZH_STOP, ZS_VIEW);      /* a View of graphics' own: not in the first version */
    *(u32 *)(Z.lib[1] + ZO_GfxBase_ActiView) = view;
    zb_blank_view();
}

void c_SetRGB4(u32 n, u32 r, u32 g, u32 bl)
{
    if (n < 32)
        COLOR(n) = (u16)((r & 15) << 8 | (g & 15) << 4 | (bl & 15));
}

void c_LoadRGB4(const u16 *c, u32 n)
{
    for (u32 i = 0; i < n && i < 32; i++)
        COLOR(i) = c[i];
}

/* ---- intuition: what take-over start-up code calls ---- */
u32 c_CloseWorkBench(void) { return 1; }    /* yes on purpose (5.5): demos take no for "cannot close it" */
u32 c_OpenWorkBench(void) { return 0; }

/* ---- the tables ---- */
#define F(x) extern void zs_##x(void)
#define X(x) extern void zx_##x(void)
F(Alert); F(SetIntVector); F(AddIntServer); F(RemIntServer); F(AllocMem); F(AllocAbs); F(FreeMem); F(AvailMem);
F(Insert); F(AddHead); F(AddTail); F(Remove); F(RemHead); F(RemTail); F(Enqueue); F(FindName); F(FindTask);
F(SetTaskPri); F(SetSignal); F(Wait); F(Signal); F(AllocSignal); F(FreeSignal); F(OldOpenLibrary); F(OpenLibrary);
F(CloseLibrary); F(OpenResource); F(OpenDevice); F(CloseDevice); F(SetFunction); F(RawDoFmt); F(TypeOfMem); F(CopyMem);
F(CacheControl); F(AllocVec); F(FreeVec); F(CreatePool); F(DeletePool); F(AllocPooled); F(FreePooled);
F(LoadView); F(WaitTOF); F(WaitBlit); F(OwnBlitter); F(DisownBlitter); F(VBeamPos); F(SetRGB4); F(LoadRGB4);
F(Output); F(Input); F(Write); F(Read); F(PutStr); F(VPrintf); F(VFPrintf); F(FPuts); F(FPutC); F(Flush); F(IoErr);
F(SetIoErr); F(Delay); F(CloseWorkBench); F(OpenWorkBench); F(rts);
X(Supervisor); X(Disable); X(Enable); X(Forbid); X(Permit); X(SetSR); X(GetCC); X(CacheClearU); X(ColdReboot); X(Exit);

/* the offsets: the RKRM's Function Offset Reference (V37), the pools and ColdReboot from the NDK 3.2's exec_lib.fd */
static const u16 exec_lvo[] = { 30, 108, 114, 120, 126, 132, 138, 144, 162, 168, 174, 198, 204, 210, 216, 234, 240,
    246, 252, 258, 264, 270, 276, 294, 300, 306, 318, 324, 330, 336, 408, 414, 420, 444, 450, 498, 522, 528, 534, 552,
    624, 630, 636, 642, 648, 684, 690, 696, 702, 708, 714, 726 };
static void (*const exec_fn[])(void) = { zx_Supervisor, zs_Alert, zs_rts, zx_Disable, zx_Enable, zx_Forbid,
    zx_Permit, zx_SetSR, zs_SetIntVector, zs_AddIntServer, zs_RemIntServer, zs_AllocMem, zs_AllocAbs, zs_FreeMem,
    zs_AvailMem, zs_Insert, zs_AddHead, zs_AddTail, zs_Remove, zs_RemHead, zs_RemTail, zs_Enqueue, zs_FindName,
    zs_FindTask, zs_SetTaskPri, zs_SetSignal, zs_Wait, zs_Signal, zs_AllocSignal, zs_FreeSignal, zs_OldOpenLibrary,
    zs_CloseLibrary, zs_SetFunction, zs_OpenDevice, zs_CloseDevice, zs_OpenResource, zs_RawDoFmt, zx_GetCC,
    zs_TypeOfMem, zs_OpenLibrary, zs_CopyMem, zs_CopyMem, zx_CacheClearU, zx_CacheClearU, zs_CacheControl,
    zs_AllocVec, zs_FreeVec, zs_CreatePool, zs_DeletePool, zs_AllocPooled, zs_FreePooled, zx_ColdReboot };
static const u16 gfx_lvo[] = { 192, 222, 228, 270, 288, 384, 456, 462 };
static void (*const gfx_fn[])(void) = { zs_LoadRGB4, zs_LoadView, zs_WaitBlit, zs_WaitTOF, zs_SetRGB4, zs_VBeamPos,
    zs_OwnBlitter, zs_DisownBlitter };
static const u16 dos_lvo[] = { 42, 48, 54, 60, 132, 144, 198, 312, 342, 354, 360, 462, 948, 954 };
static void (*const dos_fn[])(void) = { zs_Read, zs_Write, zs_Input, zs_Output, zs_IoErr, zx_Exit, zs_Delay,
    zs_FPutC, zs_FPuts, zs_VFPrintf, zs_Flush, zs_SetIoErr, zs_PutStr, zs_VPrintf };
static const u16 int_lvo[] = { 78, 210 };
static void (*const int_fn[])(void) = { zs_CloseWorkBench, zs_OpenWorkBench };
_Static_assert(sizeof exec_lvo / 2 == sizeof exec_fn / sizeof exec_fn[0], "exec table");
_Static_assert(sizeof gfx_lvo / 2 == sizeof gfx_fn / sizeof gfx_fn[0], "graphics table");
_Static_assert(sizeof dos_lvo / 2 == sizeof dos_fn / sizeof dos_fn[0], "dos table");
_Static_assert(sizeof int_lvo / 2 == sizeof int_fn / sizeof int_fn[0], "intuition table");

static const u16 slots[4] = { ZEXEC_SLOTS, 176, 193, 202 };   /* the NDK 3.2's .fd files: graphics, dos, intuition */
static const u16 possize[4] = { 632, 1024, 256, 256 };

static u32 area;                        /* the next free byte of the area (the ZINE block) */
static u32 take(u32 n)
{
    u32 a = (area + 3) & ~3u;
    area = a + n;
    zmemset((void *)a, 0, n);
    return a;
}

static const char idstr[] = "ZINE 40.1 (Zenix's small AmigaOS)\r\n";

static void lib_init(int i, u32 base)
{
    struct zlib *l = (struct zlib *)base;
    l->node.type = NT_LIBRARY;
    l->node.name = (char *)libname[i];
    l->negsize = (u16)(slots[i] * 6);
    l->possize = possize[i];
    l->version = 40;
    l->revision = 1;
    l->idstring = (char *)idstr;
    l->opencnt = 1;
    Z.lib[i] = base;
}

static struct zint chainint[16];
static const char chainname[] = "ZINE chain";

/* a BSTR, longword aligned (dos) */
static u32 bstr(const char *s)
{
    u32 n = slen(s);
    if (n > 255)
        n = 255;
    u8 *b = (u8 *)take(n + 2);
    b[0] = (u8)n;
    zmemcpy(b + 1, s, n);
    return (u32)b >> 2;
}

void zb_libs_init(void)
{
    struct zine_blk *b = Z.blk;
    area = Z.lib[0];                    /* zb_prepare left the area's start there */
    /* the libraries other than exec: in the ZINE block (fast RAM) */
    static void (*const *const fns[4])(void) = { exec_fn, gfx_fn, dos_fn, int_fn };
    static const u16 *const lvos[4] = { exec_lvo, gfx_lvo, dos_lvo, int_lvo };
    static const u16 nfn[4] = { sizeof exec_lvo / 2, sizeof gfx_lvo / 2, sizeof dos_lvo / 2, sizeof int_lvo / 2 };
    for (int i = 0; i < 4; i++) {
        u32 base = i ? take(slots[i] * 6u + possize[i]) + slots[i] * 6u : ZEXEC;
        if (!i)
            zmemset((void *)(ZEXEC - ZEXEC_SLOTS * 6), 0, ZEXEC_SLOTS * 6 + possize[0]);
        zb_jumptable(base, slots[i], lvos[i], fns[i], nfn[i]);
        lib_init(i, base);
    }
    /* ExecBase (5.1) */
    EB(ZO_ExecBase_SoftVer, u16) = 1;
    EB(ZO_ExecBase_ChkBase, u32) = 0;   /* not ~ExecBase: a reset never trusts it (no path B, owner 2026-10-30) */
    EB(ZO_ExecBase_SysStkUpper, u32) = Z.stack_top;
    EB(ZO_ExecBase_SysStkLower, u32) = Z.stack_top - (16u << 10);
    EB(ZO_ExecBase_IDNestCnt, s8) = -1;
    EB(ZO_ExecBase_TDNestCnt, s8) = -1;
    EB(ZO_ExecBase_AttnFlags, u16) = Z.attn;
    EB(ZO_ExecBase_ResModules, u32) = ZNULLSPR + 16;    /* an empty, zero-ended array */
    static const u16 lists[] = { ZO_ExecBase_MemList, ZO_ExecBase_ResourceList, ZO_ExecBase_DeviceList,
                                 ZO_ExecBase_IntrList, ZO_ExecBase_LibList, ZO_ExecBase_PortList,
                                 ZO_ExecBase_TaskReady, ZO_ExecBase_TaskWait, ZO_ExecBase_SemaphoreList };
    static const u8 ltype[] = { NT_MEMORY, NT_RESOURCE, 3, NT_INTERRUPT, NT_LIBRARY, NT_MSGPORT, NT_TASK, NT_TASK, 15 };
    for (u32 i = 0; i < sizeof lists / 2; i++)
        newlist((struct zlist *)(ZEXEC + lists[i]), ltype[i]);
    for (int i = 0; i < 4; i++)
        EB(ZO_ExecBase_LastAlert + 4 * i, u32) = 0xFFFFFFFFu;
    EB(ZO_ExecBase_VBlankFrequency, u8) = Z.pal ? 50 : 60;
    EB(ZO_ExecBase_PowerSupplyFrequency, u8) = Z.pal ? 50 : 60;
    EB(ZO_ExecBase_ex_EClockFrequency, u32) = Z.a.eclock_hz;
    for (int i = 0; i < 4; i++)
        c_AddTail((struct zlist *)(ZEXEC + ZO_ExecBase_LibList), (struct znode *)Z.lib[i]);
    /* the server chains: PORTS, COPER, VERTB, EXTER, NMI */
    static const u8 ch[] = { 3, 4, 5, 13, 15 };
    for (u32 i = 0; i < sizeof ch; i++) {
        u32 n = ch[i];
        newlist(&chains[n], NT_INTERRUPT);
        chainint[n].node.type = NT_INTERRUPT;
        chainint[n].node.name = (char *)chainname;
        chainint[n].data = (u32)&chains[n];
        chainint[n].code = (u32)zx_chain;
        IV(n)->data = (u32)&chains[n];
        IV(n)->code = (u32)zx_chain;
        IV(n)->node = &chainint[n].node;
    }
    /* the memory lists (4.1, 4.2): one MemHeader a region, its chunks exactly what the plan gives */
    for (u32 r = 0; r < 8; r++) {
        struct zmh *h = 0;
        struct zchunk **tail = 0;
        for (u32 i = 0; i < Z.nchunk; i++) {
            const struct zp_chunk *c = &Z.chunk[i];
            if (c->reg != r || c->len < 8)
                continue;
            if (!h) {
                h = (struct zmh *)take(sizeof *h);
                h->node.type = NT_MEMORY;
                h->attrs = (u16)Z.reg[r].attrs;
                h->node.pri = (s8)(Z.reg[r].attrs & MEMF_CHIP ? -10 : 0);
                h->node.name = Z.reg[r].attrs & MEMF_CHIP ? "chip memory" : "expansion memory";
                h->lower = Z.reg[r].lower < 0x1000 ? 0x1000 : Z.reg[r].lower;
                h->upper = Z.reg[r].upper;
                tail = &h->first;
                if (Z.reg[r].attrs & MEMF_CHIP)
                    EB(ZO_ExecBase_MaxLocMem, u32) = h->upper;
            }
            struct zchunk *m = (struct zchunk *)c->addr;
            m->next = 0;
            m->bytes = c->len & ~7u;
            *tail = m;
            tail = &m->next;
            h->free += m->bytes;
        }
        if (h)
            c_Enqueue((struct zlist *)(ZEXEC + ZO_ExecBase_MemList), &h->node);
    }
    /* the process and its CLI (6) */
    u8 *pr = (u8 *)take(ZO_Process_SIZEOF + 16);
    Z.proc = (u32)pr;
    struct znode *pn = (struct znode *)pr;
    pn->type = NT_PROCESS;
    pn->name = b->name;
    *(u32 *)(pr + ZO_Task_tc_SigAlloc) = 0xFFFF;
    newlist((struct zlist *)(pr + ZO_Process_pr_MsgPort + ZO_MsgPort_mp_MsgList), NT_MESSAGE);
    pr[ZO_Process_pr_MsgPort + ZO_Node_ln_Type] = NT_MSGPORT;
    *(u32 *)(pr + ZO_Process_pr_MsgPort + ZO_MsgPort_mp_SigTask) = (u32)pr;
    pr[ZO_Process_pr_MsgPort + ZO_MsgPort_mp_SigBit] = 8;   /* SIGB_DOS */
    *(u32 *)(pr + ZO_Process_pr_WindowPtr) = 0xFFFFFFFFu;
    u32 out = take(ZO_FileHandle_SIZEOF), in = take(ZO_FileHandle_SIZEOF);
    Z.out_fh = out >> 2, Z.in_fh = in >> 2;
    *(u32 *)(pr + ZO_Process_pr_COS) = Z.out_fh;
    *(u32 *)(pr + ZO_Process_pr_CIS) = Z.in_fh;
    u32 cli = take(64);
    *(u32 *)(pr + ZO_Process_pr_CLI) = cli >> 2;
    *(u32 *)(cli + ZO_CommandLineInterface_cli_CommandName) = bstr(b->name);
    *(u32 *)(cli + ZO_CommandLineInterface_cli_FailLevel) = 10;
    *(u32 *)(cli + ZO_CommandLineInterface_cli_StandardInput) = Z.in_fh;
    *(u32 *)(cli + ZO_CommandLineInterface_cli_CurrentInput) = Z.in_fh;
    *(u32 *)(cli + ZO_CommandLineInterface_cli_StandardOutput) = Z.out_fh;
    *(u32 *)(cli + ZO_CommandLineInterface_cli_CurrentOutput) = Z.out_fh;
    EB(ZO_ExecBase_ThisTask, u32) = Z.proc;
    /* graphics' and intuition's fields (5.3, 5.5) */
    u32 gb = Z.lib[1], ib = Z.lib[3];
    u32 cpr = take(ZO_cprlist_SIZEOF);
    *(u32 *)(cpr + ZO_cprlist_start) = ZBLANK;
    *(u32 *)(IB_VIEWLORD(ib) + ZO_View_LOFCprList) = cpr;
    *(u32 *)(gb + ZO_GfxBase_ActiView) = IB_VIEWLORD(ib);
    *(u32 *)(gb + ZO_GfxBase_copinit) = ZBLANK;
    *(u32 *)(gb + ZO_GfxBase_LOFlist) = ZBLANK;
    *(u32 *)(gb + ZO_GfxBase_SHFlist) = ZBLANK;
    *(u16 *)(gb + ZO_GfxBase_DisplayFlags) = Z.pal ? 4 : 1;     /* PAL, NTSC (graphics/gfxbase.h) */
    *(u8 *)(gb + ZO_GfxBase_VBlank) = Z.pal ? 50 : 60;
    u8 crb = Z.ecs ? 1 : 0;             /* GFXF_HR_AGNUS; GFXF_HR_DENISE: an ECS Denise answers $FC, Lisa $F8 */
    if ((DENISEID & 0xFF) == 0xFC || Z.aga)
        crb |= 2;
    if (Z.aga)
        crb |= 4 | 8;                   /* GFXF_AA_ALICE, GFXF_AA_LISA (the NDK's graphics/gfxbase.h) */
    *(u8 *)(gb + ZO_GfxBase_ChipRevBits0) = crb;
    *(volatile u32 *)4 = ZEXEC;         /* only now does address 4 point at an ExecBase (7.3 step 3) */
}
