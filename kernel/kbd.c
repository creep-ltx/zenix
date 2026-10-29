/* SPDX-License-Identifier: GPL-2.0-only */
/* The keymap tables below were generated from xkeyboard-config (MIT/X11 licence); its notice is above them. */
#include "kernel.h"
#include "hw.h"
#include "random.h"

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

/* cursor keys, function keys, Help and Del as the usual escape sequences. Only programs in raw mode get them.
 * Scrollback is a console key, Amiga + Up/Down (see kbd_key), like the console switch Amiga+F1..F10. */
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
            if (!mods && term_ckm())
                seq[1] = 'O';                                   /* cursor-key mode (DECCKM, ESC[?1h): ESC O A */
            if (mods) { seq[n++] = '1'; seq[n++] = ';'; seq[n++] = (char)('1' + mods); }
            seq[n++] = arrow[code - 0x4C];
            seq[n] = 0;
            emit_seq(seq);
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
static volatile u8 rep_code;            /* 0: nothing held; written by key_code's callers (kbd_work, CON_INPUT), read by the tick */
static u32 rep_at;                      /* tick of the next repeat */

/* Raw key codes from the interrupt to kbd_work: a ring with one producer (kbd_isr, level 2) and one consumer
 * (kbd_work, process context, never re-entered), so it needs no lock: the producer writes the slot and then moves
 * head, the consumer reads the slot and then moves tail. A full ring drops the code and counts it (kbd_work reports
 * the count on the kernel log) and, because the lost code may be a key going up, forgets the held modifiers and the
 * repeating key (a stuck key would repeat for ever). kwork runs at least between the 256-byte pieces of a long
 * terminal write (tty.c), so 128 slots (about 6 seconds of fast typing) are far more than it needs. */
static void reset_release(void);
static u32 rw_since;
#define KBD_RESET_MAX 800               /* 8 s: the reset-warning flush, at most (the keyboard waits 10 s) */
#define KQ 128
static volatile u8 kq[KQ];
static volatile u16 kq_head, kq_tail;
static volatile u32 kq_lost;
static u32 kq_lost_told;

/* SYS_reboot's sync gives every filesystem server at most 5 s (vfs_sync): one that hangs must not keep the machine from
 * resetting (audit A7). The tick does the reset then, as the keyboard does after the reset warning. */
volatile u32 reboot_deadline;
const char *volatile reboot_who;

void kbd_tick(void)
{
    if (reboot_deadline && (s32)(ticks - reboot_deadline) >= 0) {
        disk_frozen = 1;
        klog_set_reason(KLOG_END_REBOOT);
        kprintf("reboot: %s did not sync in 5 s; resetting\n", reboot_who);
        serial_flush();
        machine_reset();
    }
    if (rep_code)
        kwork_pending = 1;              /* kbd_work checks whether the repeat is due */
    if ((kbd_reset_state == KRS_HELD || kbd_reset_state == KRS_FLUSHING) && (u32)(ticks - rw_since) > KBD_RESET_MAX) {
        disk_frozen = 1;
        reset_release();                /* the flush took too long: let the keyboard reset (it would at 10 s anyway) */
    }
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

/* ---- the reset warning (HRM p.361; hand-off item K3) ----
 * Ctrl+Amiga+Amiga makes keyboards that have the feature send the reset-warning code $78 twice. The first is answered
 * like any key. On the second the Amiga must pull KDAT low within 250 ms; it then has up to 10 s before the keyboard
 * resets the machine, which it does as soon as KDAT goes high again. Only two consecutive $78 codes count (any other
 * code between them starts again), which the hardware hand-off note (2026-10-23) has from a long-proven driver.
 * Zenix keeps KDAT low (the handshake's output mode, not ended), lets init sync every filesystem in its own process
 * context (kbd_reset_flush, from proc_wait: the servers write what they hold and flush the drive's write cache, as
 * `sync` does; floppy writes are synchronous, so no track is half written while init runs), refuses every later disk
 * write (disk_frozen), writes a line to the kernel log - kept in chip RAM for the next boot - and releases KDAT. The
 * tick lets go after KBD_RESET_MAX ticks whatever happened, well inside the keyboard's 10 s. A keyboard without the
 * feature (the HRM: not every model) simply resets at once: nothing changes for it. */
volatile u8 kbd_reset_state;            /* KRS_*: 0 nothing; set by the interrupt, moved on by kbd_reset_flush */
static u8 rw_count;                     /* consecutive $78 codes */
static u8 rw_told;
volatile int disk_frozen;               /* set once the reset-warning flush is done: no new disk writes start */

static void reset_release(void)
{
    CIAA(CIA_CRA) &= (u8)~0x40;         /* KDAT high again: the keyboard resets the machine now */
    kbd_reset_state = KRS_DONE;
}

/* From the level-2 interrupt (the CIA-A serial-port interrupt, ICR bit 3): take the byte, answer the keyboard,
 * queue the raw code. Nothing else happens here (audit B4): no keymap, no tty, no drawing. */
void kbd_isr(void)
{
    u8 raw = CIAA(CIA_SDR);
    u8 code = (u8)~((raw >> 1) | (raw << 7));
    if (code == 0x78) {                 /* reset warning: never queued as a key */
        if (++rw_count >= 2 && kbd_reset_state == KRS_NONE) {
            CIAA(CIA_CRA) |= 0x40;      /* KDAT low, and it stays low until the disks are flushed */
            kbd_reset_state = KRS_HELD;
            rw_since = ticks;
            kwork_pending = 1;
            return;
        }
        handshake();
        return;
    }
    rw_count = 0;
    handshake();
    u16 h = kq_head, nh = (u16)((h + 1) % KQ);
    if (nh == kq_tail) {
        kq_lost++;
        return;
    }
    kq[h] = code;
    kq_head = nh;
    kwork_pending = 1;
    if ((code & 0x7F) < 0x78)
        rnd_event(RND_CR_KEY);          /* a key press or release: the keyboard's own clock (D19 9.3) */
}

/* K2 (hand-off note): at start the keyboard may still be waiting for the answer to a byte nobody took (the CIA's
 * interrupt flag was cleared with the rest at timer_init), so give it one handshake. Otherwise it would time out after
 * 143 ms (HRM p.359), resync and resend, and the first key typed at boot could be lost. */
void kbd_init(void)
{
    (void)CIAA(CIA_SDR);
    handshake();
}

/* In init's process context (proc_wait, WAIT_CONSOLE), once kbd_work has seen the held reset warning and woken it. */
/* A software reset, as exec's ColdReboot does it (ROM Kernel Manual, exec.library/ColdReboot): the ROM's start
 * address from its header, then RESET and JMP in one long word, so the JMP is already fetched when RESET takes the
 * ROM overlay and the chips back to their start state. */
void machine_reset(void)
{
    __asm__ volatile(
        "move.w #0x2700,%%sr\n\t"
        "lea 0x01000000,%%a0\n\t"
        "sub.l -0x14(%%a0),%%a0\n\t"          /* the ROM's size is in its last bytes: its start */
        "move.l 4(%%a0),%%a0\n\t"             /* the initial PC */
        "subq.l #2,%%a0\n\t"
        "bra.s 1f\n\t"
        ".balign 4\n"
        "1:\treset\n\t"
        "jmp (%%a0)\n" ::: "a0", "memory");
    for (;;)
        ;
}

void kbd_reset_flush(void)
{
    if (kbd_reset_state != KRS_HELD)
        return;
    kbd_reset_state = KRS_FLUSHING;
    svc_hurry();                                /* loaders in a back-off start at once (D11 6.3) */
    long r = vfs_sync(VS_BOUNDED);              /* a network mount answers within 2 s (D23 5.4) */
    disk_frozen = 1;
    if (kbd_reset_state != KRS_FLUSHING)        /* the tick gave up waiting: the machine is resetting */
        return;
    kprintf("kbd: reset warning: filesystems synced%s in %u ms, disk writes stopped; resetting\n",
            r < 0 ? " (with an error)" : "", (ticks - rw_since) * 10);
    reset_release();
}

/* one raw code, in process context: modifiers, console keys, repeat, then the keymap and the tty */
static void key_code(u8 code)
{
    if (code >= 0xF9 && code <= 0xFE) { /* the keyboard's own codes (HRM p.359-360, hand-off item K4), not keys */
        if (code == 0xF9 || code == 0xFA) {     /* lost sync / its buffer overflowed: key-up codes may be lost */
            rep_code = 0;
            shift_down = ctrl_down = alt_down = amiga_down = 0;
            kprintf("kbd: the keyboard reports %s; held keys forgotten\n",
                    code == 0xF9 ? "a lost sync" : "its buffer overflowed (keys lost)");
        } else if (code == 0xFC)
            kprintf("kbd: the keyboard reports a failed self test\n");
        return;                         /* $FD/$FE: start/end of the keys held at power-up */
    }
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
    if (amiga_down) {                   /* console keys, never seen by a program (Linux: Alt+Fn, Shift+PageUp) */
        rep_code = 0;
        if (code >= 0x50 && code <= 0x59)               /* Amiga+F1..F10: virtual console 1..10 */
            con_switch(code - 0x50 + 1);
        else if (code == 0x4C || code == 0x4D) {        /* Amiga+Up/Down: scroll back a screenful, with Shift a line */
            int step = shift_down ? 1 : trows - 1;      /* every Amiga keyboard has these (no Page Up on most) */
            term_scroll_view(code == 0x4C ? step : -step);
        }
        return;
    }
    rep_code = code;
    rep_at = ticks + KBD_REPEAT_DELAY;
    key_down(code);
}

/* SYS_console CON_INPUT's raw keys (DESIGN D24 6.3), in the caller's system call: process context like kbd_work, which
 * cannot run meanwhile (the kernel is not preempted), so each code goes to key_code as if typed; the interrupt's ring
 * is not touched (no second producer) and no randomness is credited (the timing came from the network). The reset
 * warning and the keyboard's status codes are refused (the whole call, nothing taken). At the end the modifiers the
 * call pressed and did not release are released, and a key it left down stops repeating: a client that went away
 * leaves no key held. The modifier states are the keyboard's own: one the call pressed is up at its end even if it is
 * held at the real keyboard too; Caps Lock, a toggle, is left as the call left it. A call stops early, as a terminal
 * write does, once two ticks have passed while input waits or a better process is ready -> the codes taken. */
int kbd_inject(const u8 *c, int n)
{
    for (int i = 0; i < n; i++)
        if ((c[i] & 0x7F) == 0x78 || c[i] >= 0xF8)
            return -EINVAL;
    u8 held = 0, mine = 0;                      /* bit m: modifier 0x60 + m pressed by this call; its repeating key */
    u32 t0 = ticks;
    int i;
    for (i = 0; i < n; i++) {
        if (i && (u32)(ticks - t0) >= 2 && (kwork_pending || need_resched))
            break;                              /* bounded as a terminal write: console switches redraw (D20 6) */
        u8 k = c[i], m = (u8)(k & 0x7F);
        if (m >= 0x60 && m <= 0x67 && m != 0x62)
            held = (u8)(k & 0x80 ? held & ~(1u << (m - 0x60)) : held | 1u << (m - 0x60));
        key_code(k);
        if (!(k & 0x80) && rep_code == m)
            mine = m;
    }
    for (int m = 0; m < 8; m++)
        if (held >> m & 1)
            key_code((u8)(0xE0 + m));
    if (mine && rep_code == mine)
        rep_code = 0;
    return i;
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
    if (kbd_reset_state == KRS_HELD && !rw_told) {
        rw_told = 1;
        kprintf("kbd: reset warning (Ctrl-Amiga-Amiga): syncing the disks before the reset\n");
        for (int i = 1; i < NPROC; i++)         /* init does it, in its own context (kbd_reset_flush) */
            if (procs[i].wait_con)
                ev_post(&procs[i], EV_CON);
    }
    u32 lost = kq_lost;
    if (lost != kq_lost_told) {
        kprintf("kbd: %u key codes lost (input ring full)\n", lost - kq_lost_told);
        kq_lost_told = lost;
        rep_code = 0;                   /* a lost "key up" must not leave a key repeating or a modifier held */
        shift_down = ctrl_down = alt_down = amiga_down = 0;
    }
}
