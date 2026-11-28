/* SPDX-License-Identifier: GPL-2.0-only */
/* SD card on the Raspberry Pi 4 / CM4 (EMMC2), for Zenix on a PiStorm under Emu68.
 *
 * Written from the SD Host Controller Simplified Specification (the register layout is the
 * standard SDHCI one) and the SD Physical Layer Simplified Specification (command sequence),
 * plus what the Emu68 / Linux drivers show about this particular chip (see
 * Knowledge/amiga/emu68-sd-controller-registers.md): the register block is reached at the
 * address that stage0 found in the device tree (hwinfo.emmc), registers are little-endian 32-bit
 * and always accessed as whole words, the data port is NOT byte-swapped, the clock comes from the
 * VideoCore through the mailbox, and the card is powered through the mailbox too.
 *
 * Polling only, PIO through the data port (no DMA, so no cache problem for the data); the one
 * DMA-like thing is the mailbox message buffer. SD cards only (CM4 Lite); eMMC is not handled.
 * Unverified on real hardware: every stage logs with kprintf so the first run tells what works.
 */
#include "kernel.h"
#include "hwinfo.h"
#include "mbox.h"

#ifdef EMMC_HOST_TEST
#include "../tools/emmc-fake.h"
#else
static u32 emmc_base, mbox_base, timer_base;

static inline u32 rd(u32 off)
{
    u32 v = __builtin_bswap32(*(volatile u32 *)(emmc_base + off));
    __asm__ volatile("nop" ::: "memory");       /* Emu68 turns NOP into a barrier */
    return v;
}
static inline void wr(u32 off, u32 v)
{
    *(volatile u32 *)(emmc_base + off) = __builtin_bswap32(v);
    __asm__ volatile("nop" ::: "memory");
}
static inline u32 data_rd(void) { return *(volatile u32 *)(emmc_base + 0x20); }
static inline void data_wr(u32 v) { *(volatile u32 *)(emmc_base + 0x20) = v; }
static inline u32 now_us(void) { return __builtin_bswap32(*(volatile u32 *)(timer_base + 0x04)); }

/* the mailbox message goes to the VideoCore, which reads it from RAM: clean the line(s) out of
 * the ARM data cache before, and drop them after. 040 instructions, translated by Emu68.
 * The Pi 4's VideoCore reaches only the first 1 GB of RAM: a message above it is never read and every call fails
 * (verified on the A1200 2026-10-24: Emu68 1.1 put the kernel at 0x41040000, and power on and the clock rate
 * failed). So the message lives in fast RAM below 1 GB (MEM_LOW1G), taken at emmc_init; the buffer in the kernel
 * is only the fallback when there is none. */
static u32 mbox_static[16] __attribute__((aligned(64)));
static u32 *mbox_msg = mbox_static;
static u32 mbox_cap = 16;              /* mbox_msg's words */
static void cache_clean(void *p)  { register void *a __asm__("a0") = p; __asm__ volatile(".word 0xF468" :: "a"(a) : "memory"); }
static void cache_inval(void *p)  { register void *a __asm__("a0") = p; __asm__ volatile(".word 0xF448" :: "a"(a) : "memory"); }

/* one tag with a value buffer of nwords (at most 34: the message in mbox_msg is 6 + nwords words): a and b its first
 * two words, the rest 0; the answer's first two words into r0, r1 -> 0, or -1 */
KINIT static int mbox_call(u32 tag, u32 a, u32 b, u32 nwords, u32 *r0, u32 *r1)
{
    volatile u32 *m = mbox_msg;
    u32 n = mbox_build(m, mbox_cap, tag, a, b, nwords);
    if (!n)
        return -1;                              /* (a message bigger than mbox_msg: never sent) */
    for (u32 i = 0; i < n; i += 4)              /* every 16-byte line of it (Emu68: the ARM's lines around them) */
        cache_clean((void *)(m + i));
    u32 t0 = now_us();
    while (__builtin_bswap32(*(volatile u32 *)(mbox_base + 0x38)) & 0x80000000u)
        if (now_us() - t0 > 100000)
            return -1;
    *(volatile u32 *)(mbox_base + 0x20) = __builtin_bswap32(((u32)mbox_msg & ~0xFu) | 8);
    for (;;) {
        if (now_us() - t0 > 1000000)
            return -1;
        if (__builtin_bswap32(*(volatile u32 *)(mbox_base + 0x18)) & 0x40000000u)
            continue;                           /* read FIFO empty */
        u32 v = __builtin_bswap32(*(volatile u32 *)mbox_base);
        if ((v & 0xF) == 8)
            break;
    }
    for (u32 i = 0; i < n; i += 4)
        cache_inval((void *)(m + i));
    if (!(__builtin_bswap32(m[1]) & 0x80000000u))
        return -1;                              /* firmware did not answer the request */
    if (r0) *r0 = __builtin_bswap32(m[5]);
    if (r1) *r1 = __builtin_bswap32(m[6]);
    return 0;
}
/* DESIGN D63 (docs/d63-pistorm-gfx.md 4): the HDMI picture's size and the monitor's EDID, asked once here at boot (a
 * message needs the cache calls only the kernel makes) for the Pi's graphics; written after the hardware table at
 * 0x1100 (HWDISP_ADDR), where the programs read it (no Pi has a 68k MMU). The EDID's message (40 words) in a block of
 * its own below 1 GB, on whole 64-byte lines (Emu68's cinvl drops the ARM's whole line), freed after */
KINIT static void mbox_display(void)
{
    struct hwdisp *d = (struct hwdisp *)HWDISP_ADDR;
    u32 w = 0, h = 0, *b, *was = mbox_msg, wc = mbox_cap;
    memset(d, 0, sizeof *d);            /* (the boot clears it too) */
    if (mbox_call(0x00040003, 0, 0, 2, &w, &h) == 0) {     /* (the readers check the sizes) */
        d->w = (unsigned short)w;
        d->h = (unsigned short)h;
        d->flags = HWD_SIZE;
    }
    if ((b = mem_alloc(256, MEM_FASTONLY | MEM_LOW1G, 0))) {      /* 160 bytes on whole lines: 192 from a 64 */
        mbox_msg = (u32 *)(((u32)b + 63) & ~63u);
        mbox_cap = 40;
        int r = mbox_call(0x00030020, 0, 0, 34, &w, &h);
        if (r == 0 && !h) {             /* block 0: the status 0, then its 128 bytes */
            memcpy(d->edid, mbox_msg + 7, 128);
            d->flags |= HWD_EDID;
        }
        mbox_msg = was;
        mbox_cap = wc;
        if (r == 0)
            mem_free(b);                /* (not after no answer: the firmware may still write it; 256 bytes kept) */
    }
    kprintf("pi: hdmi %ux%u, answers %u\n", d->w, d->h, d->flags);  /* (1 the size, 2 the EDID; 0: no answer) */
}
/* the mailbox for the kernel's calls (the SD card's power and clock, D63's display): once, at boot, on a Pi under
 * Emu68, whatever disk the machine boots from */
static u8 mbox_up;
KINIT __attribute__((noinline)) int pi_mbox_init(void)
{
    const struct hwinfo *hw = HWINFO_K;
    if (mbox_up)
        return 0;
    if (hw->magic != HWINFO_MAGIC || !hw->mailbox || !hw->soc_base)
        return -1;
    mbox_base = hw->mailbox;
    timer_base = hw->soc_base + 0x3000;                 /* system timer: 1 MHz free running counter */
    if ((u32)mbox_msg + sizeof mbox_static > 0x40000000u) {
        u8 *b = mem_alloc(64 + 64, MEM_FASTONLY | MEM_LOW1G, 0);      /* kept for good: one small block */
        if (b)
            mbox_msg = (u32 *)(((u32)b + 63) & ~63u);
        else
            kprintf("emmc: no fast RAM below 1 GB for the mailbox message: the clock calls may fail\n");
    }
    mbox_up = 1;
    if (hw->flags & HW_BCM2711)
        mbox_display();
    return 0;
}
#endif

KINIT static void delay_us(u32 us)
{
    u32 t0 = now_us();
    while (now_us() - t0 < us)
        ;
}

/* SDHCI registers */
#define R_BLKSIZECNT 0x04
#define R_ARG1       0x08
#define R_CMDTM      0x0C
#define R_RESP0      0x10
#define R_PSTATE     0x24
#define R_CTRL0      0x28
#define R_CTRL1      0x2C
#define R_INT        0x30
#define R_INT_MASK   0x34
#define R_INT_EN     0x38
#define R_CTRL2      0x3C
#define R_CAPS0      0x40

#define INT_CMD_DONE  (1u << 0)
#define INT_XFER_DONE (1u << 1)
#define INT_WRITE_RDY (1u << 4)
#define INT_READ_RDY  (1u << 5)
#define INT_ERR       (1u << 15)
#define INT_ERRMASK   0xFFFF0000u

#define C1_CLK_EN     (1u << 0)         /* internal clock enable */
#define C1_CLK_STABLE (1u << 1)
#define C1_SDCLK_EN   (1u << 2)
#define C1_RESET_ALL  (1u << 24)
#define C1_RESET_CMD  (1u << 25)
#define C1_RESET_DAT  (1u << 26)

#define CMD_NONE 0
#define CMD_R2   ((1u << 16) | (1u << 19))                      /* 136 bit, CRC */
#define CMD_R3   (2u << 16)                                     /* 48 bit, no CRC/index (OCR) */
#define CMD_R1   ((2u << 16) | (1u << 19) | (1u << 20))         /* 48 bit, CRC, index */
#define CMD_R1B  ((3u << 16) | (1u << 19) | (1u << 20))         /* 48 bit + busy */
#define CMD_DATA (1u << 21)
#define TM_BLKCNT (1u << 1)
#define TM_AUTO12 (1u << 2)
#define TM_READ   (1u << 4)
#define TM_MULTI  (1u << 5)

static u32 card_rca;
static u32 card_sectors;
static int card_hc;                     /* block addressed (SDHC/SDXC) */
static int up;

static int cmd_wait_inhibit(void)
{
    u32 t0 = now_us();
    while (rd(R_PSTATE) & 3)
        if (now_us() - t0 > 100000)
            return -1;
    return 0;
}

static void reset_lines(u32 which)
{
    wr(R_CTRL1, rd(R_CTRL1) | which);
    u32 t0 = now_us();
    while ((rd(R_CTRL1) & which) && now_us() - t0 < 100000)
        ;
}

/* send one command, wait for command complete; response in resp[0..3]. 0 = ok */
static u32 resp[4];
static int cmd(u32 index, u32 arg, u32 flags, u32 tm, u32 timeout_us)
{
    if (cmd_wait_inhibit()) {
        kprintf("emmc: cmd %u: controller busy\n", index);
        return -1;
    }
    wr(R_INT, 0xFFFFFFFFu);
    wr(R_ARG1, arg);
    wr(R_CMDTM, (index << 24) | flags | tm);
    u32 t0 = now_us(), st;
    for (;;) {
        st = rd(R_INT);
        if (st & (INT_CMD_DONE | INT_ERR))
            break;
        if (now_us() - t0 > timeout_us) {
            kprintf("emmc: cmd %u: no answer (int %x)\n", index, st);
            reset_lines(C1_RESET_CMD);
            return -1;
        }
    }
    if (st & INT_ERR) {
        wr(R_INT, st);
        reset_lines(C1_RESET_CMD | C1_RESET_DAT);
        if (index != 8 && index != 5)           /* CMD8 timing out is how an old card answers */
            kprintf("emmc: cmd %u: error int %x\n", index, st);
        return -1;
    }
    wr(R_INT, INT_CMD_DONE);
    for (int i = 0; i < 4; i++)
        resp[i] = rd(R_RESP0 + 4 * i);
    return 0;
}

KINIT static int acmd(u32 index, u32 arg, u32 flags, u32 tm)
{
    if (cmd(55, card_rca << 16, CMD_R1, 0, 500000))
        return -1;
    return cmd(index, arg, flags, tm, 500000);
}

/* SD clock = base / (2 * div); div = 0 means base. 10-bit divided clock mode. */
KINIT static int set_clock(u32 base, u32 target)
{
    u32 div = 0;
    if (target < base)
        div = (base + 2 * target - 1) / (2 * target);
    if (div > 0x3FF)
        div = 0x3FF;
    u32 c1 = rd(R_CTRL1);
    c1 &= ~C1_SDCLK_EN;
    wr(R_CTRL1, c1);
    delay_us(2000);
    c1 &= ~0xFFE0u;                                     /* old divider bits */
    c1 |= ((div & 0xFF) << 8) | ((div >> 8) << 6);
    c1 = (c1 & ~(0xFu << 16)) | (0xEu << 16);           /* longest data timeout */
    c1 |= C1_CLK_EN;
    wr(R_CTRL1, c1);
    u32 t0 = now_us();
    while (!(rd(R_CTRL1) & C1_CLK_STABLE))
        if (now_us() - t0 > 1000000) {
            kprintf("emmc: clock did not become stable (ctrl1 %x)\n", rd(R_CTRL1));
            return -1;
        }
    wr(R_CTRL1, c1 | C1_SDCLK_EN);
    delay_us(2000);
    kprintf("emmc: SD clock %u Hz (base %u, divider %u)\n", div ? base / (2 * div) : base, base, div);
    return 0;
}

/* what sector 0 holds, for the log: "Amiga RDB", "MBR partition table" or "unknown (first bytes 00 00 00 00)"
 * (the RDB may also start in sectors 1-15; diskd looks there, this line only names sector 0) */
static void sector0_kind(const u8 *sec, char *out)
{
    const char *k = 0;
    if (sec[0] == 'R' && sec[1] == 'D' && sec[2] == 'S' && sec[3] == 'K') k = "Amiga RDB";
    else if (sec[510] == 0x55 && sec[511] == 0xAA) k = "MBR partition table";
    if (k) { while ((*out++ = *k++)) ; return; }
    static const char hex[] = "0123456789abcdef", pre[] = "unknown (first bytes";
    for (k = pre; *k; ) *out++ = *k++;
    for (int i = 0; i < 4; i++) { *out++ = ' '; *out++ = hex[sec[i] >> 4]; *out++ = hex[sec[i] & 15]; }
    *out++ = ')'; *out = 0;
}

KINIT int emmc_init(void)
{
#ifdef EMMC_HOST_TEST
    const struct hwinfo *hw = fake_hw();
#else
    const struct hwinfo *hw = HWINFO_K;
#endif
    up = 0;
    if (hw->magic != HWINFO_MAGIC || !hw->emmc || !hw->mailbox || !hw->soc_base)
        return -1;
#ifndef EMMC_HOST_TEST
    emmc_base = hw->emmc;
    pi_mbox_init();                                     /* (blk_init asked first; done once) */
#endif
    kprintf("emmc: controller at %p, mailbox at %p\n", hw->emmc, hw->mailbox);

    /* power-cycle the card (firmware power domain 0 = SD card) so that it is in a known state
     * whatever the last driver did to it, and make sure the controller's clocks run */
    u32 r0 = 0, r1 = 0;
    if (mbox_call(0x00028001, 0, 2, 2, &r0, &r1))     /* set power state: device 0, off, wait */
        kprintf("emmc: mailbox: power off failed (continuing)\n");
    delay_us(20000);
    if (mbox_call(0x00028001, 0, 3, 2, &r0, &r1))     /* on, wait */
        kprintf("emmc: mailbox: power on failed (continuing)\n");
    mbox_call(0x00038001, 12, 1, 2, 0, 0);            /* clock state: EMMC2 on */
    mbox_call(0x00038002, 12, 250000000, 3, 0, 0);    /* ask for 250 MHz, 3rd word = skip turbo */
    u32 base = 0;
    for (int i = 0; i < 10 && !base; i++)
        if (mbox_call(0x00030002, 12, 0, 2, &r0, &r1))
            delay_us(1000);
        else
            base = r1;
    if (!base) {
        kprintf("emmc: cannot read the EMMC2 clock rate\n");
        return -1;
    }
    kprintf("emmc: base clock %u Hz\n", base);

    reset_lines(C1_RESET_ALL);
    if (rd(R_CTRL1) & C1_RESET_ALL) {
        kprintf("emmc: controller reset did not finish\n");
        return -1;
    }
    wr(R_CTRL2, 0);
    wr(R_INT_EN, 0);                                    /* polling: nothing goes to the interrupt controller */
    wr(R_INT, 0xFFFFFFFFu);
    wr(R_INT_MASK, 0xFFFFFFFFu & ~(1u << 8));           /* every status bit visible except card interrupt */
    kprintf("emmc: caps %x, present state %x\n", rd(R_CAPS0), rd(R_PSTATE));

    /* 3.3 V bus power: voltage first, then power on */
    u32 c0 = rd(R_CTRL0) & 0xFFFF00FFu;
    wr(R_CTRL0, c0 | 0x0E00);
    delay_us(2000);
    wr(R_CTRL0, c0 | 0x0F00);
    delay_us(2000);

    if (set_clock(base, 400000))
        return -1;
    delay_us(5000);

    /* card initialisation (SD physical layer spec, chapter 4) */
    if (cmd(0, 0, CMD_NONE, 0, 500000)) {
        kprintf("emmc: no answer to CMD0: no card?\n");
        return -1;
    }
    int v2 = 0;
    if (!cmd(8, 0x1AA, CMD_R1, 0, 500000)) {
        if ((resp[0] & 0xFFF) != 0x1AA) {
            kprintf("emmc: unusable card (CMD8 answered %x)\n", resp[0]);
            return -1;
        }
        v2 = 1;
    }
    card_rca = 0;
    u32 t0 = now_us();
    for (;;) {
        if (acmd(41, 0x00FF8000u | (v2 ? 1u << 30 : 0), CMD_R3, 0)) {
            kprintf("emmc: ACMD41 failed\n");
            return -1;
        }
        if (resp[0] & 0x80000000u)
            break;
        if (now_us() - t0 > 2000000) {
            kprintf("emmc: card stays busy after ACMD41\n");
            return -1;
        }
        delay_us(10000);
    }
    card_hc = (resp[0] >> 30) & 1;
    kprintf("emmc: card ready, OCR %x (%s)\n", resp[0], card_hc ? "SDHC/SDXC" : "SDSC");

    if (cmd(2, 0, CMD_R2, 0, 500000))
        return -1;
    if (cmd(3, 0, CMD_R1, 0, 500000))
        return -1;
    card_rca = resp[0] >> 16;
    kprintf("emmc: card address %x\n", card_rca);

    if (cmd(9, card_rca << 16, CMD_R2, 0, 500000))      /* CSD, while the card is in stand-by */
        return -1;
    if (((resp[3] >> 22) & 3) == 1)                     /* CSD version 2: C_SIZE bits 69:48 */
        card_sectors = (((resp[1] >> 8) & 0x3FFFFF) + 1) * 1024;
    else {                                              /* version 1: C_SIZE 73:62, C_SIZE_MULT 49:47, READ_BL_LEN 83:80 */
        u32 csize = ((resp[2] & 0x3) << 10) | (resp[1] >> 22);
        u32 mult = (resp[1] >> 7) & 7;
        u32 bl = (resp[2] >> 8) & 0xF;
        card_sectors = (csize + 1) << (mult + 2) << (bl - 9);
    }
    kprintf("emmc: %u sectors (%u MB)\n", card_sectors, card_sectors / 2048);

    if (cmd(7, card_rca << 16, CMD_R1B, 0, 1000000))
        return -1;
    if (!card_hc && cmd(16, 512, CMD_R1, 0, 500000))
        return -1;
    if (acmd(6, 2, CMD_R1, 0)) {                        /* 4-bit bus */
        kprintf("emmc: cannot select the 4-bit bus\n");
        return -1;
    }
    wr(R_CTRL0, rd(R_CTRL0) | 0x2);
    if (set_clock(base, 25000000))
        return -1;
    up = 1;

    /* first sector as a check of the whole path */
    static u8 sec[512];
    if (emmc_read(0, 1, sec)) {
        kprintf("emmc: first sector read failed\n");
        up = 0;
        return -1;
    }
    char what[48];
    sector0_kind(sec, what);
    kprintf("emmc: sector 0: %s\n", what);
    return 0;
}

u32 emmc_sectors(void)
{
    return up ? card_sectors : 0;
}

/* Move "count" blocks. One command with the block count programmed; the controller stops the
 * transfer itself with an automatic CMD12 for multi-block transfers. */
static int rw(u32 lba, u32 count, void *buf, int write)
{
    u8 *p = buf;
    if (!up || lba + count > card_sectors || !count)
        return -1;
    while (count) {
        u32 n = count > 0xFFFF ? 0xFFFF : count;
        if (cmd_wait_inhibit())
            return -1;
        wr(R_INT, 0xFFFFFFFFu);
        wr(R_BLKSIZECNT, (n << 16) | 512);
        u32 tm = TM_BLKCNT | (write ? 0 : TM_READ);
        if (n > 1)
            tm |= TM_MULTI | TM_AUTO12;
        u32 index = write ? (n > 1 ? 25 : 24) : (n > 1 ? 18 : 17);
        if (cmd(index, card_hc ? lba : lba * 512, CMD_R1 | CMD_DATA, tm, 1000000))
            return -1;
        u32 want = write ? INT_WRITE_RDY : INT_READ_RDY;
        for (u32 b = 0; b < n; b++) {
            u32 t0 = now_us(), st;
            for (;;) {
                st = rd(R_INT);
                if (st & (want | INT_ERR))
                    break;
                if (now_us() - t0 > 1000000) {
                    kprintf("emmc: %s block %u: timeout (int %x)\n", write ? "write" : "read", lba + b, st);
                    reset_lines(C1_RESET_CMD | C1_RESET_DAT);
                    return -1;
                }
            }
            if (st & INT_ERR) {
                kprintf("emmc: %s block %u: error int %x\n", write ? "write" : "read", lba + b, st);
                wr(R_INT, st);
                reset_lines(C1_RESET_CMD | C1_RESET_DAT);
                return -1;
            }
            wr(R_INT, want);
            if (write)
                for (int i = 0; i < 128; i++, p += 4) {
                    u32 v;
                    __builtin_memcpy(&v, p, 4);
                    data_wr(v);
                }
            else
                for (int i = 0; i < 128; i++, p += 4) {
                    u32 v = data_rd();
                    __builtin_memcpy(p, &v, 4);
                }
        }
        u32 t0 = now_us(), st;
        for (;;) {
            st = rd(R_INT);
            if (st & (INT_XFER_DONE | INT_ERR))
                break;
            if (now_us() - t0 > 2000000) {
                kprintf("emmc: transfer does not finish (int %x)\n", st);
                reset_lines(C1_RESET_CMD | C1_RESET_DAT);
                return -1;
            }
        }
        if (st & INT_ERR) {
            kprintf("emmc: transfer error int %x\n", st);
            wr(R_INT, st);
            reset_lines(C1_RESET_CMD | C1_RESET_DAT);
            return -1;
        }
        wr(R_INT, INT_XFER_DONE);
        if (write) {                                    /* wait until the card has finished programming */
            u32 t1 = now_us();
            for (;;) {
                if (cmd(13, card_rca << 16, CMD_R1, 0, 500000))
                    return -1;
                /* READY_FOR_DATA only says the card's buffer is free; some cards set it while still programming,
                 * so the card state (bits 12:9) must have left "prg" (7) too - as Linux's mmc driver checks */
                if ((resp[0] & (1u << 8)) && ((resp[0] >> 9) & 15) != 7)
                    break;
                if (now_us() - t1 > 3000000) {
                    kprintf("emmc: card stays busy after a write\n");
                    return -1;
                }
            }
        }
        lba += n;
        count -= n;
    }
    return 0;
}

int emmc_read(u32 lba, u32 count, void *buf) { return rw(lba, count, buf, 0); }
int emmc_write(u32 lba, u32 count, const void *buf) { return rw(lba, count, (void *)buf, 1); }

/* emmc_write returns after the card has programmed the block (CMD13: ready for data and out of the programming state);
 * there is no write cache in the driver to flush. */
int emmc_flush(void)
{
    return 0;
}
