/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#ifdef SERIAL_HOST_TEST
#include "../tools/serial-fake.h"               /* Paula's serial registers as a model (tools/serial-test.sh) */
#else
#include "hw.h"
#endif
#include "random.h"

/* TX: ring buffer with opportunistic draining - console output never
 * waits for the wire. Every drain pops only what Paula's transmit
 * buffer can take RIGHT NOW (no busy-wait), and drains are hooked
 * where they matter: each putc, the timer tick, and a hot idle loop
 * (kmain spins on the backlog instead of STOPping). The TBE interrupt
 * is deliberately not used: FS-UAE's direct serial mode never delivers
 * it (verified empirically - the request pends, level 1 never fires).
 *
 * Interrupts are masked only for O(1) work per call, never across a
 * character time, so Paula's 1-byte RX buffer cannot overrun because
 * of TX. (The old busy-wait mirror did mask across whole characters;
 * RX overrun garbage ate keystrokes and once decoded to a stray ^C
 * that SIGINT'd the shell.) */

static u8 txring[4096];
static volatile u16 txhead, txtail;

void serial_init(void)
{
    SERPER = 30;                /* 115200 baud, PAL, 8N1 */
}

static void tx_drain_locked(void)       /* call with ints masked */
{
    while (txtail != txhead && (SERDATR & 0x2000)) {    /* TBE */
        SERDAT = 0x100 | txring[txtail];        /* stop bit + data */
        txtail = (u16)((txtail + 1) & 4095);
    }
}

int serial_tx_pending(void)
{
    return txtail != txhead;
}

void serial_tx_drain(void)
{
    u16 s = irq_save();
    tx_drain_locked();
    irq_restore(s);
}

/* The kernel's own output (kprintf, serial_puts): always complete - when the ring is full it waits for the port. */
static void serial_putc_wait(char c)
{
    if (c == '\n')
        serial_putc_wait('\r');
    for (;;) {
        u16 s = irq_save();
        u16 nh = (u16)((txhead + 1) & 4095);
        if (nh != txtail) {
            txring[txhead] = (u8)c;
            txhead = nh;
            tx_drain_locked();
            irq_restore(s);
            return;
        }
        tx_drain_locked();      /* ring full: wait with ints ON between */
        irq_restore(s);
    }
}

/* The console's copy (tty.c): every byte drawn on the screen is also sent here. Paula sends about 11.5 KB/s at
 * 115200 baud, far slower than the screen, so a copy that always waited capped the screen at that speed even with no
 * cable. So: until the first byte has been RECEIVED since boot (a terminal or the test harness typed something), a
 * byte that does not fit the 4 KB ring is dropped and counted (the kernel log says so, see serial_work); once
 * something has been received, someone is listening and the copy waits, so a terminal and the tests see all of it. */
static int rx_heard;                    /* a byte has been received since boot */
static volatile u32 tx_dropped;         /* console bytes dropped before that */
static u32 tx_dropped_told;

static void put_or_drop(u8 c)
{
    u16 s = irq_save();
    u16 nh = (u16)((txhead + 1) & 4095);
    if (nh != txtail) {
        txring[txhead] = c;
        txhead = nh;
    } else if (++tx_dropped >= 2 * tx_dropped_told) {
        kwork_pending = 1;              /* serial_work logs the first drop and each doubling */
    }
    tx_drain_locked();
    irq_restore(s);
}

static int serdev;                      /* /dev/ttyS0 is open: the port belongs to that program (D12) */

void serial_putc(char c)
{
    if (serdev)
        return;                         /* the console copy is off while a program owns the port */
    if (rx_heard) {
        serial_putc_wait(c);
        return;
    }
    if (c == '\n')
        put_or_drop('\r');
    put_or_drop((u8)c);
}

void serial_flush(void)
{
    while (serial_tx_pending())
        serial_tx_drain();
}

/* Receive: the RBF interrupt (level 5) drains Paula's 1-byte buffer (HRM: read SERDATR, then reset INTF_RBF to
 * avoid overrun) into a ring, and that is all it does (audit B4). serial_work, in process context, hands the bytes
 * to the tty and runs the test hook. One producer, one consumer: the interrupt writes the slot and then moves head,
 * serial_work reads the slot and then moves tail, so no lock is needed. 1024 bytes are about 90 ms of input at
 * 115200 baud; serial_work runs at least every tick while a program computes, and at once when the machine is idle
 * or a system call ends. A full ring drops the byte and counts it (serial_work reports it on the kernel log). */
#define RXQ 1024
static volatile u8 rxq[RXQ];
static volatile u16 rxq_head, rxq_tail;
static volatile u32 rxq_lost;
static u32 rxq_lost_told;

void serial_rx_isr(void)
{
    while (INTREQR & INTF_RBF) {
        u8 c = SERDATR & 0xFF;
        INTREQ = INTF_RBF;
        u16 h = rxq_head, nh = (u16)((h + 1) % RXQ);
        if (nh == rxq_tail) {
            rxq_lost++;
            continue;
        }
        rxq[h] = c;
        rxq_head = nh;
        kwork_pending = 1;
        if (c == 0xC0)
            rnd_event(RND_CR_SLIP);     /* a SLIP frame's end: the far machine's clock (D19 9.3) */
    }
}

/* Process context (kwork): the queued bytes, to the tty of the console on the screen; 0x1d starts a test/remote-control
 * command: 1-9 show console 1-9 and 0 console 10 (as Amiga+F1..F10), u/d and U/D scroll back a line or a page. */
void serial_work(void)
{
    static int hot;
    int rxq_took = 0;
    if (serdev) {                       /* the bytes wait in the ring for serdev_read; wake its reader */
        if (rxq_tail != rxq_head)
            ev_wake(&serdev_wait);
        u32 lost = rxq_lost;
        if (lost != rxq_lost_told) {
            kprintf("serial: %u bytes lost (input ring full)\n", lost - rxq_lost_told);
            rxq_lost_told = lost;
        }
        hot = 0;
        return;
    }
    while (rxq_tail != rxq_head) {
        rxq_took = 1;
        u16 t = rxq_tail;
        u8 c = rxq[t];
        rxq_tail = (u16)((t + 1) % RXQ);
        if (hot) {
            hot = 0;
            if (c >= '0' && c <= '9')
                con_switch(c == '0' ? 10 : c - '0');
            else if (c == 'u' || c == 'd')          /* scrollback one line */
                term_scroll_view(c == 'u' ? 1 : -1);
            else if (c == 'U' || c == 'D')          /* a page */
                term_scroll_view(c == 'U' ? trows - 1 : -(trows - 1));
            continue;
        }
        if (c == 0x1d) {
            hot = 1;
            continue;
        }
        tty_input(c);
    }
    if (!rx_heard && rxq_took) {
        rx_heard = 1;
        if (tx_dropped)
            kprintf("serial: input seen: the console copy now waits for the port (%u bytes were dropped before)\n",
                    tx_dropped);
        tx_dropped_told = tx_dropped;
    }
    u32 dr = tx_dropped;
    if (!rx_heard && dr && (tx_dropped_told == 0 || dr >= 2 * tx_dropped_told)) {   /* the first, then each doubling */
        if (!tx_dropped_told)
            kprintf("serial: nothing received on the serial port yet: console output that does not fit its send ring is "
                    "dropped there, not waited for (the copy waits once a byte arrives)\n");
        kprintf("serial: %u console bytes not sent so far\n", dr);
        tx_dropped_told = dr;
    }
    u32 lost = rxq_lost;
    if (lost != rxq_lost_told) {
        kprintf("serial: %u bytes lost (input ring full)\n", lost - rxq_lost_told);
        rxq_lost_told = lost;
    }
}

void serial_puts(const char *s)
{
    while (*s && !serdev)
        serial_putc_wait(*s++);
}

/* /dev/ttyS0 (DESIGN D12) */
long serdev_open(void)
{
    if (serdev)
        return -EBUSY;
    serdev = 1;
    u16 s = irq_save();
    rxq_tail = rxq_head;                /* what arrived before is the console's, not the program's */
    irq_restore(s);
    return 0;
}

void serdev_close(void)
{
    serdev = 0;
}

u32 serdev_wait;                        /* D16: the slots waiting for received bytes (event.c) */

/* poll (D16): POLLIN when bytes wait; always writable (the tick empties the send ring: rule 11) */
u16 serdev_poll(void)
{
    ev_arm(&serdev_wait);
    return (u16)(POLLOUT | (rxq_tail != rxq_head ? POLLIN : 0));
}

u32 serdev_take(u8 *buf, u32 n)
{
    u32 got = 0;
    while (got < n && rxq_tail != rxq_head) {
        u16 t = rxq_tail;
        buf[got++] = rxq[t];
        rxq_tail = (u16)((t + 1) % RXQ);
    }
    return got;
}

/* a program's write (D12): every byte, as it is. With the ring full it waits a tick at a time (the tick drains the
 * ring), never spinning with the machine stopped (audit A5); a kill ends the wait, and so do 2 s in which the port
 * took nothing (Paula wedged: logged): what was written so far, or -EINTR / -EIO if nothing */
#define SERDEV_STALL 200u
__attribute__((optimize("Os"))) long serdev_write(const u8 *buf, u32 n)
{
    u32 i = 0, since = ticks;
    while (i < n) {
        u32 i0 = i;
        u16 s = irq_save();
        tx_drain_locked();
        while (i < n && ((txhead + 1) & 4095) != txtail) {
            txring[txhead] = buf[i++];
            txhead = (u16)((txhead + 1) & 4095);
        }
        tx_drain_locked();
        irq_restore(s);
        if (i == n)
            break;
        if (i != i0)
            since = ticks;                      /* progress: the 2 s count again */
        if (current->killed || SIG_DUE(current)) {     /* a kill, or a handler (D16): what was written so far */
            if (!i && !current->killed)
                current->sig_intr = 1;
            return i ? (long)i : -EINTR;
        }
        if ((s32)(ticks - since) >= (s32)SERDEV_STALL) {
            kprintf("serial: /dev/ttyS0: the port took nothing for 2 s: write failed\n");
            return i ? (long)i : -EIO;
        }
        sleep_until(ticks + 1);
        schedule();
    }
    return (long)i;
}

/* The kernel log: everything kprintf prints is also kept in a ring, for the `dmesg` command and so that the boot
 * messages can be shown on the screen later (the display comes up after the first messages). While "mirror" is
 * on, kprintf output also goes to the visible workspace; kmain switches it off when the boot is finished.
 *
 * Previous-boot log (audit A12): the ring lives at the top of chip RAM (KLOG_AREA_SIZE below the highest chip
 * address in bootinfo), which a warm reset (Ctrl-Amiga-Amiga) is expected to leave alone: exec keeps its own data low
 * in chip RAM and its free-memory header at the *start* of a free block, and the bootblock AllocAbs()es this area
 * before it asks for any buffer. The kernel keeps it out of its pools (mem_init). The area holds two slots, each a
 * 256-byte header and an 8 KB ring; a boot writes the slot the previous boot did not use, so the previous log stays
 * where it is (no copy in the kernel's memory) for klog_show_prev() and `dmesg -p` (SYS_klog with KLOG_PREV set in
 * the offset). The header - magic, ring size, boot number, characters logged, a sum over the ring, the reason the
 * boot ended, a sum over the header - tells a real log from a half-cleared area. Both sums are updated per
 * character (O(1), interrupts masked), so a slot is consistent between any two characters. If the area overlaps the
 * kernel or the RAM disk (a machine with little chip RAM), a 1 KB ring in the BSS is used and the log does not
 * outlive a reset. */
#define KLOG_SIZE       8192
#define KLOG_HDR        256
#define KLOG_SMALL      1024
#define KLOG_MAGIC      0x5A4B4C47u     /* "ZKLG" */
#define KLOG_PREV_LINES 20

struct klog_hdr {                       /* KLOG_HDR bytes; the ring follows it */
    u32 magic;
    u32 size;                           /* ring size: KLOG_SIZE in chip RAM */
    u32 bootno;                         /* 1 on a cold start, +1 per boot that found the previous log */
    u32 total;                          /* characters ever logged in that boot: the ring holds the last `size` */
    u32 textsum;                        /* sum of the ring's bytes */
    u32 reason;                         /* KLOG_END_* */
    u32 hdrsum;                         /* ~(sum of the six words above) */
    u32 pad[(KLOG_HDR - 28) / 4];
};

static struct { struct klog_hdr h; char text[KLOG_SMALL]; } klog_small;     /* the fallback */
static struct klog_hdr *kl, *kprev;     /* kl is 0 until klog_init: nothing is logged before it */
static int klog_mirror;
static int prev_state;                  /* 0 none, 1 valid (kprev), 2 found but damaged, 3 area not usable */
static int prev_damaged;                /* a slot had the magic but failed its header checks */
static int prev_text;                   /* kprev's ring passes its sum: only then is its text shown */
static u32 klog_area_pa;

#define KTEXT(h) ((char *)(h) + KLOG_HDR)

static u32 hdr_sum(const struct klog_hdr *a)
{
    return ~(a->magic + a->size + a->bootno + a->total + a->textsum + a->reason);
}

/* A slot counts by its header alone (magic, size, header sum): the slot choice, the boot number and the reason come
 * from it. The text sum decides only whether the text is shown: an old boot menu wrote 8 bytes into slot 0's text (an
 * AllocAbs() remainder's header), and a slot rejected for that made the kernel show an older boot as the previous one */
static int slot_valid(const struct klog_hdr *a)
{
    if (a->magic != KLOG_MAGIC)
        return 0;
    if (a->size == KLOG_SIZE && a->hdrsum == hdr_sum(a))
        return 1;
    prev_damaged = 1;
    return 0;
}

static int text_valid(const struct klog_hdr *a)
{
    u32 sum = 0;
    const char *t = KTEXT(a);
    for (u32 i = 0; i < KLOG_SIZE; i++)
        sum += (u8)t[i];
    return sum == a->textsum;
}

u32 klog_area_base(void) { return klog_area_pa; }

/* very first thing in kmain, before anything prints: find the previous boot's log, start this one in the other slot */
void klog_init(const struct bootinfo *bi)
{
    u32 top = 0;
    for (int i = 0; i < bi->nregions && i < 8; i++)
        if ((bi->r[i].attrs & MEMF_CHIP) && bi->r[i].upper > top)
            top = bi->r[i].upper;
    top &= ~0xFFFu;
    u32 lo = top - KLOG_AREA_SIZE, hi = top;
    u32 kpa = bi->kernel_pa, kmem = *(u32 *)(bi->kernel_pa + 12);
    int ok = top >= 0x80000 &&
             (kpa + kmem <= lo || kpa >= hi) &&
             (!bi->ram_size || bi->ram_pa + bi->ram_size <= lo || bi->ram_pa >= hi);
    if (!ok) {
        prev_state = 3;
        kl = &klog_small.h;
        memset(kl, 0, sizeof klog_small);
        kl->size = KLOG_SMALL;
    } else {
        struct klog_hdr *s0 = (struct klog_hdr *)lo, *s1 = (struct klog_hdr *)(lo + KLOG_HDR + KLOG_SIZE);
        int v0 = slot_valid(s0), v1 = slot_valid(s1);
        klog_area_pa = lo;
        if (v0 && (!v1 || s0->bootno > s1->bootno))
            kprev = s0;
        else if (v1)
            kprev = s1;
        prev_state = kprev ? 1 : prev_damaged ? 2 : 0;
        prev_text = kprev && text_valid(kprev);
        kl = kprev == s0 ? s1 : s0;
        memset(kl, 0, KLOG_HDR + KLOG_SIZE);
        memset((u8 *)lo + UPDNOTE_OFF, 0, 16);  /* the update note says what this boot did (zenixboot read it) */
        kl->size = KLOG_SIZE;
    }
    kl->magic = KLOG_MAGIC;
    kl->bootno = kprev ? kprev->bootno + 1 : 1;
    kl->reason = KLOG_END_RUNNING;
    kl->hdrsum = hdr_sum(kl);
}

/* why this boot ends (KLOG_END_PANIC from panic and kernel exceptions), kept for the next boot */
long bootnote_set(u32 kind)
{
    if (!klog_area_pa)
        return -ENODEV;
    volatile u32 *n = (volatile u32 *)(klog_area_pa + BOOTNOTE_OFF);
    n[0] = BOOTNOTE_MAGIC;
    n[1] = kind;
    n[2] = ~(BOOTNOTE_MAGIC + kind);
    n[3] = 0;
    return 0;
}

/* the update note (D15): what zup did and whether this boot came up (login on console 1), for zenixboot. A
 * confirmation never erases a failed root half of the same version (audit3 B1): it becomes ROOTFAILED, so zenixboot
 * keeps the bundle and offers it again instead of retiring it */
long updnote_set(u32 kind, u32 version)
{
    if (!klog_area_pa)
        return -ENODEV;
    volatile u32 *n = (volatile u32 *)(klog_area_pa + UPDNOTE_OFF);
    if (kind == UPDNOTE_CONFIRMED && n[0] == UPDNOTE_MAGIC && n[3] == ~(n[0] + n[1] + n[2]) && n[2] == version &&
        (n[1] == UPDNOTE_FAILED || n[1] == UPDNOTE_ROOTFAILED))
        kind = UPDNOTE_ROOTFAILED;
    n[0] = UPDNOTE_MAGIC;
    n[1] = kind;
    n[2] = version;
    n[3] = ~(UPDNOTE_MAGIC + kind + version);
    return 0;
}

void klog_set_reason(u32 r)
{
    if (!kl)
        return;
    u16 s = irq_save();
    kl->reason = r;
    kl->hdrsum = hdr_sum(kl);
    irq_restore(s);
}

static void kputc(char c)
{
    if (kl) {
        u16 s = irq_save();
        char *t = KTEXT(kl);
        u32 i = kl->total % kl->size;
        kl->textsum += (u32)(u8)c - (u32)(u8)t[i];
        t[i] = c;
        kl->total++;
        kl->hdrsum = hdr_sum(kl);
        irq_restore(s);
    }
    if (!serdev)                        /* a program owns the port: the log stays in the ring (dmesg) */
        serial_putc_wait(c);
    if (klog_mirror) {
        if (c == '\n')
            term_log("\r", 1);
        term_log(&c, 1);
    }
}

static void kputs(const char *s)
{
    while (*s)
        kputc(*s++);
}

/* copy up to n characters of log h starting at position "off" (0 = the first ever); positions that have scrolled
 * out of the ring start at the oldest one still held. Returns the count; 0 = no more yet. */
static u32 ring_read(const struct klog_hdr *h, u32 off, char *buf, u32 n)
{
    u32 total = h->total, size = h->size, got = 0;
    const char *t = KTEXT(h);
    u32 oldest = total > size ? total - size : 0;
    if (off < oldest)
        off = oldest;
    while (got < n && off + got < total) {
        buf[got] = t[(off + got) % size];
        got++;
    }
    return got;
}

/* SYS_klog: this boot's log; with KLOG_PREV set in off, the previous boot's (nothing if there is none) */
u32 klog_read(u32 off, char *buf, u32 n)
{
    if (off & KLOG_PREV)
        return prev_text ? ring_read(kprev, off & ~KLOG_PREV, buf, n) : 0;
    return kl ? ring_read(kl, off, buf, n) : 0;
}

/* the display is up: show what was logged so far, and keep showing new messages */
void klog_to_screen(void)
{
    char c;
    u32 total = kl->total;
    u32 oldest = total > kl->size ? total - kl->size : 0;
    for (u32 i = oldest; i < total; i++) {
        c = KTEXT(kl)[i % kl->size];
        if (c == '\n')
            term_log("\r", 1);
        term_log(&c, 1);
    }
    klog_mirror = 1;
}

void klog_mirror_off(void) { klog_mirror = 0; }
void klog_write(const char *s, u32 n) { while (n--) kputc(*s++); }
void klog_mirror_on(void) { klog_mirror = 1; }

/* Says what klog_init found, and shows the previous boot's last lines (on the screen too while the boot log is
 * mirrored there), framed and marked so they cannot be taken for this boot's messages. */
void klog_show_prev(void)
{
    if (prev_state == 3) {
        kprintf("klog: chip RAM top is in use (kernel or RAM disk): this log will not survive a reset\n");
        return;
    }
    kprintf("klog: boot %u, log kept at %p (top of chip RAM) for the next boot\n", kl->bootno, (u32)kl);
    if (prev_damaged)
        kprintf("klog: a previous boot's log was found but is damaged (checksum): not shown\n");
    if (!kprev) {
        if (!prev_damaged)
            kprintf("klog: no log from a previous boot (power-on, or the memory was cleared)\n");
        return;
    }
    const char *why = kprev->reason == KLOG_END_PANIC ? " with a kernel panic" :
                      kprev->reason == KLOG_END_REBOOT ? " with reboot" :
                      " without a panic message (it hung, or was reset or switched off)";
    if (!prev_text) {
        kprintf("klog: previous boot (%u) ended%s; its log text is damaged (checksum): not shown\n", kprev->bootno, why);
        return;
    }
    const char *t = KTEXT(kprev);
    u32 size = kprev->size, end = kprev->total, first = end > size ? end - size : 0, i = end, lines = 0;
    if (i > first && t[(i - 1) % size] == '\n')
        i--;
    while (i > first) {
        if (t[(i - 1) % size] == '\n' && ++lines == KLOG_PREV_LINES)
            break;
        i--;
    }
    kprintf("=== previous boot (%u) ended here%s; its last lines (all of it: dmesg -p): ===\n", kprev->bootno, why);
    int bol = 1;
    for (; i < end; i++) {                  /* each line marked "> ", so it cannot pass for one of this boot's */
        char c = t[i % size];
        if (bol)
            kputs("> ");
        kputc(c >= 32 || c == '\n' || c == '\t' ? c : '?');
        bol = c == '\n';
    }
    if (!bol)
        kputc('\n');
    kprintf("=== end of the previous boot's log ===\n");
}

static void putnum(u32 v, u32 base, int sign)
{
    char buf[12];
    int i = 0;
    if (sign && (s32)v < 0) {
        kputc('-');
        v = (u32)-(s32)v;
    }
    do {
        u32 d = v % base;
        buf[i++] = d < 10 ? '0' + d : 'a' + d - 10;
        v /= base;
    } while (v);
    while (i--)
        kputc(buf[i]);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            kputc(*fmt);
            continue;
        }
        switch (*++fmt) {
        case 'c': kputc((char)va_arg(ap, int)); break;
        case 's': kputs(va_arg(ap, const char *)); break;
        case 'p': kputs("0x"); /* fall through */
        case 'x': putnum(va_arg(ap, u32), 16, 0); break;
        case 'u': putnum(va_arg(ap, u32), 10, 0); break;
        case 'd': putnum(va_arg(ap, u32), 10, 1); break;
        case '%': kputc('%'); break;
        default:  kputc('?'); break;
        }
    }
    va_end(ap);
}
