/* Kernel entry and low-level glue (68020+, no MMU, flat 32-bit).
 *
 * The kernel is a ZEXE image (tools/mkzexe.py): linked at address 0 and
 * relocated by itself at boot. The bootblock copies the file (image plus
 * relocation table) to its home - the largest fast RAM region, else chip
 * 0x40000 - and jumps to offset 0 in supervisor mode with interrupts and
 * DMA off. Bootinfo is at 0x1000. There is no MMU: every address is
 * physical.
 */
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
2:  lea     __bss_start,%a0         /* relocated now */
    lea     __bss_end,%a1
3:  cmp.l   %a1,%a0
    bcc.s   4f
    clr.l   (%a0)+
    bra.s   3b
4:  lea     kstack_top,%sp
    pea     0x1000                  /* bootinfo */
    jsr     kmain
5:  bra.s   5b

    .text

/* Every exception without its own handler: save all registers, pass the
 * block (d0-d7/a0-a6 ascending, CPU exception frame at +60) to C. */
    .globl  exc_common
exc_common:
    movem.l %d0-%d7/%a0-%a6,-(%sp)
    move.l  %sp,-(%sp)
    jsr     exc_c
6:  bra.s   6b                      /* exc_c does not return in M0 */

/* Level 2 autovector: CIA-A (timer tick) */
    .globl  irq2_entry
irq2_entry:
    movem.l %d0-%d1/%a0-%a1,-(%sp)
    jsr     irq2_c
    movem.l (%sp)+,%d0-%d1/%a0-%a1
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

/* void cache_enable(void): 68020 has only an instruction cache.
 * CACR bit 0 = enable, bit 3 = clear. */
    .globl  cache_enable
cache_enable:
    move.l  #0x00000009,%d0
    movec   %d0,%cacr
    rts

    .section .bss
    .balign 8
    .space  8192
kstack_top:
