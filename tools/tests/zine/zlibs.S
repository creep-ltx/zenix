/* SPDX-License-Identifier: GPL-2.0-only */
/* zlibs: what Latex's Ltx-Snigelsaft does with the SID player library it carries (docs/d29-zine.md 16.8), with a
 * library of ZINE's own (ztlib.S, embedded here): writes it to RAM:, Executes "Ram:Assign LIBS: Ram:", opens it
 * (OpenLibrary loads it from RAM:, InitResident with auto-init: MakeLibrary, InitStruct, its init code), calls its
 * functions, closes it, Executes "C:Assign LIBS: SYS:LIBS/" and deletes the file. Around that: an OpenLibrary while
 * LIBS: is a directory without it (fails), one asking for a newer version (fails), one after the delete (the library
 * stays loaded, as on AmigaOS), RemLibrary (its Expunge: the segment list back, unloaded), one after that (fails);
 * DeleteFile of a missing file; the semaphores; audio.device's channels; Paula's channel 0 interrupt through
 * SetIntVector and CIA-B's timer A through ciab.resource, as a SID player plays; AvailMem at the end as at the start.
 * Exit 0 when everything held; else the return code names the step (and a line says it). PC-relative code. */
#include "zt.i"
#define LVO_Execute     -222
#define LVO_DeleteFile  -72
#define LVO_Open        -30
#define LVO_Close       -36
#define LVO_IoErr       -132
#define LVO_Read        -42
#define LVO_UnLoadSeg   -156
#define LVO_RemLibrary  -402
#define LVO_FindTask    -294
#define LVO_SetIntVector -162
#define LVO_OpenDevice  -444
#define LVO_CloseDevice -450
#define LVO_DoIO        -456
#define LVO_OpenResource -498
#define LVO_InitSemaphore -558
#define LVO_ReleaseSemaphore -570
#define LVO_AttemptSemaphore -576
#define MODE_OLDFILE    1005
#define MODE_NEWFILE    1006
/* the variables (at vars): */
#define V_dos   0
#define V_lib   4
#define V_mem   8               /* AvailMem at the start */
#define V_cnt   12              /* interrupts counted */
#define V_old   16              /* the audio vector before */
#define V_chip  20              /* a chip buffer for Paula */
#define V_ciab  24
#define V_int   28              /* an Interrupt, 22 bytes */
#define V_sem   52              /* a SignalSemaphore, 46 bytes */
#define V_io    100             /* an IOAudio, 68 bytes */
#define V_io2   168             /* another */
#define V_mask  236             /* the channel masks wanted */
#define V_end   240
#define INT_DATA 14
#define INT_CODE 18

    .text
main:
    movem.l %d2-%d7/%a2-%a6,-(%sp)
    lea     vars(%pc),%a4
    moveq   #1,%d6              /* the step */
    move.l  4.w,%a6
    moveq   #0,%d1
    jsr     LVO_AvailMem(%a6)
    move.l  %d0,V_mem(%a4)
    lea     dosname(%pc),%a1
    moveq   #0,%d0
    jsr     LVO_OpenLibrary(%a6)
    move.l  %d0,V_dos(%a4)
    beq     fail
    move.l  %d0,%a6
    jsr     LVO_Output(%a6)
    move.l  %d0,%d7
    /* 2: the library written to RAM: and read back */
    moveq   #2,%d6
    lea     f_lib(%pc),%a0
    move.l  %a0,%d1
    move.l  #MODE_NEWFILE,%d2
    jsr     LVO_Open(%a6)
    move.l  %d0,%d4
    beq     fail
    move.l  %d4,%d1
    lea     lib(%pc),%a0
    move.l  %a0,%d2
    move.l  #libend-lib,%d3
    jsr     LVO_Write(%a6)
    cmp.l   #libend-lib,%d0
    bne     fail
    move.l  %d4,%d1
    jsr     LVO_Close(%a6)
    lea     f_lib(%pc),%a0
    move.l  %a0,%d1
    move.l  #MODE_OLDFILE,%d2
    jsr     LVO_Open(%a6)
    move.l  %d0,%d4
    beq     fail
    move.l  %d4,%d1
    lea     V_end+vars(%pc),%a0        /* read into the scratch after the variables */
    move.l  %a0,%d2
    moveq   #64,%d3
    jsr     LVO_Read(%a6)
    move.l  %d0,%d5
    move.l  %d4,%d1
    jsr     LVO_Close(%a6)
    moveq   #64,%d0
    cmp.l   %d0,%d5
    bne     fail
    lea     V_end+vars(%pc),%a0
    lea     lib(%pc),%a1
    moveq   #15,%d0
1:  cmpm.l  (%a0)+,(%a1)+
    dbne    %d0,1b
    bne     fail
    lea     m_ram(%pc),%a0
    bsr     puts
    /* 3: LIBS: a directory without it: OpenLibrary fails */
    moveq   #3,%d6
    lea     c_sys(%pc),%a0
    bsr     execute
    bsr     openlib
    tst.l   %d0
    bne     fail
    /* 4: "Ram:Assign LIBS: Ram:", then it opens from RAM: */
    moveq   #4,%d6
    lea     c_ram(%pc),%a0
    bsr     execute
    bsr     openlib
    move.l  %d0,V_lib(%a4)
    beq     fail
    move.l  %d0,%a5
    cmp.w   #3,20(%a5)          /* lib_Version, lib_Revision from InitStruct */
    bne     fail
    cmp.w   #7,22(%a5)
    bne     fail
    cmp.w   #1,32(%a5)          /* lib_OpenCnt: its Open ran */
    bne     fail
    cmp.w   #1,50(%a5)          /* its init had a6 = ExecBase */
    bne     fail
    moveq   #5,%d6
    move.l  %a5,%a6
    moveq   #20,%d0
    jsr     -30(%a6)            /* Double */
    moveq   #41,%d1
    cmp.l   %d1,%d0
    bne     fail
    jsr     -36(%a6)            /* Sum of InitStruct's bytes */
    cmp.l   #627,%d0
    bne     fail
    jsr     -42(%a6)            /* the segment list it was given */
    tst.l   %d0
    beq     fail
    lea     m_lib(%pc),%a0
    bsr     puts
    /* 6: a newer version than it is: 0 */
    moveq   #6,%d6
    move.l  4.w,%a6
    lea     libname(%pc),%a1
    moveq   #4,%d0
    jsr     LVO_OpenLibrary(%a6)
    tst.l   %d0
    bne     fail
    move.l  %a5,%a1
    jsr     LVO_CloseLibrary(%a6)
    tst.w   32(%a5)
    bne     fail
    /* 7: LIBS: back, the file deleted, gone; a second delete fails with ERROR_OBJECT_NOT_FOUND */
    moveq   #7,%d6
    lea     c_back(%pc),%a0
    bsr     execute
    move.l  V_dos(%a4),%a6
    lea     f_libl(%pc),%a0
    move.l  %a0,%d1
    jsr     LVO_DeleteFile(%a6)
    tst.l   %d0
    beq     fail
    lea     f_lib(%pc),%a0
    move.l  %a0,%d1
    move.l  #MODE_OLDFILE,%d2
    jsr     LVO_Open(%a6)
    tst.l   %d0
    bne     fail
    lea     f_lib(%pc),%a0
    move.l  %a0,%d1
    jsr     LVO_DeleteFile(%a6)
    tst.l   %d0
    bne     fail
    jsr     LVO_IoErr(%a6)
    cmp.l   #205,%d0
    bne     fail
    lea     m_del(%pc),%a0
    bsr     puts
    /* 8: still loaded (AmigaOS keeps an unused library until memory runs short): it opens again */
    moveq   #8,%d6
    bsr     openlib
    cmp.l   %a5,%d0
    bne     fail
    move.l  4.w,%a6
    move.l  %a5,%a1
    jsr     LVO_CloseLibrary(%a6)
    /* 9: RemLibrary: its Expunge answers its segment list, unloaded; then it is gone */
    moveq   #9,%d6
    move.l  %a5,%a1
    jsr     LVO_RemLibrary(%a6)
    move.l  %d0,%d1
    beq     fail
    move.l  V_dos(%a4),%a6
    jsr     LVO_UnLoadSeg(%a6)
    bsr     openlib
    tst.l   %d0
    bne     fail
    lea     m_gone(%pc),%a0
    bsr     puts
    /* 10: semaphores: nested, every register kept by Obtain and Release */
    moveq   #10,%d6
    move.l  4.w,%a6
    lea     V_sem(%a4),%a0
    jsr     LVO_InitSemaphore(%a6)
    lea     V_sem(%a4),%a0
    move.l  #0x11112222,%d0
    move.l  #0x33334444,%d1
    move.l  %a0,%a1
    jsr     LVO_ObtainSemaphore(%a6)
    cmp.l   #0x11112222,%d0
    bne     fail
    cmp.l   #0x33334444,%d1
    bne     fail
    cmp.l   %a0,%a1
    bne     fail
    jsr     LVO_AttemptSemaphore(%a6)
    tst.l   %d0
    beq     fail
    lea     V_sem(%a4),%a0
    cmp.w   #2,14(%a0)          /* ss_NestCount */
    bne     fail
    sub.l   %a1,%a1
    jsr     LVO_FindTask(%a6)
    lea     V_sem(%a4),%a0
    cmp.l   40(%a0),%d0         /* ss_Owner */
    bne     fail
    jsr     LVO_ReleaseSemaphore(%a6)
    jsr     LVO_ReleaseSemaphore(%a6)
    lea     V_sem(%a4),%a0
    tst.w   14(%a0)
    bne     fail
    tst.l   40(%a0)
    bne     fail
    /* 11: audio.device: all four channels on open; a second request finds none; its CMD_WRITE is refused for the
     * channels it does not hold (ADIOERR_NOALLOCATION; d29 16.16: a held channel plays) */
    moveq   #11,%d6
    move.b  #15,V_mask(%a4)
    lea     V_io(%a4),%a1
    bsr     audreq
    lea     audname(%pc),%a0
    moveq   #0,%d0
    moveq   #0,%d1
    jsr     LVO_OpenDevice(%a6)
    tst.l   %d0
    bne     fail
    lea     V_io(%a4),%a1
    cmp.l   #15,24(%a1)         /* io_Unit: the channels */
    bne     fail
    tst.w   32(%a1)             /* ioa_AllocKey */
    beq     fail
    lea     V_io2(%a4),%a1
    bsr     audreq
    move.l  V_io+20(%a4),20(%a1)   /* io_Device */
    move.w  #32,28(%a1)         /* ADCMD_ALLOCATE */
    jsr     LVO_DoIO(%a6)
    cmp.l   #-11,%d0            /* ADIOERR_ALLOCFAILED */
    bne     fail
    lea     V_io2(%a4),%a1
    move.w  #3,28(%a1)          /* CMD_WRITE */
    move.l  #15,24(%a1)
    jsr     LVO_DoIO(%a6)
    moveq   #-10,%d1            /* ADIOERR_NOALLOCATION */
    cmp.l   %d1,%d0
    bne     fail
    lea     m_aud(%pc),%a0
    bsr     puts
    /* 12: Paula's channel 0 interrupt through SetIntVector, as a player's level 4 code gets it */
    moveq   #12,%d6
    moveq   #16,%d0
    moveq   #MEMF_CHIP,%d1
    bset    #16,%d1             /* MEMF_CLEAR */
    jsr     LVO_AllocMem(%a6)
    move.l  %d0,V_chip(%a4)
    beq     fail
    lea     V_int(%a4),%a1
    move.b  #2,8(%a1)           /* NT_INTERRUPT */
    move.l  %a4,INT_DATA(%a1)
    lea     audint(%pc),%a0
    move.l  %a0,INT_CODE(%a1)
    moveq   #7,%d0              /* INTB_AUD0 */
    jsr     LVO_SetIntVector(%a6)
    move.l  %d0,V_old(%a4)
    clr.l   V_cnt(%a4)
    lea     0xDFF000,%a0
    move.l  V_chip(%a4),0xA0(%a0)  /* AUD0LC */
    move.w  #8,0xA4(%a0)        /* AUD0LEN: 8 words */
    move.w  #400,0xA6(%a0)      /* AUD0PER */
    clr.w   0xA8(%a0)           /* AUD0VOL 0: silent */
    move.w  #0xC080,0x9A(%a0)   /* INTENA: AUD0 */
    move.w  #0x8201,0x96(%a0)   /* DMACON: DMAEN, AUD0EN */
    move.l  V_dos(%a4),%a6
    moveq   #50,%d5             /* up to a second for 5 */
2:  moveq   #1,%d1
    jsr     -198(%a6)           /* Delay(1) */
    moveq   #5,%d0
    cmp.l   V_cnt(%a4),%d0
    dbls    %d5,2b
    lea     0xDFF000,%a0
    move.w  #0x0001,0x96(%a0)
    move.w  #0x0080,0x9A(%a0)
    move.w  #0x0080,0x9C(%a0)
    move.l  4.w,%a6
    moveq   #7,%d0
    move.l  V_old(%a4),%a1
    jsr     LVO_SetIntVector(%a6)
    moveq   #5,%d0
    cmp.l   V_cnt(%a4),%d0
    bhi     fail
    lea     m_paula(%pc),%a0
    bsr     puts
    /* 13: CIA-B timer A through ciab.resource: AddICRVector, the timer run, its interrupts counted */
    moveq   #13,%d6
    lea     ciabname(%pc),%a1
    jsr     LVO_OpenResource(%a6)
    move.l  %d0,V_ciab(%a4)
    beq     fail
    clr.l   V_cnt(%a4)
    lea     V_int(%a4),%a1
    lea     ciaint(%pc),%a0
    move.l  %a0,INT_CODE(%a1)
    move.l  V_ciab(%a4),%a6
    moveq   #0,%d0              /* CIAICRB_TA */
    jsr     -6(%a6)             /* AddICRVector */
    tst.l   %d0
    bne     fail
    lea     0xBFD000,%a0
    move.b  #0,0xE00(%a0)       /* CRA: stopped */
    move.b  #0xC5,0x400(%a0)    /* TALO, TAHI: 709 E-clock ticks, about 1 ms */
    move.b  #0x02,0x500(%a0)
    move.b  #0x11,0xE00(%a0)    /* CRA: start, load, continuous */
    move.l  V_dos(%a4),%a6
    moveq   #50,%d5
3:  moveq   #1,%d1
    jsr     -198(%a6)           /* Delay(1) */
    moveq   #20,%d0
    cmp.l   V_cnt(%a4),%d0
    dbls    %d5,3b
    lea     0xBFD000,%a0
    move.b  #0,0xE00(%a0)
    move.l  V_ciab(%a4),%a6
    moveq   #0,%d0
    lea     V_int(%a4),%a1
    jsr     -12(%a6)            /* RemICRVector */
    moveq   #20,%d0
    cmp.l   V_cnt(%a4),%d0
    bhi     fail
    lea     m_cia(%pc),%a0
    bsr     puts
    /* 14: the channels and the chip buffer given back; AvailMem as at the start */
    moveq   #14,%d6
    move.l  4.w,%a6
    lea     V_io(%a4),%a1
    jsr     LVO_CloseDevice(%a6)
    move.l  V_chip(%a4),%a1
    moveq   #16,%d0
    jsr     LVO_FreeMem(%a6)
    moveq   #0,%d1
    jsr     LVO_AvailMem(%a6)
    cmp.l   V_mem(%a4),%d0
    bne     fail
    lea     m_mem(%pc),%a0
    bsr     puts
    moveq   #0,%d0
    bra.s   done
fail:
    lea     m_fail(%pc),%a0
    bsr     puts
    move.l  %d6,%d0
done:
    move.l  %d0,%d2
    move.l  4.w,%a6
    move.l  V_dos(%a4),%d0
    beq.s   1f
    move.l  %d0,%a1
    jsr     LVO_CloseLibrary(%a6)
1:  move.l  %d2,%d0
    movem.l (%sp)+,%d2-%d7/%a2-%a6
    rts

/* the interrupt handlers: a1 is_Data (vars), a0 the custom chips; Paula's handler clears its own request */
audint:
    addq.l  #1,V_cnt(%a1)
    move.w  #0x0080,0x9C(%a0)
    rts
ciaint:
    addq.l  #1,V_cnt(%a1)
    rts

/* audreq: a1 an IOAudio cleared, asking for the masks at V_mask (one) */
audreq:
    move.l  %a1,%a0
    moveq   #16,%d0
1:  clr.l   (%a0)+
    dbra    %d0,1b
    lea     V_mask(%a4),%a0
    move.l  %a0,34(%a1)         /* ioa_Data */
    move.l  #1,38(%a1)          /* ioa_Length */
    rts

/* openlib: OpenLibrary("zt.library", 1) -> d0 (a6 left ExecBase) */
openlib:
    move.l  4.w,%a6
    lea     libname(%pc),%a1
    moveq   #1,%d0
    jmp     LVO_OpenLibrary(%a6)

/* execute: Execute(a0, 0, 0), which must answer DOSTRUE */
execute:
    move.l  V_dos(%a4),%a6
    move.l  %a0,%d1
    moveq   #0,%d2
    moveq   #0,%d3
    jsr     LVO_Execute(%a6)
    moveq   #-1,%d1
    cmp.l   %d1,%d0
    bne.s   1f
    rts
1:  addq.l  #4,%sp              /* not DOSTRUE: fail from the caller */
    bra     fail

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
libname: .asciz "zt.library"
audname: .asciz "audio.device"
ciabname: .asciz "ciab.resource"
f_lib:   .asciz "RAM:zt.library"
f_libl:  .asciz "ram:zt.library"
c_sys:   .asciz "Assign LIBS: SYS:LIBS"
c_ram:   .asciz "Ram:Assign LIBS: Ram:"
c_back:  .asciz "C:Assign LIBS: SYS:LIBS/"
m_ram:   .asciz "zlibs: the library written to RAM: and read back\n"
m_lib:   .asciz "zlibs: zt.library opened from RAM: (version 3.7, its Open, init and functions ran, InitStruct's bytes right)\n"
m_del:   .asciz "zlibs: the file deleted from RAM:, gone; a second delete answered ERROR_OBJECT_NOT_FOUND\n"
m_gone:  .asciz "zlibs: still loaded after the delete; RemLibrary expunged it, then it was gone\n"
m_aud:   .asciz "zlibs: audio.device gave the four channels, refused a second request and its CMD_WRITE\n"
m_paula: .asciz "zlibs: Paula's channel 0 interrupt through SetIntVector\n"
m_cia:   .asciz "zlibs: CIA-B timer A through ciab.resource\n"
m_mem:   .asciz "zlibs: memory as at the start\n"
m_fail:  .asciz "zlibs: failed (the return code names the step)\n"
    .even
lib:
    .incbin "ztlib.hunk"
libend:
    .even
vars:   .space  V_end
    .space  64                  /* the read-back scratch */
