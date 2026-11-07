/* SPDX-License-Identifier: GPL-2.0-only */
/* displayd's windows (DESIGN D38 3.2-3.3, 6): stacking, the visible region of each, frames drawn from the manager's
 * style, and the kept copies.
 *
 * Smart refresh, as this server keeps it: a window that is not wholly on the screen keeps a copy of its inside in chip
 * RAM, so raising, uncovering or moving it is a copy and never asks its program to draw again. Its drawing goes to
 * the screen where it is visible and to the copy (a window wholly on the screen keeps none, and its drawing goes only
 * to the screen). The copy is taken from the screen at the moment the window starts to be covered, while it was still
 * whole, and freed when it is whole on the screen again. When the copies would pass the budget (256 KB, D38 6.2) a
 * window becomes simple refresh: what is uncovered is cleared and its program gets EV_EXPOSE for it. A window hidden
 * for a workspace gives its copy up (D38 revision 2).
 *
 * Every change (a new window, close, move, resize, raise, hide, show) goes through change(): the visible regions are
 * worked out again, copies taken where windows start to be covered, then what changed is painted back to front (the
 * background, each window's shadow, frame and inside), each pixel once. */
#include "dpyd.h"

struct win wins[NWIN];
u16 order[NWIN];
int norder;
struct dpystyle style;
int focus = -1;
int dragging;
static int dkeep = -1;                  /* a window moved whole: its kept copy stays for the drag (its inside's source) */
u32 nmoves, tmoves;
static u16 next_id = 1;
static void lift_end(void);

/* ---- geometry ---- */
static int framed(const struct win *w) { return w->kind != WK_BAR && w->kind != WK_DESKTOP; }
static int bl(const struct win *w) { return framed(w) ? style.left : 0; }
static int br(const struct win *w) { return framed(w) ? style.right : 0; }
static int bt(const struct win *w) { return framed(w) ? style.top : 0; }
static int bb(const struct win *w) { return framed(w) ? style.bottom : 0; }
static int sw(const struct win *w) { return framed(w) ? style.shadow : 0; }
void win_frame_rect(const struct win *w, struct rect *r)
{
    r->x0 = w->fx;
    r->y0 = w->fy;
    r->x1 = (s16)(w->fx + bl(w) + w->iw + br(w));
    r->y1 = (s16)(w->fy + bt(w) + w->ih + bb(w));
}
void win_inside(const struct win *w, struct rect *r)
{
    r->x0 = (s16)(w->fx + bl(w));
    r->y0 = (s16)(w->fy + bt(w));
    r->x1 = (s16)(r->x0 + w->iw);
    r->y1 = (s16)(r->y0 + w->ih);
}
/* the rectangles a window covers: the frame and the shadow's two strips */
static int outer(const struct win *w, struct rect *o)
{
    win_frame_rect(w, &o[0]);
    int s = sw(w), n = 1;
    if (s) {
        o[1] = (struct rect){ o[0].x1, (s16)(o[0].y0 + s), (s16)(o[0].x1 + s), (s16)(o[0].y1 + s) };
        o[2] = (struct rect){ (s16)(o[0].x0 + s), o[0].y1, o[0].x1, (s16)(o[0].y1 + s) };
        n = 3;
    }
    return n;
}
static void bounds(const struct win *w, struct rect *b)
{
    win_frame_rect(w, b);
    b->x1 = (s16)(b->x1 + sw(w));
    b->y1 = (s16)(b->y1 + sw(w));
}
static int shown(const struct win *w) { return w->id && (w->state & WST_SHOWN) && !w->lifted; }
struct win *win_get(int id)
{
    for (int i = 0; i < NWIN; i++)
        if (wins[i].id && wins[i].id == id)
            return &wins[i];
    return 0;
}

/* ---- the visible regions ---- */
static void calc_vis(void)
{
    for (int k = 0; k < norder; k++) {
        struct win *w = &wins[order[k]];
        w->vis.n = 0;
        if (!shown(w))
            continue;
        struct rect o[3];
        int n = outer(w, o);
        for (int i = 0; i < n; i++)
            rg_add(&w->vis, &o[i]);
        for (int j = k + 1; j < norder; j++) {
            struct win *a = &wins[order[j]];
            if (!shown(a))
                continue;
            struct rect ao[3];
            int an = outer(a, ao);
            for (int i = 0; i < an; i++)
                rg_sub(&w->vis, &ao[i]);
        }
        struct rect all = { 0, 0, (s16)scr.w, (s16)scr.h };
        rg_and(&w->vis, &all);
    }
}
/* the inside wholly visible on the screen */
static int whole(const struct win *w, const struct region *vis)
{
    struct rect in;
    win_inside(w, &in);
    if (in.x0 < 0 || in.y0 < 0 || in.x1 > scr.w || in.y1 > scr.h)
        return 0;
    struct region g;
    rg_set(&g, &in);
    for (int i = 0; i < vis->n; i++)
        rg_sub(&g, &vis->r[i]);
    return g.n == 0 && !g.over;         /* pieces dropped: not known to be whole */
}

/* ---- painting ---- */
static int gad_x(int i, const struct rect *f)
{
    int g = style.gadx[i];
    return g >= 128 ? f->x1 - (g - 128) : f->x0 + g;
}
/* the frame of w clipped to c (the shadow too) */
static void frame_paint(struct win *w, const struct rect *c)
{
    struct rect f, in, k, t;
    int act = (int)(w - wins) == focus;
    win_frame_rect(w, &f);
    win_inside(w, &in);
    struct rect o[3];
    int n = outer(w, o);
    for (int i = 1; i < n; i++)
        if (r_and(&k, &o[i], c))
            bm_fill(&scr, &k, style.shadowc);
    /* the border: the frame minus the inside */
    struct region b;
    rg_set(&b, &f);
    rg_sub(&b, &in);
    for (int i = 0; i < b.n; i++)
        if (r_and(&k, &b.r[i], c))
            bm_fill(&scr, &k, style.border[act]);
    if (style.bevel) {
        struct rect l1 = { (s16)(f.x0 + 1), (s16)(f.y0 + 1), (s16)(f.x1 - 1), (s16)(f.y0 + 2) };
        struct rect l2 = { (s16)(f.x0 + 1), (s16)(f.y0 + 1), (s16)(f.x0 + 2), (s16)(f.y1 - 1) };
        struct rect d1 = { (s16)(f.x0 + 1), (s16)(f.y1 - 2), (s16)(f.x1 - 1), (s16)(f.y1 - 1) };
        struct rect d2 = { (s16)(f.x1 - 2), (s16)(f.y0 + 1), (s16)(f.x1 - 1), (s16)(f.y1 - 1) };
        if (r_and(&k, &l1, c)) bm_fill(&scr, &k, style.light);
        if (r_and(&k, &l2, c)) bm_fill(&scr, &k, style.light);
        if (r_and(&k, &d1, c)) bm_fill(&scr, &k, style.dark);
        if (r_and(&k, &d2, c)) bm_fill(&scr, &k, style.dark);
    }
    if (style.title && framed(w)) {
        int tx0 = f.x0 + 1, tx1 = f.x1 - 1, ty0 = f.y0 + 1, ty1 = f.y0 + 1 + style.title - 1;
        t = (struct rect){ (s16)tx0, (s16)ty0, (s16)tx1, (s16)ty1 };
        if (r_and(&k, &t, c)) {
            /* the title between the gadgets: after the left ones and a gap, before the right ones (a long one cut) */
            int lx = tx0 + 5, rx = tx1 - 4;
            for (int i = 0; i < 3; i++) {
                if (!style.gadx[i])
                    continue;
                struct bm *gi = 0;
                if (style.gad[i])
                    dpy_image(style.gad[i], -1, &gi);
                int gx = gad_x(i, &f), gw = gi ? gi->w : 8;
                if (style.gadx[i] < 128 && gx + gw + 6 > lx) lx = gx + gw + 6;
                if (style.gadx[i] >= 128 && gx - 4 < rx) rx = gx - 4;
            }
            /* the desktop's font (the 8 x 8 without one): a long title cut after its last whole character */
            int tf = font_id(DPYF_UI), fh = font_h(tf), tw;
            u32 tn = font_fit(tf, (const u8 *)w->title, strlen(w->title), rx - lx > 0 ? rx - lx : 0, &tw);
            int x = style.centre ? (tx0 + tx1 - tw) / 2 : lx;
            if (x < lx) x = lx;
            if (x + tw > rx) x = rx - tw;
            int y = ty0 + (style.title - 1 - fh) / 2, gy = ty0 + (style.title - 1 - 8) / 2;
            if (y < ty0) y = ty0;
            if (gy < ty0) gy = ty0;
            int patch = style.centre || (act && style.stripes);    /* the title on a plain patch over the stripes */
            int ppen = style.bar[0] == style.bar[1] || !style.stripes ? style.bar[act] : style.bar[0];
            /* every pixel once: the bar (and its stripes) beside the title, the patch around it, then the text */
            int px0 = patch ? x - 4 : x, px1 = patch ? x + tw + 4 : x + tw;
            for (int side = 0; side < 2; side++) {
                int a0 = side ? px1 : tx0, a1 = side ? tx1 : px0;
                if (a0 >= a1)
                    continue;
                struct rect s = { (s16)a0, (s16)ty0, (s16)a1, (s16)ty1 }, ks;
                if (!(act && style.stripes)) {
                    if (r_and(&ks, &s, &k)) bm_fill(&scr, &ks, style.bar[act]);
                    continue;
                }
                for (int yy = ty0; yy < ty1; yy++) {
                    struct rect l = { (s16)a0, (s16)yy, (s16)a1, (s16)(yy + 1) };
                    if (r_and(&ks, &l, &k))
                        bm_fill(&scr, &ks, (yy - ty0) & 1 ? style.stripes : style.bar[act]);
                }
            }
            struct rect pl = { (s16)px0, (s16)ty0, (s16)x, (s16)ty1 }, pr = { (s16)(x + tw), (s16)ty0, (s16)px1, (s16)ty1 },
                        tt = { (s16)x, (s16)ty0, (s16)(x + tw), (s16)y }, tb = { (s16)x, (s16)(y + fh), (s16)(x + tw), (s16)ty1 },
                        kp;
            if (r_and(&kp, &pl, &k)) bm_fill(&scr, &kp, ppen);
            if (r_and(&kp, &pr, &k)) bm_fill(&scr, &kp, ppen);
            if (r_and(&kp, &tt, &k)) bm_fill(&scr, &kp, patch ? ppen : style.bar[act]);
            if (r_and(&kp, &tb, &k)) bm_fill(&scr, &kp, patch ? ppen : style.bar[act]);
            struct rect tc = { (s16)x, (s16)ty0, (s16)(x + tw), (s16)ty1 };
            if (r_and(&kp, &tc, &k))
                font_run(&scr, x, y, tf, (const u8 *)w->title, tn, style.text[act], patch ? ppen : style.bar[act],
                         act ? TA_BOLD : 0, &kp);
            for (int i = 0; i < 3; i++) {
                if (!style.gadx[i])
                    continue;                   /* no such gadget in this style */
                int gx = gad_x(i, &f);
                struct bm *img;
                const u8 *m = style.gad[i] ? dpy_image(style.gad[i], -1, &img) : 0;
                if (style.gad[i] && img) {
                    struct rect d = { (s16)gx, (s16)(ty0 + (style.title - 1 - img->h) / 2), 0, 0 };
                    d.x1 = (s16)(d.x0 + img->w);
                    d.y1 = (s16)(d.y0 + img->h);
                    if (r_and(&k, &d, c) && r_and(&k, &k, &t))
                        bm_image(img, m, k.x0 - d.x0, k.y0 - d.y0, &scr, &k);
                } else if (!style.gad[i]) {
                    struct rect g = t;
                    if (r_and(&k, &g, c))
                        bm_text(&scr, gx, gy, font_glyph("x_o"[i]), style.text[act], style.bar[act], 0, &k);
                }
            }
        }
        struct rect sep = { (s16)tx0, (s16)ty1, (s16)tx1, (s16)(ty1 + 1) };   /* the line under the title */
        if (r_and(&k, &sep, c))
            bm_fill(&scr, &k, style.border[act]);
    }
}

static void expose(struct win *w, const struct rect *r)        /* r: on the screen */
{
    struct rect in;
    win_inside(w, &in);
    ev_win((int)(w - wins), EV_EXPOSE, 0, 0, r->x0 - in.x0, r->y0 - in.y0, r->x1 - r->x0, r->y1 - r->y0);
}

/* paint the damage d back to front; fresh[slot]: the parts of a window's inside that are newly visible (painted from
 * the copy, or cleared and exposed) - a part not fresh is on the screen already */
static struct region fresh[NWIN];
u32 npaints;
static void paint(const struct rect *d)
{
    npaints++;
    struct region bg;
    rg_set(&bg, d);
    for (int k = 0; k < norder; k++) {
        struct win *w = &wins[order[k]];
        if (!shown(w))
            continue;
        struct rect o[3];
        int n = outer(w, o);
        for (int i = 0; i < n; i++)
            rg_sub(&bg, &o[i]);
    }
    int full = bg.over;                 /* too many pieces: the whole damage cleared, every window drawn in it again */
    if (full)
        bm_fill(&scr, d, 0);
    else
        for (int i = 0; i < bg.n; i++)
            bm_fill(&scr, &bg.r[i], 0);
    for (int k = 0; k < norder; k++) {
        int s = order[k];
        struct win *w = &wins[s];
        if (!shown(w))
            continue;
        struct rect in;
        win_inside(w, &in);
        for (int i = 0; i < w->vis.n; i++) {
            struct rect c, ci;
            if (!r_and(&c, &w->vis.r[i], d))
                continue;
            frame_paint(w, &c);
            if (w->back.p && r_and(&ci, &c, &in))
                bm_copy(&w->back, ci.x0 - in.x0, ci.y0 - in.y0, &scr, &ci);
        }
        if (w->back.p)
            continue;
        struct region f = full || fresh[s].over ? w->vis : fresh[s];
        rg_and(&f, d);
        struct region v = w->vis;       /* fresh and visible: cleared, then the program draws it */
        for (int i = 0; i < f.n; i++)
            for (int j = 0; j < v.n; j++) {
                struct rect c;
                if (r_and(&c, &f.r[i], &v.r[j]) && r_and(&c, &c, &in)) {
                    bm_fill(&scr, &c, 0);
                    expose(w, &c);
                }
            }
    }
}

/* ---- the change ---- */
static struct region ovis[NWIN];
static struct rect oin[NWIN];
static u8 owhole[NWIN], oshown[NWIN];
static void before(void)
{
    drag_hide();                        /* no outline in what is copied or painted over */
    for (int i = 0; i < NWIN; i++) {
        struct win *w = &wins[i];
        fresh[i].n = 0;
        ovis[i] = w->vis;
        oshown[i] = (u8)shown(w);
        if (w->id)
            win_inside(w, &oin[i]);
        owhole[i] = (u8)(oshown[i] && whole(w, &w->vis));
    }
}
/* copy the inside from where it was on the screen (whole) into a new kept copy; 0 if none could be had */
static int keep(int s)
{
    struct win *w = &wins[s];
    if (w->back.p)
        return 1;
    if (w->simple || !w->iw || !w->ih || w->kind == WK_DESKTOP)
        return 0;                       /* the desktop is redrawn by its program: a whole screen's copy costs too much */
    u32 need = (u32)(w->iw + 15) / 16 * 2 * scr.depth * w->ih;
    if (chip_used + need > chip_budget || bm_alloc(&w->back, w->iw, w->ih, scr.depth) < 0) {
        w->back.p = 0;
        w->simple = 1;                  /* simple refresh from now on: its program draws what is uncovered */
        return 0;
    }
    struct rect d = { 0, 0, (s16)w->iw, (s16)w->ih };
    bm_copy(&scr, oin[s].x0, oin[s].y0, &w->back, &d);
    return 1;
}
/* after the change of state: vis again; copies where needed; paint d */
static void after(const struct rect *d)
{
    calc_vis();
    for (int i = 0; i < NWIN; i++) {
        struct win *w = &wins[i];
        if (!w->id)
            continue;
        struct rect in;
        win_inside(w, &in);
        int moved = oshown[i] && (in.x0 != oin[i].x0 || in.y0 != oin[i].y0);
        int now = shown(w) && whole(w, &w->vis);
        if (owhole[i] && (!now || moved))
            keep(i);                    /* the screen still shows it whole where it was */
        /* fresh: what is visible now and was not (all of it if it moved or was not shown) */
        if (shown(w)) {
            fresh[i] = w->vis;
            if (oshown[i] && !moved)
                for (int j = 0; j < ovis[i].n; j++)
                    rg_sub(&fresh[i], &ovis[i].r[j]);
        }
    }
    paint(d);
    drag_show();
    for (int i = 0; i < NWIN; i++) {    /* whole on the screen again: the copy goes */
        struct win *w = &wins[i];
        if (w->id && w->back.p && shown(w) && whole(w, &w->vis) && i != dkeep)
            bm_free(&w->back);
        if (w->id && shown(w) && whole(w, &w->vis))
            w->simple = 0;              /* a new try at keeping a copy next time */
    }
}
static void damage_of(int s, struct rect *d, int first)
{
    struct rect b;
    bounds(&wins[s], &b);
    if (first) { *d = b; return; }
    if (b.x0 < d->x0) d->x0 = b.x0;
    if (b.y0 < d->y0) d->y0 = b.y0;
    if (b.x1 > d->x1) d->x1 = b.x1;
    if (b.y1 > d->y1) d->y1 = b.y1;
}

/* ---- the operations ---- */
static void order_rm(int s)
{
    int k = 0;
    for (int i = 0; i < norder; i++)
        if (order[i] != s)
            order[k++] = order[i];
    norder = k;
}
/* the stacking rule: desktops at the back, bars in front, the rest between */
static int layer(const struct win *w) { return w->kind == WK_DESKTOP ? 0 : w->kind == WK_BAR ? 2 : 1; }
static void order_put(int s, int back)
{
    order_rm(s);
    int L = layer(&wins[s]), i;
    if (back) {
        for (i = 0; i < norder && layer(&wins[order[i]]) < L; i++)
            ;
    } else {
        for (i = norder; i > 0 && layer(&wins[order[i - 1]]) > L; i--)
            ;
    }
    for (int j = norder; j > i; j--)
        order[j] = order[j - 1];
    order[i] = (u16)s;
    norder++;
}
static int clampsz(int v, int mn, int mx, int step, int base)
{
    if (mn && v < mn) v = mn;
    if (mx && v > mx) v = mx;
    if (step > 1) v = base + (v - base) / step * step;
    if (v < 1) v = 1;
    return v;
}

/* D38 "Windows stay on the screen": a framed window's frame top left (x, y) for an inside iw x ih, moved the least
 * so that the whole frame is on the screen, below a full-width bar along the top and above one along the bottom (one
 * taller than the room: its top at the top edge, the rest past the bottom) and across (one wider than the screen: its
 * left edge at the first column). x stays on the 8-pixel step. The pointer may go further; the window stops at the
 * edge. */
static void edges(int *top, int *bot)
{
    *top = 0;
    *bot = scr.h;
    for (int i = 0; i < NWIN; i++) {
        const struct win *b = &wins[i];
        if (!b->id || b->kind != WK_BAR || !(b->state & WST_SHOWN) || b->iw < scr.w)
            continue;                   /* a menu (narrower than the screen) is no edge */
        if (b->fy <= 0 && b->fy + b->ih > *top)
            *top = b->fy + b->ih;
        else if (b->fy > 0 && b->fy + b->ih >= scr.h && b->fy < *bot)
            *bot = b->fy;
    }
}
static void onscreen(const struct win *w, int iw, int ih, int *x, int *y)
{
    if (!framed(w))
        return;
    int top, bot;
    edges(&top, &bot);
    int xmin = ((bl(w) + 7) & ~7) - bl(w), xmax = ((scr.w - iw - br(w)) & ~7) - bl(w);
    int ymax = bot - bt(w) - ih - bb(w);        /* the whole frame above the bottom edge */
    if (*x > xmax) *x = xmax;
    if (*x < xmin) *x = xmin;
    if (*y > ymax) *y = ymax;
    if (*y < top) *y = top;             /* taller than the room: at the top, its bottom past the edge */
}
/* a bar or the frames changed: every other window back inside the edges (not one being dragged) */
static void reclamp(void)
{
    for (int i = 0; i < NWIN; i++) {
        struct win *w = &wins[i];
        if (!w->id || !w->placed || !framed(w) || w->lifted || dragging == i + 1)
            continue;
        int x = w->fx, y = w->fy;
        onscreen(w, w->iw, w->ih, &x, &y);
        if (x != w->fx || y != w->fy) {
            if (w->state & WST_SHOWN)
                win_place(i, x, y, w->iw, w->ih);
            else {
                w->fx = (s16)x;
                w->fy = (s16)y;
            }
        }
    }
}

int win_new(int cl, const struct dpywin *d)
{
    int s = -1;
    for (int i = 0; i < NWIN; i++)
        if (!wins[i].id) { s = i; break; }
    if (s < 0)
        return -ENOSPC;
    if (d->kind > WK_DESKTOP)
        return -EINVAL;
    struct win *w = &wins[s];
    memset(w, 0, sizeof *w);
    do {                                /* a number no window has now (the 16 bits wrap) */
        w->id = next_id++;
        if (!next_id) next_id = 1;
    } while (win_get(w->id) != w);
    w->cl = (u8)cl;
    w->kind = (u8)d->kind;
    w->flags = d->flags;
    w->minw = d->minw; w->minh = d->minh; w->maxw = d->maxw; w->maxh = d->maxh;
    w->stepw = d->stepw; w->steph = d->steph;
    memcpy(w->title, d->title, DPY_TITLE);
    w->title[DPY_TITLE - 1] = 0;
    int iw = d->w ? d->w : 160, ih = d->h ? d->h : 80;
    if (w->kind == WK_DESKTOP) { iw = scr.w; ih = scr.h; }
    if (w->kind == WK_BAR && !d->w) iw = scr.w;    /* a bar: the screen's width unless asked (a menu: its own) */
    w->iw = (u16)clampsz(iw > scr.w ? scr.w : iw, w->minw, w->maxw, 0, 0);
    w->ih = (u16)clampsz(ih > scr.h ? scr.h : ih, w->minh, w->maxh, 0, 0);
    w->fx = d->x;
    w->fy = d->y;
    order_put(s, w->kind == WK_DESKTOP);
    return s;
}

void win_close(int s)
{
    struct win *w = &wins[s];
    struct rect d;
    before();
    damage_of(s, &d, 1);
    int was = shown(w);
    if (focus == s)
        focus = -1;
    if (dragging == s + 1)
        dragging = 0;
    if (dkeep == s)
        dkeep = -1;
    if (w->lifted)
        lift_end();                     /* its place was painted when it was lifted: nothing to paint */
    bm_free(&w->back);
    free(w->menu);
    w->menu = 0;
    w->id = 0;
    w->state = 0;
    order_rm(s);
    if (was)
        after(&d);
    else {
        calc_vis();
        drag_show();
    }
}

/* the frame's top left at x, y and the inside w x h; the inside's left edge on an 8-pixel column (D38 3.2) */
void win_place(int s, int x, int y, int iw, int ih)
{
    struct win *w = &wins[s];
    struct rect d;
    if (w->kind == WK_DESKTOP) { x = 0; y = 0; iw = scr.w; ih = scr.h; }
    iw = clampsz(iw, w->minw, w->maxw, w->stepw, w->stepw ? w->iw % w->stepw : 0);    /* steps from the size it has */
    ih = clampsz(ih, w->minh, w->maxh, w->steph, w->steph ? w->ih % w->steph : 0);
    if (iw > scr.w) iw = scr.w;
    if (ih > scr.h) ih = scr.h;
    x = ((x + bl(w)) & ~7) - bl(w);
    onscreen(w, iw, ih, &x, &y);
    int resized = iw != w->iw || ih != w->ih;
    before();
    damage_of(s, &d, 1);
    if (resized)
        bm_free(&w->back);              /* a new size: the program draws it all again */
    w->fx = (s16)x;
    w->fy = (s16)y;
    w->iw = (u16)iw;
    w->ih = (u16)ih;
    w->placed = 1;
    damage_of(s, &d, 0);
    if (resized) {
        owhole[s] = 0;                  /* nothing of the old inside is kept */
        oshown[s] = 0;
        ev_win(s, EV_RESIZE, 0, 0, 0, 0, iw, ih);
    }
    if (shown(w) || oshown[s])
        after(&d);
    else {
        calc_vis();
        drag_show();
    }
    if (w->kind == WK_BAR)
        reclamp();
}

void win_stack(int s, int back)
{
    struct rect d;
    before();
    damage_of(s, &d, 1);
    order_put(s, back);
    if (shown(&wins[s]))
        after(&d);
    else {
        calc_vis();
        drag_show();
    }
}

void win_show(int s, int show, int why)
{
    struct win *w = &wins[s];
    struct rect d;
    if (!w->placed && show) {           /* nobody placed it: below the last one (the fallback without a manager) */
        static int casc;
        int x = w->fx >= 0 ? w->fx : w->kind == WK_BAR ? 0 : 24 + 16 * (casc % 8);
        int y = w->fy >= 0 ? w->fy : w->kind == WK_BAR ? 0 : 16 + 12 * (casc % 8);
        casc++;
        win_place(s, x, y, w->iw, w->ih);
    }
    if (show && !(w->state & WST_SHOWN)) {    /* placed by the manager without DPY_WM_PLACE: on the screen too */
        int x = w->fx, y = w->fy;
        onscreen(w, w->iw, w->ih, &x, &y);
        w->fx = (s16)x;
        w->fy = (s16)y;
    }
    before();
    damage_of(s, &d, 1);
    if (show)
        w->state = (u16)((w->state | WST_SHOWN) & ~(WST_MINIMISED | WST_WORKSPACE));
    else {
        w->state = (u16)((w->state & ~WST_SHOWN) | (why == HIDE_WORKSPACE ? WST_WORKSPACE : WST_MINIMISED));
        if (focus == s)
            focus = -1;
    }
    after(&d);
    if (!show && why == HIDE_WORKSPACE)
        bm_free(&w->back);              /* a workspace's window gives its copy up (D38 revision 2) */
    ev_win(s, EV_MINIMISED, 0, !show, 0, 0, 0, 0);
    if (show && w->kind == WK_BAR)
        reclamp();
}

void win_focus(int s)
{
    int old = focus;
    if (s == old)
        return;
    focus = s;
    drag_hide();
    for (int k = 0; k < 2; k++) {
        int t = k ? s : old;
        if (t < 0 || !shown(&wins[t]))
            continue;
        struct win *w = &wins[t];
        struct rect f;
        win_frame_rect(w, &f);
        for (int i = 0; i < w->vis.n; i++) {
            struct rect c;
            if (r_and(&c, &w->vis.r[i], &f))
                frame_paint(w, &c);
        }
        ev_win(t, EV_FOCUS, 0, k, 0, 0, 0, 0);
    }
    drag_show();
}

void win_title(int s)
{
    struct win *w = &wins[s];
    if (!shown(w))
        return;
    drag_hide();
    for (int i = 0; i < w->vis.n; i++)
        frame_paint(w, &w->vis.r[i]);
    drag_show();
}

void win_style(const struct dpystyle *st)
{
    style = *st;
    for (int i = 0; i < NWIN; i++) {
        bm_free(&wins[i].back);
        if (wins[i].id && framed(&wins[i]) && !wins[i].lifted && dragging != i + 1) {
            int x = wins[i].fx, y = wins[i].fy;    /* new borders: still on the screen (all is painted again) */
            onscreen(&wins[i], wins[i].iw, wins[i].ih, &x, &y);
            wins[i].fx = (s16)x;
            wins[i].fy = (s16)y;
        }
    }
    win_redraw_all();
}

/* a new screen (DPY_MODE, D38 "Screen modes"): the desktop's window its size; a window taller than the room below
 * the bar the room's (in its steps; told EV_RESIZE); every framed window kept on it; the kept copies given up (they
 * are of the old screen's look: the programs draw again) and all painted again */
void win_screen(void)
{
    int top, bot;
    edges(&top, &bot);
    for (int i = 0; i < NWIN; i++) {
        struct win *w = &wins[i];
        if (!w->id)
            continue;
        bm_free(&w->back);
        int iw = w->iw, ih = w->ih;
        if (w->kind == WK_DESKTOP) {
            iw = scr.w;
            ih = scr.h;
        } else if (framed(w) && bt(w) + ih + bb(w) > bot - top) {
            ih = clampsz(bot - top - bt(w) - bb(w), w->minh, 0, w->steph, w->steph ? w->ih % w->steph : 0);
        } else if (ih > scr.h)
            ih = scr.h;
        if (iw != w->iw || ih != w->ih) {
            w->iw = (u16)iw;
            w->ih = (u16)ih;
            ev_win(i, EV_RESIZE, 0, 0, 0, 0, iw, ih);
        }
        if (framed(w) && !w->lifted) {
            int x = w->fx, y = w->fy;
            onscreen(w, w->iw, w->ih, &x, &y);
            w->fx = (s16)x;
            w->fy = (s16)y;
        }
    }
    win_redraw_all();
}

/* everything again: the background, the frames, the insides from their copies, the rest exposed */
void win_redraw_all(void)
{
    for (int i = 0; i < NWIN; i++) {
        oshown[i] = 0;
        owhole[i] = 0;
    }
    calc_vis();
    for (int i = 0; i < NWIN; i++)
        fresh[i] = wins[i].vis;
    struct rect all = { 0, 0, (s16)scr.w, (s16)scr.h };
    paint(&all);
}

int win_at(int x, int y, int *part)
{
    for (int k = norder - 1; k >= 0; k--) {
        int s = order[k];
        struct win *w = &wins[s];
        if (!shown(w))
            continue;
        struct rect f, in;
        win_frame_rect(w, &f);
        win_inside(w, &in);
        if (x < f.x0 || x >= f.x1 || y < f.y0 || y >= f.y1)
            continue;
        if (x >= in.x0 && x < in.x1 && y >= in.y0 && y < in.y1) {
            *part = 0;
            return s;
        }
        *part = FP_BORDER;
        if (x >= f.x1 - 8 && y >= f.y1 - 8)
            *part = FP_SIZE;
        else if (style.title && y < f.y0 + 1 + style.title) {
            *part = FP_TITLE;
            for (int i = 0; i < 3; i++) {
                if (!style.gadx[i])
                    continue;
                int gx = gad_x(i, &f), gw = 8;
                struct bm *img;
                if (style.gad[i] && (dpy_image(style.gad[i], -1, &img), img))
                    gw = img->w;
                if (x >= gx && x < gx + gw)
                    *part = i == 0 ? FP_CLOSE : i == 1 ? FP_MIN : FP_ZOOM;
            }
        }
        return s;
    }
    return -1;
}

/* ---- a DRAW batch (D38 4.2): every order checked, clipped to the window, its clip and what is visible; drawn on the
 * screen where visible and into the kept copy ---- */
static int dr_slot;
static struct rect dr_in;
static void each(const struct rect *wr, void (*f)(struct bm *, const struct rect *, int, int, const void *), int a,
                 const void *p)
{
    struct win *w = &wins[dr_slot];
    struct rect r = { (s16)(wr->x0 + dr_in.x0), (s16)(wr->y0 + dr_in.y0), (s16)(wr->x1 + dr_in.x0),
                      (s16)(wr->y1 + dr_in.y0) }, c;
    if (!r_and(&r, &r, &dr_in))
        return;
    if (w->clip.x1) {
        struct rect cl = { (s16)(w->clip.x0 + dr_in.x0), (s16)(w->clip.y0 + dr_in.y0), (s16)(w->clip.x1 + dr_in.x0),
                           (s16)(w->clip.y1 + dr_in.y0) };
        if (!r_and(&r, &r, &cl))
            return;
    }
    if (shown(w))
        for (int i = 0; i < w->vis.n; i++)
            if (r_and(&c, &r, &w->vis.r[i]))
                f(&scr, &c, a, 0, p);
    if (w->back.p) {
        c = (struct rect){ (s16)(r.x0 - dr_in.x0), (s16)(r.y0 - dr_in.y0), (s16)(r.x1 - dr_in.x0), (s16)(r.y1 - dr_in.y0) };
        f(&w->back, &c, a, 1, p);
    }
}
static void d_fill(struct bm *b, const struct rect *c, int pen, int inback, const void *p)
{
    (void)inback; (void)p;
    bm_fill(b, c, pen);
}
static void d_xor(struct bm *b, const struct rect *c, int pen, int inback, const void *p)
{
    (void)inback; (void)p;
    bm_xor(b, c, pen);
}
struct textarg { int x, y, fg, bg, attr, font; const u8 *s; u32 n; };
static void d_text(struct bm *b, const struct rect *c, int unused, int inback, const void *p)
{
    (void)unused;
    const struct textarg *t = p;
    int ox = inback ? 0 : dr_in.x0, oy = inback ? 0 : dr_in.y0;
    font_run(b, t->x + ox, t->y + oy, t->font, t->s, t->n, t->fg, t->bg, t->attr, c);
}
struct imgarg { struct bm *img; const u8 *mask; int sx, sy, dx, dy; };
static void d_image(struct bm *b, const struct rect *c, int unused, int inback, const void *p)
{
    (void)unused;
    const struct imgarg *a = p;
    int ox = inback ? 0 : dr_in.x0, oy = inback ? 0 : dr_in.y0;
    bm_image(a->img, a->mask, a->sx + c->x0 - (a->dx + ox), a->sy + c->y0 - (a->dy + oy), b, c);
}

static int s16ok(int v) { return v >= -2048 && v <= 4095; }

void win_draw(int s, const u16 *o, u32 nw, u32 *bad)
{
    struct win *w = &wins[s];
    dr_slot = s;
    win_inside(w, &dr_in);
    for (u32 i = 0; i < nw;) {
        u32 len = o[i] & 0xFF, op = o[i] >> 8;
        if (!len || i + len > nw) {
            *bad = i * 2;
            return;
        }
        const s16 *a = (const s16 *)&o[i + 1];
        u32 na = len - 1;
        switch (op) {
        case O_CLIP:
            if (na < 4) goto bad;
            if (a[2] <= 0 || a[3] <= 0)
                w->clip.x1 = 0;
            else
                w->clip = (struct rect){ a[0], a[1], (s16)(a[0] + a[2]), (s16)(a[1] + a[3]) };
            break;
        case O_FILL:
        case O_XLINE: {
            if (na < 4 || (op == O_FILL && na < 5)) goto bad;
            if (!s16ok(a[0]) || !s16ok(a[1]) || a[2] < 0 || a[3] < 0 || a[2] > 4096 || a[3] > 4096) goto bad;
            struct rect r = { a[0], a[1], (s16)(a[0] + a[2]), (s16)(a[1] + a[3]) };
            if (op == O_FILL)
                each(&r, d_fill, a[4] & 15, 0);
            else
                each(&r, d_xor, na >= 5 ? a[4] & 15 : 15, 0);   /* the pen's planes (all, in the first form) */
            break;
        }
        case O_TEXT: {
            if (na < 6) goto bad;
            u32 nb = (u16)a[5];
            if (nb > (na - 6) * 2 || !s16ok(a[0]) || !s16ok(a[1])) goto bad;
            int f = font_id((u16)a[2]);         /* 0 the 8 x 8, 1 the desktop's font */
            struct textarg t = { a[0], a[1], a[3] & 15, (a[3] >> 8) & 15, a[4] & 7, f, (const u8 *)&a[6], nb };
            int tw = f ? font_w(f, t.s, nb) : 8 * (int)nb;
            struct rect r = { a[0], a[1], (s16)(a[0] + (tw < 8192 ? tw : 8192)), (s16)(a[1] + font_h(f)) };
            each(&r, d_text, 0, &t);
            break;
        }
        case O_COPY: {
            if (na < 6) goto bad;
            int sx = a[0], sy = a[1], cw = a[2], ch = a[3], dx = a[4], dy = a[5];
            if (cw <= 0 || ch <= 0) break;
            /* both rectangles clipped to the inside, the same amount from each */
            if (sx < 0) { dx -= sx; cw += sx; sx = 0; }
            if (sy < 0) { dy -= sy; ch += sy; sy = 0; }
            if (dx < 0) { sx -= dx; cw += dx; dx = 0; }
            if (dy < 0) { sy -= dy; ch += dy; dy = 0; }
            if (sx + cw > w->iw) cw = w->iw - sx;
            if (dx + cw > w->iw) cw = w->iw - dx;
            if (sy + ch > w->ih) ch = w->ih - sy;
            if (dy + ch > w->ih) ch = w->ih - dy;
            if (cw <= 0 || ch <= 0) break;
            struct rect dst = { (s16)dx, (s16)dy, (s16)(dx + cw), (s16)(dy + ch) };
            if (w->back.p) {            /* the copy holds it all: copied there, then shown where visible */
                bm_copy(&w->back, sx, sy, &w->back, &dst);
                struct rect sd = { (s16)(dst.x0 + dr_in.x0), (s16)(dst.y0 + dr_in.y0), (s16)(dst.x1 + dr_in.x0),
                                   (s16)(dst.y1 + dr_in.y0) }, c;
                if (shown(w))
                    for (int k = 0; k < w->vis.n; k++)
                        if (r_and(&c, &sd, &w->vis.r[k]))
                            bm_copy(&w->back, c.x0 - dr_in.x0, c.y0 - dr_in.y0, &scr, &c);
            } else if (shown(w)) {
                struct rect sd = { (s16)(dst.x0 + dr_in.x0), (s16)(dst.y0 + dr_in.y0), (s16)(dst.x1 + dr_in.x0),
                                   (s16)(dst.y1 + dr_in.y0) };
                if (whole(w, &w->vis))
                    bm_copy(&scr, sx + dr_in.x0, sy + dr_in.y0, &scr, &sd);
                else                    /* simple refresh, partly covered: the program draws it again */
                    expose(w, &sd);
            }
            break;
        }
        case O_IMAGE: {
            if (na < 7) goto bad;
            struct bm *img;
            const u8 *m = dpy_image((u16)a[0], w->cl, &img);   /* the program's own images */
            if (!img) goto bad;
            int sx = a[1], sy = a[2], iw = a[3], ih = a[4];
            if (sx < 0 || sy < 0 || iw <= 0 || ih <= 0 || sx + iw > img->w || sy + ih > img->h) goto bad;
            if (!s16ok(a[5]) || !s16ok(a[6])) goto bad;
            struct imgarg ia = { img, m, sx, sy, a[5], a[6] };
            struct rect r = { a[5], a[6], (s16)(a[5] + iw), (s16)(a[6] + ih) };
            each(&r, d_image, 0, &ia);
            break;
        }
        default:
            break;                      /* an order of a later version: skipped by its length */
        }
        i += len;
        continue;
bad:
        *bad = i * 2;
        return;
    }
}

/* ---- the chips' drag (D38 "Dragging with the chips"): the window lifted into AGA's playfield 2, its bitmap placed
 * by the Copper each move, no blit; painted back into the screen's planes on the drop ---- */
static struct bm lb;                    /* the lifted window: plane 0 of line 0; a plane's line: its bytes, 88 zero bytes */
static u8 *lmem;                        /* lb's chip RAM: 88 zero bytes (the zero line), then the lines */
static int loff, lwb, lpen = -1;             /* the frame's left edge in its bitmap (0..7); pen 0's stand-in, -1 none */
static int hw;                          /* the drag is the chips' */
static u32 lrow(int y, int k) { return (u32)y * lb.stride + (u32)k * lb.bpr; }
/* the pens of the pixels in r (lb's coordinates): bit v for pen v; only bit 0 when pen0only */
static u16 lpens(const struct rect *r, int pen0only)
{
    u16 used = 0;
    int l0 = r->x0 >> 5, l1 = (r->x1 - 1) >> 5;
    u32 m0 = 0xFFFFFFFFu >> (r->x0 & 31), m1 = 0xFFFFFFFFu << (31 - ((r->x1 - 1) & 31));
    for (int y = r->y0; y < r->y1; y++) {
        const u32 *p = (const u32 *)(lb.p + lrow(y, 0)), *q = (const u32 *)(lb.p + lrow(y, 1)),
                  *t = (const u32 *)(lb.p + lrow(y, 2)), *u = (const u32 *)(lb.p + lrow(y, 3));
        for (int i = l0; i <= l1; i++) {
            u32 m = (i == l0 ? m0 : ~0u) & (i == l1 ? m1 : ~0u), a = p[i], b = q[i], c = t[i], d = u[i];
            if (pen0only) {
                if (~(a | b | c | d) & m)
                    return 1;
                continue;
            }
            u32 ab[4] = { ~a & ~b, a & ~b, ~a & b, a & b }, cd[4] = { ~c & ~d & m, c & ~d & m, ~c & d & m, c & d & m };
            for (int v = 0; v < 16; v++)
                if (!(used >> v & 1) && (ab[v & 3] & cd[v >> 2]))
                    used |= (u16)(1 << v);
            if (used == 0xFFFF)
                return used;
        }
    }
    return used;
}
/* pen 0's pixels in r become pen x (playfield 2's pen 0 is transparent; x's playfield-2 colour is pen 0's) */
static void lremap(const struct rect *r, int x)
{
    int l0 = r->x0 >> 5, l1 = (r->x1 - 1) >> 5;
    u32 m0 = 0xFFFFFFFFu >> (r->x0 & 31), m1 = 0xFFFFFFFFu << (31 - ((r->x1 - 1) & 31));
    for (int y = r->y0; y < r->y1; y++) {
        u32 *pl[4];
        for (int k = 0; k < 4; k++)
            pl[k] = (u32 *)(lb.p + lrow(y, k));
        for (int i = l0; i <= l1; i++) {
            u32 z = ~(pl[0][i] | pl[1][i] | pl[2][i] | pl[3][i]) & (i == l0 ? m0 : ~0u) & (i == l1 ? m1 : ~0u);
            for (int k = 0; k < 4; k++)
                if (x >> k & 1)
                    pl[k][i] |= z;
        }
    }
}
static void lift_end(void)
{
    if (!lmem)
        return;
    hw_off();
    blit_idle();
    sys_memfree(lmem);
    lmem = 0;
    hw = 0;
}
/* 1: the window is in playfield 2, its place painted under it; 0: not possible (the caller drags the old way) */
static int lift_start(int s)
{
    struct win *w = &wins[s];
    if (!hw_ok() || shadowing || !shown(w) || w->kind == WK_DESKTOP || lmem)
        return 0;
    if (!w->back.p) {                   /* its inside's source while lifted and at the drop */
        if (!whole(w, &w->vis))
            return 0;
        win_inside(w, &oin[s]);
        if (!keep(s))
            return 0;
    }
    struct rect b;
    bounds(w, &b);
    loff = b.x0 & 7;
    int wb = (loff + b.x1 - b.x0 + 63) >> 6 << 3, h = b.y1 - b.y0;
    u32 S = (u32)wb + 88, n = 88 + S * 4 * (u32)h;
    u8 *m = sys_memalloc_chip(n + 8);
    if (!m)
        return 0;
    u8 *base = (u8 *)(((u32)m + 7) & ~7u);
    memset(base, 0, 88);
    lb = (struct bm){ base + 88, (u16)(S * 8), (u16)h, (u16)S, 4, S * 4, 0 };
    struct rect all = { 0, 0, (s16)lb.w, (s16)h };
    struct bm save = scr;
    scr = lb;
    bm_fill(&scr, &all, 0);             /* the zero bytes around the window: transparent */
    scr.w = (u16)(wb * 8);
    s16 fx = w->fx, fy = w->fy;
    w->fx = (s16)loff;
    w->fy = 0;
    all.x1 = (s16)scr.w;
    frame_paint(w, &all);
    struct rect in, o[3];
    win_inside(w, &in);
    bm_copy(&w->back, 0, 0, &scr, &in);
    int no = outer(w, o);
    w->fx = fx;
    w->fy = fy;
    lb = scr;
    scr = save;
    blit_idle();
    lpen = -1;
    int p0 = 0;
    for (int i = 0; i < no && !p0; i++)
        p0 = lpens(&o[i], 1);
    if (p0) {                           /* pen 0 in it: a pen it does not use stands in */
        u16 used = 0;
        for (int i = 0; i < no; i++)
            used |= lpens(&o[i], 0);
        for (int v = 1; v < 16 && lpen < 0; v++)
            if (!(used >> v & 1))
                lpen = v;
        if (lpen < 0) {
            sys_memfree(m);
            return 0;
        }
        for (int i = 0; i < no; i++)
            lremap(&o[i], lpen);
    }
    lmem = m;
    lwb = wb;
    hw_show(&lb, base, b.x0 - loff, b.y0, lpen);
    hw_wait();                          /* shown in playfield 2 (the same picture where it is): now its place is painted */
    before();
    w->lifted = 1;
    after(&b);
    dkeep = s;
    hw = 1;
    return 1;
}
/* the drop: the window painted at (x, y) under playfield 2, which shows it there; then playfield 2 off */
static void lift_drop(int s, int x, int y)
{
    struct win *w = &wins[s];
    hw_show(&lb, lmem ? (const u8 *)(((u32)lmem + 7) & ~7u) : 0, x - loff, y, lpen);
    w->fx = (s16)x;
    w->fy = (s16)y;
    w->placed = 1;
    struct rect d;
    bounds(w, &d);
    hw_wait();
    before();
    w->lifted = 0;
    after(&d);
    blit_idle();
    lift_end();
}

/* ---- drags run in the server (D38 3.3): an outline (XOR) or the whole window, until the button is let go ---- */
static int dmode, dpx, dpy, dx0, dy0, dw0, dh0, ox, oy, ow, oh, odrawn;
static void outline(void)
{
    struct win *w = &wins[dragging - 1];
    int x0 = ox, y0 = oy, x1 = ox + bl(w) + ow + br(w), y1 = oy + bt(w) + oh + bb(w);
    struct rect e[4] = { { (s16)x0, (s16)y0, (s16)x1, (s16)(y0 + 1) }, { (s16)x0, (s16)(y1 - 1), (s16)x1, (s16)y1 },
                         { (s16)x0, (s16)(y0 + 1), (s16)(x0 + 1), (s16)(y1 - 1) },
                         { (s16)(x1 - 1), (s16)(y0 + 1), (s16)x1, (s16)(y1 - 1) } };
    for (int i = 0; i < 4; i++)
        bm_xor(&scr, &e[i], 15);
    odrawn = !odrawn;
}
void win_drag(int s, int mode, int px, int py)
{
    struct win *w = &wins[s];
    if (dragging || !shown(w))
        return;
    if ((mode & 15) == DRAG_MOVE && lift_start(s))
        mode = DRAG_MOVE;               /* the chips' drag: the frame style's choice is the fallback's */
    else if (!(mode & DRAG_OUTLINE)) {
        /* the whole window: drawn off the screen and shown once a pass (draw.c, shadow_*); a move takes its inside
         * from its kept copy, kept for the whole drag. No chip RAM for either: an outline */
        if (!shadow_begin())
            mode |= DRAG_OUTLINE;
        else if ((mode & 15) == DRAG_MOVE) {
            if (!w->back.p && whole(w, &w->vis)) {
                win_inside(w, &oin[s]);
                keep(s);
            }
            if (w->back.p)
                dkeep = s;
            else {
                shadow_end();           /* no copy (the budget, or simple refresh): its program would draw every step */
                mode |= DRAG_OUTLINE;
            }
        }
    }
    dragging = s + 1;
    dmode = mode;
    dpx = px; dpy = py;
    dx0 = ox = w->fx; dy0 = oy = w->fy; dw0 = ow = w->iw; dh0 = oh = w->ih;
    odrawn = 0;
    if (mode & DRAG_OUTLINE)
        outline();
}
/* the outline is off while anything else is drawn (an XOR over new pixels would leave marks) */
void drag_hide(void) { if (dragging && odrawn) outline(); }
void drag_show(void) { if (dragging && !odrawn && (dmode & DRAG_OUTLINE)) outline(); }
void win_drag_event(int x, int y, int up)
{
    if (!dragging)
        return;
    int s = dragging - 1;
    struct win *w = &wins[s];
    int nx = dx0, ny = dy0, nw = dw0, nh = dh0;
    if ((dmode & 15) == DRAG_MOVE) {
        nx = dx0 + x - dpx;
        ny = dy0 + y - dpy;
        nx = ((nx + bl(w)) & ~7) - bl(w);
    } else {
        int top, bot, mw = scr.w - dx0 - bl(w) - br(w), mh;      /* the frame's right and bottom stay on it */
        edges(&top, &bot);
        mh = bot - dy0 - bt(w) - bb(w);
        if (w->maxw && w->maxw < mw) mw = w->maxw;
        if (w->maxh && w->maxh < mh) mh = w->maxh;
        int mnw = w->minw ? w->minw : 16, mnh = w->minh ? w->minh : 8;   /* (no room for the least: it moves) */
        nw = clampsz(dw0 + x - dpx, mnw, mw >= mnw ? mw : w->maxw, w->stepw, w->stepw ? dw0 % w->stepw : 0);
        nh = clampsz(dh0 + y - dpy, mnh, mh >= mnh ? mh : w->maxh, w->steph, w->steph ? dh0 % w->steph : 0);
    }
    onscreen(w, nw, nh, &nx, &ny);      /* D38 "Windows stay on the screen", every drag's way */
    if (hw) {                           /* playfield 2's pointer stays inside its bitmap's zero bytes */
        if (nx - loff < 64 - lwb * 8)
            nx = 64 - lwb * 8 + loff;
        if (nx > scr.w - 8)
            nx = ((scr.w - 8 + bl(w)) & ~7) - bl(w);
        if (up) {
            dragging = 0;
            lift_drop(s, nx, ny);
            if (dkeep == s) {
                dkeep = -1;
                if (w->back.p && shown(w) && whole(w, &w->vis))
                    bm_free(&w->back);
            }
            ev_wm(EV_WM_DRAGEND, w->id, 0, 0, w->fx, w->fy, w->iw, w->ih);
            return;
        }
        if (nx != ox || ny != oy) {
            u32 t0 = sys_hrtime();
            hw_show(&lb, (const u8 *)(((u32)lmem + 7) & ~7u), nx - loff, ny, lpen);
            tmoves += sys_hrtime() - t0;
            nmoves++;
            ox = nx;
            oy = ny;
        }
        return;
    }
    if (dmode & DRAG_OUTLINE) {
        if (nx != ox || ny != oy || nw != ow || nh != oh || up) {
            if (odrawn)
                outline();
            ox = nx; oy = ny; ow = nw; oh = nh;
            if (!up)
                outline();
        }
    } else if (nx != w->fx || ny != w->fy || nw != w->iw || nh != w->ih) {
        u32 t0 = sys_hrtime();
        win_place(s, nx, ny, nw, nh);
        blit_idle();                    /* (its time: until the last blit ended) */
        tmoves += sys_hrtime() - t0;
        nmoves++;
    }
    if (up) {
        dragging = 0;
        if (dmode & DRAG_OUTLINE)
            win_place(s, nx, ny, nw, nh);
        if (dkeep == s) {               /* whole where it ended: the copy goes */
            dkeep = -1;
            if (w->back.p && shown(w) && whole(w, &w->vis))
                bm_free(&w->back);
        }
        ev_wm(EV_WM_DRAGEND, w->id, 0, 0, w->fx, w->fy, w->iw, w->ih);
    }
}
