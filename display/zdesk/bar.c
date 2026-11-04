/* SPDX-License-Identifier: GPL-2.0-only */
/* zdesk's bar (DESIGN D38 revision 2): one bar at the top in three zones of modules (/etc/zdesk/bar): left the Zenix
 * menu and the active program's menus (DPY_MENU), opened by a click (the Mac's way) and/or by holding the right
 * button anywhere (Workbench's), as the theme says; the middle a window list (minimised ones dimmed) or the
 * workspaces' numbers; right status blocks - the clock, memory, the network, the CPU, or a command whose first line
 * of output is shown, run every N seconds (never under 1). Menus open as a frameless window under their title and
 * also work from the keyboard (Amiga+Space, the arrows, Return, Esc). */
#include "zdesk.h"
#include "socket.h"

/* ---- the hit areas the last drawing left ---- */
enum { H_ZENIX = 1, H_MENU, H_WIN, H_WS };
#define NHIT 48
static struct { short x0, x1; u8 type; int arg; } hits[NHIT];
static int nhits;
static void hit(int x0, int x1, int type, int arg)
{
    if (nhits < NHIT)
        hits[nhits++] = (typeof(hits[0])){ (short)x0, (short)x1, (u8)type, arg };
}

/* ---- the active program's menus: "Title\tItem\tItem\nTitle..." ---- */
static char amenu[DPY_MENUMAX + 1];
static int amenu_win;
static const char *menu_title(int m)            /* the m-th title (its length up to \t or \n), 0 past the last */
{
    const char *s = amenu;
    for (int i = 0; i < m && s; i++) {
        s = strchr(s, '\n');
        if (s) s++;
    }
    return s && *s ? s : 0;
}
static int seglen(const char *s) { int n = 0; while (s[n] && s[n] != '\t' && s[n] != '\n') n++; return n; }
static void app_menus(void)
{
    amenu[0] = 0;
    amenu_win = focus_id;
    if (focus_id > 0) {
        int n = dpy_call(DPY_WM_MENUGET, (u32)focus_id, (u32)amenu, DPY_MENUMAX, 0);
        amenu[n > 0 ? n : 0] = 0;
    }
}

/* ---- drawing ---- */
static int text(int x, const char *s, int n, int fg, int bg, int bold)
{
    dpy_text(bar_id, x, 2, fg, bg, bold ? TA_BOLD : 0, s, n);
    return x + 8 * n;
}
static char right_text[160];
void bar_draw(void)
{
    if (bar_id <= 0)
        return;
    if (amenu_win != focus_id)
        app_menus();
    nhits = 0;
    int bg = pen(R_BAR), fg = pen(R_BARFG);
    dpy_fill(bar_id, 0, 0, 640, bar_h - 1, bg);
    dpy_fill(bar_id, 0, bar_h - 1, 640, 1, pen(R_BARLINE));
    int x = 8;
    /* the right zone first: the middle's list gets what is left */
    right_text[0] = 0;
    for (int i = 0; i < nblocks; i++)
        if (blocks[i].zone == 2 && blocks[i].text[0]) {
            if (right_text[0] && strlen(right_text) + 2 < sizeof right_text) strcat(right_text, "  ");
            if (strlen(right_text) + strlen(blocks[i].text) < sizeof right_text) strcat(right_text, blocks[i].text);
        }
    int rw = 8 * (int)strlen(right_text), rx = 640 - 8 - rw;
    for (int z = 0; z < 2; z++) {
        if (z == 1)
            x += 24;
        for (int i = 0; i < nblocks; i++) {
            struct block *k = &blocks[i];
            if (k->zone != z)
                continue;
            if (k->kind == B_ZENIX) {
                int x0 = x;
                x = text(x, "Zenix", 5, pen(R_LOGO), bg, 1);
                hit(x0 - 4, x + 4, H_ZENIX, 0);
                x += 16;
            } else if (k->kind == B_MENUS) {
                for (int m = 0; menu_title(m); m++) {
                    const char *t = menu_title(m);
                    int x0 = x;
                    x = text(x, t, seglen(t), fg, bg, 0);
                    hit(x0 - 4, x + 4, H_MENU, m);
                    x += 16;
                }
            } else if (k->kind == B_WINDOWS) {
                for (int j = 0; j < NZW; j++) {
                    struct zw *w = &zws[j];
                    if (!w->id || w->own || w->ws != cur_ws)
                        continue;
                    int n = (int)strlen(w->title);
                    if (n > 12) n = 12;
                    if (!n) n = 1;
                    if (x + 8 * n + 8 > rx)
                        break;
                    int cur = w->id == focus_id;
                    if (cur)
                        dpy_fill(bar_id, x - 4, 1, 8 * n + 8, bar_h - 3, pen(R_BARCURBG));
                    int x0 = x;
                    x = text(x, w->title[0] ? w->title : "?", n, cur ? pen(R_BARCUR) : w->min ? pen(R_BARDIM) : fg,
                             cur ? pen(R_BARCURBG) : bg, cur);
                    hit(x0 - 4, x + 4, H_WIN, w->id);
                    x += 16;
                }
            } else if (k->kind == B_WORKSPACES) {
                for (int n = 1; n <= 9; n++) {
                    int used = 0;
                    for (int j = 0; j < NZW; j++)
                        if (zws[j].id && !zws[j].own && zws[j].ws == n) used = 1;
                    char c = (char)('0' + n);
                    int cur = n == cur_ws;
                    if (cur)
                        dpy_fill(bar_id, x - 4, 1, 16, bar_h - 3, pen(R_BARCURBG));
                    text(x, &c, 1, cur ? pen(R_BARCUR) : used ? fg : pen(R_BARDIM), cur ? pen(R_BARCURBG) : bg, cur);
                    hit(x - 4, x + 12, H_WS, n);
                    x += 16;
                }
            }
        }
    }
    if (rw)
        text(rx, right_text, (int)strlen(right_text), pen(R_CLOCK), bg, 0);
    dpy_flush();
    if (verbose)
        dprintf(1, "zdesk: bar right: %s\n", right_text);
}

/* ---- the blocks ---- */
static u32 attn(void)
{
    u32 zi[7];
    return sys3(93, 0, (u32)zi, sizeof zi) == 0 ? zi[2] : 0;
}
static void kb(char *o, u32 k, const char *name)
{
    if (k >= 1024)
        sprintf(o, "%s %u.%uM", name, k / 1024, k % 1024 * 10 / 1024);
    else
        sprintf(o, "%s %uK", name, k);
}
static void cmd_start(struct block *k)
{
    int fd[2];
    if (sys_pipe(fd) < 0)
        return;
    char *argv[] = { "/bin/sh", "-c", k->cmd, 0 };
    int io[3] = { 0, fd[1], 2 };
    int pid = sys_spawn("/bin/sh", argv, io);
    sys_close(fd[1]);
    if (pid < 0) {
        sys_close(fd[0]);
        strcpy(k->text, "?");
        return;
    }
    k->pid = pid;
    k->fd = fd[0];
    k->nout = 0;
    k->started = sys_ticks();
}
static void cmd_end(struct block *k, int show)
{
    if (k->fd >= 0)
        sys_close(k->fd);
    k->fd = -1;
    if (k->pid > 0) {
        if (sys_waitnb(k->pid) == -2) {         /* still running: it has had its time */
            sys_killsig(k->pid, SIGKILL);
            sys_wait(k->pid);
        }
    }
    k->pid = 0;
    if (show) {
        k->out[k->nout] = 0;
        char *nl = strchr(k->out, '\n');
        if (nl) *nl = 0;
        strncpy(k->text, k->out, sizeof k->text - 1);
    }
}
void bar_poll(struct pollfd *p, int *n)
{
    for (int i = 0; i < nblocks; i++)
        if (blocks[i].fd >= 0 && *n < 16) {
            p[*n].fd = blocks[i].fd;
            p[*n].events = POLLIN;
            p[*n].revents = 0;
            (*n)++;
        }
}
void bar_tick(void)
{
    int changed = 0;
    u32 now = sys_ticks();
    for (int i = 0; i < nblocks; i++) {
        struct block *k = &blocks[i];
        char t[48];
        if (k->fd >= 0) {                       /* a command's output so far */
            struct pollfd p = { k->fd, POLLIN, 0 };
            if (sys_evwait(&p, 1, 0) > 0) {
                int r = sys_read(k->fd, k->out + k->nout, (u32)(sizeof k->out - 1 - k->nout));
                if (r > 0)
                    k->nout += r;
                int nl = 0;
                for (int j = 0; j < k->nout; j++) nl |= k->out[j] == '\n';
                if (r <= 0 || nl || k->nout >= (int)sizeof k->out - 1) {
                    cmd_end(k, 1);
                    changed = 1;
                }
            } else if ((u32)(now - k->started) > (u32)(k->every < 10 ? 10 : k->every) * 100) {
                cmd_end(k, 0);                  /* a command that does not answer in time: ended, its text kept */
            }
        }
        if (!k->every || (int)(now - k->due) < 0)
            continue;
        k->due = now + (u32)k->every * 100;
        t[0] = 0;
        if (k->kind == B_CLOCK) {
            u32 s = sys_time() % 86400;
            sprintf(t, "%02u:%02u", s / 3600, s / 60 % 60);
        } else if (k->kind == B_MEMORY) {
            u32 mi[4];
            if (sys_meminfo(mi) == 0) {
                char c[20], f[20];
                kb(c, mi[0] >> 10, "chip");
                if (mi[1]) {
                    kb(f, mi[1] >> 10, "fast");
                    sprintf(t, "%s %s", c, f);
                } else
                    strcpy(t, c);
            }
        } else if (k->kind == B_CPU) {
            u32 a = attn();
            strcpy(t, a & 0x80 ? "68060" : a & 0x08 ? "68040" : a & 0x04 ? "68030" : "68020");
        } else if (k->kind == B_NETWORK) {
            int s = socket(AF_INET, SOCK_DGRAM, 0);
            strcpy(t, "net off");
            if (s >= 0) {
                static struct unetif rows[4];
                struct usockreq r;
                memset(&r, 0, sizeof r);
                r.buf = (u32)rows;
                r.len = sizeof rows;
                r.flags = sizeof rows[0];
                int n = sys_sock(SOCK_IFLIST | (u32)sizeof r << 16, s, &r);
                for (int j = 0; j < n; j++)
                    if (strcmp(rows[j].name, "lo")) {
                        u32 a = rows[j].addr;
                        sprintf(t, "%s %u.%u.%u.%u", rows[j].name, a >> 24, a >> 16 & 255, a >> 8 & 255, a & 255);
                        break;
                    }
                sys_close(s);
            }
        } else if (k->kind == B_CMD) {
            if (k->fd < 0)
                cmd_start(k);
            continue;
        } else
            continue;
        if (strcmp(t, k->text)) {
            strcpy(k->text, t);
            changed = 1;
        }
    }
    if (changed)
        bar_draw();
}

/* ---- menus ---- */
static int mopen = -2;                  /* -2 none, -1 the Zenix menu, else the active program's menu m */
static int mwin, mx, mw, mh, sel = -1, nitems, held, sticky, kbd;
static int mtitle_x;
static const char *item_text(int i, int *len)  /* the i-th item of the open menu */
{
    if (mopen == -1) {
        if (i < nmenu_items) { *len = (int)strlen(menu_items[i].label); return menu_items[i].label; }
        if (i > nmenu_items + 1)
            return 0;
        const char *t = i == nmenu_items ? "About Zenix" : "Log out";
        *len = (int)strlen(t);
        return t;
    }
    const char *s = menu_title(mopen);
    for (int k = 0; s && k <= i; k++) {
        s += seglen(s);
        if (*s != '\t')
            return 0;
        s++;
    }
    if (s)
        *len = seglen(s);
    return s;
}
static void menu_paint(void)
{
    if (!mwin)
        return;
    dpy_fill(mwin, 0, 0, mw, mh, pen(R_MENUFG));
    dpy_fill(mwin, 1, 1, mw - 2, mh - 2, pen(R_MENUBG));
    for (int i = 0; i < nitems; i++) {
        int n;
        const char *t = item_text(i, &n);
        int on = i == sel;
        if (on)
            dpy_fill(mwin, 1, 2 + 10 * i, mw - 2, 10, pen(R_SELBG));
        dpy_text(mwin, 8, 3 + 10 * i, on ? pen(R_SELFG) : pen(R_MENUFG), on ? pen(R_SELBG) : pen(R_MENUBG), 0, t, n);
    }
    dpy_flush();
}
int menu_open(void) { return mopen != -2; }
int menu_win(int id) { return id && id == mwin; }
void menu_close(void)
{
    if (mwin)
        dpy_destroy(mwin);
    mwin = 0;
    mopen = -2;
    sel = -1;
    held = sticky = 0;
    if (kbd) {
        kbd = 0;
        extern void grabs_set(int menu);
        grabs_set(0);
    }
}
static void menu_show(int which, int x)
{
    if (which == mopen)
        return;
    if (mwin)
        dpy_destroy(mwin);
    mwin = 0;
    mopen = which;
    sel = -1;
    nitems = 0;
    int maxl = 4, n;
    while (nitems < 32 && item_text(nitems, &n)) {
        if (n > maxl) maxl = n;
        nitems++;
    }
    mw = 8 * maxl + 16;
    mh = 10 * nitems + 4;
    mx = (x + mw > 640 ? 640 - mw : x) & ~7;   /* the server puts an inside on an 8-pixel column */
    mtitle_x = x;
    mwin = dpy_window(WK_BAR, "", mx, bar_h, mw, mh, WF_MOTION);
    if (mwin < 0)
        mwin = 0;
    menu_paint();
}
static void pick(void)
{
    int m = mopen, i = sel;
    menu_close();
    if (i < 0)
        return;
    if (m == -1) {
        if (i < nmenu_items) run(menu_items[i].cmd);
        else if (i == nmenu_items) about();
        else logout();
    } else if (focus_id > 0)
        dpy_call(DPY_WM_MENUPICK, (u32)focus_id, (u32)m, (u32)i, 0);
}
static int title_at(int x, int *which, int *tx)
{
    for (int i = 0; i < nhits; i++)
        if (x >= hits[i].x0 && x < hits[i].x1 && (hits[i].type == H_ZENIX || hits[i].type == H_MENU)) {
            *which = hits[i].type == H_ZENIX ? -1 : hits[i].arg;
            *tx = hits[i].x0;
            return 1;
        }
    return 0;
}
/* the pointer at x, y (screen) while a menu is held or shown */
static void track(int x, int y)
{
    int w, tx;
    if (y < bar_h && title_at(x, &w, &tx))
        menu_show(w, tx);
    int s = -1;
    if (mwin && x >= mx && x < mx + mw && y >= bar_h + 2 && y < bar_h + 2 + 10 * nitems)
        s = (y - bar_h - 2) / 10;
    if (s != sel) {
        sel = s;
        menu_paint();
    }
}
void menu_hold(int button)
{
    if (!(th.menus & M_HOLD) && button == 2)
        return;
    if (dpy_call(DPY_WM_PGRAB, (u32)bar_id, 0, 0, 0) < 0)
        return;
    held = button;
    sticky = 0;
}
void menu_keyboard(void)
{
    extern void grabs_set(int menu);
    if (menu_open()) {
        menu_close();
        return;
    }
    menu_show(-1, 4);
    sel = 0;
    menu_paint();
    kbd = 1;
    grabs_set(1);
}
void menu_key(int code)
{
    int w, tx;
    if (code == 0x45) { menu_close(); return; }                 /* Esc */
    if (code == 0x44 || code == 0x43) { pick(); return; }       /* Return */
    if (code == 0x4C || code == 0x4D) {                         /* Up, Down */
        sel = (sel + (code == 0x4D ? 1 : nitems - 1)) % (nitems ? nitems : 1);
        menu_paint();
        return;
    }
    if (code == 0x4E || code == 0x4F) {                         /* Right, Left: the next or previous menu */
        int k = -1, cur = -1;
        for (int i = 0; i < nhits; i++)
            if ((hits[i].type == H_ZENIX && mopen == -1) || (hits[i].type == H_MENU && hits[i].arg == mopen))
                cur = i;
        for (int d = 1; d < nhits && cur >= 0; d++) {
            int i = (cur + (code == 0x4E ? d : nhits - d)) % nhits;
            if (hits[i].type == H_ZENIX || hits[i].type == H_MENU) { k = i; break; }
        }
        if (k >= 0 && title_at(hits[k].x0 + 4, &w, &tx)) {
            menu_show(w, tx);
            sel = 0;
            menu_paint();
        }
    }
}

void bar_menus_changed(void)
{
    amenu_win = -1;
    bar_draw();
}

void bar_event(const struct dpyev *e)
{
    if (e->type == EV_EXPOSE) {
        if (e->win == mwin)
            menu_paint();
        else if (e->win == bar_id)
            bar_draw();
        return;
    }
    int x = e->x, y = e->y;
    if (e->win == mwin) {               /* the menu's own window: window-relative */
        x += mx;
        y += bar_h;
    }
    if (held || (mopen != -2 && e->type == EV_MOTION)) {
        if (e->type == EV_MOTION)
            track(x, y);
        if (e->type == EV_BUTTON && (e->code & 0x80) && held && (e->code & 0x7F) == held) {
            held = 0;
            int w, tx;
            if (sel >= 0)
                pick();
            else if (mopen != -2 && y < bar_h && title_at(x, &w, &tx) && (th.menus & M_CLICK))
                sticky = 1;             /* let go on its title: the menu stays, a click picks */
            else
                menu_close();
        }
        return;
    }
    if (e->type != EV_BUTTON || (e->code & 0x80))
        return;
    int b = e->code & 0x7F;
    if (b == 2) {
        menu_close();
        menu_hold(2);
        if (held)
            track(x, y);
        return;
    }
    if (b != 1)
        return;
    if (e->win == mwin) {               /* a click in a menu that stayed */
        track(x, y);
        pick();
        return;
    }
    for (int i = 0; i < nhits; i++) {
        if (x < hits[i].x0 || x >= hits[i].x1)
            continue;
        if ((hits[i].type == H_ZENIX || hits[i].type == H_MENU) && (th.menus & M_CLICK)) {
            if (sticky && ((hits[i].type == H_ZENIX && mopen == -1) || hits[i].arg == mopen)) {
                menu_close();
                return;
            }
            menu_close();
            menu_show(hits[i].type == H_ZENIX ? -1 : hits[i].arg, hits[i].x0);
            menu_hold(1);
            return;
        }
        menu_close();
        if (hits[i].type == H_WIN) {
            struct zw *w = zw_get(hits[i].arg);
            if (w && w->min) {
                w->min = 0;
                dpy_call(DPY_WM_SHOW, (u32)w->id, 0, 0, 0);
            }
            focus_win(hits[i].arg);
        } else if (hits[i].type == H_WS) {
            extern void workspace(int n);
            workspace(hits[i].arg);
        }
        return;
    }
    menu_close();
}

void bar_init(void)
{
    u32 now = sys_ticks();
    for (int i = 0; i < nblocks; i++)
        blocks[i].due = now;
    bar_tick();
}
