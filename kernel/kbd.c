#include "kernel.h"
#include "hw.h"

/* Amiga keyboard: bytes arrive in CIA-A's serial shift register (SP/CNT
 * pins), raising ICR bit 3. Data is rotated and inverted on the wire;
 * after each byte the CPU must handshake by driving SP as output low
 * for at least 85us (HRM ch.8). Key release = keycode | 0x80.
 *
 * Keycodes 00-3F are positional; their meaning depends on the national
 * layout, so those live in selectable keymaps (Latin-1 output — the
 * console renders it, Topaz has the glyphs). 40-67 are layout-neutral. */

static u8 shift_down, ctrl_down, amiga_down;

struct keymap {
    char name[4];
    u8 normal[0x40];
    u8 shifted[0x40];
};

static const struct keymap keymaps[] = {
    { "us",
      { '`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\\', 0, '0',
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, '1', '2', '3',
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', 0, 0, '4', '5', '6',
        0, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '.', '7', '8', '9' },
      { '~', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '|', 0, '0',
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, '1', '2', '3',
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', 0, 0, '4', '5', '6',
        0, 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '.', '7', '8', '9' } },
    { "sv",       /* Swedish: Latin-1 for a-ring/a-diaeresis/o-diaeresis */
      { 0xA7, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '+', 0xB4, '\'', 0, '0',
        'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 0xE5, 0xA8, 0, '1', '2', '3',
        'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 0xF6, 0xE4, '\'', 0, '4', '5', '6',
        '<', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0, '.', '7', '8', '9' },
      { 0xBD, '!', '"', '#', 0xA4, '%', '&', '/', '(', ')', '=', '?', '`', '*', 0, '0',
        'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xC5, '^', 0, '1', '2', '3',
        'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xD6, 0xC4, '*', 0, '4', '5', '6',
        '>', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0, '.', '7', '8', '9' } },
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
    if (!(tty_getmode(ws_cur) & TTY_RAW))
        return;
    while (*q)
        tty_input((u8)*q++);
}

static int special_key(u8 code)
{
    static const char *const fkeys[10] = {
        "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~",
        "\033[17~", "\033[18~", "\033[19~", "\033[20~", "\033[21~" };
    if (code >= 0x4C && code <= 0x4F) {
        if ((shift_down || ctrl_down) && code <= 0x4D) {       /* scrollback */
            int step = ctrl_down ? TROWS - 1 : 1;
            term_scroll_view(code == 0x4C ? step : -step);
            return 1;
        }
        if (shift_down && code >= 0x4E) {                      /* Shift+Right/Left = End/Home */
            emit_seq(code == 0x4E ? "\033[F" : "\033[H");
            return 1;
        }
        static const char *const arrows[4] = { "\033[A", "\033[B", "\033[C", "\033[D" };
        emit_seq(arrows[code - 0x4C]);
        return 1;
    }
    if (code >= 0x50 && code <= 0x59) { emit_seq(fkeys[code - 0x50]); return 1; }
    if (code == 0x5F) { emit_seq("\033[2~"); return 1; }       /* Help = Insert */
    if (code == 0x46) { emit_seq("\033[3~"); return 1; }       /* Del */
    return 0;
}

void kbd_isr(void)
{
    u8 raw = CIAA(CIA_SDR);
    u8 code = (u8)~((raw >> 1) | (raw << 7));

    /* handshake: SP output (low) for >85us, then back to input */
    CIAA(CIA_CRA) |= 0x40;
    for (volatile int i = 0; i < 3000; i++)
        ;
    CIAA(CIA_CRA) &= (u8)~0x40;

    int up = code & 0x80;
    code &= 0x7F;
    switch (code) {
    case 0x60: case 0x61:               /* shift */
        shift_down = (u8)!up;
        return;
    case 0x63:                          /* control */
        ctrl_down = (u8)!up;
        return;
    case 0x66: case 0x67:               /* left / right Amiga */
        amiga_down = (u8)!up;
        return;
    }
    if (amiga_down) {                   /* workspace keys, like i3's mod key */
        if (!up && code >= 0x01 && code <= 0x09)
            ws_switch(code);
        else if (!up && code == 0x44)   /* Return: a shell here */
            ws_request_shell(ws_cur);
        return;
    }
    if (up)
        return;
    if (special_key(code))
        return;
    u8 c = 0;
    if (code == 0x41 && (tty_getmode(ws_cur) & TTY_RAW))
        c = 127;                                    /* Backspace is DEL for raw-mode programs */
    else if (code < 0x40)
        c = shift_down ? kmap->shifted[code] : kmap->normal[code];
    else if (code < 0x68)
        c = (u8)keyspecial[code - 0x40];
    if (!c)
        return;
    if (ctrl_down && (c & 0x40) && c < 0x80)
        c &= 0x1F;                      /* Ctrl-letter, incl. Ctrl-C */
    tty_input(c);
}
