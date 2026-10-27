/* SPDX-License-Identifier: GPL-2.0-only */
/* netd's two hot loops in 68020 assembler (DESIGN D19 2.2): the Internet checksum (lwIP's LWIP_CHKSUM) and the copy
 * (lwIP's MEMCPY: every byte a program sends or receives passes through it once). Both run on every 68020-68060 and
 * on Emu68; both read the source whole even when it is not aligned (the 68020 and later do that in hardware), which
 * libz's memcpy cannot assume and copies byte by byte. Measured against C: docs/d19-network.md section 14, step 4. */

        .text

/* unsigned short netd_chksum(const void *data, int len): the 16-bit ones' complement sum of the data as big-endian
 * words (an odd last byte is the high half of a word), folded, not complemented: what lwip_standard_chksum gives on
 * a big-endian machine. 32 bytes a turn with addx.l, the carries folded in at the end. */
        .globl  netd_chksum
netd_chksum:
        move.l  4(%sp),%a0
        move.l  8(%sp),%d1
        movem.l %d2-%d4,-(%sp)
        moveq   #0,%d0                  | the sum
        moveq   #0,%d4                  | zero, for the carries
        move.l  %d1,%d3                 | the length, kept
        ble     9f
        lsr.l   #5,%d1                  | 32-byte blocks
        beq.s   3f
        subq.l  #1,%d1                  | X = 0 (d1 was at least 1); dbra counts down to -1
1:      move.l  (%a0)+,%d2
        addx.l  %d2,%d0
        move.l  (%a0)+,%d2
        addx.l  %d2,%d0
        move.l  (%a0)+,%d2
        addx.l  %d2,%d0
        move.l  (%a0)+,%d2
        addx.l  %d2,%d0
        move.l  (%a0)+,%d2
        addx.l  %d2,%d0
        move.l  (%a0)+,%d2
        addx.l  %d2,%d0
        move.l  (%a0)+,%d2
        addx.l  %d2,%d0
        move.l  (%a0)+,%d2
        addx.l  %d2,%d0
        dbra    %d1,1b                  | at most 65536 blocks: lwIP's buffers are below 64 KB
        addx.l  %d4,%d0                 | the last carry
3:      moveq   #28,%d1                 | the longwords left: (len & 31) / 4
        and.l   %d3,%d1
        lsr.l   #2,%d1
        beq.s   5f
        subq.l  #1,%d1                  | X = 0
4:      move.l  (%a0)+,%d2
        addx.l  %d2,%d0
        dbra    %d1,4b
        addx.l  %d4,%d0
5:      btst    #1,%d3                  | a word left
        beq.s   6f
        moveq   #0,%d2
        move.w  (%a0)+,%d2
        add.l   %d2,%d0
        addx.l  %d4,%d0
6:      btst    #0,%d3                  | a byte left: the high half of a word
        beq.s   7f
        moveq   #0,%d2
        move.b  (%a0),%d2
        lsl.w   #8,%d2
        add.l   %d2,%d0
        addx.l  %d4,%d0
7:      move.l  %d0,%d1                 | fold 32 bits into 16
        swap    %d1
        add.w   %d1,%d0
        addx.w  %d4,%d0
        andi.l  #0xffff,%d0
9:      movem.l (%sp)+,%d2-%d4
        rts

/* void *netd_copy(void *dst, const void *src, unsigned long n): memcpy for buffers that do not overlap. The
 * destination is aligned to a longword first, then 32 bytes a turn, then the rest. */
        .globl  netd_copy
netd_copy:
        move.l  4(%sp),%a1
        move.l  8(%sp),%a0
        move.l  12(%sp),%d1
        cmp.l   #12,%d1
        bcs.s   8f
1:      move.w  %a1,%d0
        and.w   #3,%d0
        beq.s   2f
        move.b  (%a0)+,(%a1)+
        subq.l  #1,%d1
        bra.s   1b
2:      move.l  %d1,%d0
        lsr.l   #5,%d0
        beq.s   5f
4:      move.l  (%a0)+,(%a1)+
        move.l  (%a0)+,(%a1)+
        move.l  (%a0)+,(%a1)+
        move.l  (%a0)+,(%a1)+
        move.l  (%a0)+,(%a1)+
        move.l  (%a0)+,(%a1)+
        move.l  (%a0)+,(%a1)+
        move.l  (%a0)+,(%a1)+
        subq.l  #1,%d0
        bne.s   4b
5:      moveq   #28,%d0
        and.l   %d1,%d0
        lsr.l   #2,%d0
        bra.s   7f
6:      move.l  (%a0)+,(%a1)+
7:      dbra    %d0,6b
        moveq   #3,%d0
        and.l   %d0,%d1
        bra.s   8f
9:      move.b  (%a0)+,(%a1)+
8:      dbra    %d1,9b                  | fewer than 12 bytes here
        move.l  4(%sp),%d0
        rts
