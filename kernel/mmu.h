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
void mmu_slot_new(struct proc *p);              /* the slot is given to a new process */
void mmu_disarm(void);                          /* D45 13: the table back to the stubs (the release, protection off) */

/* DESIGN D45 section 13: the runtime code lies in the span .zhwmmu, given back after the boot when protection is off.
 * It is reached only through this table: mmu_ops points at resident stubs until mmu_setup arms it (at the span's own
 * table, mmu_ops_on) and again after the release; the gates below test mmu_on first (for speed: the stubs make a missed
 * test harmless). Armed exactly when mmu_on is set, and then the span is never given back. */
/* D60 stage 2 (docs/d60-full-protection.md 5.1): the kernel's record of a call while protection is on. The message as
 * the kernel keeps it comes first (queue link, state, the stamped ids; the caller's groups right after it, as
 * UMSG_GROUPS_AT reads them): the caller's message is read once at call() and written once at its end (result and
 * a[0..3]; for the kernel's own messages also kflags), never trusted in between. The server works on a copy in its
 * inbox (slot). Internal, not frozen. */
#define NKCOPY 6                /* small buffers copied into a slot (two paths and a ustat; a netreq and four) */
struct kcopy { u32 at; u16 len, off; u8 wr, pad[3]; };     /* the buffer's address, its bytes, where in the slot */
struct krec {
    struct umsg m;
    struct umsg_groups g;
    u16 gpad;                   /* (the longs below aligned) */
    struct umsg *um;            /* the caller's message (in its memory, or the kernel's on its stack) */
    u32 slot;                   /* the server's copy, 0 while none (queued, or answered) */
    u32 orig[4];                /* a[] as sent: an argument the kernel rewrote to a copy gets its value back */
    u8 kp;                      /* KP_* */
    u8 rw;                      /* bit i: a[i] rewritten */
    u8 ncopy, nwin;             /* copies (c[]), windows (in mmu.c's table) */
    u8 srv;                     /* the server's slot index (procs[]) while slot is set */
    u8 heap;                    /* from the kernel heap (a call made while the process's own record is in use) */
    u8 done;                    /* answered by the server's reply (its copies and windows go back to the client) */
    u8 sj;                      /* the slot's number in the server's inbox */
    u8 ex;                      /* D60 stage 3: the port is PEER_EXACT (its server asks for what it touches, 5.8): set
                                 * by ipc_recv as the port is when the server takes the request */
    u8 ndv, dvw;                /* SYS_callv: the client's declared buffers (dva/dvn), bit i: buffer i written */
    struct kcopy c[NKCOPY];
    u32 dva[NCALLB], dvn[NCALLB];
};
/* sock.c's table of each SOCK_* op's pointers (the server reads R, writes W): the checks there, the declaration here */
#define D_BUFR  0x01
#define D_BUFW  0x02
#define D_ADDRR 0x04
#define D_ADDRW 0x08
#define D_VALR  0x10
#define D_VALW  0x20
extern const u8 sock_dirs[];
extern u32 mmu_winners;         /* bit n: procs[n] holds a window of a request (resident) */
extern u32 mmu_syncing;         /* bit n: procs[n] is in a system call with a window's copy page given back to its
                                 * client (D60 5.7); resident, tested at every system call's end */

struct mmu_ops {
    void (*sw)(struct proc *);
    void (*forget)(struct proc *);
    void (*redo)(struct proc *);
    int (*block)(u32, u32, u32);
    void (*upd_gone)(void);
    void (*freed)(u32, u32, u32);
    void (*hand)(int);
    void (*before_reset)(void);
    int (*fault)(const u8 *, u32);
    struct umsg *(*rec)(struct umsg *, u32);
    void (*unrec)(struct umsg *);
    int (*give)(struct krec *, u32 *);
    void (*take)(struct krec *);
    void (*end)(struct krec *);
    int (*callok)(struct proc *, u32, u32, int);
    void (*unsync)(void);
    struct umsg *(*held)(struct umsg *);
    int (*grant)(struct proc *, u32, u32, u32);
    void (*ungrant)(struct proc *, u32);
    long (*peer)(struct krec *, u32, u32, int);
    long (*shm)(u32, u32, u32);
    int (*shmfree)(u32);
    long (*callv)(u32, u32, u32);
    long (*ramdisk)(u32, u32, u32);
    void (*postbuf)(struct port *, u32, struct port *, u32, u32);
    long (*drv)(u32, void *, u32);      /* D60 stage 4: drv.c's DRV_AUDIO and DRV_PARXFER (op, the caller's claim, b) */
};
long drv_span_(u32 op, void *claim, u32 a);
/* D60 stage 3, SYS_callv: the declaration of the call being made (syscall.c sets it, mmu_rec takes it; resident) */
extern const struct ucalldecl *mmu_decl;
extern const struct mmu_ops *mmu_ops;
#define G_IO 0x40u              /* mmu_grant: a page of registers (inhibited, serialized; CI on the 030) */
#define G_RO 0x04u              /* mmu_grant: read only */
/* D60 stage 3 (3.1 items 2, 4, 6): [lo, hi) (rounded out to whole pages) valid in p's tree from now on, beside its view:
 * a claimed device's registers, its claim's DMA memory, a shared region it took. 0, or -ENOMEM. Nothing without
 * protection */
static inline int mmu_grant(struct proc *p, u32 lo, u32 hi, u32 how) { return __builtin_expect(mmu_on, 0) ? mmu_ops->grant(p, lo, hi, how) : 0; }
static inline void mmu_ungrant(struct proc *p, u32 lo) { if (__builtin_expect(mmu_on, 0)) mmu_ops->ungrant(p, lo); }
int drv_dmamem(u32 a, u32 *size);       /* drv.c: current holds a claim whose DMA memory starts at a (its bytes) */
extern const struct mmu_ops *mmu_ops;           /* &the stubs, or &the span's table while protection is on */

/* schedule(), interrupts masked, before switch_to (section 9) */
static inline __attribute__((always_inline)) void mmu_switch(struct proc *n) { if (__builtin_expect(mmu_on, 0)) mmu_ops->sw(n); }
/* proc_exit: p's tree given back, its view out of every server's */
static inline __attribute__((always_inline)) void mmu_forget(struct proc *p) { if (__builtin_expect(mmu_on, 0)) mmu_ops->forget(p); }
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

/* D60 stage 2, ipc.c: call()'s record of the caller's message m (kp: KP_*): m itself while protection is off; 0 if no
 * record could be had (a nested call with the kernel heap full) */
static inline __attribute__((always_inline)) struct umsg *mmu_rec(struct umsg *m, u32 kp) { return __builtin_expect(mmu_on, 0) ? mmu_ops->rec(m, kp) : m; }
/* syscall.c, at the end of every system call: the windows' copy pages made the server's again (D60 5.7) */
static inline __attribute__((always_inline)) void mmu_unsync(void) { if (__builtin_expect(mmu_syncing, 0)) mmu_ops->unsync(); }

/* mem.c, for mmu_fault's line: the owner of the block holding a (*used 0: free), or MEM_NOT_POOL outside the pools */
#define MEM_NOT_POOL 0xFFFFFFFFu
u32 mem_owner_of(u32 a, int *used);

/* cache.c (D37), for mmu.c: D37's block over the fast RAM (a CP_* answer, kernel/cache_plan.h) and in io[8] the 16 MB
 * units that hold a board that is not RAM; the hardware table must be there */
int cache_block(const struct bootinfo *bi, int is060, unsigned *base, unsigned *mask, unsigned *io);
