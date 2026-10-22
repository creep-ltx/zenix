/* Kernel entry and low-level glue.
 *
 * The bootblock jumps here at the kernel's PHYSICAL load address
 * (256KB-aligned, ideally fast RAM), supervisor mode, interrupts and DMA
 * off, temporary stack in chip RAM. The kernel is LINKED at virtual
 * 0x80000000, so everything up to the jump into virt_entry must be
 * position-independent (PC-relative).
 *
 * The stub: installs a catch-all early vector table, builds early page
 * tables (full 4GB identity via 32MB early-termination descriptors, plus
 * a 4MB kernel window mapping VA 0x80000000 -> load PA), enables the
 * MMU, then VERIFIES translation actually happens by comparing a marker
 * read via PC-relative (physical) and absolute (virtual) addressing.
 * On a 68EC030 the translation silently doesn't work — that's exactly
 * what the check catches; we refuse cleanly over serial instead of
 * corrupting memory. Then it jumps to the linked virtual address.
 */

#define TC_VALUE 0x82C07760     /* E | SRE | PS=4K | TIA=7 TIB=7 TIC=6 */

    .section .text.start
    .globl  _start
_start:
    move.w  #0x2700,%sr
    lea     _start(%pc),%a5         /* a5 = physical load address */

    /* early vector table: every exception -> early_exc */
    lea     early_vec(%pc),%a0
    move.l  %a0,%d0
    movec   %d0,%vbr
    lea     early_exc(%pc),%a1
    move.l  %a1,%d1
    move.w  #255,%d0
1:  move.l  %d1,(%a0)+
    dbf     %d0,1b

    /* early root table: 4GB identity, 32MB early-termination pages */
    lea     early_rootA(%pc),%a0
    moveq   #0,%d1
    move.w  #127,%d0
2:  move.l  %d1,%d2
    addq.l  #1,%d2                  /* DT=1 page descriptor */
    move.l  %d2,(%a0)+
    add.l   #0x2000000,%d1
    dbf     %d0,2b

    /* kernel window: rootA[64] -> B table, 16 x 256KB pages from load PA */
    lea     early_kernB(%pc),%a1
    move.l  %a1,%d1
    addq.l  #2,%d1                  /* DT=2 table descriptor */
    lea     early_rootA(%pc),%a0
    move.l  %d1,256(%a0)            /* entry 64 (0x80000000 >> 25) */
    move.l  %a5,%d1
    addq.l  #1,%d1
    move.w  #15,%d0
3:  move.l  %d1,(%a1)+
    add.l   #0x40000,%d1
    dbf     %d0,3b

    /* load root pointers and TC — MMU on */
    lea     early_rp(%pc),%a0
    move.l  #0x7FFF0002,(%a0)
    lea     early_rootA(%pc),%a1
    move.l  %a1,4(%a0)
    pmove   (%a0),%srp
    pmove   (%a0),%crp
    pflusha
    lea     early_tc(%pc),%a0
    pmove   (%a0),%tc

    /* Verify translation with a WRITE probe. A read-back test can pass
     * vacuously on machines without MMU: reading unmapped physical
     * 0x800000xx returns open-bus echo — often the very value just on
     * the bus. A write through the VA mapping must land in physical RAM
     * at the scratch location; open bus cannot fake that. */
    lea     early_scratch(%pc),%a0  /* physical view of scratch */
    move.l  %a0,%d0
    sub.l   %a5,%d0
    add.l   #0x80000000,%d0
    move.l  %d0,%a1                 /* virtual view of same scratch */
    clr.l   (%a0)
    move.l  #0xCAFEBABE,%d1
    move.l  %d1,(%a1)               /* write through the MMU */
    cmp.l   (%a0),%d1               /* did it reach physical RAM? */
    bne.s   mmu_dead

    lea     virt_entry,%a0          /* absolute VA */
    jmp     (%a0)

mmu_dead:
    lea     msg_nommu(%pc),%a0
    bsr.s   early_puts
    move.w  #0xF00,0xDFF180
4:  bra.s   4b

early_exc:                          /* any exception during early boot */
    lea     msg_exc(%pc),%a0
    bsr.s   early_puts
    move.w  #0xF0F,0xDFF180
5:  bra.s   5b

/* a0 = NUL-terminated string (PC-relative); clobbers d0/d1/a1 */
early_puts:
    lea     0xDFF000,%a1
    move.w  #368,0x32(%a1)          /* SERPER: 9600 baud PAL */
6:  move.b  (%a0)+,%d0
    beq.s   8f
7:  move.w  0x18(%a1),%d1           /* SERDATR */
    btst    #13,%d1                 /* TBE */
    beq.s   7b
    move.w  #0x100,%d1
    move.b  %d0,%d1
    move.w  %d1,0x30(%a1)           /* SERDAT */
    bra.s   6b
8:  rts

virt_entry:                         /* executing at VA 0x80000000+ now */
    lea     kstack0_top,%sp
    lea     __bss_start,%a0
    lea     __bss_end,%a1
9:  cmp.l   %a1,%a0
    bcc.s   10f
    clr.l   (%a0)+
    bra.s   9b
10: pea     0x1000                  /* bootinfo (chip, identity-mapped) */
    jsr     kmain
11: bra.s   11b

msg_nommu:
    .asciz  "\r\nzenith: MMU translation not functional (68EC030 or no MMU) - halting.\r\n"
msg_exc:
    .asciz  "\r\nzenith: unexpected exception during early boot - halting.\r\n"
    .balign 4

    .text

/* Every kernel entry saves the full user register block (d0-d7/a0-a6
 * ascending; the CPU exception frame follows at +60) and passes its
 * address to a C handler. The handler RETURNS the — possibly moved —
 * block address (signal delivery rewrites/resizes frames), which
 * becomes the SP for the restore. */
    .globl  exc_common
exc_common:
    movem.l %d0-%d7/%a0-%a6,-(%sp)
    move.l  %sp,-(%sp)
    jsr     exc_c
    addq.l  #4,%sp
    move.l  %d0,%sp
    movem.l (%sp)+,%d0-%d7/%a0-%a6
    rte

/* Syscall gate: TRAP #0, num in d0, args in d1-d3, result in d0. */
    .globl  trap0_entry
trap0_entry:
    movem.l %d0-%d7/%a0-%a6,-(%sp)
    move.l  %sp,-(%sp)
    jsr     syscall_c
    addq.l  #4,%sp
    move.l  %d0,%sp
    movem.l (%sp)+,%d0-%d7/%a0-%a6
    rte

/* First dispatch of a forked child: its kstack was built to hold the
 * parent's user registers (with d0=0) and an rte frame. */
    .globl  fork_child_entry
fork_child_entry:
    movem.l (%sp)+,%d0-%d7/%a0-%a6
    rte

/* u32 get_usp(void) / void set_usp(u32) */
    .globl  get_usp
get_usp:
    move.l  %usp,%a1
    move.l  %a1,%d0
    rts
    .globl  set_usp
set_usp:
    move.l  4(%sp),%a1
    move.l  %a1,%usp
    rts

/* Level 2 autovector: CIA-A (timer tick + keyboard) */
    .globl  irq2_entry
irq2_entry:
    movem.l %d0-%d7/%a0-%a6,-(%sp)
    move.l  %sp,-(%sp)
    jsr     irq2_c
    addq.l  #4,%sp
    move.l  %d0,%sp
    movem.l (%sp)+,%d0-%d7/%a0-%a6
    rte

/* Level 5 autovector: Paula serial receive */
    .globl  irq5_entry
irq5_entry:
    movem.l %d0-%d7/%a0-%a6,-(%sp)
    move.l  %sp,-(%sp)
    jsr     irq5_c
    addq.l  #4,%sp
    move.l  %d0,%sp
    movem.l (%sp)+,%d0-%d7/%a0-%a6
    rte

/* void set_vbr(void *table) */
    .globl  set_vbr
set_vbr:
    move.l  4(%sp),%d0
    movec   %d0,%vbr
    rts

/* u16 irq_save(void): mask interrupts, return previous SR */
    .globl  irq_save
irq_save:
    move.w  %sr,%d0
    or.w    #0x0700,%sr
    rts

/* void irq_restore(u16 sr) */
    .globl  irq_restore
irq_restore:
    move.l  4(%sp),%d0
    move.w  %d0,%sr
    rts

/* void ints_on(void) / ints_off(void) / cpu_idle(void) */
    .globl  ints_on
ints_on:
    move.w  #0x2000,%sr
    rts
    .globl  ints_off
ints_off:
    or.w    #0x0700,%sr
    rts
    .globl  cpu_idle
cpu_idle:
    stop    #0x2000
    rts

/* void switch_to(struct proc *old, struct proc *next)
 * struct proc layout (kernel.h): +0 ksp, +4 usp, +8 crp[2], +16 entry.
 * PMOVE to CRP flushes the ATC (MC68030UM 9.5.4), so no pflusha needed.
 * The 030 caches are LOGICAL (virtual-tagged), so they must be cleared
 * when the user address space changes: CACR 0x3919 = clear both caches
 * + keep them enabled with burst and write-allocate. */
    .globl  switch_to
switch_to:
    move.l  4(%sp),%a0              /* old */
    move.l  8(%sp),%a1              /* next */
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  %usp,%a2
    move.l  %a2,4(%a0)
    move.l  %sp,(%a0)
    pmove   8(%a1),%crp             /* switch user address space */
    move.l  #0x3919,%d0
    movec   %d0,%cacr               /* clear logical caches */
    move.l  4(%a1),%a2
    move.l  %a2,%usp
    move.l  (%a1),%sp
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* First dispatch of a new process: switch_to has loaded USP and CRP.
 * Reset the FPU to a clean state, then fabricate a format-0 exception
 * frame with the entry point from the process header and drop to user. */
    .globl  user_first_entry
user_first_entry:
    jsr     fpu_reset
    move.l  current,%a0
    move.w  #0,-(%sp)               /* frame format 0, vector 0 */
    move.l  16(%a0),-(%sp)          /* pc = current->entry */
    move.w  #0x0000,-(%sp)          /* sr: user mode, ints enabled */
    rte

    .section .data
    .balign 16
early_vec:   .space 1024
early_rootA: .space 512
early_kernB: .space 512
early_rp:    .space 8
early_scratch: .space 4
early_tc:    .long TC_VALUE

    .section .bss
    .balign 8
    .space  4096                    /* boot/idle kernel stack (proc 0) */
kstack0_top:
