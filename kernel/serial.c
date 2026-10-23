#include "kernel.h"
#include "hw.h"

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

void serial_putc(char c)
{
    if (c == '\n')
        serial_putc('\r');
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
    }
}

/* Process context (kwork): the queued bytes, to the focused tty; 0x1d starts a test/remote-control command:
 * 1-9 switch workspace, Return opens a shell there, u/d and U/D scroll back a line or a page. */
void serial_work(void)
{
    static int hot;
    while (rxq_tail != rxq_head) {
        u16 t = rxq_tail;
        u8 c = rxq[t];
        rxq_tail = (u16)((t + 1) % RXQ);
        if (hot) {
            hot = 0;
            if (c >= '1' && c <= '9')
                ws_switch(c - '0');
            else if (c == '\r' || c == '\n')
                ws_request_shell(ws_cur);
            else if (c == 'u' || c == 'd')          /* scrollback one line */
                term_scroll_view(c == 'u' ? 1 : -1);
            else if (c == 'U' || c == 'D')          /* a page */
                term_scroll_view(c == 'U' ? TROWS - 1 : -(TROWS - 1));
            continue;
        }
        if (c == 0x1d) {
            hot = 1;
            continue;
        }
        tty_input(c);
    }
    u32 lost = rxq_lost;
    if (lost != rxq_lost_told) {
        kprintf("serial: %u bytes lost (input ring full)\n", lost - rxq_lost_told);
        rxq_lost_told = lost;
    }
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

/* The kernel log: everything kprintf prints is also kept in a ring, for the `dmesg` command and so that the boot
 * messages can be shown on the screen later (the display comes up after the first messages). While "mirror" is
 * on, kprintf output also goes to the visible workspace; kmain switches it off when the boot is finished. */
#define KLOG_SIZE 8192
static char klog[KLOG_SIZE];
static u32 klog_total;                  /* characters ever logged: the ring holds the last KLOG_SIZE of them */
static int klog_mirror;

static void kputc(char c)
{
    klog[klog_total++ % KLOG_SIZE] = c;
    serial_putc(c);
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

/* copy up to n logged characters starting at position "off" (0 = the first ever); positions that have
 * scrolled out of the ring start at the oldest one still held. Returns the count; 0 = no more yet. */
u32 klog_read(u32 off, char *buf, u32 n)
{
    u32 oldest = klog_total > KLOG_SIZE ? klog_total - KLOG_SIZE : 0;
    if (off < oldest)
        off = oldest;
    u32 got = 0;
    while (got < n && off + got < klog_total) {
        buf[got] = klog[(off + got) % KLOG_SIZE];
        got++;
    }
    return got;
}

/* the display is up: show what was logged so far, and keep showing new messages */
void klog_to_screen(void)
{
    char c;
    u32 oldest = klog_total > KLOG_SIZE ? klog_total - KLOG_SIZE : 0;
    for (u32 i = oldest; i < klog_total; i++) {
        c = klog[i % KLOG_SIZE];
        if (c == '\n')
            term_log("\r", 1);
        term_log(&c, 1);
    }
    klog_mirror = 1;
}

void klog_mirror_off(void) { klog_mirror = 0; }

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
