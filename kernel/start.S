/* SPDX-License-Identifier: GPL-2.0-only */
/* Kernel entry and low-level glue (68020+, no MMU, flat 32-bit).
 *
 * The kernel is a ZEXE image (tools/mkzexe.py): linked at address 0 and
 * relocated by itself at boot. The bootblock copies the file (image plus
 * relocation table) to its home - the largest fast RAM region, else chip
 * 0x40000 - and jumps to offset 0 in supervisor mode with interrupts and
 * DMA off. Bootinfo is at 0x1000. There is no MMU: every address is
 * physical.
 */
/* boot stage colours (audit A11, kernel/diag.h): 4 yellow = the kernel runs, 5 orange = relocated, BSS cleared */
#define DIAG_YELLOW 0xFF0
#define DIAG_ORANGE 0xF80
#ifdef DIAG_STOP
#define DIAGSTOP(n)     .if DIAG_STOP == n ; 99: bra.s 99b ; .endif
#else
#define DIAGSTOP(n)
#endif
    .section .text.start
    .globl  _start
_start:
    bra.w   _entry
    .ascii  "ZEXE"
    .long   0                       /* image_size   (patched by mkzexe) */
    .long   0                       /* mem_size */
    .long   0                       /* nreloc */
    .long   0                       /* reloc_off */
    .long   0                       /* stack_size */
    .long   0                       /* reserved */
_entry:
    move.w  #0x2700,%sr
    move.w  #DIAG_YELLOW,0xDFF180   /* stage 4 */
    DIAGSTOP(4)
    lea     _start(%pc),%a5         /* a5 = where we really are */
    move.l  16(%a5),%d1             /* the table's length in longwords (tools/mkzexe.py --delta) */
    beq.s   2f
    move.l  20(%a5),%d0
    lea     (%a5,%d0.l),%a0         /* relocation table: byte distances from one relocated word to the next */
    move.l  %a5,%a1                 /* the next word to relocate: distances count from the image's start */
    move.l  %a5,%d2
1:  moveq   #0,%d0
    move.b  (%a0)+,%d0              /* 1-127: a distance in 2-byte units; 0: the end */
    beq.s   5f
    bpl.s   3f
    and.b   #0x7F,%d0               /* bit 7: the top 7 bits of 23, the low 16 follow */
    swap    %d0
    move.b  (%a0)+,%d1
    lsl.w   #8,%d1
    move.b  (%a0)+,%d1
    move.w  %d1,%d0
3:  add.l   %d0,%a1
    add.l   %d0,%a1
    add.l   %d2,(%a1)
    bra.s   1b
5:
    /* The relocation loop wrote into code. Nothing below may run from a stale instruction cache (or a stale Emu68
     * translation): flush, then reach the next instruction through a register jump so the pipeline / the JIT
     * fetches the patched bytes. CPU type from bootinfo (0x1000, AttnFlags): the bootblock filled it. A 68000 has
     * no cache and no CACR: skip (it is refused below, after the BSS clear). */
    move.w  0x1000,%d0
    and.w   #0x008E,%d0             /* 68020 or better? */
    beq.s   2f
    and.w   #0x0088,%d0             /* 68040 or 68060? */
    bne.s   11f
    movec   %cacr,%d0
    or.w    #0x0008,%d0             /* 020/030: CACR CI = clear the instruction cache, enable bits kept */
    movec   %d0,%cacr
    bra.s   12f
11: .word   0xF4F8                  /* cpusha bc: push and invalidate both caches (never cinva: Emu68 would drop dirty data) */
12: lea     2f(%pc),%a0
    jmp     (%a0)
2:  lea     __bss_start,%a0         /* relocated now */
    lea     __bss_end,%a1
3:  cmp.l   %a1,%a0
    bcc.s   4f
    clr.l   (%a0)+
    bra.s   3b
4:  move.w  #DIAG_ORANGE,0xDFF180   /* stage 5 */
    DIAGSTOP(5)
    /* needs a 68020 or better: AttnFlags (bootinfo +0) bits 1-3 and 7 */
    move.w  0x1000,%d0
    and.w   #0x008E,%d0
    bne.w   6f
    lea     msg_cpu(%pc),%a0
    lea     0xDFF000,%a1
    move.w  #30,0x32(%a1)           /* SERPER: 115200 baud PAL */
7:  move.b  (%a0)+,%d0
    beq.s   9f
8:  move.w  0x18(%a1),%d1           /* SERDATR */
    btst    #13,%d1                 /* TBE */
    beq.s   8b
    move.w  #0x100,%d1
    move.b  %d0,%d1
    move.w  %d1,0x30(%a1)
    bra.s   7b
9:  move.l  #DIAG_ORANGE,-(%sp)     /* split screen: orange (stage 5) over red */
    bsr.w   diag_halt
msg_cpu:
    .asciz  "\r\nzenix: needs a 68020 or better (this is a 68000/68010) - halting.\r\n"
    .balign 2

/* void diag_halt(u16 top): stop for good with the screen split - the stage colour reached on top, red from line 150
 * down - so a phone photo shows both "it stopped on an error" and where (audit A11, kernel/diag.h). The beam counter
 * (VPOSR/VHPOSR as one long: V8 in bit 16, V7-V0 in bits 15-8) works without DMA; line 150 is inside the picture on
 * PAL and NTSC. Plain 68000 code: the CPU check above uses it. Interrupts must be off already. */
    .globl  diag_halt
diag_halt:
    move.w  6(%sp),%d1
    lea     0xDFF000,%a0
1:  move.l  4(%a0),%d0
    lsr.l   #8,%d0
    and.w   #0x1FF,%d0
    cmp.w   #150,%d0
    bcc.s   2f
    move.w  %d1,0x180(%a0)
    bra.s   1b
2:  move.w  #0xF00,0x180(%a0)
    bra.s   1b
6:
enter_kmain:                        /* (tools/check-init.py: the one call into the boot's span from outside it) */
    lea     kstack_top,%sp
    pea     0x1000                  /* bootinfo */
    jsr     kmain
5:  bra.s   5b

    .text

/* stage0 (boot/stage0.c): position-independent code the bootblock calls while AmigaOS is alive,
 * found through the ZEXE header (+28, patched by tools/mkzexe.py). */
    .section .zinit.stage0,"ax"      /* DESIGN D45: given back with the boot's span */
    .balign 4
    .globl  stage0_blob
stage0_blob:
    .incbin "stage0.bin"             /* found through -I$(BUILD) (Makefile) */
stage0_blob_end:                    /* (tools/check-init.py: stage0's own code, run before the kernel) */
    .globl  stage0_blob_end
    .balign 4
    .text

/* Every exception without its own handler: save all registers, pass the
 * block (d0-d7/a0-a6 ascending, CPU exception frame at +60) to C. */
    .globl  exc_common
exc_common:
    movem.l %d0-%d7/%a0-%a6,-(%sp)
    move.l  %sp,-(%sp)
    jsr     exc_c                   /* returns only for the debugger (DESIGN D54): a step, a breakpoint, a stray trace */
    addq.l  #4,%sp
    movem.l (%sp)+,%d0-%d7/%a0-%a6
    rte

/* Level 2 autovector: CIA-A (timer tick, keyboard). Saves every register like the system-call entry (DESIGN D16
 * section 5: a handler can then be sent to from a tick that cut into user code, sig_deliver editing the block) and
 * passes the block: d0-d7/a0-a6 ascending, the CPU frame (sr.w, pc.l, fmt.w) at +60. */
    .globl  irq2_entry
irq2_entry:
    movem.l %d0-%d7/%a0-%a6,-(%sp)
    move.l  %sp,-(%sp)
    jsr     irq2_c
    addq.l  #4,%sp
    movem.l (%sp)+,%d0-%d7/%a0-%a6
    rte

/* Level 3 autovector: vertical blank (applies Copper-list scroll updates) */
    .globl  irq3_entry
irq3_entry:
    movem.l %d0-%d1/%a0-%a1,-(%sp)
    jsr     irq3_c
    movem.l (%sp)+,%d0-%d1/%a0-%a1
    rte

/* Level 5 autovector: Paula serial receive */
    .globl  irq5_entry
irq5_entry:
    movem.l %d0-%d1/%a0-%a1,-(%sp)
    jsr     irq5_c
    movem.l (%sp)+,%d0-%d1/%a0-%a1
    rte

/* The other interrupt vectors (24 spurious, 25 level 1, 28 level 4, 30 level 6, 31 NMI): no source of them is
 * enabled, so one that arrives anyway is logged once and returned from (irq_other_c), never taken for a fault of
 * whatever process happened to run. The format/vector word is at 22(%sp) after the 4 saved registers. */
    .globl  irq_other_entry
irq_other_entry:
    movem.l %d0-%d1/%a0-%a1,-(%sp)
    moveq   #0,%d0
    move.w  22(%sp),%d0
    move.l  %d0,-(%sp)
    jsr     irq_other_c
    addq.l  #4,%sp
    movem.l (%sp)+,%d0-%d1/%a0-%a1
    rte

/* System call gate: TRAP #0. Block = d0-d7/a0-a6 ascending; the C
 * handler writes the result into the saved d0. */
    .globl  trap0_entry
trap0_entry:
    movem.l %d0-%d7/%a0-%a6,-(%sp)
    move.l  %sp,-(%sp)
    jsr     syscall_c
    addq.l  #4,%sp
    movem.l (%sp)+,%d0-%d7/%a0-%a6
    rte

/* void switch_to(struct proc *old, struct proc *next)
 * struct proc: +0 ksp, +4 usp, +8 entry. Callee-saved registers go on
 * the old kernel stack; the new process resumes where it last switched
 * (or in user_first_entry). */
    .globl  switch_to
switch_to:
    move.l  4(%sp),%a0
    move.l  8(%sp),%a1
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    move.l  %usp,%a2
    move.l  %a2,4(%a0)
    move.l  %sp,(%a0)
    move.l  4(%a1),%a2
    move.l  %a2,%usp
    move.l  (%a1),%sp
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* First run of a new process: build a format-0 exception frame (user
 * mode, interrupts on, pc = entry) and drop to user mode. */
    .globl  user_first_entry
user_first_entry:
    move.l  current,%a0
    move.w  #0,-(%sp)               /* format 0, vector 0 */
    move.l  8(%a0),-(%sp)           /* pc = current->entry */
    move.w  #0,-(%sp)               /* sr: user mode, mask 0 */
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

/* u16 irq_save2(void): mask interrupt levels 1-2 only (CIAs, Gayle IDE)
 * but leave the serial receive interrupt (level 5) running; never
 * lowers an existing mask. Returns the previous SR. */
    .globl  irq_save2
irq_save2:
    move.w  %sr,%d0
    move.w  %d0,%d1
    and.w   #0x0700,%d1
    cmp.w   #0x0200,%d1
    bcc.s   1f
    move.w  %d0,%d1
    and.w   #0xF8FF,%d1
    or.w    #0x0200,%d1
    move.w  %d1,%sr
1:  rts

    .globl  ints_on
ints_on:
    move.w  #0x2000,%sr
    rts
    .globl  ints_off
ints_off:
    or.w    #0x0700,%sr
    rts
    .globl  get_usp
get_usp:
    move.l  %usp,%a0
    move.l  %a0,%d0
    rts
    .globl  set_usp
set_usp:
    move.l  4(%sp),%a0
    move.l  %a0,%usp
    rts
    .globl  cpu_idle
cpu_idle:
    stop    #0x2000
    rts

/* void cache_enable(int is040plus): instruction cache only, the data cache
 * off: the 020/030 always (no MMU to keep chip RAM out of it), Emu68, and a
 * 040/060 where kernel/cache.c does not turn its data cache on (D37).
 * 020/030: CACR bit 0 = enable I-cache, bit 3 = clear it.
 * 040/060: push and invalidate both caches with CPUSHA BC (0xF4F8), then
 * CACR bit 15 = enable I-cache. Never CINVA (0xF4D8) on the data cache: on
 * Emu68 (and a copyback 040/060) it throws away unwritten data. */
    .globl  cache_enable
cache_enable:
    tst.l   4(%sp)
    bne.s   1f
    move.l  #0x00000009,%d0
    movec   %d0,%cacr
    rts
1:  .word   0xF4F8                  /* cpusha bc */
    move.l  #0x00008000,%d0
    movec   %d0,%cacr
    rts

/* DESIGN D37 (docs/d37-data-cache.md section 5): the 68040/68060's cache registers, for kernel/cache.c. Every one runs
 * in supervisor mode with interrupts masked. The control registers are written by their MOVEC encodings (as zenixboot
 * and ZINE's blob do), with the illegal-instruction and line-F vectors caught: an EC/LC part may lack one. */

/* long cpu_pcr(void): the 68060's PCR, or -1 where MOVEC PCR does not exist (a 68040) */
    .globl  cpu_pcr
cpu_pcr:
    moveq   #-1,%d0
    lea     1f(%pc),%a0
    bsr.s   ctrap_on
    .long   0x4E7A0808              /* movec pcr,d0 */
1:  bra.s   ctrap_off

/* ctrap_on: the illegal (4) and line-F (11) vectors to a0 until ctrap_off; a trap goes on at a0 with the stack as at
 * the call of the routine that called ctrap_on (its own return address on top). Uses a1, a5 (saved), d1. */
ctrap_on:
    move.l  (%sp)+,%d1              /* our return address */
    move.l  %a5,-(%sp)
    movec   %vbr,%a1
    move.l  0x10(%a1),-(%sp)
    move.l  0x2C(%a1),-(%sp)
    move.l  %a0,ctrap_pc
    lea     ctrap(%pc),%a0
    move.l  %a0,0x10(%a1)
    move.l  %a0,0x2C(%a1)
    move.l  %sp,%a5                 /* the stack to go back to */
    move.l  %d1,-(%sp)
    rts
ctrap:
    move.l  %a5,%sp                 /* the exception's frame dropped */
    move.l  ctrap_pc,-(%sp)
    rts
/* ctrap_off: the two vectors back; returns from the routine that called ctrap_on */
ctrap_off:
    movec   %vbr,%a1
    move.l  (%sp)+,0x2C(%a1)
    move.l  (%sp)+,0x10(%a1)
    move.l  (%sp)+,%a5
    rts

/* int cache_apply(const u32 *r): r[0] DTT0, r[1] DTT1, r[2] TCR, r[3] CACR, r[4] PCR (-1: not written). Both caches
 * pushed and invalidated first (cpusha bc; never cinva), the instruction TTRs cleared. 0 when every register took
 * its value; -1 when one does not exist here (then CACR was not written: the caches stay as they were). */
    .globl  cache_apply
cache_apply:
    move.l  4(%sp),%a0
    move.l  %a2,-(%sp)
    move.l  %a0,%a2
    lea     9f(%pc),%a0
    bsr.s   ctrap_on
    nop
    .word   0xF4F8                  /* cpusha bc */
    nop
    move.l  16(%a2),%d0
    cmp.l   #-1,%d0
    beq.s   1f
    .long   0x4E7B0808              /* movec d0,pcr */
1:  move.l  8(%a2),%d0
    .long   0x4E7B0003              /* movec d0,tc */
    moveq   #0,%d0
    .long   0x4E7B0004              /* movec d0,itt0 */
    .long   0x4E7B0005              /* movec d0,itt1 */
    move.l  (%a2),%d0
    .long   0x4E7B0006              /* movec d0,dtt0 */
    move.l  4(%a2),%d0
    .long   0x4E7B0007              /* movec d0,dtt1 */
    move.l  12(%a2),%d0
    movec   %d0,%cacr
    nop
    moveq   #0,%d0
    bsr.s   ctrap_off2
    move.l  (%sp)+,%a2
    rts
9:  moveq   #-1,%d0
    bsr.s   ctrap_off2
    move.l  (%sp)+,%a2
    rts
ctrap_off2:                         /* ctrap_off for a caller that keeps going: d0 kept */
    move.l  (%sp)+,%d1
    movec   %vbr,%a1
    move.l  (%sp)+,0x2C(%a1)
    move.l  (%sp)+,0x10(%a1)
    move.l  (%sp)+,%a5
    move.l  %d1,-(%sp)
    rts

/* u32 cache_probe(u32 *p, u32 on, u32 off): the self-test (D37 5.5) on the 16-byte line at p, with CACR "on" (the
 * data cache enabled) and "off" (the same without it). Between the steps nothing but p is touched. Bit 0: a read was
 * served from the cache (a line loaded, memory changed behind it, the old value read); bit 1: a write that hit the
 * line stayed in the cache (memory still old: copyback). The long at p is put back; the data cache is left on, pushed
 * and invalidated. */
    .globl  cache_probe
cache_probe:
    move.l  4(%sp),%a0
    move.l  8(%sp),%d0
    move.l  12(%sp),%d1
    movem.l %d2-%d5,-(%sp)
    movec   %d1,%cacr
    nop
    .word   0xF478                  /* cpusha dc: the stack's lines out, everything invalid */
    nop
    move.l  (%a0),%d4               /* the long as it was (memory) */
    move.l  #0x5A5A0001,(%a0)       /* B in memory */
    nop
    movec   %d0,%cacr
    nop
    move.l  (%a0),%d2               /* B, a line loaded if cacheable */
    movec   %d1,%cacr
    nop
    move.l  #0x5A5A0002,(%a0)       /* A in memory, behind the line */
    nop
    movec   %d0,%cacr
    nop
    move.l  (%a0),%d3               /* B from the cache, A from memory */
    move.l  #0x5A5A0003,(%a0)       /* C: a hit; copyback keeps it in the line */
    nop
    movec   %d1,%cacr
    nop
    move.l  (%a0),%d5               /* memory: C written through, A held back */
    .word   0xF478                  /* cpusha dc: C pushed, the line invalid */
    nop
    move.l  %d4,(%a0)               /* the old long back, into memory */
    nop
    movec   %d0,%cacr
    nop
    moveq   #0,%d0
    cmp.l   #0x5A5A0001,%d3
    bne.s   1f
    or.b    #1,%d0
1:  cmp.l   #0x5A5A0003,%d5
    beq.s   2f
    or.b    #2,%d0
2:  movem.l (%sp)+,%d2-%d5
    rts

/* void cache_reset_state(void): before a software reset, after cache_apply took (cache.c), interrupts masked: CACR 0
 * and at once everything pushed (nothing can dirty a line in between), then TCR and the four TTRs 0, as a hardware
 * reset leaves them (the ROM starts with no cache set-up of Zenix's; the 060's PCR keeps ESS) */
    .globl  cache_reset_state
cache_reset_state:
    moveq   #0,%d0
    movec   %d0,%cacr
    .word   0xF4F8                  /* cpusha bc */
    nop
    .long   0x4E7B0003              /* movec d0,tc */
    .long   0x4E7B0004              /* itt0 */
    .long   0x4E7B0005              /* itt1 */
    .long   0x4E7B0006              /* dtt0 */
    .long   0x4E7B0007              /* dtt1 */
    rts

/* void cache_push_lines(u32 lo, u32 hi): every 16-byte data cache line in [lo, hi) pushed and invalidated (cpushl dc;
 * lo and hi multiples of 16) */
    .globl  cache_push_lines
cache_push_lines:
    move.l  4(%sp),%a0
    move.l  8(%sp),%d0
    bra.s   2f
1:  .word   0xF468                  /* cpushl dc,(a0) */
    lea     16(%a0),%a0
2:  cmp.l   %d0,%a0
    bcs.s   1b
    nop
    rts

/* void cache_flush_code(void): call after writing code that will run
 * (a loaded program, a relocated image). 020/030: CACR bit 3 (CI) clears
 * the instruction cache, the enable bits are kept. 040/060 (and Emu68,
 * which reports an 040): CPUSHA BC, which also makes Emu68's JIT drop
 * its translations of the old bytes. The CPU type comes from bootinfo
 * (0x1000, AttnFlags bits 3 and 7), low chip RAM the kernel keeps. */
    .globl  cache_flush_code
cache_flush_code:
    move.w  0x1000,%d0
    and.w   #0x0088,%d0
    bne.s   1f
    movec   %cacr,%d0
    or.w    #0x0008,%d0
    movec   %d0,%cacr
    rts
1:  .word   0xF4F8                  /* cpusha bc */
    rts

    .section .bss
    .balign 4
ctrap_pc:                           /* ctrap_on's resume address */
    .space  4
    .balign 8
    .globl  kstack_bottom
kstack_bottom:                      /* pid 0's stack guard lies here (proc.c) */
    .space  8192
kstack_top:
