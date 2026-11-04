/* SPDX-License-Identifier: GPL-2.0-only */
/* displayd, the display server (DESIGN D38, docs/d38-desktop.md; docs/man/displayd.txt). A service (D11: port
 * "display" in /etc/svctab), started by the first program that calls it. It takes the console of that first client
 * (SYS_peer PEER_CTTY) from the kernel (SYS_console CON_GRAB: its Copper list, the pointer as sprite 0, the keys and
 * the mouse in a ring), and from then on owns the screen and the blitter while that console is shown: it draws
 * everything - windows, frames from the window manager's style, the programs' drawing orders - and sends events into
 * each client's ring. It ends when its last client has gone (-persist: never; dlogin's) and the console is text
 * again.
 *
 * The messages (display/proto.h) are answered at once; the server never waits for a client. Every pointer a client
 * gives is checked with SYS_peer before it is read or written, every order's lengths and places before use; events
 * reach a client only through SYS_post, which the kernel checks against the client's memory at every write. */
#include "dpyd.h"

int aga;
int wm_cl = -1;
static int port, gfd = -1, confd = -1, persist, grabbed;
static u16 *copl;                       /* the Copper list (chip RAM) */
static int cop_col;                     /* the index of COLOR00's value in it */
static u16 *spr;                        /* the pointer: sprite 0 */
static int ptx, pty;                    /* the pointer, from the kernel */
static u16 btns;                        /* its buttons (GQ_*BUTTON) */
static int grab_w = -1;                 /* the window a held button belongs to */
static int grab_id;                     /* its id: a window closed or replaced meanwhile is not it */
static int session_uid = -1;
static int pgrab_w = -1, pgrab_id;      /* DPY_WM_PGRAB: the manager's window that gets the pointer */
/* the last press on a frame (EV_WM_FRAME): a manager's DPY_WM_DRAG may come after the button is let go (a slow
 * manager, a quick drag): the drag then runs from the press to where it was let go */
static struct { int win, x, y, up, ux, uy; } fpress;

/* ---- clients ---- */
#define NCL 16
#define NPEND 32
struct client {
    int used, pid, port, uid;
    u32 ring, nrec, head;               /* the ring in the client's memory; the next record we write */
    char name[DPY_NAME];
    u32 imgbytes;                       /* its images' chip RAM (at most IMG_BUDGET) */
    struct dpyev pend[NPEND];
    int npend, lost;
};
static struct client cls[NCL];

/* ---- images ---- */
#define NIMG 48
#define IMG_BUDGET (64u << 10)          /* a program's images in chip RAM: icons and gadgets, never a screenful */
struct image { u16 id; u8 cl; struct bm bm; u8 *mask; };
static struct image imgs[NIMG];
static u16 next_img = 1;

const u8 *dpy_image(int id, int cl, struct bm **img)
{
    for (int i = 0; i < NIMG; i++)
        if (imgs[i].id && imgs[i].id == id && (cl < 0 || imgs[i].cl == cl)) {
            *img = &imgs[i].bm;
            return imgs[i].mask;
        }
    *img = 0;
    return 0;
}

static void img_free(int i)
{
    if (imgs[i].cl < NCL)
        cls[imgs[i].cl].imgbytes -= imgs[i].bm.stride * imgs[i].bm.h;
    bm_free(&imgs[i].bm);
    free(imgs[i].mask);
    imgs[i].mask = 0;
    imgs[i].id = 0;
}

/* ---- events ---- */
static void flush(int cl);
void ev_to(int cl, struct dpyev *e)
{
    struct client *c = &cls[cl];
    if (!c->used)
        return;
    if (c->npend >= NPEND) {
        flush(cl);                      /* a burst (keys typed fast): into the ring now; only a full ring loses any */
        if (!c->used || c->npend >= NPEND) {
            c->lost = 1;
            return;
        }
    }
    if (e->type == EV_MOTION && c->npend) {     /* motion merged first (D38 4.1) */
        struct dpyev *l = &c->pend[c->npend - 1];
        if (l->type == EV_MOTION && l->win == e->win && l->qual == e->qual) {
            *l = *e;
            return;
        }
    }
    c->pend[c->npend++] = *e;
}
void ev_win(int s, int type, int code, int arg, int x, int y, int w, int h)
{
    struct win *wi = &wins[s];
    struct dpyev e;
    memset(&e, 0, sizeof e);
    e.type = (u8)type; e.code = (u8)code; e.win = wi->id; e.arg = (u16)arg;
    e.x = (s16)x; e.y = (s16)y; e.w = (u16)w; e.h = (u16)h;
    e.qual = btns;
    ev_to(wi->cl, &e);
}
void ev_wm(int type, int win, int code, int arg, int x, int y, int w, int h)
{
    if (wm_cl < 0)
        return;
    struct dpyev e;
    memset(&e, 0, sizeof e);
    e.type = (u8)type; e.code = (u8)code; e.win = (u16)win; e.arg = (u16)arg;
    e.x = (s16)x; e.y = (s16)y; e.w = (u16)w; e.h = (u16)h;
    e.qual = btns;
    ev_to(wm_cl, &e);
}
static void drop(int cl);
static int post(struct client *c, u32 off, const void *src, u32 len)
{
    struct upost p = { off, (u32)src, len };
    return sys_post(POST_WRITE, c->port, &p);
}
/* the queued events into the client's ring: as many as fit (the client's tail read from its memory), then the head;
 * the rest dropped and DPYR_LOST set */
static void flush(int cl)
{
    struct client *c = &cls[cl];
    if (!c->used || (!c->npend && !c->lost))
        return;
    volatile const struct dpyring *r = (const struct dpyring *)c->ring;
    u32 tail = r->tail, used = c->head - tail;
    if (used > c->nrec)                 /* the client's tail is nonsense: nothing fits, the client is told */
        used = c->nrec;
    int put = 0, rc = 0;
    while (put < c->npend && used < c->nrec) {
        u32 at = c->head % c->nrec, run = c->nrec - at;
        u32 n = (u32)(c->npend - put);
        if (n > run) n = run;
        if (n > c->nrec - used) n = c->nrec - used;
        rc = post(c, sizeof(struct dpyring) + at * sizeof(struct dpyev), &c->pend[put], n * sizeof(struct dpyev));
        if (rc < 0)
            break;
        put += (int)n;
        used += n;
        c->head += n;
    }
    if (put < c->npend)
        c->lost = 1;
    c->npend = 0;
    if (rc >= 0 && c->lost) {
        u32 f = DPYR_LOST;
        rc = post(c, 8, &f, 4);
        c->lost = 0;
    }
    if (rc >= 0)
        rc = post(c, 0, &c->head, 4);
    if (rc < 0)
        drop(cl);                       /* its port or its ring is gone: so is the client */
}

/* ---- the screen ---- */
static const u16 def_pal[16] = {       /* look (a), "flat with a hint of depth" (docs/desktop-looks) */
    0x344, 0x112, 0xEEE, 0x223, 0x47B, 0xBBC, 0x455, 0xFFF,
    0xCCD, 0xD54, 0x7C5, 0xEB4, 0x69E, 0xB7C, 0x5BC, 0x567,
};
static void default_style(void)
{
    memset(&style, 0, sizeof style);
    style.size = sizeof style;
    memcpy(style.palette, def_pal, sizeof def_pal);
    style.left = style.right = style.bottom = 1;
    style.top = 12;
    style.title = 11;
    style.shadow = 1;
    style.border[0] = style.border[1] = 1;
    style.bar[0] = 5; style.bar[1] = 4;
    style.text[0] = 6; style.text[1] = 7;
    style.shadowc = 3;
    style.gadx[0] = 128 + 10;           /* the close gadget at the right, an x (no manager: the built-in style) */
    /* drags: the outline on a 68020/68030, the whole window on a 68040/68060 (and Emu68, which says 68040): D38
     * question 3's default, from the CPU AmigaOS found (exec's AttnFlags, through ZINE_INFO) */
    u32 zi[7];
    style.drag = sys3(93, 0, (u32)zi, sizeof zi) == 0 && (zi[2] & 0x88) ? 0 : DRAG_OUTLINE;
}
static void set_palette(void)
{
    for (int i = 0; i < (1 << scr.depth); i++)
        copl[cop_col + 2 * i + 1] = style.palette[i] & 0xFFF;
}

/* the pointer, 16 x 16 lores pixels: o the outline (colour 17), w the fill (18) */
static const char *const arrow[16] = {
    "o...............", "oo..............", "owo.............", "owwo............", "owwwo...........",
    "owwwwo..........", "owwwwwo.........", "owwwwwwo........", "owwwwwwwo.......", "owwwwwoooo......",
    "owwowwo.........", "owo.owwo........", "oo..owwo........", "o....owwo.......", ".....owwo.......",
    "......oo........",
};

static int screen_up(int con)
{
    char dev[12] = "/dev/tty";
    if (con >= 10) { dev[8] = '1'; dev[9] = '0'; dev[10] = 0; } else { dev[8] = (char)('0' + con); dev[9] = 0; }
    confd = sys_open(dev, O_RDWR);
    if (confd < 0)
        return confd;
    static u8 topaz[224 * 8];
    u16 *mem = sys_memalloc_chip(1024);
    if (!mem)
        return -ENOMEM;
    spr = mem;
    u16 *nul = mem + 2 * 16 + 4;
    copl = mem + 64;
    memset(mem, 0, 1024);
    for (int y = 0; y < 16; y++) {
        u16 a = 0, b = 0;
        for (int x = 0; x < 16; x++) {
            if (arrow[y][x] == 'o') a |= (u16)(0x8000 >> x);
            if (arrow[y][x] == 'w') b |= (u16)(0x8000 >> x);
        }
        spr[2 + 2 * y] = a;
        spr[3 + 2 * y] = b;
    }
    /* a first grab with no screen yet only to learn the machine: the kernel's answer has the chips and the size */
    struct ucongrab g;
    memset(&g, 0, sizeof g);
    g.size = sizeof g;
    g.topaz = (u32)topaz;
    g.sprite = (u32)spr;
    g.cop = (u32)copl;
    g.sprh = 16;
    /* the Copper list is built for the screen's size; the size is the console's: 640 x 256 PAL, 640 x 200 NTSC -
     * asked with a list that is only its end, then the real one written in place before the hand-over takes effect
     * (the Copper reads it from the next frame) */
    copl[0] = 0xFFFF; copl[1] = 0xFFFE;
    gfd = sys_congrab(confd, &g);
    if (gfd < 0) {
        sys_close(confd);
        sys_memfree(mem);
        return gfd;
    }
    aga = g.chips & GCHIP_AGA;
    int depth = aga ? 4 : 3;
    if (bm_alloc(&scr, g.width, g.height, depth) < 0) {    /* no chip RAM: the console given back, nothing kept */
        sys_close(gfd);
        gfd = -1;
        sys_close(confd);
        sys_memfree(mem);
        return -ENOMEM;
    }
    scr.kept = 0;
    chip_used = 0;                      /* the screen is not a kept copy */
    grabbed = 1;
    font_init(topaz);
    u16 *c = copl;
    int f3 = aga;
#define MOVE(r, v) do { *c++ = (u16)(r); *c++ = (u16)(v); } while (0)
    if (aga) {
        MOVE(0x1FC, f3 ? 3 : 0);        /* FMODE: 64-bit bitplane fetch, 16-pixel sprites (gcon.c's contract) */
        MOVE(0x106, 0x0C00);            /* BPLCON3 */
        MOVE(0x10C, 0x0011);            /* BPLCON4: the sprites' colours 16-31 */
    }
    MOVE(0x100, 0x8200 | depth << 12);  /* BPLCON0: hires */
    MOVE(0x102, 0);
    MOVE(0x104, 0x0024);                /* BPLCON2: the sprites in front of the planes */
    MOVE(0x108, scr.stride - scr.bpr);
    MOVE(0x10A, scr.stride - scr.bpr);
    MOVE(0x092, f3 ? 0x38 : 0x3C);      /* the console's fetch window (kernel/gfx.c) */
    MOVE(0x094, f3 ? 0xC0 : 0xD4);
    cop_col = (int)(c - copl);
    for (int i = 0; i < (1 << depth); i++)
        MOVE(0x180 + 2 * i, 0);
    MOVE(0x1A2, 0x000);                 /* the pointer: black outline, white face */
    MOVE(0x1A4, 0xFFF);
    MOVE(0x1A6, 0x888);
    MOVE(0x120, (u32)spr >> 16);
    MOVE(0x122, (u32)spr);
    for (int i = 1; i < 8; i++) {
        MOVE(0x120 + 4 * i, (u32)nul >> 16);
        MOVE(0x122 + 4 * i, (u32)nul);
    }
    for (int k = 0; k < depth; k++) {
        u32 p = (u32)scr.p + (u32)k * scr.bpr;
        MOVE(0x0E0 + 4 * k, p >> 16);
        MOVE(0x0E2 + 4 * k, p);
    }
    *c++ = 0xFFFF;
    *c++ = 0xFFFE;
#undef MOVE
    set_palette();
    ptx = g.width / 2;
    pty = g.height / 2;
    return 0;
}

/* ---- input from the kernel's ring ---- */
static struct dpykey grabs[DPY_MAXGRAB];
static int ngrabs;
static void key(const struct ugevent *e)
{
    int up = e->code & 0x80, code = e->code & 0x7F;
    if (wm_cl >= 0)
        for (int i = 0; i < ngrabs; i++)
            if (grabs[i].code == code && (e->qual & grabs[i].mask) == grabs[i].qual) {
                if (!up)
                    ev_wm(EV_WM_KEY, focus >= 0 ? wins[focus].id : 0, code, grabs[i].id, ptx, pty, 0, 0);
                return;
            }
    if (focus < 0)
        return;
    struct dpyev v;
    struct rect in;
    win_inside(&wins[focus], &in);
    memset(&v, 0, sizeof v);
    v.type = EV_KEY;
    v.code = e->code;
    v.qual = e->qual;
    v.win = wins[focus].id;
    v.x = (s16)(ptx - in.x0);
    v.y = (s16)(pty - in.y0);
    memcpy(v.ch, e->ch, 4);
    ev_to(wins[focus].cl, &v);
}
static void grab_check(void)
{
    if (grab_w >= 0 && (wins[grab_w].id != grab_id || !(wins[grab_w].state & WST_SHOWN)))
        grab_w = -1;
}
static void button(int which, int down, u16 qual)
{
    grab_check();
    int part, s = grab_w >= 0 && !down ? grab_w : win_at(ptx, pty, &part);
    if (grab_w >= 0 && !down)
        part = 0;
    if (!down && which == 1 && fpress.win && !fpress.up) {
        fpress.up = 1;
        fpress.ux = ptx;
        fpress.uy = pty;
    }
    if (dragging) {
        if (!down && which == 1)
            win_drag_event(ptx, pty, 1);
        return;
    }
    if (s < 0)
        return;
    struct win *w = &wins[s];
    struct rect in;
    win_inside(w, &in);
    if (down && part) {                 /* a frame: the manager's, or the built-in rules */
        if (wm_cl >= 0) {
            if (which == 1)
                fpress = (typeof(fpress)){ w->id, ptx, pty, 0, 0, 0 };
            ev_wm(EV_WM_FRAME, w->id, part, which, ptx, pty, 0, 0);
            return;
        }
        win_stack(s, 0);
        win_focus(s);
        if (which == 1 && (part == FP_TITLE || part == FP_SIZE))
            win_drag(s, (part == FP_TITLE ? DRAG_MOVE : DRAG_RESIZE) | style.drag, ptx, pty);
        else if (which == 1 && part == FP_CLOSE)
            ev_win(s, EV_CLOSE, 0, 0, 0, 0, 0, 0);
        return;
    }
    if (part)
        return;
    if (down) {
        grab_w = s;
        grab_id = w->id;
        if (wm_cl >= 0)
            ev_wm(EV_WM_CLICK, w->id, which, 0, ptx, pty, 0, 0);
        else if (w->kind != WK_DESKTOP && w->kind != WK_BAR) {
            win_stack(s, 0);
            win_focus(s);
        }
    } else
        grab_w = -1;
    struct dpyev v;
    memset(&v, 0, sizeof v);
    v.type = EV_BUTTON;
    v.code = (u8)(which | (down ? 0 : 0x80));
    v.qual = qual;
    v.win = w->id;
    v.x = (s16)(ptx - in.x0);
    v.y = (s16)(pty - in.y0);
    ev_to(w->cl, &v);
}
static const u16 bit[3] = { GQ_LBUTTON, GQ_RBUTTON, GQ_MBUTTON };
static void pgrab_event(u16 old, u16 now, u16 qual, int moved)
{
    struct win *w = &wins[pgrab_w];
    struct rect in;
    win_inside(w, &in);
    struct dpyev v;
    memset(&v, 0, sizeof v);
    v.win = w->id;
    v.qual = qual;
    v.x = (s16)(ptx - in.x0);
    v.y = (s16)(pty - in.y0);
    if (moved) {
        v.type = EV_MOTION;
        ev_to(w->cl, &v);
    }
    for (int i = 0; i < 3; i++)
        if ((old ^ now) & bit[i]) {
            v.type = EV_BUTTON;
            v.code = (u8)((i + 1) | (now & bit[i] ? 0 : 0x80));
            ev_to(w->cl, &v);
        }
    if (!now)
        pgrab_w = -1;
}
static void pointer(const struct ugevent *e)
{
    u16 old = btns, now = e->qual & (GQ_LBUTTON | GQ_RBUTTON | GQ_MBUTTON);
    int moved = e->x != ptx || e->y != pty;
    ptx = e->x;
    pty = e->y;
    btns = now;
    if (pgrab_w >= 0 && (wins[pgrab_w].id != pgrab_id || !(wins[pgrab_w].state & WST_SHOWN)))
        pgrab_w = -1;
    if (pgrab_w >= 0) {                 /* the manager's menu has the pointer until the buttons are up */
        pgrab_event(old, now, e->qual, moved);
        return;
    }
    if (moved) {
        grab_check();
        if (dragging)
            win_drag_event(ptx, pty, 0);
        else {
            int part, s = grab_w >= 0 ? grab_w : win_at(ptx, pty, &part);
            if (s >= 0 && (grab_w >= 0 || (wins[s].flags & WF_MOTION))) {
                struct rect in;
                win_inside(&wins[s], &in);
                struct dpyev v;
                memset(&v, 0, sizeof v);
                v.type = EV_MOTION;
                v.qual = e->qual;
                v.win = wins[s].id;
                v.x = (s16)(ptx - in.x0);
                v.y = (s16)(pty - in.y0);
                ev_to(wins[s].cl, &v);
            }
        }
    }
    for (int i = 0; i < 3; i++)
        if ((old ^ now) & bit[i])
            button(i + 1, (now & bit[i]) != 0, e->qual);
}
static int has_clients(void);
static void kernel_events(void)
{
    struct ugevent ev[16];
    for (;;) {
        struct pollfd p = { gfd, POLLIN, 0 };
        if (sys_evwait(&p, 1, 0) <= 0)  /* the ring's read waits when it is empty: only what is there */
            return;
        int n = sys_read(gfd, ev, sizeof ev);
        if (n <= 0)
            return;
        for (int i = 0; i < n / (int)sizeof ev[0]; i++) {
            const struct ugevent *e = &ev[i];
            switch (e->type) {
            case GEV_RELEASE:           /* the blit in progress ends, the blitter is the kernel's: then the answer */
                blit_idle();
                use_blit = 0;
                sys_congack(gfd, (u16)e->x);
                break;
            case GEV_ACQUIRE:           /* every key counts as up; the screen was never touched */
                use_blit = 1;
                ptx = e->x;
                pty = e->y;
                break;
            case GEV_KEY:
                key(e);
                break;
            case GEV_POINTER:
                pointer(e);
                break;
            }
        }
    }
}
void kick(void) { kernel_events(); }

/* ---- the requests ---- */
static int peer(struct umsg *m, int op, u32 addr, u32 len)
{
    struct upeermem pm = { addr, len };
    return sys_peer(m, op, &pm);
}
static int client_of(struct umsg *m)
{
    for (int i = 0; i < NCL; i++)
        if (cls[i].used && cls[i].pid == m->from)
            return i;
    return -1;
}
static int has_clients(void)
{
    for (int i = 0; i < NCL; i++)
        if (cls[i].used)
            return 1;
    return 0;
}
static void drop(int cl)
{
    struct client *c = &cls[cl];
    if (!c->used)
        return;
    c->used = 0;
    if (wm_cl == cl) {                  /* the manager gone: the server places and shows by itself (D38 3.3) */
        wm_cl = -1;
        for (int k = 0; k < NWIN; k++)
            if (wins[k].id && wins[k].cl != cl && !(wins[k].state & WST_SHOWN))
                win_show(k, 1, 0);
    }
    for (int i = 0; i < NWIN; i++)
        if (wins[i].id && wins[i].cl == cl) {
            int id = wins[i].id;
            win_close(i);
            ev_wm(EV_WM_GONE, id, 0, 0, 0, 0, 0, 0);
        }
    for (int i = 0; i < NIMG; i++)
        if (imgs[i].id && imgs[i].cl == cl)
            img_free(i);
}
static int hello(struct umsg *m)
{
    struct dpyhello *h = (struct dpyhello *)m->a[0];
    if (peer(m, PEER_WR, (u32)h, DPYHELLO_FIRST) < 0 || h->size < DPYHELLO_FIRST)
        return -EFAULT;
    if (h->version < 1)
        return -EINVAL;
    if (client_of(m) >= 0)
        return -EBUSY;
    if (session_uid >= 0 && m->uid != session_uid && m->uid != 0)
        return -EACCES;                 /* another user's program on this desktop: no */
    u32 nrec = h->ringsize > sizeof(struct dpyring) ? (h->ringsize - sizeof(struct dpyring)) / sizeof(struct dpyev) : 0;
    if (nrec < 8 || peer(m, PEER_WR, h->ring, h->ringsize) < 0)
        return -EINVAL;
    if (sys_peer(m, PEER_PORT, (void *)h->port) < 0)
        return -EPERM;                  /* events go only into the asking program's own ring */
    int cl = -1;
    for (int i = 0; i < NCL; i++)
        if (!cls[i].used) { cl = i; break; }
    if (cl < 0)
        return -ENOSPC;
    if (!grabbed) {                     /* the first client: the console it is on is the desktop's */
        int con = sys_peer(m, PEER_CTTY, 0);
        if (con < 1 || con > 10)
            return -ENOTTY;             /* a pseudo-terminal, a service: not a console */
        int e = screen_up(con);
        if (e < 0) {
            dprintf(2, "displayd: the console %d could not be taken: %d\n", con, e);
            return e;
        }
        session_uid = m->uid;
        default_style();
        set_palette();
        use_blit = 0;                   /* until GEV_ACQUIRE (the console may not be shown) */
        win_redraw_all();
    }
    struct client *c = &cls[cl];
    memset(c, 0, sizeof *c);
    c->used = 1;
    c->pid = m->from;
    c->uid = m->uid;
    c->port = h->port;
    c->ring = h->ring;
    c->nrec = nrec;
    memcpy(c->name, h->name, DPY_NAME);
    c->name[DPY_NAME - 1] = 0;
    struct dpyring hd = { 0, 0, 0, nrec };
    if (post(c, 0, &hd, sizeof hd) < 0) {       /* the client's own post window (POST_BUF) must be there */
        c->used = 0;
        return -EPERM;
    }
    h->width = scr.w;
    h->height = scr.h;
    h->depth = scr.depth;
    h->id = (u16)(cl + 1);
    return DPY_VERSION;
}
static struct win *mine(int cl, u32 id)
{
    struct win *w = win_get((int)id);
    return w && (w->cl == cl || cl == wm_cl) ? w : 0;
}
static int req(struct umsg *m)
{
    if (m->type == DPY_HELLO)
        return hello(m);
    int cl = client_of(m);
    if (cl < 0)
        return -ENOTCONN;
    u32 *a = m->a;
    struct win *w;
    switch (m->type) {
    case DPY_WIN_NEW: {
        struct dpywin *d = (struct dpywin *)a[0];
        if (peer(m, PEER_WR, a[0], DPYWIN_FIRST) < 0 || d->size < DPYWIN_FIRST)
            return -EFAULT;
        if ((d->kind == WK_BAR || d->kind == WK_DESKTOP) && cl != wm_cl)
            return -EPERM;              /* the manager's kinds */
        int s = win_new(cl, d);
        if (s < 0)
            return s;
        w = &wins[s];
        if (wm_cl >= 0 && cl != wm_cl)
            ev_wm(EV_WM_NEW, w->id, 0, w->kind, d->x, d->y, w->iw, w->ih);
        else
            win_show(s, 1, 0);
        d->x = w->fx; d->y = w->fy; d->w = w->iw; d->h = w->ih;
        return w->id;
    }
    case DPY_WIN_SET:
        if (!(w = mine(cl, a[0])))
            return -ENOENT;
        if (a[1] == WS_TITLE) {
            int whole = peer(m, PEER_RD, a[2], DPY_TITLE - 1) == 0, i = 0;    /* else byte by byte (its memory's end) */
            for (; i < DPY_TITLE - 1; i++) {
                if (!whole && peer(m, PEER_RD, a[2] + (u32)i, 1) < 0)
                    return -EFAULT;
                char ch = ((const char *)a[2])[i];
                if (!ch)
                    break;
                w->title[i] = ch;
            }
            w->title[i] = 0;
            win_title((int)(w - wins));
            ev_wm(EV_WM_TITLE, w->id, 0, 0, 0, 0, 0, 0);
            return 0;
        }
        if (a[1] == WS_SIZE) {
            if (wm_cl >= 0 && cl != wm_cl)
                ev_wm(EV_WM_SIZE, w->id, 0, 0, w->fx, w->fy, (int)(a[2] >> 16), (int)(a[2] & 0xFFFF));
            else
                win_place((int)(w - wins), w->fx, w->fy, (int)(a[2] >> 16), (int)(a[2] & 0xFFFF));
            return 0;
        }
        return -EINVAL;
    case DPY_WIN_CLOSE: {
        if (!(w = mine(cl, a[0])))
            return -ENOENT;
        int id = w->id;
        win_close((int)(w - wins));
        ev_wm(EV_WM_GONE, id, 0, 0, 0, 0, 0, 0);
        return 0;
    }
    case DPY_DRAW: {
        if (!(w = win_get((int)a[0])) || w->cl != cl)
            return -ENOENT;
        if ((a[1] & 1) || (a[2] & 1) || a[2] > 65536 || peer(m, PEER_RD, a[1], a[2]) < 0)
            return -EFAULT;
        u32 bad = ~0u;
        drag_hide();
        win_draw((int)(w - wins), (const u16 *)a[1], a[2] / 2, &bad);
        drag_show();
        if (bad != ~0u) {
            a[3] = bad;
            return -EINVAL;
        }
        return 0;
    }
    case DPY_FONT:
        return 0 << 16 | 8 << 8 | 8;    /* one font in version 1: the console's, 8 x 8 */
    case DPY_IMAGE: {
        struct dpyimage *d = (struct dpyimage *)a[0];
        if (peer(m, PEER_RD, a[0], DPYIMAGE_FIRST) < 0 || d->size < DPYIMAGE_FIRST)
            return -EFAULT;
        int planes = d->depth + (d->flags & IMG_MASK ? 1 : 0);
        u32 wpr = (u32)(d->w + 15) / 16 * 2;
        if (!d->w || !d->h || d->w > 640 || d->h > 256 || d->depth < 1 || d->depth > 4)
            return -EINVAL;
        if (peer(m, PEER_RD, d->data, wpr * d->h * (u32)planes) < 0)
            return -EFAULT;
        int k = -1;
        for (int i = 0; i < NIMG; i++)
            if (!imgs[i].id) { k = i; break; }
        if (k < 0)
            return -ENOSPC;
        u32 bytes = wpr * d->h * d->depth;
        if (cls[cl].imgbytes + bytes > IMG_BUDGET)
            return -ENOSPC;             /* past its images' budget */
        struct image *im = &imgs[k];
        if (bm_alloc(&im->bm, d->w, d->h, d->depth) < 0)
            return -ENOMEM;
        im->bm.kept = 0;                /* not the kept copies' budget: its own */
        chip_used -= bytes;
        cls[cl].imgbytes += bytes;
        const u8 *src = (const u8 *)d->data;
        for (int p = 0; p < d->depth; p++)
            for (int y = 0; y < d->h; y++)
                memcpy(im->bm.p + (u32)y * im->bm.stride + (u32)p * im->bm.bpr, src + ((u32)p * d->h + y) * wpr, wpr);
        im->mask = 0;
        if (d->flags & IMG_MASK) {
            im->mask = malloc(wpr * d->h);
            if (!im->mask) {
                bm_free(&im->bm);
                return -ENOMEM;
            }
            memcpy(im->mask, src + (u32)d->depth * d->h * wpr, wpr * d->h);
        }
        struct bm *dummy;
        do {                            /* a number no image has now */
            im->id = next_img++;
            if (!next_img) next_img = 1;
            dpy_image(im->id, -1, &dummy);
        } while (dummy && dummy != &im->bm);
        im->cl = (u8)cl;
        return im->id;
    }
    case DPY_IMAGE_FREE:
        for (int i = 0; a[0] && i < NIMG; i++)
            if (imgs[i].id == a[0] && imgs[i].cl == cl) {
                img_free(i);
                return 0;
            }
        return -ENOENT;
    case DPY_PALETTE: {
        u32 n = a[1] > 16 ? 16 : a[1];
        if (n && peer(m, PEER_WR, a[0], n * 2) < 0)
            return -EFAULT;
        for (u32 i = 0; i < n; i++)
            ((u16 *)a[0])[i] = i < (1u << scr.depth) ? style.palette[i] : 0;
        return scr.depth;
    }
    case DPY_BYE:
        drop(cl);
        return 0;
    case DPY_MENU: {
        if (!(w = mine(cl, a[0])) || w->cl != cl)
            return -ENOENT;
        if (a[2] > DPY_MENUMAX || (a[2] && peer(m, PEER_RD, a[1], a[2]) < 0))
            return a[2] > DPY_MENUMAX ? -EINVAL : -EFAULT;
        char *t = a[2] ? malloc(a[2]) : 0;
        if (a[2] && !t)
            return -ENOMEM;
        if (t)
            memcpy(t, (const void *)a[1], a[2]);
        free(w->menu);
        w->menu = t;
        w->nmenu = (u16)a[2];
        ev_wm(EV_WM_MENU, w->id, 0, 0, 0, 0, 0, 0);
        return 0;
    }
    case DPY_WIN_INFO: {
        if (!(w = mine(cl, a[0])))
            return -ENOENT;
        struct dpyinfo *d = (struct dpyinfo *)a[1];
        if (peer(m, PEER_WR, a[1], DPYINFO_FIRST) < 0 || d->size < DPYINFO_FIRST)
            return -EFAULT;
        struct rect in;
        win_inside(w, &in);
        d->x = w->fx; d->y = w->fy; d->w = w->iw; d->h = w->ih; d->ix = in.x0; d->iy = in.y0;
        d->state = (u16)(w->state | ((int)(w - wins) == focus ? WST_FOCUS : 0));
        d->kind = w->kind;
        d->pid = cls[w->cl].pid;
        memcpy(d->title, w->title, DPY_TITLE);
        return 0;
    }
    case DPY_WM_HELLO:
        if (wm_cl >= 0 && wm_cl != cl)
            return -EBUSY;
        if (m->uid != session_uid && m->uid != 0)
            return -EPERM;
        wm_cl = cl;
        for (int k = 0; k < norder; k++) {      /* the windows there already are the new manager's */
            struct win *x = &wins[order[k]];
            if (x->id && x->cl != cl)
                ev_wm(EV_WM_NEW, x->id, 0, x->kind | ((x->state & WST_SHOWN) ? 0x100 : 0), x->fx, x->fy, x->iw, x->ih);
        }
        return 0;
    }
    if (cl != wm_cl)
        return m->type >= DPY_WM_HELLO ? -EPERM : -ENOSYS;
    int s = (w = win_get((int)a[0])) ? (int)(w - wins) : -1;
    switch (m->type) {
    case DPY_WM_STYLE: {
        struct dpystyle *d = (struct dpystyle *)a[0];
        if (peer(m, PEER_RD, a[0], DPYSTYLE_FIRST) < 0 || d->size < DPYSTYLE_FIRST)
            return -EFAULT;
        struct dpystyle st = *d;
        st.size = sizeof st;
        if (st.title > st.top || st.left > 64 || st.right > 64 || st.top > 64 || st.bottom > 64 || st.shadow > 8)
            return -EINVAL;
        win_style(&st);
        set_palette();
        return 0;
    }
    case DPY_WM_GRAB: {
        u32 n = a[1] > DPY_MAXGRAB ? DPY_MAXGRAB : a[1];
        if (n && peer(m, PEER_RD, a[0], n * sizeof(struct dpykey)) < 0)
            return -EFAULT;
        memcpy(grabs, (const void *)a[0], n * sizeof(struct dpykey));
        ngrabs = (int)n;
        return 0;
    }
    }
    if (s < 0)
        return m->type == DPY_WM_FOCUS && !a[0] ? (win_focus(-1), 0) : -ENOENT;
    switch (m->type) {
    case DPY_WM_PLACE:
        win_place(s, (s16)(a[1] >> 16), (s16)(a[1] & 0xFFFF), (int)(a[2] >> 16), (int)(a[2] & 0xFFFF));
        return 0;
    case DPY_WM_STACK:
        win_stack(s, a[1] != 0);
        return 0;
    case DPY_WM_FOCUS:
        win_focus(s);
        return 0;
    case DPY_WM_SHOW:
        wins[s].placed = 1;
        win_show(s, 1, 0);
        return 0;
    case DPY_WM_HIDE:
        win_show(s, 0, (int)a[1]);
        return 0;
    case DPY_WM_DRAG:
        if (dragging || fpress.win != wins[s].id)
            return -EINVAL;             /* a drag runs, or no press on this window's frame to follow */
        win_drag(s, (int)a[1], fpress.x, fpress.y);
        if (fpress.up)                  /* let go already: the whole drag at once */
            win_drag_event(fpress.ux, fpress.uy, 1);
        else if (ptx != fpress.x || pty != fpress.y)
            win_drag_event(ptx, pty, 0);
        fpress.win = 0;
        return 0;
    case DPY_WM_CLOSE:
        ev_win(s, EV_CLOSE, 0, 0, 0, 0, 0, 0);
        return 0;
    case DPY_WM_MENUGET: {
        u32 n = wins[s].nmenu < a[2] ? wins[s].nmenu : a[2];
        if (n && peer(m, PEER_WR, a[1], n) < 0)
            return -EFAULT;
        if (n)
            memcpy((void *)a[1], wins[s].menu, n);
        return (int)n;
    }
    case DPY_WM_MENUPICK:
        if (a[1] > 255 || a[2] > 0xFFFF)
            return -EINVAL;
        ev_win(s, EV_MENU, (int)a[1], (int)a[2], 0, 0, 0, 0);
        return 0;
    case DPY_WM_PGRAB:
        if (!btns || wins[s].cl != cl)
            return -EINVAL;
        if (grab_w >= 0 && grab_w != s) {       /* the window the button went to sees it let go */
            struct dpyev v;
            memset(&v, 0, sizeof v);
            v.type = EV_BUTTON;
            v.win = wins[grab_w].id;
            for (int i = 0; i < 3; i++)
                if (btns & bit[i]) {
                    v.code = (u8)((i + 1) | 0x80);
                    ev_to(wins[grab_w].cl, &v);
                }
            grab_w = -1;
        }
        pgrab_w = s;
        pgrab_id = wins[s].id;
        return 0;
    case DPY_WM_KILL: {
        int id = wins[s].id;
        win_close(s);
        ev_wm(EV_WM_GONE, id, 0, 0, 0, 0, 0, 0);
        return 0;
    }
    }
    return -ENOSYS;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-persist"))
            persist = 1;
    port = sys_port_create(DPY_PORTNAME);
    if (port < 0) {
        dprintf(2, "displayd: the port %s: %s\n", DPY_PORTNAME, zwhy("busy"));
        return 1;
    }
    int had = 0;
    u32 alive_at = sys_ticks();
    for (;;) {
        struct pollfd p[2] = { { port, POLLPORT | POLLIN, 0 }, { gfd, POLLIN, 0 } };
        sys_evwait(p, gfd >= 0 ? 2 : 1, 50);
        if (gfd >= 0)
            kernel_events();
        struct umsg *m;
        while ((m = sys_recv(port, 0)) && (int)m != -1) {
            int r = req(m);
            sys_reply(m, r);
        }
        if ((u32)(sys_ticks() - alive_at) >= 100) {     /* once a second: a client that ended without DPY_BYE */
            alive_at = sys_ticks();
            u8 live[NCL] = { 0 };       /* a client that ended (a zombie whose parent has not waited counts as ended) */
            struct uproc pr;
            for (u32 k = 0; k < 64; k++)
                if (sys_procinfo(k, &pr) == 0 && pr.state && pr.state != 5)
                    for (int i = 0; i < NCL; i++)
                        if (cls[i].used && cls[i].pid == (int)pr.pid)
                            live[i] = 1;
            for (int i = 0; i < NCL; i++)
                if (cls[i].used && !live[i])
                    drop(i);
        }
        for (int i = 0; i < NCL; i++)
            flush(i);
        if (has_clients())
            had = 1;
        else if (had && !persist)
            break;
    }
    blit_idle();
    if (gfd >= 0)
        sys_close(gfd);                 /* the console is text again */
    return 0;
}
