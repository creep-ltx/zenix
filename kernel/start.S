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
    move.l  16(%a5),%d1             /* nreloc */
    beq.s   2f
    move.l  20(%a5),%d0
    lea     (%a5,%d0.l),%a0         /* relocation table */
    move.l  %a5,%d2
1:  move.l  (%a0)+,%d0
    add.l   %d2,(%a5,%d0.l)
    subq.l  #1,%d1
    bne.s   1b
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
6:  lea     kstack_top,%sp
    pea     0x1000                  /* bootinfo */
    jsr     kmain
5:  bra.s   5b

    .text

/* stage0 (boot/stage0.c): position-independent code the bootblock calls while AmigaOS is alive,
 * found through the ZEXE header (+28, patched by tools/mkzexe.py). */
    .section .text.stage0
    .balign 4
    .globl  stage0_blob
stage0_blob:
    .incbin "stage0.bin"             /* found through -I$(BUILD) (Makefile) */
    .balign 4
    .text

/* Every exception without its own handler: save all registers, pass the
 * block (d0-d7/a0-a6 ascending, CPU exception frame at +60) to C. */
    .globl  exc_common
exc_common:
    movem.l %d0-%d7/%a0-%a6,-(%sp)
    move.l  %sp,-(%sp)
    jsr     exc_c
6:  bra.s   6b                      /* exc_c does not return in M0 */

/* Level 2 autovector: CIA-A (timer tick). Passes the interrupted SR so
 * the scheduler only preempts user-mode code. After the 4 saved
 * registers the CPU frame is at 16(%sp): sr.w, pc.l, fmt.w. */
    .globl  irq2_entry
irq2_entry:
    movem.l %d0-%d1/%a0-%a1,-(%sp)
    moveq   #0,%d0
    move.w  16(%sp),%d0
    move.l  %d0,-(%sp)
    jsr     irq2_c
    addq.l  #4,%sp
    movem.l (%sp)+,%d0-%d1/%a0-%a1
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
    .globl  cpu_idle
cpu_idle:
    stop    #0x2000
    rts

/* void cache_enable(int is040plus): instruction cache only. There is no
 * MMU to mark chip RAM cache-inhibited, so the data cache stays off
 * (DMA and copy-back would disagree).
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
    .balign 8
    .space  8192
kstack_top:
