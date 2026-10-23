#include "kernel.h"

/* One tty per workspace. Line discipline: echo, backspace, ^U, ^C
 * (kills the tty's foreground process), ^D (end of file). Reads return
 * one line at a time. Input (serial or Amiga keyboard) always goes to
 * the focused workspace's tty; output goes to that tty's terminal and,
 * for the focused one, is mirrored to the serial port.
 * Everything here runs in process context: tty_input is called by kwork
 * (main.c), never by an interrupt (audit B4), so the queue, the line and
 * the screen are never changed by two pieces of code at once. */

#define NTTY 10
#define QSZ 1024
#define LSZ 256

struct tty {
    u8 q[QSZ];
    u16 qhead, qtail;
    u8 line[LSZ];
    u16 llen;
    int eof_pending;
    int fg;
    int mode;                   /* TTY_RAW, TTY_NOSIG */
    int intr;                   /* ^C was typed with no foreground process: the shell polls this */
    int owner;                  /* pid that set the mode */
};
static struct tty *tt[NTTY];
int tty_mirror = 1;                     /* copy the focused screen to the serial port */

static struct tty *get(int i)
{
    if (i < 1 || i >= NTTY)
        i = 1;
    if (!tt[i]) {
        tt[i] = kmalloc(sizeof *tt[i]);
        if (!tt[i])
            panic("tty: out of memory");
        memset(tt[i], 0, sizeof *tt[i]);
    }
    return tt[i];
}

void tty_init(void) { }

int tty_of(const struct proc *p)
{
    return (p->fd[0] && p->fd[0]->type == FT_TTY) ? (int)p->fd[0]->off : 1;
}

void tty_setmode(int tty, int mode, int owner)
{
    struct tty *t = get(tty);
    t->mode = mode;
    t->owner = mode ? owner : 0;
    t->llen = 0;
}

int tty_getmode(int tty) { return get(tty)->mode; }

int tty_take_intr(int tty)
{
    struct tty *t = get(tty);
    int v = t->intr;
    t->intr = 0;
    return v;
}

void tty_release(int pid)
{
    for (int i = 1; i < NTTY; i++)
        if (tt[i] && tt[i]->owner == pid) {
            tt[i]->mode = 0;
            tt[i]->owner = 0;
        }
}

static void commit(struct tty *t, u8 c);

void tty_inject(int tty, const char *s)
{
    struct tty *t = get(tty);
    while (*s)
        commit(t, (u8)*s++);
    wake_io();
}

void tty_setfg(int tty, int pid) { get(tty)->fg = pid; }
int tty_getfg(int tty) { return get(tty)->fg; }

static void out(int i, char c)
{
    term_putc(i, c);
    if (i == ws_cur && tty_mirror)
        serial_putc(c);
}

/* Drawing a big write takes long on a slow machine (seconds for 4 KB on a 68020 with chip RAM only), and input is
 * only taken by kwork: so a write is drawn in pieces, and between two pieces - the terminal is in a consistent state
 * there, and no check-then-block is pending - queued input is handled (echo, Ctrl-C: a killed writer stops at once).
 * This keeps the input rings from overflowing while a program prints a lot. */
#define TTY_WPIECE 256

long tty_write(int i, const void *buf, u32 n)
{
    const char *s = buf;
    u32 done = 0;
    while (done < n) {
        u32 k = n - done > TTY_WPIECE ? TTY_WPIECE : n - done;
        term_write(i, s + done, k);         /* the screen: cursor handled once per piece */
        if (i == ws_cur && tty_mirror)
            for (u32 j = 0; j < k; j++)
                serial_putc(s[done + j]);
        done += k;
        if (done < n && kwork_pending) {
            kwork();
            if (current->killed)
                break;
        }
    }
    return (long)done;
}

static u32 q_dropped;

static void commit(struct tty *t, u8 c)
{
    t->q[t->qhead] = c;
    t->qhead = (u16)((t->qhead + 1) % QSZ);
    if (t->qhead == t->qtail) {                     /* full: the oldest byte goes, and the log says so once per 64 */
        t->qtail = (u16)((t->qtail + 1) % QSZ);
        if (q_dropped++ % 64 == 0)
            kprintf("tty: input queue full (nobody reading?), oldest bytes dropped (%u so far)\n", q_dropped);
    }
}

static void erase1(int i)
{
    out(i, '\b'); out(i, ' '); out(i, '\b');
}

/* one typed byte, from kwork (serial_work or kbd_work): process context, never an interrupt */
void tty_input(u8 c)
{
    int i = ws_cur;
    struct tty *t = get(i);
    if (t->mode & TTY_RAW) {                /* the program does its own editing */
        if (c == 3 && !(t->mode & TTY_NOSIG)) {
            struct proc *p = t->fg ? proc_by_pid(t->fg) : 0;
            if (p)
                proc_kill(p);
            else
                t->intr = 1;
        } else {
            commit(t, c);
        }
        wake_io();
        return;
    }
    if (c == '\r')
        c = '\n';
    if (c == 3) {                           /* ^C */
        out(i, '^'); out(i, 'C'); out(i, '\n');
        t->llen = 0;
        struct proc *p = t->fg ? proc_by_pid(t->fg) : 0;
        if (p)
            proc_kill(p);
        else
            t->intr = 1;                    /* the shell itself is running something: it polls */
        wake_io();
        return;
    }
    if (c == 26) {                          /* ^Z: stop the foreground program (the shell offers it as a job) */
        struct proc *p = t->fg ? proc_by_pid(t->fg) : 0;
        if (p) {
            out(i, '^'); out(i, 'Z'); out(i, '\n');
            t->llen = 0;
            proc_stop(p);
        }
        wake_io();
        return;
    }
    if (c == 4) {                           /* ^D */
        if (t->llen == 0)
            t->eof_pending = 1;
        else {
            for (u16 k = 0; k < t->llen; k++)
                commit(t, t->line[k]);
            t->llen = 0;
        }
        wake_io();
        return;
    }
    if (c == 8 || c == 127) {               /* backspace */
        if (t->llen) {
            t->llen--;
            erase1(i);
        }
        return;
    }
    if (c == 21) {                          /* ^U */
        while (t->llen) {
            t->llen--;
            erase1(i);
        }
        return;
    }
    if (c == '\n') {
        out(i, '\n');
        for (u16 k = 0; k < t->llen; k++)
            commit(t, t->line[k]);
        commit(t, '\n');
        t->llen = 0;
        wake_io();
        return;
    }
    if (c >= 32 && t->llen < LSZ - 1) {
        t->line[t->llen++] = c;
        out(i, (char)c);
    }
}

static int have_line(struct tty *t)
{
    if (t->mode & TTY_RAW)
        return t->qtail != t->qhead;        /* any byte will do */
    for (u16 k = t->qtail; k != t->qhead; k = (u16)((k + 1) % QSZ))
        if (t->q[k] == '\n')
            return 1;
    return 0;
}

long tty_read(int i, void *buf, u32 n)
{
    struct tty *t = get(i);
    u8 *d = buf;
    for (;;) {
        if (current->killed)
            return -1;
        if (have_line(t)) {
            u32 got = 0;
            while (got < n && t->qtail != t->qhead) {
                u8 c = t->q[t->qtail];
                t->qtail = (u16)((t->qtail + 1) % QSZ);
                d[got++] = c;
                if (c == '\n')
                    break;
            }
            return (long)got;
        }
        if (t->eof_pending) {
            t->eof_pending = 0;
            return 0;
        }
        /* No lost wakeup between the checks above and block_io (audit B5): input reaches the queue only through
         * kwork, which runs only at the points listed in main.c, none of them between here and block_io, so a
         * byte that arrives now waits in its ring and its wake_io() comes after this process is P_IO. */
        block_io();
    }
}
