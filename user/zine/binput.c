/* SPDX-License-Identifier: GPL-2.0-only */
/* ZINE's input for programs (docs/d29-zine.md 16.17): the keyboard (CIA-A's serial port, its byte taken and the
 * handshake given in blob.c's level 2 interrupt) and the mouse and joystick ports (JOY0DAT/JOY1DAT, CIA-A's fire
 * bits, POTINP's right buttons) made into InputEvents at the vertical blank, and given as AmigaOS gives them: to the
 * program's input handlers (input.device IND_ADDHANDLER, highest priority first), then as intuition does to the
 * active window (IDCMP RAWKEY, VANILLAKEY, MOUSEBUTTONS, MOUSEMOVE, MENUPICK, INTUITICKS; its MouseX/MouseY) and to
 * console.device's reads (the keys as characters, the USA keymap), and to keyboard.device and gameport.device reads.
 * Also RawKeyConvert, input.device's PeekQualifier and keymap.library's MapRawKey.
 *
 * Nothing runs while nobody listens (zi_live 0: no handler, window, console unit, keyboard or gameport user): the
 * keyboard's byte is dropped as before and the vertical blank tests one byte. A program that takes the keyboard
 * itself (its own level 2 vector, a ciaa.resource ICR handler for the serial port, the interrupt switched off) never
 * reaches this code. The interrupt only puts the key code in a ring; the events are made and given at the vertical
 * blank (as input.device's task would, at once).
 *
 * Written from the RKRM (Devices: input, console, keyboard, gameport; Libraries: intuition's IDCMP, keymap), the
 * Hardware Reference Manual (the keyboard, the controller ports) and the NDK 3.2's include files for names and
 * numbers; never from AmigaOS code. */
#include "blob.h"

/* devices/inputevent.h */
#define IECLASS_RAWKEY 1
#define IECLASS_RAWMOUSE 2
#define IECLASS_TIMER 6
#define IECODE_UP 0x80
#define IECODE_LBUTTON 0x68
#define IECODE_RBUTTON 0x69
#define IECODE_NOBUTTON 0xFF
#define IEQ_SHIFT 0x0003
#define IEQ_CAPS 0x0004
#define IEQ_CONTROL 0x0008
#define IEQ_ALT 0x0030
#define IEQ_NUMPAD 0x0100
#define IEQ_RBUTTON 0x2000
#define IEQ_LBUTTON 0x4000
#define IEQ_RELATIVE 0x8000
/* intuition/intuition.h */
#define IDCMP_MOUSEBUTTONS 0x00000008u
#define IDCMP_MOUSEMOVE 0x00000010u
#define IDCMP_MENUPICK 0x00000100u
#define IDCMP_RAWKEY 0x00000400u
#define IDCMP_DELTAMOVE 0x00100000u
#define IDCMP_VANILLAKEY 0x00200000u
#define IDCMP_INTUITICKS 0x00400000u
#define WFLG_REPORTMOUSE 0x00000200u
#define WFLG_WINDOWACTIVE 0x00002000u
#define WFLG_RMBTRAP 0x00010000u
#define MENUNULL 0xFFFF
/* Window: LeftEdge 4, TopEdge 6, MouseY 12, MouseX 14, Flags 24, WScreen 46, IDCMPFlags 82, UserPort 86, GZZMouseX
 * 108, GZZMouseY 110; Screen: Width 12, Height 14, MouseY 16, MouseX 18; IntuitionBase: amigaofs.h */
#define W16(w, o) (*(s16 *)((w) + (o)))
#define W32(w, o) (*(u32 *)((w) + (o)))
/* IOStdReq: io_Unit 24, io_Command 28, io_Flags 30, io_Error 31, io_Actual 32, io_Length 36, io_Data 40 */
#define IO_CMD(io) (*(u16 *)((io) + 28))
#define IO_ACT(io) (*(u32 *)((io) + 32))
#define IO_LEN(io) (*(u32 *)((io) + 36))
#define IO_DATA(io) (*(u8 **)((io) + 40))
#define IOERR_NOCMD (-3)
#define IOERR_ABORTED (-2)

struct ie { u32 next; u8 cls, sub; u16 code, qual; s16 x, y; u32 secs, micros; };
_Static_assert(sizeof(struct ie) == 22, "InputEvent");

u8 zi_live;                             /* someone listens (zb_irq tests it) */
static volatile u8 kring[32], kin, kout;    /* raw key codes from the keyboard interrupt */
static u16 qual;                        /* the qualifiers held (keys and buttons) */
static u8 matrix[16];                   /* the keys held (KBD_READMATRIX) */
static u8 prevk[4];                     /* the last two key-downs and their qualifiers (dead keys, IAddress) */
static struct zlist handlers;           /* IND_ADDHANDLER's Interrupts, highest priority first */
#define NWIN 8
static u8 *wins[NWIN];                  /* the open windows, the active one last */
static u8 nwin, ncon, nkbd, ngp;
static s16 px, py;                      /* the pointer on the active window's screen */
static u8 joy[2], mready;               /* port 0's last counters (the pointer) */
static u8 *pool;                        /* the IntuiMessages (IDCMP), allocated with the first window */
#define NMSG 32
#define MSGSIZE 60                      /* IntuiMessage 52, eim_TabletData 4, the dead-key bytes IAddress points to */
static u32 frames;

static void update(void)
{
    zi_live = handlers.head->succ || nwin || ncon || nkbd || ngp;
}

/* from blob.c's level 2 interrupt, the handshake given: the code into the ring (the HRM: inverted, rotated right).
 * The keyboard's own codes (0x78 reset warning, 0xF9-0xFE) are not keys. */
void zi_key(u8 sdr)
{
    if (!zi_live)
        return;
    u8 c = (u8)~sdr;
    c = (u8)(c >> 1 | c << 7);
    if (c == 0x78 || c >= 0xF9)
        return;
    u8 n = (u8)((kin + 1) & 31);
    if (n != kout) {
        kring[kin] = c;
        kin = n;
    }
}

/* ---- the USA keymap (the ROM's default): raw codes 0x00-0x3F plain and shifted, then 0x40-0x5F ---- */
static const char klo[64] __attribute__((nonstring)) = "`1234567890-=\\\0000qwertyuiop[]\000123asdfghjkl;'\000\000456\000zxcvbnm,./\000.789";
static const char khi[64] __attribute__((nonstring)) = "~!@#$%^&*()_+|\0000QWERTYUIOP{}\000123ASDFGHJKL:\"\000\000456\000ZXCVBNM<>?\000.789";
static const char k40[32] __attribute__((nonstring)) = " \b\t\r\r\033\177\000\000\000-\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000()/*+\000";

/* a key-down as the console gives it: its characters into b (at most 4), how many */
static u32 convert(u16 code, u16 q, u8 *b)
{
    if (code & IECODE_UP)
        return 0;
    if (code < 0x40) {
        char c = q & IEQ_SHIFT ? khi[code] : klo[code];
        if (!c)
            return 0;
        if ((q & IEQ_CAPS) && c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        if ((q & IEQ_CONTROL) && ((c >= '@' && c <= '_') || (c >= 'a' && c <= 'z')))
            c = (char)(c & 0x1F);
        b[0] = (u8)c;
        return 1;
    }
    if (code < 0x60 && k40[code - 0x40]) {
        b[0] = (u8)k40[code - 0x40];
        return 1;
    }
    u32 n = 0;
    b[n++] = 0x9B;                      /* CSI */
    if (code >= 0x4C && code <= 0x4F) {     /* the cursor keys: A B C D, shifted T S and " A", " @" */
        static const char pl[] = "ABCD", sh[] = "TS A @";
        if (!(q & IEQ_SHIFT))
            b[n++] = (u8)pl[code - 0x4C];
        else if (code < 0x4E)
            b[n++] = (u8)sh[code - 0x4C];
        else
            b[n++] = ' ', b[n++] = (u8)sh[2 + 2 * (code - 0x4E) + 1];
        return n;
    }
    if (code >= 0x50 && code <= 0x59) {     /* F1-F10: "0~".."9~", shifted "10~".."19~" */
        if (q & IEQ_SHIFT)
            b[n++] = '1';
        b[n++] = (u8)('0' + code - 0x50);
        b[n++] = '~';
        return n;
    }
    if (code == 0x5F) {                 /* Help */
        b[n++] = '?', b[n++] = '~';
        return n;
    }
    return 0;
}

/* RawKeyConvert (console.device -48) and keymap.library's MapRawKey (-42): one IECLASS_RAWKEY event into its
 * characters; the keymap argument is not looked at (ZINE has the USA one only). The count, -1 when it does not fit. */
s32 c_RawKeyConvert(const struct ie *e, u8 *buf, s32 len)
{
    if (!e || !buf || !c_TypeOfMem((u32)e) || e->cls != IECLASS_RAWKEY)
        return 0;
    u8 b[4];
    u32 n = convert(e->code, e->qual, b);
    if ((s32)n > len)
        return -1;
    for (u32 i = 0; i < n; i++)
        buf[i] = b[i];
    return (s32)n;
}

/* keymap.library MapANSI (-48): characters back to raw key codes (code and qualifier byte pairs) */
s32 c_MapANSI(const u8 *s, s32 n, u8 *buf, s32 len)
{
    s32 k = 0;
    for (s32 i = 0; s && buf && i < n; i++) {
        s32 code = -1, q = 0;
        for (int c = 0; c < 64 && code < 0; c++)
            if (klo[c] && (u8)klo[c] == s[i])
                code = c;
            else if (khi[c] && (u8)khi[c] == s[i])
                code = c, q = 1;
        for (int c = 0; c < 32 && code < 0; c++)
            if (k40[c] && (u8)k40[c] == s[i])
                code = 0x40 + c;
        if (code < 0)
            return 0;                   /* not on the keymap */
        if (k + 2 > len)
            return -1;
        buf[k++] = (u8)code;
        buf[k++] = (u8)q;
    }
    return k / 2;
}

u32 c_PeekQualifier(void) { return qual; }

/* ---- the windows (intuition: blibs.c's OpenWindow and CloseWindow tell us); the active one is the last opened or
 * the one ActivateWindow names ---- */
static u8 *active(void) { return nwin ? wins[nwin - 1] : 0; }

static void set_active(u8 *w, int on)
{
    if (on)
        W32(w, 24) |= WFLG_WINDOWACTIVE;
    else
        W32(w, 24) &= ~WFLG_WINDOWACTIVE;
    u8 *ib = (u8 *)Z.lib[3];
    W32(ib, ZO_IntuitionBase_ActiveWindow) = on ? (u32)w : 0;
    W32(ib, ZO_IntuitionBase_ActiveScreen) = on ? W32(w, 46) : 0;
}

void zi_activate(u8 *w)
{
    u16 s = zt_dis();
    int i = 0;
    while (i < nwin && wins[i] != w)
        i++;
    if (i < nwin) {
        if (active())
            set_active(active(), 0);
        for (; i + 1 < nwin; i++)
            wins[i] = wins[i + 1];
        wins[nwin - 1] = w;
        set_active(w, 1);
    }
    zt_en(s);
}

void zi_win_open(u8 *w)
{
    if (!pool) {
        pool = (u8 *)zx_AllocMem(NMSG * MSGSIZE, MEMF_PUBLIC | MEMF_CLEAR);
        for (int i = 0; pool && i < NMSG; i++)
            pool[i * MSGSIZE + 8] = 6;  /* NT_FREEMSG */
    }
    u16 s = zt_dis();
    if (nwin == NWIN) {                 /* the oldest one no longer gets input */
        for (int i = 0; i + 1 < NWIN; i++)
            wins[i] = wins[i + 1];
        nwin--;
    }
    if (active())
        set_active(active(), 0);
    wins[nwin++] = w;
    set_active(w, 1);
    u8 *scr = (u8 *)W32(w, 46);
    if (!mready) {                      /* the pointer starts in the middle */
        px = (s16)(scr ? W16(scr, 12) / 2 : 160);
        py = (s16)(scr ? W16(scr, 14) / 2 : 100);
    }
    update();
    zt_en(s);
}

void zi_win_close(u8 *w)
{
    u16 s = zt_dis();
    int i = 0;
    while (i < nwin && wins[i] != w)
        i++;
    if (i < nwin) {
        int was = i == nwin - 1;
        set_active(w, 0);
        for (; i + 1 < nwin; i++)
            wins[i] = wins[i + 1];
        nwin--;
        if (was && active())
            set_active(active(), 1);
    }
    for (int k = 0; pool && k < NMSG; k++)  /* its messages not replied: free again */
        if (W32(pool + k * MSGSIZE, 44) == (u32)w)
            pool[k * MSGSIZE + 8] = 6, W32(pool + k * MSGSIZE, 44) = 0;
    update();
    zt_en(s);
}

/* an IntuiMessage to the window's port when it asks for the class; once: at most one not yet replied (MOUSEMOVE,
 * INTUITICKS, as intuition does) */
static void idcmp(u8 *w, u32 cls, u16 code, u16 q, int once)
{
    u8 *port = (u8 *)W32(w, 86);
    if (!(W32(w, 82) & cls) || !port || !pool)
        return;
    u8 *m = 0;
    for (int k = 0; k < NMSG; k++) {
        u8 *p = pool + k * MSGSIZE;
        if (p[8] == 6) {
            if (!m)
                m = p;
        } else if (once && W32(p, 20) == cls && W32(p, 44) == (u32)w)
            return;
    }
    if (!m)
        return;                         /* the program replies none: the event is dropped */
    zmemset(m, 0, MSGSIZE);
    *(u16 *)(m + 18) = 52;              /* mn_Length */
    W32(m, 20) = cls;
    *(u16 *)(m + 24) = code;
    *(u16 *)(m + 26) = q;
    for (int i = 0; i < 4; i++)
        m[56 + i] = prevk[i];
    W32(m, 28) = (u32)(m + 56);         /* IAddress: the previous keys (RAWKEY), as MapRawKey's ie_EventAddress */
    if (cls == IDCMP_MOUSEMOVE && (W32(w, 82) & IDCMP_DELTAMOVE)) {
        W16(m, 32) = 0, W16(m, 34) = 0;
    } else {
        W16(m, 32) = W16(w, 14);
        W16(m, 34) = W16(w, 12);
    }
    c_GetSysTime((u32 *)(m + 36));
    W32(m, 44) = (u32)w;
    zb_port_put(port, (struct znode *)m, 5);
}

/* ---- console.device's reads (blibload.c keeps them), keyboard.device and gameport.device ---- */
void zb_con_input(const u8 *s, u32 n);

#define KQ 16
static struct ie kq[KQ];                /* keyboard.device's events not yet read */
static u8 kqin, kqout;
static u8 *kread;                       /* a KBD_READEVENT in progress */

struct gp { u8 *read; s8 ctype; u16 keys, timeout, xd, yd; s16 dx, dy; u16 waited; u8 last, lx, ly; };
static struct gp gp[2];

static void finish(u8 *io, s8 err)
{
    io[31] = (u8)err;
    zb_io_done(io);
}

static void kbd_give(void)
{
    if (!kread || kqin == kqout)
        return;
    u8 *io = kread;
    u32 n = 0, max = IO_LEN(io) / 22;
    u8 *d = IO_DATA(io);
    if (max > KQ || !c_TypeOfMem((u32)d + 22 * max - 1))
        max = 1;                        /* the first one only (its 22 bytes were checked) */
    while (n < max && kqin != kqout) {
        zmemcpy(d + 22 * n, &kq[kqout], 22);
        if (n)
            W32(d + 22 * (n - 1), 0) = (u32)(d + 22 * n);
        W32(d + 22 * n, 0) = 0;
        kqout = (u8)((kqout + 1) % KQ);
        n++;
    }
    IO_ACT(io) = 22 * n;
    kread = 0;
    finish(io, 0);
}

/* ---- the events of one vertical blank ---- */
#define NEV 24
static struct ie ev[NEV];
static u32 nev;

static struct ie *newev(u8 cls, u16 code, u16 q, s16 x, s16 y)
{
    if (nev == NEV)
        return 0;
    struct ie *e = &ev[nev++];
    e->next = 0;
    e->cls = cls, e->sub = 0, e->code = code, e->qual = q, e->x = x, e->y = y;
    c_GetSysTime(&e->secs);
    if (nev > 1)
        ev[nev - 2].next = (u32)e;
    return e;
}

/* the controller in port p: x and y counters (JOYxDAT), the left/fire button (CIA-A PRA bits 6 and 7), the right
 * one (POTINP bits 10 and 14, low when pressed) */
static u16 joydat(int p) { return CUSTOM(p ? 0x00C : 0x00A); }
static int lbutton(int p) { return !(CIAA(CIA_PRA) & (p ? 0x80 : 0x40)); }
static int rbutton(int p) { return !(CUSTOM(0x016) & (p ? 0x4000 : 0x0400)); }

static void key(u8 c)
{
    u8 k = c & 0x7F;
    if (k >= 0x60 && k <= 0x67) {       /* the qualifier keys: Shift, Caps Lock, Ctrl, Alt, Amiga */
        static const u16 qb[8] = { 1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80 };
        if (c & IECODE_UP)
            qual &= (u16)~qb[k - 0x60];
        else
            qual |= qb[k - 0x60];
    }
    if (c & IECODE_UP)
        matrix[k >> 3] &= (u8)~(1u << (k & 7));
    else
        matrix[k >> 3] |= (u8)(1u << (k & 7));
    u16 q = qual & 0xFF;
    if (k == 0x0F || k == 0x1D || k == 0x1E || k == 0x1F || (k >= 0x2D && k <= 0x2F) ||
        (k >= 0x3C && k <= 0x3F) || k == 0x43 || k == 0x4A || (k >= 0x5A && k <= 0x5E))
        q |= IEQ_NUMPAD;
    q |= qual & (IEQ_LBUTTON | IEQ_RBUTTON);
    struct ie *e = newev(IECLASS_RAWKEY, c, q, 0, 0);
    if (e) {
        e->x = (s16)(prevk[0] << 8 | prevk[1]);
        e->y = (s16)(prevk[2] << 8 | prevk[3]);
    }
    if (!(c & IECODE_UP)) {
        prevk[2] = prevk[0], prevk[3] = prevk[1];
        prevk[0] = c, prevk[1] = (u8)q;
    }
    if (nkbd && e) {                    /* keyboard.device's own queue */
        u8 n = (u8)((kqin + 1) % KQ);
        if (n != kqout) {
            kq[kqin] = *e;
            kq[kqin].next = 0;
            kqin = n;
        }
    }
}

static void mouse(void)
{
    u16 d = joydat(0);
    u8 x = (u8)d, y = (u8)(d >> 8);
    s16 dx = (s16)(s8)(u8)(x - joy[0]), dy = (s16)(s8)(u8)(y - joy[1]);
    if (!mready)
        dx = dy = 0, mready = 1;
    joy[0] = x, joy[1] = y;
    int l = lbutton(0), r = rbutton(0);
    u16 q = (qual & 0xFF) | IEQ_RELATIVE;
    if (dx || dy)
        newev(IECLASS_RAWMOUSE, IECODE_NOBUTTON, q | (qual & (IEQ_LBUTTON | IEQ_RBUTTON)), dx, dy);
    if (l != !!(qual & IEQ_LBUTTON)) {
        qual ^= IEQ_LBUTTON;
        newev(IECLASS_RAWMOUSE, (u16)(IECODE_LBUTTON | (l ? 0 : IECODE_UP)), q | (qual & 0x6000), 0, 0);
    }
    if (r != !!(qual & IEQ_RBUTTON)) {
        qual ^= IEQ_RBUTTON;
        newev(IECLASS_RAWMOUSE, (u16)(IECODE_RBUTTON | (r ? 0 : IECODE_UP)), q | (qual & 0x6000), 0, 0);
    }
}

/* intuition's part: the events to the active window and the console */
static void intuition(u32 chain)
{
    u8 *w = active();
    u8 *scr = w ? (u8 *)W32(w, 46) : 0;
    s16 sw = scr ? W16(scr, 12) : 320, sh = scr ? W16(scr, 14) : 200;
    int n = 0;
    for (u32 p = chain; p && n < 64; p = ((struct ie *)p)->next, n++) {
        if ((p & 1) || !c_TypeOfMem(p))
            break;                      /* a handler's chain that is not memory: the rest dropped */
        const struct ie *e = (const struct ie *)p;
        if (e->cls == IECLASS_RAWKEY) {
            u8 b[4];
            u32 k = convert(e->code, e->qual, b);
            if (ncon && k)
                zb_con_input(b, k);
            if (w) {
                if (k == 1 && (W32(w, 82) & IDCMP_VANILLAKEY))
                    idcmp(w, IDCMP_VANILLAKEY, b[0], e->qual, 0);
                else
                    idcmp(w, IDCMP_RAWKEY, e->code, e->qual, 0);
            }
        } else if (e->cls == IECLASS_RAWMOUSE) {
            if (e->x || e->y) {
                px = (s16)(px + e->x), py = (s16)(py + e->y);
                px = px < 0 ? 0 : px >= sw ? (s16)(sw - 1) : px;
                py = py < 0 ? 0 : py >= sh ? (s16)(sh - 1) : py;
                if (w) {
                    W16(w, 14) = (s16)(px - W16(w, 4)), W16(w, 12) = (s16)(py - W16(w, 6));
                    if (W32(w, 24) & WFLG_REPORTMOUSE)
                        idcmp(w, IDCMP_MOUSEMOVE, 0, e->qual, 1);
                }
            }
            u16 c = e->code & 0x7F;
            if (w && c == IECODE_LBUTTON)
                idcmp(w, IDCMP_MOUSEBUTTONS, e->code, e->qual, 0);
            else if (w && c == IECODE_RBUTTON) {
                if (W32(w, 24) & WFLG_RMBTRAP)
                    idcmp(w, IDCMP_MOUSEBUTTONS, e->code, e->qual, 0);
                else if (e->code & IECODE_UP)
                    idcmp(w, IDCMP_MENUPICK, MENUNULL, e->qual, 0);     /* no menus drawn: nothing chosen */
            }
        }
    }
    if (w) {
        W16(w, 14) = (s16)(px - W16(w, 4)), W16(w, 12) = (s16)(py - W16(w, 6));
        W16(w, 108) = W16(w, 14), W16(w, 110) = W16(w, 12);
        if (scr)
            W16(scr, 18) = px, W16(scr, 16) = py;
        u8 *ib = (u8 *)Z.lib[3];
        W16(ib, ZO_IntuitionBase_MouseX) = px, W16(ib, ZO_IntuitionBase_MouseY) = py;
        if (frames % 5 == 0)            /* about ten a second */
            idcmp(w, IDCMP_INTUITICKS, 0, qual, 1);
    }
}

/* the chain through the program's handlers of one side of intuition's priority (50) */
static u32 handle(u32 chain, int high)
{
    for (struct znode *n = handlers.head; n->succ && chain; n = n->succ)
        if ((n->pri > 50) == high)
            chain = zb_libcall(((struct zint *)n)->code, zb_inp_base, 0, chain, 0, ((struct zint *)n)->data);
    return chain;
}

static void dispatch(u32 chain)
{
    chain = handle(chain, 1);
    intuition(chain);
    handle(chain, 0);
}

/* gameport.device: a unit's controller read, an event when its trigger is met */
static void gameport(int u)
{
    struct gp *g = &gp[u];
    if (!g->ctype)
        return;
    u16 d = joydat(u);
    u8 b = (u8)lbutton(u);
    s16 dx, dy;
    if (g->ctype == 3) {                /* GPCT_ABSJOYSTICK: the directions as -1, 0, 1 */
        dx = (s16)((d & 2) ? 1 : (d & 0x200) ? -1 : 0);
        dy = (s16)(((d >> 1) ^ d) & 1 ? 1 : ((d >> 9) ^ (d >> 8)) & 1 ? -1 : 0);
    } else {                            /* the mouse (and the relative joystick, counted the same) */
        u8 x = (u8)d, y = (u8)(d >> 8);
        dx = (s16)(s8)(u8)(x - g->lx), dy = (s16)(s8)(u8)(y - g->ly);
        g->lx = x, g->ly = y;
    }
    u8 *io = g->read;
    if (g->ctype == 3) {
        int moved = dx != g->dx || dy != g->dy;
        g->dx = dx, g->dy = dy;
        if (!moved && b == g->last && !(g->timeout && ++g->waited >= g->timeout))
            return;
    } else {
        g->dx = (s16)(g->dx + dx), g->dy = (s16)(g->dy + dy);
        u16 ax = (u16)(g->dx < 0 ? -g->dx : g->dx), ay = (u16)(g->dy < 0 ? -g->dy : g->dy);
        int moved = (ax && ax >= (g->xd ? g->xd : 1)) || (ay && ay >= (g->yd ? g->yd : 1));
        int keyt = b != g->last && (g->keys & (b ? 1 : 2));
        if (!moved && !keyt && !(g->timeout && ++g->waited >= g->timeout)) {
            g->last = b;
            return;
        }
    }
    if (!io)
        return;                         /* nobody reads now: the movement is kept (mouse) */
    u16 code = b == g->last ? IECODE_NOBUTTON : (u16)(IECODE_LBUTTON | (b ? 0 : IECODE_UP));
    g->last = b;
    g->waited = 0;
    struct ie *e = (struct ie *)IO_DATA(io);
    e->next = 0, e->cls = IECLASS_RAWMOUSE, e->sub = 0, e->code = code;
    e->qual = (u16)((qual & 0xFF) | (b ? IEQ_LBUTTON : 0) | (g->ctype == 3 ? 0 : IEQ_RELATIVE));
    e->x = g->dx, e->y = g->dy;
    c_GetSysTime(&e->secs);
    if (g->ctype != 3)
        g->dx = g->dy = 0;
    IO_ACT(io) = 22;
    g->read = 0;
    finish(io, 0);
}

/* from zb_irq at the vertical blank, while someone listens */
void zi_frame(void)
{
    frames++;
    nev = 0;
    while (kout != kin) {
        key(kring[kout]);
        kout = (u8)((kout + 1) & 31);
    }
    mouse();
    if (handlers.head->succ && frames % 5 == 0)     /* IECLASS_TIMER, about ten a second, for the handlers */
        newev(IECLASS_TIMER, 0, qual, 0, 0);
    if (nev)
        dispatch((u32)ev);
    else if (active())
        intuition(0);
    kbd_give();
    for (int u = 0; u < 2; u++)
        gameport(u);
}

/* ---- input.device (IND_ADDHANDLER 9, IND_REMHANDLER 10, IND_WRITEEVENT 11; the thresholds and the mouse port's
 * commands are taken and change nothing) ---- */
void zb_inp_beginio(u8 *io)
{
    io[8] = 5;
    io[31] = 0;
    IO_ACT(io) = 0;
    struct znode *n = (struct znode *)IO_DATA(io);
    u16 s = zt_dis();
    switch (IO_CMD(io)) {
    case 9:                             /* an Interrupt whose is_Code is code in the program's memory */
        if (n && !((u32)n & 1) && c_TypeOfMem((u32)n) && c_TypeOfMem((u32)n + 21) &&
            !(((struct zint *)n)->code & 1) && c_TypeOfMem(((struct zint *)n)->code))
            c_Enqueue(&handlers, n);
        else
            io[31] = (u8)-4;            /* IOERR_BADLENGTH: nothing taken */
        break;
    case 10:
        for (struct znode *h = handlers.head; h->succ; h = h->succ)
            if (h == n) {
                c_Remove(n);
                break;
            }
        break;
    case 11:                            /* an event of the program's own, given as if it came from the hardware */
        if (n && c_TypeOfMem((u32)n) && IO_LEN(io) >= 22) {
            nev = 0;
            struct ie *e = newev(0, 0, 0, 0, 0);
            zmemcpy(e, n, 22);
            e->next = 0;
            dispatch((u32)e);
        }
        break;
    }
    update();
    zt_en(s);
    zb_io_done(io);
}

/* ---- keyboard.device (KBD_READEVENT 9, KBD_READMATRIX 10, the reset handler commands 11-13 taken) and
 * gameport.device (GPD_READEVENT 9, GPD_ASKCTYPE 10, GPD_SETCTYPE 11, GPD_ASKTRIGGER 12, GPD_SETTRIGGER 13); io_Unit
 * is 0 (keyboard) and the port (gameport) ---- */
u32 zb_kbd_base, zb_gp_base;

u32 zi_open(u8 *io, u32 base, u32 unit)
{
    if (!io)
        return (u32)-1;
    if (base == zb_gp_base && unit > 1) {
        io[31] = 0xFF;                  /* IOERR_OPENFAIL: there are two ports */
        return (u32)-1;
    }
    *(u32 *)(io + ZO_IORequest_io_Device) = base;
    *(u32 *)(io + 24) = base == zb_gp_base ? unit : 0;
    io[31] = 0;
    u16 s = zt_dis();
    if (base == zb_kbd_base)
        nkbd++;
    else
        ngp++;
    update();
    zt_en(s);
    return 0;
}

int zi_pending(u8 *io)
{
    return io && (io == kread || io == gp[0].read || io == gp[1].read);
}

void zi_abort(u8 *io)
{
    u16 s = zt_dis();
    int was = zi_pending(io);
    if (io == kread)
        kread = 0;
    for (int u = 0; u < 2; u++)
        if (io == gp[u].read)
            gp[u].read = 0;
    zt_en(s);
    if (was) {
        IO_ACT(io) = 0;
        finish(io, IOERR_ABORTED);
    }
}

void zi_close(u8 *io)
{
    zi_abort(io);
    u16 s = zt_dis();
    if (*(u32 *)(io + ZO_IORequest_io_Device) == zb_kbd_base) {
        if (nkbd)
            nkbd--;
    } else {
        if (ngp)
            ngp--;
        u32 u = *(u32 *)(io + 24);
        if (u < 2 && !ngp)
            gp[u].ctype = 0;
    }
    *(u32 *)(io + ZO_IORequest_io_Device) = 0;
    update();
    zt_en(s);
}

void zi_beginio(u8 *io)
{
    io[8] = 5;
    io[31] = 0;
    IO_ACT(io) = 0;
    u16 cmd = IO_CMD(io);
    u8 *d = IO_DATA(io);
    int kb = *(u32 *)(io + ZO_IORequest_io_Device) == zb_kbd_base;
    if ((kb ? cmd >= 9 && cmd <= 10 : cmd >= 9 && cmd <= 13) &&
        (!d || ((u32)d & 1) || !c_TypeOfMem((u32)d) || !c_TypeOfMem((u32)d + (kb && cmd == 10 ? 15 : 21)))) {
        /* the reads and every gameport command have their data in the program's memory */
        finish(io, (s8)-4);             /* IOERR_BADLENGTH: no data where the command needs it */
        return;
    }
    u16 s = zt_dis();
    if (kb) {
        if (cmd == 9 && IO_LEN(io) >= 22 && !kread) {
            io[30] &= (u8)~1;           /* not quick: done when a key comes */
            kread = io;
            kbd_give();
            zt_en(s);
            return;
        }
        if (cmd == 10 && d) {
            u32 n = IO_LEN(io) < 16 ? IO_LEN(io) : 16;
            zmemcpy(d, matrix, n);
            IO_ACT(io) = n;
        } else if (cmd < 11 || cmd > 13)
            io[31] = (u8)IOERR_NOCMD;
        zt_en(s);
        zb_io_done(io);
        return;
    }
    struct gp *g = &gp[*(u32 *)(io + 24) & 1];
    switch (cmd) {
    case 9:
        if (IO_LEN(io) >= 22 && !g->read) {
            io[30] &= (u8)~1;
            g->read = io;
            zt_en(s);
            return;
        }
        break;
    case 10:
        *d = (u8)g->ctype;
        IO_ACT(io) = 1;
        break;
    case 11:
        g->ctype = (s8)*d;
        g->dx = g->dy = 0, g->waited = 0, g->last = 0;
        g->lx = (u8)joydat(*(u32 *)(io + 24) & 1);
        g->ly = (u8)(joydat(*(u32 *)(io + 24) & 1) >> 8);
        IO_ACT(io) = 1;
        break;
    case 12:
        ((u16 *)d)[0] = g->keys, ((u16 *)d)[1] = g->timeout, ((u16 *)d)[2] = g->xd, ((u16 *)d)[3] = g->yd;
        IO_ACT(io) = 8;
        break;
    case 13:
        g->keys = ((u16 *)d)[0], g->timeout = ((u16 *)d)[1], g->xd = ((u16 *)d)[2], g->yd = ((u16 *)d)[3];
        IO_ACT(io) = 8;
        break;
    default:
        if (cmd > 8)
            io[31] = (u8)IOERR_NOCMD;
    }
    zt_en(s);
    zb_io_done(io);
}

/* console.device's units open (blibload.c) */
void zi_con_units(int d)
{
    u16 s = zt_dis();
    ncon = (u8)(ncon + d);
    update();
    zt_en(s);
}

void zi_init(void)
{
    handlers.head = (struct znode *)&handlers.tail;
    handlers.tail = 0;
    handlers.tailpred = (struct znode *)&handlers.head;
    handlers.type = NT_INTERRUPT;
    update();
}
