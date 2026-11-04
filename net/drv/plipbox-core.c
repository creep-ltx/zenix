/* SPDX-License-Identifier: GPL-2.0-only */
/* The plipbox's line protocol (DESIGN D39), shared by the driver (net/drv/plipbox.c) and the host test against the
 * model of the box (tools/plipbox-test.c, tools/plipbox-model.h). Included after the includer has defined:
 *   unsigned char pb_rd(u32 a); void pb_wr(u32 a, unsigned char v);   a CIA register, one access each
 *   void pb_bset(u32 a, int bit); void pb_bclr(u32 a, int bit);        one bit, read-modify-write in one instruction
 *   u32 pb_now(void);                  the E-clock (SYS_hrtime)
 *   long pb_acks(void);                the ACK line's edges since boot (DRV_PARACK), or -errno
 *   void pb_sleep(u32 ticks);
 *   void pb_log(struct pb *b, const char *fmt, ...);
 *
 * How a transfer goes (pb_proto.c's other side, as plipbox.device does it): the Amiga waits for BUSY low, drives the
 * data lines with the command and raises SEL; the box answers with BUSY high. Then every byte is a toggle of POUT
 * answered by a toggle of BUSY (the size first, high byte first, then the frame, always whole words), except in a
 * burst send, where the Amiga writes the frame's bytes and toggles POUT without waiting (the box polls POUT with its
 * interrupts off; each CIA access takes a whole E-clock cycle, which is what paces the burst on every CPU). At the end
 * the Amiga lets go of the data lines and drops SEL; the box drops BUSY. A wait that passes the transfer's deadline
 * (500 ms) ends it: SEL drops, and the box ends its side when it sees that.
 *
 * Frames from the box: it pulses ACK when it has one (or a magic frame) and keeps that one request until the Amiga
 * takes it with CMD_RECV. The kernel counts the pulses; a request is served by the next CMD_RECV, so the count read
 * once the box has taken that command (it cannot pulse during a transfer) is the count served. A CMD_RECV without a
 * request makes the box read its Ethernet chip's memory where no frame is (bridge.c fill_pkt): never done. */

static void pb_begin(struct pb *b, u32 limit)
{
    b->t0 = pb_now();
    b->limit = limit;
}

/* BUSY to become want (0 or 1), until the transfer's deadline. Every read is a CIA access (an E-clock cycle on every
 * CPU), the clock is looked at every PB_POLLCHECK reads, and once more after the deadline: the process may not have
 * run while the clock did */
static int pb_wait(struct pb *b, int want, const char *stage)
{
    for (unsigned n = 1;; n++) {
        if ((pb_rd(PB_CTRL) & 1) == want)
            return 0;
        if (!(n % PB_POLLCHECK) && (u32)(pb_now() - b->t0) > b->limit) {
            if ((pb_rd(PB_CTRL) & 1) == want)
                return 0;
            b->stage = stage;
            return -ETIMEDOUT;
        }
    }
}

/* every transfer's end: the data lines the box's again, SEL down (the box ends its side), POUT down */
static void pb_end(void)
{
    pb_wr(PB_DDATA, 0x00);
    pb_bclr(PB_CTRL, PB_SEL);
    pb_bclr(PB_CTRL, PB_POUT);
}

/* the port set up for the box: SEL and POUT low, then driven; BUSY read; the data lines not driven */
void pb_lines_on(void)
{
    pb_wr(PB_DDATA, 0x00);
    pb_bclr(PB_CTRL, PB_SEL);
    pb_bclr(PB_CTRL, PB_POUT);
    pb_bclr(PB_DCTRL, PB_BUSY);
    pb_bset(PB_DCTRL, PB_POUT);
    pb_bset(PB_DCTRL, PB_SEL);
}

/* the port left as the machine starts it (plipbox.device's end): SEL and POUT low, then not driven, nor the data */
void pb_lines_off(void)
{
    pb_wr(PB_DDATA, 0x00);
    pb_bclr(PB_CTRL, PB_SEL);
    pb_bclr(PB_CTRL, PB_POUT);
    pb_bclr(PB_DCTRL, PB_POUT);
    pb_bclr(PB_DCTRL, PB_SEL);
}

/* the same as the kernel's quiesce program: SEL first (it ends a burst the box is in), then the directions; CIA-B's
 * bits 3-7 (the serial port's) are kept by the read-modify-write */
void pb_quiesce(struct udrvquiesce *q)
{
    memset(q, 0, sizeof *q);
    q->n = 3;
    q->step[0].op = DRVS_RMW;
    q->step[0].size = 1;
    q->step[0].addr = PB_CTRL;
    q->step[0].mask = (1u << PB_SEL) | (1u << PB_POUT);
    q->step[1] = q->step[0];
    q->step[1].addr = PB_DCTRL;
    q->step[2].op = DRVS_WRITE;
    q->step[2].size = 1;
    q->step[2].addr = PB_DDATA;
    q->step[2].value = 0;
}

/* A box held in a burst's end. plipbox 0.6's burst loops end only when SEL drops, and their way out (pb_proto.c,
 * cmd_send_burst's send_burst_exit) loops back to itself for as long as SEL stays low: a burst cut off (a deadline
 * passed, the driver killed: its quiesce drops SEL) leaves the box there, BUSY high, deaf to every command. SEL up
 * with the data lines not driven, POUT up and down, each edge answered by BUSY, takes it out: the burst ends as
 * "timeout", the box waits for SEL low and drops BUSY. A box that was not held sees SEL with POUT high (ignored), then
 * low: a command it reads from the undriven lines (0xFF, not one: it answers BUSY high and waits for SEL low). */
static void pb_unstick(struct pb *b)
{
    pb_begin(b, PB_PROBE);
    pb_wr(PB_DDATA, 0x00);
    pb_bset(PB_CTRL, PB_POUT);
    pb_bset(PB_CTRL, PB_SEL);
    if (!pb_wait(b, 0, "freeing a held box")) {
        pb_bclr(PB_CTRL, PB_POUT);
        (void)pb_wait(b, 1, "freeing a held box");
    }
    pb_bclr(PB_CTRL, PB_SEL);
    pb_bclr(PB_CTRL, PB_POUT);
    pb_begin(b, PB_PROBE);
    (void)pb_wait(b, 0, "freeing a held box");
}

/* a transfer failed (r) or went through (0): the count of failures in a row and the link */
static void pb_result(struct pb *b, int r, const char *what)
{
    if (!r) {
        if (b->down)
            pb_log(b, "the plipbox answers again: link up");
        b->down = 0;
        b->fails = 0;
        return;
    }
    if (b->nlog < 8 || !(b->nlog % 256))
        pb_log(b, "%s: %s at %s (%d)", what, r == -ETIMEDOUT ? "no answer in 500 ms" : "refused",
               b->stage ? b->stage : "?", r);
    b->nlog++;
    if (++b->fails >= PB_DOWNAFTER && !b->down) {
        b->down = 1;
        pb_log(b, "the plipbox stopped answering: link down (looked for every %u s)", PB_RETRY / 100);
    }
}

/* the frame of len bytes in b->tx + 2 to the box -> 0, or -errno */
int pb_send(struct pb *b, unsigned len)
{
    unsigned char *p = b->tx;
    int r;
    if (len < PB_HDR || len > PB_MAXFRAME)
        return -EINVAL;
    p[0] = (unsigned char)(len >> 8);
    p[1] = (unsigned char)len;
    p[2 + len] = 0;                     /* the box takes whole words: an odd frame's last byte has a partner */
    unsigned words = (len + 1) / 2;
    pb_begin(b, b->down ? PB_PROBE : PB_WAIT);
    if ((r = pb_wait(b, 0, "BUSY before the command")))
        goto out;
    pb_wr(PB_DDATA, 0xFF);
    pb_wr(PB_DATA, b->burst ? PB_CMD_SENDB : PB_CMD_SEND);
    pb_bset(PB_CTRL, PB_SEL);
    if (b->burst) {
        if ((r = pb_wait(b, 1, "the command's answer")))
            goto out;
        pb_wr(PB_DATA, p[0]);
        pb_bset(PB_CTRL, PB_POUT);
        if ((r = pb_wait(b, 0, "the size")))
            goto out;
        pb_wr(PB_DATA, p[1]);
        pb_bclr(PB_CTRL, PB_POUT);
        if ((r = pb_wait(b, 1, "the burst's start")))
            goto out;
        const unsigned char *q = p + 2;
        for (unsigned i = 0; i < words; i++) {          /* the box samples the byte at each edge of POUT */
            pb_wr(PB_DATA, *q++);
            pb_bset(PB_CTRL, PB_POUT);
            pb_wr(PB_DATA, *q++);
            pb_bclr(PB_CTRL, PB_POUT);
        }
        pb_bset(PB_CTRL, PB_POUT);
        if ((r = pb_wait(b, 0, "the burst's end")))
            goto out;
        pb_bclr(PB_CTRL, PB_POUT);
        r = pb_wait(b, 1, "the last answer");
    } else {
        const unsigned char *q = p;
        for (unsigned i = 0; i <= words; i++) {         /* the size word, then the frame */
            if ((r = pb_wait(b, 1, i ? "the frame's bytes" : "the command's answer")))
                goto out;
            pb_wr(PB_DATA, *q++);
            pb_bset(PB_CTRL, PB_POUT);
            if ((r = pb_wait(b, 0, i ? "the frame's bytes" : "the size")))
                goto out;
            pb_wr(PB_DATA, *q++);
            pb_bclr(PB_CTRL, PB_POUT);
        }
        r = pb_wait(b, 1, "the last answer");
    }
out:
    pb_end();
    if (r && b->burst && b->stage && (!strcmp(b->stage, "the burst's end") || !strcmp(b->stage, "the last answer") ||
                                     !strcmp(b->stage, "the burst's start")))
        pb_unstick(b);                  /* the box may be in the burst still */
    return r;
}

/* the box's frame into buf (room for max bytes, the odd byte's partner included) -> 0 and *len (0: the box had
 * none), or -errno (-EMSGSIZE: it said more than max or a full frame: nothing taken) */
int pb_recv(struct pb *b, unsigned char *buf, unsigned max, unsigned *len)
{
    unsigned n = 0;
    int r;
    *len = 0;
    pb_begin(b, b->down ? PB_PROBE : PB_WAIT);
    if ((r = pb_wait(b, 0, "BUSY before the command")))
        goto out;
    pb_wr(PB_DDATA, 0xFF);
    pb_wr(PB_DATA, PB_CMD_RECV);
    pb_bset(PB_CTRL, PB_SEL);
    if ((r = pb_wait(b, 1, "the command's answer")))
        goto out;
    long a = pb_acks();                 /* the box has taken the command: every request it made is this one's */
    if (a >= 0)
        b->acks = (u32)a;
    pb_wr(PB_DDATA, 0x00);
    pb_bset(PB_CTRL, PB_POUT);
    if ((r = pb_wait(b, 0, "the size")))
        goto out;
    n = (unsigned)pb_rd(PB_DATA) << 8;
    pb_bclr(PB_CTRL, PB_POUT);
    if ((r = pb_wait(b, 1, "the size")))
        goto out;
    n |= pb_rd(PB_DATA);
    pb_bset(PB_CTRL, PB_POUT);
    if (n > PB_MAXFRAME || n + (n & 1) > max) {
        b->stage = "the size";
        r = -EMSGSIZE;
        goto out;
    }
    for (unsigned i = 0; i < (n + 1) / 2; i++) {
        if ((r = pb_wait(b, 0, "the frame's bytes")))
            goto out;
        *buf++ = pb_rd(PB_DATA);
        pb_bclr(PB_CTRL, PB_POUT);
        if ((r = pb_wait(b, 1, "the frame's bytes")))
            goto out;
        *buf++ = pb_rd(PB_DATA);
        pb_bset(PB_CTRL, PB_POUT);
    }
    *len = n;
out:
    pb_end();
    return r;
}

/* a magic frame: no destination (plipbox.device puts its version there; the box reads only the source and type),
 * this driver's MAC as the source */
static int pb_magic(struct pb *b, unsigned type)
{
    unsigned char *f = b->tx + 2;
    memset(f, 0, PB_HDR);
    memcpy(f + 6, b->mac, 6);
    f[12] = (unsigned char)(type >> 8);
    f[13] = (unsigned char)type;
    return pb_send(b, PB_HDR);
}

static int pb_mac_ok(const unsigned char *m)
{
    int zero = 1, ones = 1;
    for (int i = 0; i < 6; i++) {
        if (m[i]) zero = 0;
        if (m[i] != 0xFF) ones = 0;
    }
    return !zero && !ones && !(m[0] & 1);
}

/* the box made ready (at the start, and after it stopped answering): a loopback frame makes it ask for an online
 * frame (bridge.c magic_loopback), the request is taken (that frame's source is the MAC the box keeps in its
 * EEPROM: adopted when adopt, so the box writes nothing), then the online frame (the box writes its EEPROM only when
 * the MAC differs from the one it keeps). The request is taken even when no pulse comes: a box whose last request
 * was never taken (the Amiga restarted) pulses no more, and the loopback's answer is a magic frame, never a read of
 * its Ethernet chip. -> 0, or -errno (said) */
int pb_hello(struct pb *b, int adopt)
{
    unsigned char m[64];
    unsigned n;
    long a = pb_acks();
    int r;
    if (a < 0) {
        pb_log(b, "the ACK count was refused (%d)", (int)a);
        return (int)a;
    }
    b->acks = (u32)a;
    if ((r = pb_magic(b, PB_MAGIC_LOOPBACK)))
        return r;
    for (int i = 0; i < 5 && pb_acks() == (long)b->acks; i++)
        pb_sleep(1);                    /* its pulse, normally at once; 50 ms at most */
    if ((r = pb_recv(b, m, sizeof m, &n)))
        return r;
    if (n < PB_HDR || m[12] != 0xFF || m[13] != 0xFF) {
        b->stage = "the box's answer to the loopback frame (not an online request: not plipbox 0.6's bridge mode?)";
        return -EIO;
    }
    if (adopt)
        memcpy(b->mac, pb_mac_ok(m + 6) ? m + 6 : (const unsigned char[6])PB_DEFMAC, 6);
    return pb_magic(b, PB_MAGIC_ONLINE);
}

/* the start: -> 0, or -errno (said in the log, the port left as it was found) */
int pb_start(struct pb *b, int adopt)
{
    pb_lines_on();
    if (pb_rd(PB_CTRL) & 1)
        pb_unstick(b);                  /* BUSY high: a box held since a burst was cut off (or none at all) */
    int r = pb_hello(b, adopt);
    if (r) {
        if (r == -ETIMEDOUT && b->stage && !strcmp(b->stage, "BUSY before the command"))
            pb_log(b, "nothing answers on the parallel port: BUSY stays high (no plipbox, or it is off)");
        else if (r == -ETIMEDOUT)
            pb_log(b, "the plipbox does not answer (%s): is it in its bridge mode, firmware 0.6?", b->stage);
        else
            pb_log(b, "the plipbox's start failed at %s (%d)", b->stage ? b->stage : "?", r);
        pb_lines_off();
        return r;
    }
    b->fails = 0;
    b->down = 0;
    return 0;
}

/* the end: the box told (best effort: it drops the network's frames until the next online frame), the port left */
void pb_stop(struct pb *b)
{
    if (!b->down) {
        b->down = 1;                    /* the short deadline: a box that is gone holds nothing up */
        (void)pb_magic(b, PB_MAGIC_OFFLINE);
    }
    pb_lines_off();
}

/* a frame from the box: 1 for netd, 0 handled here (a magic frame, an empty answer, a bad one) */
static int pb_frame(struct pb *b, unsigned char *f, unsigned len)
{
    if (!len) {
        b->rx_empty++;                  /* a request with nothing behind it: harmless */
        return 0;
    }
    if (len < PB_HDR) {
        b->rx_bad++;
        if (b->nlog++ < 8)
            pb_log(b, "a frame of %u bytes from the box: dropped", len);
        return 0;
    }
    unsigned type = (unsigned)f[12] << 8 | f[13];
    if (type == PB_MAGIC_ONLINE) {      /* the box restarted, or a frame came while it was offline */
        b->online_asks++;
        pb_result(b, pb_magic(b, PB_MAGIC_ONLINE), "the online frame");
        return 0;
    }
    if (type == PB_MAGIC_LOOPBACK) {    /* the box's test modes: sent back as it came */
        b->loops++;
        memcpy(b->tx + 2, f, len);
        pb_result(b, pb_send(b, len), "the loopback frame");
        return 0;
    }
    if (type == PB_MAGIC_OFFLINE) {
        b->rx_bad++;
        return 0;
    }
    return 1;
}

/* the box's requests taken, at most max frames given to the sink -> frames given, or -errno (DRV_PARACK refused) */
int pb_poll(struct pb *b, const struct pb_sink *s, int max)
{
    int got = 0;
    for (int tries = 0; got < max && tries < max + 4 && !b->down; tries++) {      /* magic and empty answers too */
        long a = pb_acks();
        if (a < 0)
            return (int)a;
        if ((u32)a == b->acks)
            break;
        unsigned char *buf;
        int num = s->take(s->ctx, &buf);
        if (num < 0)
            break;                      /* no buffer: the box keeps its request until one is free */
        unsigned len;
        int r = pb_recv(b, buf, PB_BUF, &len);
        if (r == -EMSGSIZE) {
            b->rx_bad++;
            if (b->nlog++ < 8)
                pb_log(b, "the box said a frame of more than %u bytes: dropped", PB_MAXFRAME);
            pb_result(b, 0, "receiving");       /* the box answered: the line is fine */
            continue;
        }
        pb_result(b, r, "receiving");
        if (r)
            break;
        if (pb_frame(b, buf, len)) {
            s->give(s->ctx, num, len);
            b->rx_ok++;
            got++;
        }
    }
    return got;
}
