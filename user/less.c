#include "libz.h"
#include "regex.h"
/* less [file...]   (also installed as more)
 * Keys: Space/f page down, b page up, Enter/j/Down line down, k/Up line up, d/u half page, g/Home top, G/End bottom,
 * / pattern search, ? backward search, n/N next/previous match, h help, q quit.
 * Short files are just printed. Text from a pipe works: the keys come from /dev/tty. */
static int ROWS = 25;                   /* the terminal's rows (main asks the kernel: 32 PAL, 25 NTSC) */
static char *text; static u32 tlen, tcap;
struct row { u32 off; unsigned short len; };
static struct row *rows; static u32 nrows, rcap;
static int tty_fd = -1, top;
static void *re; static const char *name = "(standard input)";

static void append(const char *b, u32 n)
{
    if (tlen + n + 1 > tcap) { u32 nc = tcap ? tcap * 2 : 8192; while (nc < tlen + n + 1) nc *= 2; char *nt = realloc(text, nc); if (!nt) { fprintf(2, "less: out of memory\n"); _zexit(1); } text = nt; tcap = nc; }
    memcpy(text + tlen, b, n); tlen += n;
}
static void add_row(u32 off, u32 len)
{
    if (nrows == rcap) { u32 nc = rcap ? rcap * 2 : 512; struct row *nr = realloc(rows, nc * sizeof *nr); if (!nr) { fprintf(2, "less: out of memory\n"); _zexit(1); } rows = nr; rcap = nc; }
    rows[nrows].off = off; rows[nrows].len = (unsigned short)len; nrows++;
}
/* split the text into screen rows: lines of up to 80 columns, tabs to the next multiple of 8 */
static void layout(void)
{
    nrows = 0;
    u32 i = 0;
    while (i < tlen) {
        u32 start = i, col = 0;
        while (i < tlen && text[i] != '\n') {
            u32 w = text[i] == '\t' ? 8 - (col % 8) : ((unsigned char)text[i] < 32 ? 2u : 1u);
            if (col + w > 80 && col > 0) { add_row(start, i - start); start = i; col = 0; }
            col += w; i++;
        }
        add_row(start, i - start);
        if (i < tlen) i++;                                  /* the newline */
    }
}
static void put_row(const struct row *r)
{
    u32 col = 0;
    for (u32 k = 0; k < r->len; k++) {
        unsigned char c = (unsigned char)text[r->off + k];
        if (c == '\t') { do { oputc(' '); col++; } while (col % 8); }
        else if (c < 32) { oputc('^'); oputc(c + 64); col += 2; }
        else { oputc(c); col++; }
    }
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
static int read_key(void)
{
    unsigned char c;
    if (sys_read(tty_fd, &c, 1) != 1) return 'q';
    if (c != 27) return c;
    unsigned char s1, s2 = 0;
    if (sys_read(tty_fd, &s1, 1) != 1) return 'q';
    if (s1 != '[' && s1 != 'O') return 0;
    int num = 0;
    while (sys_read(tty_fd, &s2, 1) == 1 && s2 >= '0' && s2 <= '9') num = num * 10 + (s2 - '0');
    if (s2 == 'A') return 'k';
    if (s2 == 'B') return 'j';
    if (s2 == 'H') return 'g';
    if (s2 == 'F') return 'G';
    if (s2 == '~') return num == 5 ? 'b' : num == 6 ? 'f' : num == 1 ? 'g' : num == 4 ? 'G' : 0;
    return 0;
}
static void prompt_line(const char *label, char *out, int max)
{
    int n = 0;
    oputs("\033[H"); for (int r = 0; r < ROWS - 1; r++) oputs("\033[B");
    oputs("\r\033[K"); oputs(label); oflush();
    for (;;) {
        unsigned char c;
        if (sys_read(tty_fd, &c, 1) != 1) break;
        if (c == '\r' || c == '\n') break;
        if (c == 27 || c == 3) { n = 0; break; }
        if ((c == 127 || c == 8) && n) { n--; oputs("\b \b"); }
        else if (c >= 32 && n < max - 1) { out[n++] = (char)c; oputc(c); }
        oflush();
    }
    out[n] = 0;
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
int main(int argc, char **argv)
{
    int i = 1;
    { int ti = sys_ttyinfo(); ROWS = ti > 0 ? ti >> 16 : 25; if (ROWS < 3) ROWS = 3; }    /* 32 on PAL, 25 on NTSC */
    while (i < argc && argv[i][0] == '-' && argv[i][1]) i++;         /* options are accepted and ignored */
    int tty_out = sys_isatty(1);
    if (i >= argc) {
        if (sys_isatty(0)) { fprintf(2, "usage: less file...   (or: command | less)\n"); return 1; }
        char b[1024]; int n; while ((n = sys_read(0, b, sizeof b)) > 0) append(b, (u32)n);
    } else {
        for (; i < argc; i++) {
            int fd = sys_open(argv[i], O_RDONLY);
            if (fd < 0) { fprintf(2, "less: %s: %s\n", argv[i], zwhy("cannot open")); continue; }
            name = argv[i];
            char b[1024]; int n; while ((n = sys_read(fd, b, sizeof b)) > 0) append(b, (u32)n);
            sys_close(fd);
        }
    }
    if (!tlen) return 0;
    layout();
    if (!tty_out || nrows <= (u32)(ROWS - 1)) {                    /* not a terminal, or it all fits: just print it */
        for (u32 r = 0; r < nrows; r++) { put_row(&rows[r]); oputc('\n'); }
        oflush();
        return 0;
    }
    tty_fd = sys_open("/dev/tty", O_RDONLY);
    if (tty_fd < 0) { fprintf(2, "less: no terminal\n"); return 1; }
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    oputs("\033[2J");
    draw(0);
    for (;;) {
        int k = read_key();
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
        default: continue;
        }
        (void)maxtop; (void)old;
        if (top > (int)nrows - page) top = (int)nrows - page;      /* stop when the last line reaches the bottom */
        if (top < 0) top = 0;
        draw(0);
    }
done:
    sys_ttymode(0);
    oputs("\033[H"); for (int r = 0; r < ROWS - 1; r++) oputs("\033[B");
    oputs("\r\033[K");
    oflush();
    return 0;
}
