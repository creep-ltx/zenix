/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the serial copy of the console (kernel/serial.c) against a model of Paula's port (serial-fake.h):
 * with nothing received since boot a full send ring drops console bytes (counted, logged) instead of waiting; the
 * kernel's own output always waits; after the first received byte the console copy waits and loses nothing. And
 * /dev/ttyS0 (DESIGN D12): one owner; while open the console copy and the log stay off the wire and received bytes
 * wait for the reader.
 * Build and run: tools/serial-test.sh */
#include <stdio.h>
#define SERIAL_HOST_TEST 1
#include "../kernel/serial.c"

volatile int kwork_pending;
int trows = 32;
u16 irq_save(void) { return 0; }
void irq_restore(u16 sr) { (void)sr; }
void con_switch(int n) { (void)n; }
void term_scroll_view(int d) { (void)d; }
void term_log(const char *s, u32 n) { (void)s; (void)n; }
static unsigned long typed;
void tty_input(u8 c) { (void)c; typed++; }
static int woken;
void wake_io(void) { woken++; }
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
    EXPECT_TRUE(sent_has("serial: nothing received on the serial port yet"), "... the first drop is logged");
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
    EXPECT_TRUE(serdev_write((const u8 *)"h\n", 2) == 2, "a write to the port takes every byte");
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

    /* 5. a write with the send ring full (audit A5): it waits by ticks, never spinning; a port that takes nothing for
     * 2 s fails the write; a kill ends the wait */
    static u8 big[10000];
    memset(big, 'z', sizeof big);
    fake_wire_period = 3;                                 /* slower than the writer: the ring fills */
    s0 = fake_nsent; sleeps = 0;
    EXPECT_TRUE(serdev_write(big, sizeof big) == (long)sizeof big, "a slow port: a 10000-byte write takes them all");
    drain_all();
    EXPECT_TRUE(fake_nsent == s0 + sizeof big, "... every byte reached the wire");
    fake_wire_period = 0; fake_tbe = 0;                   /* Paula takes nothing any more */
    u32 t0k = ticks; sleeps = 0;
    long w1 = serdev_write(big, sizeof big);
    EXPECT_TRUE(w1 == 4095, "a stuck port: the write returns what fitted the ring (4095)");
    EXPECT_TRUE(ticks - t0k >= 200 && ticks - t0k <= 202 && sleeps >= 200, "... after waiting 2 s, a tick at a time");
    t0k = ticks;
    EXPECT_TRUE(serdev_write(big, 10) == -EIO && ticks - t0k >= 200, "... the next write fails with -EIO after 2 s");
    me.killed = 1; t0k = ticks;
    EXPECT_TRUE(serdev_write(big, 10) == -EINTR && ticks == t0k, "a kill ends the wait at once (-EINTR)");
    me.killed = 0;
    fake_tbe = 1; drain_all();

    /* 6. a panic while the port is a program's (audit A10): panic_begin (main.c) takes the port back first */
    serdev_close();                                       /* what panic_begin does */
    kprintf("*** kernel panic: a test\n");
    drain_all();
    EXPECT_TRUE(sent_has("*** kernel panic: a test"), "a panic's text reaches the wire although /dev/ttyS0 was open");
    serdev_close();                                       /* the program's own close afterwards changes nothing */
    s0 = fake_nsent;
    serial_putc('Y');
    drain_all();
    EXPECT_TRUE(fake_nsent == s0 + 1 && serdev_open() == 0, "after the close the console copy is back, and it opens again");
    serdev_close();
    printf(bad ? "FAILED\n" : "all ok\n");
    return bad != 0;
}
