/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's blob: the way in (7.3), the machine as the program finds it (8.1, 8.2), the interrupts (8.3), the program's
 * memory (5.2's memory functions) and the way back (9.1 path A, 9.2). blob.h says what the blob is. Path B (the
 * reset capture) is not built: the owner dropped it (decisions, 2026-10-30); a reset during a session is a cold
 * start, and ExecBase is made so that the ROM never trusts it (ChkBase is not its complement). */
#include "blob.h"
#include "hunk.h"
#include "zvol.h"

struct zstate Z;
u32 zr_how, zr_code, zr_ret6, zr_callpc;
u32 zb_stack_top;

void *zmemset(void *d, int c, u32 n)
{
    u8 *p = d;
    while (n--)
        *p++ = (u8)c;
    return d;
}

void *zmemcpy(void *d, const void *s, u32 n)
{
    u8 *p = d;
    const u8 *q = s;
    if (n >= 8 && !(((u32)p | (u32)q) & 3)) {
        u32 *pl = (u32 *)p;
        const u32 *ql = (const u32 *)q;
        for (u32 k = n >> 2; k; k--)
            *pl++ = *ql++;
        p = (u8 *)pl;
        q = (const u8 *)ql;
        n &= 3;
    }
    while (n--)
        *p++ = *q++;
    return d;
}

/* gcc may call these for struct copies */
void *memset(void *d, int c, unsigned long n) { return zmemset(d, c, n); }
void *memcpy(void *d, const void *s, unsigned long n) { return zmemcpy(d, s, n); }

/* ---- timing by the beam (8.3: nothing of ZINE's uses a CIA timer, which belong to the program) ---- */
static u32 line(void)
{
    return ((u32)(VPOSR & 1) << 8) | (VHPOSR >> 8);
}

/* n beam lines (64 us each in a 15 kHz mode); a beam that does not move ends the wait after a bounded number of
 * reads (each a chip-bus access: well over a second on any machine) */
void zb_wait_lines(u32 n)
{
    u32 last = line(), seen = 0;
    for (u32 it = 0; seen < n && it < 4000000u; it++) {
        u32 l = line();
        if (l != last) {
            last = l;
            seen++;
        }
    }
}

/* the blitter: the first read of DMACONR may show BUSY too early on some Agnus chips (the WaitBlit autodoc), so it
 * is read once before the loop; the loop ends after 500 ms of beam lines at most */
void zb_waitblit(void)
{
    (void)DMACONR;
    u32 last = line(), seen = 0;
    for (u32 it = 0; (DMACONR & 0x4000) && seen < 7800 && it < 4000000u; it++) {
        u32 l = line();
        if (l != last) {
            last = l;
            seen++;
        }
    }
}

/* ---- the plan (4.3), on the kernel's stack: nothing touched yet ---- */
struct bootregion { u32 attrs, lower, upper; };
struct bootinfo { u16 attn, nregions; u32 kernel_pa, kernel_size, font; struct bootregion r[8]; };

static struct zp_region reg[8];
static u32 nreg;

static int in_fast(u32 a, u32 n)
{
    for (u32 i = 0; i < nreg; i++)
        if (!(reg[i].attrs & MEMF_CHIP) && a >= reg[i].lower && n <= reg[i].upper - a)
            return 1;
    return 0;
}

#define ZHDR_MAGIC 0x5A4D454Du          /* kernel/mem.c's block header: size | used, prev, owner, "ZMEM" */
#define AREA  (24u << 10)               /* the libraries, their tables, the process (in the ZINE block) */
#define STACK (16u << 10)

static u32 align16(u32 a) { return (a + 15) & ~15u; }

long zb_prepare(const struct zine_args *a)
{
    Z.a = *a;
    struct zine_blk *b = (struct zine_blk *)a->blk;
    Z.blk = b;
    if (a->layout != ZINE_LAYOUT || b->magic != ZB_MAGIC || a->blk_size < sizeof *b)
        return -ZE_INVAL;
    const struct bootinfo *bi = (const struct bootinfo *)0x1000;
    Z.attn = bi->attn;
    extern u32 zb_attn;
    zb_attn = bi->attn & 0xFFFF;     /* for zb_flush, which runs when the low page is no longer ZINE's */
    if (Z.attn & 0x0088)
        zb_mmu_save(Z.mmu);             /* 10.1: the 040/060's MMU and PCR come back as Zenix had them */
    nreg = 0;
    for (u32 i = 0; i < bi->nregions && i < 8; i++)
        if (bi->r[i].attrs & (MEMF_CHIP | MEMF_FAST))
            reg[nreg++] = (struct zp_region){ bi->r[i].attrs, bi->r[i].lower, bi->r[i].upper };
    /* the blob, the block and the volume must not lie in chip RAM: chip RAM is the program's */
    extern char _start[], __bss_end[];
    if (!in_fast((u32)_start, (u32)(__bss_end - _start)) || !in_fast(a->blk, a->blk_size) ||
        !in_fast(a->vol, a->vol_size))
        return -ZE_NODEV;
    /* the walk: every block of every pool (blocks tile their range; a header that is not Zenix's ends it) */
    u8 *p = (u8 *)a->blk + align16(sizeof *b), *end = (u8 *)a->blk + a->blk_size;
    struct zp_range *rng = (struct zp_range *)p;
    p += align16(a->nranges * sizeof *rng);
    struct zp_blk *wb = (struct zp_blk *)p;
    u32 nblk = 0;
    for (u32 i = 0; i < a->nranges; i++) {
        const struct zine_range *r = &a->ranges[i];
        rng[i] = (struct zp_range){ r->lo, r->hi, (u32)(r->chip != 0), nblk, 0 };
        for (u32 x = r->lo; x < r->hi;) {
            const u32 *h = (const u32 *)x;
            u32 sz = h[0] & ~1u;
            if (h[3] != ZHDR_MAGIC || sz < 16 || (sz & 15) || sz > r->hi - x)
                return -ZE_INVAL;
            if ((u8 *)&wb[nblk + 1] > end)
                goto small;
            wb[nblk++] = (struct zp_blk){ x, sz, h[0] & 1 };
            x += sz;
        }
        rng[i].n = nblk - rng[i].first;
    }
    p = (u8 *)&wb[nblk];
    p = (u8 *)align16((u32)p);
    struct zp_out o;
    zmemset(&o, 0, sizeof o);
    o.piece = (struct zp_piece *)p;
    o.piece_cap = nblk + 1;
    p += align16(o.piece_cap * sizeof *o.piece);
    o.chunk = (struct zp_chunk *)p;
    o.chunk_cap = nblk + a->nranges + 1;
    p += align16(o.chunk_cap * sizeof *o.chunk);
    if (p > end)
        goto small;
    struct zp_in in = { reg, nreg, rng, a->nranges, wb, nblk, 0 };
    int e = zp_plan(&in, &o);
    if (e)
        return e;
    if (o.why == ZP_NOFAST)
        return -ZE_NODEV;
    if (o.why == ZP_HOLDS)
        return -ZE_NOSPC;
    Z.piece = o.piece;
    Z.npiece = o.npiece;
    Z.chunk = o.chunk;
    Z.nchunk = o.nchunk;
    Z.reg = reg;
    b->chip_free = o.chip_free, b->fast_free = o.fast_free, b->fast_largest = o.fast_largest;
    Z.save = p;
    p += align16(o.save);
    Z.lib[0] = (u32)p;                  /* the area: libraries and the process (blibs.c) */
    p += AREA + STACK;
    if (p > end)
        goto small;
    Z.stack_top = zb_stack_top = (u32)p;
    return 0;
small:
    b->need = (u32)(p - (u8 *)a->blk) + (u32)(p - (u8 *)a->blk) / 4 + AREA + STACK + (64u << 10);
    return -ZE_NOMEM;
}

/* ---- the way in (7.3) and the start (6), on the ZINE block's stack ---- */
extern void zv_crash(void), zv_spurious(void), zv_irq(void), zv_priv(void), zv_linef(void);

static void cia_save_stop(void)
{
    for (int i = 0; i < 4; i++) {
        Z.cia[0][i] = CIAA(i);
        Z.cia[1][i] = CIAB(i);
    }
    u32 hi = CIAA(CIA_TODHI);           /* most significant first: the counter is latched until the low byte */
    u32 mid = CIAA(CIA_TODMID);
    Z.tod0 = hi << 16 | mid << 8 | CIAA(CIA_TODLO);
    CIAA(CIA_CRA) = 0, CIAA(CIA_CRB) = 0, CIAB(CIA_CRA) = 0, CIAB(CIA_CRB) = 0;   /* every timer stopped (8.1) */
    CIAA(CIA_ICR) = 0x7F, CIAB(CIA_ICR) = 0x7F;
    (void)CIAA(CIA_ICR);
    (void)CIAB(CIA_ICR);
}

/* every DMA and interrupt off, so nothing touches memory while it is copied (9.1 step 1; the blitter is waited for
 * while its DMA is still on: a blit frozen by BLTEN off would go on later into restored memory) */
static void all_off(void)
{
    zb_waitblit();
    INTENA = 0x7FFF;
    DMACON = 0x7FFF;
    INTREQ = 0x7FFF;
    INTREQ = 0x7FFF;
    for (int i = 0; i < 4; i++)
        AUDVOL(i) = 0;
    DSKLEN = 0x4000;
}

static void chips_for_program(void)
{
    u16 *c = (u16 *)ZBLANK;             /* the blank Copper list: null sprites, no planes, black */
    for (int i = 0; i < 16; i++) {
        *c++ = (u16)(0x120 + 2 * i);
        *c++ = (u16)(i & 1 ? ZNULLSPR : 0);
    }
    *c++ = 0x100, *c++ = 0x0200;
    *c++ = 0x180, *c++ = 0x0000;
    *c++ = 0xFFFF, *c++ = 0xFFFE;
    zmemset((void *)ZNULLSPR, 0, 16);
    zb_blank_view();
    ADKCON = 0x7FFF;
    INTREQ = 0x7FFF;
    CIAA(CIA_ICR) = 0x88;               /* CIA-A: only the keyboard's serial port (8.1) */
    INTENA = 0xC000 | 0x2000 | 0x0020 | 0x0008;     /* master, EXTER, VERTB, PORTS */
    DMACON = 0x8000 | 0x0200 | 0x0100 | 0x0080 | 0x0040 | 0x0020;   /* master, planes, Copper, blitter, sprites */
}

/* the display as LoadView(NULL) leaves it (5.3): the blank list, the chips' display state the OS's defaults */
void zb_blank_view(void)
{
    COP1LC = ZBLANK;
    COP2LC = ZBLANK;
    COPJMP1 = 0;
    BPLCON0 = 0x0200;
    BPLCON2 = 0;
    if (Z.aga) {
        FMODE = 0;
        BPLCON3 = 0x0C00;
        BPLCON4 = 0x0011;
    }
    if (Z.ecs)
        BEAMCON0 = Z.pal ? 0x0020 : 0;
    for (int i = 0; i < 8; i++)
        SPRCTL(i) = 0;
}

int zb_hunk_alloc(void *u, zu32 size, zu32 memf, struct zh_seg *out)
{
    (void)u;
    u32 m = zx_AllocMem(size, (memf & (MEMF_CHIP | MEMF_FAST)) | MEMF_PUBLIC);
    if (!m)
        return -ZE_NOMEM;
    out->p = (zu8 *)m;
    out->addr = m;
    out->size = size;
    return 0;
}

void zb_hunk_free(void *u, const struct zh_seg *s)
{
    (void)u;
    zx_FreeMem(s->addr, s->size);
}

void zb_run(void) __attribute__((noreturn));
void zb_run(void)
{
    struct zine_blk *b = Z.blk;
    u32 id = (VPOSR >> 8) & 0x7F;       /* the Agnus/Alice id: ECS and AGA have bit 5 */
    Z.ecs = (id & 0x20) != 0;
    u16 d = DENISEID & 0xFF;
    Z.aga = d == 0xF8 && (DENISEID & 0xFF) == d;
    Z.pal = Z.a.eclock_hz != 715909u;
    all_off();
    cia_save_stop();
    /* every chip piece and every free fast header saved, low memory included */
    u8 *s = Z.save;
    for (u32 i = 0; i < Z.npiece; i++) {
        zmemcpy(s, (void *)Z.piece[i].addr, Z.piece[i].len);
        s += Z.piece[i].len;
    }
    *(volatile u32 *)4 = 0;             /* a reset from here on is a plain cold start */
    /* the vectors (8.2) and the low page */
    u32 *v = (u32 *)0;
    for (int i = 2; i < 256; i++)
        v[i] = (u32)zv_crash;
    v[8] = (u32)zv_priv;
    v[11] = (u32)zv_linef;
    v[24] = (u32)zv_spurious;
    for (int i = 25; i < 32; i++)
        v[i] = (u32)zv_irq;
    zmemset((void *)0x400, 0, 0xC00);
    zb_libs_init();                     /* the memory lists, ExecBase, the libraries, the process (blibs.c) */
    chips_for_program();
    zb_setvbr(0);
    zb_flush();
    /* the program (6) */
    const void *vol = (const void *)Z.a.vol;
    u32 idx;
    if (zv_check(vol, Z.a.vol_size) || zv_find(vol, 0, b->name, &idx))
        zb_stop(ZH_LOAD, ZS_NOFILE);
    const struct zv_ent *e = zv_ent(vol, idx);
    const zu8 *f = (const zu8 *)vol + e->off;
    struct zh_info inf;
    char msg[96];
    if (zh_check(f, e->size, b->name, &inf, msg, sizeof msg))
        zb_stop(ZH_LOAD, ZS_BADFILE);
    struct zh_seg *work = (struct zh_seg *)zx_AllocMem(inf.nhunks * sizeof *work + 8, MEMF_PUBLIC);
    struct zh_ops ops = { zb_hunk_alloc, zb_hunk_free, 0 };
    u32 seg;
    if (!work || zh_load(f, e->size, &inf, &ops, work, &seg))
        zb_stop(ZH_LOAD, ZS_NOMEM);
    if (inf.ovl && zb_ovl_root(idx, f, &inf, work, seg, b->name))     /* an overlaid program (16.6) */
        zb_stop(ZH_LOAD, ZS_NOMEM);
    zx_FreeMem((u32)work, inf.nhunks * sizeof *work + 8);
    Z.seglist = seg;
    u32 stack = b->stack ? (b->stack + 3) & ~3u : 65536;
    u32 sp = zx_AllocMem(stack, MEMF_PUBLIC);
    u32 args = zx_AllocMem(ZB_ARGMAX + 4, MEMF_PUBLIC | MEMF_CLEAR);
    if (!sp || !args)
        zb_stop(ZH_LOAD, ZS_NOMEM);
    u32 arglen = b->arglen < ZB_ARGMAX ? b->arglen : ZB_ARGMAX - 1;
    zmemcpy((void *)args, b->args, arglen);
    u32 usp = sp + stack - 8;
    ((u32 *)usp)[0] = (u32)zx_rts;      /* the program's rts ends the session */
    ((u32 *)usp)[1] = stack;            /* 4(sp): the stack's size */
    u8 *pr = (u8 *)Z.proc;
    *(u32 *)(pr + ZO_Process_pr_Task + ZO_Task_tc_SPLower) = sp;
    *(u32 *)(pr + ZO_Process_pr_Task + ZO_Task_tc_SPUpper) = sp + stack;
    *(u32 *)(pr + ZO_Process_pr_Task + ZO_Task_tc_SPReg) = usp;
    *(u32 *)(pr + ZO_Process_pr_SegList) = 0;
    *(u32 *)(pr + ZO_Process_pr_StackSize) = stack;
    *(u32 *)(pr + ZO_Process_pr_StackBase) = (sp + stack) >> 2;
    *(u32 *)(pr + ZO_Process_pr_ReturnAddr) = usp + 4;     /* "just above the return address" (6; 16.1) */
    *(u32 *)(pr + ZO_Process_pr_Arguments) = args;
    u32 cli = *(u32 *)(pr + ZO_Process_pr_CLI) << 2;
    *(u32 *)(cli + ZO_CommandLineInterface_cli_Module) = seg;
    *(u32 *)(cli + ZO_CommandLineInterface_cli_DefaultStack) = stack / 4;
    zb_flush();                         /* the loaded code (CLAUDE.md) */
    zb_go((seg << 2) + 4, usp, arglen, args, ZEXEC, seg << 2);
}

/* ---- the program's ends ---- */
void zb_stop(u32 how, u32 why)
{
    Z.blk->why = why;
    zr_how = how;
    extern void zs_catch(void);
    zs_catch();
    for (;;)
        ;
}

/* any exception reaching ZINE's vectors (9.1): the vector, the PC and the access address from the CPU's frame (its
 * format: 68020/030 A and B, 68040 7, 68060 4, address errors 2) */
void zb_serial_hex(char tag, const u32 *v, int n);
void zb_crash(u32 *rb) __attribute__((noreturn));
void zb_crash(u32 *rb)
{
    u8 *fr = (u8 *)rb + 60;
    u16 fv = *(u16 *)(fr + 6);
    u32 fmt = fv >> 12;
    struct zine_blk *b = Z.blk;
    b->vec = (fv & 0xFFF) >> 2;
    b->pc = *(u32 *)(fr + 2);
    b->addr = fmt == 0xA || fmt == 0xB ? *(u32 *)(fr + 16) : fmt == 7 ? *(u32 *)(fr + 20) :
              fmt == 4 || fmt == 2 ? *(u32 *)(fr + 8) : 0;
    if (b->vec == 11 && fmt == 4)       /* a 68040's unimplemented FPU instruction: the faulted one's own address */
        b->pc = *(u32 *)(fr + 12);
    if (b->vec == 11 && fmt == 2 && !(b->pc & 1) && b->pc >= 0x1010)    /* the frame's PC is the next instruction's
                                         * (seen in FS-UAE's 68040): the nearest FPU instruction word before it */
        for (u32 k = 4; k <= 16; k += 2)
            if ((*(const u16 *)(b->pc - k) & 0xFFC0) == 0xF200) {
                b->pc -= k;
                break;
            }
    if ((b->vec == 4 || b->vec == 10 || b->vec == 11) && !(b->pc & 1) && b->pc >= 0x1000)
        b->addr = *(const u32 *)b->pc;  /* illegal, line A/F: no access address; the instruction's first four bytes */
    zr_how = ZH_CRASH;
    extern u32 zb_tracing;
    if (zb_tracing) {                   /* the trace build: d0-d7/a0-a6, the SR, the user stack and 8 longs on it */
        u32 usp;
        __asm__ volatile("move.l %%usp,%0" : "=a"(usp));
        u32 v[24];
        for (int i = 0; i < 15; i++)
            v[i] = rb[i];
        v[15] = *(u16 *)fr;
        v[16] = usp;
        for (int i = 0; i < 7; i++)
            v[17 + i] = usp >= 0x1000 ? ((u32 *)usp)[i] : 0;
        zb_serial_hex('R', v, 24);
        for (int i = 17; i < 21; i++)   /* the code before stacked addresses that may be returns */
            if (v[i] >= 0x1000 && !(v[i] & 1) && v[i] < 0x80000000u)
                zb_serial_hex('C', (const u32 *)(v[i] - 32), 12);
    }
    zb_goback();
}

/* The line-F vector (zv_linef, blobs.S; rb: d0-d7/a0-a6, then the CPU's frame). Old programs written for a 68030
 * switch its MMU off or flush it (PMOVE to TC, TT0, TT1, CRP, SRP; PFLUSHA), or read those registers to put them
 * back later; a 68040 or 68060 (and Emu68, which reports a 68040) has no such instructions and takes the line-F
 * vector, where AmigaOS's 68040/68060.library steps over them. With the MMU off (Zenix runs so, zb_mmu_back puts the
 * registers back) they are no-ops: a write to an MMU register is skipped, a read gives 0 (MMU off, no transparent
 * translation), PFLUSH, PLOAD and PTEST do nothing; so do the 68040/060's own PFLUSH, PTEST and PLPA ($F5xx, one
 * word), which a CPU without that MMU (Emu68) also sends here. The instruction's length comes from its addressing mode;
 * (An)+ and -(An) move An as the CPU would. 0: done, the frame's PC past the instruction; 1: not one of these (a
 * crash, named by zine: an FPU instruction this CPU lacks, for one). Format 0 frames only (the 020-060 line-F). */
u32 zb_getusp(void);
void zb_setusp(u32 a);
u32 zb_linef(u32 *rb);
u32 zb_linef(u32 *rb)
{
    u8 *fr = (u8 *)rb + 60;
    u32 pc = *(u32 *)(fr + 2);
    if ((*(u16 *)(fr + 6) >> 12) || (pc & 1) || pc < 0x1000)
        return 1;
    const u16 *ip = (const u16 *)pc;
    u32 op = ip[0], ext = ip[1], mode = op >> 3 & 7, rn = op & 7, len = 4, size;
    if ((op & 0xFF00) == 0xF500) {      /* the 68040/060's PFLUSH, PFLUSHA, PTEST, PLPA (one word): Emu68 has no MMU */
        len = 2;
        goto done;
    }
    if ((op & 0xFFC0) != 0xF000)
        return 1;                       /* not coprocessor 0's general form: not a 68030 MMU instruction */
    u32 kind = ext >> 13, preg = ext >> 10 & 7, rd = ext & 0x200;
    if (kind == 2 && !(ext & 0xFF) && (preg == 0 || preg == 2 || preg == 3))
        size = preg ? 8 : 4;            /* PMOVE TC (long), SRP or CRP (double long) */
    else if (kind == 0 && !(ext & 0xFF) && (preg == 2 || preg == 3))
        size = 4;                       /* PMOVE TT0, TT1 */
    else if (kind == 3 && !(ext & 0x1DFF))
        size = 2;                       /* PMOVE MMUSR */
    else if (kind == 1 || kind == 4)
        size = 0, rd = 0;               /* PFLUSH, PFLUSHA, PLOAD; PTEST: nothing to do, only the length */
    else
        return 1;
    int user = !(*(u16 *)fr & 0x2000);
    u32 *an = rn < 7 ? &rb[8 + rn] : 0, a7 = user ? zb_getusp() : (u32)fr + 8, areg = an ? *an : a7, a = 0;
    if (size == 0 && mode <= 1)
        ;                               /* PFLUSH/PLOAD with a register for the function code (ext's fc field) */
    else if (mode == 0 || mode == 1) {
        if (mode == 1 && rn == 7)
            return 1;
        if (rd)
            rb[mode * 8 + rn] = 0;      /* a register as the target */
    } else if (mode == 2 || mode == 3 || mode == 4) {
        u32 step = size ? size : 0;
        if (mode == 4)
            areg -= step;
        a = areg;
        if (mode == 3)
            areg += step;
        if (an)
            *an = areg;
        else if (user)
            zb_setusp(areg);
        else if (mode != 2)
            return 1;                   /* the supervisor stack moved under the frame: not taken */
    } else if (mode == 5) {
        a = areg + (u32)(s32)(s16)ip[2], len = 6;
    } else if (mode == 6 || (mode == 7 && rn == 3)) {
        u32 bx = ip[2];
        if (bx & 0x100)
            return 1;                   /* the full extension format: not taken */
        u32 x = rb[bx >> 12 & 15];      /* d0-d7 then a0-a6 (a7 as above) */
        if ((bx >> 12 & 15) == 15)
            x = a7;
        if (!(bx & 0x800))
            x = (u32)(s32)(s16)x;
        a = (mode == 6 ? areg : pc + 4) + (x << (bx >> 9 & 3)) + (u32)(s32)(s8)bx, len = 6;
    } else if (mode == 7 && rn == 0) {
        a = (u32)(s32)(s16)ip[2], len = 6;
    } else if (mode == 7 && rn == 1) {
        a = (u32)ip[2] << 16 | ip[3], len = 8;
    } else if (mode == 7 && rn == 2) {
        a = pc + 4 + (u32)(s32)(s16)ip[2], len = 6;
    } else if (mode == 7 && rn == 4 && !rd) {
        len = 4 + (size < 4 ? 2 : size);    /* an immediate written to the register */
    } else
        return 1;
    if (rd && a)
        zmemset((void *)a, 0, size);    /* the register read: 0, as with the MMU off */
done:
    *(u32 *)(fr + 2) = pc + len;
    static u8 said;
    if (!said) {
        said = 1;
        static const char m[] = "ZINE: an MMU instruction (PMOVE, PFLUSH, PTEST) taken as a no-op: the MMU is off\n";
        zb_out(m, sizeof m - 1);
    }
    return 0;
}

/* the trace build: tag and n longs in hex on the serial port */
void zb_serial_hex(char tag, const u32 *v, int n)
{
    static const char hx[] = "0123456789abcdef";
    for (int i = -1; i <= n; i++) {
        char t[10];
        int m = 0;
        if (i < 0)
            t[m++] = tag;
        else if (i == n)
            t[m++] = '\n';
        else {
            t[m++] = ' ';
            for (int k = 0; k < 8; k++)
                t[m++] = hx[v[i] >> (28 - 4 * k) & 15];
        }
        for (int k = 0; k < m; k++) {
            while (!(CUSTOM(0x018) & 0x2000))
                ;
            CUSTOM(0x030) = (u16)(0x100 | (u8)t[k]);
        }
    }
}

/* the way back, path A (9.1 steps 1, 4-6; the sums of step 3 come with build step 5; the 040/060 MMU and PCR first,
 * zb_mmu_back): everything off, Zenix's memory back with low memory last, the chips' register
 * set and the CIAs from Zenix's records (9.2), then the kernel's resume */
void zb_back(void)
{
    struct zine_blk *b = Z.blk;
    if (Z.attn & 0x0088)
        zb_mmu_back(Z.mmu);             /* first: a program's translation must not stand while Zenix's memory returns */
    extern u32 zb_tracing;
    if (zb_tracing)
        zb_screen_report();
    b->how = zr_how;
    if (zr_how == ZH_RETURN || zr_how == ZH_EXIT)
        b->code = (s32)zr_code;
    if (zr_how == ZH_UNSUP) {
        u32 slot = zr_ret6 - 6;
        b->pc = zr_callpc;
        extern u32 zb_tracing;
        if (zb_tracing && (zr_callpc & 1) == 0 && zr_callpc >= 0x1000) {   /* the trace build: the 32 bytes before */
            static const char hx[] = "0123456789abcdef";
            for (u32 a = zr_callpc - 32; a < zr_callpc; a += 2) {
                u16 w = *(const u16 *)a;
                char t[5] = { hx[w >> 12], hx[w >> 8 & 15], hx[w >> 4 & 15], hx[w & 15], ' ' };
                for (int k = 0; k < 5; k++) {
                    while (!(CUSTOM(0x018) & 0x2000))
                        ;
                    CUSTOM(0x030) = (u16)(0x100 | (u8)t[k]);
                }
            }
            while (!(CUSTOM(0x018) & 0x2000))
                ;
            CUSTOM(0x030) = 0x10A;
        }
        for (u32 i = 0; i < 4; i++) {
            const struct zlib *l = (const struct zlib *)Z.lib[i];
            if (slot < (u32)l && (u32)l - slot <= l->negsize) {
                b->lib = i;
                b->lvo = (u32)l - slot;
            }
        }
    }
    all_off();
    COPCON = 0;
    CIAA(CIA_ICR) = 0x7F, CIAB(CIA_ICR) = 0x7F;
    (void)CIAA(CIA_ICR);
    (void)CIAB(CIA_ICR);
    CIAA(CIA_CRA) = 0, CIAA(CIA_CRB) = 0, CIAB(CIA_CRA) = 0, CIAB(CIA_CRB) = 0;
    u32 hi = CIAA(CIA_TODHI), mid = CIAA(CIA_TODMID);
    u32 tod = hi << 16 | mid << 8 | CIAA(CIA_TODLO);
    u32 secs = ((tod - Z.tod0) & 0xFFFFFF) / (Z.pal ? 50 : 60);
    b->secs = secs;
    ADKCON = 0x7FFF;
    ADKCON = (u16)(0x8000 | Z.a.adkcon);
    BLTAFWM = 0xFFFF;
    BLTALWM = 0xFFFF;
    /* Zenix's memory back: every piece but low memory, then low memory (piece 0) last */
    u8 *s = Z.save + Z.piece[0].len;
    for (u32 i = 1; i < Z.npiece; i++) {
        zmemcpy((void *)Z.piece[i].addr, s, Z.piece[i].len);
        s += Z.piece[i].len;
    }
    zmemcpy((void *)Z.piece[0].addr, Z.save, Z.piece[0].len);
    *(volatile u32 *)4 = 0;             /* every later reset a cold start (Zenix's reboot path assumes it) */
    /* the display (9.2): Copper DMA is off */
    if (Z.ecs)
        BEAMCON0 = Z.pal ? 0x0020 : 0;
    DIWSTRT = Z.a.diwstrt;
    DIWSTOP = Z.a.diwstop;
    BPLCON0 = 0x0200;
    for (int i = 0; i < 8; i++)
        SPRCTL(i) = 0;
    COP1LC = Z.a.cop;
    COP2LC = Z.a.cop;
    COPJMP1 = 0;
    SERPER = Z.a.serper;
    POTGO = Z.a.potgo;
    /* the CIAs: ports as they were; CIA-A timer A the 100 Hz tick, ICR timer A and the keyboard; CIA-B's timers the
     * E-clock counter where it stood at the hand-over (loaded while stopped, then the latches 0xFFFF: a high byte
     * written while a timer runs loads only the latch, HRM CIA chapter), so hr_time() does not count the session */
    for (int i = 0; i < 4; i++) {
        CIAA(i ^ 2) = Z.cia[0][i ^ 2];  /* directions first, then the ports */
        CIAB(i ^ 2) = Z.cia[1][i ^ 2];
    }
    u32 latch = (Z.a.eclock_hz + 50) / 100 - 1;
    CIAA(CIA_TALO) = (u8)latch;
    CIAA(CIA_TAHI) = (u8)(latch >> 8);
    CIAA(CIA_CRA) = 0x11;               /* force load, continuous, start */
    CIAA(CIA_ICR) = 0x89;
    u32 c = ~Z.a.hr_at;
    CIAB(CIA_TALO) = (u8)c;
    CIAB(CIA_TAHI) = (u8)(c >> 8);
    CIAB(CIA_TBLO) = (u8)(c >> 16);
    CIAB(CIA_TBHI) = (u8)(c >> 24);
    CIAB(CIA_CRB) = 0x41;               /* count timer A's underflows, continuous, start */
    CIAB(CIA_CRA) = 0x01;
    CIAB(CIA_TALO) = 0xFF;
    CIAB(CIA_TAHI) = 0xFF;
    CIAB(CIA_TBLO) = 0xFF;
    CIAB(CIA_TBHI) = 0xFF;
    zb_flush();
    zb_leave(Z.a.resume, 0, secs);
}

/* ---- interrupts as exec delivers them (8.3) ---- */
#define IV(n) ((struct ziv *)(ZEXEC + ZO_ExecBase_IntVects + 12 * (n)))
extern void zx_chain(void);
static const u16 lvlmask[8] = { 0, 0x0007, 0x0008, 0x0070, 0x0780, 0x1800, 0x2000, 0 };

/* the keyboard, while no program owns it: its byte taken and the handshake given (KDAT low for three beam lines:
 * at least 96 us even in a 31 kHz mode, above the HRM's 85 us), so it does not resynchronise; the byte is dropped */
static void kbd(void)
{
    (void)CIAA(CIA_SDR);
    CIAA(CIA_CRA) |= 0x40;
    zb_wait_lines(3);
    CIAA(CIA_CRA) &= (u8)~0x40;
}

static void chain(struct ziv *iv, u32 pend)
{
    struct zlist *l = (struct zlist *)iv->data;
    for (struct znode *n = l->head; n->succ; n = n->succ)
        if (zb_call(((struct zint *)n)->code, ((struct zint *)n)->data, pend))
            break;
}

void zb_irq(u32 vec)
{
    u32 lv = vec - 24;
    if (lv == 7) {
        if (IV(15)->code == (u32)zx_chain)
            chain(IV(15), 0);
        return;
    }
    if (lv < 1 || lv > 6)
        return;
    u32 pend = INTREQR & INTENAR & lvlmask[lv];
    if (!pend)
        return;                         /* spurious */
    if (lv == 2 && (pend & 0x0008)) {
        u32 icr = CIAA(CIA_ICR);        /* read once: the read clears it */
        u32 done = zb_cia_irq(0, icr);
        if ((icr & 8) && !(done & 8))
            kbd();                      /* the keyboard while no program's handler takes it */
    }
    if (lv == 3 && (pend & 0x0020))
        zb_screen_frame();
    if (lv == 6 && (pend & 0x2000))
        zb_cia_irq(1, CIAB(CIA_ICR));
    for (u32 bit = 0; bit < 15; bit++) {
        u16 m = (u16)(1u << bit);
        if (!(pend & m))
            continue;
        struct ziv *iv = IV(bit);
        if (iv->code == (u32)zx_chain)
            chain(iv, pend);
        else if (iv->code) {
            zb_call(iv->code, iv->data, pend);
            continue;                   /* a handler clears its own request (exec does not) */
        }
        INTREQ = m;                     /* twice: gone before the mask drops on a fast CPU (8.3) */
        INTREQ = m;
    }
}

/* ---- exec's memory (5.2): MemHeaders and MemChunks as exec/memory.h has them, first fit ---- */
_Static_assert(sizeof(struct zmh) == ZO_MemHeader_SIZEOF, "MemHeader");
_Static_assert(sizeof(struct zlib) == ZO_Library_SIZEOF, "Library");
_Static_assert(sizeof(struct zint) == ZO_Interrupt_SIZEOF, "Interrupt");
#define MEMLIST ((struct zlist *)(ZEXEC + ZO_ExecBase_MemList))

void c_Alert(u32 code);

u32 zx_AllocMem(u32 size, u32 attrs)
{
    u32 n = (size + 7) & ~7u, req = attrs & MEMF_REQ;
    if (!size || n < size)
        return 0;
    for (struct zmh *h = (struct zmh *)MEMLIST->head; h->node.succ; h = (struct zmh *)h->node.succ) {
        if ((h->attrs & req) != req)
            continue;
        struct zchunk **pp, **best = 0, *c;
        for (pp = &h->first; (c = *pp); pp = &c->next)
            if (c->bytes >= n) {
                best = pp;
                if (!(attrs & MEMF_REVERSE))
                    break;
            }
        if (!best)
            continue;
        c = *best;
        u32 r;
        if (c->bytes == n) {
            r = (u32)c;
            *best = c->next;
        } else if (attrs & MEMF_REVERSE) {
            c->bytes -= n;
            r = (u32)c + c->bytes;
        } else {
            struct zchunk *nc = (struct zchunk *)((u32)c + n);
            nc->next = c->next;
            nc->bytes = c->bytes - n;
            *best = nc;
            r = (u32)c;
        }
        h->free -= n;
        if (attrs & MEMF_CLEAR)
            zmemset((void *)r, 0, n);
        return r;
    }
    return 0;
}

static struct zmh *header_of(u32 p, u32 n)
{
    for (struct zmh *h = (struct zmh *)MEMLIST->head; h->node.succ; h = (struct zmh *)h->node.succ)
        if (p >= h->lower && p < h->upper && n <= h->upper - p)
            return h;
    return 0;
}

void zx_FreeMem(u32 p, u32 size)
{
    if (!p || !size)
        return;
    u32 n = (size + (p & 7) + 7) & ~7u;
    p &= ~7u;
    struct zmh *h = header_of(p, n);
    if (!h)
        c_Alert(AT_DeadEnd | AN_BadFreeAddr);
    struct zchunk **pp = &h->first, *prev = 0, *c;
    while ((c = *pp) && (u32)c < p) {
        prev = c;
        pp = &c->next;
    }
    if ((c && p + n > (u32)c) || (prev && (u32)prev + prev->bytes > p))
        c_Alert(AT_DeadEnd | AN_FreeTwice);
    struct zchunk *nc = (struct zchunk *)p;
    nc->bytes = n;
    nc->next = c;
    if (c && p + n == (u32)c) {
        nc->bytes += c->bytes;
        nc->next = c->next;
    }
    if (prev && (u32)prev + prev->bytes == p) {
        prev->bytes += nc->bytes;
        prev->next = nc->next;
    } else
        *pp = nc;
    h->free += n;
}

u32 c_AllocAbs(u32 size, u32 loc)
{
    u32 n = (size + (loc & 7) + 7) & ~7u, a = loc & ~7u;
    struct zmh *h = header_of(a, n);
    if (!h || !size)
        return 0;
    struct zchunk **pp, *c;
    for (pp = &h->first; (c = *pp); pp = &c->next) {
        u32 lo = (u32)c, hi = lo + c->bytes;
        if (a < lo || a + n > hi)
            continue;
        if (a + n < hi) {               /* the part above */
            struct zchunk *t = (struct zchunk *)(a + n);
            t->next = c->next;
            t->bytes = hi - a - n;
            c->next = t;
        }
        if (a > lo)                     /* the part below stays */
            c->bytes = a - lo;
        else
            *pp = c->next;
        h->free -= n;
        return a;
    }
    return 0;
}

u32 c_AvailMem(u32 attrs)
{
    u32 req = attrs & MEMF_REQ, sum = 0;
    for (struct zmh *h = (struct zmh *)MEMLIST->head; h->node.succ; h = (struct zmh *)h->node.succ) {
        if ((h->attrs & req) != req)
            continue;
        if (attrs & MEMF_LARGEST) {
            for (struct zchunk *c = h->first; c; c = c->next)
                if (c->bytes > sum)
                    sum = c->bytes;
        } else
            sum += attrs & MEMF_TOTAL ? h->upper - h->lower : h->free;
    }
    return sum;
}

u32 c_TypeOfMem(u32 a)
{
    struct zmh *h = header_of(a, 1);
    return h ? h->attrs : 0;
}

u32 c_AllocVec(u32 size, u32 attrs)
{
    u32 p = size + 4 > size ? zx_AllocMem(size + 4, attrs) : 0;
    if (!p)
        return 0;
    *(u32 *)p = size + 4;
    return p + 4;
}

void c_FreeVec(u32 p)
{
    if (p)
        zx_FreeMem(p - 4, *(u32 *)(p - 4));
}

void c_CopyMem(u32 s, u32 d, u32 n)
{
    zmemcpy((void *)d, (const void *)s, n);
}

/* pools (V39, the NDK's exec_lib.fd): each piece a separate AllocMem, listed in the pool so DeletePool frees all */
struct zpool { u32 req, first; };
u32 c_CreatePool(u32 req, u32 puddle, u32 thresh)
{
    (void)puddle;
    (void)thresh;
    struct zpool *pl = (struct zpool *)zx_AllocMem(sizeof *pl, MEMF_PUBLIC);
    if (pl)
        pl->req = req, pl->first = 0;
    return (u32)pl;
}

u32 c_AllocPooled(u32 pool, u32 size)
{
    struct zpool *pl = (struct zpool *)pool;
    if (!pl || size + 8 < size)
        return 0;
    u32 p = zx_AllocMem(size + 8, pl->req);
    if (!p)
        return 0;
    ((u32 *)p)[0] = pl->first;
    ((u32 *)p)[1] = size + 8;
    pl->first = p;
    return p + 8;
}

void c_FreePooled(u32 pool, u32 mem, u32 size)
{
    struct zpool *pl = (struct zpool *)pool;
    (void)size;
    if (!pl || !mem)
        return;
    for (u32 *pp = &pl->first; *pp; pp = (u32 *)*pp)
        if (*pp == mem - 8) {
            *pp = ((u32 *)(mem - 8))[0];
            zx_FreeMem(mem - 8, ((u32 *)(mem - 8))[1]);
            return;
        }
}

void c_DeletePool(u32 pool)
{
    struct zpool *pl = (struct zpool *)pool;
    if (!pl)
        return;
    while (pl->first) {
        u32 p = pl->first;
        pl->first = ((u32 *)p)[0];
        zx_FreeMem(p, ((u32 *)p)[1]);
    }
    zx_FreeMem(pool, sizeof *pl);
}
