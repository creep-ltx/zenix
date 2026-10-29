/* SPDX-License-Identifier: GPL-2.0-only */
/* The text-UI library on Zenix (roadmap block 3; DESIGN D27 sections 8 and 9): the owner's tui.library, linked into
 * each program (libtui.a; D27 8: static, no shared library). Its upstream file, ports/tui/upstream/src/core.c (MIT),
 * is compiled here unchanged: the grid, the colours, the characters, the drawing and the key parser are all its own.
 * Its terminal half is AmigaOS's (a wait packet to the console beside Ctrl-C..F signals in one Wait(), SetMode, the
 * size by CSI 0 q, CTerm found by ACTION_DISK_INFO); on Zenix that half is these functions instead, over the C
 * library (D27 5.4 and 9's table):
 *  - the keys: raw mode by termios (cfmakeraw: no echo, no signals, so ^C and ^Z come as keys, as Ctrl-C..F do from
 *    upstream's signals), and poll on the input descriptor for every wait, with the timeouts upstream has (40 ms for
 *    the rest of an escape sequence);
 *  - the size: TIOCGWINSZ (the console's 80 x 32 PAL, 80 x 25 NTSC), else 80 x 24 as upstream; the console never
 *    changes size, so there are no resize events (no SIGWINCH);
 *  - a BPTR is a file descriptor; 0 for the input is descriptor 0 and 0 for the output descriptor 1 (upstream: Input()
 *    and Output());
 *  - CTerm is 1: the Zenix console takes the ANSI colours as they are (256 and 24-bit ones as the nearest of its 16)
 *    and ESC [ ? 25 h / l for the cursor: upstream's CTerm path. UTF8 is 0: the console is Latin-1 (one byte one
 *    cell) until its UTF-8 (DESIGN D28) lands; ZENIX_TUI_UTF8 (ports/tui/README) switches the output to UTF-8 and the
 *    widths to the C library's wcwidth then. CellW and CellH are 0; no mouse, no pastes;
 *  - the console has no second screen: closing (and suspending) clears it, as upstream does in a plain console;
 *  - TuiSignal answers 0 (no AmigaOS signals); a program with a poll loop of its own polls descriptor 0 and calls
 *    TuiPoll. TuiWait's extra signals are none: a signal that interrupts the wait (its handler ran) ends it with
 *    TE_NONE, as upstream's extra signals do.
 * The upstream terminal functions are renamed by the defines below and declared static first, so they stay in the
 * file unused and are never emitted (the C rule: a later declaration without static keeps the internal linkage). */
#ifdef ZENIX_TUI_UTF8
#define TUI_LIB                 /* upstream's way to utf8.library's widths: wcwidth here (include/proto/utf8.h) */
#endif
#include <errno.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <amiga.h>
#include <libraries/tui.h>

#define TuiPossible   up_TuiPossible
#define TuiOpen       up_TuiOpen
#define TuiOpenIO     up_TuiOpenIO
#define TuiClose      up_TuiClose
#define TuiSuspend    up_TuiSuspend
#define TuiResume     up_TuiResume
#define TuiKeyAfter   up_TuiKeyAfter
#define TuiWait       up_TuiWait
#define TuiSignal     up_TuiSignal
#define TuiPoll       up_TuiPoll
#define TuiKeyWaiting up_TuiKeyWaiting
static LONG TuiPossible(BPTR in);
static struct Tui *TuiOpen(BPTR in);
static struct Tui *TuiOpenIO(BPTR in, BPTR out);
static void TuiClose(struct Tui *t);
static void TuiSuspend(struct Tui *t);
static void TuiResume(struct Tui *t);
static void TuiKeyAfter(struct Tui *t, CONST_STRPTR msg);
static LONG TuiWait(struct Tui *t, TuiEv *e, LONG us, ULONG extra, ULONG *got);
static ULONG TuiSignal(struct Tui *t);
static LONG TuiPoll(struct Tui *t, TuiEv *e);
static LONG TuiKeyWaiting(struct Tui *t);
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../upstream/src/core.c"
#undef TuiPossible
#undef TuiOpen
#undef TuiOpenIO
#undef TuiClose
#undef TuiSuspend
#undef TuiResume
#undef TuiKeyAfter
#undef TuiWait
#undef TuiSignal
#undef TuiPoll
#undef TuiKeyWaiting

#ifdef ZENIX_TUI_UTF8
#define ZUTF8 1
#else
#define ZUTF8 0
#endif

/* upstream's Priv, and the terminal's settings from before (put back on close and suspend) */
typedef struct {
    Priv p;
    struct termios saved;
} ZPriv;

static int zraw(Priv *p)
{
    struct termios t = ((ZPriv *)p)->saved;
    cfmakeraw(&t);
    return tcsetattr((int)p->tin, TCSANOW, &t);
}

static void zcooked(Priv *p)
{
    tcsetattr((int)p->tin, TCSANOW, &((ZPriv *)p)->saved);
}

static void zmodes_on(Priv *p)
{
    outs(p, "\x1b[?1049h\x1b[?7l");     /* a second screen and no wrap where the terminal has them (the console not) */
    p->pub.CTerm = 1;
    p->pub.UTF8 = ZUTF8;
    forget(p);
}

static void zmodes_off(Priv *p)
{
    outs(p, "\x1b[0m\x1b[?7h\x1b[?25h\x1b[H\x1b[J\x1b[?1049l");   /* no second screen: cleared */
    oflush(p);
}

static void zasksize(Priv *p)
{
    struct winsize ws;
    int rows = 24, cols = 80;
    if ((ioctl((int)p->tout, TIOCGWINSZ, &ws) == 0 || ioctl((int)p->tin, TIOCGWINSZ, &ws) == 0) &&
        ws.ws_row >= 3 && ws.ws_col >= 10) {
        rows = ws.ws_row > MAXH ? MAXH : ws.ws_row;
        cols = ws.ws_col > MAXW ? MAXW : ws.ws_col;
    }
    p->pub.Rows = rows;
    p->pub.Cols = cols;
}

/* input on the console within ms (-1: no limit): 1 yes, 0 no, -1 a signal came (its handler ran) */
static int zwaitin(Priv *p, int ms)
{
    struct pollfd f;
    int r;
    f.fd = (int)p->tin;
    f.events = POLLIN;
    f.revents = 0;
    r = poll(&f, 1, ms);
    if (r < 0) {
        if (errno == EINTR) return -1;
        p->ended = 1;                   /* the descriptor is gone: the end, as upstream's 0-byte Read */
        return 0;
    }
    return r > 0;
}

/* what the console has into ibuf; 0 bytes after "input is there": the input ended */
static int zreadsome(Priv *p)
{
    ssize_t n = read((int)p->tin, p->ibuf + p->ilen, sizeof(p->ibuf) - (size_t)p->ilen);
    if (n <= 0) {
        if (n < 0 && errno == EINTR) return 0;
        p->ended = 1;
        return 0;
    }
    p->ilen += (int)n;
    return 1;
}

/* upstream's readnow over poll: what is there now, and a moment for the rest of a sequence cut between reads */
static void zreadnow(Priv *p)
{
    int guard = 16;
    while (guard-- && zwaitin(p, 0) > 0) {
        if (!zreadsome(p)) break;
        parse(p, 0);
        if (p->ilen >= (int)sizeof(p->ibuf) - 8) {
            parse(p, 1);
            if (p->ilen >= (int)sizeof(p->ibuf) - 8) p->ilen = 0;
        }
    }
    if (p->ilen) {
        if (zwaitin(p, 40) > 0 && zreadsome(p)) parse(p, 0);
        if (p->ilen && !p->inpaste) parse(p, 1);
    }
}

static int zgive(Priv *p, TuiEv *e)
{
    if (p->evh != p->evt) {
        *e = p->evq[p->evh];
        p->evh = (p->evh + 1) % EVQ;
        return 1;
    }
    if (p->ended) {
        memset(e, 0, sizeof(*e));
        e->type = TE_END;
        return 1;
    }
    return 0;
}

LONG TuiPossible(BPTR in)
{
    return isatty((int)in) && isatty(1);
}

struct Tui *TuiOpenIO(BPTR in, BPTR out)
{
    ZPriv *z;
    Priv *p;
    if (!out) out = 1;
    if (!isatty((int)in) || !isatty((int)out)) return NULL;
    if (!(z = calloc(1, sizeof(*z)))) return NULL;
    p = &z->p;
    p->tin = in;
    p->tout = out;
    p->pub.Cols = 80;
    p->pub.Rows = 24;
    forget(p);
    if (tcgetattr((int)in, &z->saved) < 0 || zraw(p) < 0) {
        free(z);
        return NULL;
    }
    zmodes_on(p);
    zasksize(p);
    if (!gridalloc(p)) {
        zmodes_off(p);
        zcooked(p);
        free(z);
        return NULL;
    }
    TuiRedraw(&p->pub);
    return &p->pub;
}

struct Tui *TuiOpen(BPTR in)
{
    return TuiOpenIO(in, 0);
}

void TuiClose(struct Tui *t)
{
    Priv *p = P(t);
    if (!p) return;
    if (!p->tsusp) {
        zmodes_off(p);
        zcooked(p);
    }
    gridfree(p);
    free(p->pastebuf);
    free(p->pastegive);
    free(p);
}

void TuiSuspend(struct Tui *t)
{
    Priv *p = P(t);
    if (!p || p->tsusp) return;
    p->tsusp = 1;
    zmodes_off(p);
    zcooked(p);
}

void TuiKeyAfter(struct Tui *t, CONST_STRPTR msg)
{
    Priv *p = P(t);
    char c;
    if (!p) return;
    if (!msg) msg = (CONST_STRPTR)" a key goes back ";
    Write(p->tout, "\n\x1b[7m", 5);
    Write(p->tout, msg, (LONG)strlen((const char *)msg));
    Write(p->tout, "\x1b[0m", 4);
    zraw(p);
    if (read((int)p->tin, &c, 1) <= 0) p->ended = 1;
    while (!p->ended && zwaitin(p, 20) > 0)     /* the rest of a key's sequence */
        if (read((int)p->tin, &c, 1) <= 0) p->ended = 1;
    Write(p->tout, "\r\x1b[K", 4);
    zcooked(p);
}

void TuiResume(struct Tui *t)
{
    Priv *p = P(t);
    if (!p || !p->tsusp) return;
    zraw(p);
    p->tsusp = 0;
    zmodes_on(p);
    zasksize(p);
    gridalloc(p);
    TuiRedraw(t);
}

LONG TuiWait(struct Tui *t, TuiEv *e, LONG us, ULONG extra, ULONG *got)
{
    Priv *p = P(t);
    (void)extra;
    if (got) *got = 0;
    if (!p) {
        memset(e, 0, sizeof(*e));
        e->type = TE_END;
        return 1;
    }
    for (;;) {
        int r;
        if (zgive(p, e)) return 1;
        zreadnow(p);
        if (p->evh != p->evt || p->ended) continue;
        if (us == 0) break;
        TuiFlush(t);
        r = zwaitin(p, us < 0 ? -1 : (int)((us + 999) / 1000));
        if (r < 0 || (r == 0 && !p->ended && us > 0)) break;   /* a signal, or the time is up */
    }
    e->type = TE_NONE;
    return 0;
}

ULONG TuiSignal(struct Tui *t)
{
    (void)t;
    return 0;
}

LONG TuiPoll(struct Tui *t, TuiEv *e)
{
    Priv *p = P(t);
    if (!p) {
        memset(e, 0, sizeof(*e));
        e->type = TE_END;
        return 1;
    }
    if (p->evh == p->evt && !p->ended) zreadnow(p);
    if (zgive(p, e)) return 1;
    e->type = TE_NONE;
    return 0;
}

LONG TuiKeyWaiting(struct Tui *t)
{
    Priv *p = P(t);
    if (!p || p->ended) return 1;
    zreadnow(p);
    return p->ended || p->evh != p->evt;
}
