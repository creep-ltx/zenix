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

static const struct { u32 base, latch; const char *name; } ide_ports[] = {
    { 0xDD2020u, 0xDD3020u, "A4000" },
    { 0xDA0000u, 0xDA9000u, "A1200/A600" },
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
    IDE_REG(6) = 0xE0;              /* master, LBA mode */
    if (ide_wait(ST_BSY, 0))
        return -1;
    IDE_REG(7) = 0xEC;              /* IDENTIFY DEVICE */
    return ide_wait(ST_BSY | ST_DRQ, ST_DRQ);
}

int ide_init(void)
{
    int found = -1;
    for (u32 i = 0; i < sizeof ide_ports / sizeof ide_ports[0]; i++) {
        ide_base = ide_ports[i].base;
        ide_irq_latch = ide_ports[i].latch;
        if (ide_identify() == 0) {
            found = (int)i;
            break;
        }
    }
    if (found < 0) {
        kprintf("ide: no drive found (A4000/A1200 ports probed)\n");
        return -1;
    }
    static u16 id[256];
    for (int i = 0; i < 256; i++)
        id[i] = swab(IDE_DATA);
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

static int ide_rw(u32 lba, u32 count, void *buf, int write)
{
    u16 *p = buf;
    while (count--) {
        if (ide_wait(ST_BSY, 0))
            return -1;
        IDE_REG(6) = 0xE0 | ((lba >> 24) & 0x0F);
        IDE_REG(2) = 1;
        IDE_REG(3) = lba & 0xFF;
        IDE_REG(4) = (lba >> 8) & 0xFF;
        IDE_REG(5) = (lba >> 16) & 0xFF;
        IDE_REG(7) = write ? 0x30 : 0x20;
        if (ide_wait(ST_BSY | ST_DRQ, ST_DRQ))
            return -1;
        if (write) {
            for (int i = 0; i < 256; i++)
                IDE_DATA = *p++;
            if (ide_wait(ST_BSY, 0))
                return -1;
            if (IDE_REG(7) & ST_ERR)
                return -1;
        } else {
            for (int i = 0; i < 256; i++)
                *p++ = IDE_DATA;
        }
        ide_ack();
        lba++;
    }
    return 0;
}

int ide_read(u32 lba, u32 count, void *buf)
{
    return ide_rw(lba, count, buf, 0);
}

int ide_write(u32 lba, u32 count, const void *buf)
{
    return ide_rw(lba, count, (void *)buf, 1);
}
