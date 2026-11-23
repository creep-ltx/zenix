/* SPDX-License-Identifier: GPL-2.0-only */
/* The preference programs' window (display/prefswin.h): deskprefs' layout, shared with mouseprefs. Cold code: -Os. */
#include "libz.h"
#include "../prefswin.h"

static const char *const button[4] = { "Test", "Use", "Save", "Cancel" };

void pw_layout(struct prefswin *p, int ww)
{
    dpy_font(DPYF_UI, &p->uf);
    p->lh = p->uf.height > 8 ? p->uf.height : 8;
    p->ww = ww;
    p->wh = pw_by(p) + p->lh + 6 + 4 + p->lh + 4;
}
int pw_gy(const struct prefswin *p, int i) { return 6 + i * (p->lh + 6); }
int pw_ey(const struct prefswin *p) { return pw_gy(p, p->nrows) + 2; }
int pw_by(const struct prefswin *p) { return pw_gy(p, p->nrows) + 4 + p->extra; }
int pw_bx(const struct prefswin *p, int i) { return 8 + i * ((p->ww - 16 + 6) / 4); }
int pw_bw(const struct prefswin *p) { return (p->ww - 16 + 6) / 4 - 6; }
void pw_text(const struct prefswin *p, int x, int y, int fg, int bg, const char *s)
{
    dpy_textf(p->win, &p->uf, x, y, fg, bg, 0, s, (int)strlen(s));
}
void pw_box(const struct prefswin *p, int x, int y, int w, int h, int light, int dark)
{
    dpy_fill(p->win, x, y, w, 1, light);
    dpy_fill(p->win, x, y, 1, h, light);
    dpy_fill(p->win, x, y + h - 1, w, 1, dark);
    dpy_fill(p->win, x + w - 1, y, 1, h, dark);
}
void pw_draw(const struct prefswin *p)
{
    int lh = p->lh, ww = p->ww;
    dpy_fill(p->win, 0, 0, ww, p->wh, 2);
    for (int i = 0; i < p->nrows; i++) {
        char v[48];
        int y = pw_gy(p, i), on = i == p->row;
        pw_text(p, 8, y + 3, 1, 2, p->label[i]);
        dpy_fill(p->win, p->gx, y, ww - p->gx - 8, lh + 4, on ? 4 : 5);
        pw_box(p, p->gx, y, ww - p->gx - 8, lh + 4, 7, 1);
        p->value(i, v, sizeof v);
        pw_text(p, p->gx + 6, y + 2, on ? 7 : 1, on ? 4 : 5, "\xc2\xab");     /* the cycle mark */
        pw_text(p, p->gx + 18, y + 2, on ? 7 : 1, on ? 4 : 5, v);
    }
    for (int i = 0; i < 4; i++) {
        dpy_fill(p->win, pw_bx(p, i), pw_by(p), pw_bw(p), lh + 6, 5);
        pw_box(p, pw_bx(p, i), pw_by(p), pw_bw(p), lh + 6, 7, 1);
        int tw = dpy_textw(&p->uf, button[i], (int)strlen(button[i]));
        pw_text(p, pw_bx(p, i) + (pw_bw(p) - tw) / 2, pw_by(p) + 3, 1, 5, button[i]);
    }
    dpy_fill(p->win, 8, pw_by(p) + lh + 10, ww - 16, lh, 2);
    if (p->msg[0])
        pw_text(p, 8, pw_by(p) + lh + 10, 9, 2, p->msg);
}
void pw_say(struct prefswin *p, const char *s)
{
    strncpy(p->msg, s, sizeof p->msg - 1);
    p->msg[sizeof p->msg - 1] = 0;
}
int pw_hit(const struct prefswin *p, int x, int y)
{
    for (int i = 0; i < p->nrows; i++)
        if (x >= p->gx && x < p->ww - 8 && y >= pw_gy(p, i) && y < pw_gy(p, i) + p->lh + 4)
            return i;
    for (int i = 0; i < 4; i++)
        if (x >= pw_bx(p, i) && x < pw_bx(p, i) + pw_bw(p) && y >= pw_by(p) && y < pw_by(p) + p->lh + 6)
            return 100 + i;
    return -1;
}
int pw_key(struct prefswin *p, const struct dpyev *e)
{
    if (e->code & 0x80)
        return 0;
    switch (e->code) {
    case 0x4C: p->row = (p->row + p->nrows - 1) % p->nrows; return 0;
    case 0x4D: p->row = (p->row + 1) % p->nrows; return 0;
    case 0x4E: return 1 + p->row;
    case 0x4F: return -(1 + p->row);
    case 0x44: return 100 + PW_USE;
    case 0x45: return 100 + PW_CANCEL;
    }
    switch (e->ch[0] | 0x20) {
    case 't': return 100 + PW_TEST;
    case 'u': return 100 + PW_USE;
    case 's': return 100 + PW_SAVE;
    case 'c': return 100 + PW_CANCEL;
    }
    return 0;
}
void pw_where(const struct prefswin *p, const char *prog)
{
    struct dpyinfo in;
    if (dpy_info(p->win, &in) < 0)
        return;
    dprintf(1, "%s: window %d %d %d %d", prog, in.ix, in.iy, p->ww, p->wh);
    for (int i = 0; i < p->nrows; i++)
        dprintf(1, " row%d %d %d", i, in.ix + p->gx + 20, in.iy + pw_gy(p, i) + 3);
    for (int i = 0; i < 4; i++)
        dprintf(1, " %s %d %d", button[i], in.ix + pw_bx(p, i) + pw_bw(p) / 2, in.iy + pw_by(p) + 4);
    dprintf(1, "\n");
}
