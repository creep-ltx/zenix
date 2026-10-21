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

/* Receive: RBF interrupt (level 5) drains Paula's 1-byte buffer into
 * the console input ring; HRM: read SERDATR, then reset INTF_RBF to
 * avoid overrun. */
void serial_rx_isr(void)
{
    while (INTREQR & INTF_RBF) {
        u8 c = SERDATR & 0xFF;
        INTREQ = INTF_RBF;
        con_input_push(c);
    }
}

void serial_puts(const char *s)
{
    while (*s)
        con_putc(*s++);
}

static void putnum(u32 v, u32 base, int sign)
{
    char buf[12];
    int i = 0;
    if (sign && (s32)v < 0) {
        con_putc('-');
        v = (u32)-(s32)v;
    }
    do {
        u32 d = v % base;
        buf[i++] = d < 10 ? '0' + d : 'a' + d - 10;
        v /= base;
    } while (v);
    while (i--)
        con_putc(buf[i]);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            con_putc(*fmt);
            continue;
        }
        switch (*++fmt) {
        case 'c': con_putc((char)va_arg(ap, int)); break;
        case 's': serial_puts(va_arg(ap, const char *)); break;
        case 'p': serial_puts("0x"); /* fall through */
        case 'x': putnum(va_arg(ap, u32), 16, 0); break;
        case 'u': putnum(va_arg(ap, u32), 10, 0); break;
        case 'd': putnum(va_arg(ap, u32), 10, 1); break;
        case '%': con_putc('%'); break;
        default:  con_putc('?'); break;
        }
    }
    va_end(ap);
}
