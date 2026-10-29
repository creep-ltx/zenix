/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
typedef unsigned char u8;
/* Terminal test: ANSI features, then a raw-mode key reader (q quits). For the screenshot tests (DESIGN D14):
 * `termtest columns` numbers the 80 columns in binary (the display's fetch window); `termtest colours` shows the 16
 * colours as blocks and as text on black and on white, with bold, underline and inverse; `termtest scroll` writes
 * coloured lines that scroll the screen several times, with background-colour erases, a scroll region, inserted and
 * deleted lines, then ends.
 * DESIGN D24 6.1-6.3 (root; tools/con-test.sh): `termtest conops` checks CON_READ's grid and CON_LINE's pens against what
 * it drew; `termtest rawkeys` raw key codes through CON_INPUT against the keyboard test's bytes (tools/kbd-test.sh);
 * `termtest text` text through CON_INPUT, `termtest readline` and `termtest readraw N` the same through the serial
 * line, printed alike for the script to compare; `termtest grab FILE` the shown display through CON_LINE (pens and
 * palette, "ZGRB"); `termtest depth N` CON_COLOURS. In `termtest colours` the key g grabs to /tmp/grab.zg. */
#define ESC "\033"
static void at(int r, int c, const char *s) { printf(ESC "[%d;%dH%s", r, c, s); }

static int grab(const char *path);

/* wait for q in raw mode, the cursor hidden or not; g grabs the display (D24, tools/colour-test.sh) */
static void keys(int show)
{
    sys_ttymode(TTY_RAW);
    for (;;) {
        char b[8];
        int n = sys_read(0, b, sizeof b);
        if (n <= 0 || b[0] == 'q') break;
        if (b[0] == 'g') { grab("/tmp/grab.zg"); continue; }
        if (show) {
            printf("key:");
            for (int i = 0; i < n; i++) printf(" %02x", (unsigned char)b[i]);
            printf("\n");
        }
    }
    sys_ttymode(0);
}

/* rows 1-7: column c lit (white, every plane) where bit (row - 1) of c is set; row 8 lit across all 80 columns */
static void columns(void)
{
    printf(ESC "[?25l" ESC "[2J");
    for (int k = 0; k < 8; k++) {
        printf(ESC "[%d;1H", k + 1);
        for (int c = 0; c < 80; c++)
            printf("%s", k == 7 || (c >> k & 1) ? ESC "[7;97m " ESC "[m" : " ");
    }
    printf(ESC "[10;1Hcolumns 0-79 in binary, row 1 the lowest bit; q quits");
    keys(0);
    printf(ESC "[?25h" ESC "[2J" ESC "[H");
}

/* rows 3-4: the 16 colours as background blocks, 5 columns each (colour i from column 5i); rows 6-9: the 16 as text
 * on black and on white, then bold, underline and inverse */
static void colours(void)
{
    printf(ESC "[?25l" ESC "[2J" ESC "[1;1Hthe 16 colours (ESC[40-47m, ESC[100-107m), as text on black and on white");
    for (int r = 3; r <= 4; r++) {
        printf(ESC "[%d;1H", r);
        for (int i = 0; i < 16; i++) printf(ESC "[%dm     ", i < 8 ? 40 + i : 100 + i - 8);
        printf(ESC "[m");
    }
    for (int b = 0; b < 2; b++) {
        printf(ESC "[%d;1H" ESC "[%dm", 6 + b, b ? 107 : 40);
        for (int i = 0; i < 16; i++) printf(ESC "[%dmAa%-2d ", i < 8 ? 30 + i : 90 + i - 8, i);
        printf(ESC "[m");
    }
    printf(ESC "[8;1H" ESC "[1;31mbold red" ESC "[m " ESC "[4;32munderlined green" ESC "[m " ESC "[7;34minverse blue"
           ESC "[m " ESC "[1;4;7;33mall three yellow" ESC "[m " ESC "[38;5;208m256: 208" ESC "[m "
           ESC "[38;2;0;200;200mtrue colour" ESC "[m");
    printf(ESC "[10;1Hq quits");
    keys(0);
    printf(ESC "[?25h" ESC "[m" ESC "[2J" ESC "[H");
}

/* line n in colour (n % 7) + 1, the line's rest erased in a background colour every 5th line */
static void scroll(void)
{
    printf(ESC "[m" ESC "[2J" ESC "[H");
    for (int n = 1; n <= 100; n++) {
        printf(ESC "[%dmline %d " ESC "[7minverse" ESC "[27m", 30 + n % 7 + 1, n);
        if (n % 5 == 0) printf(ESC "[4%dm" ESC "[K", n / 5 % 8);
        printf(ESC "[m\n");
    }
    printf(ESC "[5;20r" ESC "[20;1H");                          /* a region: scroll inside it */
    for (int n = 1; n <= 30; n++) printf("\n" ESC "[3%dm" ESC "[44mregion %d" ESC "[K" ESC "[m", n % 7 + 1, n);
    printf(ESC "[8;1H" ESC "[45m" ESC "[3L" ESC "[12;1H" ESC "[2M" ESC "[5;1H" ESC "[43m" ESC "M" ESC "[2S" ESC "[m");
    printf(ESC "[r" ESC "[31;1H" ESC "[42m" ESC "[J" ESC "[m");
}

/* DESIGN D20 step 1: a console's loaded font and the BBS rules (tools/colour-test.sh). FILE is a PSF1 font; without
 * one a test font made here: glyph b shows its two hex digits as bits two pixels wide (lines 1-3 and 4-6), so a
 * screenshot names every cell's glyph. On fd 1 (the console): the font, then every rule but LFONLY; drawn (rows 1-based):
 *   1      'H' after a 'Z' at 5,5 and ESC[2J (CONR_CLRHOME: the cursor went home)
 *   3-18   bytes 16r .. 16r + 15 in columns 1-16 (CONR_PCCTRL: the controls are glyphs; BEL BS HT LF CR ESC and NUL,
 *          which act, as spaces)
 *   20-21  the 16 colours as blocks of 5 columns (ESC[40-47m, ESC[100-107m: CONR_CGAPAL's palette)
 *   23     ESC[1;31m R, ESC[0;31m r (CONR_BRIGHT: R bright red, not bold); ESC[5;44m and two spaces, ESC[0;44m and two
 *          (CONR_ICE: a bright blue background, then the plain one), at columns 1-6
 *   24-26  80 W, then CR LF and X (CONR_WRAPNOW: row 25 stays empty, X at 26,1)
 *   27-28  with CONR_LFONLY: A, LF, B (B at 28,2)
 * Keys: q cleans up (the screen cleared, the font and rules dropped) and ends; x ends at once (the kernel drops them);
 * 2 4 8 6 switch the display to 2, 4, 8, 16 colours (root). */
static unsigned char *psf_file(const char *path)
{
    static unsigned char b[2052];
    if (!path) {
        b[0] = 0x36; b[1] = 0x04; b[2] = 0; b[3] = 8;
        static const unsigned char wide[16] = { 0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F,
                                                0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF };
        for (int c = 0; c < 256; c++) {         /* lines 1-3 the high nibble, 4-6 the low one, each bit 2 pixels */
            unsigned char *g = b + 4 + 8 * c;
            g[1] = g[2] = g[3] = wide[c >> 4];
            g[4] = g[5] = g[6] = wide[c & 15];
        }
        return b;
    }
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) { fprintf(2, "termtest: %s: %s\n", path, zstrerror(fd)); return 0; }
    int n = sys_read(fd, b, sizeof b);
    sys_close(fd);
    if (n != (int)sizeof b) { fprintf(2, "termtest: %s: not a 2,052-byte PSF1 font\n", path); return 0; }
    return b;
}

static int pcfont(const char *path)
{
    unsigned char *f = psf_file(path);
    if (!f) return 1;
    int r = sys_confont(1, f, 2052);
    if (r < 0) { fprintf(2, "termtest: CON_FONT: %s\n", zstrerror(r)); return 1; }
    u32 all = CONR_PCCTRL | CONR_WRAPNOW | CONR_CLRHOME | CONR_BRIGHT | CONR_ICE | CONR_CGAPAL;
    r = sys_conrules(1, all, all | CONR_LFONLY);
    if (r < 0) { fprintf(2, "termtest: CON_RULES: %s\n", zstrerror(r)); sys_confont(1, 0, 0); return 1; }
    printf(ESC "[?25l" ESC "[m" ESC "[5;5HZ" ESC "[2JH");
    for (int row = 0; row < 16; row++) {
        unsigned char l[16];
        printf(ESC "[%d;1H", row + 3);
        for (int c = 16 * row; c < 16 * row + 16; c++) {
            int acts = c == 0 || c == 7 || c == 8 || c == 9 || c == 10 || c == 13 || c == 27;
            l[c - 16 * row] = acts ? ' ' : (unsigned char)c;
        }
        sys_write(1, l, sizeof l);
    }
    for (int r2 = 20; r2 <= 21; r2++) {
        printf(ESC "[%d;1H", r2);
        for (int i = 0; i < 16; i++) printf(ESC "[%dm     ", i < 8 ? 40 + i : 100 + i - 8);
        printf(ESC "[m");
    }
    printf(ESC "[23;1H" ESC "[1;31mR" ESC "[0;31mr" ESC "[5;44m  " ESC "[0;44m  " ESC "[m");
    printf(ESC "[24;1H%s\r\nX", "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW");
    sys_conrules(1, CONR_LFONLY, CONR_LFONLY);
    printf(ESC "[27;1HA\nB");
    sys_conrules(1, 0, CONR_LFONLY);
    printf(ESC "[30;1H");
    sys_ttymode(TTY_RAW);
    for (;;) {
        char b[8];
        int n = sys_read(0, b, sizeof b);
        if (n <= 0 || b[0] == 'q') break;
        if (b[0] == 'x') return 0;              /* the kernel drops the font and the rules (D20 4.2) */
        int c = b[0] == '2' ? 2 : b[0] == '4' ? 4 : b[0] == '8' ? 8 : b[0] == '6' ? 16 : 0;
        if (c && (r = sys_console(CON_COLOURS, c)) < 0)
            fprintf(2, "termtest: colours %d: %s\n", c, zstrerror(r));
    }
    sys_ttymode(0);
    printf(ESC "[m" ESC "[2J" ESC "[H" ESC "[?25h");       /* cleared before the font goes (D20 4.2) */
    sys_conrules(1, 0, CONR_PCCTRL | CONR_WRAPNOW | CONR_CLRHOME | CONR_BRIGHT | CONR_ICE | CONR_CGAPAL | CONR_LFONLY);
    sys_confont(1, 0, 0);
    return 0;
}


/* ---- DESIGN D24 6.1-6.3: the remote control's console ops ---- */
static char res[4096];                  /* the checks' lines, printed once the screen is no longer being checked */
static u32 resn;
static int fails;
static void ck(int ok, const char *what, int got)
{
    resn += (u32)snprintf(res + resn, sizeof res - resn, "%s %s (%d)\n", ok ? "PASS" : "FAIL", what, got);
    if (!ok)
        fails++;
}
static int report(const char *name)
{
    sys_write(1, res, resn);
    printf("%s: %d failed\n", name, fails);
    return fails != 0;
}
static void out(const char *s) { sys_write(1, s, strlen(s)); }
static int conread(int fd, struct uconread *r, void *cells, u32 n)
{
    memset(r, 0, sizeof *r);
    r->size = sizeof *r;
    r->cells = (u32)cells;
    r->ncells = n;
    return sys_conread(fd, r);
}
static int conline(int fd, int line, struct uconline *d, void *pens, u32 n)
{
    memset(d, 0, sizeof *d);
    d->size = sizeof *d;
    d->pens = (u32)pens;
    d->npens = n;
    return sys_conline(fd, line, d);
}
static int ttyfd(int con)
{
    char p[12];
    snprintf(p, sizeof p, "/dev/tty%d", con);
    return sys_open(p, O_RDWR);
}

static int conops(void)
{
    int rows = sys_ttyinfo() >> 16, depth = 0, r;
    for (int c = sys_console(CON_COLOURS, 0); c > 1; c >>= 1) depth++;
    u8 *g = malloc((u32)rows * 80 * 3 + 8), *pens = malloc(656);
    struct uconread h;
    struct uconline d;
    if (!g || !pens) { fprintf(2, "termtest: out of memory\n"); return 1; }
    out(ESC "[?25h" ESC "[m" ESC "[2J" ESC "[HABC" ESC "[2;1H" ESC "[1;4;7;31;42mxyz" ESC "[m" ESC "[3;71H\xe5"
        ESC "[5;1H" ESC "[44m " ESC "[m" ESC "[6;11H");
    r = conread(1, &h, g, (u32)rows * 80 * 3);
    ck(r == rows * 240 && h.rows == rows && h.cols == 80 && h.csize == 3, "CON_READ: the whole grid, 3 bytes a cell", r);
    ck(h.crow == 5 && h.ccol == 10, "CON_READ: the cursor", h.crow * 100 + h.ccol);
    ck((h.flags & (CONRD_SHOWN | CONRD_CURSOR | CONRD_FONT | CONRD_BACK)) == (CONRD_SHOWN | CONRD_CURSOR), "CON_READ: shown, cursor on, no font", h.flags);
#define CELL(y, x, c, a, o) (g[3 * ((y) * 80 + (x))] == (c) && g[3 * ((y) * 80 + (x)) + 1] == (a) && g[3 * ((y) * 80 + (x)) + 2] == (o))
    ck(CELL(0, 0, 'A', 0, 0x07) && CELL(0, 2, 'C', 0, 0x07) && CELL(0, 3, ' ', 0, 0x07), "CON_READ: plain text", g[0]);
    ck(CELL(1, 0, 'x', CONA_INV | CONA_UND | CONA_BOLD, 0x21) && CELL(1, 2, 'z', 7, 0x21) && CELL(1, 3, ' ', 0, 0x07),
       "CON_READ: attributes and colours (fg low nibble, bg high)", g[3 * 80 + 2]);
    ck(CELL(2, 70, 0xE5, 0, 0x07) && CELL(4, 0, ' ', 0, 0x47) && CELL(rows - 1, 79, ' ', 0, 0x07), "CON_READ: Latin-1, an erase colour, the last cell", g[3 * (2 * 80 + 70)]);
    u32 c0 = h.changes;
    out(ESC "[6;11H");
    conread(1, &h, 0, 0);
    ck(h.changes != c0, "CON_READ: the change counter moves with a write", (int)(h.changes - c0));
    memset(g, 0xAA, 8);
    r = conread(1, &h, g, 5);
    ck(r == 3 && g[0] == 'A' && g[3] == 0xAA, "CON_READ: a 5-byte buffer gets one whole cell", r);
    h.size = 0;
    h.ncells = 0;
    r = sys_conread(1, &h);
    ck(r == 0 && h.rows == rows && h.size == UCONREAD_FIRST, "CON_READ: the header alone; size written back", r);
    h.size = 4;
    ck(sys_conread(1, &h) == -EINVAL, "CON_READ: a struct below its first size", 0);
    h.size = 0;
    h.ncells = 0;
    ck(sys_conread(1, &h) == 0, "CON_READ: size 0 is the first size", 0);
    ck(sys3(89, CON_READ | 2u << 8, (u32)&h, 1) == -EINVAL, "CON_READ: a3 not 0", 0);
    ck(sys_conread(14, &h) == -EBADF, "CON_READ: no such descriptor", 0);
    int nul = sys_open("/dev/null", O_RDWR), t9 = ttyfd(9);
    ck(sys_conread(nul, &h) == -ENOTTY, "CON_READ: not a console", 0);
    sys_close(nul);
    for (int k = 0; k < 2; k++) {
        r = conread(t9, &h, g, 240);
        ck(r == -ENXIO, "CON_READ of a console never used: ENXIO (twice: nothing allocated)", r);
    }
    ck(conline(t9, 0, &d, pens, 640) == -ENXIO && sys_coninput(t9, CONI_TEXT, "x", 1) == -ENXIO,
       "CON_LINE, CON_INPUT text of a console never used: ENXIO", 0);
    sys_close(t9);
    ck(sys3(89, 8 | 2u << 8, 0, 0) == -EINVAL && sys3(89, 4 | 2u << 8, 0, 0) == -EINVAL, "ops 4 and 8: EINVAL", 0);
    /* CON_LINE */
    r = conline(1, 0, &d, 0, 0);
    ck(r == 0 && d.width == 640 && d.height == rows * 8 && d.depth == depth, "CON_LINE: the display's facts", d.depth);
    u32 gen = d.generation;
    ck(d.palette[0] == 0 && (depth < 3 || d.palette[4] == 0x45E) && (depth == 4 || d.palette[1 << depth] == 0),
       "CON_LINE: the palette as the Copper shows it", d.palette[4]);
    int blue = depth >= 3 ? 4 : depth == 2 ? 2 : 0, grey = depth >= 3 ? 7 : depth == 2 ? 3 : 1, ok = 1;
    for (int l = 32; l < 40 && ok; l++) {
        ok = conline(1, l, &d, pens, 640) == 640;
        for (int x = 0; x < 16 && ok; x++)
            ok = pens[x] == (x < 8 ? blue : 0);
    }
    ck(ok, "CON_LINE: a blue cell's 8 x 8 pens, the next cell's background", pens[0]);
    int lit = 0;
    ok = 1;
    for (int l = 0; l < 8 && ok; l++) {
        ok = conline(1, l, &d, pens, 640) == 640;
        for (int x = 0; x < 640 && ok; x++) {
            ok = x < 24 ? pens[x] == 0 || pens[x] == grey : pens[x] == 0;
            lit += x < 8 && pens[x] == grey;
        }
    }
    ck(ok && lit > 8, "CON_LINE: 'A' drawn in grey in its cell, nothing else on the first row", lit);
    ck(conline(1, rows * 8, &d, pens, 640) == -EINVAL, "CON_LINE: a line below the display", 0);
    ck(conline(1, 0, &d, pens, 7) == 0 && conline(1, 0, &d, pens, 650) == 640, "CON_LINE: whole bytes of 8 pens, at most 640", 0);
    ck(d.generation == gen, "CON_LINE: the generation stays while nothing changes", (int)d.generation);
    /* not root */
    sys_setuid(1000);
    ck(conread(1, &h, 0, 0) == -EPERM && conline(1, 0, &d, 0, 0) == -EPERM && sys_coninput(1, CONI_TEXT, "x", 1) == -EPERM,
       "not root: EPERM", 0);
    out(ESC "[2J" ESC "[H");
    return report("conops");
}

/* the bytes waiting on the tty (raw mode), without waiting for more */
static int take(u8 *b, int max)
{
    int n = 0;
    struct pollfd p = { 0, POLLIN, 0 };
    while (n < max && poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
        int k = sys_read(0, b + n, (u32)(max - n));
        if (k <= 0) break;
        n += k;
    }
    return n;
}
static int keys_give(const char *codes, int n, const char *want, int wn, const char *what)
{
    u8 b[32];
    int r = sys_coninput(1, CONI_RAW, codes, (u32)n);
    int got = take(b, sizeof b);
    int ok = r == n && got == wn && !memcmp(b, want, (u32)wn);
    char m[160];
    int k = snprintf(m, sizeof m, "%s:", what);
    for (int i = 0; i < got && k < 150; i++) k += snprintf(m + k, sizeof m - (u32)k, " %02x", b[i]);
    ck(ok, m, r);
    return ok;
}

static int rawkeys(void)
{
    struct uconread h;
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    take((u8 *)res, 64);                                        /* anything typed before */
    /* tools/kbd-test.sh's k1 and k2, by the same key positions: the Swedish layers, then Caps Lock */
    keys_give("\x1a\x9a\x29\xa9\x2a\xaa\x60\x03\x83\xe0\x65\x1a\x9a\xe5\x65\x60\x1a\x9a\xe0\xe5\x65\x02\x82\xe5\x65\x30\xb0\xe5",
              28, "\xe5\xf6\xe4\xa3\x5b\x7b\xb2\x5c", 8, "raw keys: Swedish layers plain, Shift, Alt, Shift+Alt");
    keys_give("\x62\x20\xa0\x35\xb5\x01\x81\x60\x33\xb3\xe0\xe2\x22\xa2", 14, "AB1cd", 5,
              "raw keys: Caps Lock on letters only, Shift inverts it");
    keys_give("\x63\x33\xb3\xe3\x4c\xcc", 6, "\x03\x1b[A", 4, "raw keys: Ctrl-C (raw, no signals) and an arrow");
    keys_give("\x60", 1, "", 0, "raw keys: Shift down alone");
    keys_give("\x20\xa0", 2, "a", 1, "raw keys: ... was released at the end of its call");
    keys_give("\x31", 1, "z", 1, "raw keys: z down, no up");
    sys_sleep(100);
    keys_give("\xb1", 1, "", 0, "raw keys: ... does not repeat after its call (1 s later)");
    ck(sys_coninput(1, CONI_RAW, "\x20\x78", 2) == -EINVAL && sys_coninput(1, CONI_RAW, "\xf9", 1) == -EINVAL,
       "raw keys: the reset warning and status codes refused", 0);
    keys_give("", 0, "", 0, "raw keys: ... and nothing of that call typed");
    ck(sys_coninput(1, CONI_RAW, res, 257) == -EINVAL && sys_coninput(1, 2, "a", 1) == -EINVAL, "CON_INPUT: 257 bytes, kind 2", 0);
    sys_coninput(1, CONI_RAW, "\x66\x51\xd1\xe6", 4);           /* Amiga+F2 */
    int t2 = ttyfd(2);
    struct uconline d;
    conread(1, &h, 0, 0);
    int one = h.flags;
    ck(!(one & CONRD_SHOWN) && conread(t2, &h, 0, 0) == 0 && (h.flags & CONRD_SHOWN) && conline(1, 0, &d, 0, 0) == -ENXIO,
       "raw keys: Amiga+F2 shows console 2 (CON_LINE of console 1 now ENXIO)", one);
    sys_coninput(t2, CONI_RAW, "\x66\x50\xd0\xe6", 4);          /* Amiga+F1, through console 2's descriptor */
    conread(1, &h, 0, 0);
    ck(h.flags & CONRD_SHOWN, "raw keys: Amiga+F1 back on console 1", h.flags);
    sys_close(t2);
    sys_ttymode(0);
    return report("rawkeys");
}

static void hex(const char *tag, const u8 *b, int n)
{
    printf("%s:", tag);
    for (int i = 0; i < n; i++) printf("%02x", b[i]);
    printf("\n");
}

static volatile int got_int;
static void on_int(int s) { (void)s; got_int = 1; }

static int text(void)
{
    u8 b[64];
    int r, n;
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    take(b, sizeof b);
    r = sys_coninput(1, CONI_TEXT, "ab\x03" "c\r", 5);
    n = take(b, sizeof b);
    ck(r == 5, "CON_INPUT text, raw mode: all taken", r);
    hex("raw", b, n);
    u8 *q = malloc(1024);
    if (!q) return 1;
    memset(q, 'q', 1024);
    int k1 = sys_coninput(1, CONI_TEXT, q, 256), k2 = sys_coninput(1, CONI_TEXT, q, 256),
        k3 = sys_coninput(1, CONI_TEXT, q, 256), k4 = sys_coninput(1, CONI_TEXT, q, 256);
    sys_ttymode(TTY_RAW);                                       /* signals on: ^C into the full queue */
    signal(SIGINT, on_int);
    int kc = sys_coninput(1, CONI_TEXT, "\x03", 1);
    ck(kc == 1 && got_int, "CON_INPUT text: ^C gets through a full queue (SIGINT)", kc);
    signal(SIGINT, SIG_DFL);
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    n = take(q, 1024);
    ck(k1 == 256 && k2 == 256 && k3 == 256 && k4 == 255 && n == 1023, "CON_INPUT text: a full queue takes fewer and says so", k4);
    sys_ttymode(0);
    r = sys_coninput(1, CONI_TEXT, "abc\b\bX\r", 7);
    n = sys_read(0, b, sizeof b);
    ck(r == 7, "CON_INPUT text, cooked: all taken", r);
    hex("line", b, n < 0 ? 0 : n);
    return report("text");
}

static int readline(void)
{
    u8 b[64];
    printf("readline:\n");
    int n = sys_read(0, b, sizeof b);
    hex("line", b, n < 0 ? 0 : n);
    return 0;
}

static int readraw(int want)
{
    u8 b[64];
    int n = 0;
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    printf("readraw:\n");
    while (n < want && n < (int)sizeof b) {
        int k = sys_read(0, b + n, (u32)(want - n));
        if (k <= 0) break;
        n += k;
    }
    sys_ttymode(0);
    hex("raw", b, n);
    return 0;
}

/* the shown display through CON_LINE: "ZGRB", u16 width, height, depth, the 16 palette entries (12 bits, big-endian),
 * then a pen a pixel, line by line */
static int grab(const char *path)
{
    struct uconread h;
    struct uconline d;
    int fd = -1;
    for (int c = 1; c <= 10 && fd < 0; c++) {
        fd = ttyfd(c);
        if (fd >= 0 && (conread(fd, &h, 0, 0) || !(h.flags & CONRD_SHOWN))) { sys_close(fd); fd = -1; }
    }
    u8 *pens = malloc(640);
    if (fd < 0 || !pens || conline(fd, 0, &d, 0, 0)) { fprintf(2, "termtest: grab: no display\n"); return 1; }
    int o = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (o < 0) { fprintf(2, "termtest: %s: %s\n", path, zstrerror(o)); return 1; }
    u8 hd[42] = { 'Z', 'G', 'R', 'B', (u8)(d.width >> 8), (u8)d.width, (u8)(d.height >> 8), (u8)d.height, 0, (u8)d.depth };
    for (int i = 0; i < 16; i++) { hd[10 + 2 * i] = (u8)(d.palette[i] >> 8); hd[11 + 2 * i] = (u8)d.palette[i]; }
    sys_write(o, hd, sizeof hd);
    for (int l = 0; l < d.height; l++) {
        if (conline(fd, l, &d, pens, 640) != 640) { fprintf(2, "termtest: grab: line %d\n", l); return 1; }
        sys_write(o, pens, 640);
    }
    sys_close(o);
    sys_close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "conops")) return conops();
    if (argc > 1 && !strcmp(argv[1], "rawkeys")) return rawkeys();
    if (argc > 1 && !strcmp(argv[1], "text")) return text();
    if (argc > 1 && !strcmp(argv[1], "readline")) return readline();
    if (argc > 2 && !strcmp(argv[1], "readraw")) return readraw(atoi(argv[2]));
    if (argc > 2 && !strcmp(argv[1], "grab")) return grab(argv[2]);
    if (argc > 2 && !strcmp(argv[1], "depth")) { int r = sys_console(CON_COLOURS, atoi(argv[2])); printf("colours %d\n", r); return r < 0; }
    if (argc > 1 && !strcmp(argv[1], "pcfont")) return pcfont(argc > 2 ? argv[2] : 0);
    if (argc == 3 && !strcmp(argv[1], "rules")) {          /* the rules of another console (root): D20's tests */
        int fd = sys_open(argv[2], O_RDWR);
        int r = fd < 0 ? fd : sys_conrules(fd, 0, 0);
        if (r < 0) { fprintf(2, "termtest: %s: %s\n", argv[2], zstrerror(r)); return 1; }
        printf("rules %d\n", r);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "columns")) { columns(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "colours")) { colours(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "scroll")) { scroll(); return 0; }
    if (argc > 1) { fprintf(2, "usage: termtest [columns|colours|scroll|pcfont [FILE]|rules TTY|conops|rawkeys|text|readline|readraw N|grab FILE|depth N]\n"); return 2; }
    int sz = sys_ttyinfo();
    printf(ESC "[2J" ESC "[H");
    printf(ESC "[7m Zenix terminal test " ESC "[0m  size %dx%d\n", sz & 0xFFFF, sz >> 16);
    printf("normal " ESC "[1mbold" ESC "[0m " ESC "[4munderline" ESC "[0m " ESC "[7minverse" ESC "[0m "
           ESC "[1;4;7mall three" ESC "[0m\n");
    /* the box is 36 columns wide: its longest line (27 characters from column 5) keeps a margin */
    at(4, 1, "+----------------------------------+");
    for (int r = 5; r < 9; r++) { at(r, 1, "|"); at(r, 36, "|"); }
    at(9, 1, "+----------------------------------+");
    at(6, 5, "cursor addressing works");
    at(7, 5, "ESC[row;colH, erase, scroll");
    /* scroll region rows 11..14: print 8 numbers, only that area scrolls */
    at(10, 1, "scroll region (rows 11-14):");
    printf(ESC "[11;14r");
    for (int i = 1; i <= 8; i++) printf(ESC "[14;1H\nline %d", i);
    printf(ESC "[r");
    at(16, 1, "erase line demo: ");
    printf("XXXXXXXXXXXX" ESC "[6D" ESC "[K|end\n");
    at(17, 1, "insert/delete line: A");
    at(18, 1, "B");
    at(19, 1, "C");
    printf(ESC "[18;1H" ESC "[1L" "inserted");
    at(20, 1, "keys: press arrows/F-keys/Del, q quits");
    at(22, 1, "");
    keys(1);
    printf("done\n");
    return 0;
}
