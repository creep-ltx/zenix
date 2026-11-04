/* SPDX-License-Identifier: GPL-2.0-only */
/* libdisplay (DESIGN D38 5): the program's side of the display protocol; display/display.h has the calls. */
#include "libz.h"
#include "../display.h"

#define RECS 64
#define QWORDS 1024                     /* the order queue: 2 KB, sent when full */
static int sport = -1, myport = -1;
static struct { struct dpyring h; struct dpyev r[RECS]; } ring;
static unsigned short q[QWORDS];
static unsigned nq;
static int qwin, qerr, lost;
unsigned short dpy_w, dpy_h, dpy_depth;

int dpy_call(int type, unsigned a0, unsigned a1, unsigned a2, unsigned a3)
{
    struct umsg m;
    memset(&m, 0, sizeof m);
    m.type = (u32)type;
    m.a[0] = a0; m.a[1] = a1; m.a[2] = a2; m.a[3] = a3;
    if (sport < 0)
        return -ENOTCONN;
    return sys_call(sport, &m);
}

int dpy_open(const char *name)
{
    if (sport >= 0)
        return -EBUSY;
    char pn[16] = "dpy.";
    int pid = sys_getpid(), n = 4;
    char d[10];
    int k = 0;
    do { d[k++] = (char)('0' + pid % 10); pid /= 10; } while (pid && k < 10);
    while (k && n < 15) pn[n++] = d[--k];
    pn[n] = 0;
    if (myport < 0)                     /* a second try after a failure keeps the port it made */
        myport = sys_port_create(pn);
    if (myport < 0)
        return myport;
    sport = sys_port_find(DPY_PORTNAME);
    if (sport < 0)
        return -ECONNREFUSED;           /* no "display" service (/etc/svctab) */
    memset(&ring, 0, sizeof ring);
    struct upostbuf b = { (u32)&ring, sizeof ring, sport };
    int r = sys_post(POST_BUF, myport, &b);
    if (r < 0)
        return r;
    struct dpyhello h;
    memset(&h, 0, sizeof h);
    h.size = sizeof h;
    h.version = DPY_VERSION;
    strncpy(h.name, name ? name : "", DPY_NAME - 1);
    h.ring = (u32)&ring;
    h.ringsize = sizeof ring;
    h.port = myport;
    r = dpy_call(DPY_HELLO, (u32)&h, 0, 0, 0);
    if (r < 0) {
        sport = -1;
        return r;
    }
    dpy_w = h.width;
    dpy_h = h.height;
    dpy_depth = h.depth;
    return 0;
}

void dpy_close(void)
{
    dpy_flush();
    dpy_call(DPY_BYE, 0, 0, 0, 0);
    sport = -1;
}

int dpy_newwin(struct dpywin *d)
{
    dpy_flush();
    d->size = sizeof *d;
    return dpy_call(DPY_WIN_NEW, (u32)d, 0, 0, 0);
}
int dpy_window(int kind, const char *title, int x, int y, int w, int h, int flags)
{
    struct dpywin d;
    memset(&d, 0, sizeof d);
    d.kind = (unsigned short)kind;
    d.flags = (unsigned short)flags;
    d.x = (short)x; d.y = (short)y; d.w = (unsigned short)w; d.h = (unsigned short)h;
    strncpy(d.title, title ? title : "", DPY_TITLE - 1);
    return dpy_newwin(&d);
}
int dpy_title(int win, const char *t) { return dpy_call(DPY_WIN_SET, (u32)win, WS_TITLE, (u32)t, 0); }
int dpy_resize(int win, int w, int h) { return dpy_call(DPY_WIN_SET, (u32)win, WS_SIZE, (u32)w << 16 | (u32)h, 0); }
int dpy_menu(int win, const char *t, int n) { return dpy_call(DPY_MENU, (u32)win, (u32)t, (u32)n, 0); }
int dpy_destroy(int win)
{
    if (win == qwin)
        dpy_flush();
    return dpy_call(DPY_WIN_CLOSE, (u32)win, 0, 0, 0);
}
int dpy_info(int win, struct dpyinfo *i)
{
    i->size = sizeof *i;
    return dpy_call(DPY_WIN_INFO, (u32)win, (u32)i, 0, 0);
}

int dpy_flush(void)
{
    int r = 0;
    if (nq) {
        struct umsg m;
        memset(&m, 0, sizeof m);
        m.type = DPY_DRAW;
        m.a[0] = (u32)qwin; m.a[1] = (u32)q; m.a[2] = nq * 2;
        r = sport < 0 ? -ENOTCONN : sys_call(sport, &m);
        nq = 0;
    }
    if (!r && qerr)
        r = -EINVAL;
    qerr = 0;
    return r;
}
/* room for an order of n words in the queue for window win (sent first when it is another window's or full) */
static unsigned short *order(int win, int op, unsigned n)
{
    if (n > QWORDS || n > 255) {
        qerr = 1;
        return 0;
    }
    if ((win != qwin && nq) || nq + n > QWORDS)
        dpy_flush();
    qwin = win;
    unsigned short *o = &q[nq];
    o[0] = (unsigned short)(op << 8 | n);
    nq += n;
    return o + 1;
}
void dpy_clip(int win, int x, int y, int w, int h)
{
    unsigned short *o = order(win, O_CLIP, 5);
    if (o) { o[0] = (unsigned short)x; o[1] = (unsigned short)y; o[2] = (unsigned short)w; o[3] = (unsigned short)h; }
}
void dpy_fill(int win, int x, int y, int w, int h, int pen)
{
    unsigned short *o = order(win, O_FILL, 6);
    if (o) { o[0] = (unsigned short)x; o[1] = (unsigned short)y; o[2] = (unsigned short)w; o[3] = (unsigned short)h;
             o[4] = (unsigned short)pen; }
}
void dpy_xor(int win, int x, int y, int w, int h)
{
    unsigned short *o = order(win, O_XLINE, 5);
    if (o) { o[0] = (unsigned short)x; o[1] = (unsigned short)y; o[2] = (unsigned short)w; o[3] = (unsigned short)h; }
}
void dpy_text(int win, int x, int y, int fg, int bg, int attr, const char *s, int n)
{
    while (n > 0) {
        int k = n > 200 ? 200 : n;      /* an order is at most 255 words */
        if (k < n)                      /* never inside a UTF-8 character */
            while (k > 1 && ((unsigned char)s[k] & 0xC0) == 0x80)
                k--;
        unsigned short *o = order(win, O_TEXT, (unsigned)(7 + (k + 1) / 2));
        if (!o)
            return;
        o[0] = (unsigned short)x; o[1] = (unsigned short)y; o[2] = 0;
        o[3] = (unsigned short)(fg | bg << 8); o[4] = (unsigned short)attr; o[5] = (unsigned short)k;
        memcpy(&o[6], s, (u32)k);
        int cols = 0;
        for (int i = 0; i < k; i++)
            if (((unsigned char)s[i] & 0xC0) != 0x80)
                cols++;
        x += 8 * cols;
        s += k;
        n -= k;
    }
}
void dpy_copy(int win, int sx, int sy, int w, int h, int dx, int dy)
{
    unsigned short *o = order(win, O_COPY, 7);
    if (o) { o[0] = (unsigned short)sx; o[1] = (unsigned short)sy; o[2] = (unsigned short)w; o[3] = (unsigned short)h;
             o[4] = (unsigned short)dx; o[5] = (unsigned short)dy; }
}
void dpy_image(int win, int id, int sx, int sy, int w, int h, int dx, int dy)
{
    unsigned short *o = order(win, O_IMAGE, 8);
    if (o) { o[0] = (unsigned short)id; o[1] = (unsigned short)sx; o[2] = (unsigned short)sy; o[3] = (unsigned short)w;
             o[4] = (unsigned short)h; o[5] = (unsigned short)dx; o[6] = (unsigned short)dy; }
}

int dpy_image_new(int w, int h, int depth, int flags, const void *planes)
{
    struct dpyimage d = { sizeof d, (unsigned short)w, (unsigned short)h, (unsigned short)depth,
                          (unsigned short)flags, (u32)planes };
    return dpy_call(DPY_IMAGE, (u32)&d, 0, 0, 0);
}
int dpy_image_free(int id) { return dpy_call(DPY_IMAGE_FREE, (u32)id, 0, 0, 0); }
int dpy_palette(unsigned short *pal, int n) { return dpy_call(DPY_PALETTE, (u32)pal, (u32)n, 0, 0); }

int dpy_port(void) { return myport; }
int dpy_lost(void)
{
    int l = lost || (ring.h.flags & DPYR_LOST);
    lost = 0;
    ring.h.flags &= ~(unsigned)DPYR_LOST;
    return l;
}
int dpy_next(struct dpyev *e, int ticks)
{
    dpy_flush();
    for (;;) {
        volatile struct dpyring *h = &ring.h;
        if (h->tail != h->head) {
            unsigned n = h->nrec ? h->nrec : RECS;
            *e = ring.r[h->tail % n];
            h->tail++;
            if (h->flags & DPYR_LOST)
                lost = 1;
            return 1;
        }
        if (!ticks)
            return 0;
        struct pollfd p = { myport, POLLPORT | POLLRDBAND, 0 };
        int r = sys_evwait(&p, 1, ticks);
        if (r < 0)
            return r;
        if (r == 0 && ticks > 0)
            return 0;
    }
}
