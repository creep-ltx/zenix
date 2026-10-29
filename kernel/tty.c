/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"

/* One tty per virtual console (1..NCONS, /dev/tty1..). Line discipline:
 * echo, backspace, ^U, ^C (SIGINT to the tty's foreground: a process group or
 * one process, DESIGN D16), ^Z (SIGTSTP), ^D (end of file); Tab is kept. Reads return one line at a time. Input (serial or
 * Amiga keyboard) always goes to the tty of the console on the screen (CON_INPUT's text to the console it names);
 * output goes to that tty's terminal and, for the console on the screen,
 * is mirrored to the serial port. A process's tty is its controlling tty
 * (struct proc ctty, given at spawn), not whatever its fd 0 is, so
 * `ls | less` on console 2 reads console 2's keys.
 * Everything here runs in process context: tty_input is called by kwork
 * (main.c) and by SYS_console's CON_INPUT (DESIGN D24 6.3: raw keys through
 * kbd.c, text through tty_type) in its caller's system call, never by an
 * interrupt (audit B4). Of kwork's four points only tty_write's piece
 * boundary is inside a system call, and these calls never reach it, so the queue, the
 * line and the screen are never changed by two pieces of code at once. */

#define NTTY (NCONS + 1)
#define QSZ 1024
/* A cooked line holds 511 characters: more than the 478 the shell's line editor holds, so a line typed ahead for the
 * shell (while a command runs, the tty is cooked) is never cut here. A key typed into a full line rings the bell and
 * marks the line too long (`over`); at its end the whole line is refused with a message, never handed on cut (audit
 * 2026-10-26 F2: a 260-character line typed ahead lost its middle, `break` arrived as `ak`). The mark outlives a
 * switch to raw mode: the rest of that line, typed after the shell's editor took over, is refused too, so no tail of
 * it runs as a command. */
#define LSZ 512

struct tty {
    u8 q[QSZ];
    u16 qhead, qtail;
    u8 line[LSZ];
    u16 llen;
    u8 over;                    /* the line being typed was too long: refused at its end */
    int eof_pending;
    int fg;
    u8 fg_grp;                  /* D16 rule 7: fg is a process group (its leader may end first), else one pid */
    int mode;                   /* TTY_RAW, TTY_NOSIG */
    int intr;                   /* ^C was typed with no foreground process: the shell polls this */
    int owner;                  /* pid that set the mode */
    u32 wait;                   /* D16: the slots waiting to read (event.c) */
};
static struct tty *tt[NTTY];
int tty_mirror = 1;                     /* copy the console on the screen to the serial port */

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
    return p->ctty;                     /* always 1..NCONS: proc_load sets it */
}

static void commit(struct tty *t, u8 c);

/* A program switching the tty to raw mode (the shell's line editor after a command) gets what was typed meanwhile:
 * the half-typed cooked line goes into the queue as it is, so nothing typed ahead is lost (as on a Unix tty). */
void tty_setmode(int tty, int mode, int owner)
{
    struct tty *t = get(tty);
    if ((mode & TTY_RAW) && !(t->mode & TTY_RAW) && !t->over) {
        for (u16 k = 0; k < t->llen; k++)
            commit(t, t->line[k]);
        if (t->llen)
            ev_wake(&t->wait);
    }
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

void tty_inject(int tty, const char *s)
{
    struct tty *t = get(tty);
    while (*s)
        commit(t, (u8)*s++);
    ev_wake(&t->wait);
}

void tty_setfg(int tty, int id, int group)
{
    struct tty *t = get(tty);
    t->fg = id;
    t->fg_grp = (u8)(id && group);
}
int tty_fgpid(int tty) { struct tty *t = get(tty); return t->fg_grp ? 0 : t->fg; }
int tty_in_fg(int tty, const struct proc *p)
{
    struct tty *t = get(tty);
    return !t->fg || (t->fg_grp ? p->pgid == t->fg : p->pid == t->fg);
}

/* ^C, ^Z: the signal to the foreground (a group: every member, rule 7). 1 if someone got it; nobody in the foreground
 * (or a group with no living member) and ^C: intr is set, which an old shell polls (SYS_ttyintr) */
static int fg_signal(struct tty *t, int sig)
{
    int sent = 0;
    if (t->fg_grp)
        sent = sig_group(t->fg, sig, 0) == 0;
    else {
        struct proc *p = t->fg ? proc_by_pid(t->fg) : 0;
        if (p && p->state != P_ZOMBIE) {
            sig_send(p, sig);
            sent = 1;
        }
    }
    if (!sent && sig == SIGINT)
        t->intr = 1;
    return sent;
}

static void out(int i, char c)
{
    term_putc(i, c);
    if (i == con_cur && tty_mirror)
        serial_putc(c);
}

/* Drawing a big write takes long on a slow machine (in FS-UAE's stock A1200 a 256-byte piece of short lines took
 * 100-350 ms, nearly all of it the scrolls), and input is only taken by kwork: so a write is drawn in pieces, and
 * between two pieces - the terminal is in a consistent state there, and no check-then-block is pending - queued
 * input is handled (echo, Ctrl-C: a killed writer stops at once), and a process it woke that is better than the
 * writer (or of its level, once the writer's slice is over) runs: a yield point, as at the end of a system call. A
 * piece is at most 256 bytes and ends early after 10-20 ms of drawing when input waits or someone better is ready
 * (term_write_some), so netd, woken by SLIP's bytes, runs within about one piece and the serial port's receive ring
 * does not overflow while a program prints a lot (DESIGN D20 section 6, step 2). */
#define TTY_WPIECE 256

long tty_write(int i, const void *buf, u32 n)
{
    const char *s = buf;
    u32 done = 0;
    while (done < n) {
        u32 k = n - done > TTY_WPIECE ? TTY_WPIECE : n - done;
        k = term_write_some(i, s + done, k);    /* the screen: cursor handled once per piece */
        if (i == con_cur && tty_mirror)
            for (u32 j = 0; j < k; j++)
                serial_putc(s[done + j]);
        done += k;
        if (done < n) {
            if (kwork_pending)
                kwork();
            if (need_resched)
                resched();                  /* the yield point: a better process (netd for SLIP) runs now */
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

/* the end of a line that was too long: nothing of it is handed on, and the console says so */
static void refuse_line(int i, struct tty *t)
{
    static const char msg[] = "tty: the line was too long (more than 511 characters): not taken\n";
    if (t->mode & TTY_RAW)
        out(i, '\n');                      /* raw: the program's own echo did not end the line */
    for (const char *m = msg; *m; m++)
        out(i, *m);
    t->llen = 0;
    t->over = 0;
}

/* one typed byte for console i's tty, from kwork (serial_work or kbd_work: the console on the screen) or CON_INPUT
 * (tty_type): process context, never an interrupt */
static void input(int i, u8 c)
{
    struct tty *t = get(i);
    if (t->mode & TTY_RAW) {                /* the program does its own editing */
        if (t->over && c != 3) {            /* the rest of a line too long, typed ahead: refused up to its end */
            if (c == '\r' || c == '\n')
                refuse_line(i, t);
            else if (c >= 32)
                out(i, '\a');
            return;
        }
        t->over = 0;
        if (c == 3 && !(t->mode & TTY_NOSIG))
            fg_signal(t, SIGINT);
        else
            commit(t, c);
        ev_wake(&t->wait);
        return;
    }
    if (c == '\r')
        c = '\n';
    if (c == 3) {                           /* ^C */
        out(i, '^'); out(i, 'C'); out(i, '\n');
        t->llen = 0;
        t->over = 0;
        fg_signal(t, SIGINT);               /* nobody: intr, for a shell that polls */
        ev_wake(&t->wait);
        return;
    }
    if (c == 26) {                          /* ^Z: stop the foreground (the shell offers it as a job) */
        if (t->fg && (t->fg_grp || proc_by_pid(t->fg))) {
            out(i, '^'); out(i, 'Z'); out(i, '\n');
            t->llen = 0;
            fg_signal(t, SIGTSTP);
        }
        ev_wake(&t->wait);
        return;
    }
    if (c == 4) {                           /* ^D */
        if (t->over) {
            refuse_line(i, t);
            return;
        }
        if (t->llen == 0)
            t->eof_pending = 1;
        else {
            for (u16 k = 0; k < t->llen; k++)
                commit(t, t->line[k]);
            t->llen = 0;
        }
        ev_wake(&t->wait);
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
        t->over = 0;
        return;
    }
    if (c == '\n') {
        out(i, '\n');
        if (t->over) {
            refuse_line(i, t);
            return;
        }
        for (u16 k = 0; k < t->llen; k++)
            commit(t, t->line[k]);
        commit(t, '\n');
        t->llen = 0;
        ev_wake(&t->wait);
        return;
    }
    if (c >= 32 || c == '\t') {
        if (t->llen < LSZ - 1 && !t->over) {
            t->line[t->llen++] = c;
            out(i, (char)c);
        } else {
            out(i, '\a');                   /* the line is full: the bell (on the screen and the serial port) */
            t->over = 1;
        }
    }
}

void tty_input(u8 c) { input(con_cur, c); }

/* CON_INPUT's text (D24 6.3): bytes into console con's tty as the serial line's are typed (the line discipline, echo,
 * Ctrl-C), without its 0x1d hook. Stops before a byte the queue could not keep (a cooked line needs room for the
 * whole line and its newline; ^C, ^Z, ^U and backspace commit nothing, so they always get through) -> the bytes taken; -ENXIO when the console's tty was never used (nothing allocated) */
int tty_type(int con, const u8 *b, int n)
{
    struct tty *t = con >= 1 && con < NTTY ? tt[con] : 0;
    int i;
    if (!t)
        return -ENXIO;
    for (i = 0; i < n; i++) {
        u8 c = b[i];
        u32 room = QSZ - 1 - (u32)((t->qhead - t->qtail + QSZ) % QSZ), need;
        if (t->mode & TTY_RAW)                  /* what commits: all but a signalling ^C and a refused long line */
            need = (c == 3 && !(t->mode & TTY_NOSIG)) || (t->over && c != 3) ? 0 : 1;
        else if (c == '\r' || c == '\n')
            need = t->over ? 0 : t->llen + 1u;
        else if (c == 4)
            need = t->over ? 0 : t->llen;
        else
            need = (c >= 32 || c == '\t') && !t->over ? t->llen + 2u : 0;     /* ^C ^Z ^U backspace: always */
        if (room < need)
            break;
        input(con, c);
    }
    return i;
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

/* poll (D16): a read would not wait; ttys are always writable (the tick empties the send ring: rule 11) */
u16 tty_poll(int i)
{
    struct tty *t = get(i);
    ev_arm(&t->wait);
    return (u16)(POLLOUT | (have_line(t) || t->eof_pending ? POLLIN : 0));
}

long tty_read(int i, void *buf, u32 n)
{
    struct tty *t = get(i);
    u8 *d = buf;
    for (;;) {
        if (current->killed)
            return -1;
        ev_arm(&t->wait);                   /* D16 step 1: register and clear the bit, then check */
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
        /* No lost wakeup between the checks above and the block (audit B5, D16 section 3): input reaches the queue only
         * through kwork, which runs only at the points listed in main.c, none of them between here and ev_block, and
         * through CON_INPUT, another process's system call, which cannot run here either (no preemption); and a
         * wake-up after ev_arm sets the bit, which ev_block tests with interrupts masked. A handler interrupts the read
         * (-EINTR, restarted with SA_RESTART) */
        long e = ev_block(EV_IO, 0, 0, 1);
        if (e)
            return e;
    }
}
