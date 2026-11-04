/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"

/* One tty per virtual console (1..NCONS, /dev/tty1..) and per pseudo-terminal. Line discipline:
 * echo, backspace, ^U, ^C (SIGINT to the tty's foreground: a process group or
 * one process, DESIGN D16), ^Z (SIGTSTP), ^D (end of file); Tab is kept. Reads return one line at a time. Input (serial or
 * Amiga keyboard) always goes to the tty of the console on the screen (CON_INPUT's text to the console it names);
 * output goes to that tty's terminal and, for the console on the screen,
 * is mirrored to the serial port. A process's tty is its controlling tty
 * (struct proc ctty, given at spawn), not whatever its fd 0 is, so
 * `ls | less` on console 2 reads console 2's keys.
 * Ptys (DESIGN D20 7.2, at the end of this file) share the line discipline: ttys PTY_FIRST.. after the consoles.
 * Everything here runs in process context: tty_input is called by kwork
 * (main.c) and by SYS_console's CON_INPUT (DESIGN D24 6.3: raw keys through
 * kbd.c, text through tty_type) in its caller's system call, never by an
 * interrupt (audit B4). Of kwork's four points only tty_write's piece
 * boundary is inside a system call, and these calls never reach it, so the queue, the
 * line and the screen are never changed by two pieces of code at once. */

#define NTTY NTTYS                      /* consoles 1..NCONS, ptys PTY_FIRST.. (DESIGN D20 7.2) */
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
    int leader;                 /* D20 7.2: the session leader (0: none) */
    /* a pty only (D20 7.2): the output ring the master reads, who may open the slave, the size, the open ends */
    u8 *o;                      /* OSZ bytes after the struct (0: a console) */
    u16 ohead, otail;
    u16 ouid;                   /* the owner uid: root and it may open /dev/pts/N */
    u16 rows, cols;
    u8 mopen;                   /* the master is open */
    u8 hup;                     /* the master was closed: the slave is hung up */
    u16 sopen;                  /* open files naming the slave (dups share one) */
    u32 wwait;                  /* slots waiting to write the slave (room in o) */
    u32 mwait;                  /* slots waiting on the master (output to read, room in q) */
};
#define OSZ 2048                /* a pty's output ring: with q and line about 3.6 KB a pty */
static struct tty *tt[NTTY];
int tty_mirror = 1;                     /* copy the console on the screen to the serial port */
static struct tty *dead;                /* a pty number nobody holds: hung up, never console 1's tty (D20 7.2); allocated
                                         * with the first pty, so it exists whenever a pty number can */

static int is_pty(int i) { return i >= PTY_FIRST && i < NTTY; }

static struct tty *get(int i)
{
    if (is_pty(i))
        return tt[i] ? tt[i] : dead;
    if (i < 1 || i > NCONS)
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
    return p->ctty;                     /* always 1..NTTYS-1: proc_load sets it */
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

static u32 o_used(const struct tty *t) { return (u32)((t->ohead - t->otail + OSZ) % OSZ); }
static u32 o_room(const struct tty *t) { return OSZ - 1 - o_used(t); }
static void o_put(struct tty *t, u8 c)
{
    t->o[t->ohead] = c;
    t->ohead = (u16)((t->ohead + 1) % OSZ);
}
static u32 o_dropped;

/* a pty's output: LF as CR LF, as the console draws it (the master's terminal or telnetd gets what the console
 * shows); echo that finds the ring full is dropped and counted (the master is not reading) */
static void pty_echo(struct tty *t, u8 c)
{
    if (o_room(t) < (c == '\n' ? 2u : 1u)) {
        if (o_dropped++ % 64 == 0)
            kprintf("tty: a pty's output ring is full (master not reading), echo dropped (%u so far)\n", o_dropped);
        return;
    }
    if (c == '\n')
        o_put(t, '\r');
    o_put(t, c);
    ev_wake(&t->mwait);
}

static void out(int i, char c)
{
    if (is_pty(i)) {
        if (tt[i])
            pty_echo(tt[i], (u8)c);
        return;
    }
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

/* a pty's slave written: into the output ring (LF as CR LF), waiting while it is full; -EIO once hung up */
static long pty_write(struct tty *t, const u8 *s, u32 n)
{
    u32 done = 0;
    for (;;) {
        if (t->hup)
            return done ? (long)done : -EIO;
        u32 was = done;
        while (done < n && o_room(t) >= (s[done] == '\n' ? 2u : 1u)) {
            if (s[done] == '\n')
                o_put(t, '\r');
            o_put(t, s[done++]);
        }
        if (done != was)
            ev_wake(&t->mwait);
        if (done == n)
            return (long)n;
        if (current->killed)
            return done ? (long)done : -EINTR;
        ev_arm(&t->wwait);
        if (o_room(t) >= 2 || t->hup)
            continue;
        long e = ev_block(EV_IO, 0, 0, !done);
        if (e)
            return done ? (long)done : e;
    }
}

long tty_write(int i, const void *buf, u32 n)
{
    if (is_pty(i))
        return pty_write(get(i), buf, n);
    const char *s = buf;
    u32 done = 0;
    while (done < n) {
        u32 k = n - done > TTY_WPIECE ? TTY_WPIECE : n - done;
        k = term_write_some(i, s + done, k, n - done);  /* the screen: cursor handled once per piece; never inside a
                                                         * UTF-8 character (D28 4) */
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

/* DESIGN D28 10: the line's last character, as the console decoded it (Linux's IUTF8, always on outside 8-bit mode):
 * up to 3 bytes back to a lead whose length agrees and whose bytes form a character, else one byte (the Latin-1
 * fallback's one-byte cells). Removed, and `\b \b` once per column it took (a zero-width one took none). */
static void erase_char(int i, struct tty *t)
{
    u32 n = 1, w = 1;
    if (!term_eight(i) && t->line[t->llen - 1] >= 0x80) {
        u32 k = 1;
        while (k < 4 && k < t->llen && (t->line[t->llen - k] & 0xC0) == 0x80)
            k++;
        const u8 *b = t->line + t->llen - k;
        u32 len = b[0] >= 0xC2 && b[0] <= 0xDF ? 2 : b[0] >= 0xE0 && b[0] <= 0xEF ? 3 : b[0] >= 0xF0 && b[0] <= 0xF4 ? 4 : 0;
        if (len == k && k >= 2) {
            u8 lo = b[0] == 0xE0 ? 0xA0 : b[0] == 0xF0 ? 0x90 : 0x80;
            u8 hi = b[0] == 0xED ? 0x9F : b[0] == 0xF4 ? 0x8F : 0xBF;
            if (b[1] >= lo && b[1] <= hi) {             /* the later bytes are continuations: the scan says so */
                u32 cp = k == 2 ? (u32)(b[0] & 0x1F) << 6 | (b[1] & 0x3F) :
                         k == 3 ? (u32)(b[0] & 0x0F) << 12 | (u32)(b[1] & 0x3F) << 6 | (b[2] & 0x3F) :
                         (u32)(b[0] & 0x07) << 18 | (u32)(b[1] & 0x3F) << 12 | (u32)(b[2] & 0x3F) << 6 | (b[3] & 0x3F);
                int cw = con_cwidth(cp);
                n = k;
                w = cp < 0xA0 ? 0 : cw < 0 ? 0 : (u32)cw;      /* C1 and zero-width ones were never drawn */
            }
        }
    }
    t->llen = (u16)(t->llen - n);
    while (w--)
        erase1(i);
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
    if (c == 8 || c == 127) {               /* backspace: one character (D28 10) */
        if (t->llen)
            erase_char(i, t);
        return;
    }
    if (c == 21) {                          /* ^U */
        while (t->llen)
            erase_char(i, t);
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
 * Ctrl-C), without its 0x1d hook. Stops before a byte the queue could not keep (need_of) -> the bytes taken; -ENXIO
 * when the console's tty was never used (nothing allocated) */
static u32 q_room(const struct tty *t) { return QSZ - 1 - (u32)((t->qhead - t->qtail + QSZ) % QSZ); }

/* the bytes of the input queue typing c would commit (a cooked line needs room for the whole line and its newline;
 * ^C, ^Z, ^U and backspace commit nothing, so they always get through) */
static u32 need_of(const struct tty *t, u8 c)
{
    if (t->mode & TTY_RAW)                      /* what commits: all but a signalling ^C and a refused long line */
        return (c == 3 && !(t->mode & TTY_NOSIG)) || (t->over && c != 3) ? 0 : 1;
    if (c == '\r' || c == '\n')
        return t->over ? 0 : t->llen + 1u;
    if (c == 4)
        return t->over ? 0 : t->llen;
    return (c >= 32 || c == '\t') && !t->over ? t->llen + 2u : 0;
}

int tty_type(int con, const u8 *b, int n)
{
    struct tty *t = con >= 1 && con <= NCONS ? tt[con] : 0;
    int i;
    if (!t)
        return -ENXIO;
    for (i = 0; i < n; i++) {
        if (q_room(t) < need_of(t, b[i]))
            break;
        input(con, b[i]);
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
    if (t == dead)
        return POLLIN | POLLHUP;
    ev_arm(&t->wait);
    u16 r = have_line(t) || t->eof_pending ? POLLIN : 0;
    if (!t->o)
        return (u16)(r | POLLOUT);
    ev_arm(&t->wwait);                      /* a pty's slave: writable while its output ring has room */
    if (t->hup)
        return (u16)(r | POLLIN | POLLHUP);
    return (u16)(r | (o_room(t) >= 2 ? POLLOUT : 0));
}

long tty_read(int i, void *buf, u32 n)
{
    struct tty *t = get(i);
    u8 *d = buf;
    for (;;) {
        if (current->killed)
            return -1;
        if (t == dead)
            return 0;
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
            ev_wake(&t->mwait);             /* a pty: room for its master's writes */
            return (long)got;
        }
        if (t->eof_pending) {
            t->eof_pending = 0;
            return 0;
        }
        if (t->hup)
            return 0;                       /* a hung-up pty: end of file (D20 7.2) */
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

/* ---- DESIGN D20 7.2: pseudo-terminals. The line discipline above is the consoles'; a pty's output goes into its ring
 * (out, pty_write) and its input comes from the master's writes (ptm_write -> input). Everything runs in the system
 * calls of the processes holding the two ends (no interrupt touches a pty), so the kernel's no-preemption rule keeps
 * every check-then-block here whole, as in tty_read. ---- */

static int ctty_users(int i)
{
    for (int k = 1; k < NPROC; k++)
        if (procs[k].state != P_FREE && procs[k].state != P_ZOMBIE && procs[k].ctty == i)
            return 1;
    return 0;
}

/* /dev/ptmx: the lowest free number. A number is free only when its master and every slave descriptor are closed and
 * no living process names it as its controlling tty (tty_gc), so a lingering process of a hung-up session never
 * reads the next session's input or gets its signals */
long pty_alloc(void)
{
    if (!dead) {
        dead = kmalloc(sizeof *dead);
        if (!dead)
            return -ENOMEM;
        memset(dead, 0, sizeof *dead);
        dead->hup = 1;
    }
    for (int i = PTY_FIRST; i < NTTY; i++)
        if (!tt[i]) {
            struct tty *t = kmalloc(sizeof *t + OSZ);
            if (!t)
                return -ENOMEM;
            memset(t, 0, sizeof *t);
            t->o = (u8 *)(t + 1);
            t->ouid = current->uid;
            t->rows = PTY_ROWS;
            t->cols = PTY_COLS;
            tt[i] = t;
            return i;
        }
    return -EAGAIN;                         /* posix_openpt's answer when every pty is in use */
}

/* /dev/pts/n: an existing pty whose master is open; root and its owner uid only */
long pty_slave_ok(int n)
{
    int i = PTY_FIRST + n;
    if (n < 0 || n >= NPTY || !tt[i])
        return -ENOENT;
    if (tt[i]->hup || !tt[i]->mopen)
        return -EIO;
    if (current->uid != 0 && current->uid != tt[i]->ouid)
        return -EACCES;
    return i;
}

void tty_opened(int type, int i)
{
    if (!is_pty(i) || !tt[i])
        return;
    if (type == FT_PTM)
        tt[i]->mopen = 1;
    else
        tt[i]->sopen++;
}

void tty_gc(int i)
{
    struct tty *t = is_pty(i) ? tt[i] : 0;
    if (!t || t->mopen || t->sopen || ctty_users(i))
        return;
    tt[i] = 0;
    kfree(t);
}

/* the master's last close: the slave hangs up. SIGHUP then SIGCONT to every process whose controlling tty it is (its
 * groups' members on this tty: a stopped job never waits for ever with SIGHUP pending; a member of such a group on
 * another terminal is not hung up, so the program that holds the master never signals itself); reads on the slave
 * give 0, writes -EIO */
static void hangup(int i, struct tty *t)
{
    t->hup = 1;
    t->mopen = 0;
    ev_wake(&t->wait);
    ev_wake(&t->wwait);
    for (int k = 1; k < NPROC; k++) {
        struct proc *p = &procs[k];
        if (p->state != P_FREE && p->state != P_ZOMBIE && p->ctty == i) {
            sig_send(p, SIGHUP);
            sig_send(p, SIGCONT);
        }
    }
}

void tty_closed(int type, int i)
{
    struct tty *t = is_pty(i) ? tt[i] : 0;
    if (!t)
        return;
    if (type == FT_PTM)
        hangup(i, t);
    else if (t->sopen && !--t->sopen)
        ev_wake(&t->mwait);                 /* the master sees the end: -EIO, POLLHUP */
    tty_gc(i);
}

/* the master read: what the slave wrote; -EIO when no slave is open and nothing is left (Linux; how telnetd learns
 * the session ended) */
long ptm_read(int i, void *buf, u32 n, int nonblock)
{
    struct tty *t = get(i);
    u8 *d = buf;
    for (;;) {
        if (current->killed)
            return -EINTR;
        if (t == dead)
            return -EIO;
        ev_arm(&t->mwait);
        if (t->ohead != t->otail) {
            u32 got = 0;
            while (got < n && t->otail != t->ohead) {
                d[got++] = t->o[t->otail];
                t->otail = (u16)((t->otail + 1) % OSZ);
            }
            ev_wake(&t->wwait);
            return (long)got;
        }
        if (!t->sopen)
            return -EIO;
        if (nonblock)
            return -EAGAIN;
        long e = ev_block(EV_IO, 0, 0, 1);
        if (e)
            return e;
    }
}

/* the master written: typed into the slave's line discipline (echo, ^C to its foreground, cooked lines) as the
 * keyboard types into a console's; waits while the input queue has no room for the next byte (need_of) */
long ptm_write(int i, const void *buf, u32 n, int nonblock)
{
    struct tty *t = get(i);
    const u8 *s = buf;
    u32 done = 0;
    for (;;) {
        if (t == dead)
            return -EIO;
        while (done < n && q_room(t) >= need_of(t, s[done]))
            input(i, s[done++]);
        if (done == n)
            return (long)n;
        if (!t->sopen && !ctty_users(i))
            return done ? (long)done : -EIO;    /* the session ended: nobody will read (Linux's master write) */
        if (nonblock)
            return done ? (long)done : -EAGAIN;
        if (current->killed)
            return done ? (long)done : -EINTR;
        ev_arm(&t->mwait);
        if (q_room(t) >= need_of(t, s[done]))
            continue;
        long e = ev_block(EV_IO, 0, 0, !done);
        if (e)
            return done ? (long)done : e;
    }
}

u16 ptm_poll(int i)
{
    struct tty *t = get(i);
    if (t == dead)
        return POLLHUP;
    ev_arm(&t->mwait);
    u16 r = 0;
    if (t->ohead != t->otail)
        r |= POLLIN;
    if (!t->sopen)
        r |= POLLHUP;
    if (q_room(t) >= need_of(t, 'a'))
        r |= POLLOUT;                       /* room for a printable byte, the most a byte needs: a write would not wait */
    return r;
}

int tty_size(int i)
{
    if (!is_pty(i))
        return term_size();
    struct tty *t = get(i);
    return t->rows << 16 | t->cols;
}

/* SYS_ttyctl: the descriptor's terminal (a console, a pty's slave or master) */
long tty_ctl(struct file *f, u32 op, u32 arg)
{
    if (f->type != FT_TTY && f->type != FT_PTM)
        return -ENOTTY;
    int i = (int)f->off;
    struct tty *t = get(i);
    switch (op) {
    case TTYC_GWINSZ: {
        if (!user_ok_wr(current, arg, sizeof(struct uwinsize)))
            return -EFAULT;
        struct uwinsize *w = (struct uwinsize *)arg;
        int sz = tty_size(i);
        w->row = (u16)(sz >> 16);
        w->col = (u16)sz;
        w->xpixel = w->ypixel = 0;
        return 0;
    }
    case TTYC_SWINSZ: {
        if (!is_pty(i))
            return -EINVAL;                 /* a console's size is the screen's */
        if (!user_ok_rd(current, arg, sizeof(struct uwinsize)))
            return -EFAULT;
        const struct uwinsize *w = (const struct uwinsize *)arg;
        if (!w->row || !w->col || w->row > 0x7FFF || w->col > 0x7FFF)
            return -EINVAL;
        if (t == dead)
            return -EIO;
        if (w->row != t->rows || w->col != t->cols) {
            t->rows = w->row;
            t->cols = w->col;
            fg_signal(t, SIGWINCH);
        }
        return 0;
    }
    case TTYC_OWNER:
        if (current->uid != 0)
            return -EPERM;
        if (!is_pty(i))
            return -ENOTTY;                 /* a console has no owner to set (login asks on every terminal) */
        if (t == dead || arg > 0xFFFF)
            return t == dead ? -EIO : -EINVAL;
        t->ouid = (u16)arg;
        return 0;
    case TTYC_SCTTY: {
        if (arg || f->type != FT_TTY)
            return -EINVAL;
        if (!is_pty(i) && current->uid != 0)
            return -EPERM;                  /* a console: root's (a descriptor inherited is not a session) */
        if (t == dead || t->hup)
            return -EIO;
        struct proc *l = t->leader ? proc_by_pid(t->leader) : 0;
        if (l && l != current && l->state != P_ZOMBIE)
            return -EPERM;
        int old = current->ctty;
        if (old != i && tt[old] && tt[old]->leader == current->pid)
            tt[old]->leader = 0;            /* it leads its old terminal no longer */
        current->ctty = (u8)i;
        t->leader = current->pid;
        if (old != i)
            tty_gc(old);                    /* a pty nobody names any more is freed */
        return 0;
    }
    }
    return -EINVAL;
}

/* the session leader (D20 7.2): the first process spawned onto a tty from another one (login on a pty) */
void tty_spawned(struct proc *p, int parent_ctty)
{
    if (p->ctty == parent_ctty)
        return;
    struct tty *t = get(p->ctty);
    if (t == dead)
        return;
    struct proc *l = t->leader ? proc_by_pid(t->leader) : 0;
    if (!l || l->state == P_ZOMBIE)
        t->leader = p->pid;
}

void tty_lead_move(int from, int to)
{
    for (int i = 1; i < NTTY; i++)
        if (tt[i] && tt[i]->leader == from)
            tt[i]->leader = to;
}

void tty_proc_gone(struct proc *p)
{
    int i = p->ctty;
    if (i >= 1 && i < NTTY && tt[i] && tt[i]->leader == p->pid)
        tt[i]->leader = 0;
    tty_gc(i);
}
