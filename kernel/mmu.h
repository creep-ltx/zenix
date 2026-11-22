/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* DESIGN D41 (docs/d41-memory-protection.md): optional memory protection with the 68030/68040/68060 MMU, identity
 * mapped; DESIGN D60 stage 1: a tree per process. kernel/mmu.c; the hooks below are called from mem.c, proc.c, ipc.c, vfs.c, gcon.c, syscall.c, drv.c, zine.c, cache.c and main.c.
 * Every hook is an inline gate on mmu_on and the table mmu_ops (below): nothing when protection is off. */
#include "kernel.h"

#ifdef MMU_PAGE8
#define MMU_PG 8192u            /* D60 9.2 item 7: the 8 KB page variant, measured in stage 1 (make DIAG=-DMMU_PAGE8) */
#else
#define MMU_PG 4096u            /* the page: 4 KB on all three CPUs (D41 section 5) */
#endif

extern u8 mmu_on;               /* 0: off (every hook does nothing); 1: on, a 68030; 2: on, a 68040/68060; written only
                                 * by mmu_arm and mmu_before_reset_ (tools/check-init.py) */

void mmu_setup(const struct bootinfo *bi);      /* the boot, after cache_setup, before any process (section 12) */
void mmu_sysmark(struct proc *p);               /* p is a system process from now on (section 7) */
void mmu_slot_new(struct proc *p);              /* the slot is given to a new process */
int mmu_is_sys(const struct proc *p);           /* a system process (section 7) */
void mmu_disarm(void);                          /* D45 13: the table back to the stubs (the release, protection off) */

/* DESIGN D45 section 13: the runtime code lies in the span .zhwmmu, given back after the boot when protection is off.
 * It is reached only through this table: mmu_ops points at resident stubs until mmu_setup arms it (at the span's own
 * table, mmu_ops_on) and again after the release; the gates below test mmu_on first (for speed: the stubs make a missed
 * test harmless). Armed exactly when mmu_on is set, and then the span is never given back. */
struct mmu_ops {
    void (*sw)(struct proc *);
    void (*forget)(struct proc *);
    void (*lend)(struct proc *);
    void (*redo)(struct proc *);
    int (*block)(u32, u32, u32);
    void (*upd_gone)(void);
    void (*freed)(u32, u32, u32);
    void (*hand)(int);
    void (*before_reset)(void);
    int (*fault)(const u8 *, u32);
};
extern const struct mmu_ops *mmu_ops;           /* &the stubs, or &the span's table while protection is on */

/* schedule(), interrupts masked, before switch_to (section 9) */
static inline __attribute__((always_inline)) void mmu_switch(struct proc *n) { if (__builtin_expect(mmu_on, 0)) mmu_ops->sw(n); }
/* proc_exit: p's tree given back, its view out of every server's */
static inline __attribute__((always_inline)) void mmu_forget(struct proc *p) { if (__builtin_expect(mmu_on, 0)) mmu_ops->forget(p); }
/* ipc_recv gave c's request to the current process (section 8) */
static inline __attribute__((always_inline)) void mmu_lend(struct proc *c) { if (__builtin_expect(mmu_on, 0)) mmu_ops->lend(c); }
/* seg_disown: p's code is its private copy now (no longer global): its tree made again */
static inline __attribute__((always_inline)) void mmu_redo(struct proc *p) { if (__builtin_expect(mmu_on, 0)) mmu_ops->redo(p); }
/* mem.c: a MEM_USER block was made: valid now in its owner's tree */
static inline __attribute__((always_inline)) void mmu_block(u32 data, u32 len, u32 owner) { if (__builtin_expect(mmu_on, 0)) mmu_ops->block(data, len, owner); }
/* mem.c upd_release: the update bundle's pages out of every view */
static inline __attribute__((always_inline)) void mmu_upd_gone(void) { if (__builtin_expect(mmu_on, 0)) mmu_ops->upd_gone(); }
/* mem.c: a block is freed: its whole pages invalid in every tree (a shared segment's owner: every global entry too) */
static inline __attribute__((always_inline)) void mmu_freed(u32 data, u32 len, u32 owner) { if (__builtin_expect(mmu_on, 0)) mmu_ops->freed(data, len, owner); }
/* ZINE: translation off (0) for the session, back on (1) after */
static inline __attribute__((always_inline)) void mmu_hand(int back) { if (__builtin_expect(mmu_on, 0)) mmu_ops->hand(back); }
/* machine_reset: translation off, the TTRs cleared */
static inline __attribute__((always_inline)) void mmu_before_reset(void) { if (__builtin_expect(mmu_on, 0)) mmu_ops->before_reset(); }
/* exc_c, a user-mode exception: 1 if it was protection (said) */
static inline __attribute__((always_inline)) int mmu_fault(const u8 *frame, u32 pc) { return __builtin_expect(mmu_on, 0) ? mmu_ops->fault(frame, pc) : 0; }

/* mem.c, for mmu_fault's line: the owner of the block holding a (*used 0: free), or MEM_NOT_POOL outside the pools */
#define MEM_NOT_POOL 0xFFFFFFFFu
u32 mem_owner_of(u32 a, int *used);

/* cache.c (D37), for mmu.c: D37's block over the fast RAM (a CP_* answer, kernel/cache_plan.h) and in io[8] the 16 MB
 * units that hold a board that is not RAM; the hardware table must be there */
int cache_block(const struct bootinfo *bi, int is060, unsigned *base, unsigned *mask, unsigned *io);
