/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#include "hw.h"

/* Gayle IDE: 16-bit data port at the base, byte-wide task-file
 * registers at base + 2 + (reg << 2) (reg: 1 error/feature, 2 nsect,
 * 3-5 lba, 6 dev, 7 status/cmd). Same layout on A4000 and A1200/A600 —
 * only the base and the Gayle IRQ latch differ, so we probe both.
 * PIO polling only, LBA28, master drive. Both regions are mapped
 * cache-inhibited by the supervisor map. */

static u32 ide_base;                    /* set by ide_init probe */
static u32 ide_irq_latch;
#define IDE_DATA (*(volatile u16 *)ide_base)
#define IDE_REG(n) (*(volatile u8 *)(ide_base + ((u32)(n) << 2) + 2))
#define IDE_CTL  (*(volatile u8 *)(ide_base + 0x101Au))       /* device control / alternate status (A1200: 0xDA101A) */
#define GAYLE_A1200_BASE  0xDA0000u
#define GAYLE_A1200_LATCH 0xDA9000u     /* bit 7: IDE interrupt; write 0x7C clears it, keeps the other bits */
#define GAYLE_A1200_INTEN 0xDAA000u     /* which Gayle sources may raise INT2: bit 7 IDE, 6-2 the PCMCIA ones */

static const struct { u32 base, latch; const char *name; } ide_ports[] = {
    { 0xDD2020u, 0xDD3020u, "A4000" },
    { GAYLE_A1200_BASE, GAYLE_A1200_LATCH, "A1200/A600" },
};

#define ST_BSY  0x80
#define ST_DRDY 0x40
#define ST_DRQ  0x08
#define ST_ERR  0x01

static u32 nsectors;

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
 * ~95% of the CPU - discovered as "fork takes 2.8 seconds". */
static void ide_ack(void)
{
    (void)IDE_REG(7);
    (void)*(volatile u8 *)ide_irq_latch;
    if (ide_irq_latch == 0xDA9000u)
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

static int ide_wait(u8 mask, u8 want)
{
    for (u32 i = 0; i < 2000000; i++) {
        u8 s = IDE_REG(7);
        if (s & ST_ERR)
            return -1;
        if ((s & mask) == want)
            return 0;
    }
    return -1;
}

static int ide_identify(void)
{
    if (IDE_REG(7) == 0xFF)         /* nothing drives the bus (no drive, or Emu68's disable_scsi): do not wait for it */
        return -1;
    IDE_REG(6) = 0xE0;              /* master, LBA mode */
    ide_settle();
    if (ide_wait(ST_BSY, 0))
        return -1;
    IDE_REG(7) = 0xEC;              /* IDENTIFY DEVICE */
    ide_settle();
    return ide_wait(ST_BSY | ST_DRQ, ST_DRQ);
}

/* Gayle's IDE interrupt is a level-2 source shared with the CIA tick. Zenix polls, so it must never fire: nIEN in the
 * drive's device control register (it stops the drive asserting INTRQ), Gayle's own enable register cleared (no
 * drive fitted: the INTRQ line floats, and a floating line with the enable set is an INT2 storm), and the latch
 * cleared in case it is already set. On an A4000 the 0xDAxxxx addresses are not decoded: harmless. */
static void gayle_quiet(void)
{
    ide_base = GAYLE_A1200_BASE;
    ide_irq_latch = GAYLE_A1200_LATCH;
    IDE_CTL = 0x02;                                 /* nIEN */
    *(volatile u8 *)GAYLE_A1200_INTEN = 0;
    (void)IDE_REG(7);
    *(volatile u8 *)GAYLE_A1200_LATCH = 0x7C;
}

static u32 stray_irqs;

/* called by irq2_c when a level-2 interrupt had no CIA cause: Gayle's IDE latch (A1200; the A4000's is cleared by
 * the read). Counts them and says so once; with gayle_quiet() done this is not expected to happen. */
void ide_stray_irq(void)
{
    u8 v = *(volatile u8 *)ide_irq_latch;
    if (!(v & 0x80))
        return;
    if (ide_irq_latch == GAYLE_A1200_LATCH)
        *(volatile u8 *)ide_irq_latch = 0x7C;
    if (!stray_irqs++)
        kprintf("ide: stray Gayle IDE interrupt cleared (latch %p)\n", ide_irq_latch);
}

int ide_init(void)
{
    int found = -1;
    gayle_quiet();
    for (u32 i = 0; i < sizeof ide_ports / sizeof ide_ports[0]; i++) {
        ide_base = ide_ports[i].base;
        ide_irq_latch = ide_ports[i].latch;
        if (ide_identify() == 0) {
            found = (int)i;
            break;
        }
    }
    if (found < 0) {
        ide_base = GAYLE_A1200_BASE;                /* the latch irq2_c watches: the A1200's (the owner's machine) */
        ide_irq_latch = GAYLE_A1200_LATCH;
        kprintf("ide: no drive found (A4000/A1200 ports probed)\n");
        return -1;
    }
    IDE_CTL = 0x02;                                 /* nIEN on the port that has the drive (polling only) */
    static u16 id[256];
    for (int i = 0; i < 256; i++)
        id[i] = swab(IDE_DATA);
    ide_settle();
    ide_ack();

    nsectors = ((u32)id[61] << 16) | id[60];
    char model[41];
    for (int i = 0; i < 20; i++) {  /* ATA strings: big-endian byte pairs */
        model[i * 2] = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = 0;
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--)
        model[i] = 0;
    kprintf("ide: %s port %p: %s - %u sectors (%uMB)\n",
            ide_ports[found].name, ide_base, model, nsectors, nsectors / 2048);
    return 0;
}

/* One sector. Levels 1-2 are masked from the command to the Gayle
 * acknowledge: the drive's INT2 would otherwise storm into the level 2
 * handler (it shares the line with the CIAs). The serial receive
 * interrupt (level 5) stays live so no characters are lost. */
static int ide_sector(u32 lba, u16 *p, int write)
{
    u16 sv = irq_save2();
    int rc = -1;
    if (ide_wait(ST_BSY, 0))
        goto out;
    IDE_REG(6) = 0xE0 | ((lba >> 24) & 0x0F);
    IDE_REG(2) = 1;
    IDE_REG(3) = lba & 0xFF;
    IDE_REG(4) = (lba >> 8) & 0xFF;
    IDE_REG(5) = (lba >> 16) & 0xFF;
    IDE_REG(7) = write ? 0x30 : 0x20;
    ide_settle();
    if (ide_wait(ST_BSY | ST_DRQ, ST_DRQ))
        goto out;
    if (write) {
        for (int i = 0; i < 256; i++)
            IDE_DATA = *p++;
        ide_settle();
        if (ide_wait(ST_BSY, 0) || (IDE_REG(7) & ST_ERR))
            goto out;
    } else {
        for (int i = 0; i < 256; i++)
            *p++ = IDE_DATA;
        ide_settle();
    }
    rc = 0;
out:
    ide_ack();
    irq_restore(sv);
    return rc;
}

static int ide_rw(u32 lba, u32 count, void *buf, int write)
{
    u16 *p = buf;
    while (count--) {
        if (ide_sector(lba++, p, write))
            return -1;
        p += 256;
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

/* ATA FLUSH CACHE (E7h): returns when the drive has written its cache. A drive may take seconds. */
int ide_flush(void)
{
    u16 sv = irq_save2();
    int rc = -1;
    if (ide_wait(ST_BSY, 0))
        goto out;
    IDE_REG(6) = 0xE0;
    IDE_REG(7) = 0xE7;
    ide_settle();
    for (u32 i = 0; i < 40; i++) {                  /* up to ~40 x the poll limit of ide_wait */
        u8 st = IDE_REG(7);
        if (!(st & ST_BSY)) {
            rc = (st & ST_ERR) ? -1 : 0;
            break;
        }
        (void)ide_wait(ST_BSY, 0);
    }
out:
    ide_ack();
    irq_restore(sv);
    return rc;
}
