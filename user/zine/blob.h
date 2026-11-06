/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* ZINE's blob (DESIGN D29 sections 3, 5, 7.3, 8, 9.1, 9.2; step 3): Zenix's own small AmigaOS. Built from this tree
 * with the kernel and /bin/zine (kernel/zine.h's interface), run by the kernel in supervisor mode with interrupts
 * masked. Freestanding C and assembly (blobs.S); the plain cores (hunk.c, zvol.c, zplan.c) are shared with /bin/zine
 * and the host tests. Written from the public interface documents only (the RKRM, the HRM, the AmigaDOS Manual; the
 * NDK 3.2's include and .fd files for names and numbers): never from AmigaOS code. */
#include "zcore.h"

typedef zu8 u8;
typedef zu16 u16;
typedef zu32 u32;
typedef int32_t s32;
typedef int8_t s8;
typedef int16_t s16;
#include "../../kernel/zine.h"
#include "amigaofs.h"
#include "zplan.h"

/* the custom chips and the CIAs (the HRM's register map) */
#define CUSTOM(o)   (*(volatile u16 *)(0xDFF000 + (o)))
#define CUSTOML(o)  (*(volatile u32 *)(0xDFF000 + (o)))
#define DMACONR     CUSTOM(0x002)
#define VPOSR       CUSTOM(0x004)
#define VHPOSR      CUSTOM(0x006)
#define ADKCONR     CUSTOM(0x010)
#define INTENAR     CUSTOM(0x01C)
#define INTREQR     CUSTOM(0x01E)
#define SERPER      CUSTOM(0x032)
#define POTGO       CUSTOM(0x034)
#define BLTAFWM     CUSTOM(0x044)
#define BLTALWM     CUSTOM(0x046)
#define DENISEID    CUSTOM(0x07C)
#define COP1LC      CUSTOML(0x080)
#define COP2LC      CUSTOML(0x084)
#define COPJMP1     CUSTOM(0x088)
#define DIWSTRT     CUSTOM(0x08E)
#define DIWSTOP     CUSTOM(0x090)
#define DMACON      CUSTOM(0x096)
#define INTENA      CUSTOM(0x09A)
#define INTREQ      CUSTOM(0x09C)
#define ADKCON      CUSTOM(0x09E)
#define DSKLEN      CUSTOM(0x024)
#define COPCON      CUSTOM(0x02E)
#define BPLCON0     CUSTOM(0x100)
#define BPLCON2     CUSTOM(0x104)
#define BPLCON3     CUSTOM(0x106)
#define BPLCON4     CUSTOM(0x10C)
#define BEAMCON0    CUSTOM(0x1DC)
#define FMODE       CUSTOM(0x1FC)
#define COLOR(n)    CUSTOM(0x180 + 2 * (n))
#define SPRCTL(n)   CUSTOM(0x142 + 8 * (n))
#define AUDVOL(n)   CUSTOM(0x0A8 + 0x10 * (n))
#define CIAA(r)     (*(volatile u8 *)(0xBFE001 + ((u32)(r) << 8)))
#define CIAB(r)     (*(volatile u8 *)(0xBFD000 + ((u32)(r) << 8)))
enum { CIA_PRA, CIA_PRB, CIA_DDRA, CIA_DDRB, CIA_TALO, CIA_TAHI, CIA_TBLO, CIA_TBHI, CIA_TODLO, CIA_TODMID, CIA_TODHI,
       CIA_UNUSED, CIA_SDR, CIA_ICR, CIA_CRA, CIA_CRB };

/* exec/memory.h */
#define MEMF_PUBLIC   (1u << 0)
#define MEMF_CHIP     (1u << 1)
#define MEMF_FAST     (1u << 2)
#define MEMF_LOCAL    (1u << 8)
#define MEMF_24BITDMA (1u << 9)
#define MEMF_KICK     (1u << 10)
#define MEMF_CLEAR    (1u << 16)
#define MEMF_LARGEST  (1u << 17)
#define MEMF_REVERSE  (1u << 18)
#define MEMF_TOTAL    (1u << 19)
#define MEMF_REQ      (MEMF_PUBLIC | MEMF_CHIP | MEMF_FAST | MEMF_LOCAL | MEMF_24BITDMA | MEMF_KICK)
#define AT_DeadEnd    0x80000000u
#define AN_FreeTwice  0x01000009u
#define AN_BadFreeAddr 0x0100000Fu
/* exec/nodes.h */
#define NT_TASK      1
#define NT_MESSAGE   5
#define NT_INTERRUPT 2
#define NT_MSGPORT   4
#define NT_LIBRARY   9
#define NT_MEMORY    10
#define NT_RESOURCE  8
#define NT_PROCESS   13

/* exec's structures (exec/nodes.h, lists.h, memory.h, libraries.h, interrupts.h): gcc lays them out for the 68k as
 * the Amiga's compilers do (longs on even addresses), checked against amigaofs.h's offsets */
struct znode { struct znode *succ, *pred; u8 type; s8 pri; char *name; };
struct zlist { struct znode *head, *tail, *tailpred; u8 type, pad; };
struct zchunk { struct zchunk *next; u32 bytes; };
struct zmh { struct znode node; u16 attrs; struct zchunk *first; u32 lower, upper, free; };
struct zlib { struct znode node; u8 flags, pad; u16 negsize, possize, version, revision; char *idstring; u32 sum;
              u16 opencnt; };
struct zint { struct znode node; u32 data, code; };
struct ziv { u32 data, code; struct znode *node; };

/* where things are: ExecBase in the low page (chip RAM: 0x400-0xFFF is ZINE's; 9.3's capture code is not built, the
 * owner dropped path B), its jump table below it; the blank Copper list and the null sprite above it */
#define ZEXEC      0x0800u
#define ZEXEC_SLOTS 152u                /* exec's vectors up to -912 (the NDK 3.2's exec_lib.fd) */
#define ZBLANK     0x0B00u              /* the blank Copper list */
#define ZNULLSPR   0x0B80u              /* 16 zero bytes */
#define EB(o, t)   (*(volatile t *)(ZEXEC + (o)))

/* the session's state (blob.c); everything else is in the ZINE block */
struct zstate {
    struct zine_args a;
    struct zine_blk *blk;
    u8 *save;                           /* the saved pieces' bytes, in the ZINE block */
    struct zp_piece *piece;
    u32 npiece;
    const struct zp_chunk *chunk;       /* the program's free memory (the plan) */
    u32 nchunk;
    const struct zp_region *reg;        /* bootinfo's regions, as the plan numbers them */
    u32 stack_top;                      /* the supervisor stack's top */
    u8 cia[2][4];                       /* the CIAs' PRA, PRB, DDRA, DDRB at the hand-over */
    u32 tod0;                           /* CIA-A's TOD at the hand-over */
    u16 attn;
    u8 aga, ecs, pal;
    u32 shown;                          /* the screen ZINE shows (OpenScreen), 0: none */
    u32 lib[4];                         /* exec, graphics, dos, intuition bases (ZINE_LIB_*) */
    u32 proc, seglist;
    u32 out_fh, in_fh;                  /* the file handles (BPTRs) */
    u32 hport;                          /* every file handle's fh_Type: the handler port that answers packets */
    u32 ioerr;
    u32 cur_in, cur_out;                /* a command dos Execute runs (16.11): its Input(), Output(); 0 the console's */
    u8 blitter;
    u32 mmu[9];                         /* 040/060 (zb_mmu_save): which were read (bit 0 TC, ITT0/1, DTT0/1, URP, SRP;
                                         * bit 1 the 060's PCR), then their values in that order */
};
extern struct zstate Z;
extern u32 zr_how, zr_code, zr_ret6, zr_callpc, zr_d1;   /* set by the stubs in blobs.S on the way out */

/* blobs.S */
void zb_setvbr(u32 v);
void zb_mmu_save(u32 *m);               /* 040/060: the MMU's registers and the 060's PCR as Zenix has them */
void zb_mmu_back(const u32 *m);         /* and back, after the program (it may have switched the MMU or PCR) */
void zb_flush(void);
void zb_go(u32 pc, u32 usp, u32 d0, u32 a0, u32 a6, u32 a3) __attribute__((noreturn));
void zb_leave(u32 resume, u32 d0, u32 d1) __attribute__((noreturn));
void zb_goback(void) __attribute__((noreturn));
u32 zb_call(u32 code, u32 data, u32 d1);   /* a handler or server: a0 custom, a1 data, a5 code, a6 ExecBase */
u32 zb_putch(u32 proc, u32 *a3, u32 c);    /* RawDoFmt's PutChProc */
void zx_unsup(void);
extern const u8 zx_rts[];             /* the program's return address */
void zb_jumptable(u32 base, u32 slots, const u16 *lvo, void (*const *fn)(void), u32 n);

/* blob.c, blibs.c */
void zb_back(void) __attribute__((noreturn));
void zb_stop(u32 how, u32 why) __attribute__((noreturn));
void zb_libs_init(void);
u32 zb_out(const char *s, u32 n);
int zb_say_text(const char *pre, u32 p, const char *post);
u32 c_TypeOfMem(u32 a);                 /* blob.c: the memory attributes of a, 0 outside the program's memory */
u32 zb_cia_irq(int c, u32 icr);
void zb_screen_frame(void);
void zb_screen_report(void);
void zb_wait_lines(u32 n);
void zb_waitblit(void);
void zb_blank_view(void);
u32 zx_AllocMem(u32 size, u32 attrs);
/* the loader's memory (struct zh_ops of hunk.h): AllocMem and FreeMem from the program's lists */
struct zh_seg;
int zb_hunk_alloc(void *u, u32 size, u32 memf, struct zh_seg *out);
void zb_hunk_free(void *u, const struct zh_seg *s);
struct zh_info;
/* an overlaid program's root was loaded (d29 16.6): its hunk table, the overlay table's copy, the open file handle,
 * the first hunk's overlay fields; 0, or -ZE_NOMEM with nothing kept */
int zb_ovl_root(u32 ent, const u8 *f, const struct zh_info *inf, const struct zh_seg *work, u32 seg,
                const char *name);
void zx_FreeMem(u32 p, u32 size);
/* libraries from files, the exec calls that build them, semaphores, audio.device (blibload.c; d29 16.8) */
u32 zb_libcall(u32 fn, u32 a6, u32 d0, u32 a0, u32 d1, u32 a1);    /* blobs.S: d2-d7/a2-a6 kept */
u32 zb_lib_open(const char *name, u32 version);
void zb_lib_close(u32 lib);
int zb_lib_flush(void);                 /* AllocMem short of memory: unused libraries expunged; 1 when one went */
/* blibs.c: 1 when name is a file (of the volume or RAM:), assigns followed; where gets the path it was found as */
int zb_file_exists(const char *name, char *where, u32 max);
u32 c_LoadSeg(const char *name, u32 tab, u32 fh);
u32 c_UnLoadSeg(u32 seg);
u32 c_FindName(struct zlist *l, const char *name);
void c_Enqueue(struct zlist *l, struct znode *n);
void c_Remove(struct znode *n);
void zb_io_done(u8 *io);
extern u32 zb_audio_base;
u32 zb_audio_open(u8 *io, u32 unit, u32 flags);
void zb_audio_close(u8 *io);
void zb_audio_beginio(u8 *io);
void zb_audio_abort(u8 *io);
int zb_audio_pending(u8 *io);
void zb_audio_init(u32 base);
void *zmemset(void *d, int c, u32 n);
void *zmemcpy(void *d, const void *s, u32 n);
