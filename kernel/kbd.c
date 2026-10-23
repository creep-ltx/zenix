#include "kernel.h"
#include "hw.h"

/* Amiga keyboard: bytes arrive in CIA-A's serial shift register (SP/CNT
 * pins), raising ICR bit 3. Data is rotated and inverted on the wire;
 * after each byte the CPU must handshake by driving SP as output low
 * for at least 85us (HRM appendix G). Key release = keycode | 0x80.
 *
 * Keycodes 00-3F are positional; their meaning depends on the national
 * layout, so those live in selectable keymaps (Latin-1 output - the
 * console renders it, Topaz has the glyphs). 40-67 are layout-neutral.
 *
 * Each keymap has four layers, as AmigaOS keymaps do: plain, Shift, Alt,
 * Shift+Alt. The Alt layer is AmigaOS's own and the same on every national
 * keyboard: Latin-1 symbols on the letters and digits (Alt+A ae, Alt+O
 * o-slash, Alt+S sharp s, Alt+1 superscript 1 ...), the US character on
 * the other keys - which is where [ ] { } \ | @ live on a Swedish Amiga.
 * Caps Lock acts as Shift on the keys marked in `caps` (the letters,
 * including aring/adiaeresis/odiaeresis). Accents that AmigaOS treats as
 * dead keys (acute, grave, circumflex, tilde, diaeresis) are typed as the
 * spacing character here: there is no composition.
 *
 * The Swedish table is the AmigaOS keymap "s", generated from
 * xkeyboard-config's se layout by the Fuzix Amiga port's mkxkbkeymap.py
 * (xkeyboard-config: MIT/X11 licence, its notice is above the tables) with
 * the Amiga conventions applied, then checked key by key against
 * DEVS:Keymaps/s of Workbench 3.1 (two Alt keys corrected: Alt+' is the
 * acute accent, Alt+the key left of Return is u-diaeresis). The US Alt layer
 * comes from the same generator. */

static u8 shift_down, ctrl_down, amiga_down, alt_down, caps_lock;

struct keymap {
    char name[4];
    u8 normal[0x40];
    u8 shifted[0x40];
    u8 alt[0x40];
    u8 shift_alt[0x40];
    u8 caps[8];                 /* bit (code & 7) of byte code >> 3: Caps Lock shifts this key */
};

/* xkeyboard-config notice (the Swedish layout and the US Alt layer were generated from it):
 * Copyright 1996 by Joseph Moss; Copyright (C) 2002-2007 Free Software Foundation, Inc.; and others.
 * Permission to use, copy, modify, distribute, and sell this software and its documentation for any purpose is
 * hereby granted without fee, provided that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting documentation, and that the name of the
 * copyright holder(s) not be used in advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission. The copyright holder(s) makes no representations about the suitability of
 * this software for any purpose. It is provided "as is" without express or implied warranty. */
static const struct keymap keymaps[] = {
    { "us",
      { /* plain */
         '`',  '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',  '-',  '=', '\\',    0,  '0',
         'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  'o',  'p',  '[',  ']',    0,  '1',  '2',  '3',
         'a',  's',  'd',  'f',  'g',  'h',  'j',  'k',  'l',  ';', '\'',    0,    0,  '4',  '5',  '6',
           0,  'z',  'x',  'c',  'v',  'b',  'n',  'm',  ',',  '.',  '/',    0,  '.',  '7',  '8',  '9', },
      { /* Shift */
         '~',  '!',  '@',  '#',  '$',  '%',  '^',  '&',  '*',  '(',  ')',  '_',  '+',  '|',    0,  '0',
         'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',  'O',  'P',  '{',  '}',    0,  '1',  '2',  '3',
         'A',  'S',  'D',  'F',  'G',  'H',  'J',  'K',  'L',  ':',  '"',    0,    0,  '4',  '5',  '6',
           0,  'Z',  'X',  'C',  'V',  'B',  'N',  'M',  '<',  '>',  '?',    0,  '.',  '7',  '8',  '9', },
      { /* Alt */
         '`', 0xB9, 0xB2, 0xB3, 0xA2, 0xBC, 0xBD, 0xBE, 0xB7, 0xAB, 0xBB,  '-',  '=', '\\',    0,  '0',
        0xE5, 0xB0, 0xA9, 0xAE, 0xFE, 0xA4, 0xB5, 0xA1, 0xF8, 0xB6,  '[',  ']',    0,  '1',  '2',  '3',
        0xE6, 0xDF, 0xF0, 0xB4,  '`',  '^',  '~', 0xA8, 0xA3,  ';', '\'',    0,    0,  '4',  '5',  '6',
        '\\', 0xB1, 0xD7, 0xE7, 0xAA, 0xBA, 0xAD, 0xB8,  ',',  '.',  '/',    0,  '.',  '7',  '8',  '9', },
      { /* Shift+Alt */
         '~',  '!',  '@',  '#',  '$',  '%',  '^',  '&',  '*',  '(',  ')',  '_',  '+',  '|',    0,  '0',
        0xC5, 0xB0, 0xA9, 0xAE, 0xDE, 0xA5, 0xB5, 0xA6, 0xD8, 0xB6,  '{',  '}',    0,  '1',  '2',  '3',
        0xC6, 0xA7, 0xD0, 0xB4,  '`',  '^',  '~', 0xA8, 0xA3,  ':',  '"',    0,    0,  '4',  '5',  '6',
         '|', 0xAC, 0xF7, 0xC7, 0xAA, 0xBA, 0xAF, 0xBF,  '<',  '>',  '?',    0,  '.',  '7',  '8',  '9', },
      { 0x00, 0x00, 0xFF, 0x03, 0xFF, 0x01, 0xFE, 0x00 } },
    { "sv",       /* AmigaOS keymap "s" (Swedish and Finnish) */
      { /* plain */
         '`',  '1',  '2',  '3',  '4',  '5',  '6',  '7',  '8',  '9',  '0',  '+', 0xB4, '\\',    0,  '0',
         'q',  'w',  'e',  'r',  't',  'y',  'u',  'i',  'o',  'p', 0xE5, 0xA8,    0,  '1',  '2',  '3',
         'a',  's',  'd',  'f',  'g',  'h',  'j',  'k',  'l', 0xF6, 0xE4, '\'',    0,  '4',  '5',  '6',
         '<',  'z',  'x',  'c',  'v',  'b',  'n',  'm',  ',',  '.',  '-',    0,  '.',  '7',  '8',  '9', },
      { /* Shift */
         '~',  '!',  '"', 0xA3,  '$',  '%',  '&',  '/',  '(',  ')',  '=',  '?',  '`',  '|',    0,  '0',
         'Q',  'W',  'E',  'R',  'T',  'Y',  'U',  'I',  'O',  'P', 0xC5,  '^',    0,  '1',  '2',  '3',
         'A',  'S',  'D',  'F',  'G',  'H',  'J',  'K',  'L', 0xD6, 0xC4,  '*',    0,  '4',  '5',  '6',
         '>',  'Z',  'X',  'C',  'V',  'B',  'N',  'M',  ';',  ':',  '_',    0,  '.',  '7',  '8',  '9', },
      { /* Alt */
         '`', 0xB9, 0xB2, 0xB3, 0xA2, 0xBC, 0xBD, 0xBE, 0xB7, 0xAB, 0xBB,  '-',  '=', '\\',    0,  '0',
        0xE5, 0xB0, 0xA9, 0xAE, 0xFE, 0xA4, 0xB5, 0xA1, 0xF8, 0xB6,  '[',  ']',    0,  '1',  '2',  '3',
        0xE6, 0xDF, 0xF0, 0xB4,  '`',  '^',  '~', 0xA8, 0xA3,  ';', 0xB4, 0xFC,    0,  '4',  '5',  '6',
        '\\', 0xB1, 0xD7, 0xE7, 0xAA, 0xBA, 0xAD, 0xB8,  ',',  '.',  '/',    0,  '.',  '7',  '8',  '9', },
      { /* Shift+Alt */
         '~',  '!',  '@',  '#',  '$',  '%',  '^',  '&',  '*',  '(',  ')',  '_',  '+',  '|',    0,  '0',
        0xC5, 0xB0, 0xA9, 0xAE, 0xDE, 0xA5, 0xB5, 0xA6, 0xD8, 0xB6,  '{',  '}',    0,  '1',  '2',  '3',
        0xC6, 0xA7, 0xD0, 0xB4,  '`',  '^',  '~', 0xA8, 0xA3,  ':',  '"', 0xDC,    0,  '4',  '5',  '6',
         '|', 0xAC, 0xF7, 0xC7, 0xAA, 0xBA, 0xAF, 0xBF,  '<',  '>',  '?',    0,  '.',  '7',  '8',  '9', },
      { 0x00, 0x00, 0xFF, 0x07, 0xFF, 0x07, 0xFE, 0x00 } },
};

static const struct keymap *kmap = &keymaps[1];   /* sv; /etc/keymap later */

int kbd_set_keymap(const char *name)
{
    for (u32 i = 0; i < sizeof keymaps / sizeof keymaps[0]; i++)
        if (keymaps[i].name[0] == name[0] && keymaps[i].name[1] == name[1] &&
            name[2] == 0) {
            kmap = &keymaps[i];
            return 0;
        }
    return -1;
}

const char *kbd_keymap_name(void)
{
    return kmap->name;
}

/* layout-neutral keycodes 40-67 */
static const char keyspecial[0x28] = {
    ' ', '\b', '\t', '\r', '\r', 27, 127, 0, 0, 0, '-', 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '(', ')', '/', '*', '+', 0,
    0, 0, 0, 0, 0, 0, 0, 0,
};

/* cursor keys, function keys, Help and Del as the usual escape sequences. Only
 * programs in raw mode get them; Shift/Ctrl + Up/Down scroll the screen back. */
static void emit_seq(const char *q)
{
    if (!(tty_getmode(con_cur) & TTY_RAW))
        return;
    while (*q)
        tty_input((u8)*q++);
}

static int special_key(u8 code)
{
    static const char *const fkeys[10] = {
        "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~",
        "\033[17~", "\033[18~", "\033[19~", "\033[20~", "\033[21~" };
    int raw = (tty_getmode(con_cur) & TTY_RAW) != 0;
    if (code >= 0x4C && code <= 0x4F) {
        static const char arrow[4] = { 'A', 'B', 'C', 'D' };
        int mods = (shift_down ? 1 : 0) | (alt_down ? 2 : 0) | (ctrl_down ? 4 : 0);
        if (raw) {                                              /* programs get xterm's modified arrows: ESC[1;5A */
            char seq[8] = "\033[";
            int n = 2;
            if (mods) { seq[n++] = '1'; seq[n++] = ';'; seq[n++] = (char)('1' + mods); }
            seq[n++] = arrow[code - 0x4C];
            seq[n] = 0;
            emit_seq(seq);
            return 1;
        }
        if ((shift_down || ctrl_down) && code <= 0x4D) {        /* a plain terminal: scrollback */
            int step = ctrl_down ? trows - 1 : 1;
            term_scroll_view(code == 0x4C ? step : -step);
            return 1;
        }
        return 1;                                               /* cursor keys mean nothing to line editing in the kernel */
    }
    if (code >= 0x50 && code <= 0x59) {
        if (raw && (shift_down || ctrl_down || alt_down) && code - 0x50 >= 4) { /* modified F5..F10: ESC[15;2~ */
            static const char *const num[6] = { "15", "17", "18", "19", "20", "21" };
            char seq[16] = "\033["; int n = 2;
            for (const char *q = num[code - 0x54]; *q; q++) seq[n++] = *q;
            seq[n++] = ';'; seq[n++] = (char)('1' + ((shift_down ? 1 : 0) | (alt_down ? 2 : 0) | (ctrl_down ? 4 : 0))); seq[n++] = '~'; seq[n] = 0;
            emit_seq(seq);
            return 1;
        }
        if (raw && (shift_down || ctrl_down || alt_down) && code - 0x50 < 4) {  /* modified F1..F4: ESC[1;2P */
            char seq[10] = "\033[1;"; seq[4] = (char)('1' + ((shift_down ? 1 : 0) | (alt_down ? 2 : 0) | (ctrl_down ? 4 : 0))); seq[5] = "PQRS"[code - 0x50]; seq[6] = 0;
            emit_seq(seq);
            return 1;
        }
        emit_seq(fkeys[code - 0x50]);
        return 1;
    }
    if (code == 0x5F) { emit_seq("\033[2~"); return 1; }       /* Help = Insert */
    if (code == 0x46) { emit_seq("\033[3~"); return 1; }       /* Del */
    return 0;
}

/* a key going down (also again from kbd_work while it is held): translate and hand it to the tty */
static void key_down(u8 code)
{
    if (special_key(code))
        return;
    u8 c = 0;
    if (code == 0x41 && (tty_getmode(con_cur) & TTY_RAW))
        c = 127;                                    /* Backspace is DEL for raw-mode programs */
    else if (code < 0x40) {
        int shift = shift_down;
        if (caps_lock && (kmap->caps[code >> 3] >> (code & 7) & 1))
            shift = !shift;                         /* Caps Lock: Shift on the letters only */
        c = alt_down ? (shift ? kmap->shift_alt[code] : kmap->alt[code])
                     : (shift ? kmap->shifted[code] : kmap->normal[code]);
    } else if (code < 0x68)
        c = (u8)keyspecial[code - 0x40];
    if (!c)
        return;
    if (ctrl_down && (c & 0x40) && c < 0x80)
        c &= 0x1F;                      /* Ctrl-letter, incl. Ctrl-C */
    tty_input(c);
}

/* Software key repeat (an Amiga keyboard sends one code per press; AmigaOS repeats in software too): the last key
 * held repeats after KBD_REPEAT_DELAY ticks, then every KBD_REPEAT_RATE ticks, until it is released or another key
 * goes down. Modifier and Amiga-combination keys do not repeat. The tick only asks for the work (kbd_tick); the
 * repeat itself happens in kbd_work, in process context. */
#define KBD_REPEAT_DELAY 50             /* 500 ms */
#define KBD_REPEAT_RATE  3              /* about 33 per second */
static volatile u8 rep_code;            /* 0: nothing held; written by kbd_work only, read by the tick */
static u32 rep_at;                      /* tick of the next repeat */

/* Raw key codes from the interrupt to kbd_work: a ring with one producer (kbd_isr, level 2) and one consumer
 * (kbd_work, process context, never re-entered), so it needs no lock: the producer writes the slot and then moves
 * head, the consumer reads the slot and then moves tail. A full ring drops the code and counts it (kbd_work reports
 * the count on the kernel log) and, because the lost code may be a key going up, forgets the held modifiers and the
 * repeating key (a stuck key would repeat for ever). kwork runs at least between the 256-byte pieces of a long
 * terminal write (tty.c), so 128 slots (about 6 seconds of fast typing) are far more than it needs. */
#define KQ 128
static volatile u8 kq[KQ];
static volatile u16 kq_head, kq_tail;
static volatile u32 kq_lost;
static u32 kq_lost_told;

void kbd_tick(void)
{
    if (rep_code)
        kwork_pending = 1;              /* kbd_work checks whether the repeat is due */
}

/* Handshake: SP as output, low, for at least 85 us (HRM: "to ensure compatibility with all keyboard models"), then
 * back to input. Timed with the E-clock counter (hr_time, CIA-B timers A+B), not a counting loop: the CPU may be a
 * 14 MHz 68020 or Emu68 at about 1000 MIPS. 75 E-clock ticks = 106 us on PAL (709379 Hz), 105 us on NTSC. */
#define KBD_HANDSHAKE_TICKS 75
static int handshake_failed;

static void handshake(void)
{
    CIAA(CIA_CRA) |= 0x40;
    u32 t0 = hr_time(), n = 0;
    while ((u32)(hr_time() - t0) < KBD_HANDSHAKE_TICKS)
        if (++n > 20000) {              /* every hr_time() is six CIA accesses: this is far more than 85 us on any CPU */
            if (!handshake_failed++)
                kprintf("kbd: the E-clock counter did not advance during the keyboard handshake\n");
            break;
        }
    CIAA(CIA_CRA) &= (u8)~0x40;
}

/* From the level-2 interrupt (the tick handler sees the CIA's serial-port bit): take the byte, answer the keyboard,
 * queue the raw code. Nothing else happens here (audit B4): no keymap, no tty, no drawing. */
void kbd_isr(void)
{
    u8 raw = CIAA(CIA_SDR);
    u8 code = (u8)~((raw >> 1) | (raw << 7));
    handshake();
    u16 h = kq_head, nh = (u16)((h + 1) % KQ);
    if (nh == kq_tail) {
        kq_lost++;
        return;
    }
    kq[h] = code;
    kq_head = nh;
    kwork_pending = 1;
}

/* one raw code, in process context: modifiers, console keys, repeat, then the keymap and the tty */
static void key_code(u8 code)
{
    int up = code & 0x80;
    code &= 0x7F;
    switch (code) {
    case 0x60: case 0x61:               /* shift */
        shift_down = (u8)!up;
        return;
    case 0x62:                          /* Caps Lock: the keyboard sends "down" when its LED goes on, "up" when off */
        caps_lock = (u8)!up;
        return;
    case 0x63:                          /* control */
        ctrl_down = (u8)!up;
        return;
    case 0x64: case 0x65:               /* left / right Alt */
        alt_down = (u8)!up;
        return;
    case 0x66: case 0x67:               /* left / right Amiga */
        amiga_down = (u8)!up;
        return;
    }
    if (up) {
        if (code == rep_code)
            rep_code = 0;
        return;
    }
    if (amiga_down) {                   /* Amiga+F1..F10: virtual console 1..10 (Linux: Alt+Fn); other Amiga keys: nothing */
        rep_code = 0;
        if (code >= 0x50 && code <= 0x59)
            con_switch(code - 0x50 + 1);
        return;
    }
    rep_code = code;
    rep_at = ticks + KBD_REPEAT_DELAY;
    key_down(code);
}

/* Process context (kwork): everything the keyboard interrupt queued, then a key repeat if one is due. */
void kbd_work(void)
{
    while (kq_tail != kq_head) {
        u16 t = kq_tail;
        u8 code = kq[t];
        kq_tail = (u16)((t + 1) % KQ);
        key_code(code);
    }
    if (rep_code && (s32)(ticks - rep_at) >= 0) {
        rep_at = ticks + KBD_REPEAT_RATE;
        key_down(rep_code);
    }
    u32 lost = kq_lost;
    if (lost != kq_lost_told) {
        kprintf("kbd: %u key codes lost (input ring full)\n", lost - kq_lost_told);
        kq_lost_told = lost;
        rep_code = 0;                   /* a lost "key up" must not leave a key repeating or a modifier held */
        shift_down = ctrl_down = alt_down = amiga_down = 0;
    }
}
