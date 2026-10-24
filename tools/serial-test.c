/* SPDX-License-Identifier: GPL-2.0-only */
/* Host test of the serial copy of the console (kernel/serial.c) against a model of Paula's port (serial-fake.h):
 * with nothing received since boot a full send ring drops console bytes (counted, logged) instead of waiting; the
 * kernel's own output always waits; after the first received byte the console copy waits and loses nothing.
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
    printf(bad ? "FAILED\n" : "all ok\n");
    return bad != 0;
}
