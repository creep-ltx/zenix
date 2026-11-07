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
#include "../screenfile.h"

int aga;
int wm_cl = -1;
static int port, gfd = -1, confd = -1, persist, grabbed, grabbed_con;
u32 ndraws;
/* the Copper list (chip RAM): its fixed part twice, one a field of an interlaced screen (the long field's, the short
 * field's: D38 "Screen modes"; without interlace the first alone runs), each jumping to its field's changing part */
static u16 *copl[2];
static int cop_col, cop_col2;           /* the index of COLOR00's value in them; of playfield 2's first (AGA) */
static u16 *dyn[2][2], *cop_jmp[2];     /* the changing part: two buffers, each a list a field; COP2LCL's values */
static int dyn_cur;                     /* the buffer the Copper is told to use */
int lace;                               /* the screen is interlaced: a field shows every other line */
int frame_l = 312;                      /* a frame's lines: 312 PAL, 262 NTSC (a long field one more) */
static int base_w, base_h;              /* the console's size (640 x 256 PAL, 640 x 200 NTSC): a field's */
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
    int used, pid, port, uid, dead;     /* dead: a post failed; dropped from the main loop, never mid-paint */
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
        if (!c->used || c->dead || c->npend >= NPEND) {
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
/* a whole drag's exposes not answered yet (a bit a client) and since when: what is drawn off the screen is shown when
 * they have drawn (their next DPY_DRAW), or after 60 ms, so a frame never shows the part cleared and not yet drawn */
static u32 expose_wait, expose_t;
void ev_win(int s, int type, int code, int arg, int x, int y, int w, int h)
{
    struct win *wi = &wins[s];
    struct dpyev e;
    memset(&e, 0, sizeof e);
    e.type = (u8)type; e.code = (u8)code; e.win = wi->id; e.arg = (u16)arg;
    e.x = (s16)x; e.y = (s16)y; e.w = (u16)w; e.h = (u16)h;
    e.qual = btns;
    ev_to(wi->cl, &e);
    if (type == EV_EXPOSE && shadowing) {       /* a whole drag uncovered a part its program draws: shown with it */
        if (!expose_wait)
            expose_t = sys_hrtime();
        expose_wait |= 1u << wi->cl;
    }
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
    if (!c->used || c->dead || (!c->npend && !c->lost))
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
        c->dead = 1;                    /* its port or its ring is gone: so is the client (dropped in the loop) */
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
    int fh = font_h(font_id(DPYF_UI));  /* a taller desktop font: the title bar as much taller (as zdesk's themes) */
    if (fh > 8) {
        style.top = (u8)(style.top + fh - 8);
        style.title = (u8)(style.title + fh - 8);
    }
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
static int pf2_pen = -1;                /* a lifted window's stand-in for pen 0 (D38 "Dragging with the chips") */
static void set_palette(void)
{
    for (int f = 0; f < 2; f++) {
        for (int i = 0; i < (1 << scr.depth); i++)
            copl[f][cop_col + 2 * i + 1] = style.palette[i] & 0xFFF;
        if (cop_col2)
            for (int i = 0; i < 16; i++)
                copl[f][cop_col2 + 2 * i + 1] = style.palette[i == pf2_pen ? 0 : i] & 0xFFF;
    }
}

/* ---- the Copper list's changing part (D38 "Dragging with the chips") ----
 * dyn_build writes the list for the screen alone (lift 0) or with a lifted window in playfield 2: its bitmap's first
 * line of plane 0 at lift->p, a plane's line lift->bpr bytes (the window's bytes, then 88 zero bytes), placed with
 * its column 0 at screen column hx (a multiple of 8) and its line 0 at hy, lift->h lines; zero: 88 zero bytes. */
static const struct bm *lift;
static int hx, hy;
static const u8 *zero88;
static void dyn_build(u16 *c, int on, int f)    /* f: the field (0 long: lines 0, 2...; 1 short: 1, 3...) */
{
#define MOVE(r, v) do { *c++ = (u16)(r); *c++ = (u16)(v); } while (0)
    int depth = scr.depth, f3 = aga, wrapped = 0;
    u32 fo = lace && f ? scr.stride : 0;    /* the short field's planes start a line further in */
    u16 lm = (u16)(lace ? scr.stride : 0);  /* and each field passes over the other's lines */
    if (!on) {
        MOVE(0x100, 0x8200 | depth << 12 | lace << 2);  /* BPLCON0: hires (LACE: interlaced) */
        MOVE(0x102, 0);
        MOVE(0x104, 0x0024);                /* BPLCON2: the sprites in front of the planes */
        MOVE(0x108, scr.stride - scr.bpr + lm);
        MOVE(0x10A, scr.stride - scr.bpr + lm);
        MOVE(0x092, f3 ? 0x38 : 0x3C);      /* the console's fetch window (kernel/gfx.c) */
        MOVE(0x094, f3 ? 0xC0 : 0xD4);
        for (int k = 0; k < depth; k++) {
            u32 p = (u32)scr.p + (u32)k * scr.bpr + fo;
            MOVE(0x0E0 + 4 * k, p >> 16);
            MOVE(0x0E2 + 4 * k, p);
        }
    } else {
        /* playfield 2 placed: the pointer in 64-pixel steps (8-byte aligned for the 64-bit fetch), BPLCON1's delay the
         * rest. One more fetch unit on the left (DDFSTRT $28): the first unit lies left of the display window */
        int sft = hx & 63;
        const u8 *p2 = lift->p - 8 * (((hx - sft) >> 6) + 1);
        int d = sft * 2;                    /* the delay in 35 ns steps: bits 2-5 to 4-7, 0-1 to 12-13, 6-7 to 14-15 */
        MOVE(0x100, 0x8610 | lace << 2);    /* BPLCON0: hires, dual playfield, 8 planes */
        MOVE(0x102, (d & 0x3C) << 2 | (d & 3) << 12 | (d & 0xC0) << 8);
        MOVE(0x104, 0x0064);                /* BPLCON2: playfield 2 in front, the sprites in front of both */
        MOVE(0x108, scr.stride - 88 + lm);
        MOVE(0x10A, (u16)-88);
        MOVE(0x092, 0x28);
        MOVE(0x094, 0xC0);
        for (int k = 0; k < 4; k++) {
            u32 p = (u32)scr.p + (u32)k * scr.bpr - 8 + fo, z = (u32)zero88;
            MOVE(0x0E0 + 8 * k, p >> 16);   /* playfield 1: planes 1, 3, 5, 7 */
            MOVE(0x0E2 + 8 * k, p);
            MOVE(0x0E4 + 8 * k, z >> 16);   /* playfield 2: 2, 4, 6, 8, the zero line until the window */
            MOVE(0x0E6 + 8 * k, z);
        }
        for (int e = 0; e < 2; e++) {
            int y = e ? hy + lift->h : hy;
            if (lace && (y & 1) != f)
                y++;                        /* the field's first line at or below it */
            if (y >= scr.h)
                break;
            int v = DIW_V + (y >> lace) - 1;    /* the line above: after its fetch, before the next one's */
            if (v >= 256 && !wrapped) {
                *c++ = 0xFFDF; *c++ = 0xFFFE;
                wrapped = 1;
            }
            *c++ = (u16)((v & 0xFF) << 8 | 0xD9);
            *c++ = 0xFFFE;
            for (int k = 0; k < 4; k++) {
                u32 p = e ? (u32)zero88 : (u32)p2 + (u32)k * lift->bpr + (u32)(y - hy) * lift->stride;
                MOVE(0x0E4 + 8 * k, p >> 16);
                MOVE(0x0E6 + 8 * k, p);
            }
            MOVE(0x10A, e ? (u16)-88 : (u16)(lift->stride - 88 + (lace ? lift->stride : 0)));
        }
    }
    if (lace) {
        /* the next field's list, kept right by the Copper: the long field's after a short one; only a long field has
         * line frame_l (312 PAL, 262 NTSC), and there the short field's is named instead */
        MOVE(0x080, (u32)copl[0] >> 16);
        MOVE(0x082, (u32)copl[0]);
        if (!wrapped) {
            *c++ = 0xFFDF; *c++ = 0xFFFE;
        }
        *c++ = (u16)((frame_l & 0xFF) << 8 | 0x01);
        *c++ = 0xFFFE;
        MOVE(0x080, (u32)copl[1] >> 16);
        MOVE(0x082, (u32)copl[1]);
    }
    *c++ = 0xFFFF;
    *c++ = 0xFFFE;
#undef MOVE
}
/* the other buffer's lists written, then the words that name them (both fields'): the Copper takes them at the top
 * of the next frame (in interlace a field). Before writing again the frame after the last change must have begun
 * (the Copper may still run the old lists until then) */
static u32 flip_t;
static int flip_line = -1;
static void dyn_settle(void)
{
    if (flip_line < 0)
        return;
    u32 need = (u32)(frame_l - flip_line + 3) * 46u;   /* lines of 64 us in E-clock ticks (709,379 Hz PAL, 715,909 NTSC) */
    while ((u32)(sys_hrtime() - flip_t) < need)
        ;
    flip_line = -1;
}
static void dyn_flip(int on)
{
    dyn_settle();
    int b = dyn_cur ^ 1;
    for (int f = 0; f < 2; f++)
        dyn_build(dyn[b][f], on, f);
    flip_line = dpy_beam() + DIW_V;
    flip_t = sys_hrtime();
    for (int f = 0; f < 2; f++)
        *cop_jmp[f] = (u16)(u32)dyn[b][f];
    dyn_cur = b;
}
int hw_ok(void) { return aga && use_blit && cop_col2 && scr.depth == 4; }
void hw_show(const struct bm *b, const u8 *zero, int x, int y, int pen)
{
    if (pen != pf2_pen) {
        pf2_pen = pen;
        set_palette();
    }
    lift = b;
    zero88 = zero;
    hx = x;
    hy = y;
    dyn_flip(1);
}
void hw_off(void)
{
    dyn_flip(0);
    dyn_settle();                       /* in use now: the window's bitmap may go */
    lift = 0;
}
void hw_wait(void) { dyn_settle(); }

/* the pointer, 16 x 16 lores pixels: o the outline (colour 17), w the fill (18) */
static const char *const arrow[16] = {
    "o...............", "oo..............", "owo.............", "owwo............", "owwwo...........",
    "owwwwo..........", "owwwwwo.........", "owwwwwwo........", "owwwwwwwo.......", "owwwwwoooo......",
    "owwowwo.........", "owo.owwo........", "oo..owwo........", "o....owwo.......", ".....owwo.......",
    "......oo........",
};

/* ---- screen modes (D38 "Screen modes"): hires or hires interlaced, PAL or NTSC as the console is ---- */
#define LACE_SPARE (512u << 10)         /* chip RAM left after an interlaced screen: drags, copies, sound, disks */
/* /etc/zdesk/screen (display/lib/screenfile.c): its mode -> DPYM_LACE or DPYM_HIRES (no file, no line, auto: hires);
 * its "font NAME SIZE" (the desktop's font, docs/d38-desktop.md "Fonts") into font_name and font_size (none: "", 0) */
static char font_name[32];
static int font_size;
static int mode_file(void)
{
    struct dpyscreen sf;
    dpy_screenfile("/etc/zdesk/screen", &sf, "displayd");
    strcpy(font_name, sf.font);
    font_size = sf.size;
    return sf.mode;
}
/* the screen's bitmap for a mode: 0, -ENOMEM (no chip RAM; interlace also when less than LACE_SPARE would be left) */
static int mode_alloc(struct bm *b, int want, int depth)
{
    u32 need = (u32)(base_h << want) * (u32)(base_w / 8) * (u32)depth, mi[4];
    if (want && sys_meminfo(mi) == 0 && mi[0] < need + LACE_SPARE) {
        dprintf(2, "displayd: interlace needs %u KB of chip RAM, %u KB free: hires\n", (need + LACE_SPARE) >> 10,
                mi[0] >> 10);
        return -ENOMEM;
    }
    u32 was = chip_used;
    if (bm_alloc(b, base_w, base_h << want, depth) < 0)
        return -ENOMEM;
    b->kept = 0;                        /* the screen is not a kept copy */
    chip_used = was;
    return 0;
}

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
    copl[0] = mem + 64;                 /* the fixed part: at most 64 moves each */
    copl[1] = copl[0] + 128;
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
    g.cop = (u32)copl[0];
    g.sprh = 16;
    /* the Copper list is built for the screen's size; the size is the console's: 640 x 256 PAL, 640 x 200 NTSC
     * (doubled in interlace) - asked with a list that is only its end, then the real one written in place before the
     * hand-over takes effect (the Copper reads it from the next frame) */
    copl[0][0] = 0xFFFF; copl[0][1] = 0xFFFE;
    gfd = sys_congrab(confd, &g);
    if (gfd < 0) {
        sys_close(confd);
        sys_memfree(mem);
        return gfd;
    }
    aga = g.chips & GCHIP_AGA;
    int depth = aga ? 4 : 3;
    base_w = g.width;
    base_h = g.height;
    frame_l = base_h > 200 ? 312 : 262;
    lace = mode_file() == DPYM_LACE;
    if (lace && mode_alloc(&scr, 1, depth) < 0)
        lace = 0;
    if (!lace && mode_alloc(&scr, 0, depth) < 0) {  /* no chip RAM: the console given back, nothing kept */
        sys_close(gfd);
        gfd = -1;
        sys_close(confd);
        sys_memfree(mem);
        return -ENOMEM;
    }
    chip_used = 0;
    grabbed = 1;
    font_init(topaz);
    if (font_name[0])
        font_ui_load(font_name, font_size);
    /* the fixed part of the Copper list; the part that changes (the planes, and playfield 2 while a window is dragged
     * by the chips) is one of two buffers of a list a field in dynmem, jumped to with COP2LC and COPJMP2 (D38
     * "Dragging with the chips", "Screen modes") */
    u16 *dm = sys_memalloc_chip(1024 + 1024);
    if (!dm) {
        sys_close(gfd);
        gfd = -1;
        sys_close(confd);
        sys_memfree(mem);
        bm_free(&scr);
        return -ENOMEM;
    }
    u16 *d0 = (u16 *)(((u32)dm + 1023) & ~1023u);  /* an aligned 1 KB block never crosses 64 KB: one word flips */
    for (int b = 0; b < 2; b++)
        for (int f = 0; f < 2; f++)
            dyn[b][f] = d0 + 128 * (2 * b + f);
    for (int f = 0; f < 2; f++) {
        u16 *c = copl[f];
        int f3 = aga;
#define MOVE(r, v) do { *c++ = (u16)(r); *c++ = (u16)(v); } while (0)
        if (aga) {
            MOVE(0x1FC, f3 ? 3 : 0);    /* FMODE: 64-bit bitplane fetch, 16-pixel sprites (gcon.c's contract) */
            MOVE(0x106, 0x1400);        /* BPLCON3: bank 0, playfield 2's colours from 32 (PF2OF) */
            MOVE(0x10C, 0x0011);        /* BPLCON4: the sprites' colours 16-31 */
        }
        cop_col = (int)(c - copl[f]);
        for (int i = 0; i < (1 << depth); i++)
            MOVE(0x180 + 2 * i, 0);
        MOVE(0x1A2, 0x000);             /* the pointer: black outline, white face */
        MOVE(0x1A4, 0xFFF);
        MOVE(0x1A6, 0x888);
        if (aga) {                      /* playfield 2's colours (32-47): bank 1 */
            MOVE(0x106, 0x3400);
            cop_col2 = (int)(c - copl[f]);
            for (int i = 0; i < 16; i++)
                MOVE(0x180 + 2 * i, 0);
            MOVE(0x106, 0x1400);
        }
        MOVE(0x120, (u32)spr >> 16);
        MOVE(0x122, (u32)spr);
        for (int i = 1; i < 8; i++) {
            MOVE(0x120 + 4 * i, (u32)nul >> 16);
            MOVE(0x122 + 4 * i, (u32)nul);
        }
        MOVE(0x084, (u32)dyn[0][f] >> 16);  /* COP2LC: the field's list in use; the low word is the one a change writes */
        cop_jmp[f] = c + 1;
        MOVE(0x086, (u32)dyn[0][f]);
        MOVE(0x08A, 0);                 /* COPJMP2 */
#undef MOVE
        dyn_build(dyn[0][f], 0, f);
    }
    set_palette();
    ptx = g.width / 2;
    pty = scr.h / 2;
    return 0;
}

/* DPY_MODE: the screen in another mode now (a preferences program's Test, Use and Cancel; D38 "Screen modes"): a new
 * screen allocated, every window kept on it and drawn again there (the kept copies given up: the programs draw again,
 * EV_EXPOSE; the desktop's window the new size, EV_RESIZE), the Copper pointed at it, the old one freed, every
 * client told (EV_SCREEN) -> the mode now, or -EBUSY (a drag under way), -ENOMEM (no chip RAM for it: as it was) */
static int mode_switch(int want)
{
    if (want == lace)
        return want;
    if (dragging || shadowing || !use_blit)
        return -EBUSY;
    struct bm nb, old = scr;
    if (mode_alloc(&nb, want, scr.depth) < 0)
        return -ENOMEM;
    blit_idle();
    int was = lace;
    scr = nb;
    lace = want;
    pty = pty >> was << lace;
    struct dpyev e;                     /* told first: their resizes and exposes after it see the new size */
    memset(&e, 0, sizeof e);
    e.type = EV_SCREEN;
    e.arg = (u16)lace;
    e.w = scr.w;
    e.h = scr.h;
    for (int i = 0; i < NCL; i++)
        if (cls[i].used)
            ev_to(i, &e);
    win_screen();                       /* drawn into the new screen while the old one is shown */
    blit_idle();
    dyn_flip(0);
    dyn_settle();                       /* the Copper shows the new one: the old may go */
    bm_free(&old);
    dprintf(1, "displayd: screen %ux%u%s\n", scr.w, scr.h, lace ? " interlaced" : "");
    return lace;
}

/* DPY_FONT_SET: the desktop's font changed now (a preferences program's Test, Use and Cancel; D38 "Preferences"):
 * loaded (a font that cannot be read: said, the one in use stays), the server's own style's title bar its height
 * when no window manager runs, every client told (EV_SCREEN with DPYS_FONT: zdesk takes the new height for its bar
 * and its style), every window drawn again -> 0, or -EBUSY (a drag under way), -ENOENT, -EINVAL, -ENOMEM */
static int font_switch(const char *name, int size)
{
    if (dragging || shadowing)
        return -EBUSY;
    int r = 0;
    if (name[0])
        r = font_ui_load(name, size);
    else
        font_ui_drop();
    if (r < 0)
        return r;
    if (wm_cl < 0)
        default_style();
    struct dpyev e;
    memset(&e, 0, sizeof e);
    e.type = EV_SCREEN;
    e.code = DPYS_FONT;
    e.arg = (u16)lace;
    e.w = scr.w;
    e.h = scr.h;
    for (int i = 0; i < NCL; i++)
        if (cls[i].used)
            ev_to(i, &e);
    blit_idle();
    win_screen();
    if (!name[0])
        dprintf(1, "displayd: font 8x8\n");     /* a font that was read said so itself (font_ui_load) */
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
        if (!down && which == 1) {
            drag_moved = 0;
            win_drag_event(ptx, pty, 1);
        }
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
    int moved = e->x != ptx || e->y << lace != pty;
    ptx = e->x;
    pty = e->y << lace;                 /* the kernel's pointer is in frame lines: two lines a frame line in interlace */
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
            drag_moved = 1;             /* one move a pass of the main loop, to where the pointer is by then */
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
                shadow_end();           /* (a drag goes on drawing on the screen itself) */
                blit_idle();
                use_blit = 0;
                sys_congack(gfd, (u16)e->x);
                break;
            case GEV_ACQUIRE:           /* every key counts as up; the screen was never touched */
                use_blit = 1;
                ptx = e->x;
                pty = e->y << lace;
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
int drag_moved;

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
static void wm_gone(void)              /* the manager gone: the server places and shows by itself (D38 3.3) */
{
    int cl = wm_cl;
    wm_cl = -1;
    for (int k = 0; k < NWIN; k++)
        if (wins[k].id && wins[k].cl != cl && !(wins[k].state & WST_SHOWN))
            win_show(k, 1, 0);
}
static void drop(int cl)
{
    struct client *c = &cls[cl];
    if (!c->used)
        return;
    expose_wait &= ~(1u << cl);
    c->used = 0;
    if (wm_cl == cl)
        wm_gone();
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
        grabbed_con = con;
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
        ndraws++;
        if ((a[1] & 1) || (a[2] & 1) || a[2] > 65536 || peer(m, PEER_RD, a[1], a[2]) < 0)
            return -EFAULT;
        u32 bad = ~0u;
        expose_wait &= ~(1u << cl);
        drag_hide();
        win_draw((int)(w - wins), (const u16 *)a[1], a[2] / 2, &bad);
        drag_show();
        if (bad != ~0u) {
            a[3] = bad;
            return -EINVAL;
        }
        return 0;
    }
    case DPY_FONT: {                    /* a[0] 0: the desktop's font; any name: the 8 x 8 (the terminals') */
        struct dpyfont fi;
        font_info(a[0] ? 0 : DPYF_UI, &fi);
        return (int)((u32)fi.id << 16 | (u32)(fi.xsize & 0xFF) << 8 | fi.height);
    }
    case DPY_FONT_INFO: {
        _Static_assert(sizeof(struct dpyfont) == DPYFONT_FIRST, "struct dpyfont is frozen at 304 bytes");
        struct dpyfont *d = (struct dpyfont *)a[1];
        if (a[0] > DPYF_UI)
            return -EINVAL;
        if (peer(m, PEER_WR, a[1], DPYFONT_FIRST) < 0 || d->size < DPYFONT_FIRST)
            return -EFAULT;
        struct dpyfont fi;
        font_info((int)a[0], &fi);
        fi.size = d->size;
        memcpy(d, &fi, DPYFONT_FIRST);
        return 0;
    }
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
    case DPY_STAT: {
        int nw = a[1] & 2 ? 12 : 6;
        if (peer(m, PEER_WR, a[0], (u32)nw * 4) < 0)
            return -EFAULT;
        u32 *o = (u32 *)a[0];
        o[0] = nblits; o[1] = npix; o[2] = nglyphs; o[3] = ndraws; o[4] = npaints; o[5] = ncopies;
        if (nw > 6) {
            o[6] = nmoves; o[7] = tmoves; o[8] = nshows; o[9] = tshows; o[10] = twaits; o[11] = nlate;
        }
        if (a[1] & 1)
            nblits = npix = nglyphs = ndraws = npaints = ncopies = nmoves = tmoves = nshows = tshows = twaits =
                nlate = 0;
        return 0;
    }
    case DPY_MODE:                      /* any client of the desktop's session (a preferences program) */
        if (a[0] == DPYM_ASK)
            return lace;
        if (a[0] > DPYM_LACE)
            return -EINVAL;
        return mode_switch((int)a[0]);
    case DPY_FONT_SET: {                /* the same clients: a[0] 32 bytes, the name (0: the 8 x 8), a[1] the size */
        char nm[32];
        nm[0] = 0;
        if (a[0]) {
            if (peer(m, PEER_RD, a[0], sizeof nm) < 0)
                return -EFAULT;
            memcpy(nm, (const void *)a[0], sizeof nm);
            if (nm[sizeof nm - 1] || !nm[0] || a[1] < 1 || a[1] > 255)
                return -EINVAL;
        }
        return font_switch(nm, (int)a[1]);
    }
    case DPY_SESSION:
        if (m->uid != 0 || cls[cl].uid != 0)
            return -EPERM;
        if (sys_peer(m, PEER_CTTY, 0) != grabbed_con)
            return -EBUSY;              /* a login manager of another console: this desktop is not its */
        if (a[0] > 0xFFFF)
            return -EINVAL;
        session_uid = (int)a[0];
        for (int i = 0; i < NCL; i++)   /* the last session's programs leave with it (no fake greeter over dlogin's) */
            if (cls[i].used && cls[i].uid != 0 && cls[i].uid != session_uid)
                drop(i);
        return 0;
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
    if (m->type == DPY_WM_MOUSE)
        return sys_conmouse(gfd, a[0]);
    if (m->type == DPY_WM_BYE) {
        wm_gone();
        return 0;
    }
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
        /* a whole drag (D38 6.3): one move a pass, to where the mouse is by then; what changed shown at once, timed
         * to the beam - unless a program still has to draw a part the move uncovered (then the next pass) */
        if (expose_wait && (!shadowing || (u32)(sys_hrtime() - expose_t) >= 42000u))
            expose_wait = 0;
        if (!expose_wait) {
            if (drag_moved) {
                drag_moved = 0;
                if (dragging)
                    win_drag_event(ptx, pty, 0);
            }
            if (shadowing && !expose_wait) {
                if (dragging)
                    shadow_show();
                else
                    shadow_end();
            }
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
        for (int i = 0; i < NCL; i++) {
            flush(i);
            if (cls[i].used && cls[i].dead)
                drop(i);
        }
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
