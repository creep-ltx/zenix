/* SPDX-License-Identifier: GPL-2.0-only */
/* mmuleft: an AmigaOS program for tools/zenixboot-test.sh's a1200-030mmu setup (audit 2026-11-12 A6-14). It leaves the
 * 68030's translation on, as Enforcer, MMULib or CPU FASTROM do, before zenixboot runs: CRP an early-termination page
 * descriptor at address 0 (every address mapped to itself, no tables in memory), TC with E set and the 4 KB layout
 * Zenix itself uses, TT1 on for the first 16 MB. zenixboot's hand-over must switch all of it off before the kernel
 * copies itself; the kernel's own check then finds nothing left on. Position independent; tools/zenixboot-test.sh
 * assembles it and wraps it as a one-hunk executable. On anything but a 68030 it does nothing. */
    .text
start:
    move.l  4.w,%a6
    btst    #2,297(%a6)             /* AttnFlags: AFF_68030 */
    beq.s   1f
    move.w  296(%a6),%d0
    and.w   #0x0088,%d0             /* not a 68040/060 */
    bne.s   1f
    lea     super(%pc),%a5
    jsr     -30(%a6)                /* Supervisor */
1:  moveq   #0,%d0
    rts
super:
    lea     crp(%pc),%a0
    .long   0xF0104C00              /* pmove (a0),crp */
    lea     tt1(%pc),%a0
    .long   0xF0100C00              /* pmove (a0),tt1 */
    lea     tc(%pc),%a0
    .long   0xF0104000              /* pmove (a0),tc: translation on */
    rte
    .balign 4
crp:    .long   0x7FFF0001, 0       /* no limit, DT 1: a page descriptor at 0, every address to itself */
tc:     .long   0x80C07760          /* E, 4 KB pages, IS 0, TIA 7, TIB 7, TIC 6 */
tt1:    .long   0x00008107          /* the first 16 MB, every function code, RWM */
