/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#include "hw.h"

/* IDE on the Gayle (A600/A1200) and on the A4000: a 16-bit data port, byte-wide task-file registers at
 * base + 2 + (reg << 2) (reg: 1 error/feature, 2 nsect, 3-5 lba, 6 dev, 7 status/cmd). Same layout on both machines;
 * the base and the interrupt latch differ. PIO polling only, LBA28, the master drive.
 *
 * Which port is probed depends on the machine, decided first (hardware hand-off note, 2026-10-23, items I1-I4):
 *   - a Ramsey chip (A3000/A4000, its version at $DE0043, as a3000.c reads it): the A4000 port at $DD2020 only;
 *   - else, if the $DExxxx area is not a mirror of the custom chips (checked without writing there: a mirror shows
 *     INTENAR at offset $1C), the Gayle ID - write any byte to $DE1000, then four reads of it give one bit each in
 *     bit 7, the first the most significant - must be $D (A600/A1200): then the Gayle port at $DA0000 only;
 *   - anything else: no IDE (a probe there could hit an empty bus or a mirror of the chip registers).
 * Before IDENTIFY the drive must show that it is there (I2): BSY and DRDY together is impossible (no drive); BSY alone
 * must read back the same value from every register (ATA: while BSY is set any register read returns the status);
 * DRDY must keep two different values written to the cylinder-low register; status 0 is a drive still spinning up
 * (some show 0 for about 9 s) and is given up to 10 s to show BSY or DRDY (I3). Every wait is timed on the E-clock
 * (hr_time), never counted in polls, and ERR is only believed once BSY is clear (I4: the other bits are not valid
 * while BSY is set, ATA). IDENTIFY word 49 bit 9 must say LBA (I9): a CHS-only drive is refused, not misaddressed.
 * The data port goes through the fast window ($DA2000 on the Gayle: Gayle spec p.7-8 - "16 bit" timing from $DA2000,
 * "8 bit" from $DA0000), the task-file registers stay in the $DA0000 window. Up to 256 sectors per READ/WRITE SECTORS
 * command (I10; the Gayle port only, see max_sectors), one DRQ per sector. */

static u32 ide_base;                    /* task-file registers; set by ide_init */
static u32 ide_data;                    /* the 16-bit data port */
static u32 ide_irq_latch;
#define IDE_DATA (*(volatile u16 *)ide_data)
#define IDE_REG(n) (*(volatile u8 *)(ide_base + ((u32)(n) << 2) + 2))
#define IDE_CTL  (*(volatile u8 *)(ide_base + 0x101Au))       /* device control / alternate status (A1200: 0xDA101A) */
#define GAYLE_A1200_BASE  0xDA0000u
#define GAYLE_A1200_DATA  0xDA2000u     /* the same data register in Gayle's fast ("16 bit") window */
#define GAYLE_A1200_LATCH 0xDA9000u     /* bit 7: IDE interrupt; write 0x7C clears it, keeps the other bits */
#define GAYLE_A1200_INTEN 0xDAA000u     /* which Gayle sources may raise INT2: bit 7 IDE, 6-2 the PCMCIA ones */
#define GAYLE_ID          0xDE1000u
#define A4000_IDE_BASE    0xDD2020u
#define A4000_IDE_LATCH   0xDD3020u
#define RAMSEY_VERSION    (*(volatile u8 *)0xDE0043)

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

#define T_START_MS  10000u              /* status 0 after power-up: up to 10 s to show BSY or DRDY (I3) */
#define T_BUSY_MS   30000u              /* BSY may last up to 30 s (spin-up from standby; ATA) */

static u32 nsectors;
static const char *port_name = "A1200/A600";
/* sectors per command: up to 256 on the Gayle port (I10); one on the A4000 port, where FS-UAE's IDE hung now and
 * then (BSY for good after a READ SECTORS of 2) once commands covered several sectors - the cause was not found,
 * so that port keeps the one-sector commands it always had */
static u32 max_sectors = 256;

/* Sector data comes out of the port in correct byte order for a
 * big-endian CPU; only ATA IDENTIFY needs per-word swapping (its
 * numeric fields are little-endian words). */
static inline u16 swab(u16 v)
{
    return (u16)((v >> 8) | (v << 8));
}

/* ATA raises an interrupt per command; reading the status register acks
 * the drive, then the Gayle board latch must be cleared - by READ on
 * A4000, by WRITE on A600/A1200 (0x7C keeps the non-IDE bits set, the
 * same clear the Linux gayle driver uses). A read-only "ack" on the
 * A1200 leaves the latch asserted: INT2 storms at ~100kHz and eats
 * ~95% of the CPU - discovered as "fork takes 2.8 seconds". The order - the drive's status first (the drive drops its
 * line), then the latch - matters: Gayle's latch fires on both edges of the line (hand-off note I6). */
static void ide_ack(void)
{
    (void)IDE_REG(7);
    (void)*(volatile u8 *)ide_irq_latch;
    if (ide_irq_latch == GAYLE_A1200_LATCH)
        *(volatile u8 *)ide_irq_latch = 0x7C;
}

/* ATA asks for 400 ns after a command is written before status is read, and after the last data word before status
 * is read. Two CIA reads: every CIA access is synchronised to the E-clock (709 kHz, 1.4 us), so this is at least
 * 1.4 us on any CPU - a 68020 or Emu68 alike (an empty loop would be 100 times too short on Emu68). */
static void ide_settle(void)
{
    (void)CIAA(CIA_CRA);
    (void)CIAA(CIA_CRA);
}

static u32 ms_ticks(u32 ms)
{
    return ms * (eclock_hz / 1000);
}

/* Wait until (status & mask) == want with BSY clear, at most ms milliseconds. ERR counts only once BSY is clear (I4).
 * 0, or -1 with what it waited for on the log. */
static int ide_wait(u8 mask, u8 want, u32 ms, const char *what)
{
    u32 t0 = hr_time(), lim = ms_ticks(ms);
    u8 s;
    for (;;) {
        s = IDE_REG(7);
        if (!(s & ST_BSY)) {
            if (s & ST_ERR)
                break;
            if ((s & mask) == want)
                return 0;
        }
        if ((u32)(hr_time() - t0) > lim) {
            kprintf("ide: no answer while waiting for %s (status %x after %u ms)\n", what, s, ms);
            return -1;
        }
    }
    kprintf("ide: drive error while waiting for %s (status %x, error %x)\n", what, s, IDE_REG(1));
    return -1;
}

/* I2/I3: is a drive answering on this port? With an empty bus nothing may wait long, nothing is written but the
 * cylinder-low register. 0 = a drive, ready for a command. */
static int ide_present(void)
{
    u8 s = IDE_REG(7);
    if (s == 0xFF)                  /* nothing drives the bus (no drive, or Emu68's disable_scsi) */
        return -1;
    IDE_REG(6) = 0xE0;              /* the master (Kickstart may have left the slave selected; an absent slave reads 0) */
    ide_settle();
    s = IDE_REG(7);
    if (s == 0xFF)
        return -1;
    if (s == 0) {                   /* "maybe": a drive still starting; give it up to 10 s to show BSY or DRDY */
        kprintf("ide: %s port: status 0, waiting up to %u s for a drive to start\n", port_name, T_START_MS / 1000);
        u32 t0 = hr_time();
        while (!((s = IDE_REG(7)) & (ST_BSY | ST_DRDY)))
            if ((u32)(hr_time() - t0) > ms_ticks(T_START_MS))
                return -1;
    }
    if ((s & (ST_BSY | ST_DRDY)) == (ST_BSY | ST_DRDY))
        return -1;                  /* impossible for a drive: a floating bus */
    if (s & ST_BSY) {               /* a busy drive answers its status from every register */
        if ((IDE_REG(4) ^ s) & ~0x02)
            return -1;
        u32 t0 = hr_time();
        while ((s = IDE_REG(7)) & ST_BSY)
            if ((u32)(hr_time() - t0) > ms_ticks(T_BUSY_MS)) {
                kprintf("ide: %s port: the drive stayed busy for %u s\n", port_name, T_BUSY_MS / 1000);
                return -1;
            }
        if (!(s & ST_DRDY) && ide_wait(ST_DRDY, ST_DRDY, T_START_MS, "the drive to become ready"))
            return -1;
    }
    if (!(IDE_REG(7) & ST_DRDY))
        return -1;
    IDE_REG(4) = 0x55;              /* DRDY: the registers must hold what is written to them */
    ide_settle();
    if (IDE_REG(4) != 0x55)
        return -1;
    IDE_REG(4) = 0xAA;
    ide_settle();
    if (IDE_REG(4) != 0xAA)
        return -1;
    return 0;
}

static int ide_identify(void)
{
    if (ide_present())
        return -1;
    IDE_REG(6) = 0xE0;              /* master, LBA mode */
    ide_settle();
    if (ide_wait(ST_BSY | ST_DRDY, ST_DRDY, T_BUSY_MS, "the drive before IDENTIFY"))
        return -1;
    IDE_REG(7) = 0xEC;              /* IDENTIFY DEVICE */
    ide_settle();
    return ide_wait(ST_BSY | ST_DRQ, ST_DRQ, T_BUSY_MS, "IDENTIFY data");
}

/* Gayle's IDE interrupt is a level-2 source shared with the CIA tick. Zenix polls, so it must never fire: nIEN in the
 * drive's device control register (it stops the drive asserting INTRQ), Gayle's own enable register cleared (no
 * drive fitted: the INTRQ line floats, and a floating line with the enable set is an INT2 storm), and the latch
 * cleared in case it is already set. Only on a machine whose Gayle ID was read. */
static void gayle_quiet(void)
{
    ide_base = GAYLE_A1200_BASE;
    ide_irq_latch = GAYLE_A1200_LATCH;
    IDE_CTL = 0x02;                                 /* nIEN */
    *(volatile u8 *)GAYLE_A1200_INTEN = 0;
    (void)IDE_REG(7);
    *(volatile u8 *)GAYLE_A1200_LATCH = 0x7C;
}

/* Does the area at "a" (512-byte aligned) mirror the custom chips? Seen without writing there: the real INTENA is changed (an
 * audio channel's enable bit, with the CPU's interrupts masked - kmain runs at level 7 until the boot is done - and
 * put back), and a mirror shows the same INTENAR at offset $1C, both times. */
static int custom_mirror(u32 a)
{
    volatile u16 *m = (volatile u16 *)(a + 0x1C);
    u16 was = INTENAR & 0x0400, same = 1;
    INTENA = INTF_SETCLR | 0x0400;
    if (*m != INTENAR) same = 0;
    INTENA = 0x0400;
    if (*m != INTENAR) same = 0;
    INTENA = was ? (INTF_SETCLR | 0x0400) : 0x0400;
    return same;
}

/* The Gayle ID (hand-off note I1; not in the public Gayle spec): -1 if the area is a mirror of the custom chips */
static int gayle_id(void)
{
    if (custom_mirror(GAYLE_ID))
        return -1;
    volatile u8 *g = (volatile u8 *)GAYLE_ID;
    *g = 0;
    int id = 0;
    for (int i = 0; i < 4; i++)
        id = id << 1 | ((*g >> 7) & 1);
    return id;
}

static u32 stray_irqs;

/* called by irq2_c when a level-2 interrupt had no CIA cause: the IDE interrupt latch. The drive's status is read
 * first (it drops its line, and on the A4000 that read is what clears the latch), then the Gayle latch. Counts them
 * and says so once; with gayle_quiet() and nIEN this is not expected to happen. */
void ide_stray_irq(void)
{
    if (!ide_irq_latch)
        return;
    u8 v = *(volatile u8 *)ide_irq_latch;
    if (!(v & 0x80))
        return;
    if (ide_base)
        (void)IDE_REG(7);
    if (ide_irq_latch == GAYLE_A1200_LATCH)
        *(volatile u8 *)ide_irq_latch = 0x7C;
    if (!stray_irqs++)
        kprintf("ide: stray IDE interrupt cleared (latch %p)\n", ide_irq_latch);
}

int ide_init(void)
{
    u8 ramsey = RAMSEY_VERSION;
    if (ramsey == 0x0D || ramsey == 0x0F) {         /* A3000/A4000: the A4000 port, if anything */
        ide_base = ide_data = A4000_IDE_BASE;
        ide_irq_latch = A4000_IDE_LATCH;
        port_name = "A4000";
        max_sectors = 1;
    } else {
        int id = gayle_id();
        if (id != 0xD) {
            if (id < 0)
                kprintf("ide: no Gayle (the $DE0000 area mirrors the custom chips): no IDE port probed\n");
            else
                kprintf("ide: no A600/A1200 Gayle (Gayle id %x): no IDE port probed\n", id);
            return -1;
        }
        gayle_quiet();
        ide_data = GAYLE_A1200_DATA;
        port_name = "A1200/A600";
    }
    if (ide_identify()) {
        kprintf("ide: no drive found on the %s port\n", port_name);
        return -1;
    }
    IDE_CTL = 0x02;                                 /* nIEN on the port that has the drive (polling only) */
    static u16 id[256];
    for (int i = 0; i < 256; i++)
        id[i] = swab(IDE_DATA);
    ide_settle();
    ide_ack();

    char model[41];
    for (int i = 0; i < 20; i++) {  /* ATA strings: big-endian byte pairs */
        model[i * 2] = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = 0;
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--)
        model[i] = 0;
    if (!(id[49] & 0x0200)) {       /* I9: a CHS-only drive would take LBA numbers as cylinder/head/sector */
        kprintf("ide: %s port: %s cannot address by LBA (CHS only): not used\n", port_name, model);
        return -1;
    }
    nsectors = ((u32)id[61] << 16) | id[60];
    kprintf("ide: %s port %p: %s - %u sectors (%uMB)\n", port_name, ide_base, model, nsectors, nsectors / 2048);
    return 0;
}

/* One READ SECTORS / WRITE SECTORS command for n sectors (1..256), one DRQ per sector. The waits run with interrupts
 * on: the drive cannot interrupt (nIEN, and Gayle's IDE enable is off), and a wait of seconds with level 2 masked
 * would make the keyboard time out after 143 ms and drop keys (hand-off note K5). Levels 1-2 are masked only around
 * each sector's 256 data words and the acknowledge. The serial receive interrupt (level 5) is never masked. */
static int ide_cmd(u32 lba, u32 n, u16 *p, int write)
{
    int rc = -1;
    if (ide_wait(ST_BSY, 0, T_BUSY_MS, "the drive before a command"))
        goto out;
    {
        u16 sv = irq_save2();                       /* the task file and the command in one go */
        IDE_REG(6) = 0xE0 | ((lba >> 24) & 0x0F);
        IDE_REG(2) = (u8)n;                         /* 256 is written as 0 */
        IDE_REG(3) = lba & 0xFF;
        IDE_REG(4) = (lba >> 8) & 0xFF;
        IDE_REG(5) = (lba >> 16) & 0xFF;
        IDE_REG(7) = write ? 0x30 : 0x20;
        ide_settle();
        irq_restore(sv);
    }
    for (u32 k = 0; k < n; k++) {
        if (ide_wait(ST_BSY | ST_DRQ, ST_DRQ, T_BUSY_MS, write ? "the drive to take data" : "data from the drive")) {
            kprintf("ide: %s of %u sectors at %u failed at sector %u\n", write ? "write" : "read", n, lba, k);
            goto out;
        }
        u16 sv = irq_save2();
        if (write)
            for (int i = 0; i < 256; i++)
                IDE_DATA = *p++;
        else
            for (int i = 0; i < 256; i++)
                *p++ = IDE_DATA;
        ide_settle();
        irq_restore(sv);
    }
    if (write && ide_wait(ST_BSY, 0, T_BUSY_MS, "the end of a write"))   /* some drives report early (I5) */
        goto out;
    rc = 0;
out:
    {
        u16 sv = irq_save2();
        ide_ack();
        irq_restore(sv);
    }
    return rc;
}

static int ide_rw(u32 lba, u32 count, void *buf, int write)
{
    u16 *p = buf;
    while (count) {
        u32 n = count > max_sectors ? max_sectors : count;
        if (ide_cmd(lba, n, p, write))
            return -1;
        lba += n;
        count -= n;
        p += 256 * n;
    }
    return 0;
}

u32 ide_sectors(void)
{
    return nsectors;
}

int ide_read(u32 lba, u32 count, void *buf)
{
    return ide_rw(lba, count, buf, 0);
}

int ide_write(u32 lba, u32 count, const void *buf)
{
    return ide_rw(lba, count, (void *)buf, 1);
}

/* ATA FLUSH CACHE (E7h): returns when the drive has written its cache. A drive may take seconds: up to 30 s, with
 * interrupts on (K5). */
int ide_flush(void)
{
    int rc = -1;
    if (ide_wait(ST_BSY, 0, T_BUSY_MS, "the drive before FLUSH CACHE"))
        goto out;
    IDE_REG(6) = 0xE0;
    IDE_REG(7) = 0xE7;
    ide_settle();
    rc = ide_wait(ST_BSY, 0, T_BUSY_MS, "FLUSH CACHE");
out:
    {
        u16 sv = irq_save2();
        ide_ack();
        irq_restore(sv);
    }
    return rc;
}
