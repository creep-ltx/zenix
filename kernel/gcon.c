/* SPDX-License-Identifier: GPL-2.0-only */
#include "kernel.h"
#include "hw.h"

/* DESIGN D38 section 3.4, step 2: a console in graphics mode, handed to a display server (displayd), as Linux gives a
 * console to X. SYS_console's CON_GRAB (root; the console named by a descriptor) gives the caller a new descriptor:
 * while that console is shown, the Copper runs the caller's list, the kernel draws nothing there (gfx_up() is 0),
 * and every key but the console switches (Amiga+F1..F10), and the mouse, are records in a ring the descriptor reads
 * (struct ugevent; poll says POLLIN). The keymap stays in the kernel: a key's record carries its characters.
 *
 * Switching away waits for the server's yes (Linux's VT_PROCESS): Amiga+Fn on the grabbed console puts GEV_RELEASE
 * (a number) in the ring; the server finishes its blit, stops using the blitter and answers CON_GACK with that
 * number; then the kernel shows the text console. No answer within 1 s: the kernel switches anyway and says so in
 * the log (a server that ignores this is a bug, not a hang). Back again: GEV_ACQUIRE, and the server may use the
 * blitter again; its bitplanes were never touched.
 *
 * The pointer is hardware sprite 0, the server's image in its chip RAM (its Copper list points SPR0PT at it each
 * frame). The 100 Hz tick adds the mouse's movement to the pointer's place and latches the buttons' changes; the
 * vertical-blank interrupt writes the image's two control words when the place changed (before sprite DMA reads
 * them, once a frame), so the pointer never freezes while the server or a disk transfer is busy: two words of
 * position, no drawing (the two reviews of 2026-11-04 accepted this in an interrupt, in the vertical blank). Moves
 * and buttons become GEV_POINTER records in kwork (gcon_work), merged while unread; a press and a release between
 * two runs of kwork are both told. The grab ends at the descriptor's last close and when the server ends (its chip
 * RAM goes with it; SYS_memfree refuses the regions the grab uses): the kernel's Copper list, sprites off, the text
 * drawn again.
 *
 * A server that does not answer within 1 s may still be writing the blitter's registers (a user process can be
 * preempted half-way through a blit's set-up), so after a forced switch the kernel draws with the CPU only until the
 * server answers that release (late, CON_GACK still takes it) or ends.
 *
 * Process context: the key path (kbd_work through kwork, CON_INPUT), gcon_work (kwork), con_switch and the system
 * calls; the kernel is not preempted, so the ring has one writer at a time. Interrupts: the tick writes px, py and
 * the button bits, the vertical blank the sprite's control words; process context writes those words only with
 * interrupts masked. */

#define GQN 64                  /* records in the ring (1 KB) */
#define GQ_CTL 2                /* slots kept for GEV_RELEASE and GEV_ACQUIRE: input never takes them */
#define G_AWAY 0                /* another console is shown */
#define G_SHOWN 1               /* the grabbed console is shown: the server has the screen and the blitter */
#define G_RELEASING 2           /* asked to let go (GEV_RELEASE), until CON_GACK or the deadline */
#define G_HZ 100

struct gcon {
    struct file *file;          /* the descriptor CON_GRAB gave */
    int pid;
    u8 con, state, target;
    u16 seq;                    /* the last GEV_RELEASE's number */
    u32 deadline;               /* G_RELEASING: the tick at which the switch happens anyway */
    u32 cop;                    /* the server's Copper list */
    volatile u16 *spr;          /* its sprite 0: the two control words */
    u16 sprh;
    short hx, hy;
    u16 w, h;
    volatile short px, py;        /* the pointer (the tick) */
    volatile u16 btn;           /* its buttons, as GQ_*BUTTON bits (the tick) */
    volatile u16 bdown, bup;    /* buttons pressed, released since kwork last looked (the tick latches them) */
    volatile u8 moved;          /* the place changed: the vertical blank writes the control words */
    u8 forced;                  /* switched away without the server's answer: the kernel draws with the CPU */
    short lx, ly;                 /* what the ring was last told */
    u16 lbtn;
    u16 lost;                   /* input records dropped (a full ring): GEV_LOST says how many */
    u16 head, tail;
    u32 wait;                   /* the slots polling or reading the descriptor */
    struct ugevent q[GQN];
};
static struct gcon *gc;
static u8 redraw_to;            /* a close with interrupts off (proc_exit): kwork shows this console as text */

/* A pointer must lie in chip RAM (the Copper and the sprite DMA reach only that): one of the bootinfo's chip regions */
static int chip_ok(u32 a, u32 n)
{
    const struct bootinfo *bi = (const struct bootinfo *)0x1000;
    for (int i = 0; i < bi->nregions && i < 8; i++)
        if ((bi->r[i].attrs & MEMF_CHIP) && a >= (bi->r[i].lower & ~0x3Fu) && a + n <= bi->r[i].upper && a + n > a)
            return 1;
    return 0;
}

/* the sprite's two control words for the pointer at (px, py): hires pixels and lines, the hot spot taken off.
 * Sprites are placed in lores pixels: HSTART 0x80 is the first playfield pixel's column (DIWSTRT's 0x81) */
static void sprite_pos(struct gcon *g)
{
    s32 x = (g->px - g->hx) / 2 + 0x80, y = g->py - g->hy + gfx_diwv(), e = y + g->sprh;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (e > 511) e = 511;               /* VSTOP has 9 bits: never wrapped above the image */
    g->spr[0] = (u16)((y & 0xFF) << 8 | (x >> 1 & 0xFF));
    g->spr[1] = (u16)((e & 0xFF) << 8 | (y >> 8 & 1) << 2 | (e >> 8 & 1) << 1 | (x & 1));
}

static u32 used(const struct gcon *g) { return (u32)((g->head - g->tail + GQN) % GQN); }

/* a record into the ring (process context). Input (ctl 0) leaves GQ_CTL slots free and counts what it drops; a
 * control record always goes in (at worst over the oldest record, counted as lost) */
static void push(struct gcon *g, u8 type, u8 code, u16 qual, short x, short y, const u8 *ch, int ctl)
{
    if (!ctl && used(g) >= GQN - 1 - GQ_CTL) {
        g->lost++;
        return;
    }
    if (g->lost && used(g) < GQN - 1 - GQ_CTL) {
        struct ugevent *l = &g->q[g->head];
        memset(l, 0, sizeof *l);
        l->type = GEV_LOST;
        l->x = (short)g->lost;
        l->time = ticks;
        g->head = (u16)((g->head + 1) % GQN);
        g->lost = 0;
    }
    if (used(g) >= GQN - 1) {
        g->tail = (u16)((g->tail + 1) % GQN);
        g->lost++;
    }
    struct ugevent *e = &g->q[g->head];
    e->type = type;
    e->code = code;
    e->qual = qual;
    e->x = x;
    e->y = y;
    for (int i = 0; i < 4; i++)
        e->ch[i] = ch ? ch[i] : 0;
    e->time = ticks;
    g->head = (u16)((g->head + 1) % GQN);
    ev_wake(&g->wait);
}

int gcon_active(void) { return gc && gc->state != G_AWAY; }
int gcon_grabbed(void) { return gc != 0; }

/* the grabbed console is shown now (con_switch, or the grab itself): the screen is the server's -> 1 */
int gcon_shown(int con)
{
    struct gcon *g = gc;
    if (!g || g->con != con)
        return 0;
    g->state = G_SHOWN;
    u16 s = irq_save();
    sprite_pos(g);
    irq_restore(s);
    kbd_forget();                       /* no key held in text mode repeats into the ring */
    gfx_hand(g->cop);
    push(g, GEV_ACQUIRE, 0, kbd_qual() | g->btn, g->px, g->py, 0, 1);
    return 1;
}

/* con_switch to console `to` while the grabbed one is shown: ask the server to let go -> 1 (the switch waits for
 * CON_GACK or the deadline), 0 when the grabbed console is not the one shown */
int gcon_away(int to)
{
    struct gcon *g = gc;
    if (!g || g->con != con_cur || g->state == G_AWAY)
        return 0;
    g->target = (u8)to;
    if (g->state == G_RELEASING)
        return 1;                       /* a second Amiga+Fn while it is asked: only the target changes */
    g->state = G_RELEASING;
    g->seq++;
    g->deadline = ticks + G_HZ;
    push(g, GEV_RELEASE, 0, kbd_qual() | g->btn, (short)g->seq, 0, 0, 1);
    return 1;
}

/* the server let go (or the deadline passed): the kernel's screen, then the console asked for */
static void let_go(struct gcon *g)
{
    g->state = G_AWAY;
    kbd_forget();                       /* no key held in the desktop repeats into a text console */
    gfx_hand(0);
    con_show(g->target);
}

/* SYS_console CON_GRAB: console `con` (term_conop checked the descriptor), a2 the caller's struct ucongrab */
long gcon_grab(int con, u32 a2)
{
    if (current->uid != 0)
        return -EPERM;                  /* the Copper list can write the chips' registers: root's, as the kernel's */
    if (!gfx_up() && !gc)
        return -ENODEV;
    if (gc)
        return -EBUSY;                  /* one display server at a time */
    if (!user_ok_wr(current, a2, UCONGRAB_FIRST))
        return -EFAULT;
    struct ucongrab *u = (struct ucongrab *)a2;
    if (u->size < UCONGRAB_FIRST || u->sprh < 1 || u->sprh > 255 || (u->cop & 1) || (u->sprite & 1))
        return -EINVAL;
    u32 sbytes = 4u * (u->sprh + 2u);
    if (!user_ok_wr(current, u->sprite, sbytes) || !chip_ok(u->sprite, sbytes))
        return -EFAULT;
    int topaz = u->size >= UCONGRAB_TOPAZ && user_ok_rd(current, a2, UCONGRAB_TOPAZ) && u->topaz;
    if (topaz && !user_ok_wr(current, u->topaz, 224 * 8))
        return -EFAULT;
    /* the Copper list: the caller's chip RAM up to its end (0xFFFF 0xFFFE), at most 4,096 instructions */
    u32 n = 0;
    for (;; n++) {
        u32 a = u->cop + 4 * n;
        if (n >= 4096 || !user_ok_rd(current, a, 4) || !chip_ok(a, 4))
            return n >= 4096 ? -EINVAL : -EFAULT;
        const u16 *w = (const u16 *)a;
        if (w[0] == 0xFFFF && w[1] == 0xFFFE)
            break;
    }
    struct gcon *g = kmalloc(sizeof *g);
    struct file *f = g ? file_new(FT_GCON) : 0;
    if (!f) {
        kfree(g);
        return g ? -ENFILE : -ENOMEM;
    }
    memset(g, 0, sizeof *g);
    int fd = fd_install(current, f);
    if (fd < 0) {
        f->type = FT_NONE;
        kfree(g);
        return fd;
    }
    g->file = f;
    g->pid = current->pid;
    g->con = (u8)con;
    g->target = (u8)con;
    g->cop = u->cop;
    g->spr = (volatile u16 *)u->sprite;
    g->sprh = u->sprh;
    g->hx = u->hotx;
    g->hy = u->hoty;
    gfx_geom(&g->w, &g->h);
    g->px = g->lx = (short)(g->w / 2);
    g->py = g->ly = (short)(g->h / 2);
    u->width = g->w;
    u->height = g->h;
    u->depth = (u16)gfx_depth;
    u->chips = gfx_aga() ? GCHIP_AGA : 0;
    if (topaz)
        gfx_topaz((u8 *)u->topaz);      /* step 3: the server draws the console's font, never shipped (D38 5) */
    f->flags = O_RDWR;
    u16 s = irq_save();
    gc = g;
    irq_restore(s);
    kprintf("display: console %d handed to pid %d (%s)\n", con, current->pid, current->name);
    if (con == con_cur)
        gcon_shown(con);
    return fd;
}

/* SYS_console CON_GACK on the grab's descriptor: the server let go of GEV_RELEASE number seq */
long gcon_ack(struct file *f, u32 seq)
{
    struct gcon *g = gc;
    if (f->type != FT_GCON || !g || g->file != f || g->pid != current->pid)
        return -EBADF;
    if ((u16)seq != g->seq)
        return -EINVAL;                 /* an answer to an older request */
    if (g->state == G_RELEASING) {
        let_go(g);
        return 0;
    }
    if (g->forced) {                    /* the late answer to a forced switch: the blitter is the kernel's again */
        g->forced = 0;
        gfx_blit_cpu(0);
        return 0;
    }
    return -EINVAL;                     /* nothing asked */
}

/* the last close of the grab's descriptor (the server's end too, then with interrupts off: proc_exit): the console
 * is the kernel's again; drawing it waits for kwork */
static void end_grab(struct gcon *g)
{
    u16 s = irq_save();
    gc = 0;
    irq_restore(s);
    if (g->state != G_AWAY) {
        kbd_forget();
        gfx_hand(0);
        redraw_to = g->state == G_RELEASING ? g->target : g->con;
        kwork_pending = 1;
    }
    if (g->forced)
        gfx_blit_cpu(0);                /* the server is gone: nobody else writes the blitter */
    ev_wake(&g->wait);
    kprintf("display: console %d given back by pid %d\n", g->con, g->pid);
    kfree(g);
}

void gcon_close(struct file *f)
{
    if (gc && gc->file == f)
        end_grab(gc);
}

/* the server ends (proc_exit, interrupts off): the grab with it, whoever else holds the descriptor (a child given it
 * as 0-2), since the Copper list and the sprite are its memory */
void gcon_proc_exit(int pid)
{
    if (gc && gc->pid == pid)
        end_grab(gc);
}

/* SYS_memfree: a region holding the grab's Copper list or sprite is not given back while the grab lasts */
int gcon_uses(u32 base, u32 size)
{
    struct gcon *g = gc;
    return g && g->pid == current->pid &&
           ((g->cop >= base && g->cop < base + size) || ((u32)g->spr >= base && (u32)g->spr < base + size));
}

/* read: whole records (a buffer under one record: -EINVAL); waits for one unless O_NONBLOCK */
long gcon_read(struct file *f, void *buf, u32 n)
{
    if (n < sizeof(struct ugevent))
        return -EINVAL;
    for (;;) {
        struct gcon *g = gc;
        if (!g || g->file != f || g->pid != current->pid)
            return -EBADF;
        if (current->killed)
            return -EINTR;
        ev_arm(&g->wait);
        u32 k = 0, max = n / sizeof(struct ugevent);
        struct ugevent *d = buf;
        while (k < max && g->tail != g->head) {
            d[k++] = g->q[g->tail];
            g->tail = (u16)((g->tail + 1) % GQN);
        }
        if (k)
            return (long)(k * sizeof(struct ugevent));
        if (f->flags & O_NONBLOCK)
            return -EAGAIN;
        long e = ev_block(EV_IO, 0, 0, 1);
        if (e)
            return e;
    }
}

u16 gcon_poll(struct file *f)
{
    struct gcon *g = gc;
    if (!g || g->file != f)
        return POLLNVAL;
    ev_arm(&g->wait);
    return g->tail != g->head ? POLLIN : 0;
}

/* a key for the ring (kbd.c, process context): the raw code (bit 7 up), the qualifiers, its characters */
void gcon_key(u8 code, u16 qual, const u8 *ch)
{
    struct gcon *g = gc;
    if (g && g->state != G_AWAY)
        push(g, GEV_KEY, code, qual | g->btn, g->px, g->py, ch, 0);
}

/* the 100 Hz tick (interrupt): the mouse moves the pointer (two control words); kwork tells the ring */
void gcon_tick(void)
{
    struct gcon *g = gc;
    if (!g || g->state == G_AWAY)
        return;
    s32 dx, dy;
    u32 b;
    input_take(&dx, &dy, &b);
    u16 bq = (u16)((b & 1 ? GQ_LBUTTON : 0) | (b & 2 ? GQ_RBUTTON : 0) | (b & 4 ? GQ_MBUTTON : 0));
    if (dx || dy) {
        s32 x = g->px + 2 * dx, y = g->py + dy;     /* a hires pixel is half as wide as a line is tall */
        g->px = (short)(x < 0 ? 0 : x >= g->w ? g->w - 1 : x);
        g->py = (short)(y < 0 ? 0 : y >= g->h ? g->h - 1 : y);
        g->moved = 1;
    }
    g->bdown |= (u16)(bq & ~g->btn);
    g->bup |= (u16)(g->btn & ~bq);
    if (dx || dy || bq != g->btn || (g->state == G_RELEASING && (s32)(ticks - g->deadline) >= 0)) {
        g->btn = bq;
        kwork_pending = 1;
    }
}

/* the vertical-blank interrupt: the pointer's new place into the sprite's control words */
void gcon_vblank(void)
{
    struct gcon *g = gc;
    if (g && g->moved && g->state != G_AWAY) {
        g->moved = 0;
        sprite_pos(g);
    }
}

/* kwork (process context): the pointer's news into the ring, the release's deadline, a console given back */
void gcon_work(void)
{
    if (redraw_to) {
        int to = redraw_to;
        redraw_to = 0;
        if (to == con_cur)
            term_redraw();
        else
            con_show(to);
    }
    struct gcon *g = gc;
    if (!g || g->state == G_AWAY)
        return;
    if (g->state == G_RELEASING && (s32)(ticks - g->deadline) >= 0) {
        kprintf("display: pid %d did not let go of console %d within 1 s: switched anyway, the kernel draws with "
                "the CPU until it answers\n", g->pid, g->con);
        g->forced = 1;
        gfx_blit_cpu(1);
        let_go(g);
        return;
    }
    u16 s = irq_save();
    short x = g->px, y = g->py;
    u16 b = g->btn, dn = g->bdown, up = g->bup;
    g->bdown = g->bup = 0;
    irq_restore(s);
    u16 b1 = (u16)(g->lbtn | dn), b2 = (u16)(b1 & ~up);    /* a press and a release since: both told */
    if (b1 != g->lbtn && b1 != b) {
        push(g, GEV_POINTER, 0, kbd_qual() | b1, x, y, 0, 0);
        g->lbtn = b1;
    }
    if (b2 != g->lbtn && b2 != b) {
        push(g, GEV_POINTER, 0, kbd_qual() | b2, x, y, 0, 0);
        g->lbtn = b2;
    }
    if (x == g->lx && y == g->ly && b == g->lbtn)
        return;
    u16 q = kbd_qual() | b;
    if (g->head != g->tail) {           /* merged into the newest unread record when nothing else changed */
        struct ugevent *e = &g->q[(g->head + GQN - 1) % GQN];
        if (e->type == GEV_POINTER && e->qual == q) {
            e->x = x;
            e->y = y;
            e->time = ticks;
            g->lx = x;
            g->ly = y;
            g->lbtn = b;
            return;
        }
    }
    push(g, GEV_POINTER, 0, q, x, y, 0, 0);
    g->lx = x;
    g->ly = y;
    g->lbtn = b;
}
