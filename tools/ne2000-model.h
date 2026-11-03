/* SPDX-License-Identifier: GPL-2.0-only */
/* A model of the A1200's PC Card slot with an NE2000 card in it, for the host test of net/drv/ne2000-core.c
 * (tools/ne2000-test.c, DESIGN D36): Gayle's slot registers and reset, the card's attribute memory (a CIS and the
 * configuration registers), and the chip: a DP8390 (16 KB of frame memory at 0x4000, the PROM doubled below it) or an
 * AX88190 / AX88790 (the MAC at 0x400, the MII port with a PHY, the TEST register, the ISR that does not always clear
 * at the first write). Built from the same facts as the driver (net/drv/ne2000.h names the sources).
 *
 * Strict where the real thing would misbehave without a sign: an odd port through the even window, a word at any
 * port but the data port, a byte at the data port in word mode or a word in byte mode, a remote DMA read past its
 * count, I/O before the card is configured or while its reset is held, any write to Gayle's interrupt registers, a
 * write outside the slot's addresses. Each is a FAULT, which fails the test. */
#pragma once

#define FAULT(...) do { if (!M.quiet) { printf("FAIL model: " __VA_ARGS__); printf("\n"); } M.faults++; } while (0)

enum { CHIP_DP8390, CHIP_AX88190, CHIP_AX88790, CHIP_NE1000 };

static struct model {
    /* the slot */
    int present;                        /* a card is in */
    int reset_held;
    unsigned char gcs, gcfg;            /* Gayle's card control written, its config */
    unsigned gayle_irq_writes;
    unsigned char attr[0x10000];        /* attribute bytes by CIS index (byte k at PC_ATTR + 2k) */
    unsigned cor_at;                    /* the configuration registers' attribute offset */
    unsigned char cor, ccsr, iob0, iob1;
    unsigned cor_index;                 /* the configuration the card has (an AX88x90: 5); another index: no I/O */
    unsigned wrong_cfg;                 /* I/O while configured with an index the card does not have */
    unsigned fixed_io;                  /* the base a card without IOBASE registers decodes */
    int has_iobase;
    /* the chip */
    int chip;
    unsigned char mem[0x10000];
    unsigned char mac[6];
    unsigned char cr, isr, imr, dcr, rcr, tcr, pstart, pstop, bnry, curr, tpsr, par[6], mar[8];
    unsigned tbcr, rsar, rbcr, dma_addr, dma_left;
    int dma_dir;                        /* 0 none, 1 read, 2 write */
    unsigned char cntr[3], tsr;
    unsigned isr_stubborn;              /* AX: every third acknowledgement leaves the bits set once */
    unsigned char gpio;
    /* the PHY (AX) */
    int phy_addr, phy_powered, link, full100;
    int link_latched_low;
    unsigned long long hist;
    int mii_reading, mii_k, mii_reg;
    unsigned char mii_last;
    /* the wire */
    unsigned char sent[64][1600];
    unsigned sent_len[64], nsent, total_sent;
    int stuck_tx;
    unsigned missed, overruns_seen;
    /* time and the test's view */
    unsigned long us;
    unsigned faults, late_access;
    int quiet;                          /* faults expected (a test of a refusal): counted, not printed */
} M;

static int m_trace;

static void m_reset_chip(void)
{
    M.cr = CR_STP | CR_NODMA;
    M.isr = ISR_RST;
    M.imr = 0;
    M.dma_dir = 0;
    M.tcr = TCR_LB;
}
static unsigned m_iobase(void)
{
    return M.has_iobase ? (unsigned)(M.iob1 << 8 | M.iob0) : M.fixed_io;
}
static int m_io_on(void) { return M.present && !M.reset_held && (M.cor & 0x3F) == M.cor_index; }
/* I/O the card cannot answer: -1 a FAULT (not configured at all, or held in reset), 0 floating (configured with an
 * index it does not have: it decodes nothing, reads float high), 1 the card's */
static int m_io_ok(u32 a)
{
    if (!M.cor || M.reset_held) { FAULT("I/O at %x before the card is configured", a); return -1; }
    if (!m_io_on()) { M.wrong_cfg++; return 0; }
    return 1;
}

/* ---- the chip's registers ---- */
static int m_word_mode(void) { return M.dcr & DCR_WTS; }
static void m_dma_start(unsigned dir)
{
    M.dma_dir = (int)dir;
    M.dma_addr = M.rsar;
    M.dma_left = M.rbcr;
    if (!M.rbcr && M.chip != CHIP_AX88190 && M.chip != CHIP_AX88790) FAULT("a remote DMA started with a count of 0");
}
static unsigned char m_mem_rd(unsigned a)
{
    a &= 0xFFFF;
    if (a < 0x4000) {                                   /* below the frame memory */
        if (M.chip == CHIP_AX88190 || M.chip == CHIP_AX88790)
            return a >= 0x400 && a < 0x406 ? M.mac[a - 0x400] : 0;
        return M.mem[a & 31];                           /* the PROM, repeated */
    }
    return M.mem[a];
}
static void m_dma_step(unsigned n)
{
    M.dma_addr += n;
    if (M.dma_dir == 1 && M.pstop > M.pstart && M.dma_addr >= (unsigned)M.pstop << 8 && M.dma_addr - n < (unsigned)M.pstop << 8)
        M.dma_addr = (unsigned)M.pstart << 8 | (M.dma_addr & 0xFF);     /* a read wraps at the ring's end */
    M.dma_left = M.dma_left >= n ? M.dma_left - n : 0;
    if (!M.dma_left) { M.isr |= ISR_RDC; M.dma_dir = 0; }
}
static unsigned m_data(int word, int write, unsigned v)
{
    if (word != !!m_word_mode() && M.chip != CHIP_NE1000) { FAULT("a %s at the data port in %s mode", word ? "word" : "byte", m_word_mode() ? "word" : "byte"); return 0xFFFF; }
    if (!M.dma_dir || (write ? 2 : 1) != M.dma_dir) { FAULT("the data port %s without that remote DMA running", write ? "written" : "read"); return 0xFFFF; }
    if (M.dma_left < (word ? 2u : 1u)) { FAULT("the data port past the remote DMA's count"); return 0xFFFF; }
    unsigned r = 0;
    if (write) {
        if (M.dma_addr < 0x4000 || M.dma_addr + (word ? 2 : 1) > 0x8000) FAULT("a remote write outside the frame memory: %x", M.dma_addr);
        else if (word) { M.mem[M.dma_addr] = (unsigned char)(v >> 8); M.mem[M.dma_addr + 1] = (unsigned char)v; }
        else M.mem[M.dma_addr] = (unsigned char)v;
    } else
        r = word ? (unsigned)m_mem_rd(M.dma_addr) << 8 | m_mem_rd(M.dma_addr + 1) : m_mem_rd(M.dma_addr);
    m_dma_step(word ? 2 : 1);
    return r;
}

/* the PHY on the AX88x90's MII port: bits sampled at MDC's rising edge, a clause 22 read answered */
static unsigned m_phy_reg(unsigned reg)
{
    switch (reg) {
    case 1: {
        unsigned v = 0x7809 | (M.link && !M.link_latched_low ? 4 : 0);
        M.link_latched_low = 0;
        return v;
    }
    case 2: return 0x003B;
    case 3: return 0x1881;
    case 5: return M.link ? (M.full100 ? 0x41E1 : 0x0021) : 0;
    }
    return 0;
}
static int m_phy_here(unsigned addr)
{
    if (M.chip == CHIP_AX88790 && M.gpio != 0x10) return 0;       /* the internal PHY not selected */
    return (int)addr == M.phy_addr && M.phy_powered;
}
static void m_mii_write(unsigned char v)
{
    int rising = (v & MII_MDC) && !(M.mii_last & MII_MDC);
    M.mii_last = v;
    if (!rising) return;
    if (v & MII_DIR_IN) {
        if (M.mii_reading) M.mii_k++;
        if (M.mii_k >= 19) M.mii_reading = 0;
        return;
    }
    M.mii_reading = 0;
    M.hist = M.hist << 1 | ((v & MII_MDO) != 0);
    if (((M.hist >> 10) & 0xF) == 0x6 && ((M.hist >> 14) & 0xFFFFFFFFull) == 0xFFFFFFFFull) {
        unsigned phy = (unsigned)(M.hist >> 5) & 31, reg = (unsigned)M.hist & 31;
        M.hist = 0;
        if (m_phy_here(phy)) { M.mii_reading = 1; M.mii_k = 0; M.mii_reg = (int)m_phy_reg(reg); }
    }
}
static unsigned char m_mii_read(void)
{
    unsigned bit = 1;                                   /* nobody drives MDI: it floats high */
    if (M.mii_reading) {
        int k = M.mii_k + 1;                            /* reads 1-2 the turnaround, 3-18 the value, 19 idle */
        if (k == 1) bit = 1;
        else if (k == 2) bit = 0;
        else if (k <= 18) bit = ((unsigned)M.mii_reg >> (18 - k)) & 1;
    }
    return (unsigned char)(M.mii_last | (bit ? MII_MDI : 0));
}

/* -> 1 sent (done at once), 0 still sending (stuck) */
static int m_transmit(void)
{
    unsigned a = (unsigned)M.tpsr << 8, n = M.tbcr;
    if (n < 60 || n > 1514 || a < 0x4000 || a + n > 0x8000) { FAULT("a send of %u bytes at %x", n, a); return 1; }
    if (M.tcr & TCR_LB) { FAULT("a send while the chip is in loopback"); return 1; }
    if (M.stuck_tx) return 0;
    unsigned k = M.nsent % 64;
    memcpy(M.sent[k], M.mem + a, n);
    M.sent_len[k] = n;
    M.nsent++;
    M.total_sent++;
    M.tsr = TSR_PTX;
    M.isr |= ISR_PTX;
    return 1;
}

static unsigned char m_reg_rd(unsigned r)
{
    int page1 = (M.cr & 0xC0) == CR_PAGE1;
    int ax = M.chip == CHIP_AX88190 || M.chip == CHIP_AX88790;
    if (r == E_CR) return M.cr;
    if (r == NE_RESET) { m_reset_chip(); return 0; }
    if (r >= 0x10) {
        if (ax && r == AX_MII) return m_mii_read();
        if (ax && r == AX_TEST) return M.chip == CHIP_AX88790 ? 0x40 : 0;
        if (r == NE_DATA) return (unsigned char)m_data(0, 0, 0);
        return 0xFF;                                    /* the data port's aliases: never used by the driver for data */
    }
    if (page1) {
        if (r >= E1_PAR0 && r < E1_PAR0 + 6) return M.par[r - E1_PAR0];
        if (r == E1_CURR) return M.curr;
        if (r >= E1_MAR0) return M.mar[r - E1_MAR0];
        return 0;
    }
    switch (r) {
    case E_BNRY: return M.bnry;
    case E_TSR: return M.tsr;
    case E_ISR: return M.isr;
    case E_CNTR0: case E_CNTR1: case E_CNTR2: { unsigned char v = M.cntr[r - E_CNTR0]; M.cntr[r - E_CNTR0] = 0; return v; }
    }
    return 0;
}
static void m_reg_wr(unsigned r, unsigned char v)
{
    int page1 = (M.cr & 0xC0) == CR_PAGE1;
    int ax = M.chip == CHIP_AX88190 || M.chip == CHIP_AX88790;
    if (r == E_CR) {
        unsigned char txp = M.cr & CR_TXP;
        M.cr = (unsigned char)(v & ~CR_TXP);
        if (M.cr & CR_STP) { M.cr &= (unsigned char)~CR_STA; txp = 0; }
        unsigned rd = (v >> 3) & 7;
        if (rd == 1 && (v & CR_STA)) m_dma_start(1);
        else if (rd == 2 && (v & CR_STA)) m_dma_start(2);
        else if (rd & 4) M.dma_dir = 0;
        if ((v & CR_TXP) && (v & CR_STA) && !(v & CR_STP)) {
            if (txp) FAULT("a send started while one runs");
            txp = m_transmit() ? 0 : CR_TXP;
        }
        M.cr |= txp;
        return;
    }
    if (r == NE_RESET) return;
    if (r >= 0x10) {
        if (ax && r == AX_MII) { m_mii_write(v); return; }
        if (ax && r == AX_GPIO) { M.gpio = v; return; }
        if (r == NE_DATA) { m_data(0, 1, v); return; }
        return;
    }
    if (page1) {
        if (r >= E1_PAR0 && r < E1_PAR0 + 6) M.par[r - E1_PAR0] = v;
        else if (r == E1_CURR) M.curr = v;
        else if (r >= E1_MAR0) M.mar[r - E1_MAR0] = v;
        return;
    }
    switch (r) {
    case E_PSTART: M.pstart = v; break;
    case E_PSTOP: M.pstop = v; break;
    case E_BNRY: M.bnry = v; break;
    case E_TPSR: M.tpsr = v; break;
    case E_TBCR0: M.tbcr = (M.tbcr & 0xFF00) | v; break;
    case E_TBCR1: M.tbcr = (M.tbcr & 0xFF) | (unsigned)v << 8; break;
    case E_ISR:
        if (ax && v && ((++M.isr_stubborn % 3) == 0)) break;   /* the AX88190's habit: this write is lost */
        M.isr &= (unsigned char)~v;
        break;
    case E_RSAR0: M.rsar = (M.rsar & 0xFF00) | v; break;
    case E_RSAR1: M.rsar = (M.rsar & 0xFF) | (unsigned)v << 8; break;
    case E_RBCR0: M.rbcr = (M.rbcr & 0xFF00) | v; break;
    case E_RBCR1: M.rbcr = (M.rbcr & 0xFF) | (unsigned)v << 8; break;
    case E_RCR: M.rcr = v; break;
    case E_TCR: M.tcr = v; break;
    case E_DCR: M.dcr = v; break;
    case E_IMR: M.imr = v; if (v) FAULT("the chip's interrupts enabled (%x): the driver polls", v); break;
    }
}

/* ---- the 68k's bus ---- */
static unsigned m_attr_rd(unsigned off)
{
    if (off & 1) return 0xFF;                           /* only even attribute bytes exist */
    unsigned k = off / 2;
    if (M.cor_at != 0xFFFF && off >= M.cor_at && off < M.cor_at + 0x20) {
        switch (off - M.cor_at) {
        case CCR_COR: return M.cor;
        case CCR_CCSR: return M.ccsr;
        case CCR_IOBASE0: return M.iob0;
        case CCR_IOBASE1: return M.iob1;
        }
        return 0;
    }
    return k < sizeof M.attr ? M.attr[k] : 0xFF;
}
static void m_attr_wr(unsigned off, unsigned char v)
{
    if (!(M.gcs & CS_WR)) { FAULT("an attribute write with Gayle's writes off"); return; }
    if (M.cor_at == 0xFFFF || off < M.cor_at || off >= M.cor_at + 0x20) { FAULT("an attribute write outside the configuration registers: %x", off); return; }
    switch (off - M.cor_at) {
    case CCR_COR: M.cor = v; break;
    case CCR_CCSR: M.ccsr = v; if (v & CCSR_PWRDWN) M.phy_powered = 1; break;
    case CCR_IOBASE0: M.iob0 = v; break;
    case CCR_IOBASE1: M.iob1 = v; break;
    default: FAULT("a write to configuration register %x", off - M.cor_at);
    }
}
/* an I/O address -> the chip's register (0-31), -1 not the card's; odd says which window */
static int m_io_reg(u32 a, int *odd)
{
    u32 p;
    if (a >= PC_IO_ODD && a < PC_IO_ODD + 0x10000) { p = a - PC_IO_ODD + 1; *odd = 1; }
    else { p = a - PC_IO; *odd = 0; }
    unsigned b = m_iobase();
    if (p < b || p >= b + 32) return -1;
    return (int)(p - b);
}

static unsigned char ne_rb(u32 a)
{
    if (a == GAYLE_CS) return (unsigned char)((M.present ? CS_CCDET : 0) | (M.gcs & (CS_WR | CS_DA)));
    if (a == GAYLE_CONFIG) return M.gcfg;
    if (a == GAYLE_INTREQ || a == GAYLE_INTEN) return 0;
    if (a == PC_RESET) { M.reset_held = 0; return 0xFF; }
    if (a >= PC_ATTR && a < PC_ATTR + PC_ATTR_SIZE) return M.present ? (unsigned char)m_attr_rd(a - PC_ATTR) : 0xFF;
    if (a >= PC_IO && a < PC_IO_ODD + 0x10000) {
        if (!M.present) { M.late_access++; return 0xFF; }
        if (m_io_ok(a) <= 0) return 0xFF;
        int odd, r = m_io_reg(a, &odd);
        if (r < 0) { FAULT("an I/O read at %x, not the card's ports", a); return 0xFF; }
        if ((r & 1) != odd) { FAULT("port %x read through the %s window", r, odd ? "odd" : "even"); return 0xFF; }
        return m_reg_rd((unsigned)r);
    }
    FAULT("a byte read at %x", a);
    return 0xFF;
}
static void ne_wb(u32 a, unsigned char v)
{
    if (a == GAYLE_CS) { M.gcs = v; return; }
    if (a == GAYLE_CONFIG) { M.gcfg = v; return; }
    if (a == GAYLE_INTREQ || a == GAYLE_INTEN) { M.gayle_irq_writes++; FAULT("Gayle's interrupt register %x written", a); return; }
    if (a == PC_RESET) {
        M.reset_held = 1;
        M.cor = M.ccsr = M.iob0 = M.iob1 = 0;
        m_reset_chip();
        M.mii_reading = 0;
        return;
    }
    if (a >= PC_ATTR && a < PC_ATTR + PC_ATTR_SIZE) { if (M.present) m_attr_wr(a - PC_ATTR, v); else M.late_access++; return; }
    if (a >= PC_IO && a < PC_IO_ODD + 0x10000) {
        if (!M.present) { M.late_access++; return; }
        if (m_io_ok(a) <= 0) return;
        int odd, r = m_io_reg(a, &odd);
        if (r < 0) { FAULT("an I/O write at %x, not the card's ports", a); return; }
        if ((r & 1) != odd) { FAULT("port %x written through the %s window", r, odd ? "odd" : "even"); return; }
        m_reg_wr((unsigned)r, v);
        return;
    }
    FAULT("a byte write at %x", a);
}
static unsigned short ne_rw(u32 a)
{
    if (!M.present) { M.late_access++; return 0xFFFF; }
    if (m_io_ok(a) <= 0) return 0xFFFF;
    int odd, r = m_io_reg(a, &odd);
    if (odd || r != NE_DATA) { FAULT("a word read at %x", a); return 0xFFFF; }
    return (unsigned short)m_data(1, 0, 0);
}
static void ne_ww(u32 a, unsigned short v)
{
    if (!M.present) { M.late_access++; return; }
    if (m_io_ok(a) <= 0) return;
    int odd, r = m_io_reg(a, &odd);
    if (odd || r != NE_DATA) { FAULT("a word write at %x", a); return; }
    m_data(1, 1, v);
}

/* ---- the wire: a frame arrives (as the chip stores it: header, frame, CRC, into the ring at CURR) ---- */
static int m_receive(const unsigned char *f, unsigned len)
{
    if (!m_io_on() || !(M.cr & CR_STA) || (M.tcr & TCR_LB)) { M.missed++; return 0; }
    int bcast = !memcmp(f, "\xff\xff\xff\xff\xff\xff", 6);
    if (!(bcast && (M.rcr & RCR_AB)) && memcmp(f, M.par, 6)) return 0;       /* not ours */
    unsigned ring = (unsigned)(M.pstop - M.pstart), count = len + 4, pages = (count + 4 + 255) >> 8;
    unsigned freep = ((unsigned)M.bnry + ring - M.curr) % ring;
    if (pages >= freep) {                               /* it would reach the boundary: an overrun */
        M.isr |= ISR_OVW;
        M.missed++;
        if (M.cntr[2] < 0xFF) M.cntr[2]++;
        M.isr |= ISR_CNT;
        M.overruns_seen++;
        return 0;
    }
    unsigned next = M.curr + pages;
    if (next >= M.pstop) next -= ring;
    unsigned char h[4] = { 0x01, (unsigned char)next, (unsigned char)count, (unsigned char)(count >> 8) };
    unsigned a = (unsigned)M.curr << 8;
    for (unsigned i = 0; i < 4 + len + 4; i++) {
        unsigned char b = i < 4 ? h[i] : i < 4 + len ? f[i - 4] : (unsigned char)(0xC0 + i);    /* a stand-in CRC */
        M.mem[a] = b;
        if (++a >= (unsigned)M.pstop << 8) a = (unsigned)M.pstart << 8;
    }
    M.curr = (unsigned char)next;
    M.isr |= ISR_PRX;
    return 1;
}

static void m_card(int chip, const unsigned char *mac)
{
    memset(&M, 0, sizeof M);
    M.present = 1;
    M.chip = chip;
    memcpy(M.mac, mac, 6);
    M.cor_at = 0xFFFF;
    M.phy_addr = 0x10;
    M.phy_powered = 1;
    M.link = 1;
    M.full100 = 1;
    for (int i = 0; i < 32; i++) M.mem[i] = mac[(i / 2) % 6];       /* the PROM: each byte twice */
    if (chip == CHIP_NE1000) for (int i = 0; i < 32; i++) M.mem[i] = mac[i % 6];
    m_reset_chip();
}
/* CIS bytes from a list (for the test's cards) */
static void m_cis(const unsigned char *b, unsigned n)
{
    memset(M.attr, 0xFF, sizeof M.attr);
    memcpy(M.attr, b, n);
}

/* a card put in (again): powered up, unconfigured, its chip reset */
static void m_insert(void)
{
    M.present = 1;
    M.cor = M.ccsr = M.iob0 = M.iob1 = 0;
    M.gpio = 0;
    M.mii_reading = 0;
    M.hist = 0;
    m_reset_chip();
}
static void m_set_mac(const unsigned char *mac)
{
    memcpy(M.mac, mac, 6);
    for (int i = 0; i < 32; i++) M.mem[i] = mac[(i / 2) % 6];
}
