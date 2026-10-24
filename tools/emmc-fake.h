/* SPDX-License-Identifier: GPL-2.0-only */
/* A model of an SDHCI controller with an SDHC card behind it, for testing kernel/emmc.c on the
 * host. It follows the SD Host Controller and SD Physical Layer specifications as the driver is
 * written from them, and it is strict: any use that a real controller or card would reject
 * (command before the clock is stable, data before the ready flag, wrong block size, a command
 * in the wrong card state, wrong response type) is counted as a fault. This shows the driver's
 * sequencing is consistent with the specs; it cannot show the real chip behaves the same. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../kernel/hwinfo.h"

#define FAKE_SECTORS 4096                       /* 2 MB card */
static u8 fake_img[FAKE_SECTORS * 512];
static u32 R[0x100 / 4];
static u32 fake_t;
static int faults, trace;
static int app, acmd41_calls, rca_given, wr_busy_once;
static int fake_ready_in_prg;   /* a card that reports READY_FOR_DATA while still programming (Linux checks the state too) */
enum { S_IDLE, S_READY, S_IDENT, S_STBY, S_TRAN, S_DATA, S_RCV, S_PRG };
static int state;
static int blocks_left, pos, xfer_write;
static u32 xfer_lba;
static int width4;
static struct hwinfo fake_hwinfo;

#define FAULT(...) do { faults++; printf("FAKE FAULT: " __VA_ARGS__); printf("\n"); } while (0)
static const struct hwinfo *fake_hw(void) { return &fake_hwinfo; }
static inline u32 now_us(void) { return fake_t += 7; }

static void csd_bits(u8 *csd, int hi, int lo, u32 v)    /* set bits hi..lo of the 128 bit CSD (bit 127 = csd[0] msb) */
{
    for (int b = lo; b <= hi; b++) {
        int bit = (v >> (b - lo)) & 1;
        int byte = 15 - b / 8, sh = b % 8;
        csd[byte] = (u8)((csd[byte] & ~(1 << sh)) | (bit << sh));
    }
}

static void put_resp136(const u8 *b16)                  /* register holds bits 127:8 (CRC dropped) */
{
    u32 w[4] = { 0 };
    for (int bit = 8; bit < 128; bit++)
        if ((b16[15 - bit / 8] >> (bit % 8)) & 1)
            w[(bit - 8) / 32] |= 1u << ((bit - 8) % 32);
    for (int i = 0; i < 4; i++)
        R[0x10 / 4 + i] = w[i];
}

#define R_INT_ 0x30
static void int_set(u32 v) { R[R_INT_ / 4] |= v; }

static int powered(void) { return (R[0x28 / 4] & 0xF00) == 0xF00; }
static u32 rd(u32 off);
static int clk_running(void) { return (rd(0x2C) & 7) == 7; }

static void exec(u32 v)
{
    u32 idx = (v >> 24) & 0x3F, rsp = (v >> 16) & 3, arg = R[0x08 / 4];
    u32 tm = v & 0xFFFF;
    int isdata = (v >> 21) & 1, crc = (v >> 19) & 1, ix = (v >> 20) & 1;
    int a = app; app = 0;
    if (trace) printf("  [cmd %u%s arg %08x flags rsp%u crc%d ix%d data%d tm %x state %d]\n", idx, a ? " (app)" : "", arg, rsp, crc, ix, isdata, tm, state);
    if (R[0x24 / 4] & 3) FAULT("command issued while inhibit set");
    if (!powered()) { int_set(1u << 15 | 1u << 16); return; }
    if (!clk_running()) { FAULT("command %u without a running SD clock", idx); int_set(1u << 15 | 1u << 16); return; }
    int exp_rsp = -1, exp_crc = 1, exp_ix = 1, exp_data = 0;
#define EXPECT(r, c, i, d) do { exp_rsp = r; exp_crc = c; exp_ix = i; exp_data = d; } while (0)
    u32 r0 = 0;
    int ok = 1;
    if (a) {
        switch (idx) {
        case 41:
            EXPECT(2, 0, 0, 0);
            if (state == S_IDLE) {
                acmd41_calls++;
                r0 = 0x00FF8000u;
                if (acmd41_calls >= 3) { r0 |= 0x80000000u | ((arg & 1u << 30) ? 1u << 30 : 0); state = S_READY; }
            } else ok = 0;
            break;
        case 6:
            EXPECT(2, 1, 1, 0);
            if (state != S_TRAN) ok = 0; else { width4 = (arg & 3) == 2; r0 = state << 9 | 1 << 8; }
            break;
        default: FAULT("unexpected app command %u", idx); ok = 0;
        }
    } else switch (idx) {
        case 0: EXPECT(0, 0, 0, 0); state = S_IDLE; acmd41_calls = 0; break;
        case 8:
            EXPECT(2, 1, 1, 0);
            if (state != S_IDLE || arg != 0x1AA) ok = 0; else r0 = arg & 0xFFF;
            break;
        case 55: EXPECT(2, 1, 1, 0); app = 1; r0 = state << 9 | 1 << 8; break;
        case 2:
            EXPECT(1, 1, 0, 0);
            if (state != S_READY) { ok = 0; break; }
            { u8 cid[16] = { 0x1D, 'Z', 'X', 'F', 'A', 'K', 'E', 0x10, 0, 0, 0, 1 }; put_resp136(cid); }
            state = S_IDENT; r0 = 0xFFFFFFFF;       /* marker: response already stored */
            break;
        case 3:
            EXPECT(2, 1, 1, 0);
            if (state != S_IDENT) { ok = 0; break; }
            r0 = 0x1234u << 16 | 0x0500; state = S_STBY; rca_given = 1;
            break;
        case 9:
            EXPECT(1, 1, 0, 0);
            if (state != S_STBY || arg != 0x1234u << 16) { ok = 0; break; }
            { u8 csd[16] = { 0 }; csd_bits(csd, 127, 126, 1); csd_bits(csd, 69, 48, FAKE_SECTORS / 1024 - 1); csd_bits(csd, 83, 80, 9);
              put_resp136(csd); }
            r0 = 0xFFFFFFFF;
            break;
        case 7:
            EXPECT(3, 1, 1, 0);
            if (state != S_STBY || arg != 0x1234u << 16) { ok = 0; break; }
            state = S_TRAN; r0 = state << 9 | 1 << 8;
            break;
        case 13:
            EXPECT(2, 1, 1, 0);
            if (state == S_PRG && wr_busy_once) { wr_busy_once = 0; r0 = 7 << 9 | (fake_ready_in_prg ? 1u << 8 : 0); }
            else { if (state == S_PRG) state = S_TRAN; r0 = state << 9 | 1 << 8; }
            break;
        case 16: EXPECT(2, 1, 1, 0); r0 = state << 9 | 1 << 8; FAULT("CMD16 on an SDHC card is not needed"); break;
        case 17: case 18: case 24: case 25: {
            int multi = idx == 18 || idx == 25;
            xfer_write = idx == 24 || idx == 25;
            EXPECT(2, 1, 1, 1);
            if (state != S_TRAN) { ok = 0; break; }
            if ((R[0x04 / 4] & 0xFFFF) != 512) FAULT("block size %u", R[0x04 / 4] & 0xFFFF);
            blocks_left = R[0x04 / 4] >> 16;
            if (!blocks_left) FAULT("zero block count");
            if (!(tm & 2)) FAULT("block count enable missing");
            if (!!(tm & 0x10) == xfer_write) FAULT("transfer direction bit wrong");
            if (multi != !!(tm & 0x20)) FAULT("multi block bit wrong");
            if (multi && !(tm & 4)) FAULT("multi block without auto CMD12");
            if (!multi && blocks_left != 1) FAULT("single block command with count %d", blocks_left);
            if (!width4) FAULT("data transfer on a 1-bit bus (driver sets 4-bit)");
            xfer_lba = arg; pos = 0;
            if (arg + (u32)blocks_left > FAKE_SECTORS) { ok = 0; break; }
            r0 = state << 9 | 1 << 8;
            state = xfer_write ? S_RCV : S_DATA;
            int_set(xfer_write ? 1u << 4 : 1u << 5);
            break; }
        default: FAULT("unexpected command %u", idx); ok = 0;
    }
    if (!ok) { FAULT("command %u not allowed in card state %d (or bad argument %08x)", idx, state, arg); int_set(1u << 15 | 1u << 16); return; }
    if (rsp != (u32)exp_rsp) FAULT("cmd %u: response type %u, expected %d", idx, rsp, exp_rsp);
    if (exp_rsp && (crc != exp_crc || ix != exp_ix)) FAULT("cmd %u: crc/index check flags %d/%d, expected %d/%d", idx, crc, ix, exp_crc, exp_ix);
    if (isdata != exp_data) FAULT("cmd %u: data flag %d", idx, isdata);
    if (!(idx == 2 || idx == 9)) R[0x10 / 4] = r0;
    int_set(1u);
}

static u32 rd(u32 off)
{
    if (off == 0x24) return 1u << 16 | 1u << 17;        /* card inserted + stable, no inhibit */
    if (off == 0x2C) { u32 c = R[off / 4]; return (c & 1) ? c | 2 : c; }
    if (off == 0x40) return 0;                          /* caps: base clock unknown (Pi quirk) */
    return R[off / 4];
}

static void wr(u32 off, u32 v)
{
    if (off == 0x2C) {
        if (v & (1u << 24)) { memset(R, 0, sizeof R); v &= ~(7u << 24); }
        v &= ~(6u << 24);
        if ((v & 4) && !(R[0x2C / 4] & 4) && !(v & 1)) FAULT("SD clock enabled with the internal clock off");
        R[off / 4] = v;
        return;
    }
    if (off == R_INT_) { R[off / 4] &= ~v; return; }
    R[off / 4] = v;
    if (off == 0x0C)
        exec(v);
}

static u32 data_rd(void)
{
    if (state != S_DATA) { FAULT("data port read in state %d", state); return 0; }
    if (R[R_INT_ / 4] & (1u << 5)) FAULT("data port read while READ_RDY not cleared by the driver");
    u32 v; memcpy(&v, fake_img + (xfer_lba) * 512 + pos, 4);
    pos += 4;
    if (pos == 512) {
        pos = 0; xfer_lba++;
        if (--blocks_left) int_set(1u << 5);
        else { state = S_TRAN; int_set(1u << 1); }
    }
    return v;
}

static void data_wr(u32 v)
{
    if (state != S_RCV) { FAULT("data port write in state %d", state); return; }
    if (R[R_INT_ / 4] & (1u << 4)) FAULT("data port written while WRITE_RDY not cleared by the driver");
    memcpy(fake_img + xfer_lba * 512 + pos, &v, 4);
    pos += 4;
    if (pos == 512) {
        pos = 0; xfer_lba++;
        if (--blocks_left) int_set(1u << 4);
        else { state = S_PRG; wr_busy_once = 1; int_set(1u << 1); }
    }
}

static int mbox_log;
static int mbox_call(u32 tag, u32 a, u32 b, u32 nwords, u32 *r0, u32 *r1)
{
    (void)nwords;
    if (mbox_log) printf("  [mbox tag %x %u %u]\n", tag, a, b);
    if (tag == 0x00030002 && a == 12) { if (r0) *r0 = 12; if (r1) *r1 = 250000000; return 0; }
    if (tag == 0x00028001 && a == 0) { if (b == 2) state = S_IDLE; return 0; }
    if (tag == 0x00038001 || tag == 0x00038002) return 0;
    FAULT("unexpected mailbox tag %x", tag);
    return -1;
}
