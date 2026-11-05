/* SPDX-License-Identifier: GPL-2.0-only */
/* A model of the plipbox on the Amiga's parallel port, for the host test of net/drv/plipbox-core.c (DESIGN D39).
 *
 * The Amiga's side: CIA-A's port B and its direction (the data lines), CIA-B's port A and its direction (BUSY, POUT,
 * SEL; bits 3-7 are the serial port's: given odd values that must never change). Every access the driver makes takes
 * one E-clock cycle (1.41 us, as on every real Amiga: the CIAs run on the E clock) plus the CPU's own time, and may
 * be preceded by a gap (an interrupt, another process): time is the model's, in nanoseconds.
 *
 * The box's side: plipbox 0.6's firmware as read (avr/src/pb_proto.c and bridge.c), a coroutine that runs between
 * the driver's accesses on the same clock, with the AVR's reaction times estimated from that code at 16 MHz: the
 * handshaked loops look at POUT every 1 us and answer 0.5 us later, the burst loops every 0.44 us and sample the data
 * 0.19 us after the edge, the main loop comes round every 30 us, a frame takes 1.5 us a byte to or from the ENC28J60.
 * Strict where real hardware would silently go wrong: both sides driving the data lines, a half-driven port, BUSY
 * driven by the Amiga, a CMD_RECV without a request (the real box reads its chip's memory where no frame is), the
 * serial port's bits touched. The firmware's own trap is modelled as written: a burst whose SEL drops leaves the box
 * in its exit loop until SEL rises again. */
#pragma once
#include <stdint.h>

#define M_E        1409.6              /* ns: an E-clock cycle (709379 Hz) */
#define M_NB_POLL  1000                /* ns: the handshaked wait loop's period (wait_req: timer, POUT, SEL) */
#define M_NB_ACT   500                 /* ns: from seeing POUT to the byte out (or read) and BUSY changed */
#define M_B_POLL   440                 /* ns: the burst loops' period */
#define M_B_SAMPLE 190                 /* ns: from seeing the edge to reading the data lines (burst send) */
#define M_RB_OUT   1625                /* ns: from seeing the edge to the next byte out (burst recv: DELAY + out) */
#define M_MAIN     30000               /* ns: the main loop's period (commands, the ENC28J60's count) */
#define M_ENC_BYTE 1500                /* ns: a byte to or from the ENC28J60 over SPI */
#define M_TMO      500000000ull        /* ns: the box's per-wait timeout (pb_proto_timeout, 5000 x 100 us) */
#define M_NOW_MAX  UINT64_MAX

struct mframe { unsigned len; unsigned char d[1536]; };

static int m_trace;
static struct {
    /* time */
    uint64_t t;                         /* ns */
    unsigned cpu;                       /* ns of CPU per access (0: a 68060, about 400: a 14 MHz 68020) */
    double gap_p;                       /* probability of a gap before an access */
    uint64_t gap_max, gap_min;
    unsigned long gaps;
    unsigned long gap_once_at;          /* >0: one gap of gap_once ns before this access (the count of accesses) */
    uint64_t gap_once;
    uint64_t rng;
    /* the CIAs */
    uint8_t prb, ddrb, pra, ddra;
    uint8_t ser_pra, ser_ddra;          /* bits 3-7 as given: must stay */
    unsigned long accesses;
    /* the line's faults, counted */
    unsigned long contention, halfdrive, busy_driven, serial_touched, garbage_reads, bad_cmd;
    /* the box */
    int present;                        /* 0: nothing on the port (BUSY, POUT, SEL pulled up) */
    int deaf;                           /* powered, BUSY low, never takes a command */
    int rak, drive;
    uint8_t bdata;
    int online, send_magic, req_pending;
    unsigned char mac[6];
    unsigned eeprom_writes;
    uint32_t acks;                      /* the kernel's count of the ACK line's edges */
    unsigned die_after;                 /* >0: the box goes away after this many data bytes of a transfer */
    unsigned bad_size;                  /* >0: the next frame to the Amiga says this size */
    int miss_edge;                      /* >0: the next burst send misses an edge */
    int held;                           /* in the burst's exit loop (the trap) */
    /* the box's frames */
    struct mframe in[64]; unsigned nin, inhead;     /* from the network, to the Amiga */
    struct mframe out[2048]; unsigned nout;         /* to the network, from the Amiga (the last 2048) */
    unsigned long out_total, in_dropped, magic_online, magic_offline, magic_loop;
    unsigned long status_err, status_ok;
    /* the coroutine */
    int pc;
    uint64_t at, dl;
    int timedout;
    unsigned cmd, size, words, i, got, status;
    unsigned char buf[1536];
} M;

static uint64_t m_rand(void)
{
    M.rng ^= M.rng << 13; M.rng ^= M.rng >> 7; M.rng ^= M.rng << 17;
    return M.rng;
}
static uint64_t m_rnd(uint64_t n) { return n ? m_rand() % n : 0; }

/* the lines as each side sees them */
static int m_sel(void) { return (M.ddra & 4) ? (M.pra >> 2) & 1 : 1; }
static int m_req(void) { return (M.ddra & 2) ? (M.pra >> 1) & 1 : 1; }
static int m_busy(void) { return M.present ? M.rak : 1; }
static uint8_t m_bus(void)                  /* the data lines */
{
    if (M.present && M.drive) return M.bdata;   /* both driving (counted): the AVR's pins are the stronger */
    if (M.ddrb == 0xFF) return M.prb;
    return 0xFF;                        /* pulled up */
}

/* ---- the box (pb_proto.c, bridge.c) as a coroutine: each yield gives back the time it next looks ---- */
#define BYIELD(d) do { M.at += (d); M.pc = __LINE__; if (M.at > now) return; case __LINE__:; } while (0)
/* wait for cond, looking every period; tmo 0: no deadline. Lines change only at the driver's accesses, so a
 * condition false now stays false until the access at `now` */
#define BWAIT(cond, period, tmo) do { M.dl = (tmo) ? M.at + (tmo) : M_NOW_MAX; M.timedout = 0; M.pc = __LINE__; \
    case __LINE__: if (!(cond)) { if (M.dl <= now) { M.at = M.dl; M.timedout = 1; } \
                                  else { M.at = now + 1 + m_rnd(period); return; } } } while (0)

static void m_frame_magic(void)
{
    memset(M.buf, 0xFF, 6);
    memcpy(M.buf + 6, M.mac, 6);
    M.buf[12] = M.buf[13] = 0xFF;
    M.size = 14;
}

static void m_pulse(void)
{
    M.req_pending = 1;
    M.acks++;                           /* the FLAG edge: the kernel's level-2 handler counts it at once */
}

/* proc_pkt: a frame from the Amiga */
static void m_proc(void)
{
    if (M.got < 14) { M.status_err++; return; }
    unsigned type = (unsigned)M.buf[12] << 8 | M.buf[13];
    if (type == 0xFFFF) {
        M.magic_online++;
        M.online = 1;
        if (memcmp(M.mac, M.buf + 6, 6)) { memcpy(M.mac, M.buf + 6, 6); M.eeprom_writes++; }
    } else if (type == 0xFFFE) {
        M.magic_offline++;
        M.online = 0;
    } else if (type == 0xFFFD) {
        M.magic_loop++;
        M.send_magic = 1;
        if (!M.req_pending) m_pulse();
    } else {
        struct mframe *o = &M.out[M.nout++ % 2048];
        o->len = M.got;
        memcpy(o->d, M.buf, M.got);
        M.out_total++;
        if (!M.online) { M.send_magic = 1; if (!M.req_pending) m_pulse(); }    /* request_magic */
    }
}

/* fill_pkt: the frame for a CMD_RECV */
static void m_fill(void)
{
    if (M.send_magic) {
        M.send_magic = 0;
        m_frame_magic();
    } else if (M.nin) {
        struct mframe *f = &M.in[M.inhead];
        M.inhead = (M.inhead + 1) % 64;
        M.nin--;
        memcpy(M.buf, f->d, f->len);
        M.size = f->len;
    } else {
        M.garbage_reads++;              /* the real box reads the ENC28J60 where no frame is */
        M.size = 0;
    }
    if (M.bad_size) { M.size = M.bad_size; M.bad_size = 0; }
    M.req_pending = 0;
}

static int m_dying(void)                /* the box goes away in the middle of a transfer */
{
    if (M.die_after && M.i * 2 >= M.die_after) { M.present = 0; M.die_after = 0; M.pc = 0; return 1; }
    return 0;
}

static void m_box(uint64_t now)
{
    if (!M.present || M.deaf) return;
    while (M.at <= now) {
        switch (M.pc) {
        case 0:
            for (;;) {
                if (m_sel() && !m_req()) {                                  /* pb_proto_handle */
                    M.cmd = m_bus();
                    if (M.cmd == 0x22 || M.cmd == 0x44) {
                        m_fill();
                        BYIELD(30000 + (uint64_t)M.size * M_ENC_BYTE);
                    }
                    M.rak = 1;
                    M.status = 0;
                    M.i = 0;
                    if (M.cmd == 0x22) {                                    /* cmd_recv */
                        BWAIT(m_req() || !m_sel(), M_NB_POLL, M_TMO);
                        if (M.timedout || !m_sel()) goto fail;
                        M.drive = 1;
                        BYIELD(M_NB_ACT);
                        M.bdata = (uint8_t)(M.size >> 8); M.rak = 0;
                        BWAIT(!m_req() || !m_sel(), M_NB_POLL, M_TMO);
                        if (M.timedout || !m_sel()) goto fail;      /* returns: the lines stay driven (as the firmware) */
                        BYIELD(M_NB_ACT);
                        M.bdata = (uint8_t)M.size; M.rak = 1;
                        M.words = (M.size + 1) / 2;
                        for (M.i = 0; M.i < M.words; M.i++) {
                            BWAIT(m_req() || !m_sel(), M_NB_POLL, M_TMO);
                            if (M.timedout || !m_sel()) { M.drive = 0; goto fail; }   /* break; set_input */
                            if (m_dying()) return;
                            BYIELD(M_NB_ACT);
                            M.bdata = M.buf[2 * M.i]; M.rak = 0;
                            BWAIT(!m_req() || !m_sel(), M_NB_POLL, M_TMO);
                            if (M.timedout || !m_sel()) { M.drive = 0; goto fail; }
                            BYIELD(M_NB_ACT);
                            M.bdata = M.buf[2 * M.i + 1]; M.rak = 1;
                        }
                        BWAIT(m_req() || !m_sel(), M_NB_POLL, M_TMO);
                        M.drive = 0;
                        if (M.timedout || !m_sel()) goto fail;
                    } else if (M.cmd == 0x11 || M.cmd == 0x33) {            /* cmd_send, cmd_send_burst */
                        BWAIT(m_req() || !m_sel(), M_NB_POLL, M_TMO);
                        if (M.timedout || !m_sel()) goto fail;
                        BYIELD(M_NB_ACT);
                        M.size = (unsigned)m_bus() << 8; M.rak = 0;
                        BWAIT(!m_req() || !m_sel(), M_NB_POLL, M_TMO);
                        if (M.timedout || !m_sel()) goto fail;
                        BYIELD(M_NB_ACT);
                        M.size |= m_bus();
                        if (M.cmd == 0x11) M.rak = 1;
                        if (M.size > 1514) { M.status = 5; goto fail; }    /* too large: BUSY stays as it is */
                        M.words = (M.size + 1) / 2;
                        M.got = 0;
                        if (M.cmd == 0x11) {
                            for (M.i = 0; M.i < M.words; M.i++) {
                                BWAIT(m_req() || !m_sel(), M_NB_POLL, M_TMO);
                                if (M.timedout || !m_sel()) goto fail;
                                if (m_dying()) return;
                                BYIELD(M_NB_ACT);
                                M.buf[M.got++] = m_bus(); M.rak = 0;
                                BWAIT(!m_req() || !m_sel(), M_NB_POLL, M_TMO);
                                if (M.timedout || !m_sel()) goto fail;
                                BYIELD(M_NB_ACT);
                                M.buf[M.got++] = m_bus(); M.rak = 1;
                            }
                        } else {
                            M.rak = 1;                                      /* cli(); SET_RAK: the burst */
                            for (M.i = 0; M.i < M.words; M.i++) {
                                BWAIT(m_req() || !m_sel(), M_B_POLL, 0);
                                if (!m_sel()) goto burst_exit;
                                if (m_dying()) return;
                                BYIELD(M_B_SAMPLE);
                                M.buf[M.got++] = m_bus();
                                if (M.miss_edge && M.i == 7) {              /* two edges taken for none: a word lost */
                                    M.miss_edge = 0;
                                    BWAIT(!m_req() || !m_sel(), M_B_POLL, 0);
                                    if (!m_sel()) goto burst_exit;
                                    BWAIT(m_req() || !m_sel(), M_B_POLL, 0);
                                    if (!m_sel()) goto burst_exit;
                                }
                                BWAIT(!m_req() || !m_sel(), M_B_POLL, 0);
                                if (!m_sel()) goto burst_exit;
                                BYIELD(M_B_SAMPLE);
                                M.buf[M.got++] = m_bus();
                            }
                        burst_exit:
                            /* sei(); then the firmware's way out, as written: while (!REQ) { if (!SEL) goto
                             * exit; } CLR_RAK; while (REQ) { if (!SEL) goto exit; }. With SEL low it never leaves:
                             * POUT low, it waits in the first loop (BUSY as it was); POUT high, it drops BUSY and
                             * goes round both; POUT falling then is seen in the first loop (held) or the second
                             * (out), whichever it is in */
                            for (;;) {
                                M.held = !m_sel();
                                BWAIT(m_req(), M_B_POLL, 0);                /* the first loop: POUT up */
                                M.rak = 0;
                                if (m_sel()) {
                                    BWAIT(!m_req() || !m_sel(), M_B_POLL, 0);
                                    if (!m_req()) break;
                                    continue;                               /* SEL dropped in the second loop */
                                }
                                M.held = 1;
                                BWAIT(!m_req() || m_sel(), M_B_POLL, 0);    /* going round both, SEL low */
                                if (m_sel()) { if (!m_req()) break; continue; }
                                if (m_rnd(2)) break;                        /* POUT fell in the second loop: out */
                            }
                            M.held = 0;
                            if (M.i < M.words) M.status = 2;
                            M.rak = 1;
                        }
                    } else if (M.cmd == 0x44) {                             /* cmd_recv_burst */
                        BWAIT(m_req() || !m_sel(), M_NB_POLL, M_TMO);
                        if (M.timedout || !m_sel()) goto fail;
                        M.drive = 1;
                        BYIELD(M_NB_ACT);
                        M.bdata = (uint8_t)(M.size >> 8); M.rak = 0;
                        BWAIT(!m_req() || !m_sel(), M_NB_POLL, M_TMO);
                        if (M.timedout || !m_sel()) goto fail;
                        BYIELD(M_NB_ACT);
                        M.bdata = (uint8_t)M.size; M.rak = 1;
                        BWAIT(m_req() || !m_sel(), M_NB_POLL, M_TMO);
                        if (M.timedout || !m_sel()) goto fail;
                        M.words = (M.size + 1) / 2;
                        M.rak = 0;
                        for (M.i = 0; M.i < M.words; M.i++) {
                            BYIELD(M_RB_OUT);
                            M.bdata = M.buf[2 * M.i];
                            BWAIT(!m_req() || !m_sel(), M_B_POLL, 0);
                            if (!m_sel()) break;
                            BYIELD(M_RB_OUT);
                            M.bdata = M.buf[2 * M.i + 1];
                            BWAIT(m_req() || !m_sel(), M_B_POLL, 0);
                            if (!m_sel()) break;
                        }
                        BWAIT(!m_req() || !m_sel(), M_B_POLL, 0);           /* (its exit loop: not the test's) */
                        M.rak = 1;
                        BWAIT(m_req() || !m_sel(), M_B_POLL, 0);
                        M.rak = 0;
                        M.drive = 0;                                        /* set_input (its early returns: not) */
                    } else {
                        M.bad_cmd++;
                        M.status = 4;
                    }
                    goto end;
                fail:
                    if (!M.status) M.status = 2;
                end:
                    BWAIT(!m_sel(), M_NB_POLL, M_TMO);                      /* wait_sel(0) */
                    M.rak = 0;
                    if (M.status) M.status_err++; else M.status_ok++;
                    if (!M.status && (M.cmd == 0x11 || M.cmd == 0x33)) {
                        BYIELD(30000 + (uint64_t)M.got * M_ENC_BYTE);       /* the frame to the ENC28J60 */
                        m_proc();
                    }
                }
                /* bridge_loop: the network's frames */
                if (M.nin && !M.req_pending) {
                    if (M.online) m_pulse();
                    else { M.inhead = (M.inhead + 1) % 64; M.nin--; M.in_dropped++; }
                }
                BYIELD(M_MAIN);
            }
        }
    }
}

/* ---- the Amiga's side ---- */
static void m_tick(void)                /* one access's time, a gap before it now and then, the box up to then */
{
    if (M.gap_p > 0 && (double)(m_rand() % 1000000) / 1e6 < M.gap_p) {
        M.t += M.gap_min + m_rnd(M.gap_max - M.gap_min + 1);
        M.gaps++;
    }
    if (M.gap_once_at && M.accesses + 1 == M.gap_once_at) { M.t += M.gap_once; M.gap_once_at = 0; }
    M.t += (uint64_t)(M_E + M.cpu);
    M.accesses++;
    m_box(M.t);
}
static void m_check_ports(void)
{
    if (M.ddrb != 0 && M.ddrb != 0xFF) M.halfdrive++;
    if (M.ddra & 1) M.busy_driven++;
    if ((M.pra & 0xF8) != M.ser_pra || (M.ddra & 0xF8) != M.ser_ddra) M.serial_touched++;
    if (M.ddrb == 0xFF && M.present && M.drive) M.contention++;
}
static unsigned char m_rd(u32 a)
{
    m_tick();
    switch (a) {
    case PB_DATA: return M.ddrb == 0xFF ? M.prb : m_bus();
    case PB_DDATA: return M.ddrb;
    case PB_CTRL: {
        uint8_t v = M.pra & M.ddra;
        uint8_t in = (uint8_t)(m_busy() | 2 | 4 | (M.pra & 0xF8));
        return (uint8_t)(v | (in & ~M.ddra));
    }
    case PB_DCTRL: return M.ddra;
    }
    printf("model: a read of %08x\n", (unsigned)a);
    abort();
}
static void m_wr(u32 a, unsigned char v)
{
    m_tick();
    switch (a) {
    case PB_DATA: M.prb = v; break;
    case PB_DDATA: M.ddrb = v; break;
    case PB_CTRL: M.pra = v; break;
    case PB_DCTRL: M.ddra = v; break;
    default: printf("model: a write of %08x\n", (unsigned)a); abort();
    }
    m_check_ports();
}

/* a step of the kernel's quiesce program on the model (as kernel/drv.c's quiesce) */
static void m_quiesce(const struct udrvquiesce *q)
{
    for (u32 k = 0; k < q->n; k++) {
        const struct udrvstep *s = &q->step[k];
        unsigned char v = (unsigned char)s->value;
        if (s->op == DRVS_RMW) v = (unsigned char)((m_rd(s->addr) & ~s->mask) | s->value);
        m_wr(s->addr, v);
    }
}

/* a machine: the port as the machine starts it, the serial port's bits given odd values; a box (or none) */
static void m_reset(int present, uint64_t seed)
{
    memset(&M, 0, sizeof M);
    M.rng = seed ? seed : 0x9E3779B97F4A7C15ull;
    M.ser_pra = 0xA8;                   /* serial lines: DTR low, RTS high, CD set... any odd pattern */
    M.ser_ddra = 0xC0;                  /* DTR and RTS outputs, as the ROM leaves them */
    M.pra = M.ser_pra | 0x07;
    M.ddra = M.ser_ddra;
    M.present = present;
    static const unsigned char mac[6] = { 0x1a, 0x11, 0xaf, 0xa0, 0x47, 0x11 };
    memcpy(M.mac, mac, 6);
    M.at = 0;
}
/* the box restarted (its power, its reset button): offline, nothing pending, idle */
static void m_box_restart(void)
{
    M.present = 1;
    M.deaf = 0;
    M.rak = M.drive = 0;
    M.online = M.send_magic = M.req_pending = 0;
    M.held = 0;
    M.pc = 0;
    M.at = M.t;
    M.nin = 0;
}
/* a frame from the network */
static void m_net_in(const unsigned char *d, unsigned len)
{
    if (M.nin == 64) { M.in_dropped++; return; }
    struct mframe *f = &M.in[(M.inhead + M.nin) % 64];
    f->len = len;
    memcpy(f->d, d, len);
    M.nin++;
}
/* the model's time passes with the driver doing nothing (it waits on netd's doorbell) */
static void m_idle(uint64_t ns)
{
    M.t += ns;
    m_box(M.t);
}
