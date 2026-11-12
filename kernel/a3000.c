/* SPDX-License-Identifier: GPL-2.0-only */
/* Amiga 3000 on-board SCSI: a WD33C93A SCSI chip behind the WDC 390537 DMA controller (DMAC) at 0xDD0000.
 *
 * Written from the WD33C93A data sheet's programming model as the Linux wd33c93/a3000 drivers use it
 * (register numbers, command codes and the DMAC register layout are facts about the hardware, listed in
 * Knowledge/amiga/zenix-hardware-driver-survey.md): one command at a time, no disconnect, no synchronous
 * transfers, no interrupts (the chip's INT bit is polled with interrupts on, the DMAC's interrupt
 * enable stays off). Each SCSI command is one "select with ATN and transfer" (level 2) command; data
 * moves by DMA through a bounce buffer in chip RAM, which the DMAC can reach and which is always aligned.
 * Only direct-access devices (disks) with 512-byte blocks are used.
 */
#include "kernel.h"

#ifdef A3000_HOST_TEST
#include "../tools/a3000-fake.h"
#else
#define DMAC_BASE 0xDD0000u
static inline u16 dmac16_rd(u32 o) { return *(volatile u16 *)(DMAC_BASE + o); }
static inline void dmac16_wr(u32 o, u16 v) { *(volatile u16 *)(DMAC_BASE + o) = v; }
static inline void dmac32_wr(u32 o, u32 v) { *(volatile u32 *)(DMAC_BASE + o) = v; }
static inline u8 sasr_rd(void) { return *(volatile u8 *)(DMAC_BASE + 0x41); }
static inline void sasr_wr(u8 v) { *(volatile u8 *)(DMAC_BASE + 0x41) = v; }
static inline u8 scmd_rd(void) { return *(volatile u8 *)(DMAC_BASE + 0x43); }
static inline void scmd_wr(u8 v) { *(volatile u8 *)(DMAC_BASE + 0x43) = v; }
#define DMA_ADDR(p) ((u32)(p))
#define RAMSEY_VERSION (*(volatile u8 *)0xDE0043)
#endif

/* DMAC registers (offsets from the base) */
#define D_DAWR   0x02
#define D_CNTR   0x0A
#define D_ACR    0x0C
#define D_ST_DMA 0x12
#define D_FLUSH  0x16
#define D_CINT   0x1A
#define D_ISTR   0x1E
#define D_SP_DMA 0x3E
#define CNTR_PDMD 0x08                  /* A3000 DMA mode */
#define CNTR_DDIR 0x02                  /* data direction: set = memory to device */
#define ISTR_FE_FLG 0x01                /* FIFO empty */

/* WD33C93 registers (through SASR/SCMD) */
#define W_OWN_ID   0x00
#define W_CONTROL  0x01
#define W_TIMEOUT  0x02
#define W_CDB1     0x03
#define W_TARGET_LUN 0x0F
#define W_CMD_PHASE 0x10
#define W_SYNC     0x11
#define W_COUNT    0x12                 /* 24 bit, MSB first, auto-incrementing */
#define W_DEST_ID  0x15
#define W_SRC_ID   0x16
#define W_STATUS   0x17
#define W_COMMAND  0x18
#define CMD_RESET  0x00
#define CMD_SEL_ATN_XFER 0x08
#define ASR_INT    0x80
#define ASR_BSY    0x20
#define ASR_CIP    0x10
#define CSR_SEL_XFER_DONE 0x16
#define CSR_TIMEOUT 0x42
#define CTRL_IDI   0x04
#define CTRL_EDI   0x08
#define CTRL_DMA   0x80
#define OWNID_EAF  0x08
#define OWNID_RAF  0x20
#define OWNID_FS_12 0x40                /* 12-15 MHz clock (the A3000's is 14.3 MHz) */
#define DSTID_DPD  0x40                 /* data phase direction: set = target to initiator */
#define MY_ID      7

#define BOUNCE 65536
static u8 *bounce;
static u8 target_id;
static u32 nblocks;
static int up;

static inline u8 wd_read(u8 reg) { sasr_wr(reg); return scmd_rd(); }
static inline void wd_write(u8 reg, u8 v) { sasr_wr(reg); scmd_wr(v); }
static inline u8 wd_asr(void) { return sasr_rd(); }

/* Waits are timed on the E-clock counter (hr_time: CIA-B, readable with any interrupt mask), not counted in polls,
 * which on Emu68 would be a thousand times too short (hardware hand-off note S6). Interrupts stay on while a command
 * runs: the DMAC's interrupt enable is never set, so the chip cannot interrupt, and a wait of up to 3 s with level 2
 * masked would make the keyboard time out (143 ms) and drop keys (K5). */
static int wait_int(u32 ms)
{
    u32 t0 = hr_time(), lim = ms * (eclock_hz / 1000);
    for (;;) {
        if (wd_asr() & ASR_INT)
            return 0;
        if ((u32)(hr_time() - t0) > lim)
            return -1;
    }
}

/* the selection timeout register: 250 ms (what SCSI recommends) x the chip's input clock in MHz / 80 (WD33C93A data
 * sheet; S1): 45 at the A3000's 14.3 MHz. (It was 20, about 110 ms.) */
#define WD_TIMEOUT_250MS 45

static void wd_reset(void)
{
    wd_write(W_OWN_ID, OWNID_EAF | OWNID_RAF | MY_ID | OWNID_FS_12);
    wd_write(W_CONTROL, CTRL_IDI | CTRL_EDI);
    wd_write(W_SYNC, 0);                                   /* asynchronous */
    sasr_wr(W_COMMAND);
    scmd_wr(CMD_RESET);
}

/* One SCSI command to "target". dir_in: data from the device into buf. Returns the SCSI status byte
 * (0 = GOOD), or -1 if the chip or the target did not answer. len <= BOUNCE. */
static int scsi_cmd(u8 target, const u8 *cdb, u32 cdblen, void *buf, u32 len, int dir_in)
{
    int rc = -1;
    if (wd_asr() & (ASR_BSY | ASR_CIP)) {                  /* a stuck bus: reset the chip */
        wd_reset();
        wait_int(100);
        wd_read(W_STATUS);
        wd_write(W_TIMEOUT, WD_TIMEOUT_250MS);
    }
    wd_write(W_DEST_ID, target | (len && dir_in ? DSTID_DPD : 0));
    wd_write(W_SRC_ID, 0);                                 /* no reselection: no disconnect */
    wd_write(W_TARGET_LUN, 0);
    wd_write(W_SYNC, 0);
    wd_write(W_CMD_PHASE, 0);
    sasr_wr(W_CDB1);                                         /* the CDB goes in with auto-increment */
    for (u32 i = 0; i < cdblen; i++)
        scmd_wr(cdb[i]);
    wd_write(W_OWN_ID, (u8)cdblen);                        /* level-2 commands take the CDB length here */
    if (len) {
        u16 cntr = CNTR_PDMD | (dir_in ? 0 : CNTR_DDIR);
        if (!dir_in)
            memcpy(bounce, buf, len);
        dmac16_wr(D_CNTR, cntr);
        dmac32_wr(D_ACR, DMA_ADDR(bounce));
        dmac16_wr(D_ST_DMA, 1);
        sasr_wr(W_COUNT);
        scmd_wr((u8)(len >> 16)); scmd_wr((u8)(len >> 8)); scmd_wr((u8)len);
        wd_write(W_CONTROL, CTRL_IDI | CTRL_EDI | CTRL_DMA);
    } else {
        sasr_wr(W_COUNT);
        scmd_wr(0); scmd_wr(0); scmd_wr(0);
        wd_write(W_CONTROL, CTRL_IDI | CTRL_EDI);
    }
    sasr_wr(W_COMMAND);
    scmd_wr(CMD_SEL_ATN_XFER);
    int timed_out = wait_int(3000);
    u8 csr = timed_out ? 0xFF : wd_read(W_STATUS);         /* reading the status clears the chip's interrupt */
    u8 st = 0xFF;
    if (!timed_out && csr == CSR_SEL_XFER_DONE)
        st = wd_read(W_TARGET_LUN);
    if (len) {                                             /* stop the DMA; after a read drain the FIFO first */
        dmac16_wr(D_CNTR, CNTR_PDMD | (dir_in ? 0 : CNTR_DDIR));
        if (dir_in) {
            dmac16_wr(D_FLUSH, 1);
            u32 t0 = hr_time();
            while (!(dmac16_rd(D_ISTR) & ISTR_FE_FLG) && (u32)(hr_time() - t0) < 100 * (eclock_hz / 1000))
                ;
        }
        dmac16_wr(D_CINT, 1);
        dmac16_wr(D_SP_DMA, 1);
        dmac16_wr(D_CNTR, CNTR_PDMD);
        if (!timed_out && csr == CSR_SEL_XFER_DONE && st == 0 && dir_in)
            memcpy(buf, bounce, len);
    }
    wd_write(W_CONTROL, CTRL_IDI | CTRL_EDI);
    wd_write(W_OWN_ID, OWNID_EAF | OWNID_RAF | MY_ID | OWNID_FS_12);
    if (timed_out) {
        wd_reset();                                        /* leave the chip in a known state */
        wait_int(100);
        wd_read(W_STATUS);
        wd_write(W_TIMEOUT, WD_TIMEOUT_250MS);
    } else if (csr == CSR_SEL_XFER_DONE) {
        rc = st;
    } else if (csr != CSR_TIMEOUT) {
        kprintf("a3000: scsi target %u: unexpected chip status %x\n", target, csr);
    }
    return rc;
}

/* a disk at "target"? INQUIRY (type 0 = direct access), then READ CAPACITY */
static int probe_target(u8 target)
{
    static u8 inq[36];
    static const u8 tur[6] = { 0x00, 0, 0, 0, 0, 0 };
    static const u8 sense[6] = { 0x03, 0, 0, 0, 18, 0 };
    static const u8 capc[10] = { 0x25 };
    static const u8 inqc[6] = { 0x12, 0, 0, 0, 36, 0 };
    static u8 junk[18], cap[8];
    if (scsi_cmd(target, inqc, 6, inq, 36, 1) != 0)
        return -1;
    if ((inq[0] & 0x1F) != 0 || (inq[0] >> 5) != 0)
        return -1;                                         /* not a connected direct-access device */
    char name[25];
    for (int i = 0; i < 24; i++)
        name[i] = (char)(inq[8 + i] >= 32 && inq[8 + i] < 127 ? inq[8 + i] : ' ');
    name[24] = 0;
    for (int tries = 0; tries < 20; tries++) {             /* spin-up / unit attention take a few tries */
        int r = scsi_cmd(target, tur, 6, 0, 0, 0);
        if (r == 0)
            break;
        scsi_cmd(target, sense, 6, junk, 18, 1);
    }
    if (scsi_cmd(target, capc, 10, cap, 8, 1) != 0)
        return -1;
    u32 last = (u32)cap[0] << 24 | (u32)cap[1] << 16 | (u32)cap[2] << 8 | cap[3];
    u32 bs = (u32)cap[4] << 24 | (u32)cap[5] << 16 | (u32)cap[6] << 8 | cap[7];
    if (bs != 512) {
        kprintf("a3000: scsi target %u (%s): block size %u is not supported\n", target, name, bs);
        return -1;
    }
    nblocks = last + 1;
    kprintf("a3000: scsi target %u: %s - %u sectors (%uMB)\n", target, name, nblocks, nblocks / 2048);
    return 0;
}

KINIT int a3000_init(void)
{
    up = 0;
#ifndef A3000_HOST_TEST
    u8 ramsey = RAMSEY_VERSION;
    if (ramsey != 0x0D && ramsey != 0x0F)
        return -1;                                         /* no Ramsey: not an A3000/A4000 */
#endif
    if (!bounce)
        bounce = mem_alloc(BOUNCE, MEM_CHIP, 0);
    if (!bounce)
        return -1;
    dmac16_wr(D_DAWR, 3);                                    /* A3000 DMA width */
    dmac16_wr(D_CNTR, CNTR_PDMD);
    wd_reset();
    if (wait_int(200)) {
        return -1;                                         /* nothing answers: no WD33C93 here */
    }
    u8 sr = wd_read(W_STATUS);
    if (sr != 0x00 && sr != 0x01)
        return -1;
    wd_write(W_TIMEOUT, WD_TIMEOUT_250MS);                 /* 250 ms selection timeout */
    wd_write(W_CONTROL, CTRL_IDI | CTRL_EDI);
    kprintf("a3000: WD33C93%s found (reset status %x)\n", sr == 0x01 ? "A/B" : "", sr);
    for (u8 t = 0; t < 7; t++)
        if (probe_target(t) == 0) {
            target_id = t;
            up = 1;
            return 0;
        }
    kprintf("a3000: no SCSI disk\n");
    return -1;
}

u32 a3000_sectors(void) { return up ? nblocks : 0; }

static int rw(u32 lba, u32 count, void *buf, int write)
{
    u8 *p = buf;
    if (!up || !count || lba + count > nblocks || lba + count < lba)
        return -1;
    while (count) {
        u32 n = count > BOUNCE / 512 ? BOUNCE / 512 : count;
        u8 cdb[10] = { write ? 0x2A : 0x28, 0, (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba, 0,
                       (u8)(n >> 8), (u8)n, 0 };
        int r = scsi_cmd(target_id, cdb, 10, p, n * 512, !write);
        if (r != 0) {
            kprintf("a3000: %s sector %u (%u sectors) failed, status %d\n", write ? "write" : "read", lba, n, r);
            return -1;
        }
        lba += n; count -= n; p += n * 512;
    }
    return 0;
}

int a3000_read(u32 lba, u32 count, void *buf) { return rw(lba, count, buf, 0); }
int a3000_write(u32 lba, u32 count, const void *buf) { return rw(lba, count, (void *)buf, 1); }

/* SYNCHRONIZE CACHE (10), the whole disk: returns when the drive's write cache is on the medium. A drive that does
 * not know the command (old SCSI disks, which then have no write-back cache to speak of, but we cannot know) answers
 * CHECK CONDITION: -95, so the disk server reports no flush capability instead of a promise it cannot keep. */
int a3000_flush(void)
{
    if (!up)
        return -1;
    u8 cdb[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    int r = scsi_cmd(target_id, cdb, 10, 0, 0, 0);
    if (r == 0)
        return 0;
    if (r > 0) {                        /* CHECK CONDITION etc.: clear the sense data */
        static u8 junk[18];
        u8 sense[6] = { 0x03, 0, 0, 0, 18, 0 };
        scsi_cmd(target_id, sense, 6, junk, 18, 1);
        return -95;
    }
    return -1;
}
