/* SPDX-License-Identifier: GPL-2.0-only */
/* zdesk, the floating window manager of Zenix Desktop (DESIGN D38 step 5 and revision 2, docs/man/zdesk.txt).
 * Typed after logging in on a console, like startx: it starts the display server on that console (the service
 * "display") and becomes its window manager (DPY_WM_HELLO). It places new windows below the bar, raises and focuses
 * on a click, moves and resizes them (the server drags: an outline on a 68020/030, the whole window on a 68040/060),
 * closes, minimises and zooms them from their gadgets and from the keys of /etc/zdesk/keys, keeps nine workspaces,
 * and runs the bar and the desktop's icons. Leaving it (the Zenix menu's Log out, or SIGINT or SIGTERM) asks every
 * program on the desktop to end, and the console is text again. */
#include "zdesk.h"

struct zw zws[NZW];
int cur_ws = 1, focus_id, bar_id, desk_id, bar_h = 11, depth = 4, verbose;
volatile int quit;
static int ndrag = 0;                  /* the drag mode for DPY_WM_DRAG (DRAG_OUTLINE or 0) */
static int gad_img[NGAD];

struct zw *zw_get(int id)
{
    for (int i = 0; i < NZW; i++)
        if (zws[i].id && zws[i].id == id)
            return &zws[i];
    return 0;
}
static struct zw *zw_add(int id)
{
    struct zw *w = zw_get(id);
    if (w)
        return w;
    for (int i = 0; i < NZW; i++)
        if (!zws[i].id) {
            memset(&zws[i], 0, sizeof zws[i]);
            zws[i].id = id;
            zws[i].ws = cur_ws;
            return &zws[i];
        }
    return 0;
}
static void title_of(struct zw *w)
{
    struct dpyinfo in;
    if (dpy_info(w->id, &in) == 0) {
        memcpy(w->title, in.title, DPY_TITLE);
        w->pid = in.pid;
    }
}

void focus_win(int id)
{
    if (id > 0) {
        dpy_call(DPY_WM_STACK, (u32)id, 0, 0, 0);
        dpy_call(DPY_WM_FOCUS, (u32)id, 0, 0, 0);
    } else
        dpy_call(DPY_WM_FOCUS, 0, 0, 0, 0);
    focus_id = id;
    bar_draw();
}
/* the window to focus after this one went: the last in the list on this workspace, shown */
static void focus_other(int gone)
{
    int id = 0;
    for (int i = 0; i < NZW; i++)
        if (zws[i].id && zws[i].id != gone && zws[i].ws == cur_ws && !zws[i].min)
            id = zws[i].id;
    focus_win(id);
}

/* ---- running programs ---- */
void run(const char *cmd)
{
    if (!cmd || !*cmd)
        return;
    char *argv[] = { "/bin/sh", "-c", (char *)cmd, 0 };
    int pid = sys_spawn("/bin/sh", argv, 0);
    if (pid < 0)
        dprintf(2, "zdesk: %s: %s\n", cmd, zwhy("cannot start"));
}
static void reap(void)
{
    while (sys_waitnb(-1) >= 0)         /* the programs it started that ended */
        ;
}

void own_window(int id, const char *title, int x, int y, int w, int h)
{
    struct zw *z = zw_add(id);
    if (!z)
        return;
    z->own = 1;
    strncpy(z->title, title, DPY_TITLE - 1);
    dpy_call(DPY_WM_PLACE, (u32)id, (u32)(unsigned short)x << 16 | (unsigned short)y, (u32)w << 16 | (u32)h, 0);
    focus_win(id);
}

/* ---- placing ---- */
static int frame_w(int iw) { return th.st.left + iw + th.st.right; }
static int frame_h(int ih) { return th.st.top + ih + th.st.bottom; }
static void place_new(struct zw *w, const struct dpyev *e)
{
    static int casc;
    int iw = e->w, ih = e->h;
    if (frame_w(iw) > 640) iw = 640 - th.st.left - th.st.right;
    if (frame_h(ih) > 256 - bar_h) ih = 256 - bar_h - th.st.top - th.st.bottom;
    int x = e->x >= 0 ? e->x : 32 + 24 * (casc % 6);
    int y = e->y >= bar_h ? e->y : bar_h + 8 + 12 * (casc % 6);
    if (e->x < 0) casc++;
    if (x + frame_w(iw) > 640) x = 640 - frame_w(iw);
    if (y + frame_h(ih) > 256) y = 256 - frame_h(ih);
    if (x < 0) x = 0;
    if (y < bar_h) y = bar_h;
    dpy_call(DPY_WM_PLACE, (u32)w->id, (u32)(unsigned short)x << 16 | (unsigned short)y, (u32)iw << 16 | (u32)ih, 0);
}

/* ---- workspaces (D38 revision 2) ---- */
void workspace(int n)
{
    if (n < 1 || n > 9 || n == cur_ws)
        return;
    menu_close();
    for (int i = 0; i < NZW; i++) {
        struct zw *w = &zws[i];
        if (!w->id || w->min)
            continue;
        if (w->ws == cur_ws)
            dpy_call(DPY_WM_HIDE, (u32)w->id, HIDE_WORKSPACE, 0, 0);
    }
    cur_ws = n;
    for (int i = 0; i < NZW; i++) {
        struct zw *w = &zws[i];
        if (w->id && !w->min && w->ws == cur_ws)
            dpy_call(DPY_WM_SHOW, (u32)w->id, 0, 0, 0);
    }
    dprintf(1, "zdesk: workspace %d\n", n);
    focus_other(0);
}
static void sendto(int n)
{
    struct zw *w = zw_get(focus_id);
    if (!w || n < 1 || n > 9 || n == w->ws)
        return;
    w->ws = n;
    if (n != cur_ws)
        dpy_call(DPY_WM_HIDE, (u32)w->id, HIDE_WORKSPACE, 0, 0);
    focus_other(w->id);
}
static void minimise(int id)
{
    struct zw *w = zw_get(id);
    if (!w || w->min)
        return;
    w->min = 1;
    dpy_call(DPY_WM_HIDE, (u32)id, HIDE_MINIMISE, 0, 0);
    focus_other(id);
}
static void zoom(int id)
{
    struct zw *w = zw_get(id);
    struct dpyinfo in;
    if (!w || dpy_info(id, &in) < 0)
        return;
    if (w->zoomed) {
        dpy_call(DPY_WM_PLACE, (u32)id, (u32)(unsigned short)w->ox << 16 | (unsigned short)w->oy, (u32)w->ow << 16 | w->oh, 0);
        w->zoomed = 0;
    } else {
        w->ox = in.x; w->oy = in.y; w->ow = in.w; w->oh = in.h;
        int iw = 640 - th.st.left - th.st.right, ih = 256 - bar_h - th.st.top - th.st.bottom;
        dpy_call(DPY_WM_PLACE, (u32)id, (u32)bar_h, (u32)iw << 16 | (u32)ih, 0);
        w->zoomed = 1;
    }
}
static void nudge(int id, int dx, int dy, int dw, int dh)
{
    struct dpyinfo in;
    if (id <= 0 || dpy_info(id, &in) < 0)
        return;
    int x = in.x + dx, y = in.y + dy, w = in.w + dw, h = in.h + dh;
    if (y < bar_h) y = bar_h;
    if (w < 16) w = 16;
    if (h < 8) h = 8;
    dpy_call(DPY_WM_PLACE, (u32)id, (u32)(unsigned short)x << 16 | (unsigned short)y, (u32)w << 16 | (u32)h, 0);
}
static void next_window(void)
{
    int first = 0, after = 0, seen = 0;
    for (int i = 0; i < NZW; i++) {
        struct zw *w = &zws[i];
        if (!w->id || w->ws != cur_ws || w->min || w->id == bar_id || w->id == desk_id || menu_win(w->id))
            continue;
        if (!first) first = w->id;
        if (seen && !after) after = w->id;
        if (w->id == focus_id) seen = 1;
    }
    if (after || first)
        focus_win(after ? after : first);
}

/* ---- keys: the grabs, and the menu's own while it has the keyboard ---- */
void grabs_set(int menu)
{
    static struct dpykey g[NKEY + 8];
    int n = 0;
    for (int i = 0; i < nkeys; i++)
        g[n++] = (struct dpykey){ keys[i].code, 0, keys[i].mask, keys[i].qual, (unsigned short)i };
    if (menu) {
        static const u8 mk[] = { 0x4C, 0x4D, 0x4E, 0x4F, 0x44, 0x43, 0x45 };
        for (u32 i = 0; i < sizeof mk; i++)
            g[n++] = (struct dpykey){ mk[i], 0, 0, 0, (unsigned short)(1000 + mk[i]) };
    }
    dpy_call(DPY_WM_GRAB, (u32)g, (u32)n, 0, 0);
}
static void action(int k)
{
    if (k >= 1000) {
        menu_key(k - 1000);
        return;
    }
    if (k < 0 || k >= nkeys)
        return;
    struct key *y = &keys[k];
    switch (y->action) {
    case A_TERMINAL: run("term"); break;
    case A_RUN: run(y->cmd); break;
    case A_CLOSE: if (focus_id > 0) dpy_call(DPY_WM_CLOSE, (u32)focus_id, 0, 0, 0); break;
    case A_MIN: if (focus_id > 0) minimise(focus_id); break;
    case A_NEXT: next_window(); break;
    case A_MENU: menu_keyboard(); break;
    case A_MOVE: nudge(focus_id, y->a, y->b, 0, 0); break;
    case A_RESIZE: nudge(focus_id, 0, 0, y->a, y->b); break;
    case A_WORKSPACE: workspace(y->a); break;
    case A_SENDTO: sendto(y->a); break;
    case A_LOGOUT: logout(); break;
    case A_ABOUT: about(); break;
    case A_ZOOM: if (focus_id > 0) zoom(focus_id); break;
    }
}

/* ---- the server's events ---- */
static int own(int id) { return id == bar_id || id == desk_id || menu_win(id) || (about_id() && id == about_id()); }
static void frame_click(const struct dpyev *e)
{
    if (e->arg == 2) {                  /* the right button on a frame: the menus, the Workbench way */
        menu_hold(2);
        return;
    }
    if (e->arg != 1)
        return;
    switch (e->code) {
    case FP_CLOSE: dpy_call(DPY_WM_CLOSE, e->win, 0, 0, 0); break;
    case FP_MIN: minimise(e->win); break;
    case FP_ZOOM: zoom(e->win); break;
    case FP_SIZE: focus_win(e->win); dpy_call(DPY_WM_DRAG, e->win, DRAG_RESIZE | (u32)ndrag, 0, 0); break;
    case FP_TITLE: focus_win(e->win); dpy_call(DPY_WM_DRAG, e->win, DRAG_MOVE | (u32)ndrag, 0, 0); break;
    default: focus_win(e->win); break;
    }
}
static void event(const struct dpyev *e)
{
    if (verbose && e->type != EV_EXPOSE)
        dprintf(1, "zdesk: event %d win %d code %d arg %d at %d,%d\n", e->type, e->win, e->code, e->arg, e->x, e->y);
    switch (e->type) {
    case EV_WM_NEW: {
        struct zw *w = zw_add(e->win);
        if (!w)
            break;
        title_of(w);
        if (e->arg & 0x100) {           /* there before zdesk: on this workspace as it is */
            focus_win(w->id);
            break;
        }
        place_new(w, e);
        dpy_call(DPY_WM_SHOW, (u32)w->id, 0, 0, 0);
        focus_win(w->id);
        break;
    }
    case EV_WM_GONE: {
        struct zw *w = zw_get(e->win);
        if (w)
            w->id = 0;
        if (focus_id == e->win)
            focus_other(e->win);
        else
            bar_draw();
        break;
    }
    case EV_WM_TITLE: {
        struct zw *w = zw_get(e->win);
        if (w) {
            title_of(w);
            bar_draw();
        }
        break;
    }
    case EV_WM_MENU:
        if (e->win == focus_id) {
            extern void bar_menus_changed(void);
            bar_menus_changed();
        }
        break;
    case EV_WM_SIZE:
        dpy_call(DPY_WM_PLACE, e->win, (u32)(unsigned short)e->x << 16 | (unsigned short)e->y, (u32)e->w << 16 | e->h, 0);
        break;
    case EV_WM_FRAME:
        menu_close();
        frame_click(e);
        break;
    case EV_WM_CLICK:
        if (own(e->win))
            break;                      /* zdesk's own windows: their EV_BUTTON */
        menu_close();
        if (e->code == 2) {
            menu_hold(2);
            break;
        }
        if (e->win != focus_id)
            focus_win(e->win);
        break;
    case EV_WM_KEY:
        action(e->arg);
        break;
    case EV_WM_DRAGEND:
        break;
    default:                            /* events for zdesk's own windows */
        if (e->win == desk_id) {
            if (e->type == EV_BUTTON && (e->code & 0x7F) == 2 && !(e->code & 0x80)) {
                menu_close();
                menu_hold(2);
            } else {
                if (e->type == EV_BUTTON && !(e->code & 0x80))
                    menu_close();       /* a press elsewhere closes a menu (a release may be a held menu's own) */
                desk_event(e);
            }
        } else if (e->win == bar_id || menu_win(e->win))
            bar_event(e);
        else if (about_id() && e->win == about_id())
            about_event(e);
    }
}

/* ---- leaving ---- */
void logout(void)
{
    quit = 1;
}
static void end_session(void)
{
    menu_close();
    for (int i = 0; i < NZW; i++)       /* every program on the desktop is asked to end */
        if (zws[i].id && !own(zws[i].id) && zws[i].pid > 0 && zws[i].pid != sys_getpid())
            sys_killsig(zws[i].pid, SIGTERM);
    dpy_close();
    for (int t = 0; t < 20; t++) {      /* a moment for them, then the end: the server follows its last client */
        reap();
        sys_sleep(5);
    }
}
static void on_signal(int sig) { (void)sig; quit = 1; }

int main(int argc, char **argv)
{
    const char *tname = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-theme") && i + 1 < argc) tname = argv[++i];
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else { dprintf(2, "usage: zdesk [-v] [-theme NAME]\n"); return 2; }
    if (conf_load(tname) < 0)
        return 1;
    int r = dpy_open("zdesk");
    if (r < 0) {
        dprintf(2, "zdesk: the display server: %s\n", zstrerror(r) ? zstrerror(r) : "not reachable");
        return 1;
    }
    depth = dpy_depth;
    if ((r = dpy_call(DPY_WM_HELLO, 0, 0, 0, 0)) < 0) {
        dprintf(2, "zdesk: another window manager runs (%d)\n", r);
        dpy_close();
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    /* the style: the theme's pens as this screen shows them, its gadgets as images */
    struct dpystyle st = th.st;
    if (depth < 4) {
        for (int i = 0; i < 2; i++) {
            st.border[i] = th.map8[st.border[i] & 15];
            st.bar[i] = th.map8[st.bar[i] & 15];
            st.text[i] = th.map8[st.text[i] & 15];
        }
        st.light = th.map8[st.light & 15]; st.dark = th.map8[st.dark & 15];
        st.shadowc = th.map8[st.shadowc & 15]; st.stripes = st.stripes ? th.map8[st.stripes & 15] : 0;
    }
    for (int g = 0; g < NGAD; g++) {
        if (!th.gadh[g])
            continue;
        static u8 buf[2 * 12 * 5];
        int wpr = 2;
        memset(buf, 0, sizeof buf);
        for (int y = 0; y < th.gadh[g]; y++)
            for (int x = 0; x < th.gadw[g] && th.gad[g][y][x]; x++) {
                char c = th.gad[g][y][x];
                if (c == '.' || c == ' ')
                    continue;
                int p = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 1;
                if (depth < 4) p = th.map8[p];
                for (int pl = 0; pl < depth; pl++)
                    if (p >> pl & 1)
                        buf[(pl * th.gadh[g] + y) * wpr + (x >> 3)] |= (u8)(0x80 >> (x & 7));
                buf[(depth * th.gadh[g] + y) * wpr + (x >> 3)] |= (u8)(0x80 >> (x & 7));
            }
        gad_img[g] = dpy_image_new(th.gadw[g], th.gadh[g], depth, IMG_MASK, buf);
        st.gad[g] = (unsigned short)(gad_img[g] > 0 ? gad_img[g] : 0);
    }
    if (th.drag < 0) {                  /* D38 question 3: by the CPU */
        u32 zi[7];
        st.drag = sys3(93, 0, (u32)zi, sizeof zi) == 0 && (zi[2] & 0x88) ? 0 : DRAG_OUTLINE;
    } else
        st.drag = (u8)th.drag;
    ndrag = st.drag;
    if (depth < 4)
        for (int i = 8; i < 16; i++) st.palette[i] = st.palette[th.map8[i]];
    dpy_call(DPY_WM_STYLE, (u32)&st, 0, 0, 0);
    desk_init();
    bar_id = dpy_window(WK_BAR, "", 0, 0, 640, bar_h, 0);
    grabs_set(0);
    bar_init();
    bar_draw();
    dprintf(1, "zdesk: running, theme %s, %d keys, %d icons, drag %s\n", th.name, nkeys, nicons,
            ndrag ? "outline" : "whole");
    while (!quit) {
        struct pollfd p[17];
        int n = 1;
        p[0].fd = dpy_port();
        p[0].events = POLLPORT | POLLRDBAND;
        p[0].revents = 0;
        bar_poll(p, &n);
        sys_evwait(p, (u32)n, 50);
        struct dpyev e;
        int got;
        while ((got = dpy_next(&e, 0)) > 0)
            event(&e);
        if (got < 0)
            break;                      /* the server is gone */
        if (dpy_lost()) {               /* events were dropped: the windows' state asked again */
            for (int i = 0; i < NZW; i++)
                if (zws[i].id && !own(zws[i].id))
                    title_of(&zws[i]);
            bar_draw();
        }
        bar_tick();
        reap();
    }
    dprintf(1, "zdesk: leaving\n");
    end_session();
    return 0;
}
