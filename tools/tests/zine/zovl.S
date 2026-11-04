/* SPDX-License-Identifier: GPL-2.0-only */
/* zovl: an overlaid AmigaOS program (docs/d29-zine.md 16.6), made into a hunk file by tools/mkovltest.py: the root
 * (.text: the overlay header and an overlay supervisor written from the Guru Book's description, 22.2.10.7;
 * .rootdata: a data hunk), node A (.ova, hunk slot 2) and node B (.ovb, slot 2, its BSS in slot 3), siblings at
 * level 1, then 16 bytes of data after the last node. It checks the fields LoadSeg wrote, calls A, B and A again
 * through the supervisor (each load replaces the other: Seek on the overlay handle, LoadSeg(NULL, table, handle)
 * with the answer read in d1), that B's slot 3 is cleared by A's load, reads the trailing data through the
 * handle, unloads the last node and compares AvailMem with the start. With an argument starting with b it calls
 * entry 2, whose seek offset points at the trailing data: LoadSeg fails, the supervisor raises AN_BadOverlay.
 * Each section is position-independent; a longword to relocate is marked by a symbol R32_<hunk>_<name> and holds
 * the offset in that hunk (mkovltest.py writes the relocations). Exit 0 when everything held. */
#include "zt.i"
#define LVO_Seek     -66
#define LVO_Read     -42
#define LVO_LoadSeg  -150
#define LVO_Alert    -108
#define OFFSET_BEGINNING -1
#define OFFSET_END   1
#define AN_BadOverlay 0x0700000C
#define V_exec 0
#define V_dos  4
#define V_mem  8
#define V_arg  12
#define V_buf  16

    .text
r0:
    bra.w   main                    /* the entry; offsets 4-23 as the Guru Book's 19.3.2.2 lays them out */
    .long   0x0000ABCD
ovfh:   .long 0                     /* the overlay file handle (BPTR) */
ovtab:  .long 0                     /* the overlay table's copy (APTR) */
htab:   .long 0                     /* the hunk table (BPTR) */
gvec:   .long 0                     /* the global vector */
main:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    lea     vars(%pc),%a4
    move.b  (%a0),V_arg(%a4)
    move.l  4.w,%a6
    move.l  %a6,V_exec(%a4)
    lea     dosname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,V_dos(%a4)
    beq     f20
    move.l  %d0,%a6
    jsr     LVO_Output(%a6)
    move.l  %d0,%d7                 /* the output, for the nodes too */
    move.l  ovfh(%pc),%d0
    beq     f26
    move.l  ovtab(%pc),%d0
    beq     f26
    move.l  htab(%pc),%d0
    beq     f26
    lea     m_hdr(%pc),%a0
    bsr     puts
    move.l  V_exec(%a4),%a6
    moveq   #0,%d1
    jsr     LVO_AvailMem(%a6)
    move.l  %d0,V_mem(%a4)
    cmp.b   #0x62,V_arg(%a4)        /* "b": the bad entry */
    beq     bad
    moveq   #0,%d0                  /* entry 0: node A */
    bsr     ovly
    cmp.l   #0x41,%d0
    bne     f21
    moveq   #8,%d0                  /* entry 1: node B, in A's place */
    bsr     ovly
    cmp.l   #0x42,%d0
    bne     f22
    moveq   #0,%d0                  /* A again */
    bsr     ovly
    cmp.l   #0x41,%d0
    bne     f21
    move.l  htab(%pc),%d0           /* slot 3, B's BSS, cleared by A's load */
    lsl.l   #2,%d0
    move.l  %d0,%a0
    tst.l   12(%a0)
    bne     f23
    lea     m_slot(%pc),%a0
    bsr     puts
    move.l  V_dos(%a4),%a6          /* the data after the last node, through the overlay handle */
    move.l  ovfh(%pc),%d1
    moveq   #-16,%d2
    moveq   #OFFSET_END,%d3
    jsr     LVO_Seek(%a6)
    tst.l   %d0
    bmi     f24
    move.l  ovfh(%pc),%d1
    lea     V_buf(%a4),%a0
    move.l  %a0,%d2
    moveq   #16,%d3
    jsr     LVO_Read(%a6)
    cmp.l   #16,%d0
    bne     f24
    lea     V_buf(%a4),%a0
    lea     trailer(%pc),%a1
    moveq   #3,%d0
1:  cmpm.l  (%a0)+,(%a1)+
    dbne    %d0,1b
    bne     f24
    lea     m_trail(%pc),%a0
    bsr     puts
    bsr     unload                  /* the last node freed as the supervisor would */
    move.l  V_exec(%a4),%a6
    moveq   #0,%d1
    jsr     LVO_AvailMem(%a6)
    cmp.l   V_mem(%a4),%d0
    bne     f25
    lea     m_mem(%pc),%a0
    bsr     puts
    moveq   #0,%d0
    bra     done
bad:
    moveq   #16,%d0                 /* entry 2: no node where it points */
    bsr     ovly
    moveq   #30,%d0                 /* not reached under ZINE: the alert ends the program */
    bra     done
f20: moveq  #20,%d0
    bra     done
f21: moveq  #21,%d0
    bra.s   fail
f22: moveq  #22,%d0
    bra.s   fail
f23: moveq  #23,%d0
    bra.s   fail
f24: moveq  #24,%d0
    bra.s   fail
f25: moveq  #25,%d0
    bra.s   fail
f26: moveq  #26,%d0
fail:
    move.l  %d0,%d2
    lea     m_fail(%pc),%a0         /* the step is the return code */
    bsr     puts
    move.l  %d2,%d0
done:
    move.l  %d0,%d2
    move.l  V_dos(%a4),%d0
    beq.s   1f
    move.l  %d0,%a1
    move.l  V_exec(%a4),%a6
    jsr     LVO_CloseLibrary(%a6)
1:  move.l  %d2,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* the overlay supervisor: d0 the entry's longword offset in the overlay table. Loads the entry's node unless its
 * level shows that ordinate already, freeing first the hunks from the entry's initial hunk on (the deeper levels'
 * ordinates cleared), then calls the symbol with a6 dos and d7 the output. AN_BadOverlay when the load fails. */
ovly:
    movem.l %d2-%d7/%a2-%a3/%a5-%a6,-(%sp)
    move.l  ovtab(%pc),%a2
    move.l  (%a2),%d1               /* the offset to the entries */
    add.l   %d0,%d1
    lsl.l   #2,%d1
    lea     0(%a2,%d1.l),%a3        /* the entry */
    move.l  12(%a3),%d1             /* its level */
    lsl.l   #2,%d1
    lea     0(%a2,%d1.l),%a5        /* that level's ordinate */
    move.l  16(%a3),%d1
    cmp.l   (%a5),%d1
    beq     8f
    move.l  %d1,(%a5)+
1:  tst.l   (%a5)
    beq.s   2f
    clr.l   (%a5)+
    bra.s   1b
2:  move.l  htab(%pc),%d1
    lsl.l   #2,%d1
    move.l  %d1,%a5                 /* the hunk table */
    move.l  20(%a3),%d1             /* the initial hunk */
    lsl.l   #2,%d1
    lea     0(%a5,%d1.l),%a2
    move.l  -4(%a2),%d6             /* the hunk before it: its link gets the node */
    lsl.l   #2,%d6
    move.l  V_exec(%a4),%a6
3:  move.l  (%a2)+,%d0
    beq.s   4f
    lsl.l   #2,%d0
    move.l  %d0,%a1
    subq.l  #4,%a1
    move.l  (%a1),%d0
    jsr     LVO_FreeMem(%a6)
    bra.s   3b
4:  move.l  V_dos(%a4),%a6
    move.l  ovfh(%pc),%d1
    move.l  (%a3),%d2               /* the seek offset */
    moveq   #OFFSET_BEGINNING,%d3
    jsr     LVO_Seek(%a6)
    tst.l   %d0
    bmi.s   7f
    moveq   #0,%d1
    move.l  htab(%pc),%d2
    move.l  ovfh(%pc),%d3
    moveq   #0,%d0
    jsr     LVO_LoadSeg(%a6)
    tst.l   %d1                     /* the answer in d1, as the original supervisor reads it */
    beq.s   7f
    move.l  %d6,%a0
    move.l  %d1,(%a0)
8:  move.l  htab(%pc),%d1
    lsl.l   #2,%d1
    move.l  %d1,%a5
    move.l  24(%a3),%d1             /* the symbol's hunk */
    lsl.l   #2,%d1
    move.l  0(%a5,%d1.l),%d1
    lsl.l   #2,%d1
    add.l   28(%a3),%d1             /* + its offset (from the link longword) */
    move.l  %d1,%a0
    move.l  V_dos(%a4),%a6
    jsr     (%a0)
    movem.l (%sp)+,%d2-%d7/%a2-%a3/%a5-%a6
    rts
7:  move.l  V_exec(%a4),%a6
    move.l  #AN_BadOverlay,%d7
    jsr     LVO_Alert(%a6)
    moveq   #-1,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a3/%a5-%a6
    rts

/* the last node unloaded: every hunk from slot 2 on freed, the table cleared, the root's last link cut */
unload:
    move.l  V_exec(%a4),%a6
    move.l  htab(%pc),%d0
    lsl.l   #2,%d0
    move.l  %d0,%a2
    lea     8(%a2),%a3
1:  move.l  (%a3),%d0
    beq.s   2f
    clr.l   (%a3)+
    lsl.l   #2,%d0
    move.l  %d0,%a1
    subq.l  #4,%a1
    move.l  (%a1),%d0
    jsr     LVO_FreeMem(%a6)
    bra.s   1b
2:  move.l  4(%a2),%d0
    lsl.l   #2,%d0
    move.l  %d0,%a0
    clr.l   (%a0)
    move.l  ovtab(%pc),%a0
    clr.l   4(%a0)
    rts

/* puts: a0 a text ending in 0, to d7 */
puts:
    movem.l %d0-%d3/%a0-%a1/%a6,-(%sp)
    move.l  %a0,%d2
    moveq   #-1,%d3
1:  addq.l  #1,%d3
    tst.b   0(%a0,%d3.l)
    bne.s   1b
    move.l  %d7,%d1
    move.l  V_dos(%a4),%a6
    jsr     LVO_Write(%a6)
    movem.l (%sp)+,%d0-%d3/%a0-%a1/%a6
    rts

dosname: .asciz "dos.library"
m_hdr:   .asciz "zovl: LoadSeg wrote the overlay handle, the overlay table and the hunk table into the root\n"
m_slot:  .asciz "zovl: node B's BSS slot cleared when A came back\n"
m_trail: .asciz "zovl: the data after the last node read through the overlay handle\n"
m_mem:   .asciz "zovl: memory as before\n"
m_fail:  .asciz "zovl: failed (the return code names the step)\n"
trailer: .ascii "ZOVL-TRAILER-OK!"
    .even
vars:   .space 32

    .section .rootdata,"aw"
rd0:
rootmsg: .asciz "hello from the root's data hunk"
    .even

/* node A, hunk slot 2: a line from its own hunk and from the root's data hunk (both relocated) */
    .section .ova,"ax"
ova0:
entA:
    move.l  amsgp(%pc),%a0
    bsr.s   aputs
    move.l  rootp(%pc),%a0
    bsr.s   aputs
    lea     anl(%pc),%a0
    bsr.s   aputs
    moveq   #0x41,%d0
    rts
aputs:
    movem.l %d0-%d3/%a0-%a1,-(%sp)
    move.l  %a0,%d2
    moveq   #-1,%d3
1:  addq.l  #1,%d3
    tst.b   0(%a0,%d3.l)
    bne.s   1b
    move.l  %d7,%d1
    jsr     LVO_Write(%a6)
    movem.l (%sp)+,%d0-%d3/%a0-%a1
    rts
R32_2_amsg:
amsgp:  .long amsg - ova0
R32_1_root:
rootp:  .long rootmsg - rd0
amsg:   .asciz "zovl: node A ran; the root's data says: "
anl:    .asciz "\n"
    .even

/* node B, slot 2 with its BSS hunk (chip RAM) in slot 3: the BSS is clear, takes a value and gives it back */
    .section .ovb,"ax"
ovb0:
entB:
    move.l  bssp(%pc),%a0
    tst.l   (%a0)
    bne.s   9f
    tst.l   12(%a0)
    bne.s   9f
    move.l  #0x12345678,(%a0)
    cmp.l   #0x12345678,(%a0)
    bne.s   9f
    movem.l %d2-%d3,-(%sp)
    move.l  %d7,%d1
    lea     bmsg(%pc),%a0
    move.l  %a0,%d2
    moveq   #bmsgend-bmsg,%d3
    jsr     LVO_Write(%a6)
    movem.l (%sp)+,%d2-%d3
    moveq   #0x42,%d0
    rts
9:  moveq   #0x4F,%d0
    rts
R32_3_bss:
bssp:   .long 0
bmsg:   .ascii "zovl: node B ran; its BSS hunk was clear\n"
bmsgend:
    .even
