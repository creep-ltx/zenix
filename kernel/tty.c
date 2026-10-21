#include "kernel.h"

/* Serial console with a line discipline: echo, backspace, ^U, ^C
 * (kills the foreground process), ^D (end of file). Reads return one
 * line at a time. */

#define QSZ 1024
#define LSZ 256
static u8 q[QSZ];
static u16 qhead, qtail;
static u8 line[LSZ];
static u16 llen;
static int eof_pending;
int tty_fg;

void tty_init(void)
{
    qhead = qtail = llen = 0;
    eof_pending = 0;
    tty_fg = 0;
}

static void commit(u8 c)
{
    q[qhead] = c;
    qhead = (u16)((qhead + 1) % QSZ);
    if (qhead == qtail)
        qtail = (u16)((qtail + 1) % QSZ);   /* overflow: drop oldest */
}

/* called from the serial receive interrupt */
void tty_input(u8 c)
{
    if (c == '\r')
        c = '\n';
    if (c == 3) {                           /* ^C */
        serial_putc('^');
        serial_putc('C');
        serial_putc('\n');
        llen = 0;
        struct proc *p = tty_fg ? proc_by_pid(tty_fg) : 0;
        if (p)
            proc_kill(p);
        wake_io();
        return;
    }
    if (c == 4) {                           /* ^D */
        if (llen == 0)
            eof_pending = 1;
        else {
            for (u16 i = 0; i < llen; i++)
                commit(line[i]);
            llen = 0;
        }
        wake_io();
        return;
    }
    if (c == 8 || c == 127) {               /* backspace */
        if (llen) {
            llen--;
            serial_putc('\b'); serial_putc(' '); serial_putc('\b');
        }
        return;
    }
    if (c == 21) {                          /* ^U */
        while (llen) {
            llen--;
            serial_putc('\b'); serial_putc(' '); serial_putc('\b');
        }
        return;
    }
    if (c == '\n') {
        serial_putc('\n');
        for (u16 i = 0; i < llen; i++)
            commit(line[i]);
        commit('\n');
        llen = 0;
        wake_io();
        return;
    }
    if (c >= 32 && c < 127 && llen < LSZ - 1) {
        line[llen++] = c;
        serial_putc((char)c);
    }
}

static int have_line(void)
{
    for (u16 i = qtail; i != qhead; i = (u16)((i + 1) % QSZ))
        if (q[i] == '\n')
            return 1;
    return 0;
}

long tty_read(void *buf, u32 n)
{
    u8 *d = buf;
    for (;;) {
        if (current->killed)
            return -1;
        if (have_line()) {
            u32 got = 0;
            while (got < n && qtail != qhead) {
                u8 c = q[qtail];
                qtail = (u16)((qtail + 1) % QSZ);
                d[got++] = c;
                if (c == '\n')
                    break;
            }
            return (long)got;
        }
        if (eof_pending) {
            eof_pending = 0;
            return 0;
        }
        block_io();
    }
}
