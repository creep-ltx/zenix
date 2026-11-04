/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the serial copy of the console (kernel/serial.c) against a model of Paula's port (serial-fake.h):
 * with nothing received since boot a full send ring drops console bytes (counted, logged) instead of waiting; the
 * kernel's own output always waits; after a received byte the console copy waits and loses nothing, until nothing
 * has been received for 10 s (then it drops again, and waits again from the next received byte). And
 * /dev/ttyS0 (DESIGN D12): one owner; while open the console copy and the log stay off the wire and received bytes
 * wait for the reader.
 * Build and run: tools/serial-test.sh */
#include <stdio.h>
#include <stdlib.h>
#define SERIAL_HOST_TEST 1
#include "../kernel/serial.c"
#include "../user/klogread.h"

volatile int kwork_pending;
static int nomem;                                   /* kmalloc fails (the 4 KB ring of /dev/ttyS0) */
static unsigned long allocs, frees;
void *kmalloc(u32 n) { if (nomem) return 0; allocs++; return malloc(n); }
void kfree(void *p) { frees++; free(p); }
static u32 fast_free;                               /* the machine's free fast RAM (the ring's size follows it) */
void mem_stats(u32 *chip, u32 *fast) { *chip = 1u << 20; *fast = fast_free; }
int trows = 32;
u16 irq_save(void) { return 0; }
void irq_restore(u16 sr) { (void)sr; }
void con_switch(int n) { (void)n; }
void term_scroll_view(int d) { (void)d; }
void term_log(const char *s, u32 n) { (void)s; (void)n; }
static unsigned long typed;
void tty_input(u8 c) { (void)c; typed++; }
static int woken;
void ev_wake(u32 *mask) { (void)mask; woken++; }     /* D16: the reader's slot mask */
void ev_arm(u32 *mask) { (void)mask; }
static u32 rnd_samples, rnd_credit;                 /* D19: the entropy pool's samples and credit (eighths) */
void rnd_event(u32 credit8) { rnd_samples++; rnd_credit += credit8; }
static int net_pid;                                 /* the running netd (D11), 0 none */
int svc_pid(const char *name) { (void)name; return net_pid; }
/* the scheduler, for a write that waits (audit A5): each sleep is one tick, and the tick drains the ring */
volatile u32 ticks;
static struct proc me;
struct proc *current = &me;
static u32 sleeps;
void sleep_until(u32 t) { sleeps++; ticks = t; }
void schedule(void) { serial_tx_drain(); }

static int bad;
#define EXPECT_TRUE(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); bad++; } else printf("ok:   %s\n", msg); } while (0)

static int sent_has(const char *s)
{
    unsigned long n = strlen(s);
    for (unsigned long i = 0; i + n <= fake_nsent; i++)
        if (!memcmp(fake_sent + i, s, n)) return 1;
    return 0;
}
static char kgot[4 * KLOG_SIZE];
static unsigned kgot_n;
static void take(const char *s, unsigned n) { while (n-- && kgot_n < sizeof kgot) kgot[kgot_n++] = *s++; }
static int rd_klog(unsigned off, void *b, unsigned n) { return (int)klog_read(off, b, n); }
static void drain_all(void) { fake_wire_period = 1; serial_flush(); fake_look(); }

int main(void)
{
    /* 1. nobody has typed: the wire takes a byte only every 50 looks, far slower than the writer */
    fake_wire_period = 50;
    for (int i = 0; i < 20000; i++) serial_putc((char)('a' + i % 26));     /* returns: no waiting */
    EXPECT_TRUE(tx_dropped > 10000, "nobody listening: a full send ring drops console bytes instead of waiting");
    EXPECT_TRUE(kwork_pending, "... and asks kwork to log it");
    kwork_pending = 0;
    drain_all();
    unsigned long before = fake_nsent;
    serial_work();
    drain_all();
    EXPECT_TRUE(sent_has("serial: nothing received on the serial port lately"), "... the first drop is logged");
    EXPECT_TRUE(sent_has("console bytes not sent so far"), "... with the count");
    EXPECT_TRUE(fake_nsent > before, "the kernel's own log lines are sent whole");
    u32 d0 = tx_dropped;
    serial_putc('\n');
    EXPECT_TRUE(tx_dropped == d0, "an empty ring takes the byte (nothing dropped while there is room)");

    /* 2. a byte arrives: someone listens from now on */
    fake_rx = '.';
    serial_rx_isr();
    serial_work();
    drain_all();
    EXPECT_TRUE(typed == 1, "the received byte goes to the tty");
    EXPECT_TRUE(sent_has("serial: input seen: the console copy now waits for the port ("), "input seen: logged with the total");

    /* 3. now the copy waits: every byte of a long burst arrives, in order, even on a slow wire */
    fake_wire_period = 7;
    unsigned long start = fake_nsent;
    u32 d1 = tx_dropped;
    for (int i = 0; i < 20000; i++) serial_putc((char)('A' + i % 26));
    drain_all();
    int order = fake_nsent - start == 20000;
    for (unsigned long i = 0; order && i < 20000; i++) order = fake_sent[start + i] == 'A' + i % 26;
    EXPECT_TRUE(tx_dropped == d1 && order, "after input: 20000 console bytes all sent, in order, none dropped");

    /* 3b. nothing received for 10 s (the terminal closed, the cable left in): the copy drops again, says so, and waits
     * again from the next received byte */
    fake_wire_period = 50;
    ticks += 999;
    u32 d2 = tx_dropped;
    for (int i = 0; i < 5000; i++) serial_putc('w');
    drain_all();
    EXPECT_TRUE(tx_dropped == d2, "9.99 s after the last received byte the copy still waits (none dropped)");
    ticks += 1;
    kwork_pending = 0;
    fake_wire_period = 50;
    for (int i = 0; i < 20000; i++) serial_putc('q');     /* returns: no waiting */
    EXPECT_TRUE(tx_dropped > d2 + 10000 && kwork_pending, "10 s without a received byte: the copy drops again");
    serial_work();
    drain_all();
    EXPECT_TRUE(sent_has("serial: nothing received for 10 s: the console copy no longer waits"), "... and the log says so");
    fake_rx = '.';
    serial_rx_isr();
    serial_work();
    drain_all();
    fake_wire_period = 7;
    u32 d3 = tx_dropped;
    for (int i = 0; i < 20000; i++) serial_putc('r');
    drain_all();
    EXPECT_TRUE(tx_dropped == d3, "a byte received again: the copy waits again (none dropped)");

    /* 4. /dev/ttyS0 (D12): the port belongs to one program */
    fake_wire_period = 1;
    fake_rx = 'q'; serial_rx_isr();                       /* typed before the open: the console's, not the program's */
    EXPECT_TRUE(serdev_open() == 0, "/dev/ttyS0 opens");
    EXPECT_TRUE(serdev_open() == -EBUSY, "... a second open is refused (busy)");
    unsigned long s0 = fake_nsent;
    serial_putc('X'); serial_putc('\n');
    kprintf("log line while the port is a program's\n");
    drain_all();
    EXPECT_TRUE(fake_nsent == s0, "while open: no console copy and no kernel log on the wire");
    EXPECT_TRUE(serdev_write((const u8 *)"h\n", 2, 0) == 2, "a write to the port takes every byte");
    drain_all();
    EXPECT_TRUE(fake_nsent == s0 + 2 && fake_sent[s0] == 'h' && fake_sent[s0 + 1] == '\n',
                "the program's bytes go out as they are (no CR added)");
    unsigned long t0 = typed;
    u8 got[8];
    EXPECT_TRUE(serdev_take(got, 8) == 0, "what arrived before the open is not the program's");
    fake_rx = 'k'; serial_rx_isr();
    woken = 0;
    serial_work();
    EXPECT_TRUE(typed == t0 && woken, "a received byte is not typed into the console; the reader is woken");
    EXPECT_TRUE(serdev_take(got, 8) == 1 && got[0] == 'k', "... and the program reads it");

    /* 4b. while the port is a program's its receive ring is 4 KB (DESIGN D20 section 6): 5000 bytes arrive while the
     * reader does not run, 4095 wait for it in order, the rest are lost and counted */
    EXPECT_TRUE(allocs == 1 && rxq_dev, "the open allocated /dev/ttyS0's receive ring");
    for (int i = 0; i < 5000; i++) { fake_rx = (u8)(i * 7); serial_rx_isr(); }
    static u8 rb[5000];
    u32 nr = serdev_take(rb, sizeof rb), inorder = 1;
    for (u32 i = 0; i < nr; i++) if (rb[i] != (u8)(i * 7)) inorder = 0;
    EXPECT_TRUE(nr == 4095 && inorder, "4 KB ring: 4095 of 5000 unread bytes kept, in order (the console's ring kept 1023)");
    EXPECT_TRUE(rxq_lost == 905, "... the 905 that did not fit are counted as lost");
    rxq_lost_told = rxq_lost;

    /* 5. a write with the send ring full (audit A5): it waits by ticks, never spinning; a port that takes nothing for
     * 2 s fails the write; a kill ends the wait */
    static u8 big[10000];
    memset(big, 'z', sizeof big);
    fake_wire_period = 3;                                 /* slower than the writer: the ring fills */
    s0 = fake_nsent; sleeps = 0;
    EXPECT_TRUE(serdev_write(big, sizeof big, 0) == (long)sizeof big, "a slow port: a 10000-byte write takes them all");
    drain_all();
    EXPECT_TRUE(fake_nsent == s0 + sizeof big, "... every byte reached the wire");
    fake_wire_period = 0; fake_tbe = 0;                   /* Paula takes nothing any more */
    u32 t0k = ticks; sleeps = 0;
    long w1 = serdev_write(big, sizeof big, 0);
    EXPECT_TRUE(w1 == 4095, "a stuck port: the write returns what fitted the ring (4095)");
    EXPECT_TRUE(ticks - t0k >= 200 && ticks - t0k <= 202 && sleeps >= 200, "... after waiting 2 s, a tick at a time");
    t0k = ticks;
    EXPECT_TRUE(serdev_write(big, 10, 0) == -EIO && ticks - t0k >= 200, "... the next write fails with -EIO after 2 s");
    me.killed = 1; t0k = ticks;
    EXPECT_TRUE(serdev_write(big, 10, 0) == -EINTR && ticks == t0k, "a kill ends the wait at once (-EINTR)");
    me.killed = 0;
    /* 5b. DESIGN D19 5.3: netd's writes (O_NONBLOCK) never wait, and poll says truthfully when there is room */
    t0k = ticks; sleeps = 0;
    EXPECT_TRUE(serdev_write(big, 10, 1) == -EAGAIN && ticks == t0k && !sleeps, "O_NONBLOCK, the ring full: -EAGAIN at once");
    EXPECT_TRUE(!(serdev_poll() & POLLOUT), "... and poll says not writable");
    kwork_pending = 0; woken = 0;
    fake_tbe = 1; drain_all();
    EXPECT_TRUE(kwork_pending, "the drain that makes room asks kwork");
    serial_work();
    EXPECT_TRUE(woken, "... which wakes the writer");
    EXPECT_TRUE(serdev_poll() & POLLOUT, "an empty ring: writable");
    fake_wire_period = 0; fake_tbe = 0;                   /* stuck again: a big write fills the ring */
    s0 = fake_nsent;
    long nb = serdev_write(big, sizeof big, 1);
    EXPECT_TRUE(nb == 4095 && ticks == t0k && !sleeps, "O_NONBLOCK: a big write takes what fits (4095) and never waits");
    fake_tbe = 1; drain_all();
    EXPECT_TRUE(fake_nsent == s0 + 4095, "... and those bytes go out");

    /* 6. a panic while the port is a program's (audit A10): panic_begin (main.c) takes the port back first */
    serdev_drop();                                        /* what panic_begin does */
    kprintf("*** kernel panic: a test\n");
    drain_all();
    EXPECT_TRUE(sent_has("*** kernel panic: a test"), "a panic's text reaches the wire although /dev/ttyS0 was open");
    serdev_close();                                       /* the program's own close afterwards changes nothing */
    s0 = fake_nsent;
    serial_putc('Y');
    drain_all();
    EXPECT_TRUE(fake_nsent == s0 + 1 && serdev_open() == 0, "after the close the console copy is back, and it opens again");
    serdev_close();
    EXPECT_TRUE(allocs == 2 && frees == 2 && !rxq_dev && rxq == rxq_con, "each close freed the ring; the console's is back");
    for (int i = 0; i < 2000; i++) { fake_rx = 'a'; serial_rx_isr(); }
    EXPECT_TRUE(((rxq_head - rxq_tail) & rxq_mask) == 1023, "... and it holds 1023 bytes, as before");
    rxq_tail = rxq_head; rxq_lost_told = rxq_lost;
    /* no memory for the 4 KB ring: the port opens with the console's 1 KB one, and the log says so */
    nomem = 1;
    s0 = fake_nsent;
    EXPECT_TRUE(serdev_open() == 0 && !rxq_dev && rxq_mask == 1023, "no memory: /dev/ttyS0 still opens, with the 1 KB ring");
    serdev_close();
    drain_all();
    EXPECT_TRUE(sent_has("no memory for /dev/ttyS0's 4096-byte receive ring"), "... said on the log");
    nomem = 0;
    /* with netd's large profile's fast RAM (2 MB free) the ring is 32 KB */
    fast_free = 4u << 20;
    EXPECT_TRUE(serdev_open() == 0 && rxq_dev && rxq_mask == 32767, "2 MB of free fast RAM: a 32 KB ring");
    for (int i = 0; i < 40000; i++) { fake_rx = 'b'; serial_rx_isr(); }
    EXPECT_TRUE(((rxq_head - rxq_tail) & rxq_mask) == 32767, "... it holds 32767 unread bytes");
    serdev_close();
    fast_free = 0;

    /* audit 2026-10-28 S8: SLIP's frame ends earn credit only on netd's port, after data, once a tick */
    me.pid = 7;
    rnd_samples = rnd_credit = 0;
    EXPECT_TRUE(serdev_open() == 0, "/dev/ttyS0 opened by a program that is not netd (cu, telnet's rz)");
    for (int i = 0; i < 512; i++) { fake_rx = 0xC0; serial_rx_isr(); ticks++; }
    EXPECT_TRUE(rnd_samples == 512 && rnd_credit == 0, "... 512 bytes 0xC0 are mixed and credited nothing");
    serdev_close();
    net_pid = 7;
    rnd_samples = rnd_credit = 0;
    EXPECT_TRUE(serdev_open() == 0, "/dev/ttyS0 opened by netd (SLIP)");
    for (int i = 0; i < 512; i++) { fake_rx = 0xC0; serial_rx_isr(); }
    EXPECT_TRUE(rnd_credit == 0, "... 512 bytes 0xC0 in a row (END END ...): no credit");
    for (int i = 0; i < 100; i++) { fake_rx = 'x'; serial_rx_isr(); fake_rx = 0xC0; serial_rx_isr(); }
    EXPECT_TRUE(rnd_credit == RND_CR_SLIP, "... 100 frames in one tick: credited once");
    for (int i = 0; i < 10; i++) { ticks++; fake_rx = 'x'; serial_rx_isr(); fake_rx = 0xC0; serial_rx_isr(); fake_rx = 0xC0; serial_rx_isr(); }
    EXPECT_TRUE(rnd_credit == 11 * RND_CR_SLIP, "... a frame a tick: each end credited once (the second END not)");
    serdev_close();
    net_pid = 0;
    me.pid = 0;

    /* the kernel log after its ring wrapped (audit 2026-10-26 F10): dmesg's reading (user/klogread.h) gives the
     * 8 KB held exactly once, in order; the old reading (from 0, adding what came) repeats the oldest chunk */
    static char area[KLOG_HDR + KLOG_SIZE], want[KLOG_SIZE], buf[512];
    struct klog_hdr *save = kl;
    kl = (struct klog_hdr *)area;
    kl->size = KLOG_SIZE;
    for (u32 i = 0; i < 20000; i++) {           /* 20,000 characters logged: the oldest held is 11,808 */
        char c = (char)('A' + (i * 7 + i / 97) % 26);
        KTEXT(kl)[kl->total % KLOG_SIZE] = c;
        kl->total++;
        if (i >= 20000 - KLOG_SIZE) want[i - (20000 - KLOG_SIZE)] = c;
    }
    EXPECT_TRUE(klog_read(KLOG_OLDEST, buf, 1) == 20000 - KLOG_SIZE, "KLOG_OLDEST: the offset of the oldest character held");
    EXPECT_TRUE(klog_read(KLOG_OLDEST | KLOG_PREV, buf, 1) == 0, "... and 0 for a previous boot that is not there");
    kgot_n = 0;
    unsigned n = klog_dump(rd_klog, 0, take, buf, sizeof buf);
    EXPECT_TRUE(n == KLOG_SIZE && kgot_n == KLOG_SIZE && !memcmp(kgot, want, KLOG_SIZE),
                "dmesg's reading: the 8 KB held, once, in order (F10)");
    u32 off = 0; int k; kgot_n = 0;              /* the old dmesg: from 0, adding what came */
    while ((k = (int)klog_read(off, buf, sizeof buf)) > 0) { take(buf, (unsigned)k); off += (u32)k; }
    EXPECT_TRUE(kgot_n > KLOG_SIZE, "the old reading repeated the oldest chunk (the bug this fixes)");
    kl = save;
    printf(bad ? "FAILED\n" : "all ok\n");
    return bad != 0;
}
