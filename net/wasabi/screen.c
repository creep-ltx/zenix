/* SPDX-License-Identifier: GPL-2.0-only */
/* wasabid's console commands (DESIGN D24 4, 6.1-6.3, 11): TEXT, GRAB, SUM, SCREEN, WINDOWS, KEY and INPUTS, over the
 * kernel's console operations (the OS layer's wsb_os_con_*). Answered with ERR while the kernel has none (decision 10). */
#include "wasabi_int.h"
#include "../../user/regex.h"

#define TEXT_HDR 16
#define FNV_BASIS 2166136261u
#define FNV_PRIME 16777619u

static int shown(struct wsb_con_head *h)
{
    for (int n = 1; n <= WSB_NCONS; n++)
        if (!wsb_os_con_read(n, h, 0, 0) && h->shown)
            return n;
    return -1;
}

void screen_free(struct conn *c)
{
    if (c->re) {
        re_free(c->re);
        c->re = 0;
    }
}

/* ---- TEXT ---- */
/* the grid into c->buf as TEXT's answer: the header (u16 rows, cols, cursor row, column; u32 changes; u8 shown,
 * pcfont, console, 0) and a glyph a cell, or with WX_CELLS the 3 bytes of each cell. 1 if a row matches c->re. */
static int text_read(struct conn *c, int *err)
{
    struct wsb_con_head h;
    uint32_t cells;
    int e = wsb_os_con_read(c->con, &h, 0, 0);
    *err = e;
    if (e)
        return 0;
    cells = (uint32_t)h.rows * h.cols;
    if (!cells || cells * 3 > WSB_GRID) {
        *err = -EINVAL;
        return 0;
    }
    if (!c->buf || cells > (uint32_t)c->lines) {            /* c->lines: the cells the block holds */
        if (c->buf)
            wsb_os_free(c->buf);
        c->lines = 0;
        if (!(c->buf = wsb_os_alloc(TEXT_HDR + 4 * cells))) { *err = -ENOMEM; return 0; }
        c->lines = (int)cells;
    }
    uint8_t *g3 = (uint8_t *)c->buf + TEXT_HDR + cells;     /* the 3-byte cells after where the glyphs go */
    if ((e = wsb_os_con_read(c->con, &h, g3, cells * 3))) {
        *err = e;
        return 0;
    }
    uint8_t *o = (uint8_t *)c->buf;
    put16(o, h.rows); put16(o + 2, h.cols); put16(o + 4, h.crow); put16(o + 6, h.ccol);
    put32(o + 8, h.changes);
    o[12] = h.shown; o[13] = h.pcfont; o[14] = (uint8_t)c->con; o[15] = 0;
    c->changes = h.changes;
    int hit = 0;
    if (c->xflags & 2) {                        /* WX_CELLS: the cells as they are */
        memmove(o + TEXT_HDR, g3, cells * 3);
        c->blen = TEXT_HDR + cells * 3;
    } else {
        for (uint32_t i = 0; i < cells; i++)
            o[TEXT_HDR + i] = g3[3 * i];
        c->blen = TEXT_HDR + cells;
    }
    if (c->re) {
        int caps[2 * RE_GROUPS];
        static char row[256];
        for (uint32_t r = 0; r < h.rows && !hit; r++) {
            uint32_t n = h.cols < sizeof row ? h.cols : sizeof row;
            for (uint32_t k = 0; k < n; k++)
                row[k] = (char)g3[3 * (r * h.cols + k)];
            while (n && row[n - 1] == ' ')
                n--;
            hit = re_exec(c->re, row, (int)n, 0, caps) == 1;
        }
    }
    return hit;
}

static void text_answer(struct conn *c, int timedout)
{
    char *b = c->buf;
    uint32_t n = c->blen;
    screen_free(c);
    c->buf = 0;
    conn_jobdone(c);
    qlines(c, b, n, timedout ? "timeout" : 0);
    c->xflags = 1;                              /* bytes, not lines */
}

void cmd_text(struct conn *c, const uint8_t *p, uint32_t len)
{
    uint32_t off = 0, flags, con, tmo;
    char pat[256];
    struct wsb_con_head h;
    int e;
    if (wsb_get32(p, len, &off, &flags) || wsb_get32(p, len, &off, &con) || wsb_get32(p, len, &off, &tmo) ||
        wsb_getstr(p, len, &off, pat, sizeof pat)) {
        qerr(c, 0, 0, "bad TEXT header");
        return;
    }
    if (con > WSB_NCONS) { qerr(c, 0, 0, "no such console (1-10, 0 the shown one)"); return; }
    if (tmo > WSB_TEXTMAX) { qerr(c, 0, 0, "a wait of at most 3600 s"); return; }
    if (!con && (int)(con = (uint32_t)shown(&h)) < 0) { qerr(c, 0, 0, "no console is shown"); return; }
    c->con = (int)con;
    c->xflags = flags & WX_CELLS ? 2 : 0;
    if (pat[0] && !(c->re = re_comp(pat, 0))) {
        qerr(c, 0, 0, "not an extended regular expression (or no memory for it)");
        return;
    }
    int hit = text_read(c, &e);
    if (e) {
        screen_free(c);
        conn_jobdone(c);
        qerr(c, e, 0, e == -ENXIO ? "that console was never used" : "cannot read the console");
        return;
    }
    if (!c->re || hit) { text_answer(c, 0); return; }
    if (!tmo) { text_answer(c, 1); return; }
    c->job = J_TEXT;
    c->xsize = g.now + tmo * 1000;              /* the deadline */
    c->until = g.now + WSB_LOG_MS;
}

/* ---- INPUTS ---- */
void cmd_inputs(struct conn *c, const uint8_t *p, uint32_t len)
{
    uint32_t n = len / 12, ticks = 0;
    struct wsb_con_head h;
    if (!len || len % 12 || n > WSB_INPUTS) {
        qerr(c, 0, 0, "INPUTS takes 1 to 340 records of 12 bytes");
        return;
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *r = p + 12 * i;
        uint32_t a = (uint32_t)r[4] << 8 | r[5];
        ticks += (uint32_t)r[2] << 8 | r[3];
        if (r[0] == 2) { qerr(c, 0, 0, "the mouse is not on Zenix (kind 2)"); return; }
        if (r[0] > 1) { qerr(c, 0, 0, "unknown kind of event (0 a raw key, 1 a character)"); return; }
        if (a > 0xFF || (r[0] == 1 && !a)) { qerr(c, 0, 0, "an event's key or character is out of range"); return; }
    }
    if (ticks > WSB_INPUTS_TICKS) { qerr(c, 0, 0, "the waits add up to more than 250 ticks (5 s)"); return; }
    if ((c->con = shown(&h)) < 0) { qerr(c, 0, 0, "no console is shown"); return; }
    if (!(c->buf = wsb_os_alloc(len))) { qerr(c, -ENOMEM, 0, "out of memory"); return; }
    memcpy(c->buf, p, len);
    c->blen = n;
    c->bpos = 0;
    c->until = g.now;
    c->job = J_INPUTS;
}

void screen_tick(struct conn *c, uint32_t *tmo)
{
    if (c->job == J_TEXT) {
        if (DUE(c->until)) {
            struct wsb_con_head h;
            int e = wsb_os_con_read(c->con, &h, 0, 0);
            if (!e && h.changes != c->changes && text_read(c, &e)) {
                text_answer(c, 0);
                return;
            }
            if (e) {
                screen_free(c);
                conn_jobdone(c);
                qerr(c, e, 0, "cannot read the console");
                return;
            }
            if (DUE(c->xsize)) {
                text_answer(c, 1);
                return;
            }
            c->until = g.now + WSB_LOG_MS;
        }
        TMO(tmo, LEFT(c->until));
    } else if (c->job == J_INPUTS) {
        while (DUE(c->until)) {
            if (c->bpos >= c->blen) {
                qok(c);
                conn_jobdone(c);
                return;
            }
            const uint8_t *r = (const uint8_t *)c->buf + 12 * c->bpos;
            uint8_t b = r[5];
            if (wsb_os_con_input(c->con, r[0] ? WSB_IN_TEXT : WSB_IN_RAW, &b, 1) != 1) {
                char m[48];
                struct wsb_buf w = { m, 0, sizeof m };
                bs(&w, "input event write failed at event ");
                bu(&w, c->bpos + 1);
                qerr(c, 0, 0, m);
                conn_jobdone(c);
                return;
            }
            c->bpos++;
            c->until = g.now + (((uint32_t)r[2] << 8 | r[3]) * 20u);
        }
        TMO(tmo, LEFT(c->until));
    }
}

/* ---- GRAB ---- */
static int name_con(const char *s)
{
    int n = 0;
    if (strncmp(s, "tty", 3) || !s[3])
        return -1;
    for (s += 3; *s; s++) {
        if (*s < '0' || *s > '9' || n > 10)
            return -1;
        n = n * 10 + (*s - '0');
    }
    return n >= 1 && n <= WSB_NCONS ? n : -1;
}

void cmd_grab(struct conn *c, const uint8_t *p, uint32_t len)
{
    uint32_t off = 0;
    char name[64];
    struct wsb_con_head h;
    struct wsb_con_disp d;
    int con, e;
    if (!len || wsb_getstr(p, len, &off, name, sizeof name))
        name[0] = 0;
    if ((con = shown(&h)) < 0) { qerr(c, 0, 0, "no console is shown"); return; }
    if (name[0] && name_con(name) != con) {
        qerr(c, 0, 0, name_con(name) < 0 ? "no screen with that name (tty1-tty10)" :
                                           "only the console on the display can be grabbed");
        return;
    }
    if (!(c->buf = wsb_os_alloc(WSB_SMALL / 3 + 1))) { qerr(c, -ENOMEM, 0, "out of memory"); return; }
    if ((e = wsb_os_con_line(con, 0, (uint8_t *)c->buf, WSB_SMALL / 3, &d)) || !d.width || !d.height ||
        d.width * 3u > WSB_SMALL) {
        conn_jobdone(c);
        qerr(c, e, 0, e ? "cannot read the display" : "the display is too wide to send a line at a time");
        return;
    }
    uint8_t hd[12];
    put32(hd, d.width);
    put32(hd + 4, d.height);
    put32(hd + 8, 3);
    q(c, WT_DATA, hd, 12);
    c->con = con;
    c->line = 0;
    c->lines = d.height;
    c->gen = d.generation;
    c->changes = h.changes;
    c->blen = d.width;
    c->job = J_GRAB;
}

void produce_screen(struct conn *c)
{
    struct wsb_con_disp d;
    uint32_t w = c->blen;
    while (c->line < c->lines) {
        uint32_t room = qroom(c), k = room / (w * 3), n = 0;
        if (!k)
            return;
        uint8_t *o = qres(c, room);
        while (k-- && c->line < c->lines) {
            int e = wsb_os_con_line(c->con, c->line, (uint8_t *)c->buf, w, &d);
            if (e || d.width != w) {
                if (n)
                    qcommit(c, WT_DATA, n);
                conn_jobdone(c);
                qerr(c, 0, 0, "the console was switched or changed its mode during the grab");
                return;
            }
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t *rgb = d.rgb[(uint8_t)c->buf[x] & 15];
                o[n++] = rgb[0];
                o[n++] = rgb[1];
                o[n++] = rgb[2];
            }
            c->line++;
        }
        qcommit(c, WT_DATA, n);
    }
    if (qroom(c) < 1)
        return;
    struct wsb_con_head h;
    if (wsb_os_con_read(c->con, &h, 0, 0) || h.changes != c->changes ||
        (!wsb_os_con_line(c->con, 0, (uint8_t *)c->buf, w, &d) && d.generation != c->gen))
        say("GRAB: the console changed while it was read (each line is right; the picture may mix moments)", 0);
    qend(c);
    conn_jobdone(c);
}

/* ---- SUM, SCREEN, WINDOWS ---- */
void cmd_sum(struct conn *c, const uint8_t *p, uint32_t len)
{
    struct wsb_con_head h;
    struct wsb_con_disp d;
    int con, x, y, w, hh, e;
    if (len < 8) { qerr(c, 0, 0, "bad SUM header"); return; }
    x = (int16_t)(p[0] << 8 | p[1]); y = (int16_t)(p[2] << 8 | p[3]);
    w = (int16_t)(p[4] << 8 | p[5]); hh = (int16_t)(p[6] << 8 | p[7]);
    if ((con = shown(&h)) < 0) { qerr(c, 0, 0, "no console is shown"); return; }
    uint8_t *pens = wsb_os_alloc(WSB_SMALL);
    if (!pens) { qerr(c, -ENOMEM, 0, "out of memory"); return; }
    if ((e = wsb_os_con_line(con, 0, pens, WSB_SMALL, &d))) {
        wsb_os_free(pens);
        qerr(c, e, 0, "cannot read the display");
        return;
    }
    if (!w || !hh) { x = y = 0; w = d.width; hh = d.height; }
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > d.width ? d.width : x + w,
        y1 = y + hh > d.height ? d.height : y + hh;
    if (x0 >= x1 || y0 >= y1) {
        wsb_os_free(pens);
        qerr(c, 0, 0, "the region is off the screen");
        return;
    }
    uint32_t s = FNV_BASIS, n = 0, np = d.depth >= 4 ? 16 : 1u << d.depth;
    for (uint32_t i = 0; i < np; i++)
        for (int k = 0; k < 3; k++)
            s = (s ^ d.rgb[i][k]) * FNV_PRIME;
    for (int ln = y0; ln < y1; ln++) {
        if ((e = wsb_os_con_line(con, ln, pens, WSB_SMALL, &d))) {
            wsb_os_free(pens);
            qerr(c, e, 0, "the console was switched during the sum");
            return;
        }
        for (int i = x0; i < x1; i++, n++)
            s = (s ^ pens[i]) * FNV_PRIME;
    }
    wsb_os_free(pens);
    uint8_t a[8];
    put32(a, s);
    put32(a + 4, n);
    q(c, WT_DATA, a, 8);
    qend(c);
}

static int display(struct wsb_con_disp *d, int con)
{
    uint8_t none;
    return wsb_os_con_line(con, 0, &none, 0, d);   /* the display's size and depth, no pens */
}

void cmd_screen(struct conn *c, const uint8_t *p, uint32_t len)
{
    uint32_t off = 0, flags = 0;
    char title[64];
    struct wsb_con_head h;
    struct wsb_con_disp d;
    int front, want = -1;
    title[0] = 0;
    if (!wsb_get32(p, len, &off, &flags) && wsb_getstr(p, len, &off, title, sizeof title))
        title[0] = 0;
    if ((front = shown(&h)) < 0 || display(&d, front)) { qerr(c, 0, 0, "no console is shown"); return; }
    char *t = wsb_os_alloc(512);
    struct wsb_buf b = { t, 0, 512 };
    if (!t) { qerr(c, -ENOMEM, 0, "out of memory"); return; }
    for (int pass = 0; pass < 2; pass++)
        for (int n = 1; n <= WSB_NCONS; n++) {
            if ((n == front) != !pass || wsb_os_con_read(n, &h, 0, 0))
                continue;
            bs(&b, "tty"); bu(&b, (uint32_t)n); bc(&b, ' ');
            bu(&b, d.width); bc(&b, ' ');
            bu(&b, (uint32_t)h.rows * 8); bc(&b, ' ');
            bu(&b, d.depth); bs(&b, " tty"); bu(&b, (uint32_t)n); bc(&b, '\n');
        }
    if (title[0] && (want = name_con(title)) > 0 && want != front) {
        uint8_t k[4] = { 0x66, (uint8_t)(0x50 + want - 1), (uint8_t)(0xD0 + want - 1), 0xE6 };   /* Amiga+F<n> */
        wsb_os_con_input(front, WSB_IN_RAW, k, 4);
    }
    qlines(c, t, b.n, title[0] && want < 0 ? "no screen with that title" : 0);
}

void cmd_windows(struct conn *c)
{
    struct wsb_con_head h;
    struct wsb_con_disp d;
    int con;
    if ((con = shown(&h)) < 0 || display(&d, con)) { qerr(c, 0, 0, "no console is shown"); return; }
    char *t = wsb_os_alloc(128);
    struct wsb_buf b = { t, 0, 128 };
    if (!t) { qerr(c, -ENOMEM, 0, "out of memory"); return; }
    bs(&b, "S\ttty"); bu(&b, (uint32_t)con); bs(&b, "\t0\t0\t");
    bu(&b, d.width); bc(&b, '\t'); bu(&b, d.height); bc(&b, '\t'); bu(&b, d.depth);
    bs(&b, "\ttty"); bu(&b, (uint32_t)con); bc(&b, '\n');
    qlines(c, t, b.n, 0);
}

/* ---- KEY ---- */
void cmd_key(struct conn *c, const uint8_t *p, uint32_t len)
{
    struct wsb_con_head h;
    uint8_t k[256];
    uint32_t mode, done = 0, total;
    int con;
    if (len < 2) { qerr(c, 0, 0, "bad KEY header"); return; }
    mode = (uint32_t)p[0] << 8 | p[1];
    p += 2;
    len -= 2;
    if (mode > 1) { qerr(c, 0, 0, "KEY mode 0 (raw keys) or 1 (text)"); return; }
    if (mode == 0 && len % 4) { qerr(c, 0, 0, "bad KEY events (code, qualifier pairs)"); return; }
    if (mode == 1 && len > WSB_KEYTEXT) { qerr(c, 0, 0, "at most 2048 bytes of text"); return; }
    total = mode ? len : len / 4;
    if (mode == 0)
        for (uint32_t i = 0; i < total; i++)
            if (p[4 * i]) { qerr(c, 0, 0, "not a raw key code (0-255)"); return; }
    if ((con = shown(&h)) < 0) { qerr(c, 0, 0, "no console is shown"); return; }
    while (done < total) {
        uint32_t n = total - done < sizeof k ? total - done : sizeof k;
        for (uint32_t i = 0; i < n; i++)
            k[i] = mode ? p[done + i] : p[4 * (done + i) + 1];
        int r = wsb_os_con_input(con, mode ? WSB_IN_TEXT : WSB_IN_RAW, k, (int)n);
        if (r < 0) { qerr(c, r, 0, "the console refused the input"); return; }
        done += (uint32_t)r;
        if ((uint32_t)r < n) {
            char m[80];
            struct wsb_buf b = { m, 0, sizeof m };
            bs(&b, mode ? "the console's input is full: " : "the keyboard took ");
            bu(&b, done); bs(&b, " of "); bu(&b, total);
            bs(&b, mode ? " bytes typed" : " keys");
            qerr(c, 0, 0, m);
            return;
        }
    }
    qok(c);
}
