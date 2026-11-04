/* SPDX-License-Identifier: GPL-2.0-only */
/* zdesk's desktop (D38 7 step 5): the window at the back with the icons of /etc/zdesk/icons (a double click starts
 * the command), and the About window. */
#include "zdesk.h"

/* 24 x 12 icons (the pixels are tall: 24 wide looks square): k outline, f face, a accent, w light, d dark */
static const struct { const char *name; const char *rows[12]; } arts[] = {
    { "term", { "kkkkkkkkkkkkkkkkkkkkkkkk", "kddddddddddddddddddddddk", "kdadddddddddddddddddddk.",
                "kddaddddddddddddddddddk.", "kdadddwwwwddddddddddddk.", "kddddddddddddddddddddddk",
                "kddddddddddddddddddddddk", "kkkkkkkkkkkkkkkkkkkkkkkk", ".........kkkkkk.........",
                "......kkkffffffkkk......", ".....kffffffffffffk.....", ".....kkkkkkkkkkkkkk....." } },
    { "folder", { "..kkkkkkkk..............", ".kaaaaaaaak.............", "kkkkkkkkkkkkkkkkkkkkkkk.",
                  "kwwwwwwwwwwwwwwwwwwwwwak", "kwaaaaaaaaaaaaaaaaaaaaak", "kaaaaaaaaaaaaaaaaaaaaaak",
                  "kaaaaaaaaaaaaaaaaaaaaaak", "kaaaaaaaaaaaaaaaaaaaaaak", "kaaaaaaaaaaaaaaaaaaaaaak",
                  "kaaaaaaaaaaaaaaaaaaaaaak", "kaaaaaaaaaaaaaaaaaaaaaak", "kkkkkkkkkkkkkkkkkkkkkkkk" } },
    { "gear", { "..........kkkk..........", "....kkk..kffffk..kkk....", "...kfffkkffffffkkfffk...",
                "....kffffffffffffffk....", "..kkfffffkkkkkkfffffkk..", "kkffffffk......kffffffkk",
                "kkffffffk......kffffffkk", "..kkfffffkkkkkkfffffkk..", "....kffffffffffffffk....",
                "...kfffkkffffffkkfffk...", "....kkk..kffffk..kkk....", "..........kkkk.........." } },
    { "disk", { "kkkkkkkkkkkkkkkkkkkkkkk.", "kffffkwwwwwwwwwwwwkffffk", "kffffkwwwwwwwwwwwwkffffk",
                "kffffkwwwwwwwwwwwwkffffk", "kffffkkkkkkkkkkkkkkffffk", "kffffffffffffffffffffffk",
                "kfffkkkkkkkkkkkkkkkkfffk", "kfffkddddddaaddddddkfffk", "kfffkddddddaaddddddkfffk",
                "kfffkddddddddddddddkfffk", "kfffkddddddddddddddkfffk", "kkkkkkkkkkkkkkkkkkkkkkkk" } },
};
#define NART (sizeof arts / sizeof arts[0])
static int art_img[NART];
static u32 last_click;
static int last_icon = -1, sel_icon = -1;

/* planar rows of 2 words (24 pixels in 32), plane after plane, then the mask */
void icon_image(u8 *planes, const char *const *rows, int nrows, const u8 *pens, int dep)
{
    memset(planes, 0, (u32)(4 * nrows * (dep + 1)));
    for (int y = 0; y < nrows; y++)
        for (int x = 0; rows[y][x] && x < 32; x++) {
            const char *k = strchr("kfawd", rows[y][x]);
            if (!k)
                continue;
            int p = pens[k - "kfawd"];
            u8 m = (u8)(0x80 >> (x & 7));
            for (int pl = 0; pl < dep; pl++)
                if (p >> pl & 1)
                    planes[(pl * nrows + y) * 4 + (x >> 3)] |= m;
            planes[(dep * nrows + y) * 4 + (x >> 3)] |= m;
        }
}

static int icon_y(int i) { return bar_h + 16 + 40 * i; }

void desk_draw(int x, int y, int w, int h)
{
    dpy_clip(desk_id, x, y, w, h);
    dpy_fill(desk_id, 0, 0, 640, 256, pen(R_DESK));
    for (int i = 0; i < nicons; i++) {
        int a = -1;
        for (u32 k = 0; k < NART; k++)
            if (!strcmp(icons[i].art, arts[k].name)) a = (int)k;
        int iy = icon_y(i), cx = 40;
        if (a >= 0 && art_img[a] > 0)
            dpy_image(desk_id, art_img[a], 0, 0, 24, 12, cx - 12, iy);
        const char *l = icons[i].label;
        int n = (int)strlen(l), lx = cx - n * 4;
        int bg = pen(R_LABELBG) == 255 ? pen(R_DESK) : pen(R_LABELBG);
        if (i == sel_icon)
            dpy_fill(desk_id, lx - 2, iy + 15, n * 8 + 4, 10, pen(R_SELBG));
        else if (pen(R_LABELBG) != 255)
            dpy_fill(desk_id, lx - 2, iy + 15, n * 8 + 4, 10, bg);
        dpy_text(desk_id, lx, iy + 16, i == sel_icon ? pen(R_SELFG) : pen(R_ICONFG), i == sel_icon ? pen(R_SELBG) : bg,
                 0, l, n);
    }
    dpy_clip(desk_id, 0, 0, 0, 0);
    dpy_flush();
}

void desk_init(void)
{
    u8 pens[5] = { (u8)pen(R_IK), (u8)pen(R_IF), (u8)pen(R_IA), (u8)pen(R_IW), (u8)pen(R_ID) };
    static u8 buf[4 * 12 * 5];
    for (u32 k = 0; k < NART; k++) {
        icon_image(buf, arts[k].rows, 12, pens, depth);
        art_img[k] = dpy_image_new(24, 12, depth, IMG_MASK, buf);
    }
    desk_id = dpy_window(WK_DESKTOP, "", 0, 0, 640, 256, 0);
}

/* a click on an icon selects it, a second within half a second starts it */
void desk_event(const struct dpyev *e)
{
    if (e->type == EV_EXPOSE) {
        desk_draw(e->x, e->y, e->w, e->h);
        return;
    }
    if (e->type != EV_BUTTON || e->code != 1)
        return;
    int hitn = -1;
    for (int i = 0; i < nicons; i++)
        if (e->x >= 8 && e->x < 72 && e->y >= icon_y(i) && e->y < icon_y(i) + 26)
            hitn = i;
    u32 now = sys_ticks();
    if (hitn >= 0 && hitn == last_icon && (u32)(now - last_click) < 50) {
        run(icons[hitn].cmd);
        last_icon = -1;
    } else
        last_icon = hitn;
    last_click = now;
    if (sel_icon != hitn) {
        sel_icon = hitn;
        desk_draw(0, 0, 640, 256);
    }
}

/* ---- About ---- */
static int ab_id;
int about_id(void) { return ab_id; }
static void about_draw(void)
{
    char l[5][48];
    struct uupdate u;
    if (sys_update(UPD_INFO, (u32)&u, sizeof u) < 0) strcpy(u.kver, "?");
    u.kver[15] = 0;
    sprintf(l[0], "Zenix %s", u.kver);
    u32 zi[7], mi[4] = { 0, 0, 0, 0 };
    u32 a = sys3(93, 0, (u32)zi, sizeof zi) == 0 ? zi[2] : 0;
    sprintf(l[1], "%s, %s", a & 0x80 ? "68060" : a & 0x08 ? "68040" : a & 0x04 ? "68030" : "68020",
            depth >= 4 ? "AGA" : "OCS/ECS");
    sys_meminfo(mi);
    sprintf(l[2], "Chip %uK free", mi[0] >> 10);
    sprintf(l[3], "Fast %uK free", mi[1] >> 10);
    u32 up = (u32)sys3(20, 1, 0, 0);
    sprintf(l[4], "Up %u:%02u", up / 3600, up / 60 % 60);
    int bg = pen(R_WINBG), fg = pen(R_WINFG);
    dpy_fill(ab_id, 0, 0, 240, 64, bg);
    for (int i = 0; i < 5; i++)
        dpy_text(ab_id, 8, 4 + 9 * i, fg, bg, i ? 0 : TA_BOLD, l[i], (int)strlen(l[i]));
    dpy_fill(ab_id, 176, 48, 48, 12, pen(R_BTN));
    dpy_fill(ab_id, 176, 48, 48, 1, fg);
    dpy_fill(ab_id, 176, 59, 48, 1, fg);
    dpy_fill(ab_id, 176, 48, 1, 12, fg);
    dpy_fill(ab_id, 223, 48, 1, 12, fg);
    dpy_text(ab_id, 192, 50, pen(R_BTNFG), pen(R_BTN), 0, "OK", 2);
    dpy_flush();
}
void about(void)
{
    if (ab_id > 0) {
        focus_win(ab_id);
        return;
    }
    ab_id = dpy_window(WK_NORMAL, "About Zenix", -1, -1, 240, 64, 0);
    if (ab_id < 0)
        ab_id = 0;
    else
        own_window(ab_id, "About Zenix", 200, 80, 240, 64);
}
void about_event(const struct dpyev *e)
{
    if (e->type == EV_EXPOSE || e->type == EV_RESIZE)
        about_draw();
    else if (e->type == EV_CLOSE || (e->type == EV_BUTTON && e->code == 1 && e->x >= 176 && e->x < 224 &&
                                     e->y >= 48 && e->y < 60) ||
             (e->type == EV_KEY && !(e->code & 0x80) && (e->ch[0] == '\r' || e->code == 0x45))) {
        dpy_destroy(ab_id);
        ab_id = 0;
    }
}
