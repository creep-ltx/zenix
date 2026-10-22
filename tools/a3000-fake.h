/* SPDX-License-Identifier: GPL-2.0-only */
/* A model of the A3000's WD33C93A + DMAC with one SCSI disk (target 2, 512-byte blocks), for testing
 * kernel/a3000.c on the host. Strict like tools/emmc-fake.h: using the chip in a way its programming model
 * does not allow (no CDB length in OWN_ID, DMA without the DMAC set up, wrong data-direction bits, taking
 * read data before the FIFO flush, a stopped DMA with unflushed data) is counted as a fault. It shows the
 * driver follows the programming model it was written from; it cannot show the real chip agrees. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAKE_BLOCKS 4096
static u8 fake_disk[FAKE_BLOCKS * 512];
static u8 wdreg[0x20];
static u8 areg;
static int pending_int;
static u16 cntr;
static int dma_started, acr_written, fifo_valid;
static u8 *dma_ptr;                                    /* what the DMAC's address register points at */
static u8 fifo[65536];
static u32 fifo_len;
static int faults, trace, tur_calls, disk_target = 2;
static u32 cmds, resets;

#define FAULT(...) do { faults++; printf("FAKE FAULT: " __VA_ARGS__); printf("\n"); } while (0)
/* the driver passes the address of its bounce buffer through DMA_ADDR(); on the host a pointer does not
 * fit in 32 bits, so the model remembers the pointer and the "register" gets a marker */
static u8 *last_addr;
#define DMA_ADDR(p) (last_addr = (u8 *)(p), 0xB0000000u)

static u8 sasr_rd(void) { return pending_int ? 0x80 : 0; }
static void sasr_wr(u8 v) { areg = v & 0x1F; }

static void finish(u8 csr, u8 scsi_status)
{
    wdreg[0x17] = csr;
    wdreg[0x0F] = scsi_status;
    pending_int = 1;
}

/* run one SCSI command on the disk; returns SCSI status, fills out[] (device to host) or consumes in[] */
static u8 disk_cmd(const u8 *cdb, const u8 *in, u8 *out, u32 *dlen, int *dir_in)
{
    *dlen = 0; *dir_in = 1;
    switch (cdb[0]) {
    case 0x00:                                         /* TEST UNIT READY: the first one reports a unit attention */
        if (tur_calls++ == 0) return 2;
        return 0;
    case 0x03:                                         /* REQUEST SENSE */
        memset(out, 0, 18); out[0] = 0x70; out[2] = 6; out[7] = 10; out[12] = 0x29;
        *dlen = 18;
        return 0;
    case 0x12:                                         /* INQUIRY */
        memset(out, 0, 36); out[4] = 31;
        memcpy(out + 8, "ZENIXFAK", 8); memcpy(out + 16, "TEST DISK       ", 16); memcpy(out + 32, "0.1 ", 4);
        *dlen = cdb[4] < 36 ? cdb[4] : 36;
        return 0;
    case 0x25: {                                       /* READ CAPACITY(10) */
        u32 last = FAKE_BLOCKS - 1;
        out[0] = last >> 24; out[1] = last >> 16; out[2] = last >> 8; out[3] = last;
        out[4] = 0; out[5] = 0; out[6] = 2; out[7] = 0;
        *dlen = 8;
        return 0; }
    case 0x28: case 0x2A: {                            /* READ(10) / WRITE(10) */
        u32 lba = (u32)cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5], n = cdb[7] << 8 | cdb[8];
        *dir_in = cdb[0] == 0x28;
        *dlen = n * 512;
        if (lba + n > FAKE_BLOCKS || !n) { *dlen = 0; return 2; }
        if (*dir_in) memcpy(out, fake_disk + lba * 512, *dlen);
        else memcpy(fake_disk + lba * 512, in, *dlen);
        return 0; }
    }
    FAULT("unexpected SCSI command %02x", cdb[0]);
    return 2;
}

static void command(u8 cmd)
{
    if (cmd == 0x00) {                                  /* RESET: registers back to defaults, answers with its type */
        u8 own = wdreg[0];
        memset(wdreg, 0, sizeof wdreg);
        wdreg[0] = own;
        finish((own & 0x08) ? 0x01 : 0x00, 0);
        resets++;
        return;
    }
    if (cmd != 0x08) { FAULT("unexpected WD33C93 command %02x", cmd); return; }
    cmds++;
    u8 target = wdreg[0x15] & 7;
    int dpd = (wdreg[0x15] & 0x40) != 0;
    u32 count = (u32)wdreg[0x12] << 16 | wdreg[0x13] << 8 | wdreg[0x14];
    u32 cdblen = wdreg[0x00];
    if (cdblen != 6 && cdblen != 10 && cdblen != 12) FAULT("OWN_ID/CDB length register holds %u", cdblen);
    if (wdreg[0x10] != 0) FAULT("command phase register not cleared");
    if (wdreg[0x16] & 0x08) FAULT("reselection enabled in a no-disconnect driver");
    if (target != disk_target) { finish(0x42, 0); return; }          /* selection timeout */
    if (count && !(wdreg[0x01] & 0x80)) FAULT("data transfer without DMA mode in CONTROL");
    if (!count && (wdreg[0x01] & 0x80)) FAULT("DMA mode with a zero transfer count");
    if (count && (!dma_started || !acr_written)) FAULT("data phase but DMAC not started (%d) / no address (%d)", dma_started, acr_written);
    if (count && !(cntr & 0x08)) FAULT("DMAC not in A3000 mode (PDMD)");
    u8 out[65536];
    u32 dlen; int dir_in;
    u8 status = disk_cmd(wdreg + 3, dma_ptr, out, &dlen, &dir_in);
    if (dlen != count) FAULT("transfer count %u but the command moves %u bytes", count, dlen);
    if (dlen && dpd != dir_in) FAULT("data-phase-direction bit %d but the command is a %s", dpd, dir_in ? "read" : "write");
    if (dlen && (!!(cntr & 0x02)) == dir_in) FAULT("DMAC direction bit wrong for a %s", dir_in ? "read" : "write");
    if (dlen && dir_in) { memcpy(fifo, out, dlen); fifo_len = dlen; fifo_valid = 1; }
    finish(0x16, status);
    if (trace) printf("  [cmd %02x target %u count %u status %u]\n", wdreg[3], target, count, status);
}

static void scmd_wr(u8 v)
{
    wdreg[areg] = v;
    if (areg == 0x18) { command(v); return; }
    if (areg != 0x19) areg++;
}
static u8 scmd_rd(void)
{
    u8 v = wdreg[areg];
    if (areg == 0x17) { pending_int = 0; return v; }                 /* reading the status clears the interrupt */
    if (areg != 0x19) areg++;
    return v;
}

static u16 dmac16_rd(u32 o)
{
    if (o == 0x1E) return fifo_valid ? 0 : 1;                        /* ISTR: FIFO empty flag */
    FAULT("read of DMAC register %02x", o);
    return 0;
}
static void dmac16_wr(u32 o, u16 v)
{
    switch (o) {
    case 0x02: if (v != 3) FAULT("DAWR %u", v); break;
    case 0x0A: cntr = v; break;
    case 0x12: dma_started = 1; break;
    case 0x16:                                                       /* FLUSH: the FIFO is written out to memory */
        if (fifo_valid && dma_ptr) { memcpy(dma_ptr, fifo, fifo_len); }
        fifo_valid = 0;
        break;
    case 0x1A: break;
    case 0x3E:
        if (fifo_valid) FAULT("DMA stopped with unflushed read data");
        dma_started = 0; acr_written = 0;
        break;
    default: FAULT("write of DMAC register %02x", o);
    }
}
static void dmac32_wr(u32 o, u32 v)
{
    if (o != 0x0C) { FAULT("32-bit write of DMAC register %02x", o); return; }
    (void)v;
    dma_ptr = last_addr;
    acr_written = 1;
}
