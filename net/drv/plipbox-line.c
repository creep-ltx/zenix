/* SPDX-License-Identifier: GPL-2.0-only */
/* The plipbox's line (DESIGN D39; D60 6.2): one transfer of plipbox 0.6's protocol on the CIAs. Run by the kernel
 * (kernel/drv.c's DRV_PARXFER: with D60 the CIAs are never a driver's) and by the host test against the model of the
 * box (tools/plipbox-test.c, through plipbox-core.c), so both run this one code. Included after the includer has
 * defined:
 *   unsigned char pb_rd(u32 a); void pb_wr(u32 a, unsigned char v);   a CIA register, one access each
 *   void pb_bset(u32 a, int bit); void pb_bclr(u32 a, int bit);        one bit, read-modify-write in one instruction
 *   void pb_clrbits(u32 a, unsigned mask);                            bits cleared in one instruction
 *   u32 pb_ticks(void);                the kernel's 100 Hz tick (its CIA timer: PAL and NTSC alike)
 *   long pb_acks(void);                the ACK line's edges since boot (the kernel's par_acks), bit 31 clear
 *   int pb_yield(void);                a point where a better process may run (every PB_POLLCHECK polls and every
 *                                      PB_GAPWORDS words; the kernel is not preempted); nonzero: the transfer is
 *                                      given up (killed, the claim gone), -EINTR
 *   PB_FN                              every function's attribute (the kernel's: its memory protection span, KMMU)
 *
 * How a transfer goes (pb_proto.c's other side, as plipbox.device does it): the Amiga waits for BUSY low, drives the
 * data lines with the command and raises SEL; the box answers with BUSY high. Then every byte is a toggle of POUT
 * answered by a toggle of BUSY (the size first, high byte first, then the frame, always whole words), except in a
 * burst send, where the Amiga writes the frame's bytes and toggles POUT without waiting (the box polls POUT with its
 * interrupts off; each CIA access takes a whole E-clock cycle, which is what paces the burst on every CPU). At the end
 * the Amiga lets go of the data lines and drops SEL; the box drops BUSY. A wait that passes the transfer's deadline
 * ends it: SEL drops, and the box ends its side when it sees that.
 *
 * Frames from the box: it pulses ACK when it has one (or a magic frame) and keeps that one request until the Amiga
 * takes it with CMD_RECV. The kernel counts the pulses; a request is served by the next CMD_RECV, so the count read
 * once the box has taken that command (it cannot pulse during a transfer) is the count served (acks, PARXF_ACKS). A
 * CMD_RECV without a request makes the box read its Ethernet chip's memory where no frame is (bridge.c fill_pkt):
 * the driver never asks for one. */

struct pbl {
    u32 t0, limit;                      /* the transfer's start and its deadline (ticks) */
    unsigned stage;                     /* PXS_*: where a failed transfer stopped */
    int wedged;                         /* a receive was cut between its size bytes (section 5 of D39) */
    int acked;                          /* acks is the count once the box took a receive's command */
    u32 acks;
};

PB_FN static void pb_begin(struct pbl *l, u32 limit)
{
    l->t0 = pb_ticks();
    l->limit = limit;
}

/* BUSY to become want (0 or 1), until the transfer's deadline. Every read is a CIA access (an E-clock cycle on every
 * CPU), the clock is looked at every PB_POLLCHECK reads, and once more after the deadline: the process may not have
 * run while the clock did */
PB_FN static int pb_wait(struct pbl *l, int want, unsigned stage)
{
    for (unsigned n = 1;; n++) {
        if ((pb_rd(PB_CTRL) & 1) == want)
            return 0;
        if (!(n % PB_POLLCHECK)) {
            if (pb_yield()) {           /* (the kernel's: no process waits for a box that does not answer) */
                l->stage = stage;
                return -EINTR;
            }
            if ((u32)(pb_ticks() - l->t0) > l->limit) {
                if ((pb_rd(PB_CTRL) & 1) == want)
                    return 0;
                l->stage = stage;
                return -ETIMEDOUT;
            }
        }
    }
}

/* Inside a handshaked frame: was the driver away for about the box's own wait (500 ms) since the last look? Then
 * the box may have given up on this transfer and, SEL still up, taken the lines' level as a new command and answered
 * it: BUSY toggles again, the bytes read are the pull-ups' (found in the model, a frame of the right size with a byte
 * of 0xFF in it, which nothing on the line would catch). Looked at every PB_GAPWORDS words and at the frame's end: the
 * kernel's yield point too (a better process runs at once, as it would have taken a driver in user mode) */
PB_FN static int pb_gap(struct pbl *l, u32 *last)
{
    if (pb_yield()) {
        l->stage = PXS_GAP;
        return -EINTR;
    }
    u32 t = pb_ticks();
    if ((u32)(t - *last) >= PB_GAP) {
        l->stage = PXS_GAP;
        return -ETIMEDOUT;
    }
    *last = t;
    return 0;
}

/* CIA-B's port A as it is now with SEL up and POUT down: the POUT edges of a handshaked frame (every byte answered,
 * so no edge can be missed) are then one write each instead of a bset's read and write (one CIA access a byte
 * fewer: a receive of 1,514 bytes 11.1 -> 9.0 ms on the model). The serial port's bits 3-7 are
 * written back as read: nothing else in Zenix writes this register while the driver holds the port (D39 4, the
 * rule), and its input bits (DSR, CTS, CD) only take the latch, which nobody drives. BUSY (an input) reads back 0 */
PB_FN static unsigned char pb_ctrl_lo(void)
{
    return (unsigned char)((pb_rd(PB_CTRL) & ~((1u << PB_POUT) | (1u << PB_BUSY))) | 1u << PB_SEL);
}

/* every transfer's end: the data lines the box's again, SEL and POUT down in one write (as the quiesce program) */
PB_FN static void pb_end(void)
{
    pb_wr(PB_DDATA, 0x00);
    pb_clrbits(PB_CTRL, (1u << PB_SEL) | (1u << PB_POUT));
}

/* the port set up for the box: SEL and POUT low, then driven; BUSY read; the data lines not driven */
PB_FN static void pbl_lines_on(void)
{
    pb_wr(PB_DDATA, 0x00);
    pb_bclr(PB_CTRL, PB_SEL);
    pb_bclr(PB_CTRL, PB_POUT);
    pb_bclr(PB_DCTRL, PB_BUSY);
    pb_bset(PB_DCTRL, PB_POUT);
    pb_bset(PB_DCTRL, PB_SEL);
}

/* the port left as the machine starts it (plipbox.device's end): SEL and POUT low, then not driven, nor the data */
PB_FN static void pbl_lines_off(void)
{
    pb_wr(PB_DDATA, 0x00);
    pb_clrbits(PB_CTRL, (1u << PB_SEL) | (1u << PB_POUT));
    pb_clrbits(PB_DCTRL, (1u << PB_SEL) | (1u << PB_POUT));
}

/* A box held in a burst's end. plipbox 0.6's burst loops end only when SEL drops, and their way out (pb_proto.c,
 * cmd_send_burst's send_burst_exit) loops back to itself for as long as SEL stays low: a burst cut off (a deadline
 * passed, the driver killed: its quiesce drops SEL) leaves the box there, deaf to every command, BUSY high or low
 * (the loop tests POUT before SEL: with POUT high it has dropped BUSY). SEL up, POUT up, then down, each edge
 * answered by BUSY, takes it out: the burst ends as "timeout", the box waits for SEL low and drops BUSY. The data
 * lines carry 0x00 meanwhile: a box that was not held reads that as a command, not one of its four (it answers BUSY
 * high and waits for SEL low), whatever the lines' pull-ups would have given. Done at every start and every look for
 * a lost box (microseconds for a box that was not held), and after a burst that failed. The stage of the failure
 * that led here is kept. */
PB_FN static void pbl_unstick(struct pbl *l)
{
    unsigned stage = l->stage;
    pb_begin(l, PB_PROBE);
    pb_bset(PB_CTRL, PB_POUT);
    pb_wr(PB_DATA, 0x00);
    pb_wr(PB_DDATA, 0xFF);
    pb_bset(PB_CTRL, PB_SEL);
    if (!pb_wait(l, 0, PXS_HELD)) {
        pb_bclr(PB_CTRL, PB_POUT);
        (void)pb_wait(l, 1, PXS_HELD);
    }
    pb_end();
    pb_begin(l, PB_PROBE);
    (void)pb_wait(l, 0, PXS_HELD);
    l->stage = stage;
}

/* the frame of len bytes at p + 2 to the box (p[0..1] get its size word, p[2 + len] the odd byte's partner) -> 0, or
 * -errno; limit the deadline */
PB_FN static int pb_line_send(struct pbl *l, unsigned char *p, unsigned len, int burst, u32 limit)
{
    int r;
    if (len < PB_HDR || len > PB_MAXFRAME)
        return -EINVAL;
    p[0] = (unsigned char)(len >> 8);
    p[1] = (unsigned char)len;
    p[2 + len] = 0;                     /* the box takes whole words: an odd frame's last byte has a partner */
    unsigned words = (len + 1) / 2;
    pb_begin(l, limit);
    if ((r = pb_wait(l, 0, PXS_BUSY0)))
        goto out;
    pb_wr(PB_DDATA, 0xFF);
    pb_wr(PB_DATA, burst ? PB_CMD_SENDB : PB_CMD_SEND);
    pb_bset(PB_CTRL, PB_SEL);
    if (burst) {
        if ((r = pb_wait(l, 1, PXS_CMD)))
            goto out;
        pb_wr(PB_DATA, p[0]);
        pb_bset(PB_CTRL, PB_POUT);
        if ((r = pb_wait(l, 0, PXS_SIZE)))
            goto out;
        pb_wr(PB_DATA, p[1]);
        pb_bclr(PB_CTRL, PB_POUT);
        if ((r = pb_wait(l, 1, PXS_BSTART)))
            goto out;
        const unsigned char *q = p + 2;
        for (unsigned i = 0; i < words; i++) {          /* the box samples the byte at each edge of POUT; bset and
                                                         * bclr (3 accesses a byte, as plipbox.device): the box's
                                                         * burst loop was never tried at 2. No yield in a burst: the
                                                         * box polls with its interrupts off (a few ms a frame) */
            pb_wr(PB_DATA, *q++);
            pb_bset(PB_CTRL, PB_POUT);
            pb_wr(PB_DATA, *q++);
            pb_bclr(PB_CTRL, PB_POUT);
        }
        pb_bset(PB_CTRL, PB_POUT);
        if ((r = pb_wait(l, 0, PXS_BEND)))
            goto out;
        pb_bclr(PB_CTRL, PB_POUT);
        r = pb_wait(l, 1, PXS_LAST);
    } else {
        const unsigned char *q = p;
        unsigned char lo = pb_ctrl_lo(), hi = lo | 1u << PB_POUT;
        u32 last = l->t0;
        for (unsigned i = 0; i <= words; i++) {         /* the size word, then the frame */
            if (!(i % PB_GAPWORDS) && (r = pb_gap(l, &last)))
                goto out;
            if ((r = pb_wait(l, 1, i ? PXS_BYTES : PXS_CMD)))
                goto out;
            pb_wr(PB_DATA, *q++);
            pb_wr(PB_CTRL, hi);
            if ((r = pb_wait(l, 0, i ? PXS_BYTES : PXS_SIZE)))
                goto out;
            pb_wr(PB_DATA, *q++);
            pb_wr(PB_CTRL, lo);
        }
        if (!(r = pb_wait(l, 1, PXS_LAST)))
            r = pb_gap(l, &last);
    }
out:
    pb_end();
    if (r && burst && (l->stage == PXS_BEND || l->stage == PXS_LAST || l->stage == PXS_BSTART))
        pbl_unstick(l);                  /* the box may be in the burst still */
    return r;
}

/* the box's frame into buf (room for max bytes, the odd byte's partner included) -> its bytes (0: the box had none),
 * or -errno (-EMSGSIZE: it said more than max or a full frame: nothing taken) */
PB_FN static int pb_line_recv(struct pbl *l, unsigned char *buf, unsigned max, u32 limit)
{
    unsigned n = 0;
    int r;
    pb_begin(l, limit);
    if ((r = pb_wait(l, 0, PXS_BUSY0)))
        goto out;
    pb_wr(PB_DDATA, 0xFF);
    pb_wr(PB_DATA, PB_CMD_RECV);
    pb_bset(PB_CTRL, PB_SEL);
    if ((r = pb_wait(l, 1, PXS_CMD)))
        goto out;
    long a = pb_acks();                 /* the box has taken the command: every request it made is this one's */
    if (a >= 0) {
        l->acks = (u32)a;
        l->acked = 1;
    }
    pb_wr(PB_DDATA, 0x00);
    pb_bset(PB_CTRL, PB_POUT);
    if ((r = pb_wait(l, 0, PXS_SIZE)))
        goto out;
    n = (unsigned)pb_rd(PB_DATA) << 8;
    pb_bclr(PB_CTRL, PB_POUT);
    if ((r = pb_wait(l, 1, PXS_SIZELO))) {
        l->wedged = 1;                  /* the box may have given up between the two: it keeps the lines (D39 5) */
        goto out;
    }
    n |= pb_rd(PB_DATA);
    pb_bset(PB_CTRL, PB_POUT);
    if (n > PB_MAXFRAME || n + (n & 1) > max) {
        l->stage = PXS_SIZE;
        r = -EMSGSIZE;
        goto out;
    }
    unsigned char lo = pb_ctrl_lo(), hi = lo | 1u << PB_POUT;
    u32 last = l->t0;
    for (unsigned i = 0; i < (n + 1) / 2; i++) {
        if (!(i % PB_GAPWORDS) && (r = pb_gap(l, &last)))
            goto out;
        if ((r = pb_wait(l, 0, PXS_BYTES)))
            goto out;
        *buf++ = pb_rd(PB_DATA);
        pb_wr(PB_CTRL, lo);
        if ((r = pb_wait(l, 1, PXS_BYTES)))
            goto out;
        *buf++ = pb_rd(PB_DATA);
        pb_wr(PB_CTRL, hi);
    }
    if ((r = pb_gap(l, &last)))
        goto out;
    r = (int)n;
out:
    pb_end();
    return r;
}

/* DRV_PARXFER's work (the caller checked buf: the frame and its size word, or the room, are its own memory) */
PB_FN static int pb_xfer(struct udrvparx *x, unsigned char *buf)
{
    struct pbl l = { 0, 0, 0, 0, 0, 0 };
    int r = 0;
    switch (x->op) {
    case PARX_SEND:
        r = pb_line_send(&l, buf, x->len, x->flags & PARXF_BURST, x->deadline);
        break;
    case PARX_RECV:
        r = pb_line_recv(&l, buf, x->len, x->deadline);
        break;
    case PARX_UNSTICK:
        pbl_unstick(&l);
        break;
    case PARX_ON:
        pbl_lines_on();
        break;
    case PARX_OFF:
        pbl_lines_off();
        break;
    default:
        return -EINVAL;
    }
    x->flags = (x->flags & PARXF_BURST) | (l.wedged ? PARXF_WEDGED : 0) | (l.acked ? PARXF_ACKS : 0) | l.stage << 8;
    x->acks = l.acks;
    return r;
}
