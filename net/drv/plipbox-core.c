/* SPDX-License-Identifier: GPL-2.0-only */
/* The plipbox's protocol above the line (DESIGN D39), shared by the driver (net/drv/plipbox.c) and the host test
 * against the model of the box (tools/plipbox-test.c, tools/plipbox-model.h): the start, the magic frames, the
 * requests, the link's state. The line itself (one transfer on the CIAs) is plipbox-line.c, run by the kernel for the
 * driver (DRV_PARXFER, D60 6.2: the CIAs are never a driver's) and directly by the host test. Included after the
 * includer has defined:
 *   int pb_line(struct pb *b, struct udrvparx *x, unsigned char *buf);   one transfer (DRV_PARXFER's meaning)
 *   u32 pb_ticks(void);                the kernel's 100 Hz tick
 *   long pb_acks(void);                the ACK line's edges since boot (DRV_PARACK), or -errno
 *   void pb_sleep(u32 ticks);
 *   void pb_log(struct pb *b, const char *fmt, ...);
 * A request is served by the next CMD_RECV (plipbox-line.c says how), so the count read once the box has taken that
 * command is the count served. A CMD_RECV without a request makes the box read its Ethernet chip's memory where no
 * frame is (bridge.c fill_pkt): never done. */

/* PXS_* as the log says them */
static const char *const pb_stages[] = {
    0, "BUSY before the command", "the command's answer", "the size", "the size's low byte", "the frame's bytes",
    "the frame's bytes (the driver was away as long as the box waits: not trusted)", "the burst's start",
    "the burst's end", "the last answer", "freeing a held box",
};

/* one transfer: the stage of a failure, a receive cut between its size bytes and the count served taken from it */
static int pb_do(struct pb *b, unsigned op, unsigned flags, unsigned char *buf, unsigned len, u32 deadline)
{
    struct udrvparx x = { op, flags, 0, len, deadline, 0 };
    int r = pb_line(b, &x, buf);
    unsigned st = PARXF_STAGE(x.flags);
    if (r < 0 && op != PARX_UNSTICK)
        b->stage = st && st < sizeof pb_stages / sizeof pb_stages[0] ? pb_stages[st] : r == -EINTR ? "a signal" : 0;
    if (x.flags & PARXF_WEDGED)
        b->wedged = 1;
    if (x.flags & PARXF_ACKS)
        b->acks = x.acks;
    return r;
}

static void pb_lines_on(struct pb *b) { pb_do(b, PARX_ON, 0, 0, 0, 0); }
static void pb_lines_off(struct pb *b) { pb_do(b, PARX_OFF, 0, 0, 0, 0); }
static void pb_unstick(struct pb *b) { pb_do(b, PARX_UNSTICK, 0, 0, 0, PB_PROBE); }

/* a transfer failed (r) or went through (0): the count of failures in a row and the link */
static void pb_result(struct pb *b, int r, const char *what)
{
    if (!r) {
        b->last_ok = pb_ticks();
        if (b->down)
            pb_log(b, "the plipbox answers again: link up");
        b->down = 0;
        b->fails = 0;
        return;
    }
    if (b->nlog < 8 || !(b->nlog % 256))
        pb_log(b, "%s: %s at %s (%d)", what, r == -ETIMEDOUT ? "no answer in time" : "refused",
               b->stage ? b->stage : "?", r);
    if (b->wedged) {
        b->wedged = 0;
        pb_log(b, "a receive was cut between its size bytes: plipbox 0.6 then keeps driving the data lines until "
               "its next receive ends; if nothing moves any more, switch the box off and on");
    }
    b->nlog++;
    if (++b->fails >= PB_DOWNAFTER && !b->down) {
        b->down = 1;
        pb_log(b, "the plipbox stopped answering: link down (looked for every %u s)", PB_RETRY / 100);
    }
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

/* the frame of len bytes in b->tx + 2 to the box (the line writes its size word in front) -> 0, or -errno */
int pb_send(struct pb *b, unsigned len)
{
    u32 t0 = pb_ticks();
    int r = pb_do(b, PARX_SEND, b->burst ? PARXF_BURST : 0, b->tx, len, b->down ? PB_PROBE : PB_WAIT);
    if (!r && len > PB_HDR) {
        b->tx_n++;
        b->tx_ticks += pb_ticks() - t0;
    }
    return r;
}

/* the box's frame into buf (room for max bytes, the odd byte's partner included) -> 0 and *len (0: the box had
 * none), or -errno (-EMSGSIZE: it said more than max or a full frame: nothing taken) */
int pb_recv(struct pb *b, unsigned char *buf, unsigned max, unsigned *len)
{
    u32 t0 = pb_ticks();
    int r = pb_do(b, PARX_RECV, 0, buf, max, b->down ? PB_PROBE : PB_WAIT);
    *len = r > 0 ? (unsigned)r : 0;
    if (r > PB_HDR) {
        b->rx_n++;
        b->rx_ticks += pb_ticks() - t0;
    }
    return r < 0 ? r : 0;
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
    pb_lines_on(b);
    pb_unstick(b);                      /* a box held since a burst was cut off */
    int r = pb_hello(b, adopt);
    if (r) {
        if (r == -ETIMEDOUT && b->stage && b->stage == pb_stages[PXS_BUSY0])
            pb_log(b, "nothing answers on the parallel port: BUSY stays high (no plipbox, or it is off)");
        else if (r == -ETIMEDOUT)
            pb_log(b, "the plipbox does not answer (%s): is it in its bridge mode, firmware 0.6?", b->stage);
        else
            pb_log(b, "the plipbox's start failed at %s (%d)", b->stage ? b->stage : "?", r);
        pb_lines_off(b);
        return r;
    }
    b->fails = 0;
    b->down = 0;
    b->last_ok = pb_ticks();
    return 0;
}

/* idle for PB_KEEPALIVE ticks: an online frame (one short transfer), so a box restarted meanwhile is online again
 * and one that went away is seen (the link down after PB_DOWNAFTER of them) */
void pb_keepalive(struct pb *b)
{
    if (!b->down && (u32)(pb_ticks() - b->last_ok) >= PB_KEEPALIVE)
        pb_result(b, pb_magic(b, PB_MAGIC_ONLINE), "the keepalive");
}

/* after the driver's sleep of ticks: was a request waiting when it woke? (each such wait was up to ticks too long) */
void pb_woke(struct pb *b, int ticks)
{
    if (!ticks || b->down)
        return;
    b->naps++;
    long a = pb_acks();
    if (a >= 0 && (u32)a != b->acks)
        b->late++;
}

/* the line's figures, at the driver's end (D39 13: what the A1200 says about where a transfer's time goes) */
void pb_report(struct pb *b)
{
    pb_log(b, "frames in %lu, %lu.%lu ms each on the line; out %lu, %lu.%lu ms each; %lu sleeps, %lu of them with a "
           "frame waiting at the end", (unsigned long)b->rx_n, (unsigned long)(b->rx_n ? b->rx_ticks * 10 / b->rx_n : 0),
           (unsigned long)(b->rx_n ? b->rx_ticks * 100 / b->rx_n % 10 : 0), (unsigned long)b->tx_n,
           (unsigned long)(b->tx_n ? b->tx_ticks * 10 / b->tx_n : 0),
           (unsigned long)(b->tx_n ? b->tx_ticks * 100 / b->tx_n % 10 : 0), (unsigned long)b->naps,
           (unsigned long)b->late);
}

/* the end: the box told (best effort: it drops the network's frames until the next online frame), the port left */
void pb_stop(struct pb *b)
{
    if (!b->down) {
        b->down = 1;                    /* the short deadline: a box that is gone holds nothing up */
        (void)pb_magic(b, PB_MAGIC_OFFLINE);
    }
    pb_lines_off(b);
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

/* the driver's wait before its next turn (drv_wait's ticks): none after a turn that moved something, then the next
 * tick twice, then every second tick; every PB_RETRY / 20 ticks while the link is down */
int pb_idle_ticks(const struct pb *b, int idle)
{
    return b->down ? 10 : idle > 2 ? 2 : idle ? 1 : 0;
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
