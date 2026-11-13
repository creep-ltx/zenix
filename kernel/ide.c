/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#include "hw.h"
#include "diskproto.h"

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

/* One sector (512 bytes) between the data port and memory. The drive's DA0-DA2 are on A2-A4 (Gayle spec p.8), so A1
 * selects nothing: a long access, which the bus splits into two word cycles (first word at A1=0, first in memory),
 * reaches the data register twice. On the Gayle port, once ide_init has seen IDENTIFY read the same both ways
 * (ide_long), 128 move.l in rounds of 16: half the bus requests and an eighth of the loop of a word loop; the
 * CPU's other accesses go to the buffer (fast RAM when the caller's is). Otherwise 256 move.w. */
static int ide_long;
static __attribute__((noinline)) void sec_xfer(u16 *p, int write)
{
    volatile u16 *port = (volatile u16 *)ide_data;
    u32 n = 255;
    if (write)                          /* writes stay word by word: the boot check can only prove reads */
        __asm__ volatile("1:\tmove.w (%0)+,(%2)\n\tdbra %1,1b" : "+a"(p), "+d"(n) : "a"(port) : "memory", "cc");
    else if (!ide_long)
        __asm__ volatile("1:\tmove.w (%2),(%0)+\n\tdbra %1,1b" : "+a"(p), "+d"(n) : "a"(port) : "memory", "cc");
    else {
        n = 7;
        __asm__ volatile("1:\n\t.rept 16\n\tmove.l (%2),(%0)+\n\t.endr\n\tdbra %1,1b"
                         : "+a"(p), "+d"(n) : "a"(port) : "memory", "cc");
    }
}

static u32 ms_ticks(u32 ms)
{
    return ms * (eclock_hz / 1000);
}

/* Wait until (status & mask) == want with BSY clear, at most ms milliseconds. ERR counts only once BSY is clear (I4),
 * and not at all with ST_ERR in mask (an error of the command before, which the next command clears).
 * 0, or -1 with what it waited for on the log. */
static int ide_wait(u8 mask, u8 want, u32 ms, const char *what)
{
    u32 t0 = 0, lim = 0, n = 0;
    u8 s;
    for (;;) {
        s = IDE_REG(7);
        if (!(s & ST_BSY)) {
            if ((s & ST_ERR) && !(mask & ST_ERR))
                break;
            if ((s & mask & ~ST_ERR) == want)
                return 0;
        }
        if (++n & 15)
            continue;                   /* the clock (7 CIA reads, about 10 us) at every 16th look, not every look: the
                                         * drive is seen ready within a status read, and the time-out is still the
                                         * clock's; a sector already waiting costs one status read */
        if (!lim) {
            t0 = hr_time();
            lim = ms_ticks(ms);
        } else if ((u32)(hr_time() - t0) > lim) {
            kprintf("ide: no answer while waiting for %s (status %x after %u ms)\n", what, s, ms);
            return -1;
        }
    }
    kprintf("ide: drive error while waiting for %s (status %x, error %x)\n", what, s, IDE_REG(1));
    return -1;
}

static int ide_cmd(u32 lba, u32 n, u16 *p, u8 cmd);
/* From here to ide_init: finding the drive at boot, -Os (audit block 2); the commands and their waits stay -O2 */
#pragma GCC push_options
#pragma GCC optimize("Os")
/* I2/I3: is a drive answering on this port? With an empty bus nothing may wait long, nothing is written but the
 * cylinder-low register. 0 = a drive, ready for a command. */
KINIT static int ide_present(void)
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

static u16 id[256];                     /* IDENTIFY's data (and sector 0 before it, dropped) */

/* IDENTIFY DEVICE to the master: 0 when its data is waiting */
KINIT static int ide_ident_cmd(void)
{
    IDE_REG(6) = 0xE0;              /* master, LBA mode */
    ide_settle();
    if (ide_wait(ST_BSY | ST_DRDY | ST_ERR, ST_DRDY, T_BUSY_MS, "the drive before IDENTIFY"))
        return -1;
    IDE_REG(7) = 0xEC;
    ide_settle();
    return ide_wait(ST_BSY | ST_DRQ, ST_DRQ, T_BUSY_MS, "IDENTIFY data");
}

KINIT static int ide_identify(void)
{
    if (ide_present())
        return -1;
    /* One READ SECTORS of sector 0 first, its data dropped, so that the drive's data transfer starts from a fresh
     * command of ours. FS-UAE 3.2's IDE does not reset its buffer offset for IDENTIFY (its ide_data_ready; fixed in
     * later FS-UAE): after a READ of several sectors - AmigaOS's last command when zenixboot's last read was a kernel
     * file of whole 512-byte blocks - IDENTIFY's data was a stale sector of that read, and the drive was refused
     * (tools/update-test.sh zfsroot). A one-sector READ starts its buffer at 0 again; on a real drive it is one sector
     * more at boot. Its result does not matter: IDENTIFY decides (its ERR, from a CHS-only drive, say, is not counted
     * in the wait below; writing IDENTIFY clears it). */
    (void)ide_cmd(0, 1, id, 0x20);
    return ide_ident_cmd();
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

/* D29 (ZINE) 9.2: after an Amiga program had the machine, Gayle's IDE enable and latch as ide_init left them */
void ide_zine(void)
{
    if (ide_irq_latch == GAYLE_A1200_LATCH)
        gayle_quiet();
}

/* Does the area at "a" (512-byte aligned) mirror the custom chips? Seen without writing there: the real INTENA is changed (an
 * audio channel's enable bit, with the CPU's interrupts masked - kmain runs at level 7 until the boot is done - and
 * put back), and a mirror shows the same INTENAR at offset $1C, both times. */
KINIT static int custom_mirror(u32 a)
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
KINIT static int gayle_id(void)
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

/* IDENTIFY's model name (words 27-46: big-endian byte pairs), its trailing blanks cut */
KINIT static void id_model(char *model)
{
    for (int i = 0; i < 20; i++) {
        model[i * 2] = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    model[40] = 0;
    for (int i = 39; i >= 0 && (model[i] == ' ' || model[i] == 0); i--)
        model[i] = 0;
}

/* DESIGN D44 (docs/d44-cd.md): an ATAPI CD drive on the Gayle port - the slave beside an ATA drive, or the master
 * when no ATA drive answered. cd_sel is its device register value (0xF0 slave, 0xE0 master), 0: none. */
static u8 cd_sel;

/* Is a CD drive at SEL? IDENTIFY DEVICE makes a packet device abort it and show its signature (14h EBh in the cylinder
 * registers); a status that is neither busy nor answering 50 ms after the command is no drive (an absent slave reads 0,
 * or the master's status); busy is given 10 s (a drive spinning up). An ATA drive (DRQ) is left as it is: only the master
 * is ever used for ATA. Then IDENTIFY PACKET DEVICE: a CD-ROM (type 5) taking 12-byte packets. The master is selected again at the end. */
KINIT static void cd_probe(u8 sel)
{
    u8 s;
    IDE_REG(6) = sel;
    ide_settle();
    u32 t0 = hr_time();
    while (((s = IDE_REG(7)) & ST_BSY) || s == 0xFF)                    /* busy (from power-on): up to 10 s */
        if (s == 0xFF || (s & ST_DRDY) || (u32)(hr_time() - t0) > ms_ticks(T_START_MS))
            goto out;                                                   /* no bus, a floating one, or never ready */
    IDE_REG(7) = 0xEC;
    ide_settle();
    t0 = hr_time();
    while (((s = IDE_REG(7)) & ST_BSY) || !(s & (ST_ERR | ST_DRQ)))
        if ((u32)(hr_time() - t0) > ms_ticks(s & ST_BSY ? T_START_MS : 50))
            goto out;
    if (s & ST_DRQ) {                               /* an ATA drive (or a CF card answering for the slave): its data
                                                     * read out, so no command is left half done */
        for (int i = 0; i < 256 && (IDE_REG(7) & ST_DRQ); i++)
            (void)IDE_DATA;
        goto out;
    }
    if (IDE_REG(4) != 0x14 || IDE_REG(5) != 0xEB)
        goto out;
    IDE_REG(7) = 0xA1;
    ide_settle();
    if (ide_wait(ST_BSY | ST_DRQ, ST_DRQ, T_BUSY_MS, "IDENTIFY data"))
        goto out;
    for (int i = 0; i < 256; i++)
        id[i] = swab(IDE_DATA);
    ide_settle();
    char model[41];
    id_model(model);
    if ((id[0] & 0xDF03) == 0x8500)                 /* ATAPI, a CD-ROM, 12-byte packets */
        cd_sel = sel;
    kprintf("ide: ATAPI %s %s: %s\n", sel == 0xF0 ? "slave" : "master", cd_sel ? "CD drive" : "not a CD drive: not used", model);
out:
    IDE_REG(6) = 0xE0;
    ide_settle();
    ide_ack();
}

static u32 stray_irqs;
static int gayle_found;                 /* the Gayle ID read $D: an A600/A1200, which has the PC Card slot (D36) */
int ide_gayle(void) { return gayle_found; }

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

KINIT int ide_init(void)
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
        gayle_found = 1;
        ide_data = GAYLE_A1200_DATA;
        port_name = "A1200/A600";
    }
    if (ide_identify()) {
        if (gayle_found)
            cd_probe(0xE0);                         /* D44: a CD drive alone on the port */
        if (!cd_sel)
            kprintf("ide: no drive found on the %s port\n", port_name);
        return -1;
    }
    IDE_CTL = 0x02;                                 /* nIEN on the port that has the drive (polling only) */
    u8 sum = 0;                     /* of the 512 bytes: ATA's checksum in word 255 makes it 0 */
    u32 h = 0;                      /* a hash of the words in order, for the long reads' check below */
    for (int i = 0; i < 256; i++) {
        u16 w = IDE_DATA;
        sum = (u8)(sum + w + (w >> 8));
        h = h * 31 + w;
        id[i] = swab(w);
    }
    ide_settle();
    ide_ack();

    char model[41];
    id_model(model);
    /* a backstop, not a proof: the data must be an ATA drive's - word 0 bit 15 clear (or 0x848A, CompactFlash's
     * signature), and the checksum right when word 255 has its signature A5 (ATA-5 on; FS-UAE gives none) */
    if (((id[0] & 0x8000) && id[0] != 0x848A) || ((id[255] & 0xFF) == 0xA5 && sum)) {
        kprintf("ide: %s port: IDENTIFY data is not a drive's (%x %x): not used\n", port_name, id[0], id[255]);
        return -1;
    }
    if (!(id[49] & 0x0200)) {       /* I9: a CHS-only drive would take LBA numbers as cylinder/head/sector */
        kprintf("ide: %s port: %s cannot address by LBA (CHS only): not used\n", port_name, model);
        return -1;
    }
    nsectors = ((u32)id[61] << 16) | id[60];
    /* Long reads (sec_xfer) only where they give the same data: on the Gayle port IDENTIFY once more, read with
     * move.l into id[] (its words are used above already), must hash as the word-wise read did (IDENTIFY's data is
     * never all one value, as a blank sector could be). Whatever the long reads left of the sector is read out word by
     * word, so the drive ends the command; any failure keeps words. The drive found above stays found either way. */
    if (gayle_found && !ide_ident_cmd()) {
        ide_long = 1;
        sec_xfer(id, 0);
        ide_settle();
        for (int i = 0; i < 256 && (IDE_REG(7) & ST_DRQ); i++)
            (void)IDE_DATA;
        u32 h2 = 0;
        for (int i = 0; i < 256; i++)
            h2 = h2 * 31 + id[i];
        ide_long = h2 == h;
        ide_ack();
    }
    kprintf("ide: %s port %p: %s - %u sectors (%uMB), %s reads\n", port_name, ide_base, model, nsectors,
            nsectors / 2048, ide_long ? "long" : "word");
    if (gayle_found)
        cd_probe(0xF0);                             /* D44: a CD drive as the slave */
    return 0;
}

#pragma GCC pop_options

/* One READ SECTORS (0x20) / WRITE SECTORS (0x30) command for n sectors (1..256), one DRQ per sector, or FLUSH CACHE
 * (0xE7, n 0: up to 30 s, as any wait). The waits run with interrupts
 * on: the drive cannot interrupt (nIEN, and Gayle's IDE enable is off), and a wait of seconds with level 2 masked
 * would make the keyboard time out after 143 ms and drop keys (hand-off note K5). Levels 1-2 are masked only around
 * each sector's 256 data words and the acknowledge. The serial receive interrupt (level 5) is never masked. */
static int ide_cmd(u32 lba, u32 n, u16 *p, u8 cmd)
{
    int rc = -1, write = cmd != 0x20;
    if (ide_wait(ST_BSY, 0, T_BUSY_MS, "the drive before a command"))
        goto out;
    {
        u16 sv = irq_save2();                       /* the task file and the command in one go */
        IDE_REG(6) = 0xE0 | ((lba >> 24) & 0x0F);
        IDE_REG(2) = (u8)n;                         /* 256 is written as 0 */
        IDE_REG(3) = lba & 0xFF;
        IDE_REG(4) = (lba >> 8) & 0xFF;
        IDE_REG(5) = (lba >> 16) & 0xFF;
        IDE_REG(7) = cmd;
        ide_settle();
        irq_restore(sv);
    }
    for (u32 k = 0; k < n; k++) {
        if (ide_wait(ST_BSY | ST_DRQ, ST_DRQ, T_BUSY_MS, write ? "the drive to take data" : "data from the drive")) {
            kprintf("ide: %s of %u sectors at %u failed at sector %u\n", write ? "write" : "read", n, lba, k);
            goto out;
        }
        u16 sv = irq_save2();
        sec_xfer(p, write);
        p += 256;
        ide_settle();
        irq_restore(sv);
    }
    if (write && ide_wait(ST_BSY, 0, T_BUSY_MS, "the end of a write or FLUSH CACHE"))  /* some drives report early (I5) */
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
        if (ide_cmd(lba, n, p, write ? 0x30 : 0x20))
            return -1;
        lba += n;
        count -= n;
        p += 256 * n;
    }
    return 0;
}

/* D29 16.23: what ZINE's own reader needs to read the drive during a session (user/zine/blibs.c ide_read): the task-file
 * registers (0: no drive), the data port, the drive's sectors, sectors a command and the long reads (bit 16) */
void ide_zine_port(u32 *v)
{
    v[0] = ide_base, v[1] = ide_data, v[2] = nsectors, v[3] = max_sectors | (u32)ide_long << 16;
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
    return ide_cmd(0, 0, 0, 0xE7);
}

#pragma GCC push_options
#pragma GCC optimize("Os")
/* DESIGN D44: one packet command to the CD drive (SYS_atapi; struct uatapi in diskproto.h), data from the drive only.
 * The packet is sent with a byte count limit of 0xFFFE. Then the phases are told by the interrupt reason register: data
 * in (DRQ, IO 1, CoD 0) moves the count the drive put in the cylinder registers, kept from byte SKIP for LEN bytes (whole
 * sectors inside that window with the long moves, as ide_cmd), the rest read and dropped; the status phase (no DRQ, IO 1,
 * CoD 1) ends the command - or 50 ms in a row with neither busy nor DRQ, for a drive that does not set the reason. A
 * drive that stops answering or sends nonsense gets DEVICE RESET (08h) before the error goes back. Anything else
 * is waited past: FS-UAE shows neither busy nor DRQ for a moment after the packet before it has taken it. Levels 1-2
 * masked only around 512 bytes at a time; every wait timed. A CHECK CONDITION (ERR) is the caller's to read (the error
 * register in q->err), not said on the log. The master selected again at the end. */
KHWCD static int atapi_run(struct uatapi *q)
{
    int rc = -EIO;
    u32 pos = 0, t0, t, idle = 0;
    u16 sv;
    u8 s;
    if ((q->len | q->skip) & 1)                     /* (the buffer may be odd: a 68020 moves words anywhere) */
        return -EINVAL;
    IDE_REG(6) = cd_sel;                            /* (every command before this one has ended: polling) */
    ide_settle();
    if (ide_wait(ST_BSY | ST_DRQ | ST_ERR, 0, T_BUSY_MS, "the drive before a command"))
        goto reset;
    sv = irq_save2();
    IDE_REG(1) = 0;                                 /* PIO, no overlap */
    IDE_REG(4) = 0xFE;
    IDE_REG(5) = 0xFF;
    IDE_REG(7) = 0xA0;                              /* PACKET */
    ide_settle();
    irq_restore(sv);
    if (ide_wait(ST_BSY | ST_DRQ, ST_DRQ, T_BUSY_MS, "the drive to take data"))
        goto err;
    for (int i = 0; i < 6; i++)
        IDE_DATA = ((const u16 *)q->cdb)[i];
    ide_settle();
    for (t0 = hr_time();;) {
        s = IDE_REG(7);
        u8 ir = IDE_REG(2) & 3;
        t = hr_time() - t0;
        if (!(s & ST_BSY)) {
            if (s & ST_ERR)
                goto err;
            if (!(s & ST_DRQ)) {
                if (!idle)
                    idle = hr_time() | 1;
                if (ir == 3 || (u32)(hr_time() - idle) > ms_ticks(50))
                    break;
            } else
                idle = 0;
            if ((s & ST_DRQ) && ir == 2) {
                u32 n = ((u32)IDE_REG(4) | (u32)IDE_REG(5) << 8) + 1 >> 1;      /* words in this phase */
                if (!n || pos > 0x4000000)          /* nothing, or more than any request: a drive gone wrong */
                    goto reset;
                while (n) {
                    u32 k = n < 256 ? n : 256;
                    u16 *p = (u16 *)((u8 *)q->buf + pos - q->skip);
                    sv = irq_save2();
                    if (k == 256 && pos >= q->skip && pos - q->skip + 512 <= q->len)
                        sec_xfer(p, 0);
                    else
                        for (u32 i = 0; i < k; i++, p++) {
                            u16 w = IDE_DATA;
                            if (pos + 2 * i >= q->skip && pos + 2 * i - q->skip < q->len)
                                *p = w;
                        }
                    irq_restore(sv);
                    n -= k;
                    pos += 2 * k;
                }
                ide_settle();
                t0 = hr_time();
                idle = 0;
                continue;
            }
        }
        if (s & ST_BSY)
            idle = 0;
        if (t > ms_ticks(T_BUSY_MS)) {
            kprintf("ide: no answer while waiting for %s (status %x after %u ms)\n", "data from the drive", s, T_BUSY_MS);
            goto reset;
        }
    }
    rc = (int)pos;
    goto out;
err:
    q->err = IDE_REG(1);
    goto out;
reset:                                              /* a drive that stopped answering, or went wrong: DEVICE RESET, so the
                                                     * next command does not wait out the same state (it then reports a
                                                     * UNIT ATTENTION: diskd counts the disc as changed) */
    IDE_REG(7) = 0x08;
    ide_settle();
out:
    sv = irq_save2();
    IDE_REG(6) = 0xE0;
    ide_settle();
    ide_ack();
    irq_restore(sv);
    return rc;
}
#pragma GCC pop_options
/* DESIGN D45: the resident entry. cd_sel is set only by cd_probe at boot: without a drive atapi_run (in the span
 * .zhwcd) is never called, and its memory is given back when the boot ends */
int ide_atapi(struct uatapi *q)
{
    q->err = 0;
    if (!cd_sel)
        return -ENODEV;
    return atapi_run(q);
}
int ide_cd_present(void) { return cd_sel != 0; }
