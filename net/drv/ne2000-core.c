/* SPDX-License-Identifier: GPL-2.0-only */
/* The slot, the CIS and the chip of an NE2000 PC Card (DESIGN D36; the facts and their sources in ne2000.h). Included
 * by ne2000.c (the driver) and tools/ne2000-test.c (the host test against tools/ne2000-model.h), which define:
 *   unsigned char ne_rb(u32 a); void ne_wb(u32 a, unsigned char v); unsigned short ne_rw(u32 a); void ne_ww(u32 a, unsigned short v);   (the 68k's bus)
 *   u32 ne_ticks(void); void ne_sleep(u32 ticks); void ne_log(struct ne *n, const char *fmt, ...);
 * Polled only: the chip's interrupt mask stays 0 and Gayle's interrupt enable stays the kernel's (0). Every wait has a
 * limit in ticks and says what it waited for. Everything the card says (the CIS, the ring's headers) is checked before
 * use: a bad card fails, it never makes the driver read or write outside the slot. */

/* ---- the chip's registers through Gayle: words and even ports at PC_IO + port, odd ports at PC_IO_ODD + port - 1 */
static u32 port(const struct ne *n, unsigned r)
{
    u32 p = n->io + r;
    return p & 1 ? PC_IO_ODD + p - 1 : PC_IO + p;
}
/* reads of odd ports by a word of the pair, its low byte: on the real FA411 in the A1200 with the 1260 (2026-11-04) a
 * byte read through the odd window returned the even port next to it, while byte writes there and word reads were
 * right (the station address written by bytes read back whole by words) */
static unsigned char rd(const struct ne *n, unsigned r)
{
    u32 p = n->io + r;
    if (!(p & 1)) return ne_rb(PC_IO + p);
    return (unsigned char)ne_rw(PC_IO + p - 1);
}
static void wr(const struct ne *n, unsigned r, unsigned char v) { ne_wb(port(n, r), v); }
/* an odd port through a word read of the pair (its low byte): a second way in, for the diagnostics below */
static unsigned char rd_w(const struct ne *n, unsigned r)
{
    u32 p = n->io + r;
    unsigned short w = ne_rw(PC_IO + (p & ~1u));
    return p & 1 ? (unsigned char)w : (unsigned char)(w >> 8);
}
static unsigned char cis_byte(u32 k) { return ne_rb(PC_ATTR + 2 * k); }

/* ISR bits written as 1 are cleared; the AX88x90 sometimes keeps them: written again, up to 10 times (axnet_cs) */
static void ack(const struct ne *n, unsigned char bits)
{
    wr(n, E_ISR, bits);
    if (!n->ax) return;
    for (int i = 0; i < 10 && (rd(n, E_ISR) & bits); i++) {
        wr(n, E_ISR, 0);
        wr(n, E_ISR, bits);
    }
}

/* the remote DMA: len bytes of the card's memory at addr (the chip wraps reads at the ring's end itself) */
static void dma_setup(const struct ne *n, unsigned addr, unsigned len, unsigned char cmd)
{
    wr(n, E_CR, CR_NODMA | CR_STA);
    wr(n, E_ISR, ISR_RDC);
    wr(n, E_RBCR0, (unsigned char)len);
    wr(n, E_RBCR1, (unsigned char)(len >> 8));
    wr(n, E_RSAR0, (unsigned char)addr);
    wr(n, E_RSAR1, (unsigned char)(addr >> 8));
    wr(n, E_CR, cmd | CR_STA);
}
/* words: the card's even byte is the 68k's high byte (apne.c reads them raw), so bytes come in memory order */
static void mem_read(const struct ne *n, unsigned addr, unsigned char *b, unsigned len)
{
    unsigned w = (len + 1) & ~1u;
    dma_setup(n, addr, w, CR_RD_READ);
    u32 d = PC_IO + n->io + NE_DATA;
    unsigned i = 0;
    for (; i + 1 < len; i += 2) {
        unsigned short v = ne_rw(d);
        b[i] = (unsigned char)(v >> 8);
        b[i + 1] = (unsigned char)v;
    }
    if (i < len) b[i] = (unsigned char)(ne_rw(d) >> 8);
    ack(n, ISR_RDC);
}
/* -> 0, or -ETIMEDOUT: a DP8390 that does not say the remote DMA is done within 20 ms (the AX88x90 is not waited
 * for: Linux's axnet_cs does not, its writes end with the last word) */
static int mem_write(struct ne *n, unsigned addr, const unsigned char *b, unsigned len)
{
    unsigned w = (len + 1) & ~1u;
    dma_setup(n, addr, w, CR_RD_WRITE);
    u32 d = PC_IO + n->io + NE_DATA;
    for (unsigned i = 0; i < w; i += 2)
        ne_ww(d, (unsigned short)(b[i] << 8 | (i + 1 < len ? b[i + 1] : 0)));
    if (!n->ax) {
        u32 t0 = ne_ticks();
        while (!(rd(n, E_ISR) & ISR_RDC)) {
            if (ne_ticks() - t0 > 2) {
                ne_log(n, "the chip did not finish a remote write in 20 ms (ISR %x)", rd(n, E_ISR));
                return -ETIMEDOUT;
            }
        }
    }
    ack(n, ISR_RDC);
    return 0;
}

/* ---- the slot ---- */
int ne_card_in(void) { return (ne_rb(GAYLE_CS) & CS_CCDET) != 0; }

/* Gayle's slot set for an I/O card (0 V programming voltage, 250 ns, writes on) and the card reset: its reset line
 * held 10-20 ms, then 100 ms for it to wake (the PC Card standard's 20 ms at least; apne.c holds it 10 ms) */
void ne_slot_reset(void)
{
    ne_wb(GAYLE_CONFIG, CFG_0V_250NS);
    ne_wb(GAYLE_CS, CS_WR | CS_DA);
    ne_wb(PC_RESET, 0);
    ne_sleep(2);                                        /* 10-20 ms (a tick may end at once) */
    (void)ne_rb(PC_RESET);
    ne_sleep(10);
}

/* CIS bytes are read up to the end tuple, the end of the chain or CIS_MAXBYTES. Only what the driver needs is kept */
static void cis_printable(char *d, const unsigned char *s, unsigned n, unsigned max)
{
    unsigned k = 0;
    for (unsigned i = 0; i < n && s[i] && s[i] != 0xFF && k + 1 < max; i++) d[k++] = s[i] >= 32 && s[i] < 127 ? (char)s[i] : '?';
    d[k] = 0;
}
/* the next byte of a tuple body, -1 past its end (the entry is then bad) */
#define TB(i) ((i) < len ? (int)t[(i)++] : -1)
static int skip_ext(const unsigned char *t, unsigned len, unsigned *i)
{
    int b;
    do { if ((b = TB(*i)) < 0) return -1; } while (b & 0x80);
    return 0;
}
static int read_le(const unsigned char *t, unsigned len, unsigned *i, unsigned size, u32 *v)
{
    *v = 0;
    for (unsigned k = 0; k < size; k++) {
        int b = TB(*i);
        if (b < 0) return -1;
        *v |= (u32)b << (8 * k);
    }
    return 0;
}
/* one CFTABLE_ENTRY: its index and I/O (have: bit 0 an I/O description, -1 a bad entry) */
struct cfent { unsigned index, dflt, have, lines, nr; u32 base[2], size[2]; };
static int cftable(const unsigned char *t, unsigned len, struct cfent *e)
{
    unsigned i = 0;
    int b, f;
    memset(e, 0, sizeof *e);
    if ((b = TB(i)) < 0) return -1;
    e->index = (unsigned)b & 0x3F;
    e->dflt = (unsigned)b & 0x40;
    if ((b & 0x80) && TB(i) < 0) return -1;             /* the interface byte */
    if (i >= len) return 0;                             /* nothing more: all inherited from the default */
    if ((f = TB(i)) < 0) return -1;
    for (int p = 0; p < (f & 3); p++) {                 /* power descriptions: a selection byte, then its values */
        int sel = TB(i);
        if (sel < 0) return -1;
        for (int bit = 0; bit < 7; bit++)
            if ((sel >> bit) & 1 && skip_ext(t, len, &i)) return -1;
    }
    if (f & 4) {                                        /* timing: wait, ready, reserved scales (all 1s: none) */
        int tb = TB(i);
        if (tb < 0) return -1;
        if ((tb & 3) != 3 && skip_ext(t, len, &i)) return -1;
        if (((tb >> 2) & 7) != 7 && skip_ext(t, len, &i)) return -1;
        if (((tb >> 5) & 7) != 7 && skip_ext(t, len, &i)) return -1;
    }
    if (f & 8) {                                        /* I/O space */
        int ib = TB(i);
        if (ib < 0) return -1;
        e->have = 1;
        e->lines = (unsigned)ib & 0x1F;
        if (ib & 0x80) {
            int rb = TB(i);
            if (rb < 0) return -1;
            static const unsigned char sz[4] = { 0, 1, 2, 4 };
            unsigned nr = ((unsigned)rb & 15) + 1, as = sz[(rb >> 4) & 3], ls = sz[(rb >> 6) & 3];
            for (unsigned k = 0; k < nr; k++) {
                u32 a, l;
                if (read_le(t, len, &i, as, &a) || read_le(t, len, &i, ls, &l)) return -1;
                if (k < 2) { e->base[k] = a; e->size[k] = l + 1; }
            }
            e->nr = nr;
        }
    }
    return 0;                                           /* interrupts, memory and the rest: not needed */
}
/* an entry's I/O fits the chip (32 ports, or two windows of 16 one after the other): -> 1 with the base */
static int fits(const struct cfent *e, u32 *base, int *any)
{
    if (!e->have) return 0;
    if (!e->nr) {                                       /* no ranges: the card decodes any base on its address lines */
        if (e->lines < 5) return 0;
        *base = NE_IOBASE;
        *any = 1;
        return 1;
    }
    u32 b = e->base[0];
    int ok = e->size[0] >= 32 || (e->nr >= 2 && e->size[0] >= 16 && e->size[1] >= 16 && e->base[1] == b + 16);
    if (!ok || (b & 31) || b > 0xFFE0) return 0;        /* Gayle's I/O window is 64 KB of ports */
    *base = b;
    *any = 0;
    return 1;
}

/* -> 0 with n->cis filled, or -ENODEV (no CIS at all) */
int ne_read_cis(struct ne *n)
{
    struct ne_cis *c = &n->cis;
    static unsigned char t[256];
    struct cfent e, dflt;
    int have_dflt = 0, bad = 0;
    memset(c, 0, sizeof *c);
    c->funcid = 0xFFFF;
    c->cor = 0xFFFFFFFF;
    for (u32 pos = 0; pos + 1 < CIS_MAXBYTES;) {
        unsigned code = cis_byte(pos);
        if (code == CISTPL_END) break;
        if (code == CISTPL_NULL) { pos++; continue; }
        unsigned len = cis_byte(pos + 1);
        if (len == 0xFF) break;                         /* the end of the chain */
        if (pos + 2 + len > CIS_MAXBYTES) { bad++; break; }
        for (unsigned i = 0; i < len; i++) t[i] = cis_byte(pos + 2 + i);
        c->ntuples++;
        switch (code) {
        case CISTPL_FUNCID:
            if (len >= 1) c->funcid = t[0];
            break;
        case CISTPL_MANFID:
            if (len >= 4) { c->manf = t[0] | t[1] << 8; c->card = t[2] | t[3] << 8; }
            break;
        case CISTPL_VERS_1: {
            unsigned i = 2;
            for (int s = 0; s < 2 && i < len; s++) {
                unsigned j = i;
                while (j < len && t[j] && t[j] != 0xFF) j++;
                cis_printable(c->vers[s], t + i, j - i, sizeof c->vers[s]);
                i = j + 1;
            }
            break;
        }
        case CISTPL_CONFIG: {
            unsigned as = (t[0] & 3) + 1, ms = ((t[0] >> 2) & 15) + 1;
            if (len < 2 + as + ms) { bad++; break; }
            u32 a = 0;
            for (unsigned k = 0; k < as; k++) a |= (u32)t[2 + k] << (8 * k);
            if (a & 1 || a > PC_ATTR_SIZE / 2 - 0x20) { bad++; break; }    /* the registers must lie in the window */
            c->cor = a;
            c->rmask = t[2 + as];
            break;
        }
        case CISTPL_CFTABLE:
            if (cftable(t, len, &e)) { bad++; break; }
            if (e.dflt) { dflt = e; have_dflt = 1; }
            if (!c->index && e.index) {
                const struct cfent *io = e.have ? &e : have_dflt ? &dflt : 0;
                u32 b;
                int any;
                if (io && fits(io, &b, &any)) { c->index = e.index; c->iobase = b; c->anybase = any; c->iolines = io->lines; }
            }
            break;
        }
        pos += 2 + len;
    }
    if (bad) ne_log(n, "the card's CIS has %d bad tuples (ignored)", bad);
    return c->ntuples ? 0 : -ENODEV;
}

static int is_ax_card(const struct ne_cis *c)
{
    /* Linux's axnet_cs takes a card with its registers at 0x3c0 for an AX88190; the FA411 by its name (Linux lists it
     * there; FreeBSD: "fa411 has ax88790") */
    if (c->cor == 0x3C0) return 1;
    for (int s = 0; s < 2; s++)
        if (strstr(c->vers[s], "FA411") || strstr(c->vers[s], "AX88190") || strstr(c->vers[s], "AX88790")) return 1;
    return 0;
}

/* the card's configuration registers written: as an AX88x90 (Linux's axnet_cs: option 5 with level interrupts,
 * never enabled here, then its status and the I/O base it is to decode), or as other cards (apne.c: the CIS entry's
 * index; the status and the I/O base registers only where the CIS says they exist) */
static void configure(struct ne *n)
{
    const struct ne_cis *c = &n->cis;
    u32 r = PC_ATTR + c->cor;
    ne_wb(r + CCR_COR, n->ax ? 0x05 | COR_LEVEL : (unsigned char)c->index);
    ne_sleep(4);                                        /* 40 ms, as Linux after the option write */
    if (n->ax || (c->rmask & 2)) ne_wb(r + CCR_CCSR, 0);
    if (n->ax || (c->anybase && (c->rmask & 0x60) == 0x60)) {
        ne_wb(r + CCR_IOBASE0, (unsigned char)n->io);
        ne_wb(r + CCR_IOBASE1, (unsigned char)(n->io >> 8));
    }
}
/* the card configured as an I/O card at its base: -> 0, or -EINVAL (no configuration the driver can use) */
int ne_card_on(struct ne *n)
{
    struct ne_cis *c = &n->cis;
    if (c->cor == 0xFFFFFFFF) { ne_log(n, "the card's CIS names no configuration registers"); return -EINVAL; }
    if (!c->index) { ne_log(n, "no configuration in the card's CIS has 32 I/O ports the slot can reach"); return -EINVAL; }
    n->io = c->iobase;
    n->ax = is_ax_card(c);
    n->on = 1;
    configure(n);
    return 0;
}
/* the chip stopped and the card unconfigured (its I/O off), writes to the slot off */
void ne_card_off(struct ne *n)
{
    if (n->on) {
        n->on = 0;
        wr(n, E_CR, CR_STP | CR_NODMA);
        ne_wb(PC_ATTR + n->cis.cor + CCR_COR, 0);
    }
    ne_wb(GAYLE_CS, 0);
}

/* ---- the chip ---- */
/* the chip's own reset (its reset port read, written back) and the stopped state; -> 0. A chip whose ISR never
 * shows the reset bit is used anyway, as Linux's axnet_cs and ne.c do (they only log it): the real FA411 reads
 * ISR 4c after its reset (2026-11-04, the A1200 with the 1260), and the command register check and the frame memory
 * test that follow are what decide whether a chip is there */
static int chip_reset(struct ne *n)
{
    wr(n, E_CR, CR_STP | CR_NODMA);
    wr(n, NE_RESET, rd(n, NE_RESET));
    u32 t0 = ne_ticks();
    while (!(rd(n, E_ISR) & ISR_RST)) {
        if (ne_ticks() - t0 > 3) {
            ne_log(n, "the chip's reset bit did not come in 30 ms (ISR %x): going on, as Linux does", rd(n, E_ISR));
            break;
        }
        ne_sleep(1);
    }
    ack(n, ISR_RST);
    return 0;
}

/* the AX88x90's PHY, bit by bit through its MII port (as axnet_cs's mdio_read) */
static void mii_bit(const struct ne *n, unsigned v)
{
    wr(n, AX_MII, (unsigned char)v);
    wr(n, AX_MII, (unsigned char)(v | MII_MDC));
}
static unsigned mii_read(const struct ne *n, unsigned phy, unsigned reg)
{
    u32 cmd = 0xF6u << 10 | phy << 5 | reg, v = 0;
    for (int i = 0; i < 32; i++) mii_bit(n, MII_MDO);                       /* the preamble */
    for (int i = 14; i >= 0; i--) mii_bit(n, (cmd >> i) & 1 ? MII_MDO : 0);  /* start, read, PHY, register */
    for (int i = 19; i > 0; i--) {                                          /* turnaround, 16 bits, one more */
        wr(n, AX_MII, MII_DIR_IN);
        v = v << 1 | ((rd(n, AX_MII) & MII_MDI) != 0);
        wr(n, AX_MII, MII_DIR_IN | MII_MDC);
    }
    return (v >> 1) & 0xFFFF;
}
static int find_phy(const struct ne *n)
{
    for (unsigned a = 0; a < 32; a++) {
        unsigned j = mii_read(n, a, MII_BMSR), j2 = mii_read(n, a, MII_PHYID1);
        if (j != j2 && j != 0 && j != 0xFFFF) return (int)a;
    }
    return -1;
}

static int mac_ok(const unsigned char *m)
{
    int zero = 1, ff = 1;
    for (int i = 0; i < 6; i++) { zero &= m[i] == 0; ff &= m[i] == 0xFF; }
    return !zero && !ff && !(m[0] & 1);
}

/* ne.c's PROM read: 32 bytes in byte mode, each address byte doubled on a 16-bit card; -> 0, or -ENODEV */
static int prom_read(struct ne *n)
{
    unsigned char prom[32];
    wr(n, E_DCR, DCR_BYTE);
    dma_setup(n, 0, 32, CR_RD_READ);
    for (int i = 0; i < 32; i++) prom[i] = ne_rb(PC_IO + n->io + NE_DATA);
    ack(n, ISR_RDC);
    for (int i = 0; i < 32; i += 2)
        if (prom[i] != prom[i + 1]) {
            ne_log(n, "the PROM's bytes are not doubled: an 8-bit NE1000 card (not supported)");
            return -ENODEV;
        }
    for (int i = 0; i < 6; i++) n->mac[i] = prom[2 * i];
    return 0;
}

/* the chip as the configuration says (n->ax): -> 0, or -ENODEV (quietly when quiet: the AX88x90 guess was wrong) */
static int probe_as(struct ne *n, int quiet)
{
    static const char pattern[32] = "Zenix NE2000 memory test 012345";
    unsigned char back[32];
    if (chip_reset(n)) return -ENODEV;
    unsigned char cr = rd(n, E_CR);
    /* the start bit is not looked at: the real FA411 (an AX88790) reads 23 here, stopped with the start bit still set,
       as the DP8390's datasheet allows (2026-11-04) */
    if ((cr & (CR_STP | CR_TXP | CR_NODMA)) != (CR_STP | CR_NODMA)) {
        if (!quiet) ne_log(n, "no DP8390 at port %x (its command register reads %x)", n->io, cr);
        return -ENODEV;
    }
    wr(n, E_IMR, 0);
    ack(n, 0xFF);
    wr(n, E_RCR, RCR_MON | (n->ax ? RCR_AX_INTT : 0));
    wr(n, E_TCR, TCR_LB);
    if (!n->ax && prom_read(n)) return -ENODEV;
    wr(n, E_DCR, DCR_WORD);
    /* the frame memory answers in word mode (FreeBSD's AX88x90 probe; every NE2000 has it at 0x4000) */
    if (mem_write(n, NE_MEM0 << 8, (const unsigned char *)pattern, 32)) return -ENODEV;
    mem_read(n, NE_MEM0 << 8, back, 32);
    if (memcmp(back, pattern, 32)) {
        if (!quiet) ne_log(n, "the card's frame memory at 0x4000 does not keep what is written (word mode)");
        return -ENODEV;
    }
    n->phy = -1;
    if (n->ax) {
        n->ax = rd(n, AX_TEST) ? 2 : 1;
        if (n->ax == 2) wr(n, AX_GPIO, 0x10);           /* the AX88790's internal PHY */
        mem_read(n, 0x400, n->mac, 6);                  /* the MAC address, at 0x400 in its memory */
        n->phy = find_phy(n);
        if (n->phy < 0) {                               /* the PHY may be powered down: the CCSR bit (axnet_cs) */
            ne_wb(PC_ATTR + n->cis.cor + CCR_CCSR, CCSR_PWRDWN);
            n->phy = find_phy(n);
        }
        if (n->phy < 0) return -ENODEV;                 /* every AX88x90 has a PHY (FreeBSD's probe): not one */
    }
    return 0;
}

/* which chip, its MAC address, its PHY: -> 0, or -ENODEV (no chip that works as an NE2000). A card taken for an
 * AX88x90 (by its name or its registers at 0x3c0) that does not answer as one is configured again as its CIS says
 * and read as a DP8390 (FreeBSD: the DFE-670TXD, a DL10022 card, shares the FA411's ID) */
int ne_probe(struct ne *n)
{
    int r = probe_as(n, n->ax != 0);
    if (r && n->ax) {
        ne_log(n, "no AX88x90 answers (no chip, or no PHY on its MII port): configured as its CIS says, read as a DP8390");
        n->ax = 0;
        configure(n);
        r = probe_as(n, 0);
    }
    if (r) return r;
    if (!mac_ok(n->mac)) {
        ne_log(n, "the card's MAC address %x:%x:%x:%x:%x:%x is not a usable one", n->mac[0], n->mac[1], n->mac[2],
               n->mac[3], n->mac[4], n->mac[5]);
        return -ENODEV;
    }
    return 0;
}

/* the chip running: the NS order (stopped, word mode, loopback, the ring, the address, start, then send and receive
 * on); polled: no interrupt enabled */
int ne_start(struct ne *n)
{
    wr(n, E_CR, CR_STP | CR_NODMA);
    wr(n, E_DCR, DCR_WORD);
    wr(n, E_RBCR0, 0);
    wr(n, E_RBCR1, 0);
    wr(n, E_RCR, RCR_MON | (n->ax ? RCR_AX_INTT : 0));
    wr(n, E_TCR, TCR_LB);
    wr(n, E_TPSR, NE_MEM0);
    wr(n, E_PSTART, NE_RXSTART);
    wr(n, E_BNRY, NE_MEMEND - 1);
    wr(n, E_PSTOP, NE_MEMEND);
    ack(n, 0xFF);
    wr(n, E_IMR, 0);
    wr(n, E_CR, CR_STP | CR_NODMA | CR_PAGE1);
    for (int i = 0; i < 6; i++) wr(n, E1_PAR0 + i, n->mac[i]);
    for (int i = 0; i < 8; i++) wr(n, E1_MAR0 + i, 0);     /* no multicast */
    wr(n, E1_CURR, NE_RXSTART);
    wr(n, E_CR, CR_STP | CR_NODMA);
    for (int i = 0; i < 6; i++) {
        wr(n, E_CR, CR_STP | CR_NODMA | CR_PAGE1);
        unsigned char v = rd(n, E1_PAR0 + i);
        wr(n, E_CR, CR_STP | CR_NODMA);
        if (v != n->mac[i]) {
            ne_log(n, "the chip does not keep its station address (byte %d reads %x)", i, v);
            wr(n, E_CR, CR_STP | CR_NODMA | CR_PAGE1);
            ne_log(n, "diag: MAC %x:%x:%x:%x:%x:%x; page 1 by bytes %x %x %x %x %x %x %x %x, by words %x %x %x %x %x %x %x %x",
                   n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5],
                   rd(n, 0), rd(n, 1), rd(n, 2), rd(n, 3), rd(n, 4), rd(n, 5), rd(n, 6), rd(n, 7),
                   rd_w(n, 0), rd_w(n, 1), rd_w(n, 2), rd_w(n, 3), rd_w(n, 4), rd_w(n, 5), rd_w(n, 6), rd_w(n, 7));
            wr(n, E_CR, CR_STP | CR_NODMA);
            ne_log(n, "diag: page 0 by bytes %x %x %x %x %x %x %x %x, by words %x %x %x %x %x %x %x %x; io %x ax %d",
                   rd(n, 0), rd(n, 1), rd(n, 2), rd(n, 3), rd(n, 4), rd(n, 5), rd(n, 6), rd(n, 7),
                   rd_w(n, 0), rd_w(n, 1), rd_w(n, 2), rd_w(n, 3), rd_w(n, 4), rd_w(n, 5), rd_w(n, 6), rd_w(n, 7), n->io, n->ax);
            return -EIO;
        }
    }
    n->next = NE_RXSTART;
    n->tx_len[0] = n->tx_len[1] = 0;
    n->txing = -1;
    ack(n, 0xFF);
    wr(n, E_CR, CR_STA | CR_NODMA);
    wr(n, E_TCR, n->ax && n->full ? TCR_AX_FDU : 0);
    wr(n, E_RCR, RCR_AB | (n->ax ? RCR_AX_INTT : 0));
    return 0;
}
void ne_stop(struct ne *n)
{
    wr(n, E_CR, CR_STP | CR_NODMA);
    wr(n, E_IMR, 0);
}

/* the PHY's link, every second (now: at once); -> 1 if it changed */
int ne_link_poll(struct ne *n, int now)
{
    if (n->phy < 0) {
        int was = n->link;
        n->link = 1;
        return !was;
    }
    if (!now && ne_ticks() - n->link_at < NE_LINKEVERY) return 0;
    n->link_at = ne_ticks();
    (void)mii_read(n, (unsigned)n->phy, MII_BMSR);     /* the latched bit, then the state now */
    unsigned s = mii_read(n, (unsigned)n->phy, MII_BMSR);
    int up = s != 0xFFFF && (s & BMSR_LINK);
    if (up == n->link) return 0;
    n->link = up;
    if (up) {
        unsigned lpa = mii_read(n, (unsigned)n->phy, MII_LPA);
        n->full = (lpa & LPA_FULL) != 0;
        n->speed = lpa & LPA_100 ? 100 : 10;
        wr(n, E_TCR, n->ax && n->full ? TCR_AX_FDU : 0);
        ne_log(n, "link up, %d Mbit/s %s duplex", n->speed, n->full ? "full" : "half");
    } else
        ne_log(n, "link down");
    return 1;
}

/* the received frames in the ring, at most max; -> frames moved, or -EIO (a header that cannot be right: the
 * caller starts the chip again) */
static int rx_ring(struct ne *n, const struct ne_sink *s, int max)
{
    int moved = 0;
    unsigned char h[4];
    while (moved < max) {
        wr(n, E_CR, CR_NODMA | CR_STA | CR_PAGE1);
        unsigned curr = rd(n, E1_CURR);
        wr(n, E_CR, CR_NODMA | CR_STA);
        if (curr < NE_RXSTART || curr >= NE_MEMEND) {
            ne_log(n, "the chip's receive page %x lies outside its ring", curr);
            return -EIO;
        }
        if (n->next == curr) break;
        mem_read(n, n->next << 8, h, 4);
        unsigned st = h[0], nx = h[1], count = h[2] | h[3] << 8;
        unsigned pages = (count + 4 + 255) >> 8, want = n->next + pages;
        if (want >= NE_MEMEND) want -= NE_MEMEND - NE_RXSTART;
        if (nx < NE_RXSTART || nx >= NE_MEMEND || nx == n->next || count > NE_BUF + 4) {
            n->rx_bad++;
            ne_log(n, "a receive header that cannot be right at page %x (status %x, next %x, %d bytes)", n->next, st, nx, (int)count);
            return -EIO;
        }
        if (nx != want && n->odd_next++ < 4)            /* a clone's habit (Linux warns too): its own word is used */
            ne_log(n, "the next page %x of a frame at %x is not where its length puts it (%x)", nx, n->next, want);
        if ((st & 0x0F) != 0x01 || count < 64 || count > 1518) n->rx_bad++;     /* a bad frame: skipped */
        else {
            unsigned char *p;
            int b = s->take(s->ctx, &p);
            if (b < 0) n->rx_drop++;                    /* netd holds every buffer: dropped */
            else {
                mem_read(n, (n->next << 8) + 4, p, count - 4);   /* without the CRC */
                s->give(s->ctx, b, count - 4);
            }
        }
        n->next = nx;
        wr(n, E_BNRY, (unsigned char)(nx == NE_RXSTART ? NE_MEMEND - 1 : nx - 1));
        moved++;
    }
    return moved;
}

/* a receive overrun: National's procedure (stop, wait, clear the count, loopback, start, empty the ring, back, send
 * again what the stop cut off) */
static int overrun(struct ne *n, const struct ne_sink *s, int max)
{
    int was_txing = (rd(n, E_CR) & CR_TXP) != 0;
    wr(n, E_CR, CR_STP | CR_NODMA);
    ne_sleep(2);                                        /* at least 1.6 ms: a tick may end at once, two never */
    wr(n, E_RBCR0, 0);
    wr(n, E_RBCR1, 0);
    int resend = was_txing && !(rd(n, E_ISR) & (ISR_PTX | ISR_TXE));
    wr(n, E_TCR, TCR_LB);
    wr(n, E_CR, CR_STA | CR_NODMA);
    int r = rx_ring(n, s, max);                         /* what does not fit this batch: the next look, running */
    ack(n, ISR_OVW);
    wr(n, E_TCR, n->ax && n->full ? TCR_AX_FDU : 0);
    if (resend) wr(n, E_CR, CR_STA | CR_NODMA | CR_TXP);
    n->overruns++;
    return r;
}

static void trigger(struct ne *n, int k)
{
    wr(n, E_TPSR, (unsigned char)(NE_MEM0 + k * NE_TXPAGES));
    wr(n, E_TBCR0, (unsigned char)n->tx_len[k]);
    wr(n, E_TBCR1, (unsigned char)(n->tx_len[k] >> 8));
    wr(n, E_CR, CR_STA | CR_NODMA | CR_TXP);
    n->txing = k;
    n->tx_since = ne_ticks();
}

/* one look at the chip: frames received (at most max), sends finished and the next started, the counters; -> frames
 * moved, or -EIO (the caller starts the chip again) */
int ne_poll(struct ne *n, const struct ne_sink *s, int max)
{
    unsigned char isr = rd(n, E_ISR);
    int moved = 0, r;
    if (isr & ISR_OVW) {
        if ((r = overrun(n, s, max)) < 0) return r;
        moved += r;
    } else {
        if ((r = rx_ring(n, s, max)) < 0) return r;
        moved += r;
        if (isr & (ISR_PRX | ISR_RXE)) ack(n, isr & (ISR_PRX | ISR_RXE));
    }
    if (isr & ISR_CNT) {
        n->rx_bad += rd(n, E_CNTR0) + rd(n, E_CNTR1);
        n->rx_drop += rd(n, E_CNTR2);
        ack(n, ISR_CNT);
    }
    if (n->txing >= 0) {
        if (isr & (ISR_PTX | ISR_TXE)) {
            if (!(rd(n, E_TSR) & TSR_PTX)) n->tx_err++;
            ack(n, isr & (ISR_PTX | ISR_TXE));
            n->tx_len[n->txing] = 0;
            n->txing = -1;
            moved++;
        } else if (ne_ticks() - n->tx_since > NE_TXSTUCK) {
            ne_log(n, "a send did not end in 1 s (ISR %x): the chip is started again", isr);
            n->tx_err++;
            return -EIO;
        }
    }
    if (n->txing < 0)
        for (int k = 0; k < 2; k++)
            if (n->tx_len[k]) { trigger(n, k); break; }
    return moved;
}

int ne_tx_free(const struct ne *n) { return (n->tx_len[0] == 0) + (n->tx_len[1] == 0); }

/* a frame into a free send buffer, sent at once if the chip is idle: -> 0, -ENOSPC (both buffers full), -EINVAL (too
 * long), -EIO (the chip's remote DMA failed: the caller starts it again) */
int ne_tx(struct ne *n, const unsigned char *f, unsigned len)
{
    static unsigned char pad[60];
    int k = n->tx_len[0] == 0 && n->txing != 0 ? 0 : n->tx_len[1] == 0 && n->txing != 1 ? 1 : -1;
    if (k < 0) return -ENOSPC;
    if (len > NE_BUF - 22 || len < 14) return -EINVAL;  /* 1514 bytes at most */
    if (len < 60) {                                     /* Ethernet's shortest frame */
        memset(pad, 0, sizeof pad);
        memcpy(pad, f, len);
        f = pad;
        len = 60;
    }
    if (mem_write(n, (NE_MEM0 + (unsigned)k * NE_TXPAGES) << 8, f, len)) return -EIO;
    n->tx_len[k] = (int)len;
    if (n->txing < 0) trigger(n, k);
    return 0;
}

/* the kernel's quiesce program: the chip stopped, then the card unconfigured (both inside the slot's window) */
void ne_quiesce(const struct ne *n, struct udrvquiesce *q)
{
    memset(q, 0, sizeof *q);
    q->n = 2;
    q->step[0].op = q->step[1].op = DRVS_WRITE;
    q->step[0].size = q->step[1].size = 1;
    q->step[0].addr = port(n, E_CR);
    q->step[0].value = CR_STP | CR_NODMA;
    q->step[1].addr = PC_ATTR + n->cis.cor + CCR_COR;
    q->step[1].value = 0;
}

/* the card in the slot made ready: reset, its CIS read and checked (only a network card is touched), configured,
 * the chip found and started. -> 0, or -errno (said why; the card left unconfigured) */
int ne_bring_up(struct ne *n)
{
    int r;
    ne_slot_reset();
    if ((r = ne_read_cis(n))) {
        ne_log(n, "the card in the slot has no CIS (not a PC Card this driver knows)");
        ne_card_off(n);
        return r;
    }
    const struct ne_cis *c = &n->cis;
    int f = c->funcid == 0xFFFF ? -1 : (int)c->funcid;
    ne_log(n, "card: \"%s\" \"%s\", manufacturer %x card %x, function %d", c->vers[0], c->vers[1], c->manf, c->card, f);
    if (c->funcid != FUNCID_NETWORK && !(c->funcid == 0xFFFF && c->index)) {
        ne_log(n, "not a network card (function %d): left alone", f);
        ne_card_off(n);
        return -ENODEV;
    }
    if ((r = ne_card_on(n)) || (r = ne_probe(n)) || (r = ne_start(n))) { ne_card_off(n); return r; }
    ne_log(n, "%s at port %x (configuration %d, registers at %x)", n->ax == 2 ? "AX88790" : n->ax ? "AX88190" : "DP8390",
           n->io, n->cis.index, n->cis.cor);
    return 0;
}

/* the card's comings and goings, looked at every turn (the driver acts on the answer):
 *   NE_HERE     the card is in and working (or still away: NE_AWAY);
 *   NE_LEFT     it just left: the link is down, nothing of the slot is touched until a card is back;
 *   NE_BACK     the same card (its MAC) is back, in for half a second without a bounce, and running again;
 *   NE_OTHER    another card is in (its MAC differs): left unconfigured, the driver ends (netd starts it again);
 *   NE_BAD      the card put in cannot be used (said why) */
int ne_watch(struct ne *n)
{
    u32 now = ne_ticks();
    if (!n->gone) {
        if (ne_card_in()) return NE_HERE;
        n->gone = 1;
        n->on = 0;
        n->link = 0;
        n->away_at = now;
        n->tx_len[0] = n->tx_len[1] = 0;
        n->txing = -1;
        ne_log(n, "the card left the slot: the link is down until a card is back");
        return NE_LEFT;
    }
    if (!ne_card_in()) { n->away_at = now; return NE_AWAY; }
    if (now - n->away_at < NE_SETTLE) return NE_AWAY;   /* in, but not long enough (it bounces as it goes in) */
    unsigned char mac[6];
    memcpy(mac, n->mac, 6);
    n->gone = 0;
    if (ne_bring_up(n)) { n->gone = 1; n->away_at = ne_ticks(); return NE_BAD; }
    if (memcmp(mac, n->mac, 6)) {
        ne_log(n, "another card (its address differs)");
        ne_card_off(n);
        n->gone = 1;                                    /* not ours: nothing of it is touched */
        n->away_at = ne_ticks();
        return NE_OTHER;
    }
    n->link = 0;
    ne_link_poll(n, 1);
    ne_log(n, "the card is back");
    return NE_BACK;
}
