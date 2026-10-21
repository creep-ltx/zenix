/* Kernel entry and low-level glue (68020+, no MMU, flat 32-bit).
 *
 * The bootblock copies the kernel to its link address (0x40000, chip RAM)
 * and jumps to _start in supervisor mode with interrupts and DMA off.
 * Bootinfo is at 0x1000. There is no MMU: nothing is translated or
 * protected, every address is physical.
 */
    .section .text.start
    .globl  _start
_start:
    move.w  #0x2700,%sr
    lea     kstack_top,%sp
    lea     __bss_start,%a0
    lea     __bss_end,%a1
1:  cmp.l   %a1,%a0
    bcc.s   2f
    clr.l   (%a0)+
    bra.s   1b
2:  pea     0x1000                  /* bootinfo */
    jsr     kmain
3:  bra.s   3b

    .text

/* Every exception without its own handler: save all registers, pass the
 * block (d0-d7/a0-a6 ascending, CPU exception frame at +60) to C. */
    .globl  exc_common
exc_common:
    movem.l %d0-%d7/%a0-%a6,-(%sp)
    move.l  %sp,-(%sp)
    jsr     exc_c
4:  bra.s   4b                      /* exc_c does not return in M0 */

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
