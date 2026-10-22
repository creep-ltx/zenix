#include "kernel.h"

/* One tty per workspace. Line discipline: echo, backspace, ^U, ^C
 * (kills the tty's foreground process), ^D (end of file). Reads return
 * one line at a time. Input (serial or Amiga keyboard) always goes to
 * the focused workspace's tty; output goes to that tty's terminal and,
 * for the focused one, is mirrored to the serial port. */

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

long tty_write(int i, const void *buf, u32 n)
{
    const char *s = buf;
    term_write(i, s, n);                    /* the screen: cursor handled once per batch */
    if (i == ws_cur && tty_mirror)
        for (u32 k = 0; k < n; k++)
            serial_putc(s[k]);
    return (long)n;
}

static void commit(struct tty *t, u8 c)
{
    t->q[t->qhead] = c;
    t->qhead = (u16)((t->qhead + 1) % QSZ);
    if (t->qhead == t->qtail)
        t->qtail = (u16)((t->qtail + 1) % QSZ);     /* overflow: drop oldest */
}

static void erase1(int i)
{
    out(i, '\b'); out(i, ' '); out(i, '\b');
}

/* from the serial receive interrupt or the keyboard */
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
        block_io();
    }
}
