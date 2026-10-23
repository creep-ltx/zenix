#pragma once
#include "kernel.h"

#define REG16(a) (*(volatile u16 *)(a))
#define REG8(a)  (*(volatile u8 *)(a))

/* custom chips */
#define SERDATR REG16(0xDFF018)
#define INTENAR REG16(0xDFF01C)
#define INTREQR REG16(0xDFF01E)
#define SERDAT  REG16(0xDFF030)
#define SERPER  REG16(0xDFF032)
#define DMACON  REG16(0xDFF096)
#define INTENA  REG16(0xDFF09A)
#define INTREQ  REG16(0xDFF09C)
#define COLOR00 REG16(0xDFF180)

#define INTF_SETCLR 0x8000
#define INTF_INTEN  0x4000
#define INTF_RBF    0x0800      /* serial receive buffer full, level 5 */
#define INTF_PORTS  0x0008      /* CIA-A, level 2 */
#define INTF_TBE    0x0001      /* serial transmit buffer empty, level 1 */

/* display */
#define COP1LCH REG16(0xDFF080)
#define COP1LCL REG16(0xDFF082)
#define COPJMP1 REG16(0xDFF088)
#define DIWSTRT REG16(0xDFF08E)
#define DIWSTOP REG16(0xDFF090)
#define DDFSTRT REG16(0xDFF092)
#define DDFSTOP REG16(0xDFF094)
#define BPL1PTH REG16(0xDFF0E0)
#define BPL1PTL REG16(0xDFF0E2)
#define BPLCON0 REG16(0xDFF100)
#define BPLCON1 REG16(0xDFF102)
#define BPLCON2 REG16(0xDFF104)
#define BPL1MOD REG16(0xDFF108)
#define COLOR01 REG16(0xDFF182)
#define DMAF_SETCLR 0x8000
#define DMAF_COPEN  0x0080
#define DMAF_BPLEN  0x0100
#define DMAF_MASTER 0x0200

/* CIA-A: odd byte addresses, register stride 0x100 */
#define CIAA(r) REG8(0xBFE001 + ((u32)(r) << 8))
#define CIA_TALO 4
#define CIA_TAHI 5
#define CIA_TBLO 6
#define CIA_TBHI 7
#define CIA_SDR  12
#define CIA_ICR  13
#define CIA_CRA  14
#define CIA_CRB  15

/* CIA-B: even byte addresses, register stride 0x100 (its timers A+B are the E-clock counter of hr_time) */
#define CIAB(r) REG8(0xBFD000 + ((u32)(r) << 8))

/* PAL E-clock ~709379 Hz (NTSC 715909); 100 Hz scheduler tick */
#define CIA_TICKS_100HZ 7094

/* more custom-chip registers: display, blitter */
#define DMACONR REG16(0xDFF002)
#define BPL1PTH REG16(0xDFF0E0)
#define BPL2PTH REG16(0xDFF0E4)
#define BPL2MOD REG16(0xDFF10A)
#define COLOR02 REG16(0xDFF184)
#define COLOR03 REG16(0xDFF186)
#define BLTCON0 REG16(0xDFF040)
#define BLTCON1 REG16(0xDFF042)
#define BLTAFWM REG16(0xDFF044)
#define BLTALWM REG16(0xDFF046)
#define BLTAPTH REG16(0xDFF050)
#define BLTAPTL REG16(0xDFF052)
#define BLTDPTH REG16(0xDFF054)
#define BLTDPTL REG16(0xDFF056)
#define BLTSIZE REG16(0xDFF058)
#define BLTAMOD REG16(0xDFF064)
#define BLTDMOD REG16(0xDFF066)
#define DMAF_BLTEN 0x0040
#define DMAB_BLTBUSY 0x4000
#define INTF_VERTB 0x0020       /* vertical blank, level 3 */
