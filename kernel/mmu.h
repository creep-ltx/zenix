/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* DESIGN D41 (docs/d41-memory-protection.md): optional memory protection with the 68030/68040/68060 MMU, identity
 * mapped. kernel/mmu.c; the hooks below are called from mem.c, proc.c, ipc.c, svc.c, drv.c, zine.c, cache.c and main.c.
 * Every hook is cheap when protection is off: the callers test mmu_on first where the path is hot. */
#include "kernel.h"

#define MMU_PG 4096u            /* the page: 4 KB on all three CPUs (D41 section 5) */

extern u8 mmu_on;               /* 0: off (every hook does nothing); 1: on, a 68030; 2: on, a 68040/68060 */

void mmu_setup(const struct bootinfo *bi);      /* the boot, after cache_setup, before any process (section 12) */
void mmu_switch(struct proc *n);                /* schedule(), interrupts masked, before switch_to (section 9) */
void mmu_sysmark(struct proc *p);               /* p is a system process from now on (section 7) */
void mmu_slot_new(struct proc *p);              /* the slot is given to a new process */
int mmu_is_sys(const struct proc *p);           /* a system process (section 7) */
void mmu_upd_gone(void);                        /* mem.c upd_release: the update bundle's pages out of every view */
void mmu_forget(struct proc *p);                /* proc_exit: p's view flipped out, never applied again */
void mmu_lend(struct proc *c);                  /* ipc_recv gave c's request to the current process (section 8) */
void mmu_block(u32 data, u32 len, u32 owner);   /* mem.c: a MEM_USER block was made: valid now if its owner is seen */
void mmu_freed(u32 data, u32 len);              /* mem.c: a block is freed: its whole pages invalid */
void mmu_hand(int back);                        /* ZINE: translation off (0) for the session, back on (1) after */
void mmu_before_reset(void);                    /* machine_reset: translation off, the TTRs cleared */
int mmu_fault(const u8 *frame, u32 pc);         /* exc_c, a user-mode exception: 1 if it was protection (said) */

/* mem.c, for mmu_fault's line: the owner of the block holding a (*used 0: free), or MEM_NOT_POOL outside the pools */
#define MEM_NOT_POOL 0xFFFFFFFFu
u32 mem_owner_of(u32 a, int *used);

/* cache.c (D37), for mmu.c: D37's block over the fast RAM (a CP_* answer, kernel/cache_plan.h) and in io[8] the 16 MB
 * units that hold a board that is not RAM; the hardware table must be there */
int cache_block(const struct bootinfo *bi, int is060, unsigned *base, unsigned *mask, unsigned *io);
