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
 * on, kprintf output also goes to the visible workspace; kmain switches it off when the boot is finished.
 *
 * Previous-boot log (audit A12): the ring lives in the top 16 KB of chip RAM (KLOG_AREA_SIZE below the highest chip
 * address in bootinfo), which a warm reset (Ctrl-Amiga-Amiga) is expected to leave alone: exec's free-memory headers
 * sit at the *start* of its free chip block, its own data low in chip RAM, and the bootblock AllocAbs()es this area
 * before it asks for any buffer. The kernel keeps it out of its pools (mem_init). A header with a magic, the boot
 * number, the character count, a sum over the text and a sum over the header itself lets the next boot tell a
 * real log from a half-cleared area. Before the first kprintf, klog_init() checks the area; a valid log is copied
 * into prevlog (the kernel's own BSS) and klog_show_prev() prints its last lines once the display is up;
 * `dmesg -p` reads all of it (SYS_klog with KLOG_PREV set in the offset). Then the area is reset for this boot.
 * Both sums are updated per character (O(1), interrupts masked), so the area is consistent between any two
 * characters. If the area overlaps the kernel or the RAM disk (a machine with little chip RAM), a ring in the BSS
 * is used and the log does not outlive a reset. */
#define KLOG_SIZE       8192
#define KLOG_MAGIC      0x5A4B4C47u     /* "ZKLG" */
#define KLOG_PREV_LINES 20

struct klog_area {
    u32 magic;
    u32 size;                           /* KLOG_SIZE */
    u32 bootno;                         /* 1 on a cold start, +1 per boot that found the previous log */
    u32 total;                          /* characters ever logged in that boot: the ring holds the last KLOG_SIZE */
    u32 textsum;                        /* sum of the KLOG_SIZE text bytes */
    u32 reason;                         /* KLOG_END_* */
    u32 hdrsum;                         /* ~(sum of the six words above) */
    u32 pad[57];                        /* the text starts 256 bytes in */
    char text[KLOG_SIZE];
};

static struct klog_area klog_bss;       /* the fallback when chip RAM cannot hold it */
static struct klog_area *kl = &klog_bss;
static int klog_mirror;
static char prevlog[KLOG_SIZE];         /* the previous boot's log, oldest character first */
static u32 prev_len, prev_bootno, prev_reason;
static int prev_state;                  /* 0 none, 1 valid, 2 found but damaged, 3 area not usable */
static u32 klog_area_pa;

static u32 hdr_sum(const struct klog_area *a)
{
    return ~(a->magic + a->size + a->bootno + a->total + a->textsum + a->reason);
}

u32 klog_area_base(void) { return klog_area_pa; }

/* very first thing in kmain, before anything prints: read the previous boot's log, start this one */
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
        kl = &klog_bss;
    } else {
        struct klog_area *a = (struct klog_area *)lo;
        klog_area_pa = lo;
        if (a->magic == KLOG_MAGIC) {
            u32 sum = 0;
            for (u32 i = 0; i < KLOG_SIZE; i++)
                sum += (u8)a->text[i];
            if (a->size == KLOG_SIZE && a->hdrsum == hdr_sum(a) && a->textsum == sum) {
                u32 n = a->total < KLOG_SIZE ? a->total : KLOG_SIZE;
                u32 first = a->total - n;
                for (u32 i = 0; i < n; i++)
                    prevlog[i] = a->text[(first + i) % KLOG_SIZE];
                prev_len = n;
                prev_bootno = a->bootno;
                prev_reason = a->reason;
                prev_state = 1;
            } else
                prev_state = 2;
        }
        kl = a;
    }
    memset(kl, 0, sizeof *kl);
    kl->magic = KLOG_MAGIC;
    kl->size = KLOG_SIZE;
    kl->bootno = prev_state == 1 ? prev_bootno + 1 : 1;
    kl->reason = KLOG_END_RUNNING;
    kl->hdrsum = hdr_sum(kl);
}

/* why this boot ends (KLOG_END_PANIC from panic and kernel exceptions), kept for the next boot */
void klog_set_reason(u32 r)
{
    u16 s = irq_save();
    kl->reason = r;
    kl->hdrsum = hdr_sum(kl);
    irq_restore(s);
}

static void kputc(char c)
{
    u16 s = irq_save();
    u32 i = kl->total % KLOG_SIZE;
    kl->textsum += (u32)(u8)c - (u32)(u8)kl->text[i];
    kl->text[i] = c;
    kl->total++;
    kl->hdrsum = hdr_sum(kl);
    irq_restore(s);
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
 * scrolled out of the ring start at the oldest one still held. Returns the count; 0 = no more yet.
 * With KLOG_PREV set in off: the previous boot's log (off counts from its oldest character still held). */
u32 klog_read(u32 off, char *buf, u32 n)
{
    u32 got = 0;
    if (off & KLOG_PREV) {
        off &= ~KLOG_PREV;
        while (got < n && off + got < prev_len) {
            buf[got] = prevlog[off + got];
            got++;
        }
        return got;
    }
    u32 total = kl->total;
    u32 oldest = total > KLOG_SIZE ? total - KLOG_SIZE : 0;
    if (off < oldest)
        off = oldest;
    while (got < n && off + got < total) {
        buf[got] = kl->text[(off + got) % KLOG_SIZE];
        got++;
    }
    return got;
}

/* the display is up: show what was logged so far, and keep showing new messages */
void klog_to_screen(void)
{
    char c;
    u32 total = kl->total;
    u32 oldest = total > KLOG_SIZE ? total - KLOG_SIZE : 0;
    for (u32 i = oldest; i < total; i++) {
        c = kl->text[i % KLOG_SIZE];
        if (c == '\n')
            term_log("\r", 1);
        term_log(&c, 1);
    }
    klog_mirror = 1;
}

void klog_mirror_off(void) { klog_mirror = 0; }
void klog_mirror_on(void) { klog_mirror = 1; }

/* Says what klog_init found, and shows the previous boot's last lines (on the screen too while the boot log is
 * mirrored there), framed so they cannot be taken for this boot's messages. */
void klog_show_prev(void)
{
    if (prev_state == 3) {
        kprintf("klog: chip RAM top is in use (kernel or RAM disk): this log will not survive a reset\n");
        return;
    }
    kprintf("klog: boot %u, log kept at %p (top of chip RAM) for the next boot\n", kl->bootno, klog_area_pa);
    if (prev_state == 0) {
        kprintf("klog: no log from a previous boot (power-on, or the memory was cleared)\n");
        return;
    }
    if (prev_state == 2) {
        kprintf("klog: a previous boot's log was found but is damaged (checksum): not shown\n");
        return;
    }
    u32 i = prev_len, lines = 0;
    if (i && prevlog[i - 1] == '\n')
        i--;
    while (i > 0) {
        if (prevlog[i - 1] == '\n' && ++lines == KLOG_PREV_LINES)
            break;
        i--;
    }
    kprintf("=== previous boot (%u) ended here%s; its last lines (all of it: dmesg -p): ===\n", prev_bootno,
            prev_reason == KLOG_END_PANIC ? " with a kernel panic" :
            " without a panic message (it hung, or was reset or switched off)");
    int bol = 1;
    for (; i < prev_len; i++) {             /* each line marked "> ", so it cannot pass for one of this boot's */
        char c = prevlog[i];
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
