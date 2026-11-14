/* SPDX-License-Identifier: GPL-2.0-only */
/* Lua's "zenix" module (roadmap block 7 item 7, DESIGN D52 section 8; the user's page is docs/man/zenix-lua.txt):
 * small games in Lua with a desktop window and sound. Linked into /bin/lua and registered in package.preload by a
 * wrapper around luaL_openlibs (the linker's --wrap, so Lua's own files stay unchanged): `local z = require "zenix"`.
 *
 *  - The window is a client of the display server (DESIGN D38, display/lib/libdisplay.c): fills, lines (runs of
 *    fills), text in the 8 x 8 font, pictures (planar images the server keeps, with a mask for see-through pixels);
 *    keys, the mouse, the close gadget and the rest as events. Drawing is queued and sent at flush (and before every
 *    wait for an event).
 *  - Sound goes to the sound server sndd (DESIGN D40, user/sndlib.c) as one frames stream: Paula's four channels,
 *    the samples in one bank in chip RAM. A module (ProTracker) plays through the replayer modplay uses
 *    (user/modrep.c), its frames made here a little ahead of time whenever the program waits; a sample started
 *    with play takes one channel for its length (a module's notes on that channel wait meanwhile), as games on the
 *    Amiga have always done it.
 *  - The time in milliseconds and waits for the game's loop, the sound kept fed while waiting.
 * Nothing is drawn or played without the program's asking; what the program never asks for (the display, the sound
 * server) is never opened. */
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"
#define ZENIX_LIBC 1
#include "usys.h"
#include "display.h"
#include "sndlib.h"
#include "modrep.h"

#define WINMT "zenix.window"
#define IMGMT "zenix.image"
#define SMPMT "zenix.sample"

/* ---- the display ---- */
static int dpy_up;                      /* 1 connected, -errno the connection failed */
static unsigned short pal[16];
static int npens = 16;

static const char *dpy_why(int e)
{
    if (e == -ECONNREFUSED || e == -ENOENT)
        return "no display server (start the desktop: zdesk)";
    if (e == -EPERM || e == -EACCES)
        return "the desktop belongs to another user";
    return strerror(-e);
}
static int connect(lua_State *L, int raise)
{
    if (dpy_up > 0)
        return 1;
    int r = dpy_open("lua");
    if (r < 0) {
        dpy_up = r;                     /* the reason, for z.window's answer (never a second dpy_open) */
        if (raise)
            luaL_error(L, "zenix: %s", dpy_why(r));
        return 0;
    }
    dpy_up = 1;
    int d = dpy_palette(pal, 16);
    npens = d > 0 && d < 4 ? 1 << d : 16;
    return 1;
}

struct lwin { int id; int w, h; };
static struct lwin *towin(lua_State *L, int i)
{
    struct lwin *w = luaL_checkudata(L, i, WINMT);
    if (!w->id)
        luaL_error(L, "zenix: the window is closed");
    return w;
}
static int pen(lua_State *L, int i) { return (int)(luaL_checkinteger(L, i) & 15); }
static int coord(lua_State *L, int i)
{
    lua_Integer v = luaL_checkinteger(L, i);
    return v < -16384 ? -16384 : v > 16383 ? 16383 : (int)v;   /* the protocol's coordinates are 16-bit */
}

/* z.window(title, w, h [, {x =, y =, motion =}]) -> window; nil, why */
static int z_window(lua_State *L)
{
    const char *title = luaL_checkstring(L, 1);
    lua_Integer w = luaL_checkinteger(L, 2), h = luaL_checkinteger(L, 3);
    luaL_argcheck(L, w >= 8 && w <= 2048, 2, "width 8 .. 2048");
    luaL_argcheck(L, h >= 8 && h <= 2048, 3, "height 8 .. 2048");
    int x = -1, y = -1, flags = 0;
    if (lua_istable(L, 4)) {
        if (lua_getfield(L, 4, "x") == LUA_TNUMBER) x = (int)lua_tointeger(L, -1);
        if (lua_getfield(L, 4, "y") == LUA_TNUMBER) y = (int)lua_tointeger(L, -1);
        if (lua_getfield(L, 4, "motion"), lua_toboolean(L, -1)) flags |= WF_MOTION;
        lua_pop(L, 3);
    }
    if (!connect(L, 0)) {
        lua_pushnil(L);
        lua_pushstring(L, dpy_why(dpy_up));
        return 2;
    }
    struct dpywin d;
    memset(&d, 0, sizeof d);
    d.kind = WK_NORMAL;
    d.flags = (unsigned short)flags;
    d.x = (short)x; d.y = (short)y; d.w = (unsigned short)w; d.h = (unsigned short)h;
    strncpy(d.title, title, DPY_TITLE - 1);
    int id = dpy_newwin(&d);
    if (id < 0) {
        lua_pushnil(L);
        lua_pushstring(L, dpy_why(id));
        return 2;
    }
    struct lwin *lw = lua_newuserdatauv(L, sizeof *lw, 0);
    lw->id = id; lw->w = d.w; lw->h = d.h;
    luaL_setmetatable(L, WINMT);
    lua_getfield(L, LUA_REGISTRYINDEX, "zenix.windows");      /* id -> window, for the events */
    lua_pushvalue(L, -2);
    lua_rawseti(L, -2, id);
    lua_pop(L, 1);
    return 1;
}
static int w_fill(lua_State *L)
{
    struct lwin *w = towin(L, 1);
    lua_Integer cw = luaL_checkinteger(L, 4), ch = luaL_checkinteger(L, 5);
    if (cw > 0 && ch > 0)
        dpy_fill(w->id, coord(L, 2), coord(L, 3), cw > 4096 ? 4096 : (int)cw, ch > 4096 ? 4096 : (int)ch, pen(L, 6));
    return 0;
}
static int w_clear(lua_State *L)
{
    struct lwin *w = towin(L, 1);
    dpy_fill(w->id, 0, 0, w->w, w->h, (int)(luaL_optinteger(L, 2, 0) & 15));
    return 0;
}
/* a line as runs of fills: across for a flat one, down for a steep one (Bresenham) */
static int w_line(lua_State *L)
{
    struct lwin *w = towin(L, 1);
    int x0 = coord(L, 2), y0 = coord(L, 3), x1 = coord(L, 4), y1 = coord(L, 5), p = pen(L, 6);
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x1 >= x0 ? 1 : -1, sy = y1 >= y0 ? 1 : -1;
    if (dx >= dy) {                     /* flat: a run along x for each y */
        int e = dx / 2, rs = x0, x = x0, y = y0;
        for (int i = 0; i <= dx; i++, x += sx) {
            e -= dy;
            if (e < 0 || i == dx) {
                int a = rs < x ? rs : x, b = rs < x ? x : rs;
                if (y >= 0 && y < w->h && b >= 0 && a < w->w)   /* outside the window: nothing queued */
                    dpy_fill(w->id, a, y, b - a + 1, 1, p);
                if (e < 0) { y += sy; e += dx; }
                rs = x + sx;
            }
        }
    } else {                            /* steep: a run along y for each x */
        int e = dy / 2, rs = y0, x = x0, y = y0;
        for (int i = 0; i <= dy; i++, y += sy) {
            e -= dx;
            if (e < 0 || i == dy) {
                int a = rs < y ? rs : y, b = rs < y ? y : rs;
                if (x >= 0 && x < w->w && b >= 0 && a < w->h)
                    dpy_fill(w->id, x, a, 1, b - a + 1, p);
                if (e < 0) { x += sx; e += dy; }
                rs = y + sy;
            }
        }
    }
    return 0;
}
/* win:text(x, y, s, fg [, bg]): the 8 x 8 font, a cell 8 pixels a character */
static int w_text(lua_State *L)
{
    struct lwin *w = towin(L, 1);
    size_t n;
    const char *s = luaL_checklstring(L, 4, &n);
    int fg = pen(L, 5), bg = (int)(luaL_optinteger(L, 6, 0) & 15);
    if (n > 1024)
        return luaL_error(L, "zenix: text: %d bytes, at most 1024 at once", (int)n);
    dpy_text(w->id, coord(L, 2), coord(L, 3), fg, bg, 0, s, (int)n);
    return 0;
}
struct limg { int id, w, h; };
/* win:blit(img, x, y [, sx, sy, w, h]) */
static int w_blit(lua_State *L)
{
    struct lwin *w = towin(L, 1);
    struct limg *im = luaL_checkudata(L, 2, IMGMT);
    if (!im->id || dpy_up <= 0)
        return luaL_error(L, "zenix: the picture is freed");
    int sx = (int)luaL_optinteger(L, 5, 0), sy = (int)luaL_optinteger(L, 6, 0);
    int bw = (int)luaL_optinteger(L, 7, im->w - sx), bh = (int)luaL_optinteger(L, 8, im->h - sy);
    if (sx < 0 || sy < 0 || bw <= 0 || bh <= 0 || sx > im->w || bw > im->w - sx || sy > im->h || bh > im->h - sy)
        return luaL_error(L, "zenix: blit: the part %d,%d %dx%d is not inside the %dx%d picture", sx, sy, bw, bh,
                          im->w, im->h);
    dpy_image(w->id, im->id, sx, sy, bw, bh, coord(L, 3), coord(L, 4));
    return 0;
}
static void pump(lua_State *L);
static int w_flush(lua_State *L)        /* win:flush() and z.flush(): the drawing sent, the samples asked for started */
{
    if (dpy_up > 0)
        dpy_flush();
    pump(L);
    return 0;
}
static int w_size(lua_State *L)
{
    struct lwin *w = towin(L, 1);
    lua_pushinteger(L, w->w);
    lua_pushinteger(L, w->h);
    return 2;
}
static int w_title(lua_State *L)
{
    struct lwin *w = towin(L, 1);
    char t[DPY_TITLE];
    strncpy(t, luaL_checkstring(L, 2), DPY_TITLE - 1);
    t[DPY_TITLE - 1] = 0;
    dpy_flush();
    dpy_title(w->id, t);
    return 0;
}
static int w_close(lua_State *L)
{
    struct lwin *w = luaL_checkudata(L, 1, WINMT);
    if (w->id && dpy_up > 0) {
        lua_getfield(L, LUA_REGISTRYINDEX, "zenix.windows");
        lua_pushnil(L);
        lua_rawseti(L, -2, w->id);
        dpy_destroy(w->id);
    }
    w->id = 0;
    return 0;
}

/* z.image(rows): a table of strings, one a line, a character a pixel: 0-9 and a-f a pen, "." or " " see-through */
static int hexpen(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 :
           c == '.' || c == ' ' ? -1 : -2;
}
static int z_image(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Integer h = luaL_len(L, 1);
    luaL_argcheck(L, h >= 1 && h <= 256, 1, "1 .. 256 lines (the display server's limit)");
    size_t w = 0;
    if (lua_rawgeti(L, 1, 1) != LUA_TSTRING)
        return luaL_error(L, "zenix: image: line 1 is not a string");
    lua_tolstring(L, -1, &w);
    lua_pop(L, 1);
    luaL_argcheck(L, w >= 1 && w <= 640, 1, "1 .. 640 pixels a line (the display server's limit)");
    connect(L, 1);
    int words = (int)((w + 15) / 16), plane = words * 2 * (int)h;
    /* the planes in a userdata: freed by the collector if an error leaves this function (4 planes and the mask) */
    unsigned char *bits = lua_newuserdatauv(L, (size_t)plane * 5, 0);
    memset(bits, 0, (size_t)plane * 5);
    int see = 0;
    for (lua_Integer y = 0; y < h; y++) {
        size_t n = 0;
        const char *s = lua_rawgeti(L, 1, y + 1) == LUA_TSTRING ? lua_tolstring(L, -1, &n) : 0;
        if (!s || n != w)
            return luaL_error(L, "zenix: image: line %d is not a string of %d characters", (int)y + 1, (int)w);
        for (size_t x = 0; x < w; x++) {
            int p = hexpen((unsigned char)s[x]);
            if (p == -2)
                return luaL_error(L, "zenix: image: line %d: '%c' is not a pen (0-9, a-f) or see-through (.)",
                                  (int)y + 1, s[x]);
            unsigned at = (unsigned)y * (unsigned)words * 2 + (unsigned)(x >> 3);
            unsigned char bit = (unsigned char)(0x80 >> (x & 7));
            if (p < 0) { see = 1; continue; }
            for (int k = 0; k < 4; k++)
                if (p >> k & 1)
                    bits[k * plane + at] |= bit;
            bits[4 * plane + at] |= bit;
        }
        lua_pop(L, 1);
    }
    int id = dpy_image_new((int)w, (int)h, 4, see ? IMG_MASK : 0, bits);     /* the server keeps its own copy */
    lua_pop(L, 1);
    if (id < 0)
        return luaL_error(L, "zenix: image: %s", dpy_why(id));
    struct limg *im = lua_newuserdatauv(L, sizeof *im, 0);
    im->id = id; im->w = (int)w; im->h = (int)h;
    luaL_setmetatable(L, IMGMT);
    return 1;
}
static int i_free(lua_State *L)
{
    struct limg *im = luaL_checkudata(L, 1, IMGMT);
    if (im->id && dpy_up > 0)
        dpy_image_free(im->id);
    im->id = 0;
    return 0;
}
static int i_size(lua_State *L)
{
    struct limg *im = luaL_checkudata(L, 1, IMGMT);
    lua_pushinteger(L, im->w);
    lua_pushinteger(L, im->h);
    return 2;
}

/* z.palette() -> the 16 colours as 0xRGB; z.pen(rgb) -> the pen nearest that colour */
static int z_palette(lua_State *L)
{
    connect(L, 1);
    lua_createtable(L, 16, 0);
    for (int i = 0; i < 16; i++) {
        lua_pushinteger(L, pal[i]);
        lua_rawseti(L, -2, i);
    }
    return 1;
}
static int z_pen(lua_State *L)
{
    unsigned c = (unsigned)luaL_checkinteger(L, 1) & 0xFFF;
    connect(L, 1);
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < npens; i++) {
        int dr = (int)(pal[i] >> 8 & 15) - (int)(c >> 8 & 15), dg = (int)(pal[i] >> 4 & 15) - (int)(c >> 4 & 15),
            db = (int)(pal[i] & 15) - (int)(c & 15), d = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
        if (d < bd) { bd = d; best = i; }
    }
    lua_pushinteger(L, best);
    return 1;
}
static int z_screen(lua_State *L)
{
    connect(L, 1);
    lua_pushinteger(L, dpy_w);
    lua_pushinteger(L, dpy_h);
    lua_pushinteger(L, dpy_depth);
    return 3;
}

/* ---- the sound ---- */
#define MAXSMP 64
static int sh = -1;                     /* the frames stream; -errno: none (tried again RETRY_TICKS later) */
static u32 retry_at;                    /* the tick from which a failed open may be tried again */
#define RETRY_TICKS 300                 /* 3 s: sndd busy with another program, or restarting */
static unsigned eclock = 709379, pclock = 3546895;
static struct { unsigned off, len, period; } smp[MAXSMP];      /* off: from the samples' start in the bank */
static int nsmp;
static unsigned char *sbuf;             /* the samples, one after another (each even) */
static unsigned slen;
static struct mod m, mscratch;          /* the module playing; one being read (static: 1.6 KB, not on the stack) */
static int music, musicref = LUA_NOREF, mvol = 64;
static unsigned modlen;                 /* the module's part of the bank (even) */
static int bankdirty = 1;
static u32 tdue;                        /* the E-clock time the frames sent so far reach */
static int timing;
static struct { int pending, vol; unsigned s; u32 until; } fx[4];   /* a sample started on a channel */
static int nextch;
#define LEAD_MS 100                     /* the frames are made this far ahead of their time */

static unsigned modbytes(lua_State *L)  /* the module's samples (the bank's first part), 0 for none */
{
    if (musicref == LUA_NOREF)
        return 0;
    size_t n;
    lua_rawgeti(L, LUA_REGISTRYINDEX, musicref);
    lua_tolstring(L, -1, &n);
    lua_pop(L, 1);
    unsigned off = mod_bankoff(&m);
    return (unsigned)n > off ? (((unsigned)n - off) + 1) & ~1u : 0;
}
/* the stream: opened at the first sample or module played; a failure is tried again after RETRY_TICKS */
static int snd_up(void)
{
    if (sh >= 0)
        return 1;
    if (sh < -1 && (int)(sys_ticks() - retry_at) < 0)
        return 0;
    struct sndinfo si;
    int r = snd_info(&si, 0);
    if (r >= 0) {
        eclock = si.eclock;
        pclock = si.clock;
        r = snd_open(0, SNDF_FRAMES, 64, 0);
    }
    if (r < 0) {
        sh = r < -1 ? r : -EIO;
        retry_at = sys_ticks() + RETRY_TICKS;
        return 0;
    }
    sh = r;
    bankdirty = 1;
    timing = 0;
    return 1;
}
/* the stream is gone (sndd ended: ZINE, a crash): opened again at the next need, the module from its start */
static void snd_lost(void)
{
    sh = -1;
    bankdirty = 1;
    timing = 0;
    for (int c = 0; c < 4; c++) fx[c].until = fx[c].pending = 0;
    if (music) {
        mod_goto(&m, 0);
        m.ended = 0;
    }
}
/* the bank: the module's samples, then the program's -> 0, or -1 (nothing plays: the music off, the samples
 * waiting dropped; z.sample and z.music keep the total within SND_BANKMAX, so this is sndd's -ENOMEM or its end) */
static int bank(lua_State *L)
{
    if (!bankdirty)
        return 0;
    const unsigned char *mp = 0;
    unsigned ml = modbytes(L), raw = 0;          /* even; the bytes the file has */
    if (ml) {
        size_t n;
        lua_rawgeti(L, LUA_REGISTRYINDEX, musicref);
        mp = (const unsigned char *)lua_tolstring(L, -1, &n) + mod_bankoff(&m);
        raw = (unsigned)n - mod_bankoff(&m);
        lua_pop(L, 1);
    }
    modlen = ml;
    unsigned total = modlen + slen;
    unsigned char *b = malloc(total ? total : 2);
    int r = -ENOMEM;
    if (b) {
        memset(b, 0, total ? total : 2);
        if (raw)
            memcpy(b, mp, raw);
        if (slen)
            memcpy(b + modlen, sbuf, slen);
        r = snd_bank(sh, b, total ? total : 2);
        free(b);
    }
    if (r < 0) {
        if (r == -ECONNRESET)
            snd_lost();
        else {
            music = 0;
            for (int c = 0; c < 4; c++) fx[c].pending = 0;
        }
        return -1;
    }
    bankdirty = 0;
    timing = 0;
    for (int c = 0; c < 4; c++) fx[c].until = 0;
    return 0;
}
static int ahead(u32 t, u32 now) { return (int)(t - now) > 0; }
/* one frame: the module's tick (or a short silent one), the samples started since over their channels */
static void frame(struct sndframe *f, u32 now)
{
    memset(f, 0, sizeof *f);
    f->ticks = eclock / 200;            /* without a module: 5 ms, so samples asked for together start together */
    if (music) {
        f->ticks = mod_ticklen(&m, eclock);
        mod_tick(&m);
        for (int c = 0; c < 4; c++) {
            const struct modout *o = &m.out[c];
            struct sndch *x = &f->ch[c];
            x->flags = SNDC_VOL;
            x->volume = (unsigned short)(o->vol * mvol / 64);
            if (o->period >= SND_FMINPER && o->period <= SND_FMAXPER) {
                x->flags |= SNDC_PER;
                x->period = (unsigned short)o->period;
            }
            if (o->flags & (MOD_TRIG | MOD_LOOP)) {
                x->loop = o->looplen >= 2 ? o->loop : SND_SILENT;
                x->looplen = o->looplen >= 2 ? (o->looplen > 131070 ? 131070 : o->looplen) : 2;
            }
            if (o->flags & MOD_TRIG) {
                x->flags |= SNDC_TRIG;
                x->start = o->len >= 2 ? o->start : SND_SILENT;
                x->len = o->len >= 2 ? (o->len > 131070 ? 131070 : o->len) : 2;
            } else if (o->flags & MOD_LOOP)
                x->flags |= SNDC_LOOP;
        }
    }
    for (int c = 0; c < 4; c++) {
        struct sndch *x = &f->ch[c];
        if (fx[c].pending) {            /* a sample starts: the whole channel is its own */
            unsigned s = fx[c].s;
            x->flags = SNDC_TRIG | SNDC_PER | SNDC_VOL;
            x->period = (unsigned short)smp[s].period;
            x->volume = (unsigned short)fx[c].vol;
            x->start = modlen + smp[s].off;
            x->len = smp[s].len;
            x->loop = SND_SILENT;
            x->looplen = 2;
            fx[c].pending = 0;
            fx[c].until = (now + smp[s].len * smp[s].period / 5) | 1;      /* E-clock ticks it plays (never 0) */
        } else if (fx[c].until && ahead(fx[c].until, now))
            x->flags = 0;               /* still sounding: the module waits */
        else
            fx[c].until = 0;
    }
}
/* send frames until they reach LEAD_MS ahead of now; the samples asked for since the last wait go in the first one.
 * At most LEAD_MS and a batch of frames are ever queued at sndd (its queue holds SND_QFRAMES) */
static void pump(lua_State *L)
{
    int want = music;
    for (int c = 0; c < 4; c++) want |= fx[c].pending;
    if (!want || !snd_up() || bank(L) < 0)
        return;
    u32 now = sys_hrtime(), lead = now + eclock / 1000 * LEAD_MS;
    if (!timing || !ahead(tdue, now)) {         /* not started, or behind (sndd ran dry): from now */
        tdue = now;
        timing = 1;
    }
    static struct sndframe f[4];
    for (;;) {
        int any = 0;
        for (int c = 0; c < 4; c++) any |= fx[c].pending;
        if (!any && (!music || ahead(tdue, lead)))
            return;
        int n = 0;
        do {
            frame(&f[n], tdue);
            tdue += f[n].ticks;
            n++;
        } while (n < 4 && music && !ahead(tdue, lead));
        int r = snd_frames(sh, f, (unsigned)n);
        if (r < 0) {
            if (r == -ECONNRESET)
                snd_lost();
            return;
        }
    }
}

/* z.sample(bytes [, rate]) -> sample: 8-bit signed bytes at rate Hz (8000) */
static int z_sample(lua_State *L)
{
    size_t n;
    const char *d = luaL_checklstring(L, 1, &n);
    lua_Integer rate = luaL_optinteger(L, 2, 8000);
    luaL_argcheck(L, n >= 2 && n <= 131070, 1, "2 .. 131070 bytes");
    luaL_argcheck(L, rate >= 1000 && rate <= 28000, 2, "1000 .. 28000 Hz");
    if (nsmp >= MAXSMP)
        return luaL_error(L, "zenix: sample: %d samples at most", MAXSMP);
    unsigned len = ((unsigned)n + 1) & ~1u;
    if (modbytes(L) + slen + len > SND_BANKMAX)
        return luaL_error(L, "zenix: sample: the samples and the module would be over the sound server's %d KB",
                          (int)(SND_BANKMAX >> 10));
    unsigned char *nb = realloc(sbuf, slen + len);
    if (!nb)
        return luaL_error(L, "zenix: not enough memory for the sample");
    sbuf = nb;
    memcpy(sbuf + slen, d, n);
    if (len > n)
        sbuf[slen + n] = 0;
    unsigned per = (unsigned)((pclock + (unsigned)rate / 2) / (unsigned)rate);
    smp[nsmp].off = slen;
    smp[nsmp].len = len;
    smp[nsmp].period = per < SND_FMINPER ? SND_FMINPER : per > SND_FMAXPER ? SND_FMAXPER : per;
    slen += len;
    bankdirty = 1;
    int *u = lua_newuserdatauv(L, sizeof(int), 0);
    *u = nsmp++;
    luaL_setmetatable(L, SMPMT);
    return 1;
}
/* z.play(sample [, vol [, channel]]): the sample once, on that channel (0-3; by turns when not given), from the
 * next wait or flush on (so samples asked for in one go start together) */
static int z_play(lua_State *L)
{
    int s = *(int *)luaL_checkudata(L, 1, SMPMT);
    lua_Integer v = luaL_optinteger(L, 2, 64), c = luaL_optinteger(L, 3, -1);
    luaL_argcheck(L, v >= 0 && v <= 64, 2, "volume 0 .. 64");
    luaL_argcheck(L, c >= -1 && c <= 3, 3, "channel 0 .. 3");
    if (!snd_up()) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, snd_why(sh));
        return 2;
    }
    if (c < 0) {
        c = nextch;
        nextch = (nextch + 1) & 3;
    }
    fx[c].pending = 1;
    fx[c].vol = (int)v;
    fx[c].s = (unsigned)s;
    lua_pushboolean(L, 1);
    return 1;
}
/* z.music(module [, vol]): the module (a ProTracker file's bytes) from its start, looping; z.music(nil): stop */
static int z_music(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        if (music && sh >= 0) {
            struct sndframe f;          /* the module's channels quiet, after the frames already sent */
            memset(&f, 0, sizeof f);
            f.ticks = eclock / 200;
            for (int c = 0; c < 4; c++)
                if (!fx[c].until) f.ch[c].flags = SNDC_STOP;
            if (snd_frames(sh, &f, 1) == -ECONNRESET)
                snd_lost();
            else
                tdue += f.ticks;
        }
        music = 0;
        if (musicref != LUA_NOREF) {    /* its samples leave the bank (and chip RAM) at the next need */
            luaL_unref(L, LUA_REGISTRYINDEX, musicref);
            musicref = LUA_NOREF;
            bankdirty = 1;
        }
        return 0;
    }
    size_t n;
    const char *d = luaL_checklstring(L, 1, &n);
    lua_Integer v = luaL_optinteger(L, 2, 64);
    luaL_argcheck(L, v >= 0 && v <= 64, 2, "volume 0 .. 64");
    char why[64];
    if (n > (8u << 20) || mod_load(&mscratch, (const unsigned char *)d, (unsigned)n, why) < 0) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, n > (8u << 20) ? "over 8 MB" : why);
        return 2;
    }
    unsigned off = mod_bankoff(&mscratch), ml = (unsigned)n > off ? (((unsigned)n - off) + 1) & ~1u : 0;
    if (ml + slen > SND_BANKMAX) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "its samples and the program's are over the sound server's %d KB",
                        (int)(SND_BANKMAX >> 10));
        return 2;
    }
    if (!snd_up()) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, snd_why(sh));
        return 2;
    }
    if (musicref != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, musicref);
    lua_pushvalue(L, 1);
    musicref = luaL_ref(L, LUA_REGISTRYINDEX);  /* the replayer reads the patterns from it */
    m = mscratch;
    mvol = (int)v;
    music = 1;
    bankdirty = 1;
    pump(L);
    lua_pushboolean(L, 1);
    return 1;
}

/* ---- the time, events and waits ---- */
static u32 t0;
static int z_time(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)(sys_ticks() - t0) * 10);
    return 1;
}
static const char *keyname(int code)
{
    switch (code) {
    case 0x4C: return "up";
    case 0x4D: return "down";
    case 0x4E: return "right";
    case 0x4F: return "left";
    case 0x45: return "escape";
    case 0x44: case 0x43: return "return";
    case 0x40: return "space";
    case 0x41: return "backspace";
    case 0x42: return "tab";
    case 0x46: return "delete";
    case 0x5F: return "help";
    }
    if (code >= 0x50 && code <= 0x59) {
        static const char *f[] = { "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10" };
        return f[code - 0x50];
    }
    return 0;
}
static char keys[128][5];               /* each raw key's character when it went down: its name when it goes up */
static void setf(lua_State *L, const char *k, lua_Integer v) { lua_pushinteger(L, v); lua_setfield(L, -2, k); }
static int pushev(lua_State *L, const struct dpyev *e)
{
    static const char *const names[] = { 0, "key", "button", "motion", "resize", "expose", "focus", "close",
                                          "minimised", "menu", "screen" };
    lua_createtable(L, 0, 8);
    lua_pushstring(L, e->type < sizeof names / sizeof *names && names[e->type] ? names[e->type] : "other");
    lua_setfield(L, -2, "type");
    lua_getfield(L, LUA_REGISTRYINDEX, "zenix.windows");
    lua_rawgeti(L, -1, e->win);
    lua_setfield(L, -3, "window");
    lua_pop(L, 1);
    switch (e->type) {
    case EV_KEY: {
        int code = e->code & 0x7F, up = (e->code & 0x80) != 0;
        char ch[5] = { 0 };
        memcpy(ch, e->ch, 4);
        const char *nm = keyname(code);
        if (!up && !nm && ch[0])
            memcpy(keys[code], ch, 5);
        lua_pushstring(L, nm ? nm : !up && ch[0] ? ch : keys[code][0] ? keys[code] : "");
        lua_setfield(L, -2, "key");
        lua_pushstring(L, up ? "" : ch);
        lua_setfield(L, -2, "char");
        lua_pushboolean(L, up);
        lua_setfield(L, -2, "up");
        setf(L, "code", code);
        setf(L, "qual", e->qual);
        break;
    }
    case EV_BUTTON:
        setf(L, "button", e->code & 0x7F);
        lua_pushboolean(L, (e->code & 0x80) != 0);
        lua_setfield(L, -2, "up");
        /* fall through */
    case EV_MOTION:
        setf(L, "x", e->x);
        setf(L, "y", e->y);
        setf(L, "qual", e->qual);
        break;
    case EV_RESIZE:
    case EV_EXPOSE:
    case EV_SCREEN:
        setf(L, "x", e->x);
        setf(L, "y", e->y);
        setf(L, "w", e->w);
        setf(L, "h", e->h);
        if (e->type == EV_RESIZE) {     /* the window's size follows */
            lua_getfield(L, -1, "window");
            struct lwin *w = luaL_testudata(L, -1, WINMT);
            if (w) { w->w = e->w; w->h = e->h; }
            lua_pop(L, 1);
        }
        break;
    case EV_FOCUS:
    case EV_MINIMISED:
        lua_pushboolean(L, e->arg != 0);
        lua_setfield(L, -2, "on");
        break;
    case EV_MENU:
        setf(L, "menu", e->code);
        setf(L, "item", e->arg);
        break;
    }
    return 1;
}
/* the wait's ticks for ms (nil: no limit): never longer than the frames' lead while a module plays */
static int waitticks(lua_State *L, int i, int *forever)
{
    *forever = lua_isnoneornil(L, i);
    lua_Integer ms = *forever ? 0 : luaL_checkinteger(L, i);
    int t = ms <= 0 ? 0 : (int)((ms + 9) / 10);
    if (music && (*forever || t > LEAD_MS / 20)) {     /* a module plays: woken to make its next frames */
        *forever = 0;
        if (t == 0 || t > LEAD_MS / 20) t = LEAD_MS / 20;
    }
    return t;
}
/* z.event([ms]) -> the next event (a table), or nil when none came in ms (nil: wait without a limit) */
static int z_event(lua_State *L)
{
    lua_Integer ms = lua_isnoneornil(L, 1) ? -1 : luaL_checkinteger(L, 1);
    u32 end = sys_ticks() + (u32)(ms < 0 ? 0 : (ms + 9) / 10);
    for (;;) {
        if (dpy_up > 0)                 /* the drawing first: the frames' lead is not spent on it */
            dpy_flush();
        pump(L);
        int forever;
        int t = waitticks(L, 1, &forever);
        if (ms >= 0) {                  /* within what is left of the caller's time */
            int left = (int)(end - sys_ticks());
            if (left < 0) left = 0;
            if (t > left || forever) t = left;
            forever = 0;
        }
        struct dpyev e;
        int r = dpy_up > 0 ? dpy_next(&e, forever ? -1 : t) : 0;
        if (r < 0)
            return luaL_error(L, "zenix: the display: %s", strerror(-r));
        if (r > 0) {
            pushev(L, &e);
            if (dpy_lost()) {           /* the ring was full: events (a key let go, perhaps) were dropped */
                lua_pushboolean(L, 1);
                lua_setfield(L, -2, "lost");
            }
            return 1;
        }
        if (dpy_up <= 0 && t)
            sys_sleep((u32)t);
        if (ms >= 0 && (int)(end - sys_ticks()) <= 0) {
            pump(L);
            lua_pushnil(L);
            return 1;
        }
        if (dpy_up <= 0 && forever)
            return luaL_error(L, "zenix: event: no window to wait for");
    }
}
/* z.sleep(ms): wait, the sound kept fed; events meanwhile stay queued */
static int z_sleep(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    u32 end = sys_ticks() + (u32)(ms < 0 ? 0 : (ms + 9) / 10);
    if (dpy_up > 0)
        dpy_flush();
    for (;;) {
        pump(L);
        int left = (int)(end - sys_ticks());
        if (left <= 0)
            break;
        sys_sleep((u32)(music && left > LEAD_MS / 20 ? LEAD_MS / 20 : left));
    }
    return 0;
}

/* the end of the Lua state: the sound stopped, the display left */
static int z_gc(lua_State *L)
{
    (void)L;
    if (sh >= 0) {
        snd_close(sh);
        sh = -1;
    }
    if (dpy_up > 0) {
        dpy_close();
        dpy_up = 0;
    }
    free(sbuf);
    sbuf = 0;
    slen = 0;
    nsmp = 0;
    music = 0;
    musicref = LUA_NOREF;               /* (the registry goes with the state) */
    for (int c = 0; c < 4; c++) fx[c].pending = fx[c].until = 0;
    return 0;
}

static const luaL_Reg winm[] = {
    { "fill", w_fill }, { "clear", w_clear }, { "line", w_line }, { "text", w_text }, { "blit", w_blit },
    { "flush", w_flush }, { "size", w_size }, { "title", w_title }, { "close", w_close }, { NULL, NULL } };
static const luaL_Reg imgm[] = { { "free", i_free }, { "size", i_size }, { NULL, NULL } };
static const luaL_Reg funcs[] = {
    { "window", z_window }, { "image", z_image }, { "palette", z_palette }, { "pen", z_pen }, { "screen", z_screen },
    { "flush", w_flush }, { "sample", z_sample }, { "play", z_play }, { "music", z_music }, { "time", z_time },
    { "event", z_event }, { "sleep", z_sleep }, { NULL, NULL } };

int luaopen_zenix(lua_State *L)
{
    if (lua_getfield(L, LUA_REGISTRYINDEX, "zenix.end") != LUA_TNIL) {  /* loaded again (package.loaded cleared):
        lua_pop(L, 1);                                                    * the same state, the same functions */
        luaL_newlib(L, funcs);
        return 1;
    }
    lua_pop(L, 1);
    luaL_newmetatable(L, WINMT);
    luaL_newlib(L, winm);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, w_close);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_newmetatable(L, IMGMT);
    luaL_newlib(L, imgm);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, i_free);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_newmetatable(L, SMPMT);
    lua_pop(L, 1);
    lua_newtable(L);                    /* id -> window, values weak */
    lua_createtable(L, 0, 1);
    lua_pushstring(L, "v");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, "zenix.windows");
    lua_newuserdatauv(L, 1, 0);         /* the module's own end (closed with the state) */
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, z_gc);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, "zenix.end");
    t0 = sys_ticks();
    luaL_newlib(L, funcs);
    return 1;
}

/* registered for require: Lua's luaL_openlibs wrapped by the linker (--wrap=luaL_openlibs) */
void __real_luaL_openlibs(lua_State *L);
void __wrap_luaL_openlibs(lua_State *L);
void __wrap_luaL_openlibs(lua_State *L)
{
    __real_luaL_openlibs(L);
    luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_PRELOAD_TABLE);
    lua_pushcfunction(L, luaopen_zenix);
    lua_setfield(L, -2, "zenix");
    lua_pop(L, 1);
}
