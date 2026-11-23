/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "regex.h"
/* less [file...]   (also installed as more)
 * Keys: Space/f page down, b page up, Enter/j/Down line down, k/Up line up, d/u half page, g/Home top, G/End bottom,
 * / pattern search, ? backward search, n/N next/previous match, h help, q quit.
 * Short files are just printed. Text from a pipe works: the keys come from /dev/tty. Colour codes (SGR, ESC [ ... m)
 * pass through, as GNU less -R does, and take no room on the line; other control characters are shown as ^X. */
#define ROWS zscr_rows                   /* the terminal's rows (libz's screen layer: the size, SIGWINCH) */
static char *text; static u32 tlen, tcap;
struct row { u32 off; unsigned short len; };
static struct row *rows; static u32 nrows, rcap;
static int tty_fd = -1, top;
static void *re; static const char *name = "(standard input)";

static void append(const char *b, u32 n)
{
    if (tlen + n + 1 > tcap) { u32 nc = tcap ? tcap * 2 : 8192; while (nc < tlen + n + 1) nc *= 2; char *nt = realloc(text, nc); if (!nt) { dprintf(2, "less: out of memory\n"); _zexit(1); } text = nt; tcap = nc; }
    memcpy(text + tlen, b, n); tlen += n;
}
static void add_row(u32 off, u32 len)
{
    if (nrows == rcap) { u32 nc = rcap ? rcap * 2 : 512; struct row *nr = realloc(rows, nc * sizeof *nr); if (!nr) { dprintf(2, "less: out of memory\n"); _zexit(1); } rows = nr; rcap = nc; }
    rows[nrows].off = off; rows[nrows].len = (unsigned short)len; nrows++;
}
/* the length of a colour code (ESC [ digits ; : m) at text[i], else 0 */
static u32 sgr_len(u32 i, u32 end)
{
    if (text[i] != 27 || i + 2 >= end || text[i + 1] != '[') return 0;
    u32 k = i + 2;
    while (k < end && ((text[k] >= '0' && text[k] <= '9') || text[k] == ';' || text[k] == ':')) k++;
    return k < end && text[k] == 'm' ? k + 1 - i : 0;
}
/* split the text into screen rows: lines of up to the terminal's width (zscr_cols), tabs to the next multiple of 8; DESIGN D28: by
 * characters and the console's widths (a UTF-8 letter one column, a wide one two), never inside a character */
static void layout(void)
{
    nrows = 0;
    u32 i = 0;
    while (i < tlen) {
        u32 start = i, col = 0;
        while (i < tlen && text[i] != '\n') {
            u32 sl = sgr_len(i, tlen);
            if (sl) { i += sl; continue; }                  /* a colour code: no width */
            u32 w = text[i] == '\t' ? 8 - (col % 8) : ((unsigned char)text[i] < 32 ? 2u : 1u), k = 1;
            if ((unsigned char)text[i] >= 0x80) { int cw; k = (u32)zu8next(text + i, tlen - i, &cw); w = (u32)cw; }
            if (col + w > (u32)zscr_cols && col > 0) { add_row(start, i - start); start = i; col = 0; }
            col += w; i += k;
        }
        add_row(start, i - start);
        if (i < tlen) i++;                                  /* the newline */
    }
}
/* a row; a row that continues a long line first gets the colour codes of the line's earlier rows, and every row
 * ends with the colours reset, so a colour never runs into the rows below or the status line */
static void put_row(const struct row *r)
{
    u32 col = 0, ls = r->off, end = r->off + r->len;
    while (ls > 0 && text[ls - 1] != '\n') ls--;
    int coloured = 0;
    for (u32 k = ls; k < r->off; k++) { u32 sl = sgr_len(k, r->off); if (sl) { oputn(text + k, sl); k += sl - 1; coloured = 1; } }
    for (u32 k = r->off; k < end; k++) {
        unsigned char c = (unsigned char)text[k];
        u32 sl = sgr_len(k, end);
        if (sl) { oputn(text + k, sl); k += sl - 1; coloured = 1; continue; }
        if (c == '\t') { do { oputc(' '); col++; } while (col % 8); }
        else if (c < 32) { oputc('^'); oputc(c + 64); col += 2; }
        else if (c >= 0x80) { int w, n = zu8next(text + k, end - k, &w); oputn(text + k, (u32)n); k += (u32)n - 1; col += (u32)w; }
        else { oputc(c); col++; }
    }
    if (coloured) oputs("\033[m");
}
static int row_matches(u32 r)
{
    if (!re) return 0;
    int caps[2 * RE_GROUPS];
    return re_exec(re, text + rows[r].off, rows[r].len, 0, caps);
}
static void status(const char *extra)
{
    u32 last = (u32)top + ROWS - 1; if (last > nrows) last = nrows;
    u32 pct = nrows ? last * 100 / nrows : 100;
    oputs("\033[7m");
    char b[160];
    snprintf(b, sizeof b, " %s  lines %d-%u/%u  %u%%  %s", name, top + 1, last, nrows, pct, extra ? extra : "(h help, q quit)");
    oputs(b);
    oputs("\033[K\033[0m");
}
static void draw(const char *extra)
{
    oputs("\033[H");
    for (int r = 0; r < ROWS - 1; r++) {
        if ((u32)(top + r) < nrows) put_row(&rows[top + r]);
        oputs("\033[K\r\n");
    }
    status(extra);
    oflush();
}
#define K_RESIZE 0x7FFF
static int getb(int ms)                 /* a byte from the terminal within ms (-1: none) */
{
    unsigned char c;
    struct pollfd p = { tty_fd, POLLIN, 0 };
    if (ms >= 0 && poll(&p, 1, ms) <= 0) return -1;
    return sys_read(tty_fd, &c, 1) == 1 ? c : -1;
}
/* a key from the terminal: a byte, or libz's code (ZK_*) for the arrows, Home, End, the pages, F1 and Help (the
 * console's xterm sequences); a new window size (SIGWINCH, libz's screen layer) as K_RESIZE */
static int read_key(void)
{
    for (;;) {
        struct pollfd p = { tty_fd, POLLIN, 0 };
        if (zscr_resized()) return K_RESIZE;
        if (poll(&p, 1, 500) > 0) break;
    }
    int c = getb(-1);
    if (c < 0) return 'q';
    if (c != 27) return c;
    int s1 = getb(30), s2 = 0, num = 0;
    if (s1 < 0) return 27;
    if (s1 == 'O') { s2 = getb(30); return s2 == 'P' ? ZK_F1 : s2 == 'H' ? 1 : s2 == 'F' ? 5 : 0; }
    if (s1 != '[') return 0;
    while ((s2 = getb(30)) >= '0' && s2 <= '9') num = num * 10 + (s2 - '0');
    if (s2 == ';') while ((s2 = getb(30)) >= '0' && s2 <= '9') ;      /* a modifier: the key is the same */
    switch (s2) {
    case 'A': return ZK_UP;
    case 'B': return ZK_DOWN;
    case 'C': return ZK_RIGHT;
    case 'D': return ZK_LEFT;
    case 'H': return 1;
    case 'F': return 5;
    case 'P': return ZK_F1;
    case '~': return num == 1 || num == 7 ? 1 : num == 4 || num == 8 ? 5 : num == 3 ? 4 : num == 5 ? ZK_SUP :
                     num == 6 ? ZK_SDOWN : num == 2 ? ZK_HELP : num == 11 ? ZK_F1 : 0;
    }
    return 0;
}
/* the keys as less's letters */
static int less_key(int k)
{
    switch (k) {
    case ZK_UP: return 'k';
    case ZK_DOWN: return 'j';
    case 1: return 'g';                 /* Home */
    case 5: return 'G';                 /* End */
    case ZK_SUP: return 'b';            /* Page Up */
    case ZK_SDOWN: return 'f';          /* Page Down */
    case ZK_F1: case ZK_HELP: return 'h';
    }
    return k == K_RESIZE || (k > 0 && k < 0x80) ? k : 0;
}
static void prompt_line(const char *label, char *out, int max)     /* libz's line editor on the bottom row */
{
    struct zled e;
    zled_init(&e, out, max, "");
    for (;;) {
        zled_show(&e, ROWS, 1, zscr_cols - 1, label);
        oflush();
        int k = read_key(), r = k == K_RESIZE ? ZLED_MORE : zled_key(&e, k);
        if (r == ZLED_DONE) break;
        if (r == ZLED_CANCEL || r == ZLED_EOF) { out[0] = 0; break; }
    }
}
static void search(int dir, int again)
{
    static char pat[80]; static int last_dir = 1;
    if (!again) {
        char in[80]; prompt_line(dir > 0 ? "/" : "?", in, sizeof in);
        if (!in[0]) { draw(0); return; }
        strcpy(pat, in); last_dir = dir;
        if (re) re_free(re);
        re = re_comp(pat, RE_ICASE | RE_LONGEST);
        if (!re) { draw("bad pattern"); return; }
    } else if (!re) { draw("no pattern"); return; } else dir = dir * last_dir;
    long from = top + (dir > 0 ? 1 : -1);
    for (long r = from; r >= 0 && r < (long)nrows; r += dir)
        if (row_matches((u32)r)) { top = (int)r; draw(0); return; }
    draw("pattern not found");
}
static void help(void)
{
    oputs("\033[2J\033[H");
    oputs("  less - keys\r\n\r\n"
          "  Space, f         next page            b           previous page\r\n"
          "  Enter, j, Down   next line            k, Up       previous line\r\n"
          "  d / u            half a page down/up  g, Home     top\r\n"
          "  G, End           bottom               /pattern    search forward\r\n"
          "  ?pattern         search backward      n / N       next / previous match\r\n"
          "  q                quit\r\n\r\n  (any key to go back)");
    oflush();
    read_key();
}
/* signals (DESIGN D16): the terminal is left as it was found whatever ends or stops less. SIGINT, SIGTERM, SIGHUP:
 * cooked mode back, the cursor on the last line, then the end (128 + the signal). SIGTSTP (Ctrl+Z, which less sends its
 * job itself, or a kill): the same, then it stops; SIGCONT: raw mode again and the page drawn anew. */
static void leave_screen(void)
{
    sys_ttymode(0);
    oputs("\033[H"); for (int r = 0; r < ROWS - 1; r++) oputs("\033[B");
    oputs("\r\033[K");
    oflush();
}
static void on_end(int sig) { leave_screen(); sys_exit(128 + sig); }
static void on_tstp(int sig)
{
    leave_screen();
    signal(sig, SIG_DFL);
    sigset_t m = 1u << sig;
    sigprocmask(SIG_UNBLOCK, &m, 0);
    raise(sig);                         /* stops here, until SIGCONT */
    signal(sig, on_tstp);
}
static void on_cont(int sig)
{
    (void)sig;
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    oputs("\033[2J");
    draw(0);
}
static const char USAGE[] = "usage: less file...   (or: command | less)\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int i = 1, bad = 0;                                            /* a file that could not be read: status 1 */
    zscr_size();                                                   /* the terminal's size: the rows' width too */
    if (ROWS < 3) ROWS = 3;
    while (zisopt(argc, argv, &i, USAGE)) i++;         /* options are accepted and ignored */
    int tty_out = sys_isatty(1);
    if (i >= argc) {
        if (sys_isatty(0)) zusage(USAGE);
        char b[1024]; int n; while ((n = sys_read(0, b, sizeof b)) > 0) append(b, (u32)n);
    } else {
        for (; i < argc; i++) {
            int fd = sys_open(argv[i], O_RDONLY);
            if (fd < 0) { dprintf(2, "less: %s: %s\n", argv[i], zwhy("cannot open")); bad = 1; continue; }
            name = argv[i];
            char b[1024]; int n; while ((n = sys_read(fd, b, sizeof b)) > 0) append(b, (u32)n);
            sys_close(fd);
        }
    }
    if (!tlen) return bad;
    if (!tty_out) { oputn(text, tlen); oflush(); return bad; }       /* not a terminal: as cat, every byte unchanged */
    layout();
    if (nrows <= (u32)(ROWS - 1)) {                                 /* it all fits: just print it */
        for (u32 r = 0; r < nrows; r++) { put_row(&rows[r]); oputc('\n'); }
        oflush();
        return bad;
    }
    tty_fd = sys_open("/dev/tty", O_RDONLY);
    zscr_init();
    if (tty_fd < 0) { dprintf(2, "less: no terminal\n"); return 1; }
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    signal(SIGINT, on_end); signal(SIGTERM, on_end); signal(SIGHUP, on_end);
    signal(SIGTSTP, on_tstp); signal(SIGCONT, on_cont);
    oputs("\033[2J");
    draw(0);
    for (;;) {
        int k = less_key(read_key());
        int page = ROWS - 1, maxtop = (int)nrows - 1;
        int old = top;
        switch (k) {
        case 'q': case 'Q': case 3: goto done;
        case ' ': case 'f': top += page; break;
        case 'b': top -= page; break;
        case '\r': case '\n': case 'j': case 'e': top++; break;
        case 'k': case 'y': top--; break;
        case 'd': top += page / 2; break;
        case 'u': top -= page / 2; break;
        case 'g': case '<': top = 0; break;
        case 'G': case '>': top = (int)nrows; break;
        case '/': search(1, 0); continue;
        case '?': search(-1, 0); continue;
        case 'n': search(1, 1); continue;
        case 'N': search(-1, 1); continue;
        case 'h': case 'H': oputs("\033[2J"); help(); oputs("\033[2J"); draw(0); continue;
        case 26: kill(0, SIGTSTP); continue;                       /* Ctrl+Z: the whole job stops (man too) */
        case K_RESIZE: {                                           /* a new size: the rows laid out again */
            u32 at = (u32)top < nrows ? rows[top].off : 0;
            if (ROWS < 3) ROWS = 3;
            layout();
            for (top = 0; (u32)top + 1 < nrows && rows[top + 1].off <= at; top++) ;
            oputs("\033[2J");
            break;
        }
        default: continue;
        }
        (void)maxtop; (void)old;
        if (top > (int)nrows - page) top = (int)nrows - page;      /* stop when the last line reaches the bottom */
        if (top < 0) top = 0;
        draw(0);
    }
done:
    signal(SIGTSTP, SIG_DFL); signal(SIGCONT, SIG_DFL);
    leave_screen();
    return bad;
}
