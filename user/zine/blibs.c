/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's blob: the small AmigaOS's libraries (DESIGN D29 section 5; the first set of build step 3): ExecBase and the
 * other bases with their jump tables, the memory lists, the process and its CLI, and the functions beyond memory
 * (blob.c). Every vector not listed here is "jsr zx_unsup": the program stops cleanly and `zine` names the call. */
#include "blob.h"
#include "zvol.h"
#include "hunk.h"
#include "zpath.h"

/* ---- lists (exec/lists.h) ---- */
static u32 cia_base[2];                 /* ZINE's ciaa.resource and ciab.resource (below) */
static u32 timer_base;                  /* ZINE's timer.device (below) */
static int timer_step(void);
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

/* ---- the running task's signals (FindTask, SetTaskPri, Signal, SetSignal, Wait: btask.c, d29 16.16) ---- */
#define TASK(o, t) (*(volatile t *)(EB(ZO_ExecBase_ThisTask, u32) + ZO_Task_##o))

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
#define AN_BadOverlay 0x0700000Cu       /* exec/alerts.h (NDK 3.2): only the overlay supervisor raises it */
void c_Alert(u32 code)
{
    /* AN_BadOverlay as a dead end too: the supervisor that raises it retries the load it cannot do (the Guru Book's
     * sketch, 22.2.10.7), so going on would loop; LoadSeg has put why in the output */
    if ((code & AT_DeadEnd) || (code & ~AT_DeadEnd) == AN_BadOverlay) {
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

static u32 slen(const char *s);
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
    /* utility and the math libraries, ZINE's own (16.12) */
    u32 x = name ? zb_more_open(name, version) : 0;
    if (x)
        return x;
    /* one a program added or ZINE loaded from LIBS: (blibload.c, 16.8); else named in the program's output and 0,
     * as AmigaOS answers when it has none */
    return name ? zb_lib_open(name, version) : 0;
}

/* exec's TaggedOpenLibrary (3.0, -810): a library by its OLTAG number: 1 graphics, 3 intuition, 4 dos (the NDK 3.2's
 * exec/execbase.h); the others ZINE has not (0) */
u32 c_TaggedOpenLibrary(u32 tag)
{
    static const u8 lib[] = { 0, 1, 0, 3, 2 };
    if (tag == 0 || tag >= sizeof lib || !lib[tag])
        return 0;
    struct zlib *l = (struct zlib *)Z.lib[lib[tag]];
    l->opencnt++;
    return (u32)l;
}

u32 c_OldOpenLibrary(const char *name) { return c_OpenLibrary(name, 0); }    /* any version */

void c_CloseLibrary(struct zlib *l)
{
    for (int i = 0; i < 4; i++)
        if (l && (u32)l == Z.lib[i]) {
            if (l->opencnt)
                l->opencnt--;
            return;
        }
    if (zb_more_close((u32)l))
        return;
    zb_lib_close((u32)l);               /* its own Close vector (blibload.c) */
}

static u32 slen(const char *s);
u32 c_OpenResource(const char *name)
{
    static const char msg[] = "ZINE: no resource ";  /* named in the program's output (zine shows it) */
    if (name && streq(name, "ciaa.resource"))
        return cia_base[0];
    if (name && streq(name, "ciab.resource"))
        return cia_base[1];
    u32 r = name ? c_FindName((struct zlist *)(ZEXEC + ZO_ExecBase_ResourceList), name) : 0;
    if (r)
        return r;                       /* one a program added (AddResource) */
    if (name) {
        zb_out(msg, sizeof msg - 1);
        zb_out(name, slen(name));
        zb_out("\n", 1);
    }
    return 0;
}

/* no devices in the first version (5.5): the request's io_Error and the answer are IOERR_OPENFAIL */
static u32 slen(const char *s)
{
    u32 n = 0;
    while (s && s[n])
        n++;
    return n;
}

static int is_zi(const u8 *io)          /* keyboard.device or gameport.device (binput.c) */
{
    u32 d = *(const u32 *)(io + ZO_IORequest_io_Device);
    return d && (d == zb_kbd_base || d == zb_gp_base);
}

u32 c_OpenDevice(const char *name, u32 unit, u8 *io, u32 flags)
{
    (void)unit, (void)flags;
    static const char msg[] = "ZINE: no device ";     /* said once per name in the program's output (zine shows it) */
    if (name && timer_base && streq(name, "timer.device")) {
        if (io) {
            *(u32 *)(io + ZO_IORequest_io_Device) = timer_base;
            *(u32 *)(io + 24) = unit <= 4 ? unit : 0;   /* io_Unit: the unit's number (16.13: TimerBeginIO reads it) */
            io[31] = 0;                 /* io_Error */
        }
        return 0;
    }
    if (name && streq(name, "audio.device"))
        return zb_audio_open(io, unit, flags);  /* blibload.c: its channels allocated, nothing played (16.8) */
    if (name && streq(name, "console.device"))
        return zb_con_open(io, unit, flags);    /* blibload.c: its text into the output, no keys (16.14) */
    if (name && streq(name, "input.device"))
        return zb_inp_open(io);         /* blibload.c, binput.c: the handlers get the events (16.17) */
    if (name && streq(name, "keyboard.device"))
        return zi_open(io, zb_kbd_base, unit);  /* binput.c (16.17) */
    if (name && streq(name, "gameport.device"))
        return zi_open(io, zb_gp_base, unit);
    if (name) {
        zb_out(msg, sizeof msg - 1);
        zb_out(name, slen(name));
        zb_out("\n", 1);
    }
    if (io) {
        *(u32 *)(io + ZO_IORequest_io_Device) = 0;
        io[ZO_IORequest_io_Error] = 0xFF;
    }
    return (u32)-1;
}

void c_CloseDevice(u8 *io)
{
    if (io && *(u32 *)(io + ZO_IORequest_io_Device) == zb_audio_base)
        zb_audio_close(io);
    else if (io && zb_con_base && *(u32 *)(io + ZO_IORequest_io_Device) == zb_con_base)
        zb_con_close(io);
    else if (io && is_zi(io))
        zi_close(io);
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
/* CacheControl (exec): the old bits; the instruction cache switched as asked (old programs that change their own code
 * switch it off; the 68040's 4 KB one and Emu68's translations would run the old code). The 020/030's CACR has
 * exec's layout (EnableI 0, FreezeI 1, IBE 4); the 040/060's IE is bit 15. The data cache stays off as the kernel
 * hands the machine over (zine_enter; kernel/zine.c, DESIGN D37): only Zenix's own fast RAM is set up to be cached,
 * the program's chip RAM DMA would not be seen if its writes stayed in the cache. Both caches are cleared after
 * (CACRF_ClearI/ClearD always). Supervisor mode (zs_CacheControl, blobs.S). */
u32 zb_cachectl(u32 bits, u32 mask);
u32 zb_cachectl(u32 bits, u32 mask)
{
    extern u32 zb_attn;
    int big = (zb_attn & 0x0088) != 0;
    u32 c;
    __asm__ volatile("movec %%cacr,%0" : "=d"(c));
    u32 old = big ? (c >> 15 & 1) | (c >> 31 & 1) << 8 : c & 0x3313;
    u32 nw = (old & ~mask) | (bits & mask);
    if (big) {
        c = (c & ~0x80008000u) | (nw & 1 ? 0x8000u : 0) | (old & 0x100 ? 0x80000000u : 0);
        __asm__ volatile(".word 0xF4F8\n\tmovec %0,%%cacr\n\t.word 0xF4F8" : : "d"(c) : "memory");
    } else {
        c = (c & ~0x13u) | (nw & 0x13) | 0x0808;
        __asm__ volatile("movec %0,%%cacr" : : "d"(c) : "memory");
    }
    return old;
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
    extern u32 zb_tracing;
    for (u32 i = 0; zb_tracing && i < n; i++) {     /* the trace build: the output on the serial line as it comes */
        while (!(CUSTOM(0x018) & 0x2000))
            ;
        CUSTOM(0x030) = (u16)(0x100 | (u8)s[i]);
    }
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

/* a command Execute runs (16.11) may have its own: Z.cur_in, Z.cur_out (0: the console's) */
u32 c_Output(void) { return Z.cur_out ? Z.cur_out : Z.out_fh; }
u32 c_Input(void) { return Z.cur_in ? Z.cur_in : Z.in_fh; }
u32 c_IoErr(void) { return Z.ioerr; }

u32 c_SetIoErr(u32 e)
{
    u32 o = Z.ioerr;
    Z.ioerr = e;
    return o;
}

/* ---- dos's files (5.6): the program's directory as the volume zine made (user/zine/zvol.h), never written; a file
 * written on any path is one of ZINE's memory files (16.9, below), gone with the session (owner answer 3); NIL:
 * takes the bytes and keeps none. A FileHandle (44 bytes) is given, its fh_Args the slot here. ---- */
#define NFILES 16
static struct { u8 used, sink, ram; u32 ent, pos, fh; } files[NFILES];     /* ram: a memory file's index + 1 */
#define MODE_OLDFILE 1005
#define MODE_NEWFILE 1006
#define MODE_READWRITE 1004

static int slot_of(u32 fh)
{
    if (!fh || fh == Z.in_fh || fh == Z.out_fh)
        return -1;
    u32 a = fh << 2;
    for (int i = 0; i < NFILES; i++)
        if (files[i].used && files[i].fh == a)
            return i;
    return -1;
}

/* ---- ZINE's memory files and the assigns (docs/d29-zine.md 16.8, 16.9). Every file a program writes, on any path
 * (RAM:, T:, ENV:, its own folder, SYS:, DH0:), is kept in its own memory (AllocMem) under the path's key (zpath.h
 * za_key: "PROGDIR:x" and "x" are one file), read back by that path, loaded (LoadSeg, OpenLibrary) and deleted, and
 * gone with the session (owner answers 3 and of 2026-11-04): nothing reaches a disk. A memory file stands in front of
 * the volume's file of the same path for reads and deletes; deleting a volume file leaves a memory file marked gone,
 * which hides it for the rest of the session. Names compare without case. The assigns are zpath.h's: Open, LoadSeg,
 * DeleteFile and OpenLibrary follow them. ---- */
static void say3(const char *a, const char *b, const char *c);
#define NRAM 32
#define ZPATH 200                       /* bytes of a path with its assigns followed */
static struct { char *name; u32 nlen, buf, cap, size, open, gone; } ramf[NRAM];  /* name (the key) 0: free */
static struct za_table assigns;

static int ram_find(const char *n)
{
    for (int i = 0; i < NRAM; i++)
        if (ramf[i].name && zv_same(n, slen(n), ramf[i].name))
            return i;
    return -1;
}

/* a new, empty memory file of key n: its index, or -1 and IoErr */
static int ram_new(const char *n)
{
    u32 l = slen(n);
    if (!l || n[l - 1] == ':') {
        Z.ioerr = 212;                  /* ERROR_OBJECT_WRONG_TYPE: a device itself ("RAM:") */
        return -1;
    }
    for (int i = 0; i < NRAM; i++)
        if (!ramf[i].name) {
            char *m = (char *)zx_AllocMem(l + 1, MEMF_PUBLIC);
            if (!m)
                break;
            zmemcpy(m, n, l + 1);
            ramf[i].name = m, ramf[i].nlen = l, ramf[i].buf = ramf[i].cap = ramf[i].size = ramf[i].open = 0;
            ramf[i].gone = 0;
            return i;
        }
    say3("ZINE: the program's written files are full (ZINE keeps 32, in the program's memory): no room for ", n, "\n");
    Z.ioerr = 221;                      /* ERROR_DISK_FULL */
    return -1;
}

static void ram_free(int r)
{
    if (ramf[r].buf)
        zx_FreeMem(ramf[r].buf, ramf[r].cap);
    zx_FreeMem((u32)ramf[r].name, ramf[r].nlen + 1);
    ramf[r].name = 0;
}

/* n bytes at pos of RAM: file r, the file grown as needed: n, or -1 and IoErr */
static u32 ram_write(int r, u32 pos, const char *s, u32 n)
{
    u32 end = pos + n;
    if (end < pos) {
        Z.ioerr = 221;
        return (u32)-1;
    }
    if (end > ramf[r].cap) {
        u32 cap = ramf[r].cap * 2 > end ? ramf[r].cap * 2 : end;
        cap = (cap + 4095) & ~4095u;
        u32 nb = cap > end ? zx_AllocMem(cap, MEMF_PUBLIC) : 0;
        if (!nb)                        /* overflowed, or no room for the doubled size: just what is needed */
            nb = zx_AllocMem(cap = end, MEMF_PUBLIC);
        if (!nb) {
            say3("ZINE: no memory left in the program's to keep what it writes: ", ramf[r].name, " not written\n");
            Z.ioerr = 221;              /* ERROR_DISK_FULL */
            return (u32)-1;
        }
        if (ramf[r].buf) {
            zmemcpy((void *)nb, (const void *)ramf[r].buf, ramf[r].size);
            zx_FreeMem(ramf[r].buf, ramf[r].cap);
        }
        ramf[r].buf = nb, ramf[r].cap = cap;
    }
    zmemcpy((u8 *)ramf[r].buf + pos, s, n);
    if (end > ramf[r].size)
        ramf[r].size = end;
    return n;
}

/* 1 when path is on a device that is a stream, not a disk (NIL:, CONSOLE:, PRT:, SER:, PAR:, AUX:): what is written
 * goes and nothing is kept, a read is at its end at once; everything else is a disk, its writes kept in memory */
static int stream_dev(const char *path)
{
    static const char *const dev[] = { "NIL", "CONSOLE", "PRT", "SER", "PAR", "AUX" };
    const char *c = path;
    while (*c && *c != ':')
        c++;
    if (*c)
        for (u32 i = 0; i < sizeof dev / sizeof dev[0]; i++)
            if (zv_same(path, (u32)(c - path), dev[i]))
                return 1;
    return 0;
}

/* path's key (zpath.h) in k: 1, or 0 when it has none (too long, above a device's top) */
static int key_of(const char *path, char *k)
{
    return za_key(path, ((const struct zv_hdr *)Z.a.vol)->volname, k, ZPATH);
}

/* dos CurrentDir (16.13): a name without a device is relative to the current directory, which a program may set
 * with CurrentDir to one of the volume's directories or to RAM:. Such a name is turned into one from the volume's top
 * ("SYS:dir/sub/name", or "RAM:name") before anything else looks at it; 0 the volume's top, as ZINE starts. */
static u32 cd_ent;                      /* the volume's directory entry, or CD_RAM */
#define CD_RAM 0xFFFFFFFFu
static char cdbuf[ZPATH];
static const char *cd_path(const char *name)
{
    if (!cd_ent || !name)
        return name;
    for (const char *c = name; *c; c++)
        if (*c == ':')
            return name;
    char t[ZPATH];
    u32 n = 0, k = slen(name);
    if (cd_ent == CD_RAM) {
        zmemcpy(t, "RAM:", 4);
        n = 4;
    } else {
        const void *vol = (const void *)Z.a.vol;
        u32 pos = ZPATH;                /* the directories' names from the back */
        for (u32 e = cd_ent, d = 0; e && d < 64; e = zv_ent(vol, e)->parent, d++) {
            const struct zv_ent *en = zv_ent(vol, e);
            if (en->namelen + 1u > pos)
                return name;
            pos -= en->namelen + 1;
            zmemcpy(t + pos, en->name, en->namelen);
            t[pos + en->namelen] = '/';
        }
        if (pos < 4)
            return name;
        zmemcpy(t, "SYS:", 4);
        n = 4;
        for (u32 i = pos; i < ZPATH; i++)
            t[n++] = t[i];
    }
    if (n + k + 1 > ZPATH)
        return name;
    zmemcpy(t + n, name, k + 1);
    zmemcpy(cdbuf, t, n + k + 1);
    return cdbuf;
}

/* where a path names a file now: 1 a memory file (*r), 2 a volume entry (*ent: a file or a directory), 0 none, with
 * *nodev 1 when every path it stands for lies on a device ZINE has not (NIL:, DH0:) and holds no memory file; *r a
 * memory file marked gone that hid the path (else -1) */
static char hit[ZPATH];                 /* the path locate found its file as */
static int locate1(const char *name, u32 *ent, int *r, int *nodev)
{
    int dev = 0, seen = 0, g = -1;
    char k[ZPATH];
    *nodev = 0;
    name = cd_path(name);
    for (u32 i = 0; za_expand(&assigns, name, i, hit, sizeof hit); i++) {
        seen = 1;
        int m = key_of(hit, k) ? ram_find(k) : -1;
        if (m >= 0 && !ramf[m].gone) {
            *r = m;
            return 1;
        }
        if (m >= 0 || za_ram(hit)) {    /* deleted this session (hides the volume's file), or on RAM: */
            dev = 1;
            if (m >= 0 && g < 0)
                g = m;
            continue;
        }
        int e = zv_find((const void *)Z.a.vol, 0, hit, ent);
        if (e != -ZE_NODEV)
            dev = 1;
        if (!e)
            return 2;
    }
    *nodev = seen && !dev;
    *r = g;
    return 0;
}

/* 16.13: a volume or device ZINE does not have ("Love:FLT-Lve2.Skl": the name of the disk the demo came on) is taken as
 * the program's own folder when the path is there, as the user's "Assign Love: <its folder>" would make it under
 * AmigaOS (Virtual Dreams' Love and sumea wait for their disk forever otherwise); said once a name. Not for the
 * devices that are not a disk of the program's (T:, ENV:, ENVARC:, CLIPS:, the streams). */
static int locate(const char *name, u32 *ent, int *r, int *nodev)
{
    int w = locate1(name, ent, r, nodev);
    if (w || !*nodev)
        return w;
    const char *c = name;
    while (*c && *c != ':')
        c++;
    u32 dl = (u32)(c - name);
    static const char *const keep[] = { "T", "ENV", "ENVARC", "CLIPS" };
    if (!*c || !dl || stream_dev(name))
        return 0;
    for (u32 i = 0; i < sizeof keep / sizeof keep[0]; i++)
        if (zv_same(name, dl, keep[i]))
            return 0;
    char p[ZPATH];
    u32 n = slen(c + 1);
    if (4 + n + 1 > ZPATH)
        return 0;
    zmemcpy(p, "SYS:", 4);
    zmemcpy(p + 4, c + 1, n + 1);
    int nd;
    w = locate1(p, ent, r, &nd);
    static char said[4][32];
    if (w && dl < 31) {
        int i = 0;
        while (i < 4 && said[i][0] && !zv_same(name, dl, said[i]))
            i++;
        if (i < 4 && !said[i][0]) {
            zmemcpy(said[i], name, dl);
            said[i][dl] = 0;
            say3("ZINE: ", Z.blk->name, " asks for the volume ");
            say3(said[i], ":, which ZINE takes as the program's own folder (as \"Assign ", said[i]);
            say3(": <its folder>\" would)\n", "", "");
        }
    }
    if (w)
        *nodev = 0;
    return w;
}

/* a file's bytes for loading: 0 and *f, *size, *ent (~0 for a RAM: file), or -ZE_NOENT */
static int file_bytes(const char *name, const zu8 **f, u32 *size, u32 *ent)
{
    int r, nodev;
    int w = locate(name, ent, &r, &nodev);
    if (w == 1) {
        *f = (const zu8 *)ramf[r].buf, *size = ramf[r].size, *ent = ~0u;
        return *size ? 0 : -ZE_NOENT;
    }
    if (w == 2 && !zv_ent((const void *)Z.a.vol, *ent)->dir) {
        const struct zv_ent *e = zv_ent((const void *)Z.a.vol, *ent);
        *f = (const zu8 *)Z.a.vol + e->off, *size = e->size;
        return 0;
    }
    return -ZE_NOENT;
}

/* (bfont.c, 16.13) a file's bytes by its name, assigns and the current directory followed: 0, or -ZE_NOENT */
int zb_file_bytes(const char *name, const u8 **f, u32 *size)
{
    u32 ent;
    return file_bytes(name, f, size, &ent);
}
/* a directory of the volume by its name: 1 and its entry */
int zb_dir_ent(const char *name, u32 *ent)
{
    int r, nodev;
    return locate(name, ent, &r, &nodev) == 2 && zv_ent((const void *)Z.a.vol, *ent)->dir;
}

int zb_file_exists(const char *name, char *where, u32 max)
{
    const zu8 *f;
    u32 size, ent;
    if (file_bytes(name, &f, &size, &ent))
        return 0;
    u32 n = slen(hit) < max ? slen(hit) : max - 1;
    zmemcpy(where, hit, n);
    where[n] = 0;
    return 1;
}

/* a FileHandle on volume entry ent, a RAM: file (ram its index + 1) or a sink: its BPTR, or 0 and IoErr */
static u32 fh_new(u32 ent, int sink, int ram)
{
    for (int i = 0; i < NFILES; i++)
        if (!files[i].used) {
            u32 fh = zx_AllocMem(44, 1 << 16);
            if (!fh)
                break;
            files[i].used = 1, files[i].sink = (u8)sink, files[i].ram = (u8)ram, files[i].ent = ent;
            files[i].pos = 0, files[i].fh = fh;
            if (ram)
                ramf[ram - 1].open++;
            *(u32 *)(fh + 36) = (u32)i; /* fh_Args: what a packet's dp_Arg1 names (packet() below) */
            *(u32 *)(fh + 24) = (u32)-1;
            *(u32 *)(fh + 8) = Z.hport; /* fh_Type: the port a program may send its packets to */
            return fh >> 2;
        }
    Z.ioerr = 103;                      /* ERROR_NO_FREE_STORE */
    return 0;
}

u32 c_Open(const char *name, s32 mode)
{
    extern u32 zb_tracing;
    if (zb_tracing && name && c_TypeOfMem((u32)name)) {     /* the trace build: what is opened (16.13) */
        u32 n = 0;
        while (n < 60 && name[n] >= 32 && name[n] < 127)
            n++;
        for (u32 k = 0; k <= n; k++) {
            while (!(CUSTOM(0x018) & 0x2000))
                ;
            CUSTOM(0x030) = (u16)(0x100 | (u8)(k == 0 ? 'O' : name[k - 1]));
        }
        while (!(CUSTOM(0x018) & 0x2000))
            ;
        CUSTOM(0x030) = 0x10A;
    }
    if (!name) {
        Z.ioerr = 205;                  /* ERROR_OBJECT_NOT_FOUND */
        return 0;
    }
    if ((name[0] == '*' && !name[1]) || zv_same(name, 4, "CON:") || zv_same(name, 4, "RAW:"))
        return mode == MODE_OLDFILE ? Z.in_fh : Z.out_fh;
    char cdn[ZPATH];
    const char *cdp = cd_path(name);
    if (cdp != name) {
        zmemcpy(cdn, cdp, slen(cdp) + 1);
        name = cdn;
    }
    u32 ent = 0;
    int r = -1, nodev = 0;
    int w = locate(name, &ent, &r, &nodev);
    if (mode == MODE_NEWFILE || (mode == MODE_READWRITE && w != 1)) {
        /* every path keeps what is written, in memory (16.9): a new file, an old one emptied, or (MODE_READWRITE) a
         * copy of the volume's file to change; at the path it was found as, else the name's first meaning */
        char p[ZPATH], k[ZPATH];
        if (w == 2)
            zmemcpy(p, hit, sizeof p);
        else if (!za_expand(&assigns, name, 0, p, sizeof p)) {
            Z.ioerr = 210;              /* ERROR_INVALID_COMPONENT_NAME: too long */
            return 0;
        }
        if (stream_dev(p))
            return fh_new(0, 1, 0);     /* NIL: and the like take the bytes and keep none */
        if (w == 2 && zv_ent((const void *)Z.a.vol, ent)->dir) {
            Z.ioerr = 212;              /* ERROR_OBJECT_WRONG_TYPE: a directory */
            return 0;
        }
        if (!key_of(p, k)) {
            Z.ioerr = 210;
            return 0;
        }
        int m = w == 1 ? r : ram_find(k);
        if (m < 0 && (m = ram_new(k)) < 0)
            return 0;
        if (mode == MODE_NEWFILE || ramf[m].gone)
            ramf[m].size = 0;
        ramf[m].gone = 0;
        if (w == 2 && mode == MODE_READWRITE) {
            const struct zv_ent *e = zv_ent((const void *)Z.a.vol, ent);
            ramf[m].size = 0;
            if (e->size && ram_write(m, 0, (const char *)Z.a.vol + e->off, e->size) == (u32)-1)
                return 0;
        }
        return fh_new(0, 0, m + 1);
    }
    if (w == 1)
        return fh_new(0, 0, r + 1);
    if (w == 2 && !zv_ent((const void *)Z.a.vol, ent)->dir)
        return fh_new(ent, 0, 0);
    if (!w && stream_dev(name))
        return fh_new(0, 1, 0);         /* NIL: and the like: at the end at once (any other missing file: not found) */
    static const char msg[] = "ZINE: no file ";
    zb_out(msg, sizeof msg - 1);
    zb_out(name, slen(name));
    zb_out("\n", 1);
    Z.ioerr = w ? 212 : 205;            /* ERROR_OBJECT_WRONG_TYPE, ERROR_OBJECT_NOT_FOUND */
    return 0;
}

u32 c_Close(u32 fh)
{
    int i = slot_of(fh);
    if (i < 0)
        return 1;
    if (files[i].ram)
        ramf[files[i].ram - 1].open--;
    zx_FreeMem(files[i].fh, 44);
    files[i].used = 0;
    return 1;
}

/* the bytes of slot i's file and its size */
static const u8 *slot_data(int i, u32 *size)
{
    if (files[i].sink) {
        *size = 0;
        return 0;
    }
    if (files[i].ram) {
        *size = ramf[files[i].ram - 1].size;
        return (const u8 *)ramf[files[i].ram - 1].buf;
    }
    const void *vol = (const void *)Z.a.vol;
    const struct zv_ent *en = zv_ent(vol, files[i].ent);
    *size = en->size;
    return (const u8 *)vol + en->off;
}

u32 c_Read(u32 fh, char *buf, s32 n)
{
    if (fh == Z.in_fh)
        return 0;                       /* the input is empty: end of file */
    int i = slot_of(fh);
    if (i < 0 || n < 0) {
        Z.ioerr = 114;
        return (u32)-1;
    }
    u32 size;
    const u8 *d = slot_data(i, &size);
    u32 left = size > files[i].pos ? size - files[i].pos : 0, k = (u32)n < left ? (u32)n : left;
    if (k)
        zmemcpy(buf, d + files[i].pos, k);
    files[i].pos += k;
    return k;
}

s32 c_Seek(u32 fh, s32 pos, s32 mode)
{
    int i = slot_of(fh);
    if (i < 0) {
        Z.ioerr = 114;
        return -1;
    }
    u32 size;
    slot_data(i, &size);
    s32 old = (s32)files[i].pos, base = mode < 0 ? 0 : mode > 0 ? (s32)size : old;   /* BEGINNING -1, END 1 */
    s32 np = base + pos;
    if (np < 0 || (u32)np > size) {
        Z.ioerr = 219;                  /* ERROR_SEEK_ERROR */
        return -1;
    }
    files[i].pos = (u32)np;
    return old;
}

/* dos DeleteFile (-72), in memory only (16.9): a memory file goes (not while it is open: ERROR_OBJECT_IN_USE, as
 * RAM: answers); one of the volume (the program's own files), or a memory file in front of one, is marked gone, so
 * the path finds nothing for the rest of the session while the disk keeps the file. A directory of the volume is
 * refused, said in the output. */
u32 c_DeleteFile(const char *name)
{
    char cdn[ZPATH];
    const char *cdp = cd_path(name);
    if (cdp != name) {
        zmemcpy(cdn, cdp, slen(cdp) + 1);
        name = cdn;
    }
    u32 ent = 0;
    int r = -1, nodev = 0;
    int w = name ? locate(name, &ent, &r, &nodev) : 0;
    char k[ZPATH];
    if (w == 1) {
        if (ramf[r].open) {
            Z.ioerr = 202;              /* ERROR_OBJECT_IN_USE */
            return 0;
        }
        u32 e;
        if (zv_find((const void *)Z.a.vol, 0, ramf[r].name, &e)) {
            ram_free(r);                /* nothing of the volume behind it */
            return (u32)-1;
        }
        if (ramf[r].buf)                /* in front of a volume file: kept, marked gone */
            zx_FreeMem(ramf[r].buf, ramf[r].cap);
        ramf[r].buf = ramf[r].cap = ramf[r].size = 0, ramf[r].gone = 1;
        return (u32)-1;
    }
    if (w == 2) {
        if (zv_ent((const void *)Z.a.vol, ent)->dir) {
            say3("ZINE: ", Z.blk->name, " asked to delete ");
            say3(name, ", a directory of its own: ZINE deletes files only, nothing deleted\n", "");
            Z.ioerr = 214;              /* ERROR_DISK_WRITE_PROTECTED */
            return 0;
        }
        int m = key_of(hit, k) ? ram_new(k) : -1;
        if (m < 0)
            return 0;
        ramf[m].gone = 1;               /* the volume's file hidden for the session; the disk keeps it */
        return (u32)-1;
    }
    Z.ioerr = 205;                      /* ERROR_OBJECT_NOT_FOUND */
    return 0;
}

/* ---- dos locks and directories (16.13): Lock, UnLock, DupLock, Examine, ExNext, Info, CurrentDir, ParentDir,
 * DupLockFromFH, OpenFromLock, ExamineFH, NameFromLock, SameLock, ParentOfFH, on the volume's files and directories,
 * ZINE's memory files and RAM:'s top (the autodocs; FileLock 20 bytes, FileInfoBlock 260, InfoData 36). A lock is a
 * FileLock in the program's memory, its BPTR given; ZINE keeps which are its own, so a lock it never gave is refused
 * (ERROR_INVALID_LOCK). ExNext lists a directory of the volume as it was at the start (a file written during the
 * session into it is not listed; RAM:'s top lists the memory files written there). ---- */
#define NLOCK 64
#define LK_VOL 1                        /* a volume entry (a file or a directory) */
#define LK_RAM 2                        /* a memory file */
#define LK_RAMDIR 3                     /* RAM:'s top */
#define ERROR_INVALID_LOCK 211
#define ERROR_NO_MORE_ENTRIES 232
#define ERROR_OBJECT_WRONG_TYPE 212
#define ERROR_OBJECT_NOT_FOUND 205
static struct { u32 fl; u8 kind; u32 ent; } locks[NLOCK];       /* fl: the FileLock's address, 0 free */

static int lock_of(u32 lock)
{
    u32 a = lock << 2;
    for (int i = 0; lock && i < NLOCK; i++)
        if (locks[i].fl == a)
            return i;
    return -1;
}

static u32 lock_new(u8 kind, u32 ent, s32 mode)
{
    for (int i = 0; i < NLOCK; i++)
        if (!locks[i].fl) {
            u32 fl = zx_AllocMem(ZO_FileLock_SIZEOF, MEMF_PUBLIC | MEMF_CLEAR);
            if (!fl)
                break;
            *(u32 *)(fl + ZO_FileLock_fl_Key) = kind == LK_RAMDIR ? 0 : ent;
            *(s32 *)(fl + ZO_FileLock_fl_Access) = mode == -1 ? -1 : -2;    /* EXCLUSIVE_LOCK, else SHARED_LOCK */
            *(u32 *)(fl + ZO_FileLock_fl_Task) = Z.hport;
            locks[i].fl = fl, locks[i].kind = kind, locks[i].ent = ent;
            if (kind == LK_RAM)
                ramf[ent].open++;       /* not deleted while locked (ERROR_OBJECT_IN_USE) */
            return fl >> 2;
        }
    say3("ZINE: the program holds more than 64 locks at once (ZINE keeps 64): a Lock answered 0\n", "", "");
    Z.ioerr = 103;                      /* ERROR_NO_FREE_STORE */
    return 0;
}

static int is_ramdir(const char *name)
{
    const char *r = za_ram(name);
    return r && !*r;
}

/* Lock(name d1, mode d2) (-84) */
u32 c_Lock(const char *name, s32 mode)
{
    if (!name || !c_TypeOfMem((u32)name)) {
        Z.ioerr = ERROR_OBJECT_NOT_FOUND;
        return 0;
    }
    if (!*name)                         /* "": the current directory */
        return cd_ent == CD_RAM ? lock_new(LK_RAMDIR, 0, mode) : lock_new(LK_VOL, cd_ent, mode);
    if (is_ramdir(cd_path(name)))
        return lock_new(LK_RAMDIR, 0, mode);
    u32 ent = 0;
    int r = -1, nodev = 0;
    int w = locate(name, &ent, &r, &nodev);
    if (w == 1)
        return lock_new(LK_RAM, (u32)r, mode);
    if (w == 2)
        return lock_new(LK_VOL, ent, mode);
    Z.ioerr = ERROR_OBJECT_NOT_FOUND;
    return 0;
}

/* UnLock(lock d1) (-90) */
void c_UnLock(u32 lock)
{
    int i = lock_of(lock);
    if (i < 0)
        return;
    if (locks[i].kind == LK_RAM && ramf[locks[i].ent].open)
        ramf[locks[i].ent].open--;
    if (Z.proc && *(u32 *)(Z.proc + ZO_Process_pr_CurrentDir) == lock)
        *(u32 *)(Z.proc + ZO_Process_pr_CurrentDir) = 0;    /* stays the current directory, as a name */
    zx_FreeMem(locks[i].fl, ZO_FileLock_SIZEOF);
    locks[i].fl = 0;
}

/* DupLock(lock d1) (-96): 0 duplicates to 0 (the volume's top, as AmigaDOS's 0 lock) */
u32 c_DupLock(u32 lock)
{
    if (!lock)
        return 0;
    int i = lock_of(lock);
    if (i < 0) {
        Z.ioerr = ERROR_INVALID_LOCK;
        return 0;
    }
    return lock_new(locks[i].kind, locks[i].ent, -2);
}

/* a FileInfoBlock for a lock's kind and entry */
static void fib_fill(u8 *fib, u8 kind, u32 ent)
{
    zmemset(fib, 0, ZO_FileInfoBlock_SIZEOF);
    const void *vol = (const void *)Z.a.vol;
    char *nm = (char *)fib + ZO_FileInfoBlock_fib_FileName;
    s32 type = -3;                      /* ST_FILE */
    u32 size = 0, prot = 0, *date = (u32 *)(fib + ZO_FileInfoBlock_fib_Date);
    if (kind == LK_VOL) {
        const struct zv_ent *e = zv_ent(vol, ent);
        const char *src = ent ? e->name : ((const struct zv_hdr *)vol)->volname;
        u32 n = slen(src) < 107 ? slen(src) : 107;
        zmemcpy(nm, src, n);
        type = e->dir ? (ent ? 2 : 1) : -3;     /* ST_USERDIR, ST_ROOT */
        size = e->size, prot = e->prot;
        date[0] = e->days, date[1] = e->mins, date[2] = e->ticks;
    } else {
        const char *src = "Ram Disk";
        if (kind == LK_RAM) {
            src = ramf[ent].name;
            for (const char *c = ramf[ent].name; *c; c++)
                if (*c == ':' || *c == '/')
                    src = c + 1;
            size = ramf[ent].size;
        } else
            type = 1;
        u32 n = slen(src) < 107 ? slen(src) : 107;
        zmemcpy(nm, src, n);
        c_DateStamp(date);
    }
    *(u32 *)(fib + ZO_FileInfoBlock_fib_DiskKey) = kind == LK_RAMDIR ? 0 : ent;
    *(s32 *)(fib + ZO_FileInfoBlock_fib_DirEntryType) = type;
    *(s32 *)(fib + ZO_FileInfoBlock_fib_EntryType) = type;
    *(u32 *)(fib + ZO_FileInfoBlock_fib_Protection) = prot;
    *(u32 *)(fib + ZO_FileInfoBlock_fib_Size) = size;
    *(u32 *)(fib + ZO_FileInfoBlock_fib_NumBlocks) = (size + 511) / 512;
}

/* Examine(lock d1, fib d2) (-102); a 0 lock is the volume's top */
u32 c_Examine(u32 lock, u8 *fib)
{
    int i = lock_of(lock);
    if ((lock && i < 0) || !fib || !c_TypeOfMem((u32)fib)) {
        Z.ioerr = ERROR_INVALID_LOCK;
        return 0;
    }
    fib_fill(fib, lock ? locks[i].kind : LK_VOL, lock ? locks[i].ent : 0);
    return (u32)-1;
}

/* ExNext(lock d1, fib d2) (-108): the entry after the one fib_DiskKey names (Examine leaves the directory's own key:
 * the first one then); ERROR_NO_MORE_ENTRIES at the end */
u32 c_ExNext(u32 lock, u8 *fib)
{
    int i = lock_of(lock);
    if ((lock && i < 0) || !fib || !c_TypeOfMem((u32)fib)) {
        Z.ioerr = ERROR_INVALID_LOCK;
        return 0;
    }
    u8 kind = lock ? locks[i].kind : LK_VOL;
    u32 dir = lock ? locks[i].ent : 0, key = *(u32 *)(fib + ZO_FileInfoBlock_fib_DiskKey);
    if (kind == LK_VOL) {
        const void *vol = (const void *)Z.a.vol;
        if (!zv_ent(vol, dir)->dir) {
            Z.ioerr = ERROR_OBJECT_WRONG_TYPE;
            return 0;
        }
        u32 after = key && key < ((const struct zv_hdr *)vol)->nent && zv_ent(vol, key)->parent == dir && key != dir ?
                    key : 0;
        u32 n = zv_next(vol, dir, after);
        if (n) {
            fib_fill(fib, LK_VOL, n);
            return (u32)-1;
        }
    } else if (kind == LK_RAMDIR) {     /* the memory files on RAM:'s top; the key their index + 1 */
        for (u32 r = key < NRAM ? key : NRAM; r < NRAM; r++) {
            const char *rest = ramf[r].name && !ramf[r].gone ? za_ram(ramf[r].name) : 0;
            int top = rest != 0;
            for (const char *c = rest; top && *c; c++)
                if (*c == '/')
                    top = 0;
            if (top) {
                fib_fill(fib, LK_RAM, r);
                *(u32 *)(fib + ZO_FileInfoBlock_fib_DiskKey) = r + 1;
                return (u32)-1;
            }
        }
    }
    Z.ioerr = ERROR_NO_MORE_ENTRIES;
    return 0;
}

/* Info(lock d1, infoData d2) (-114): the volume, write-protected (writes go to memory), its blocks of 512 bytes */
u32 c_Info(u32 lock, u32 *id)
{
    if ((lock && lock_of(lock) < 0) || !id || !c_TypeOfMem((u32)id)) {
        Z.ioerr = ERROR_INVALID_LOCK;
        return 0;
    }
    const struct zv_hdr *h = (const struct zv_hdr *)Z.a.vol;
    zmemset(id, 0, 36);
    id[2] = 80;                         /* id_DiskState: ID_WRITE_PROTECTED */
    id[3] = h->size / 512;
    id[4] = h->size / 512;
    id[5] = 512;
    id[6] = 0x444F5300;                 /* ID_DOS_DISK */
    return (u32)-1;
}

/* CurrentDir(lock d1) (-126): the old one back; 0 is the volume's top */
u32 c_CurrentDir(u32 lock)
{
    u32 old = *(u32 *)(Z.proc + ZO_Process_pr_CurrentDir);
    int i = lock_of(lock);
    if (!lock)
        cd_ent = 0;
    else if (i >= 0 && locks[i].kind == LK_RAMDIR)
        cd_ent = CD_RAM;
    else if (i >= 0 && locks[i].kind == LK_VOL && zv_ent((const void *)Z.a.vol, locks[i].ent)->dir)
        cd_ent = locks[i].ent;
    else
        return old;                     /* not a directory ZINE gave: nothing changes */
    *(u32 *)(Z.proc + ZO_Process_pr_CurrentDir) = lock;
    return old;
}

/* ParentDir(lock d1) (-210): the directory it is in; 0 (IoErr 0) for the volume's top */
u32 c_ParentDir(u32 lock)
{
    int i = lock_of(lock);
    Z.ioerr = 0;
    if (!lock || i < 0) {
        if (lock)
            Z.ioerr = ERROR_INVALID_LOCK;
        return 0;
    }
    if (locks[i].kind == LK_VOL)
        return locks[i].ent ? lock_new(LK_VOL, zv_ent((const void *)Z.a.vol, locks[i].ent)->parent, -2) : 0;
    if (locks[i].kind == LK_RAM && za_ram(ramf[locks[i].ent].name))
        return lock_new(LK_RAMDIR, 0, -2);
    return locks[i].kind == LK_RAM ? lock_new(LK_VOL, 0, -2) : 0;
}

/* the lock kind and entry of an open file */
static int fh_obj(u32 fh, u8 *kind, u32 *ent)
{
    int s = slot_of(fh);
    if (s < 0 || files[s].sink)
        return 0;
    *kind = files[s].ram ? LK_RAM : LK_VOL;
    *ent = files[s].ram ? files[s].ram - 1u : files[s].ent;
    return 1;
}

/* DupLockFromFH(fh d1) (-372), ParentOfFH(fh d1) (-384), ExamineFH(fh d1, fib d2) (-390) */
u32 c_DupLockFromFH(u32 fh)
{
    u8 k;
    u32 e;
    return fh_obj(fh, &k, &e) ? lock_new(k, e, -2) : 0;
}
u32 c_ParentOfFH(u32 fh)
{
    u32 l = c_DupLockFromFH(fh), p = l ? c_ParentDir(l) : 0;
    c_UnLock(l);
    return p;
}
u32 c_ExamineFH(u32 fh, u8 *fib)
{
    u8 k;
    u32 e;
    if (!fh_obj(fh, &k, &e) || !fib || !c_TypeOfMem((u32)fib)) {
        Z.ioerr = ERROR_OBJECT_WRONG_TYPE;
        return 0;
    }
    fib_fill(fib, k, e);
    return (u32)-1;
}

/* OpenFromLock(lock d1) (-378): the file opened, the lock taken over (gone) */
u32 c_OpenFromLock(u32 lock)
{
    int i = lock_of(lock);
    if (i < 0 || locks[i].kind == LK_RAMDIR ||
        (locks[i].kind == LK_VOL && zv_ent((const void *)Z.a.vol, locks[i].ent)->dir)) {
        Z.ioerr = i < 0 ? ERROR_INVALID_LOCK : ERROR_OBJECT_WRONG_TYPE;
        return 0;
    }
    u32 fh = locks[i].kind == LK_RAM ? fh_new(0, 0, (int)locks[i].ent + 1) : fh_new(locks[i].ent, 0, 0);
    if (fh)
        c_UnLock(lock);
    return fh;
}

/* NameFromLock(lock d1, buffer d2, len d3) (-402): "VOLUME:dir/name", a memory file by its path, "Ram Disk:" */
u32 c_NameFromLock(u32 lock, char *buf, u32 len)
{
    int i = lock_of(lock);
    if ((lock && i < 0) || !buf || !len) {
        Z.ioerr = ERROR_INVALID_LOCK;
        return 0;
    }
    char t[ZPATH];
    u32 n = 0;
    u8 kind = lock ? locks[i].kind : LK_VOL;
    if (kind == LK_VOL) {
        const void *vol = (const void *)Z.a.vol;
        u32 pos = ZPATH - 1;
        t[pos] = 0;
        for (u32 e = lock ? locks[i].ent : 0, d = 0; e && d < 64; e = zv_ent(vol, e)->parent, d++) {
            const struct zv_ent *en = zv_ent(vol, e);
            if (en->namelen + 1u > pos)
                goto long_;
            if (t[pos])
                t[--pos] = '/';
            pos -= en->namelen;
            zmemcpy(t + pos, en->name, en->namelen);
        }
        const char *vn = ((const struct zv_hdr *)vol)->volname;
        u32 vl = slen(vn);
        if (vl + 2 + (ZPATH - 1 - pos) > ZPATH)
            goto long_;
        char u[ZPATH];
        zmemcpy(u, vn, vl);
        u[vl] = ':';
        zmemcpy(u + vl + 1, t + pos, ZPATH - pos);
        n = slen(u);
        zmemcpy(t, u, n + 1);
    } else {
        const char *src = kind == LK_RAM ? ramf[locks[i].ent].name : "Ram Disk:";
        n = slen(src);
        if (n + 1 > ZPATH)
            goto long_;
        zmemcpy(t, src, n + 1);
    }
    if (n + 1 > len)
        goto long_;
    zmemcpy(buf, t, n + 1);
    return (u32)-1;
long_:
    Z.ioerr = 120;                      /* ERROR_LINE_TOO_LONG */
    if (len)
        buf[0] = 0;
    return 0;
}

/* SameLock(l1 d1, l2 d2) (-420): LOCK_SAME 0, LOCK_SAME_VOLUME 1, LOCK_DIFFERENT -1 */
s32 c_SameLock(u32 a, u32 b)
{
    int i = lock_of(a), j = lock_of(b);
    if (i < 0 || j < 0)
        return -1;
    if (locks[i].kind == locks[j].kind && locks[i].ent == locks[j].ent)
        return 0;
    return (locks[i].kind == LK_VOL) == (locks[j].kind == LK_VOL) ? 1 : -1;
}

/* ---- LoadSeg and overlays (docs/d29-zine.md 16.6; the Guru Book 17.1.76, 19.3.2.2, 22.2.10; the AmigaDOS Manual's
 * "hunk_overlay"). LoadSeg(name) loads a program of the volume. An overlaid one (its root, then HUNK_OVERLAY) is
 * loaded as AmigaOS leaves it for its overlay supervisor: the root's hunks, a hunk table (its header's table size +
 * 1 longwords, the root's segments in it, the rest 0), a copy of the overlay block from its third longword, and the
 * file kept open at the first byte after the block (where AmigaOS 1.x's loader, reading in order, stopped). With the
 * overlay mark $0000ABCD at the first hunk's offset 4, offsets 8, 12, 16 and 20 of its data get the handle (BPTR), the
 * table copy (APTR), the hunk table (BPTR) and the process's global vector. LoadSeg(NULL, table, handle) loads the
 * node at the handle's position into the table (zh_load_node) and leaves the handle after its HUNK_BREAK. The file
 * is the volume's copy (the whole program is in memory), so a node loads from memory. ---- */
#define NOVL 4                          /* overlaid programs at once (the program, and what it loads itself) */
static struct { u32 root, tab, tabn, ovl, ovlsize, fh, slot, ent, rootlast; } ovls[NOVL];
#define ERROR_FILE_NOT_OBJECT 121       /* dos/dos.h (NDK 3.2) */
#define ERROR_BAD_HUNK 235

static void say3(const char *a, const char *b, const char *c)
{
    zb_out(a, slen(a));
    zb_out(b, slen(b));
    zb_out(c, slen(c));
}

static void say_at(const char *a, u32 v, const char *c)
{
    char n[11];
    int i = 10;
    n[10] = 0;
    do
        n[--i] = (char)('0' + v % 10), v /= 10;
    while (v && i);
    say3(a, n + i, c);
}

static u32 unload(u32 seg)
{
    for (int k = 0; k < NOVL; k++)
        if (seg && ovls[k].root == seg) {
            u32 sl = ovls[k].slot;      /* closed only if the program has not closed it (the slot reused) */
            if (files[sl].used && files[sl].fh == ovls[k].fh << 2 && files[sl].ent == ovls[k].ent)
                c_Close(ovls[k].fh);
            zx_FreeMem(ovls[k].tab, ovls[k].tabn * 4);
            zx_FreeMem(ovls[k].ovl, ovls[k].ovlsize);
            ovls[k].root = 0;
        }
    for (u32 n = 0; seg && n < 65536; n++) {   /* bounded: a looped list ends here */
        u32 a = (seg << 2) - 4, next = *(u32 *)(a + 4);
        zx_FreeMem(a, *(u32 *)a);
        seg = next;
    }
    return 1;
}

int zb_ovl_root(u32 ent, const u8 *f, const struct zh_info *inf, const struct zh_seg *work, u32 seg,
                const char *name)
{
    int k = 0;
    while (k < NOVL && ovls[k].root)
        k++;
    if (k == NOVL) {
        say3("ZINE: ", name, " is a fifth overlaid program loaded at once: ZINE keeps 4\n");
        return -ZE_NOMEM;
    }
    u32 tabn = inf->tabsize + 1, osz = inf->ovl_words * 4;     /* tabsize <= ZH_MAXTAB: no wrap */
    u32 tab = zx_AllocMem(tabn * 4, MEMF_PUBLIC | MEMF_CLEAR);
    u32 ov = tab ? zx_AllocMem(osz, MEMF_PUBLIC) : 0;
    u32 fh = ov ? fh_new(ent, 0, 0) : 0;
    if (!fh) {
        if (ov)
            zx_FreeMem(ov, osz);
        if (tab)
            zx_FreeMem(tab, tabn * 4);
        say3("ZINE: ", name, ov ? "'s overlay file: no free file handle (ZINE has 16)\n"
                                : "'s overlay tables: not enough memory\n");
        return -ZE_NOMEM;
    }
    zh_table(inf, work, (zu8 *)tab, tabn);
    zmemcpy((void *)ov, f + inf->ovl, osz);
    files[*(u32 *)((fh << 2) + 36)].pos = inf->end;
    ovls[k].root = seg, ovls[k].tab = tab, ovls[k].tabn = tabn, ovls[k].ovl = ov, ovls[k].ovlsize = osz;
    ovls[k].fh = fh, ovls[k].slot = *(u32 *)((fh << 2) + 36), ovls[k].ent = ent, ovls[k].rootlast = inf->last;
    u32 code = work[0].addr + 8;
    if (work[0].size >= 8 + 24 && *(u32 *)(code + 4) == 0xABCDu) {
        *(u32 *)(code + 8) = fh;
        *(u32 *)(code + 12) = ov;
        *(u32 *)(code + 16) = tab >> 2;
        *(u32 *)(code + 20) = *(u32 *)((u32)Z.proc + ZO_Process_pr_GlobVec);
    } else
        say3("ZINE: ", name, " has overlays but no overlay header ($0000ABCD at its start + 4): its overlay "
             "handle and tables were not written into it\n");
    return 0;
}

u32 c_LoadSeg(const char *name, u32 tab, u32 fh)
{
    const void *vol = (const void *)Z.a.vol;
    u32 ent = 0, start = 0, fsize = 0;
    const zu8 *f = 0;
    int i = -1, k = 0;
    if (name) {                         /* a file of the volume or of RAM: (16.8), assigns followed */
        if (file_bytes(name, &f, &fsize, &ent)) {
            say3("ZINE: no file ", name, "\n");
            Z.ioerr = 205;              /* ERROR_OBJECT_NOT_FOUND */
            return 0;
        }
    } else {
        while (k < NOVL && !(ovls[k].root && ovls[k].tab >> 2 == tab))
            k++;
        i = slot_of(fh);
        if (k == NOVL || i < 0 || files[i].sink) {
            say3("ZINE: ", Z.blk->name, " loads an overlay part with a hunk table or file ZINE did not give it\n");
            Z.ioerr = ERROR_BAD_HUNK;
            return 0;
        }
        ent = files[i].ent, start = files[i].pos;
        const struct zv_ent *e = zv_ent(vol, ent);
        f = (const zu8 *)vol + e->off, fsize = e->size;
    }
    const char *who = name ? name : Z.blk->name;
    struct zh_info inf;
    char msg[160];
    if (name ? zh_check(f, fsize, who, &inf, msg, sizeof msg) : zh_node(f, fsize, start, who, &inf, msg, sizeof msg)) {
        say3("ZINE: ", msg, "");
        if (!name)
            say_at(" (the overlay part at byte ", start, ")");
        say3("\n", "", "");
        Z.ioerr = name ? ERROR_FILE_NOT_OBJECT : ERROR_BAD_HUNK;
        return 0;
    }
    u32 wsize = inf.nhunks * sizeof(struct zh_seg) + 8, seg = 0;
    struct zh_seg *work = (struct zh_seg *)zx_AllocMem(wsize, MEMF_PUBLIC);
    struct zh_ops ops = { zb_hunk_alloc, zb_hunk_free, 0 };
    if (name && inf.ovl && ent == ~0u) {    /* the overlay handle is a volume file's: not a memory file's */
        say3("ZINE: ", name, " is overlaid: ZINE loads overlaid programs from the program's own files, not from ones it wrote\n");
        if (work)
            zx_FreeMem((u32)work, wsize);
        Z.ioerr = ERROR_FILE_NOT_OBJECT;
        return 0;
    }
    int r = !work ? -ZE_NOMEM : name ? zh_load(f, fsize, &inf, &ops, work, &seg)
          : inf.first <= ovls[k].rootlast ? -ZE_NOEXEC       /* never over the root's own slots */
          : zh_load_node(f, fsize, &inf, &ops, work, (zu8 *)ovls[k].tab, ovls[k].tabn, &seg);
    if (!r && name && inf.ovl && zb_ovl_root(ent, f, &inf, work, seg, name)) {
        unload(seg);
        if (work)
            zx_FreeMem((u32)work, wsize);
        Z.ioerr = 103;
        return 0;                       /* zb_ovl_root said why */
    }
    if (work)
        zx_FreeMem((u32)work, wsize);
    if (r) {
        u32 need = inf.bytes[0] + inf.bytes[1] + inf.bytes[2];
        if (r == -ZE_NOMEM)
            say_at("ZINE: not enough memory to load ", need, " bytes of ");
        else
            say3("ZINE: cannot load ", "", "");
        say3(who, name ? "" : "'s overlay part", "");
        if (!name)
            say_at(" at byte ", start, "");
        say3(r == -ZE_NOMEM ? "\n" : " (it relocates to a hunk not loaded, or lies outside its hunk table or over "
             "the root's hunks)\n", "", "");
        Z.ioerr = r == -ZE_NOMEM ? 103 : ERROR_BAD_HUNK;
        return 0;
    }
    if (!name)
        files[i].pos = inf.end;
    extern void zx_CacheClearU(void);
    zx_CacheClearU();                   /* code was written (CLAUDE.md) */
    return seg;
}

u32 c_UnLoadSeg(u32 seg)
{
    return seg ? unload(seg) : 0;
}

u32 c_Write(u32 fh, const char *buf, s32 n)
{
    int fs = slot_of(fh);
    if (fs >= 0 && n >= 0) {
        if (files[fs].ram) {            /* a memory file keeps it (16.8, 16.9) */
            u32 k = ram_write(files[fs].ram - 1, files[fs].pos, buf, (u32)n);
            if (k != (u32)-1)
                files[fs].pos += k;
            return k;
        }
        if (files[fs].sink)
            return (u32)n;              /* NIL: */
        Z.ioerr = 223;                  /* ERROR_WRITE_PROTECTED: a volume file opened to be read */
        return (u32)-1;
    }
    if (fh != Z.out_fh || n < 0) {
        Z.ioerr = 114;                  /* ERROR_OBJECT_WRONG_TYPE: not a handle ZINE gave */
        return (u32)-1;
    }
    return zb_out(buf, (u32)n);
}



/* a program's text (a command, a name) in the output: at most 100 bytes, up to its 0, from the program's own memory
 * only; a byte outside the printable ones as '?'. 0 when p is no text ZINE can read (outside the program's memory) */
int zb_say_text(const char *pre, u32 p, const char *post)
{
    if (!p || !c_TypeOfMem(p))
        return 0;
    zb_out(pre, slen(pre));
    const u8 *t = (const u8 *)p;
    u32 n = 0;
    for (; n < 100 && t[n] && c_TypeOfMem(p + n); n++) {
        char c = t[n] >= 32 && t[n] < 127 ? (char)t[n] : t[n] >= 160 ? (char)t[n] : '?';
        zb_out(&c, 1);
    }
    if (n == 100 && t[n])
        zb_out("...", 3);
    zb_out(post, slen(post));
    return 1;
}

/* ---- dos Execute (-222; docs/d29-zine.md 16.7, 16.8, 16.11). ZINE has no shell. A command that names a program
 * file (the program's folder and its subfolders, C:, the assigns; any case) is run for real, on the same task as the
 * Shell would (synchronously, also after "run": ZINE runs one task): loaded, its arguments in a0/d0, its own stack
 * (a "$STACK:" cookie in the file, else the CLI's default), pr_CLI's fields its own, its Input() and Output() the
 * redirections, unloaded after; Execute answers DOSTRUE and the return code is IoErr() and cli_ReturnCode (cli_Result2
 * the command's IoErr). An Assign ZINE does itself (zpath.h). A command of AmigaOS's Shell or C: (zx_known) is not run
 * and answered as done (demos Execute side jobs: "run >nil: setpatch", "stack"); a name that is no file: DOSFALSE and
 * ERROR_OBJECT_NOT_FOUND, as the autodoc's "successful in finding and starting the specified program". ---- */
#define ZX_DEPTH 8                      /* commands running inside one another at once */
#define ZX_STACKMIN 8192u               /* ZINE's own calls run on the program's stack too */
#define ZX_STACKMAX (1u << 20)
u32 zb_segsp, zb_depth;                 /* blobs.S: the stack of the innermost command's caller; commands running */
u32 zb_runseg(u32 pc, u32 usp, u32 d0, u32 a0, u32 a3);    /* blobs.S: d0 the command's return code */
extern const u8 zb_segret[];
static const char *cur_name;            /* the program now running (the one whose Execute is answered) */

static void say_n(const char *t, u32 n)     /* n bytes of a text in the program's memory, at most 100 */
{
    for (u32 i = 0; i < n && i < 100; i++) {
        u8 b = (u8)t[i];
        char c = b >= 32 && b < 127 ? (char)b : b >= 160 ? (char)b : '?';
        zb_out(&c, 1);
    }
    if (n > 100)
        zb_out("...", 3);
}

/* a "$STACK:n" cookie in the program's file (the form later Shells read): n bytes, else 0 */
static u32 stack_cookie(const zu8 *f, u32 n)
{
    static const char k[] = "$STACK:";
    for (u32 i = 0; i + 8 <= n; i++) {
        u32 j = 0;
        while (j < 7 && f[i + j] == (zu8)k[j])
            j++;
        if (j < 7)
            continue;
        u32 v = 0, d = 0;
        for (i += 7; i < n && d < 8 && f[i] >= '0' && f[i] <= '9'; i++, d++)
            v = v * 10 + (f[i] - '0');
        return d ? v : 0;
    }
    return 0;
}

/* the process's fields a command gets its own of, saved and put back around it */
static const u16 pofs[] = { ZO_Process_pr_Task + ZO_Task_tc_SPLower, ZO_Process_pr_Task + ZO_Task_tc_SPUpper,
    ZO_Process_pr_Task + ZO_Task_tc_SPReg, ZO_Process_pr_StackSize, ZO_Process_pr_StackBase, ZO_Process_pr_ReturnAddr,
    ZO_Process_pr_Arguments, ZO_Process_pr_CIS, ZO_Process_pr_COS };
static const u8 cofs[] = { ZO_CommandLineInterface_cli_Module, ZO_CommandLineInterface_cli_CommandName,
    ZO_CommandLineInterface_cli_CurrentInput, ZO_CommandLineInterface_cli_CurrentOutput };
#define NPOFS (sizeof pofs / sizeof pofs[0])
#define NCOFS (sizeof cofs / sizeof cofs[0])

/* the program file c names, run (found as where): DOSTRUE with its return code in *rc, or DOSFALSE (said) */
static u32 run_file(const struct zx_cmd *c, const char *line, const char *where, u32 out, u32 *rc)
{
    const zu8 *f;
    u32 fsize, ent;
    u8 *pr = (u8 *)Z.proc;
    u32 cli = *(u32 *)(pr + ZO_Process_pr_CLI) << 2;
    u32 size = file_bytes(where, &f, &fsize, &ent) ? 0 : stack_cookie(f, fsize);
    if (!size)
        size = *(u32 *)(cli + ZO_CommandLineInterface_cli_DefaultStack) * 4;
    size = size < ZX_STACKMIN ? ZX_STACKMIN : size > ZX_STACKMAX ? ZX_STACKMAX : (size + 3) & ~3u;
    u32 seg = c_LoadSeg(where, 0, 0);   /* says why when it cannot */
    if (!seg)
        return 0;
    u32 fin = 0, fout = 0;
    if (c->in[0] && !(fin = c_Open(c->in, MODE_OLDFILE)))
        say3("ZINE: the command's input ", c->in, " cannot be opened\n");
    else if (c->out[0] && !(fout = c_Open(c->out, c->append ? MODE_READWRITE : MODE_NEWFILE)))
        say3("ZINE: the command's output ", c->out, " cannot be opened\n");
    u32 sp = (!c->in[0] || fin) && (!c->out[0] || fout) ? zx_AllocMem(size, MEMF_PUBLIC) : 0;
    u32 blk = sp ? zx_AllocMem(256 + c->arglen + 2, MEMF_PUBLIC | MEMF_CLEAR) : 0;
    if (!blk) {
        if (sp)
            zx_FreeMem(sp, size);
        if ((!c->in[0] || fin) && (!c->out[0] || fout))
            say_at("ZINE: no memory for the command's stack of ", size, " bytes\n");
        if (fin)
            c_Close(fin);
        if (fout)
            c_Close(fout);
        c_UnLoadSeg(seg);
        Z.ioerr = 103;                  /* ERROR_NO_FREE_STORE */
        return 0;
    }
    if (fout && c->append)
        c_Seek(fout, 0, 1);             /* ">>": at the end (OFFSET_END) */
    u8 *bs = (u8 *)blk;                 /* the command's name as a BSTR, then its arguments ending in "\n" */
    u32 nl = slen(c->name) > 255 ? 255 : slen(c->name);
    bs[0] = (u8)nl;
    zmemcpy(bs + 1, c->name, nl);
    char *args = (char *)blk + 256;
    zmemcpy(args, line + c->args, c->arglen);
    args[c->arglen] = '\n';
    u32 sv[NPOFS + NCOFS + 4];
    for (u32 i = 0; i < NPOFS; i++)
        sv[i] = *(u32 *)(pr + pofs[i]);
    for (u32 i = 0; i < NCOFS; i++)
        sv[NPOFS + i] = *(u32 *)(cli + cofs[i]);
    sv[NPOFS + NCOFS] = Z.cur_in, sv[NPOFS + NCOFS + 1] = Z.cur_out;
    u32 cin = fin ? fin : c_Input(), cout = fout ? fout : out ? out : c_Output();
    Z.cur_in = cin == Z.in_fh ? 0 : cin, Z.cur_out = cout == Z.out_fh ? 0 : cout;
    /* PROGDIR: the command's own folder while it runs (where's part before its name) */
    int pi = -1;
    for (int i = 0; i < ZA_MAX; i++)
        if (assigns.a[i].name[0] && zv_same(assigns.a[i].name, slen(assigns.a[i].name), "PROGDIR"))
            pi = i;
    struct za_assign old;
    if (pi >= 0)
        zmemcpy(&old, &assigns.a[pi], sizeof old);
    char dir[ZA_PATHMAX + 1];
    u32 dl = 0;
    for (u32 i = 0; where[i]; i++)
        if (where[i] == '/' || where[i] == ':')
            dl = where[i] == ':' ? i + 1 : i;
    if (dl > ZA_PATHMAX)
        dl = 0;
    zmemcpy(dir, dl ? where : "SYS:", dl ? dl : 5);
    dir[dl ? dl : 4] = 0;
    const char *dp = dir;
    if (za_set(&assigns, "PROGDIR", 7, &dp, 1, 0))
        say3("ZINE: no room for PROGDIR: as the command's folder (ZINE keeps 8 assigns): it stays ", Z.blk->name,
             "'s\n");
    u32 usp = sp + size - 8;
    ((u32 *)usp)[0] = (u32)zb_segret;   /* the command's rts comes back here; 4(sp) the stack's size */
    ((u32 *)usp)[1] = size;
    *(u32 *)(pr + pofs[0]) = sp, *(u32 *)(pr + pofs[1]) = sp + size, *(u32 *)(pr + pofs[2]) = usp;
    *(u32 *)(pr + ZO_Process_pr_StackSize) = size;
    *(u32 *)(pr + ZO_Process_pr_StackBase) = (sp + size) >> 2;
    *(u32 *)(pr + ZO_Process_pr_ReturnAddr) = usp + 4;
    *(u32 *)(pr + ZO_Process_pr_Arguments) = (u32)args;
    *(u32 *)(pr + ZO_Process_pr_CIS) = cin, *(u32 *)(pr + ZO_Process_pr_COS) = cout;
    *(u32 *)(cli + ZO_CommandLineInterface_cli_Module) = seg;
    *(u32 *)(cli + ZO_CommandLineInterface_cli_CommandName) = blk >> 2;
    *(u32 *)(cli + ZO_CommandLineInterface_cli_CurrentInput) = cin;
    *(u32 *)(cli + ZO_CommandLineInterface_cli_CurrentOutput) = cout;
    const char *caller = cur_name;
    cur_name = c->name;
    u32 osp = zb_segsp;
    Z.ioerr = 0;
    zb_depth++;
    *rc = zb_runseg((seg << 2) + 4, usp, c->arglen + 1, (u32)args, seg << 2);
    zb_depth--;
    zb_segsp = osp;
    cur_name = caller;
    u32 res2 = Z.ioerr;
    for (u32 i = 0; i < NPOFS; i++)
        *(u32 *)(pr + pofs[i]) = sv[i];
    for (u32 i = 0; i < NCOFS; i++)
        *(u32 *)(cli + cofs[i]) = sv[NPOFS + i];
    Z.cur_in = sv[NPOFS + NCOFS], Z.cur_out = sv[NPOFS + NCOFS + 1];
    if (pi >= 0)
        zmemcpy(&assigns.a[pi], &old, sizeof old);
    else
        za_set(&assigns, "PROGDIR", 7, 0, 0, 0);
    c_UnLoadSeg(seg);
    zx_FreeMem(sp, size);
    zx_FreeMem(blk, 256 + c->arglen + 2);
    if (fin)
        c_Close(fin);
    if (fout)
        c_Close(fout);
    *(u32 *)(cli + ZO_CommandLineInterface_cli_ReturnCode) = *rc;
    *(u32 *)(cli + ZO_CommandLineInterface_cli_Result2) = res2;
    Z.ioerr = *rc;
    return (u32)-1;
}

/* one line of an Execute: DOSTRUE (done, run, or answered as done) or DOSFALSE; *stop when the rest is not run,
 * *next where the next line starts */
static u32 exec_line(const char *line, u32 n, u32 out, int *stop, u32 *next)
{
    struct zx_cmd c;
    int r = zx_parse(line, n, &c);
    *next = c.next ? c.next : n;
    u32 ln = c.next && line[c.next - 1] == '\n' ? c.next - 1 : c.next;
    if (!r)
        return (u32)-1;                 /* blanks, a comment */
    say3("ZINE: ", cur_name, " asked dos Execute to run \"");
    say_n(line, ln);
    say3("\"", "", "");
    if (r < 0) {
        say3(": a name or file longer than 95 bytes, or a < or > with no file: answered as failed\n", "", "");
        Z.ioerr = 120;                  /* ERROR_LINE_TOO_LONG */
        *stop = 1;
        return 0;
    }
    struct za_cmd ac;
    int a = za_parse(line + c.at, ln - c.at, &ac);
    if (a > 0 && !za_apply(&assigns, &ac)) {
        say3(": done by ZINE itself (", ac.name, ":");
        if (!ac.n)
            say3(" removed)\n", "", "");
        else {
            say3(ac.add ? " also " : " is now ", "", "");
            for (u32 i = 0; i < ac.n; i++)
                say3(i ? ", " : "", ac.dir[i], "");
            say3(")\n", "", "");
        }
        return (u32)-1;
    }
    if (a) {
        say3(a > 0 ? ": an Assign ZINE has no room for (8 names, 3 directories each), answered as done\n"
                   : ": an Assign ZINE does not do (it does NAME: DIR..., ADD and REMOVE), answered as done\n", "", "");
        return (u32)-1;
    }
    if (zx_known(c.name)) {
        say3(": not run (ZINE has no shell), answered as done\n", "", "");
        return (u32)-1;
    }
    char where[ZPATH];
    int found = zb_file_exists(c.name, where, sizeof where);
    int sep = 0;
    for (const char *s = c.name; *s; s++)
        sep |= *s == ':' || *s == '/';
    if (!found && !sep) {               /* a plain name: the current folder (the program's), then C: */
        char p[ZPATH];
        zmemcpy(p, "C:", 2);
        zmemcpy(p + 2, c.name, slen(c.name) + 1);
        found = zb_file_exists(p, where, sizeof where);
    }
    if (!found) {
        say3(": no program ", c.name, sep ? " (a Shell says \"" : " in its folder or C: (a Shell says \"");
        say3(c.name, ": Unknown command\"), answered as failed\n", "");
        Z.ioerr = 205;                  /* ERROR_OBJECT_NOT_FOUND */
        *stop = 1;
        return 0;
    }
    if (zb_depth >= ZX_DEPTH) {
        say3(": ZINE runs at most 8 commands inside one another, answered as failed\n", "", "");
        Z.ioerr = 103;
        *stop = 1;
        return 0;
    }
    say3(": runs ", where, c.run ? " (after run, but to its end before the program goes on: ZINE runs one task)\n"
                                 : "\n");
    u32 rc = 0;
    if (!run_file(&c, line, where, out, &rc)) {
        say3("ZINE: ", c.name, " not started: answered as failed\n");
        *stop = 1;
        return 0;
    }
    say3("ZINE: the command ", c.name, " ended with return code ");
    say_at("", rc, "; ");
    say3(cur_name, " goes on\n", "");
    if ((s32)rc >= (s32)*(u32 *)((*(u32 *)((u8 *)Z.proc + ZO_Process_pr_CLI) << 2) + ZO_CommandLineInterface_cli_FailLevel))
        *stop = 1;                      /* as the Shell: a return code at the fail level ends the commands */
    return (u32)-1;
}

u32 c_Execute(u32 cmd, u32 in, u32 out)
{
    if (!cur_name)
        cur_name = Z.blk->name;
    u32 n = 0;                          /* the text within the program's memory only */
    if (cmd && c_TypeOfMem(cmd))
        while (n < 512 && c_TypeOfMem(cmd + n) && ((const u8 *)cmd)[n])
            n++;
    if (!n) {
        say3("ZINE: ", cur_name, " asked dos Execute to run ");
        if (!zb_say_text("\"", cmd, "\""))
            say3("no text it could read", "", "");
        if (in)
            say3(" (its commands from a file)", "", "");
        say3(": not run (ZINE has no shell), answered as done\n", "", "");
        return (u32)-1;
    }
    u32 ok = (u32)-1;
    int stop = 0;
    for (u32 at = 0; at < n && !stop;) {
        u32 next;
        ok = exec_line((const char *)cmd + at, n - at, out, &stop, &next);
        at += next;
    }
    if (in && !stop)
        say3("ZINE: ", cur_name, "'s Execute names a file to read more commands from: not read (ZINE has no shell)\n");
    return ok;
}

/* dos CreateNewProc and CreateProc: btask.c (d29 16.16) */
u32 zb_ioerr_set(u32 e) { return Z.ioerr = e; }

u32 c_PutStr(const char *s)
{
    if (Z.cur_out)                      /* a command's redirected output (16.11) */
        return c_Write(Z.cur_out, s, (s32)slen(s)) == (u32)-1 ? (u32)-1 : 0;
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

struct fput { u32 fh, n, err; };
static void file_put(void *u, u32 c)
{
    struct fput *p = (struct fput *)u;
    char ch = (char)c;
    if (c_Write(p->fh, &ch, 1) == (u32)-1)
        p->err = 1;
    else
        p->n++;
}

u32 c_VFPrintf(u32 fh, const char *f, const u8 *argv)
{
    if (fh == Z.out_fh) {
        u32 before = Z.blk->outlen + Z.blk->outlost;
        fmt(f, argv, out_put, 0);
        return Z.blk->outlen + Z.blk->outlost - before;
    }
    if (slot_of(fh) < 0) {
        Z.ioerr = 114;
        return (u32)-1;
    }
    struct fput p = { fh, 0, 0 };       /* one of ZINE's files (a command's redirected output, 16.11) */
    fmt(f, argv, file_put, &p);
    return p.err ? (u32)-1 : p.n;
}

u32 c_VPrintf(const char *f, const u8 *argv)
{
    return c_VFPrintf(c_Output(), f, argv);
}

u32 c_Flush(u32 fh) { (void)fh; return 1; }

/* Delay: 50 ticks a second, counted in beam lines (313 a tick: 15,625 lines a second on PAL, 15,734 on NTSC) */
void c_Delay(u32 ticks)
{
    if (!zt_sleep(Z.pal ? ticks : ticks * 6 / 5))      /* more than one task: the caller sleeps (16.16) */
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
    if (zt_sleep(1))                    /* more than one task: the caller sleeps to the next frame (16.16) */
        return;
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

static int view_show(const u8 *view);
static void show_screen(u8 *sc);
static int lord_show(const u8 *view);
/* LoadView: a View of the program's own (16.13): its first ViewPort shown. Intuition's View (ViewAddress, 16.17):
 * the front screen again, from its RasInfo as it is now (The Juggler flips its HAM screen's BitMap and the View's
 * Copper lists and calls LoadView each frame), or a ViewPort of the program's own put there; else the blank display. */
void c_LoadView(u32 view)
{
    *(u32 *)(Z.lib[1] + ZO_GfxBase_ActiView) = view;
    if (view && view != IB_VIEWLORD(Z.lib[3]) && view_show((const u8 *)view))
        return;
    if (view && view == IB_VIEWLORD(Z.lib[3]) && lord_show((const u8 *)view))
        return;
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

/* graphics' bitmaps (the RKRM autodocs; offsets amigaofs.h): plain data, and planes in chip RAM */
void c_InitBitMap(u8 *bm, u32 depth, u32 width, u32 height)
{
    *(u16 *)(bm + ZO_BitMap_BytesPerRow) = (u16)(((width + 15) >> 4) * 2);
    *(u16 *)(bm + ZO_BitMap_Rows) = (u16)height;
    bm[ZO_BitMap_Flags] = 0;
    bm[ZO_BitMap_Depth] = (u8)depth;
    *(u16 *)(bm + ZO_BitMap_pad) = 0;
}

#define RASSIZE(w, h) ((u32)(h) * (((u32)(w) + 15) >> 3 & ~1u))
/* MEMF_CHIP, and cleared: AmigaOS's AllocRaster does not promise it, but after a boot its chip RAM is mostly zeros and
 * programs show rows they never draw; ZINE's chip RAM still holds Zenix's old data */
u32 c_AllocRaster(u32 w, u32 h) { return zx_AllocMem(RASSIZE(w, h), 2 | 1 << 16); }
void c_FreeRaster(u32 p, u32 w, u32 h) { if (p) zx_FreeMem(p, RASSIZE(w, h)); }

/* InitRastPort: the RKRM's defaults (all pens and pointers 0 but Mask 0xFF, FgPen and AOlPen 1, JAM2, a solid line
 * pattern; RastPort 100 bytes: Mask 24, FgPen 25, AOlPen 27, DrawMode 28, linpatcnt 30, Flags 32, LinePtrn 34) */
void c_InitRastPort(u8 *rp)
{
    zmemset(rp, 0, 100);
    rp[24] = 0xFF;
    rp[25] = 1;
    rp[27] = 1;
    rp[28] = 1;
    rp[30] = 15;
    *(u16 *)(rp + 32) = 1;              /* FRST_DOT */
    *(u16 *)(rp + 34) = 0xFFFF;
    zb_rp_font(rp);                     /* the default font, topaz 8 (16.12) */
}

/* AllocBitMap (V39): the struct and its planes (one chip block each, cleared with BMF_CLEAR or not: always cleared
 * here); the friend bitmap and the other flags are not used */
u32 c_AllocBitMap(u32 w, u32 h, u32 depth, u32 flags, u32 friend)
{
    (void)flags; (void)friend;
    if (depth > 8)
        return 0;
    u8 *bm = (u8 *)zx_AllocMem(ZO_BitMap_SIZEOF, 1 << 16);     /* MEMF_CLEAR */
    if (!bm)
        return 0;
    c_InitBitMap(bm, depth, w, h);
    for (u32 i = 0; i < depth; i++) {
        u32 p = zx_AllocMem(RASSIZE(w, h), 2 | 1 << 16);
        if (!p) {
            while (i--)
                zx_FreeMem(*(u32 *)(bm + ZO_BitMap_Planes + 4 * i), RASSIZE(w, h));
            zx_FreeMem((u32)bm, ZO_BitMap_SIZEOF);
            return 0;
        }
        *(u32 *)(bm + ZO_BitMap_Planes + 4 * i) = p;
    }
    return (u32)bm;
}

void c_FreeBitMap(u8 *bm)
{
    if (!bm)
        return;
    u32 w = *(u16 *)(bm + ZO_BitMap_BytesPerRow) * 8u, h = *(u16 *)(bm + ZO_BitMap_Rows);
    for (u32 i = 0; i < bm[ZO_BitMap_Depth]; i++)
        if (*(u32 *)(bm + ZO_BitMap_Planes + 4 * i))
            zx_FreeMem(*(u32 *)(bm + ZO_BitMap_Planes + 4 * i), RASSIZE(w, h));
    zx_FreeMem((u32)bm, ZO_BitMap_SIZEOF);
}

/* graphics' 32-bit colours (V39): LoadRGB32's table is {count << 16 | first, r, g, b ... , 0}; the top 8 bits of each
 * 32-bit component go to AGA's registers through BPLCON3's LOCT (the low nibbles), on other chips the top 4 bits.
 * The ViewPort is not looked at: ZINE's display is the hardware's (5.3) */
static void rgb32(u32 n, u32 r, u32 g, u32 b)
{
    if (n > 255)
        return;
    u16 hi = (u16)((r >> 28) << 8 | (g >> 28) << 4 | b >> 28);
    if (Z.aga) {
        u16 lo = (u16)((r >> 24 & 15) << 8 | (g >> 24 & 15) << 4 | (b >> 24 & 15));
        u16 bank = (u16)((n >> 5) << 13);
        BPLCON3 = bank;
        COLOR(n & 31) = hi;
        BPLCON3 = bank | 0x0200;
        COLOR(n & 31) = lo;
        BPLCON3 = 0;
    } else if (n < 32)
        COLOR(n) = hi;
}

void c_LoadRGB32(u32 vp, const u32 *t)
{
    (void)vp;
    while (t && *t) {
        u32 cnt = *t >> 16, first = *t & 0xFFFF;
        t++;
        for (u32 i = 0; i < cnt; i++, t += 3)
            rgb32(first + i, t[0], t[1], t[2]);
    }
}

void c_SetRGB32(u32 vp, u32 n, u32 r, u32 g, u32 b) { (void)vp; rgb32(n, r, g, b); }

/* ---- intuition's screens (5.5): a screen ZINE never shows. OpenScreenTagList gives the program a Screen with its
 * BitMap (planes in chip RAM, cleared, or the program's own with CUSTOMBITMAP / SA_BitMap), a RastPort on it and a
 * ViewPort with a RasInfo and a ColorMap; demos then put up their own Copper list. The offsets are the NDK 3.2's
 * intuition/screens.h, graphics/view.h and rastport.h (sizes 346, 40, 100) ---- */
#define SC_Width 12
#define SC_Height 14
#define SC_ViewPort 44
#define SC_RastPort 84
#define SC_BitMap 184
#define SC_SIZEOF 346
#define VP_ColorMap 4
#define VP_DWidth 24
#define VP_DHeight 26
#define VP_RasInfo 36
#define RP_BitMap 4
#define RP_Mask 24
#define RP_FgPen 25
#define RP_DrawMode 28
#define CM_Count 2
#define CM_ColorTable 4
#define ZSCR_EXTRA 84                   /* after the Screen: a flag, the RasInfo (12), the ColorMap (52), the Copper
                                         * list (+68), its plane pointers (+72), the program's own BitMap (+76), the
                                         * BitMap a Copper list chosen by LoadView shows (+80, 16.17) */

/* the screen shown (5.5, the screens' first form): its BitMap's planes from the top, the standard display window
 * (320 or 640 wide, 256 or 200 lines), lowres, hires, HAM or EHB from its mode; colours stay what the program set */
#define VP_Modes 32
static u16 vc_con2 = 0x0024, vc_con3 = 0x0C00, vc_con4 = 0x0011;   /* BPLCON2-4 as VideoControl leaves them */
static u8 vc_full;                      /* VTAG_FULLPALETTE */
static u32 shown_depth;                 /* plane pointers in the shown screen's Copper list */
/* the screens in depth order (16.17), as intuition keeps them: IntuitionBase FirstScreen, each Screen's NextScreen
 * (0). Intuition's View's ViewPort stays 0 (with it set, Elude's demos ask GetDisplayInfoData). Ephidrena's J waits for its screen to be the FirstScreen; The
 * Juggler shows its movie by LoadView of intuition's View once its text screen is closed. */
#define FIRST (*(u32 *)(Z.lib[3] + ZO_IntuitionBase_FirstScreen))
static void scr_unlink(u8 *sc)
{
    for (u32 *pp = &FIRST; *pp; pp = (u32 *)*pp)
        if (*pp == (u32)sc) {
            *pp = *(u32 *)sc;
            break;
        }
    *(u32 *)sc = 0;
}
static void scr_place(u8 *sc, int front)
{
    scr_unlink(sc);
    if (front) {
        *(u32 *)sc = FIRST;
        FIRST = (u32)sc;
    } else {
        u32 *pp = &FIRST;
        while (*pp)
            pp = (u32 *)*pp;
        *pp = (u32)sc;
    }
}

static void show_screen(u8 *sc)
{
    u8 *bm = *(u8 **)(sc + SC_SIZEOF + 76) ? *(u8 **)(sc + SC_SIZEOF + 76) : sc + SC_BitMap;
    u16 modes = *(u16 *)(sc + SC_ViewPort + VP_Modes);
    u32 depth = bm[ZO_BitMap_Depth], bpr = *(u16 *)(bm + ZO_BitMap_BytesPerRow);
    int hires = (modes & 0x8000) != 0;
    u32 shown = hires ? 80 : 40;
    u16 *c = *(u16 **)(sc + SC_SIZEOF + 68);
    if (!c) {
        c = (u16 *)zx_AllocMem(400, 2);
        if (!c)
            return;
        *(u16 **)(sc + SC_SIZEOF + 68) = c;
    }
    u16 *l = c;
    for (int i = 0; i < 16; i++)
        *l++ = (u16)(0x120 + 2 * i), *l++ = (u16)(i & 1 ? ZNULLSPR : 0);
    u32 rows = *(u16 *)(bm + ZO_BitMap_Rows), most = Z.pal ? 256 : 200;
    if (*(u16 *)(sc + SC_Height) && *(u16 *)(sc + SC_Height) < rows)
        rows = *(u16 *)(sc + SC_Height);
    if (modes & 0x0004)
        rows /= 2;                      /* an interlaced screen: two fields of half the rows each */
    if (!rows || rows > most)
        rows = most;
    *l++ = 0x8E, *l++ = 0x2C81;
    *l++ = 0x90, *l++ = (u16)(((0x2C + rows) << 8 & 0xFF00) | 0xC1);    /* the window's end: the screen's rows only */
    /* AGA, lowres, 320 shown, rows and planes on 8-byte boundaries: 64-bit fetches (FMODE 3, DDFSTOP $B0: the
     * knowledge base's aga4k note, proven on a real A1200), which 7 and 8 planes need; else 16-bit as OCS */
    int f3 = 0;
    if (Z.aga && !hires && !(bpr & 7)) {
        f3 = 1;
        for (u32 i = 0; i < depth && i < 8; i++)
            if (*(u32 *)(bm + ZO_BitMap_Planes + 4 * i) & 7)
                f3 = 0;
    }
    *l++ = 0x92, *l++ = hires ? 0x3C : 0x38;
    *l++ = 0x94, *l++ = hires ? 0xD4 : f3 ? 0xB0 : 0xD0;
    u16 con0 = (u16)(0x0200 | (depth & 7) << 12 | (depth == 8 ? 0x0010 : 0) | (hires ? 0x8000 : 0) |
                     (modes & 0x0800) | (modes & 0x0004));             /* HAM, LACE as the mode asks */
    *l++ = 0x100, *l++ = con0;
    *l++ = 0x102, *l++ = 0;
    *l++ = 0x104, *l++ = vc_con2;
    u16 mod = (u16)(bpr > shown ? bpr - shown : 0);
    *l++ = 0x108, *l++ = mod;
    *l++ = 0x10A, *l++ = mod;
    if (Z.aga) {
        *l++ = 0x1FC, *l++ = (u16)(f3 ? 3 : 0);
        *l++ = 0x106, *l++ = vc_con3;
        *l++ = 0x10C, *l++ = vc_con4;
    }
    *(u16 **)(sc + SC_SIZEOF + 72) = l;  /* where the plane pointers are: zb_screen_frame renews them */
    shown_depth = depth < 8 ? depth : 8;
    for (u32 i = 0; i < depth && i < 8; i++) {
        u32 p = *(u32 *)(bm + ZO_BitMap_Planes + 4 * i);
        *l++ = (u16)(0xE0 + 4 * i), *l++ = (u16)(p >> 16);
        *l++ = (u16)(0xE2 + 4 * i), *l++ = (u16)p;
    }
    *l++ = 0xFFFF, *l++ = 0xFFFE;
    Z.shown = (u32)sc;
    zb_screen_frame();                  /* the RasInfo's offsets and BitMap (16.13) */
    COP1LC = (u32)c;
    COPJMP1 = 0;
}

u32 c_OpenScreenTagList(const u8 *ns, const u32 *tags)
{
    u32 w = 320, h = Z.pal ? 256 : 200, depth = 1, custom = 0, modes = 0, behind = 0, id = ~0u;
    if (ns) {
        if (*(const s16 *)(ns + 4) > 0) w = (u32)*(const s16 *)(ns + 4);
        if (*(const s16 *)(ns + 6) > 0) h = (u32)*(const s16 *)(ns + 6);
        depth = *(const u16 *)(ns + 8);
        modes = *(const u16 *)(ns + 12);
        if (*(const u16 *)(ns + 14) & 0x0040)       /* CUSTOMBITMAP */
            custom = *(const u32 *)(ns + 28);
    }
    for (const u32 *t = tags; t; t += 2) {
        u32 tag = t[0], d = t[1];
        if (tag == 0)
            break;
        if (tag == 2) {                 /* TAG_MORE */
            t = (const u32 *)d - 2;
            if (!d)
                break;
            continue;
        }
        if (tag == 3) {                 /* TAG_SKIP */
            t += 2 * d;
            continue;
        }
        if (tag == 0x80000023 && (s32)d > 0) w = d;
        if (tag == 0x80000024 && (s32)d > 0) h = d;
        if (tag == 0x80000025) depth = d;
        if (tag == 0x8000002E) custom = d;
        if (tag == 0x80000032) modes = id = d;      /* SA_DisplayID: the mode bits ViewPort.Modes has too */
        if (tag == 0x80000037) behind = d;          /* SA_Behind */
    }
    extern u32 zb_tracing;
    if (zb_tracing) {                   /* the trace build: what was asked, in the program's output */
        char t[64];
        static const char hx[] = "0123456789abcdef";
        u32 v[5] = { w, h, depth, modes, custom }, n = 0;
        static const char lead[] = "ZINE: screen";
        zmemcpy(t, lead, sizeof lead - 1);
        n = sizeof lead - 1;
        for (int i = 0; i < 5; i++) {
            t[n++] = ' ';
            for (int k = 0; k < 8; k++)
                t[n++] = hx[v[i] >> (28 - 4 * k) & 15];
        }
        t[n++] = '\n';
        zb_out(t, n);
    }
    if (depth < 1 || depth > 8)
        depth = depth > 8 ? 8 : 1;
    u8 *sc = (u8 *)zx_AllocMem(SC_SIZEOF + ZSCR_EXTRA, 1 << 16);
    if (!sc)
        return 0;
    u8 *bm = sc + SC_BitMap;
    if (custom) {
        zmemcpy(bm, (const void *)custom, ZO_BitMap_SIZEOF);
        *(u32 *)(sc + SC_SIZEOF + 76) = custom;    /* shown from the program's own struct: its planes may come later */
    }
    else {
        c_InitBitMap(bm, depth, w, h);
        for (u32 i = 0; i < depth; i++) {
            u32 p = zx_AllocMem(RASSIZE(w, h), 2 | 1 << 16);
            if (!p) {
                while (i--)
                    zx_FreeMem(*(u32 *)(bm + ZO_BitMap_Planes + 4 * i), RASSIZE(w, h));
                zx_FreeMem((u32)sc, SC_SIZEOF + ZSCR_EXTRA);
                return 0;
            }
            *(u32 *)(bm + ZO_BitMap_Planes + 4 * i) = p;
        }
    }
    *(u16 *)(sc + SC_Width) = (u16)w;
    *(u16 *)(sc + SC_Height) = (u16)h;
    *(u16 *)(sc + SC_ViewPort + VP_DWidth) = (u16)w;
    *(u16 *)(sc + SC_ViewPort + VP_DHeight) = (u16)h;
    u8 *ri = sc + SC_SIZEOF + 2, *cm = ri + 12;
    if (custom)
        bm = (u8 *)custom;
    *(u32 *)(ri + 4) = (u32)bm;
    *(u32 *)(sc + SC_ViewPort + VP_RasInfo) = (u32)ri;
    *(u16 *)(cm + CM_Count) = (u16)(1u << (depth < 5 ? depth : 5));
    cm[1] = 2;                          /* cm_Type COLORMAP_TYPE_V39: cm_vp (20) and cm_VPModeID (36) hold (16.13) */
    *(u32 *)(cm + 20) = (u32)(sc + SC_ViewPort);
    *(u32 *)(cm + 36) = id != ~0u ? id : zb_mode_id(modes);
    *(u32 *)(sc + SC_ViewPort + VP_ColorMap) = (u32)cm;
    *(u32 *)(sc + SC_RastPort + RP_BitMap) = (u32)bm;
    sc[SC_RastPort + RP_Mask] = 0xFF;
    sc[SC_RastPort + RP_FgPen] = 1;
    sc[SC_RastPort + RP_DrawMode] = 1;  /* JAM2 */
    zb_rp_font(sc + SC_RastPort);
    sc[SC_SIZEOF] = custom ? 1 : 0;     /* CloseScreen frees only planes it allocated */
    *(u16 *)(sc + SC_ViewPort + VP_Modes) = (u16)modes;
    scr_place(sc, !behind);
    if (!behind)
        show_screen(sc);
    return (u32)sc;
}

u32 c_OpenScreen(const u8 *ns) { return c_OpenScreenTagList(ns, 0); }

void zb_screen_report(void);
u32 c_CloseScreen(u8 *sc)
{
    if (!sc)
        return 0;
    u8 *bm = sc + SC_BitMap;
    if (!sc[SC_SIZEOF]) {
        u32 w = *(u16 *)(bm + ZO_BitMap_BytesPerRow) * 8u, h = *(u16 *)(bm + ZO_BitMap_Rows);
        for (u32 i = 0; i < bm[ZO_BitMap_Depth]; i++)
            if (*(u32 *)(bm + ZO_BitMap_Planes + 4 * i))
                zx_FreeMem(*(u32 *)(bm + ZO_BitMap_Planes + 4 * i), RASSIZE(w, h));
    }
    extern u32 zb_tracing;
    if (zb_tracing && Z.shown == (u32)sc)
        zb_screen_report();
    scr_unlink(sc);
    if (Z.shown == (u32)sc) {           /* the next screen shown, as intuition does (16.17), or the blank display */
        zb_blank_view();
        Z.shown = 0;
        if (FIRST)
            show_screen((u8 *)FIRST);
    }

    if (*(u32 *)(sc + SC_SIZEOF + 68))
        zx_FreeMem(*(u32 *)(sc + SC_SIZEOF + 68), 400);
    zx_FreeMem((u32)sc, SC_SIZEOF + ZSCR_EXTRA);
    return 1;
}

void c_ScreenToFront(u8 *sc)
{
    if (!sc)
        return;
    scr_place(sc, 1);
    show_screen(sc);
}
/* ScreenToBack(screen a0) (-246, 16.17): behind the others; the one in front shown */
void c_ScreenToBack(u8 *sc)
{
    if (!sc)
        return;
    scr_place(sc, 0);
    if (FIRST && FIRST != Z.shown)
        show_screen((u8 *)FIRST);
}

/* ---- d29 16.13: the display calls the Top 100 asked for next. RethinkDisplay (-390), RemakeDisplay (-384),
 * MakeScreen(screen a0) (-378), graphics MrgCop(view a1) (-210) and MakeVPort(view a0, vp a1) (-216): the screen ZINE
 * shows is shown again from its BitMap, mode and RasInfo (as AmigaOS rebuilds its Copper lists); MoveScreen(screen
 * a0, dx d0, dy d1) (-162): ZINE's screens stay at the top; ScrollVPort(vp a0) (-588): the RasInfo's offsets and
 * BitMap taken at once (zb_screen_frame takes them every frame too). ---- */
/* RethinkDisplay and RemakeDisplay (16.17): intuition's View gets Copper lists as AmigaOS's do, so a program can keep
 * them: when the program has cleared the View's LOFCprList, a new record of ZINE's (a cprlist: cp_Next 0, start 4,
 * MaxCount 8; then "ZCPR", the screen and the BitMap its RasInfo shows now) is put there, else the record there is
 * renewed. LoadView of the View with such a record shows that BitMap (The Juggler's double buffer). */
#define ZCPR 0x5A435052u
static void cpr_record(void)
{
    u32 f = FIRST, v = IB_VIEWLORD(Z.lib[3]);
    u32 *c = *(u32 **)(v + ZO_View_LOFCprList);
    if (!f)
        return;
    if (!c) {
        c = (u32 *)zx_AllocMem(24, MEMF_PUBLIC | MEMF_CLEAR);
        if (!c)
            return;
        c[1] = ZBLANK;
        c[3] = ZCPR;
        *(u32 **)(v + ZO_View_LOFCprList) = c;
    }
    if (c_TypeOfMem((u32)c) && c[3] == ZCPR) {
        const u8 *ri = *(const u8 *const *)((u8 *)f + SC_ViewPort + VP_RasInfo);
        c[4] = f;
        c[5] = ri && c_TypeOfMem((u32)ri) ? *(const u32 *)(ri + 4) : 0;
    }
}

/* LoadView of intuition's View (16.17): the front screen, showing the BitMap of the Copper list record the View has
 * (cpr_record), else its RasInfo's (taken at the next vertical blank); a ViewPort of the program's own put there
 * shown as a View of its own; 1 when something is shown */
static int lord_show(const u8 *view)
{
    u32 vp = *(const u32 *)view, f = FIRST;
    const u32 *c = *(const u32 *const *)(view + ZO_View_LOFCprList);
    if (f && (!vp || vp == f + SC_ViewPort)) {
        u32 bm = c && c_TypeOfMem((u32)c) && c[3] == ZCPR && c[4] == f ? c[5] : 0;
        *(u32 *)(f + SC_SIZEOF + 80) = bm;
        if (Z.shown != f)
            show_screen((u8 *)f);
        return 1;
    }
    return vp && view_show(view);
}

/* graphics FreeCprList(cprlist a0) (-564): a record of ZINE's freed; any other (the View's first one) kept */
void c_FreeCprList(u32 *c)
{
    if (c && !((u32)c & 1) && c_TypeOfMem((u32)c) && c[3] == ZCPR) {
        u32 v = IB_VIEWLORD(Z.lib[3]);
        if (*(u32 **)(v + ZO_View_LOFCprList) == c)
            *(u32 **)(v + ZO_View_LOFCprList) = 0;
        c[3] = 0;
        zx_FreeMem((u32)c, 24);
    }
}

u32 c_RethinkDisplay(void)
{
    if (Z.shown) {
        *(u32 *)((u8 *)Z.shown + SC_SIZEOF + 80) = 0;
        show_screen((u8 *)Z.shown);
    }
    cpr_record();
    return 0;
}
void c_MakeScreen(u8 *sc)
{
    if (sc && (u32)sc == Z.shown) {
        *(u32 *)(sc + SC_SIZEOF + 80) = 0;  /* its RasInfo's BitMap again (16.17) */
        show_screen(sc);
    }
}
void c_MoveScreen(u8 *sc, s32 dx, s32 dy) { (void)sc, (void)dx, (void)dy; }

/* a View of the program's own (graphics' InitView/InitVPort/MakeVPort/MrgCop/LoadView, 16.13): its first ViewPort shown
 * as ZINE shows a screen, from a screen of ZINE's own made for it (the ViewPort copied in: its RasInfo, BitMap,
 * Modes, DHeight; the RasInfo's offsets and BitMap taken every frame), its ColorMap's colours loaded; 0 when the View
 * has no ViewPort with a BitMap (the display blanked) */
static u8 *vscr;                        /* the screen made for a View */
static const u8 *vsrc;                  /* the ViewPort it shows */
static int view_show(const u8 *view)
{
    const u8 *vp = c_TypeOfMem((u32)view) ? *(const u8 *const *)view : 0;
    const u8 *ri = vp && c_TypeOfMem((u32)vp) ? *(const u8 *const *)(vp + VP_RasInfo) : 0;
    const u8 *bm = ri && c_TypeOfMem((u32)ri) ? *(const u8 *const *)(ri + 4) : 0;
    if (!bm || !c_TypeOfMem((u32)bm))
        return 0;
    if (!vscr && !(vscr = (u8 *)zx_AllocMem(SC_SIZEOF + ZSCR_EXTRA, MEMF_PUBLIC | MEMF_CLEAR)))
        return 0;
    zmemcpy(vscr + SC_ViewPort, vp, 40);
    *(u32 *)(vscr + SC_SIZEOF + 76) = (u32)bm;
    *(u16 *)(vscr + SC_Height) = *(const u16 *)(vp + VP_DHeight);
    vscr[SC_SIZEOF] = 1;                /* its BitMap is the program's */
    vsrc = vp;
    const u8 *cm = *(const u8 *const *)(vp + VP_ColorMap);
    const u16 *ct = cm && c_TypeOfMem((u32)cm) ? *(const u16 *const *)(cm + CM_ColorTable) : 0;
    if (ct && c_TypeOfMem((u32)ct))
        for (u32 i = 0; i < *(const u16 *)(cm + CM_Count) && i < 32; i++)
            COLOR(i) = ct[i];
    show_screen(vscr);
    return 1;
}
void c_MakeVPort(u32 view, u8 *vp)
{
    (void)view;
    if (vp && Z.shown && vp == (u8 *)Z.shown + SC_ViewPort)
        show_screen((u8 *)Z.shown);
}
void c_MrgCop(u32 view)
{
    if (Z.shown && (u32)vscr == Z.shown && view == *(u32 *)(Z.lib[1] + ZO_GfxBase_ActiView))
        view_show((const u8 *)view);
    else
        c_RethinkDisplay();
}
void c_ScrollVPort(u8 *vp)
{
    if (vp && Z.shown && (vp == (u8 *)Z.shown + SC_ViewPort || ((u32)vscr == Z.shown && vp == vsrc)))
        zb_screen_frame();
}

/* graphics' ColorMaps and ViewPorts (16.13): GetColorMap(entries d0) (-570) a V39 ColorMap (cm_Type 2) with its
 * colour table (12-bit) and the low nibbles' table, FreeColorMap(cm a0) (-576), GetRGB4(cm a0, entry d0) (-582),
 * SetRGB4CM(cm a0, n d0, r d1, g d2, b d3) (-630), GetRGB32(cm a0, first d0, n d1, table a1) (-900), SetRGB32CM(cm
 * a0, n d0, r d1, g d2, b d3) (-996); InitVPort(vp a0) (-204), InitView(view a1) (-360) cleared as the autodocs say */
u32 c_GetColorMap(u32 n)
{
    if (n > 256)
        n = 256;
    u8 *cm = (u8 *)zx_AllocMem(52 + 4 * n, MEMF_PUBLIC | MEMF_CLEAR);
    if (!cm)
        return 0;
    cm[1] = 2;
    *(u16 *)(cm + CM_Count) = (u16)n;
    *(u32 *)(cm + CM_ColorTable) = (u32)(cm + 52);
    *(u32 *)(cm + 12) = (u32)(cm + 52 + 2 * n);     /* LowColorBits */
    *(u32 *)(cm + 36) = ~0u;                        /* VPModeID: none yet */
    return (u32)cm;
}
void c_FreeColorMap(u8 *cm)
{
    if (cm && c_TypeOfMem((u32)cm) && cm[1] == 2 && *(u32 *)(cm + CM_ColorTable) == (u32)(cm + 52))
        zx_FreeMem((u32)cm, 52 + 4u * *(u16 *)(cm + CM_Count));
}
static u16 *cm_tab(const u8 *cm, u32 i, int low)
{
    if (!cm || !c_TypeOfMem((u32)cm) || i >= *(const u16 *)(cm + CM_Count))
        return 0;
    u16 *t = *(u16 *const *)(cm + (low ? 12 : CM_ColorTable));
    return t && c_TypeOfMem((u32)t) && (!low || cm[1] >= 2) ? t + i : 0;
}
s32 c_GetRGB4(const u8 *cm, u32 i)
{
    u16 *t = cm_tab(cm, i, 0);
    return t ? (s32)(*t & 0xFFF) : -1;
}
void c_SetRGB4CM(u8 *cm, u32 i, u32 r, u32 g, u32 b)
{
    u16 *t = cm_tab(cm, i, 0), *lo = cm_tab(cm, i, 1);
    if (t)
        *t = (u16)((r & 15) << 8 | (g & 15) << 4 | (b & 15));
    if (lo && t)
        *lo = *t;
}
void c_SetRGB32CM(u8 *cm, u32 i, u32 r, u32 g, u32 b)
{
    u16 *t = cm_tab(cm, i, 0), *lo = cm_tab(cm, i, 1);
    if (t)
        *t = (u16)((r >> 20 & 0xF00) | (g >> 24 & 0xF0) | (b >> 28));
    if (lo)
        *lo = (u16)((r >> 16 & 0xF00) | (g >> 20 & 0xF0) | (b >> 24 & 0xF));
}
void c_GetRGB32(const u8 *cm, u32 first, u32 n, u32 *out)
{
    for (u32 k = 0; out && c_TypeOfMem((u32)out) && k < n && k < 256; k++) {
        u16 *t = cm_tab(cm, first + k, 0), *lo = cm_tab(cm, first + k, 1);
        u32 hi = t ? *t : 0, lw = lo ? *lo : hi;
        for (int c = 0; c < 3; c++) {
            u32 v = (hi >> (8 - 4 * c) & 15) << 4 | (lw >> (8 - 4 * c) & 15);
            out[3 * k + c] = v * 0x01010101u;
        }
    }
}
void c_InitVPort(u8 *vp)
{
    if (vp && c_TypeOfMem((u32)vp))
        zmemset(vp, 0, 40);
}
void c_InitView(u8 *v)
{
    if (!v || !c_TypeOfMem((u32)v))
        return;
    zmemset(v, 0, 18);
    *(s16 *)(v + 12) = 0x2C;            /* DyOffset, DxOffset: the standard display's top left */
    *(s16 *)(v + 14) = 0x81;
}

/* VideoControl(colorMap a0, tags a1) (-708; graphics/videocontrol.h): the tags ZINE's display has (border blank and
 * border sprites, the sprite resolution, the playfields' priority against the sprites, the sprites' colour bases:
 * BPLCON2-4, from the next time the screen is shown, at once when the ColorMap is the shown screen's), the ColorMap's
 * ViewPort and mode ID, VTAG_IMMEDIATE's flag cleared (everything is immediate); a _GET tag's answer in its own
 * ti_Data. Genlock tags (chroma key, bitplane key) are taken and change nothing. 0: no error. */
static u32 pf2of(u32 base)              /* BPLCON3's PF2OF field for a colour base 0, 2, 4 ... 128 */
{
    u32 k = 0;
    while (k < 7 && (2u << k) <= base)
        k++;
    return base ? k : 0;
}
u32 c_VideoControl(u8 *cm, u32 *tags)
{
    int ok = cm && c_TypeOfMem((u32)cm), changed = 0;
    u32 *t = tags, *ti;
    for (u32 n = 0; n < 256 && (ti = (u32 *)c_NextTagItem(&t)); n++) {
        if (!c_TypeOfMem((u32)ti))
            break;
        u32 d = ti[1];
        u16 c2 = vc_con2, c3 = vc_con3, c4 = vc_con4;
        switch (ti[0]) {
        case 0x80000004: vc_con3 &= (u16)~0x20; break;                  /* VTAG_BORDERBLANK_CLR */
        case 0x80000005: vc_con3 |= 0x20; break;                        /* _SET */
        case 0x80000017: ti[1] = (vc_con3 & 0x20) != 0; break;          /* _GET */
        case 0x8000002E: ti[1] = (vc_con3 & 0x02) != 0; break;          /* VTAG_BORDERSPRITE_GET */
        case 0x8000002F: vc_con3 |= 0x02; break;
        case 0x80000030: vc_con3 &= (u16)~0x02; break;
        case 0x80000031:                                                /* VTAG_SPRITERESN_SET (-1: the default) */
            vc_con3 = (u16)((vc_con3 & ~0xC0) | ((s32)d < 0 ? 0 : (d & 3) << 6));
            if (ok)
                cm[17] = (u8)d;
            break;
        case 0x80000032: ti[1] = (vc_con3 >> 6) & 3; break;
        case 0x8000003B: if (ok) cm[18] = (u8)d; break;                 /* VTAG_DEFSPRITERESN_SET */
        case 0x8000003C: ti[1] = ok ? cm[18] : 0; break;
        case 0x80000033: vc_con2 = (u16)((vc_con2 & ~7) | (d & 7)); break;              /* PF1_TO_SPRITEPRI */
        case 0x80000034: ti[1] = vc_con2 & 7; break;
        case 0x80000035: vc_con2 = (u16)((vc_con2 & ~0x38) | (d & 7) << 3); break;      /* PF2_TO_SPRITEPRI */
        case 0x80000036: ti[1] = (vc_con2 >> 3) & 7; break;
        case 0x8000002B: vc_con3 = (u16)((vc_con3 & ~0x1C00) | pf2of(d) << 10); break; /* VTAG_PF2_BASE_SET */
        case 0x80000027: { u32 k = (vc_con3 >> 10) & 7; ti[1] = k ? 1u << k : 0; break; }
        case 0x80000026: ti[1] = 0; break;                              /* VTAG_PF1_BASE_GET */
        case 0x8000002C: vc_con4 = (u16)((vc_con4 & ~0xF0) | (d >> 4 & 15) << 4); break;    /* SPEVEN_BASE */
        case 0x8000002D: vc_con4 = (u16)((vc_con4 & ~0x0F) | (d >> 4 & 15)); break;         /* SPODD_BASE */
        case 0x80000028: ti[1] = (u32)(vc_con4 >> 4 & 15) << 4; break;
        case 0x80000029: ti[1] = (u32)(vc_con4 & 15) << 4; break;
        case 0x8000000B: if (ok) *(u32 *)(cm + 20) = d; break;          /* VTAG_ATTACH_CM_SET */
        case 0x8000001B: ti[1] = ok ? *(u32 *)(cm + 20) : 0; break;
        case 0x80000021: if (ok && cm[1] == 2) *(u32 *)(cm + 36) = d; break;    /* VTAG_VPMODEID_SET */
        case 0x80000020: ti[1] = ok && cm[1] == 2 ? *(u32 *)(cm + 36) : ok ? c_GetVPModeID(*(u8 **)(cm + 20)) :
                                 ~0u; break;
        case 0x80000037: if (d && c_TypeOfMem(d)) *(u32 *)d = 0; break; /* VTAG_IMMEDIATE */
        case 0x80000038: vc_full = 1; break;
        case 0x8000003A: vc_full = 0; break;
        case 0x80000039: ti[1] = vc_full; break;
        }
        if (c2 != vc_con2 || c3 != vc_con3 || c4 != vc_con4)
            changed = 1;
    }
    if (changed && Z.shown && cm == *(u8 **)(Z.shown + SC_ViewPort + VP_ColorMap))
        show_screen((u8 *)Z.shown);
    return 0;
}

/* EasyRequestArgs(window a0, easyStruct a1, idcmp a2, args a3) (-588): the requester's title, text and gadgets (its
 * format strings filled from args as RawDoFmt does) in the program's output, and the leftmost gadget chosen (1; 0
 * when it has one gadget: the autodoc numbers them 1, 2 ... from the left and the rightmost 0) */
struct zbuf { char *b; u32 n, max; };
static void buf_put(void *u, u32 c)
{
    struct zbuf *z = u;
    if (c && z->n + 1 < z->max)
        z->b[z->n++] = (char)c;
}
static void say_fmt(const char *f, const u8 **args, char *b, u32 max)
{
    struct zbuf z = { b, 0, max };
    if (f && c_TypeOfMem((u32)f))
        *args = fmt(f, *args, buf_put, &z);
    b[z.n] = 0;
    for (u32 i = 0; i < z.n; i++)
        if ((u8)b[i] < 32)
            b[i] = ' ';
}
u32 c_EasyRequestArgs(u32 win, const u8 *es, u32 idcmp, const u8 *args)
{
    (void)win, (void)idcmp;
    if (!es || !c_TypeOfMem((u32)es))
        return 0;
    char t[200];
    static const u8 none[8];
    const u8 *a = args && c_TypeOfMem((u32)args) ? args : none;
    zb_say("ZINE: the program asks (a requester)");
    const char *title = *(const char *const *)(es + 8);
    if (title && c_TypeOfMem((u32)title)) {
        zb_say(" \"");
        say_fmt(title, &(const u8 *){ none }, t, sizeof t);
        zb_say(t);
        zb_say("\"");
    }
    say_fmt(*(const char *const *)(es + 12), &a, t, sizeof t);
    zb_say(": ");
    zb_say(t);
    say_fmt(*(const char *const *)(es + 16), &a, t, sizeof t);
    u32 bars = 0;
    for (u32 i = 0; t[i]; i++)
        bars += t[i] == '|';
    zb_say(" [");
    zb_say(t);
    zb_say(bars ? "]: answered with the leftmost gadget\n" : "]: answered with its gadget\n");
    return bars ? 1 : 0;
}

/* the trace build: the shown screen's BitMap at the end (bytes per row, rows, depth, the planes, a sum of plane 0) */
void zb_screen_report(void)
{
    u8 *sc = (u8 *)Z.shown;
    if (!sc)
        return;
    u8 *bm = *(u8 **)(sc + SC_SIZEOF + 76) ? *(u8 **)(sc + SC_SIZEOF + 76) : sc + SC_BitMap;
    u32 sum = 0, p0 = *(u32 *)(bm + ZO_BitMap_Planes);
    for (u32 i = 0; p0 && i < 4000; i++)
        sum += ((const u8 *)p0)[i];
    u32 v[12] = { *(u16 *)(bm + 0), *(u16 *)(bm + 2), bm[5], sum, (u32)bm };
    for (int i = 0; i < 7; i++)
        v[5 + i] = *(u32 *)(bm + ZO_BitMap_Planes + 4 * i);
    static const char hx[] = "0123456789abcdef";
    char t[12 * 9 + 20];
    u32 n = 0;
    static const char lead[] = "ZINE: bitmap";
    zmemcpy(t, lead, sizeof lead - 1);
    n = sizeof lead - 1;
    for (int i = 0; i < 12; i++) {
        t[n++] = ' ';
        for (int k = 0; k < 8; k++)
            t[n++] = hx[v[i] >> (28 - 4 * k) & 15];
    }
    t[n++] = '\n';
    zb_out(t, n);
}

/* every frame (the vertical blank, before the program's servers): the shown screen's plane pointers from its
 * BitMap as it is now (a program's own BitMap may get its planes after OpenScreen, or swap them) */
void zb_screen_frame(void)
{
    u8 *sc = (u8 *)Z.shown;
    if (!sc)
        return;
    u16 *l = *(u16 **)(sc + SC_SIZEOF + 72);
    u8 *bm = *(u8 **)(sc + SC_SIZEOF + 76) ? *(u8 **)(sc + SC_SIZEOF + 76) : sc + SC_BitMap;
    /* the ViewPort's RasInfo (16.13: ScrollVPort, double buffers): its BitMap, if the program set another, and its
     * offsets: RyOffset rows and RxOffset's words down the planes, the rest of RxOffset BPLCON1's delay; a Copper
     * list's BitMap LoadView chose comes first (16.17) */
    const u8 *ri = *(const u8 *const *)(sc + SC_ViewPort + VP_RasInfo);
    u32 off = 0, fine = 0;
    if (ri && c_TypeOfMem((u32)ri)) {
        const u8 *rb = *(const u8 *const *)(sc + SC_SIZEOF + 80) ? *(const u8 *const *)(sc + SC_SIZEOF + 80) :
                       *(const u8 *const *)(ri + 4);
        if (rb && c_TypeOfMem((u32)rb))
            bm = (u8 *)rb;
        s32 rx = *(const s16 *)(ri + 8), ry = *(const s16 *)(ri + 10);
        if (rx > 0 && ry >= 0) {
            off = (u32)ry * *(u16 *)(bm + ZO_BitMap_BytesPerRow) + (u32)(rx >> 4) * 2;
            fine = (u32)(15 - ((rx - 1) & 15));
            if (fine)
                off += 2;
        } else if (ry > 0)
            off = (u32)ry * *(u16 *)(bm + ZO_BitMap_BytesPerRow);
    }
    u16 *c = *(u16 **)(sc + SC_SIZEOF + 68);
    if (c && c[42] == 0x102)            /* BPLCON1 (show_screen's 22nd move) */
        c[43] = (u16)(fine | fine << 4);
    for (u32 i = 0; i < bm[ZO_BitMap_Depth] && i < shown_depth; i++, l += 4) {
        u32 p = *(u32 *)(bm + ZO_BitMap_Planes + 4 * i) + off;
        l[1] = (u16)(p >> 16);
        l[3] = (u16)p;
    }
}

/* ---- exec's message ports (the RKRM autodocs; offsets: MsgPort 34 bytes, mp_SigBit 15, mp_SigTask 16, mp_MsgList
 * 20, mp_Flags 14): PutMsg puts the message on the port and signals its task (PA_SIGNAL); WaitPort on an empty port
 * waits for a signal nothing can send (stopped, as Wait). The one port ZINE answers is its handler port (below). ---- */
#define MP_SigBit 15
#define MP_SigTask 16
#define MP_MsgList 20
#define MP_SIZEOF 34
static void initport(u8 *mp)
{
    newlist((struct zlist *)(mp + MP_MsgList), 5);     /* NT_MESSAGE */
    mp[8] = 4;                          /* ln_Type NT_MSGPORT */
    mp[MP_SigBit] = 16;
    *(u32 *)(mp + MP_SigTask) = *(u32 *)(ZEXEC + ZO_ExecBase_ThisTask);
}

/* the message lists are changed with interrupts held off: the timer and audio.device reply from interrupts */
u32 c_GetMsg(u8 *mp)
{
    if (!mp)
        return 0;
    u16 s = zt_dis();
    u32 m = c_RemHead((struct zlist *)(mp + MP_MsgList));
    zt_en(s);
    return m;
}
/* mp_Flags (exec/ports.h): PA_SIGNAL 0 signals mp_SigTask, PA_SOFTINT 1 causes mp_SoftInt, PA_IGNORE 2 nothing */
void zb_port_put(u8 *mp, struct znode *m, u8 type)
{
    ((u8 *)m)[8] = type;                /* ln_Type */
    u16 s = zt_dis();
    c_AddTail((struct zlist *)(mp + MP_MsgList), m);
    zt_en(s);
    u32 st = *(u32 *)(mp + MP_SigTask);
    if ((mp[14] & 3) == 0 && st && mp[MP_SigBit] < 32)
        c_Signal(st, 1u << mp[MP_SigBit]);
    else if ((mp[14] & 3) == 1 && st)
        c_Cause((struct zint *)st);
}
#define port_put zb_port_put

/* ---- the handler port (docs/d29-zine.md 16.7): every file handle's fh_Type. ZINE has no handler process, but a
 * program may talk to "its file's handler" itself, as dos does: a StandardPacket (its message's ln_Name the
 * DosPacket: dp_Link 0, dp_Port 4, dp_Type 8, dp_Res1 12, dp_Res2 16, dp_Arg1.. 20; dos/dosextens.h, NDK 3.2) put
 * on fh_Type, dp_Arg1 the handle's fh_Arg1 (fh_Args). Spaceballs' Peek reads its packed data that way through its
 * overlay handle. PutMsg to this port answers at once, as the handler would: the action done by the dos call that
 * does it (Read, Write, Seek, Close), dp_Res1 and dp_Res2 set, dp_Port made this port and the message put on the
 * reply port the packet named (with its signal). An action ZINE does not know answers DOSFALSE and
 * ERROR_ACTION_NOT_KNOWN, named in the output (the first 4 times). ---- */
#define ACTION_FLUSH 27
#define ACTION_READ 82
#define ACTION_WRITE 87
#define ACTION_END 1007
#define ACTION_SEEK 1008
#define FH_IN 0x100u                    /* the fh_Args of Input() and Output() (files[] are 0-15) */
#define FH_OUT 0x101u
static u8 unknown_actions;

static u32 fh_of_arg(u32 a)
{
    return a == FH_IN ? Z.in_fh : a == FH_OUT ? Z.out_fh : a < NFILES && files[a].used ? files[a].fh >> 2 : 0;
}

static void packet(struct znode *m)
{
    u32 *p = *(u32 **)((u8 *)m + 10);   /* ln_Name: the DosPacket */
    if (c_TypeOfMem((u32)p) == 0 || ((u32)p & 1))
        return;                         /* not a packet: nothing to answer, nowhere to reply */
    u32 fh = fh_of_arg(p[5]), r1, r2 = 0;
    Z.ioerr = 0;
    switch ((s32)p[2]) {
    case ACTION_READ:
        r1 = c_Read(fh, (char *)p[6], (s32)p[7]);
        break;
    case ACTION_WRITE:
        r1 = c_Write(fh, (const char *)p[6], (s32)p[7]);
        break;
    case ACTION_SEEK:
        r1 = (u32)c_Seek(fh, (s32)p[6], (s32)p[7]);
        break;
    case ACTION_END:
        r1 = c_Close(fh) ? (u32)-1 : 0;
        break;
    case ACTION_FLUSH:
        r1 = (u32)-1;                   /* DOSTRUE: nothing is buffered */
        break;
    default:
        r1 = 0;
        Z.ioerr = 209;                  /* ERROR_ACTION_NOT_KNOWN */
        if (unknown_actions < 4) {
            unknown_actions++;
            say_at("ZINE: a packet of type ", p[2], " sent to a file's handler, which ZINE does not know: answered "
                   "with an error\n");
        }
    }
    if (r1 == (u32)-1 || !r1)
        r2 = Z.ioerr;
    p[3] = r1;
    p[4] = r2;
    u8 *rp = (u8 *)p[1];
    p[1] = Z.hport;
    if (rp && c_TypeOfMem((u32)rp))
        port_put(rp, m, 7);             /* NT_REPLYMSG */
}

void c_PutMsg(u8 *mp, struct znode *m)
{
    if (!mp || !m)
        return;
    if ((u32)mp == Z.hport)
        packet(m);
    else
        port_put(mp, m, 5);             /* NT_MESSAGE */
}
/* ReplyMsg: onto its mn_ReplyPort as NT_REPLYMSG; without one NT_FREEMSG (exec/nodes.h) */
void c_ReplyMsg(u8 *m)
{
    if (!m || !c_TypeOfMem((u32)m))
        return;
    u8 *rp = *(u8 **)(m + 14);
    if (!rp || (u32)rp == Z.hport || ((u32)rp & 1) || !c_TypeOfMem((u32)rp)) {
        m[8] = 6;
        return;
    }
    port_put(rp, (struct znode *)m, 7);
}
/* WaitPort: the port's signal waited for while its list is empty; with one task, an empty port nothing can fill (no
 * timer request or audio write under way) is a stop, as before */
u32 c_WaitPort(u8 *mp)
{
    if (!mp)
        zb_stop(ZH_STOP, ZS_WAIT);
    struct zlist *l = (struct zlist *)(mp + MP_MsgList);
    while (!l->head->succ) {
        if (!zt_on && !zb_timer_busy() && !zb_audio_busy() && !zi_live)
            zb_stop(ZH_STOP, ZS_WAIT);
        c_Wait(mp[MP_SigBit] < 32 ? 1u << mp[MP_SigBit] : 0);
    }
    return (u32)l->head;
}

u32 c_CreateMsgPort(void)
{
    u8 *mp = (u8 *)zx_AllocMem(MP_SIZEOF, 1 << 16);
    if (mp)
        initport(mp);
    return (u32)mp;
}

void c_DeleteMsgPort(u8 *mp) { if (mp) zx_FreeMem((u32)mp, MP_SIZEOF); }

/* ---- intuition's windows: a Window on the screen it names (or the first), its RastPort the screen's, an IDCMP
 * port with a signal of the task's own; the last one opened (or activated) gets the keys and the mouse (binput.c,
 * 16.17). Window 136 bytes: LeftEdge 4, TopEdge 6, Width 8, Height 10, Flags 24, IDCMPFlags 82, UserPort 86,
 * WScreen 46, RPort 50; NewWindow: LeftEdge 0, TopEdge 2, Width 4, Height 6, IDCMPFlags 10, Flags 14, Screen 30; WA_
 * tags 0x80000063 + n (WA_Left 1, WA_Top 2, WA_Flags 8, WA_ReportMouse 0x23, WA_RMBTrap 0x27) ---- */
#define WIN_SIZEOF 136
u32 c_OpenWindowTagList(const u8 *nw, const u32 *tags)
{
    u32 w = 0, h = 0, scr = 0, idcmp = 0, fl = 0, x = 0, y = 0;
    if (nw) {
        x = *(const u16 *)nw;
        y = *(const u16 *)(nw + 2);
        w = *(const u16 *)(nw + 4);
        h = *(const u16 *)(nw + 6);
        idcmp = *(const u32 *)(nw + 10);
        fl = *(const u32 *)(nw + 14);
        scr = *(const u32 *)(nw + 30);
    }
    for (const u32 *t = tags; t; t += 2) {
        u32 tag = t[0], d = t[1];
        if (tag == 0)
            break;
        if (tag == 2) {
            if (!d)
                break;
            t = (const u32 *)d - 2;
            continue;
        }
        if (tag == 3) {
            t += 2 * d;
            continue;
        }
        if (tag == 0x80000066) w = d;
        if (tag == 0x80000067) h = d;
        if (tag == 0x8000006A) idcmp = d;
        if (tag == 0x80000070) scr = d;
        if (tag == 0x80000064) x = d;
        if (tag == 0x80000065) y = d;
        if (tag == 0x8000006B) fl = d;
        if (tag == 0x80000086) fl = d ? fl | 0x200 : fl & ~0x200u;          /* WFLG_REPORTMOUSE */
        if (tag == 0x8000008A) fl = d ? fl | 0x10000 : fl & ~0x10000u;      /* WFLG_RMBTRAP */
    }
    u8 *win = (u8 *)zx_AllocMem(WIN_SIZEOF + MP_SIZEOF, 1 << 16);
    if (!win)
        return 0;
    if (scr) {
        if (!w) w = *(const u16 *)((const u8 *)scr + SC_Width);
        if (!h) h = *(const u16 *)((const u8 *)scr + SC_Height);
        *(u32 *)(win + 50) = scr + SC_RastPort;
    }
    *(u16 *)(win + 8) = (u16)(w ? w : 320);
    *(u16 *)(win + 10) = (u16)(h ? h : 200);
    *(u16 *)(win + 4) = (u16)x;
    *(u16 *)(win + 6) = (u16)y;
    *(u32 *)(win + 24) = fl & ~0x2000u; /* WFLG_WINDOWACTIVE: binput.c's to set */
    *(u32 *)(win + 46) = scr;
    *(u32 *)(win + 82) = idcmp;
    u8 *mp = win + WIN_SIZEOF;
    initport(mp);
    u32 sig = c_AllocSignal((u32)-1);   /* the port's own signal (a window's messages wake only its waiter) */
    mp[MP_SigBit] = (u8)(sig < 32 ? sig : 16);
    *(u32 *)(win + 86) = (u32)mp;
    zi_win_open(win);
    return (u32)win;
}

u32 c_OpenWindow(const u8 *nw) { return c_OpenWindowTagList(nw, 0); }
void c_CloseWindow(u8 *win)
{
    if (!win)
        return;
    zi_win_close(win);
    u8 *mp = win + WIN_SIZEOF;
    if (mp[MP_SigBit] != 16)
        c_FreeSignal(mp[MP_SigBit]);
    zx_FreeMem((u32)win, WIN_SIZEOF + MP_SIZEOF);
}
u32 c_ModifyIDCMP(u8 *win, u32 f) { if (win) *(u32 *)(win + 82) = f; return 1; }
void c_SetPointer(u32 win) { (void)win; }
/* ---- intuition: the public screen and screen buffers (16.12) ----
 * LockPubScreen(name a0) (-510): ZINE has no Workbench, but programs lock "the default public screen" to read its
 * size, mode and font before they open their own; NULL or "Workbench" gives a stand-in made once and never shown: a
 * hires screen of the display's width and height (640 x 256 PAL, 200 NTSC), 2 planes. Any other name: 0, said once.
 * UnlockPubScreen (-516) does nothing (the stand-in stays until the session ends). */
static u8 *pubscr;
u32 c_LockPubScreen(const char *name)
{
    if (name && !(name[0] == 'W' && streq(name, "Workbench"))) {
        static u8 said;
        if (!said++)
            zb_say_text("ZINE: no public screen \"", (u32)name, "\" (ZINE has only the Workbench stand-in)\n");
        return 0;
    }
    if (!pubscr) {
        u32 tags[] = { 0x80000023, 640, 0x80000024, Z.pal ? 256u : 200u, 0x80000025, 2, 0x80000032,
                       (Z.pal ? 0x00021000u : 0x00011000u) | 0x8000, 0x80000037, 1, 0 };
        pubscr = (u8 *)c_OpenScreenTagList(0, tags);
        if (pubscr) {
            *(u16 *)(pubscr + SC_ViewPort + VP_Modes) = 0x8000;    /* HIRES */
            scr_unlink(pubscr);         /* never shown: not one of the screens in depth order (16.17; with it there
                                         * Elude's demos went on to GetDisplayInfoData) */
        }
    }
    return (u32)pubscr;
}
void c_UnlockPubScreen(const char *name, u32 scr) { (void)name, (void)scr; }

/* AllocScreenBuffer(sc a0, bm a1, flags d0) (-768, V39; intuition/screens.h: struct ScreenBuffer { sb_BitMap,
 * sb_DBufInfo }, SB_SCREEN_BITMAP 1, SB_COPY_BITMAP 2): the screen's own BitMap, the program's, or a new one like the
 * screen's (its planes copied with SB_COPY_BITMAP); sb_DBufInfo a cleared DBufInfo (graphics/view.h, 84 bytes: its
 * dbi_SafeMessage at 8 and dbi_DispMessage at 40, which ChangeScreenBuffer replies at once to the ports the program
 * put in them). FreeScreenBuffer (-774) frees what AllocScreenBuffer made. ChangeScreenBuffer (-780) shows the
 * buffer's BitMap: the screen's RastPort and RasInfo take it and the shown screen's planes come from it. */
#define SB_SIZE 12                      /* sb_BitMap, sb_DBufInfo, then ZINE's: 1 when the BitMap is its own */
#define DBI_SIZE 84
u32 c_AllocScreenBuffer(u8 *sc, u32 bm, u32 flags)
{
    if (!sc)
        return 0;
    u8 *sbm = *(u8 **)(sc + SC_SIZEOF + 76) ? *(u8 **)(sc + SC_SIZEOF + 76) : sc + SC_BitMap;
    u32 *sb = (u32 *)zx_AllocMem(SB_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    u32 dbi = zx_AllocMem(DBI_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (!sb || !dbi) {
        if (sb) zx_FreeMem((u32)sb, SB_SIZE);
        if (dbi) zx_FreeMem(dbi, DBI_SIZE);
        return 0;
    }
    sb[1] = dbi;
    if (flags & 1)
        sb[0] = (u32)sbm;
    else if (bm)
        sb[0] = bm;
    else {
        u32 w = *(u16 *)(sbm + ZO_BitMap_BytesPerRow) * 8u, h = *(u16 *)(sbm + ZO_BitMap_Rows), d = sbm[ZO_BitMap_Depth];
        u8 *nb = (u8 *)c_AllocBitMap(w, h, d, 0, 0);
        if (!nb) {
            zx_FreeMem((u32)sb, SB_SIZE);
            zx_FreeMem(dbi, DBI_SIZE);
            return 0;
        }
        if (flags & 2)
            for (u32 i = 0; i < d && i < 8; i++)
                zmemcpy(*(void **)(nb + ZO_BitMap_Planes + 4 * i), *(void **)(sbm + ZO_BitMap_Planes + 4 * i),
                        RASSIZE(w, h));
        sb[0] = (u32)nb;
        sb[2] = 1;
    }
    return (u32)sb;
}
void c_FreeScreenBuffer(u8 *sc, u32 *sb)
{
    (void)sc;
    if (!sb)
        return;
    if (sb[2])
        c_FreeBitMap((u8 *)sb[0]);
    if (sb[1])
        zx_FreeMem(sb[1], DBI_SIZE);
    zx_FreeMem((u32)sb, SB_SIZE);
}
u32 c_ChangeScreenBuffer(u8 *sc, u32 *sb)
{
    if (!sc || !sb || !sb[0])
        return 0;
    *(u32 *)(sc + SC_SIZEOF + 76) = sb[0];
    *(u32 *)(sc + SC_RastPort + RP_BitMap) = sb[0];
    u8 *ri = *(u8 **)(sc + SC_ViewPort + VP_RasInfo);
    if (ri)
        *(u32 *)(ri + 4) = sb[0];
    if (Z.shown == (u32)sc)
        zb_screen_frame();              /* the planes from the new BitMap, from the next frame on */
    u8 *dbi = (u8 *)sb[1];
    for (u32 o = 8; dbi && o <= 40; o += 32) {
        u8 *rp = *(u8 **)(dbi + o + 14);    /* the message's mn_ReplyPort */
        if (rp && c_TypeOfMem((u32)rp))
            port_put(rp, (struct znode *)(dbi + o), 7);
    }
    return 1;
}

/* AllocDBufInfo(vp a0) (-966, V39), FreeDBufInfo(dbi a1) (-972), ChangeVPBitMap(vp a0, bm a1, dbi a2) (-942): the
 * ViewPort's RasInfo shows the BitMap (the shown screen's planes from the next frame), the DBufInfo's two messages
 * replied at once (16.13) */
u32 c_AllocDBufInfo(u8 *vp) { (void)vp; return zx_AllocMem(DBI_SIZE, MEMF_PUBLIC | MEMF_CLEAR); }
void c_FreeDBufInfo(u32 dbi) { if (dbi) zx_FreeMem(dbi, DBI_SIZE); }
void c_ChangeVPBitMap(u8 *vp, u32 bm, u8 *dbi)
{
    u8 *ri = vp && c_TypeOfMem((u32)vp) ? *(u8 **)(vp + VP_RasInfo) : 0;
    if (!ri || !bm || !c_TypeOfMem((u32)ri))
        return;
    *(u32 *)(ri + 4) = bm;
    if (Z.shown && vp == (u8 *)Z.shown + SC_ViewPort)
        zb_screen_frame();
    for (u32 o = 8; dbi && c_TypeOfMem((u32)dbi) && o <= 40; o += 32) {
        u8 *rp = *(u8 **)(dbi + o + 14);
        if (rp && c_TypeOfMem((u32)rp))
            port_put(rp, (struct znode *)(dbi + o), 7);
    }
}

/* ViewAddress (-294): intuition's own View (IntuitionBase's ViewLord), as old programs ask for it (16.14) */
u32 c_ViewAddress(void) { return IB_VIEWLORD(Z.lib[3]); }

u32 c_ViewPortAddress(const u8 *win)
{
    u32 scr = win ? *(const u32 *)(win + 46) : 0;
    return scr ? scr + SC_ViewPort : 0;
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
F(CloseLibrary); F(TaggedOpenLibrary); F(StackSwap); F(OpenResource); F(OpenDevice); F(CloseDevice); F(SetFunction); F(RawDoFmt); F(TypeOfMem); F(CopyMem);
F(CacheControl); F(AllocVec); F(FreeVec); F(CreatePool); F(DeletePool); F(AllocPooled); F(FreePooled);
F(InitRastPort); F(InitBitMap); F(AllocRaster); F(FreeRaster); F(AllocBitMap); F(FreeBitMap);
F(Cause); F(AddICRVector); F(RemICRVector); F(AbleICR); F(SetICR);
F(Open); F(Close); F(Seek); F(LoadSeg); F(UnLoadSeg);
F(DoIO); F(SendIO); F(CheckIO); F(WaitIO); F(AbortIO);
F(TimerBeginIO); F(TimerAbortIO); F(AddTime); F(SubTime); F(CmpTime); F(ReadEClock); F(GetSysTime);
F(GetMsg); F(PutMsg); F(ReplyMsg); F(WaitPort); F(CreateMsgPort); F(DeleteMsgPort);
F(ScreenToBack); F(FreeCprList); F(FindDisplayInfo); F(NextDisplayInfo); F(GetDisplayInfoData); F(OpenWindowTagList); F(OpenWindow); F(CloseWindow); F(ModifyIDCMP); F(SetPointer); F(ViewPortAddress);
F(LoadRGB32); F(SetRGB32); F(OpenScreenTagList); F(OpenScreen); F(CloseScreen); F(ScreenToFront);
F(LoadView); F(WaitTOF); F(WaitBlit); F(OwnBlitter); F(DisownBlitter); F(VBeamPos); F(SetRGB4); F(LoadRGB4);
F(Output); F(Input); F(Write); F(Read); F(PutStr); F(VPrintf); F(VFPrintf); F(FPuts); F(FPutC); F(Flush); F(IoErr);
F(SetIoErr); F(Delay); F(Execute); F(CloseWorkBench); F(OpenWorkBench); F(rts);
F(InitStruct); F(MakeLibrary); F(MakeFunctions); F(FindResident); F(InitResident); F(AddLibrary); F(RemLibrary);
F(SumLibrary); F(AddResource); F(InitSemaphore); F(ObtainSemaphore); F(ReleaseSemaphore); F(AttemptSemaphore);
F(DeleteFile); F(AudioBeginIO); F(AudioAbortIO); F(ConBeginIO); F(ConAbortIO); F(ViewAddress); F(InpBeginIO); F(ZiBeginIO); F(ZiAbortIO); F(RawKeyConvert); F(PeekQualifier); F(RetA0);
X(Supervisor); X(Disable); X(Enable); X(Forbid); X(Permit); X(SetSR); X(GetCC); X(CacheClearU); X(ColdReboot); X(Exit);
X(SuperState); X(UserState);
F(RawIOInit); F(RawMayGetChar); F(RawPutChar); F(CreateIORequest); F(DeleteIORequest); F(AddPort); F(RemPort);
F(FindPort); F(IsInteractive); F(FRead); F(FWrite); F(GetProgramName); F(GetArgStr); F(ReadArgs); F(FreeArgs);
F(AllocDosObject); F(FreeDosObject); F(OpenFont); F(CloseFont); F(SetFont); F(AskFont); F(TextLength); F(Text);
F(Move); F(SetAPen); F(SetBPen); F(SetDrMd); F(RectFill); F(SetRast); F(SetChipRev); F(GetBitMapAttr);
F(BestModeIDA); F(LockPubScreen); F(UnlockPubScreen); F(AllocScreenBuffer); F(FreeScreenBuffer);
F(ChangeScreenBuffer);
/* d29 16.13 */
F(Allocate); F(Deallocate); F(FindSemaphore); F(GetVPModeID); F(ModeNotAvailable); F(BltClear); F(GetSprite);
F(FreeSprite); F(Draw); F(PolyDraw); F(VideoControl); F(ScrollVPort); F(MakeVPort); F(MrgCop); F(QueryOverscan);
F(WBenchTo); F(ActivateWindow); F(DisplayAlert); F(AllocRemember); F(FreeRemember); F(DrawBorder);
F(EasyRequestArgs); F(RethinkDisplay); F(MakeScreen); F(MoveScreen); F(Lock); F(UnLock); F(DupLock); F(Examine);
F(ExNext); F(Info); F(CurrentDir); F(ParentDir); F(DateStamp); F(DupLockFromFH); F(OpenFromLock); F(ParentOfFH);
F(ExamineFH); F(NameFromLock); F(SameLock);
F(GetColorMap); F(FreeColorMap); F(GetRGB4); F(SetRGB4CM); F(SetRGB32CM); F(GetRGB32); F(InitVPort); F(InitView);
F(SumKickData); F(Fault); F(PrintFault); F(WaitBOVP); F(WritePixel); F(ReadPixel);
F(AllocDBufInfo); F(FreeDBufInfo); F(ChangeVPBitMap); F(FilePart); F(PathPart); F(AddPart);
F(CreateNewProc); F(CreateProc); F(AddTask); F(RemTask);

/* the offsets: the RKRM's Function Offset Reference (V37), the pools and ColdReboot from the NDK 3.2's exec_lib.fd */
static const u16 exec_lvo[] = { 30, 108, 114, 120, 126, 132, 138, 144, 162, 168, 174, 198, 204, 210, 216, 234, 240,
    246, 252, 258, 264, 270, 276, 294, 300, 306, 318, 324, 330, 336, 408, 414, 420, 444, 450, 498, 522, 528, 534, 552,
    624, 630, 636, 642, 648, 684, 690, 696, 702, 708, 714, 726, 732, 810, 366, 372, 378, 384, 666, 672, 456, 462, 468,
    474, 480, 180,
    78, 84, 90, 96, 102, 396, 402, 426, 486, 558, 564, 570, 576, 678, 720,
    150, 156, 504, 510, 516, 654, 660, 354, 360, 390,
    186, 192, 594, 612, 282, 288 };
static void (*const exec_fn[])(void) = { zx_Supervisor, zs_Alert, zs_rts, zx_Disable, zx_Enable, zx_Forbid,
    zx_Permit, zx_SetSR, zs_SetIntVector, zs_AddIntServer, zs_RemIntServer, zs_AllocMem, zs_AllocAbs, zs_FreeMem,
    zs_AvailMem, zs_Insert, zs_AddHead, zs_AddTail, zs_Remove, zs_RemHead, zs_RemTail, zs_Enqueue, zs_FindName,
    zs_FindTask, zs_SetTaskPri, zs_SetSignal, zs_Wait, zs_Signal, zs_AllocSignal, zs_FreeSignal, zs_OldOpenLibrary,
    zs_CloseLibrary, zs_SetFunction, zs_OpenDevice, zs_CloseDevice, zs_OpenResource, zs_RawDoFmt, zx_GetCC,
    zs_TypeOfMem, zs_OpenLibrary, zs_CopyMem, zs_CopyMem, zx_CacheClearU, zx_CacheClearU, zs_CacheControl,
    zs_AllocVec, zs_FreeVec, zs_CreatePool, zs_DeletePool, zs_AllocPooled, zs_FreePooled, zx_ColdReboot,
    zs_StackSwap, zs_TaggedOpenLibrary, zs_PutMsg, zs_GetMsg, zs_ReplyMsg, zs_WaitPort, zs_CreateMsgPort,
    zs_DeleteMsgPort, zs_DoIO, zs_SendIO, zs_CheckIO, zs_WaitIO, zs_AbortIO,
    zs_Cause,
    zs_InitStruct, zs_MakeLibrary, zs_MakeFunctions, zs_FindResident, zs_InitResident, zs_AddLibrary, zs_RemLibrary,
    zs_SumLibrary, zs_AddResource, zs_InitSemaphore, zs_ObtainSemaphore, zs_ReleaseSemaphore, zs_AttemptSemaphore,
    zs_ObtainSemaphore, zs_AttemptSemaphore,
    zx_SuperState, zx_UserState, zs_RawIOInit, zs_RawMayGetChar, zs_RawPutChar, zs_CreateIORequest,
    zs_DeleteIORequest, zs_AddPort, zs_RemPort, zs_FindPort,
    zs_Allocate, zs_Deallocate, zs_FindSemaphore, zs_SumKickData, zs_AddTask, zs_RemTask };
static const u16 gfx_lvo[] = { 192, 222, 228, 270, 288, 384, 456, 462, 390, 492, 498, 918, 924, 882, 852, 198,
    72, 78, 66, 474, 54, 60, 240, 342, 348, 354, 306, 234, 888, 960, 1050,
    792, 798, 300, 408, 414, 246, 336, 708, 588, 216, 210, 966, 972, 942,
    570, 576, 582, 630, 996, 900, 204, 360, 402, 324, 318, 564, 726, 732, 756 };
static void (*const gfx_fn[])(void) = { zs_LoadRGB4, zs_LoadView, zs_WaitBlit, zs_WaitTOF, zs_SetRGB4, zs_VBeamPos,
    zs_OwnBlitter, zs_DisownBlitter, zs_InitBitMap, zs_AllocRaster, zs_FreeRaster, zs_AllocBitMap, zs_FreeBitMap,
    zs_LoadRGB32, zs_SetRGB32, zs_InitRastPort,
    zs_OpenFont, zs_CloseFont, zs_SetFont, zs_AskFont, zs_TextLength, zs_Text, zs_Move, zs_SetAPen, zs_SetBPen,
    zs_SetDrMd, zs_RectFill, zs_SetRast, zs_SetChipRev, zs_GetBitMapAttr, zs_BestModeIDA,
    zs_GetVPModeID, zs_ModeNotAvailable, zs_BltClear, zs_GetSprite, zs_FreeSprite, zs_Draw, zs_PolyDraw,
    zs_VideoControl, zs_ScrollVPort, zs_MakeVPort, zs_MrgCop, zs_AllocDBufInfo, zs_FreeDBufInfo, zs_ChangeVPBitMap,
    zs_GetColorMap, zs_FreeColorMap, zs_GetRGB4, zs_SetRGB4CM, zs_SetRGB32CM, zs_GetRGB32, zs_InitVPort, zs_InitView,
    zs_WaitBOVP, zs_WritePixel, zs_ReadPixel, zs_FreeCprList, zs_FindDisplayInfo, zs_NextDisplayInfo,
    zs_GetDisplayInfoData };
static const u16 dos_lvo[] = { 42, 48, 54, 60, 132, 144, 198, 312, 342, 354, 360, 462, 948, 954, 30, 36, 66, 150,
    156, 222, 72, 216, 324, 330, 576, 534, 798, 858, 228, 234,
    84, 90, 96, 102, 108, 114, 126, 210, 192, 372, 378, 384, 390, 402, 420, 870, 876, 882, 468, 474, 498, 138 };
static void (*const dos_fn[])(void) = { zs_Read, zs_Write, zs_Input, zs_Output, zs_IoErr, zx_Exit, zs_Delay,
    zs_FPutC, zs_FPuts, zs_VFPrintf, zs_Flush, zs_SetIoErr, zs_PutStr, zs_VPrintf, zs_Open, zs_Close, zs_Seek,
    zs_LoadSeg, zs_UnLoadSeg, zs_Execute, zs_DeleteFile, zs_IsInteractive, zs_FRead, zs_FWrite, zs_GetProgramName,
    zs_GetArgStr, zs_ReadArgs, zs_FreeArgs, zs_AllocDosObject, zs_FreeDosObject,
    zs_Lock, zs_UnLock, zs_DupLock, zs_Examine, zs_ExNext, zs_Info, zs_CurrentDir, zs_ParentDir, zs_DateStamp,
    zs_DupLockFromFH, zs_OpenFromLock, zs_ParentOfFH, zs_ExamineFH, zs_NameFromLock, zs_SameLock, zs_FilePart,
    zs_PathPart, zs_AddPart, zs_Fault, zs_PrintFault, zs_CreateNewProc, zs_CreateProc };
static const u16 int_lvo[] = { 78, 210, 612, 198, 66, 252, 246, 606, 204, 72, 150, 270, 60, 300,
    510, 516, 768, 774, 780,
    474, 342, 336, 450, 90, 396, 408, 108, 588, 390, 384, 378, 162, 294 };
static void (*const int_fn[])(void) = { zs_CloseWorkBench, zs_OpenWorkBench, zs_OpenScreenTagList, zs_OpenScreen,
    zs_CloseScreen, zs_ScreenToFront, zs_ScreenToBack, zs_OpenWindowTagList, zs_OpenWindow, zs_CloseWindow,
    zs_ModifyIDCMP, zs_SetPointer, zs_SetPointer, zs_ViewPortAddress,
    zs_LockPubScreen, zs_UnlockPubScreen, zs_AllocScreenBuffer, zs_FreeScreenBuffer, zs_ChangeScreenBuffer,
    zs_QueryOverscan, zs_WBenchTo, zs_WBenchTo, zs_ActivateWindow, zs_DisplayAlert, zs_AllocRemember,
    zs_FreeRemember, zs_DrawBorder, zs_EasyRequestArgs, zs_RethinkDisplay, zs_RethinkDisplay, zs_MakeScreen,
    zs_MoveScreen, zs_ViewAddress };
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

/* ---- timer.device (the RKRM's Devices manual; IORequest: mn_ReplyPort 14, io_Device 20, io_Command 28, io_Flags 30,
 * io_Error 31; timerequest's timeval at 32): the time is CIA-A's time-of-day counter, which counts the display's
 * frames (20 ms on PAL), so it is as fine as a frame. TR_ADDREQUEST is kept until it is due; WaitIO, DoIO and Wait
 * wait for it (one process: nothing else could run meanwhile), CheckIO says whether it is. ---- */
#define TR_ADDREQUEST 9
#define TR_GETSYSTIME 10
#define TR_SETSYSTIME 11
#define IOF_QUICK 1
#define NTREQ 16
static u8 *treq[NTREQ];
static u32 tdue[NTREQ];
static u32 tlast, thigh;                /* the counter's last value and the times it wrapped (24 bits) */

static u32 ticks(void)                  /* frames since the program started, monotonic (interrupts read it too) */
{
    u16 s = zt_dis();
    u32 hi = CIAA(CIA_TODHI), mid = CIAA(CIA_TODMID), lo = CIAA(CIA_TODLO);
    u32 t = hi << 16 | mid << 8 | lo;
    if (t < tlast)
        thigh++;
    tlast = t;
    t = ((thigh << 24) | t) - Z.tod0;
    zt_en(s);
    return t;
}
u32 zb_frames(void) { return ticks(); }

static u32 hz(void) { return Z.pal ? 50 : 60; }
u32 zb_ticks50(void) { u32 t = ticks(); return Z.pal ? t : t / 6 * 5 + t % 6 * 5 / 6; }    /* in 1/50 s */
void zb_say(const char *s) { zb_out(s, slen(s)); }

static void tv_now(u32 *tv)
{
    u32 t = ticks();
    tv[0] = t / hz();
    tv[1] = (t % hz()) * (1000000u / hz());
}

/* a request done: replied to its port (unless quick) with the io_Error its command set */
void zb_io_done(u8 *io)
{
    io[8] = 7;                          /* ln_Type NT_REPLYMSG */
    if (io[30] & IOF_QUICK)
        return;
    u8 *rp = *(u8 **)(io + 14);
    if (rp)
        port_put(rp, (struct znode *)io, 7);
}

static int pending(u8 *io)
{
    for (int i = 0; i < NTREQ; i++)
        if (treq[i] == io)
            return i;
    return -1;
}

/* the requests that are due completed (from a task, the vertical blank or the idle loop): how many */
int zb_timer_poll(void)
{
    int n = 0;
    u32 now = ticks();
    for (int i = 0; i < NTREQ; i++) {
        u16 s = zt_dis();
        u8 *io = treq[i] && (s32)(now - tdue[i]) >= 0 ? treq[i] : 0;
        if (io)
            treq[i] = 0;
        zt_en(s);
        if (io) {
            zb_io_done(io);
            n++;
        }
    }
    return n;
}
int zb_timer_busy(void)
{
    for (int i = 0; i < NTREQ; i++)
        if (treq[i])
            return 1;
    return 0;
}

/* the earliest due request, waited for and completed: 1, or 0 when none is waiting */
static int timer_step(void)
{
    int best = -1;
    for (int i = 0; i < NTREQ; i++)
        if (treq[i] && (best < 0 || (s32)(tdue[i] - tdue[best]) < 0))
            best = i;
    if (best < 0)
        return 0;
    while ((s32)(ticks() - tdue[best]) < 0)
        ;
    u8 *io = treq[best];
    treq[best] = 0;
    zb_io_done(io);
    return 1;
}

void c_TimerBeginIO(u8 *io)
{
    u16 cmd = *(u16 *)(io + 28);
    u32 *tv = (u32 *)(io + 32);
    io[8] = 5;                          /* NT_MESSAGE: in progress */
    io[31] = 0;
    if (cmd == TR_ADDREQUEST) {
        /* the units (devices/timer.h; 16.13): MICROHZ 0 and VBLANK 1 a time from now, WAITUNTIL 2 a system time
         * (GetSysTime's), ECLOCK 3 E-clock ticks from now, WAITECLOCK 4 an E-clock value (ReadEClock's); in frames */
        u32 unit = *(u32 *)(io + 24), now = ticks(), n;
        if (unit == 3 || unit == 4) {
            u32 e = Z.a.eclock_hz / hz();       /* E-clock ticks a frame (ReadEClock's scale) */
            u32 q = ~0u;                /* (hi:lo) / e, in two 16-bit steps (hi < e < 65536: no 64-bit division) */
            if (e && tv[0] < e) {
                u32 a = tv[0] << 16 | tv[1] >> 16, b = (a % e) << 16 | (tv[1] & 0xFFFF);
                q = (a / e) << 16 | b / e;
            }
            n = unit == 3 ? q : q > now ? q - now : 0;
        } else {
            n = tv[0] > 0x7FFFFFFFu / hz() ? 0x7FFFFFFFu : tv[0] * hz() + (tv[1] % 1000000u * hz() + 999999u) / 1000000u;
            if (unit == 2)
                n = n > now ? n - now : 0;
        }
        u32 due = ticks() + n;
        io[30] &= (u8)~IOF_QUICK;
        u16 s = zt_dis();
        for (int i = 0; i < NTREQ; i++)
            if (!treq[i]) {
                tdue[i] = due;
                treq[i] = io;
                zt_en(s);
                return;
            }
        zt_en(s);
        io[31] = (u8)-1;                /* no room: IOERR_OPENFAIL as the nearest refusal */
        zb_io_done(io);
        return;
    }
    if (cmd == TR_GETSYSTIME)
        tv_now(tv);
    else if (cmd != TR_SETSYSTIME) {
        io[31] = (u8)-3;                /* IOERR_NOCMD */
        if (!(io[30] & IOF_QUICK))
            zb_io_done(io);
        return;
    }
    zb_io_done(io);
}

void c_TimerAbortIO(u8 *io)
{
    u16 s = zt_dis();
    int i = pending(io);
    if (i >= 0)
        treq[i] = 0;
    zt_en(s);
    if (i >= 0) {
        zb_io_done(io);
        io[31] = (u8)-2;                /* IOERR_ABORTED */
    }
}

/* exec's I/O calls for every device ZINE has (timer.device only) */
u32 c_CheckIO(u8 *io)
{
    zb_timer_poll();
    return io && pending(io) < 0 && !zb_audio_pending(io) && !zb_con_pending(io) && !zi_pending(io) ? (u32)io : 0;
}
s32 c_WaitIO(u8 *io)
{
    if (!io)
        return 0;
    u8 *wp = *(u8 **)(io + 14);
    while (zt_on && wp && (pending(io) >= 0 || zb_audio_pending(io) || zb_con_pending(io) || zi_pending(io)))
        c_Wait(wp[15] < 32 ? 1u << wp[15] : 0);  /* more than one task: the reply port's signal (16.16) */
    while (pending(io) >= 0)
        timer_step();
    while (zb_audio_pending(io))        /* an audio write: its interrupt ends it */
        if (EB(ZO_ExecBase_IDNestCnt, s8) >= 0 || !(INTENAR & 0x4000))
            zb_stop(ZH_STOP, ZS_WAIT);
    while (zb_con_pending(io) || zi_pending(io)) {  /* a read of keys or a port: the vertical blank ends it */
        if (!timer_step() && (EB(ZO_ExecBase_IDNestCnt, s8) >= 0 || !(INTENAR & 0x4000) || !(INTENAR & 0x0020)))
            zb_stop(ZH_STOP, ZS_WAIT);
    }
    u8 *rp = *(u8 **)(io + 14);
    if (rp && !(io[30] & IOF_QUICK)) {
        u16 s = zt_dis();
        c_Remove((struct znode *)io);   /* off the reply port */
        zt_en(s);
    }
    return (s8)io[31];
}
void c_SendIO(u8 *io)
{
    io[30] = 0;
    if (*(u32 *)(io + 20) == timer_base)
        c_TimerBeginIO(io);
    else if (*(u32 *)(io + 20) == zb_audio_base && zb_audio_base)
        zb_audio_beginio(io);
    else if (*(u32 *)(io + 20) == zb_con_base && zb_con_base)
        zb_con_beginio(io);
    else if (*(u32 *)(io + 20) == zb_inp_base && zb_inp_base)
        zb_inp_beginio(io);
    else if (is_zi(io))
        zi_beginio(io);
    else {
        io[31] = (u8)-1;
        zb_io_done(io);
    }
}
s32 c_DoIO(u8 *io)
{
    io[30] = IOF_QUICK;
    if (*(u32 *)(io + 20) == timer_base)
        c_TimerBeginIO(io);
    else if (*(u32 *)(io + 20) == zb_audio_base && zb_audio_base)
        zb_audio_beginio(io);
    else if (*(u32 *)(io + 20) == zb_con_base && zb_con_base)
        zb_con_beginio(io);
    else if (*(u32 *)(io + 20) == zb_inp_base && zb_inp_base)
        zb_inp_beginio(io);
    else if (is_zi(io))
        zi_beginio(io);
    else
        io[31] = (u8)-1;
    return c_WaitIO(io);
}
void c_AbortIO(u8 *io)
{
    if (io && *(u32 *)(io + 20) == timer_base)
        c_TimerAbortIO(io);
    else if (io && *(u32 *)(io + 20) == zb_audio_base && zb_audio_base)
        zb_audio_abort(io);
    else if (io && *(u32 *)(io + 20) == zb_con_base && zb_con_base)
        zb_con_abort(io);
    else if (io && is_zi(io))
        zi_abort(io);
}

void c_AddTime(u32 *d, const u32 *s2)
{
    d[0] += s2[0];
    d[1] += s2[1];
    if (d[1] >= 1000000u)
        d[1] -= 1000000u, d[0]++;
}
void c_SubTime(u32 *d, const u32 *s2)
{
    if (d[1] < s2[1])
        d[1] += 1000000u, d[0]--;
    d[1] -= s2[1];
    d[0] -= s2[0];
}
s32 c_CmpTime(const u32 *a, const u32 *b)
{
    if (a[0] != b[0])
        return a[0] > b[0] ? -1 : 1;
    if (a[1] != b[1])
        return a[1] > b[1] ? -1 : 1;
    return 0;
}
u32 c_ReadEClock(u32 *ev)
{
    u32 t = ticks(), e = Z.a.eclock_hz / hz();
    /* t * e as 64 bits: t below 2^24 frames for days, e about 14200 */
    u32 lo = (t & 0xFFFF) * e, hi = (t >> 16) * e;
    ev[1] = lo + (hi << 16);
    ev[0] = (hi >> 16) + (ev[1] < lo);
    return Z.a.eclock_hz;
}
void c_GetSysTime(u32 *tv) { tv_now(tv); }

/* ---- ciaa.resource and ciab.resource (the RKRM's Resources chapter; cia_lib.fd: AddICRVector -6, RemICRVector -12,
 * AbleICR -18, SetICR -24): a handler for each of a CIA's five interrupt bits, called from the level 2 (CIA-A) and
 * level 6 (CIA-B) interrupts with a1 its is_Data, as exec's own does. ICR reads clear the chip's bits, so what was
 * read is kept in `pend` for SetICR. ---- */
static struct zint *icr_vec[2][5];
static u8 icr_en[2], icr_pend[2];
static volatile u8 *cia_icr(int c) { return c ? &CIAB(CIA_ICR) : &CIAA(CIA_ICR); }
static int which(u32 base) { return base == cia_base[1]; }

u32 c_AddICRVector(u32 base, u32 bit, struct zint *iv)
{
    int c = which(base);
    if (bit > 4)
        return (u32)iv;
    if (icr_vec[c][bit])
        return (u32)icr_vec[c][bit];
    icr_vec[c][bit] = iv;
    icr_en[c] |= (u8)(1u << bit);
    *cia_icr(c) = (u8)(0x80 | 1u << bit);
    return 0;
}

void c_RemICRVector(u32 base, u32 bit, struct zint *iv)
{
    int c = which(base);
    (void)iv;
    if (bit > 4)
        return;
    icr_vec[c][bit] = 0;
    icr_en[c] &= (u8)~(1u << bit);
    *cia_icr(c) = (u8)(1u << bit);
}

u32 c_AbleICR(u32 base, u32 mask)
{
    int c = which(base);
    u32 old = icr_en[c];
    if (mask & 0x80)
        icr_en[c] |= (u8)(mask & 0x1F);
    else
        icr_en[c] &= (u8)~(mask & 0x1F);
    if (mask & 0x1F)
        *cia_icr(c) = (u8)mask;
    return old;
}

u32 c_SetICR(u32 base, u32 mask)
{
    int c = which(base);
    icr_pend[c] |= *cia_icr(c) & 0x1F;
    u32 old = icr_pend[c];
    if (mask & 0x80)
        icr_pend[c] |= (u8)(mask & 0x1F);
    else
        icr_pend[c] &= (u8)~(mask & 0x1F);
    return old;
}

/* exec's Cause (-180): a software interrupt. One process and no lower priority to wait for: the handler runs at
 * once, with a1 its is_Data (a program calls Cause from its own interrupt code, which then finishes after it) */
void c_Cause(struct zint *iv)
{
    if (iv && iv->code)
        zb_call(iv->code, iv->data, 0);
}

/* from zb_irq: the CIA's ICR (read once there), the handlers of its bits called; the bits handled are returned */
u32 zb_cia_irq(int c, u32 icr)
{
    icr_pend[c] |= (u8)(icr & 0x1F);
    u32 done = 0;
    for (int b = 0; b < 5; b++)
        if ((icr_pend[c] & icr_en[c] & (1u << b)) && icr_vec[c][b]) {
            icr_pend[c] &= (u8)~(1u << b);
            zb_call(icr_vec[c][b]->code, icr_vec[c][b]->data, 0);
            done |= 1u << b;
        }
    return done;
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
    za_init(&assigns);                  /* LIBS: as ZINE starts it (16.8) */
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
    /* timer.device: its own base (the Library 34 bytes after 11 vectors: Open, Close, Expunge, the reserved one,
     * BeginIO, AbortIO and the five of timer_lib.fd) */
    {
        static const u16 tlvo[] = { 6, 12, 18, 24, 30, 36, 42, 48, 54, 60, 66 };
        static void (*const tfn[])(void) = { zs_rts, zs_rts, zs_rts, zs_rts, zs_TimerBeginIO, zs_TimerAbortIO,
                                             zs_AddTime, zs_SubTime, zs_CmpTime, zs_ReadEClock, zs_GetSysTime };
        static const char tname[] = "timer.device";
        u32 base = take(11 * 6 + 34) + 11 * 6;
        zb_jumptable(base, 11, tlvo, tfn, 11);
        struct zlib *l = (struct zlib *)base;
        l->node.type = 3;               /* NT_DEVICE */
        l->node.name = (char *)tname;
        l->negsize = 66;
        l->possize = 34;
        l->version = 40;
        l->idstring = (char *)idstr;
        timer_base = base;
    }
    {   /* audio.device (blibload.c, 16.8): Open, Close, Expunge, the reserved one, BeginIO, AbortIO */
        static const u16 alvo[] = { 6, 12, 18, 24, 30, 36 };
        static void (*const afn[])(void) = { zs_rts, zs_rts, zs_rts, zs_rts, zs_AudioBeginIO, zs_AudioAbortIO };
        static const char aname[] = "audio.device";
        u32 base = take(6 * 6 + 34) + 6 * 6;
        zb_jumptable(base, 6, alvo, afn, 6);
        struct zlib *l = (struct zlib *)base;
        l->node.type = 3;               /* NT_DEVICE */
        l->node.name = (char *)aname;
        l->negsize = 36;
        l->possize = 34;
        l->version = 40;
        l->idstring = (char *)idstr;
        zb_audio_init(base);
    }
    {   /* console.device (blibload.c, 16.14): Open, Close, Expunge, the reserved one, BeginIO, AbortIO, then
         * console_lib.fd's CDInputHandler (-42) and RawKeyConvert (-48) */
        static const u16 clvo[] = { 6, 12, 18, 24, 30, 36, 42, 48 };
        static void (*const cfn[])(void) = { zs_rts, zs_rts, zs_rts, zs_rts, zs_ConBeginIO, zs_ConAbortIO, zs_RetA0,
                                             zs_RawKeyConvert };
        static const char cname[] = "console.device";
        u32 base = take(8 * 6 + 34) + 8 * 6;
        zb_jumptable(base, 8, clvo, cfn, 8);
        struct zlib *l = (struct zlib *)base;
        l->node.type = 3;               /* NT_DEVICE */
        l->node.name = (char *)cname;
        l->negsize = 48;
        l->possize = 34;
        l->version = 40;
        l->idstring = (char *)idstr;
        zb_con_init(base);
    }
    {   /* input.device (blibload.c, 16.14): Open, Close, Expunge, the reserved one, BeginIO, AbortIO (nothing is ever
         * in progress), then input_lib.fd's PeekQualifier (-42: no qualifier keys held) */
        static const u16 ilvo[] = { 6, 12, 18, 24, 30, 36, 42 };
        static void (*const ifn[])(void) = { zs_rts, zs_rts, zs_rts, zs_rts, zs_InpBeginIO, zs_rts, zs_PeekQualifier };
        static const char iname[] = "input.device";
        u32 base = take(7 * 6 + 34) + 7 * 6;
        zb_jumptable(base, 7, ilvo, ifn, 7);
        struct zlib *l = (struct zlib *)base;
        l->node.type = 3;               /* NT_DEVICE */
        l->node.name = (char *)iname;
        l->negsize = 42;
        l->possize = 34;
        l->version = 40;
        l->idstring = (char *)idstr;
        zb_inp_init(base);
    }
    for (int i = 0; i < 2; i++) {       /* keyboard.device and gameport.device (binput.c, 16.17): Open, Close, Expunge,
                                         * the reserved one, BeginIO, AbortIO */
        static const u16 klvo[] = { 6, 12, 18, 24, 30, 36 };
        static void (*const kfn[])(void) = { zs_rts, zs_rts, zs_rts, zs_rts, zs_ZiBeginIO, zs_ZiAbortIO };
        static const char *const kname[2] = { "keyboard.device", "gameport.device" };
        u32 base = take(6 * 6 + 34) + 6 * 6;
        zb_jumptable(base, 6, klvo, kfn, 6);
        struct zlib *l = (struct zlib *)base;
        l->node.type = 3;               /* NT_DEVICE */
        l->node.name = (char *)kname[i];
        l->negsize = 36;
        l->possize = 34;
        l->version = 40;
        l->idstring = (char *)idstr;
        *(i ? &zb_gp_base : &zb_kbd_base) = base;
    }
    zi_init();
    extern u32 zb_tracing;
    if (zb_tracing) {                   /* the trace build: the bases, so the trace's slots can be named */
        static const char hx[] = "0123456789abcdef";
        u32 v[5] = { Z.lib[0], Z.lib[1], Z.lib[2], Z.lib[3], timer_base };
        for (int i = 0; i < 5; i++) {
            char t[11] = { 'B', (char)('0' + i) };
            for (int k = 0; k < 8; k++)
                t[2 + k] = hx[v[i] >> (28 - 4 * k) & 15];
            t[10] = '\n';
            for (int k = 0; k < 11; k++) {
                while (!(CUSTOM(0x018) & 0x2000))
                    ;
                CUSTOM(0x030) = (u16)(0x100 | (u8)t[k]);
            }
        }
        extern void zb_serial_hex(char tag, const u32 *v, int n);
        u32 code = (u32)zb_libs_init;   /* where the blob's code is (a 'P' sample near it: inside a ZINE call) */
        zb_serial_hex('C', &code, 1);
    }
    for (int c = 0; c < 2; c++) {       /* the CIAs' resources: 4 vectors and a Library each */
        static const u16 clvo[] = { 6, 12, 18, 24 };
        static void (*const cfn[])(void) = { zs_AddICRVector, zs_RemICRVector, zs_AbleICR, zs_SetICR };
        static const char *const cname[2] = { "ciaa.resource", "ciab.resource" };
        u32 base = take(4 * 6 + 34) + 4 * 6;
        zb_jumptable(base, 4, clvo, cfn, 4);
        struct zlib *l = (struct zlib *)base;
        l->node.type = 8;               /* NT_RESOURCE */
        l->node.name = (char *)cname[c];
        l->negsize = 24;
        l->possize = 34;
        l->version = 40;
        cia_base[c] = base;
    }
    /* ExecBase (5.1) */
    EB(ZO_ExecBase_SoftVer, u16) = 1;
    EB(ZO_ExecBase_ChkBase, u32) = 0;   /* not ~ExecBase: a reset never trusts it (no path B, owner 2026-10-30) */
    EB(ZO_ExecBase_SysStkUpper, u32) = Z.stack_top;
    EB(ZO_ExecBase_SysStkLower, u32) = Z.stack_top - (16u << 10);
    EB(ZO_ExecBase_IDNestCnt, s8) = -1;
    EB(ZO_ExecBase_TDNestCnt, s8) = -1;
    EB(ZO_ExecBase_AttnFlags, u16) = Z.attn;
    EB(ZO_ExecBase_TaskSigAlloc, u32) = 0xFFFF;
    EB(ZO_ExecBase_Quantum, u16) = 4;              /* exec's default: 4 frames (16.16) */
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
    /* the blitter's vector holds GfxBase as its data, as graphics' own handler has it there: take-over code reads
     * GfxBase from ExecBase->IntVects[INTB_BLIT].iv_Data instead of opening the library (no handler: its interrupt
     * stays off until a program sets one) */
    IV(6)->data = Z.lib[1];
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
    u8 *hp = (u8 *)take(MP_SIZEOF);     /* the handler port (16.7): no task to signal, PA_IGNORE */
    newlist((struct zlist *)(hp + MP_MsgList), NT_MESSAGE);
    hp[8] = NT_MSGPORT;
    hp[14] = 2;
    Z.hport = (u32)hp;
    *(u32 *)(out + ZO_FileHandle_fh_Type) = Z.hport, *(u32 *)(out + ZO_FileHandle_fh_Args) = FH_OUT;
    *(u32 *)(in + ZO_FileHandle_fh_Type) = Z.hport, *(u32 *)(in + ZO_FileHandle_fh_Args) = FH_IN;
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
    zb_more_init(take);                 /* utility, the math libraries, topaz 8 (bmore.c, 16.12) */
    *(volatile u32 *)4 = ZEXEC;         /* only now does address 4 point at an ExecBase (7.3 step 3) */
}
