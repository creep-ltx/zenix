/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"
#include "regex.h"

/* edit - a text editor for the terminal, in the way of micro and nano.
 *   edit [+LINE] [file]
 * Ctrl+S save, F2 save as, Ctrl+Q quit, Ctrl+Z / Ctrl+Y undo / redo, Ctrl+C / Ctrl+X / Ctrl+V copy / cut / paste
 * (with nothing selected: the line), Ctrl+K cut line, Ctrl+D duplicate line, Ctrl+A select all, Shift+arrows select,
 * Ctrl+F find (regular expression, smart case), F3 / Ctrl+G next, Shift+F3 previous, Ctrl+R replace, Ctrl+L go to
 * line, Ctrl+Left/Right line start/end, Ctrl+Up/Down file start/end, Alt+Up/Down page, Alt+Left/Right word,
 * Tab / Shift+Tab indent / unindent, F4 line numbers, F1 help. */

static int text_rows = 23;              /* this terminal: its rows - 2 (libz's screen layer: the size, SIGWINCH) */
static int scr_cols = 80;               /* its columns (at most 255): the rows' cells, set with them (screen_alloc) */
#define SCR_COLS scr_cols

/* ------------------------------------------------------------ the buffer: an array of lines */
struct line { char *s; int len, cap; };
static struct line *L; static int nL, capL;
static char filename[ZPATH_MAX]; static int modified, final_newline = 1;
static int cy, cx, top, leftcol, show_nums;
static int sel_on, ay, ax;
static char *clip; static int clip_n;
static char message[100];

static void line_reserve(struct line *l, int n)
{
    if (n + 1 <= l->cap) return;
    int nc = l->cap ? l->cap * 2 : 32; while (nc < n + 1) nc *= 2;
    char *ns = realloc(l->s, (u32)nc); if (!ns) { dprintf(2, "edit: out of memory\n"); _zexit(1); }
    l->s = ns; l->cap = nc;
}
static void lines_reserve(int n)
{
    if (n <= capL) return;
    int nc = capL ? capL * 2 : 256; while (nc < n) nc *= 2;
    struct line *nl = realloc(L, (u32)nc * sizeof *nl); if (!nl) { dprintf(2, "edit: out of memory\n"); _zexit(1); }
    L = nl; capL = nc;
}
static void insert_line_at(int y)                       /* a new empty line before index y */
{
    lines_reserve(nL + 1);
    memmove(&L[y + 1], &L[y], (u32)(nL - y) * sizeof *L);
    memset(&L[y], 0, sizeof *L); nL++;
}
static void remove_lines(int y, int n)
{
    for (int k = y; k < y + n; k++) free(L[k].s);
    memmove(&L[y], &L[y + n], (u32)(nL - y - n) * sizeof *L); nL -= n;
}

/* insert text (may hold newlines) at (y,x); the end position goes to *ey, *ex */
static void buf_insert(int y, int x, const char *t, int n, int *ey, int *ex)
{
    struct line tail = { 0, 0, 0 };
    int start = 0, first = 1;
    for (int i = 0; i <= n; i++) {
        if (i < n && t[i] != '\n') continue;
        int seglen = i - start;
        if (first) {
            struct line *l = &L[y];
            int rest = l->len - x;
            if (i < n) { tail.s = xmalloc((u32)rest + 1); if (rest) memcpy(tail.s, l->s + x, (u32)rest); tail.len = tail.cap = rest; }
            line_reserve(l, x + seglen + (i < n ? 0 : rest));
            if (i == n) memmove(l->s + x + seglen, l->s + x, (u32)rest);
            memcpy(l->s + x, t + start, (u32)seglen);
            l->len = i < n ? x + seglen : l->len + seglen;
            *ey = y; *ex = x + seglen;
            first = 0;
        } else {
            insert_line_at(++y);
            struct line *l = &L[y];
            if (i == n) {                                   /* the last segment: followed by the old tail */
                line_reserve(l, seglen + tail.len);
                memcpy(l->s, t + start, (u32)seglen); memcpy(l->s + seglen, tail.s, (u32)tail.len);
                l->len = seglen + tail.len;
                *ey = y; *ex = seglen;
            } else { line_reserve(l, seglen); memcpy(l->s, t + start, (u32)seglen); l->len = seglen; }
        }
        start = i + 1;
    }
    free(tail.s);
}
/* remove [y1,x1) .. [y2,x2) and return the removed text (malloc'd) */
static char *buf_delete(int y1, int x1, int y2, int x2, int *outn)
{
    int total = 0;
    for (int y = y1; y <= y2; y++) total += (y == y2 ? x2 : L[y].len) - (y == y1 ? x1 : 0) + (y < y2 ? 1 : 0);
    char *out = xmalloc((u32)total + 1); int o = 0;
    for (int y = y1; y <= y2; y++) {
        int a = y == y1 ? x1 : 0, b = y == y2 ? x2 : L[y].len;
        memcpy(out + o, L[y].s + a, (u32)(b - a)); o += b - a;
        if (y < y2) out[o++] = '\n';
    }
    out[o] = 0; *outn = o;
    struct line *l1 = &L[y1];
    int suffix = L[y2].len - x2;
    line_reserve(l1, x1 + suffix);
    if (y2 == y1) memmove(l1->s + x1, l1->s + x2, (u32)suffix);
    else memcpy(l1->s + x1, L[y2].s + x2, (u32)suffix);
    l1->len = x1 + suffix;
    if (y2 > y1) remove_lines(y1 + 1, y2 - y1);
    return out;
}

/* ------------------------------------------------------------ undo */
struct undo { int kind, y, x, n, group, cy0, cx0; char *t; };       /* kind 0: text was inserted, 1: text was deleted */
static struct undo *U; static int nU, capU, redo_to;                /* U[0..nU) done; undone ones sit above nU up to redo_to */
static int group_id, last_typed_end_y = -1, last_typed_end_x;

static void note(int kind, int y, int x, const char *t, int n)
{
    if (nU == capU) { int nc = capU ? capU * 2 : 128; struct undo *nu = realloc(U, (u32)nc * sizeof *nu); if (!nu) { dprintf(2, "edit: out of memory\n"); _zexit(1); } U = nu; capU = nc; }
    redo_to = 0;
    struct undo *u = &U[nU++];
    u->kind = kind; u->y = y; u->x = x; u->n = n; u->group = group_id; u->cy0 = cy; u->cx0 = cx;
    u->t = xmalloc((u32)n + 1); memcpy(u->t, t, (u32)n); u->t[n] = 0;
}
static void end_pos(int y, int x, const char *t, int n, int *ey, int *ex)
{
    *ey = y; *ex = x;
    for (int i = 0; i < n; i++) { if (t[i] == '\n') { (*ey)++; *ex = 0; } else (*ex)++; }
}
static void do_insert(int y, int x, const char *t, int n, int *ey, int *ex) { buf_insert(y, x, t, n, ey, ex); note(0, y, x, t, n); modified = 1; }
static void do_delete(int y1, int x1, int y2, int x2) { int n; char *t = buf_delete(y1, x1, y2, x2, &n); note(1, y1, x1, t, n); free(t); modified = 1; }

static void undo_once(void)
{
    if (!nU) { snprintf(message, sizeof message, "nothing to undo"); return; }
    int g = U[nU - 1].group, before = nU;
    while (nU && U[nU - 1].group == g) {
        struct undo *u = &U[--nU]; int ey, ex;
        if (u->kind == 0) { end_pos(u->y, u->x, u->t, u->n, &ey, &ex); int n; free(buf_delete(u->y, u->x, ey, ex, &n)); cy = u->y; cx = u->x; }
        else { buf_insert(u->y, u->x, u->t, u->n, &ey, &ex); cy = ey; cx = ex; }
    }
    if (before > redo_to) redo_to = before;
    sel_on = 0; modified = 1;
}
static void redo_once(void)
{
    if (nU >= redo_to) { snprintf(message, sizeof message, "nothing to redo"); return; }
    int g = U[nU].group;
    while (nU < redo_to && U[nU].group == g) {
        struct undo *u = &U[nU++]; int ey, ex;
        if (u->kind == 0) { buf_insert(u->y, u->x, u->t, u->n, &ey, &ex); cy = ey; cx = ex; }
        else { end_pos(u->y, u->x, u->t, u->n, &ey, &ex); int n; free(buf_delete(u->y, u->x, ey, ex, &n)); cy = u->y; cx = u->x; }
    }
    sel_on = 0; modified = 1;
}

/* ------------------------------------------------------------ geometry: columns on the screen */
/* DESIGN D28: the text is UTF-8 and the cursor always at a character's start; a character takes the console's
 * columns (libz's zu8*: a letter one, a wide one two, a zero-width one none; a byte that is not UTF-8 one, as the
 * console shows it), a tab to the next multiple of 8, a control one ('?') */
static int chw(int y, int i, int col, int *n)           /* the columns of the character at byte i (at column col), *n its bytes */
{
    unsigned char c = (unsigned char)L[y].s[i];
    *n = 1;
    if (c == '\t') return (col / 8 + 1) * 8 - col;
    if (c < 0x80) return 1;
    int w; *n = zu8next(L[y].s + i, (u32)(L[y].len - i), &w);
    return w;
}
static int u8n(int y, int x) { int w; return zu8next(L[y].s + x, (u32)(L[y].len - x), &w); }    /* bytes of the character at x */
static int u8p(int y, int x) { return (int)zu8prev(L[y].s, (u32)x); }                          /* the character before x */
static int vcol(int y, int x)                           /* visual column of byte x on line y */
{
    int c = 0, n;
    for (int i = 0; i < x && i < L[y].len; i += n) c += chw(y, i, c, &n);
    return c;
}
static int byte_at_vcol(int y, int col)                 /* the character whose cells hold visual column col (or the line end) */
{
    int c = 0, n;
    for (int i = 0; i < L[y].len; i += n) { int w = chw(y, i, c, &n); if (col < c + w) return i; c += w; }
    return L[y].len;
}
static int gutter(void) { if (!show_nums) return 0; int w = 1, n = nL; while (n >= 10) { w++; n /= 10; } return w + 1; }

/* ------------------------------------------------------------ selection and clipboard */
static void sel_range(int *y1, int *x1, int *y2, int *x2)
{
    if (ay < cy || (ay == cy && ax <= cx)) { *y1 = ay; *x1 = ax; *y2 = cy; *x2 = cx; } else { *y1 = cy; *x1 = cx; *y2 = ay; *x2 = ax; }
}
static int has_sel(void) { return sel_on && !(ay == cy && ax == cx); }
static char *get_sel(int *n)
{
    int y1, x1, y2, x2; sel_range(&y1, &x1, &y2, &x2);
    int total = 0; for (int y = y1; y <= y2; y++) total += (y == y2 ? x2 : L[y].len) - (y == y1 ? x1 : 0) + (y < y2 ? 1 : 0);
    char *out = xmalloc((u32)total + 1); int o = 0;
    for (int y = y1; y <= y2; y++) { int a = y == y1 ? x1 : 0, b = y == y2 ? x2 : L[y].len; memcpy(out + o, L[y].s + a, (u32)(b - a)); o += b - a; if (y < y2) out[o++] = '\n'; }
    out[o] = 0; *n = o; return out;
}
static void delete_selection(void)
{
    if (!has_sel()) { sel_on = 0; return; }
    int y1, x1, y2, x2; sel_range(&y1, &x1, &y2, &x2);
    do_delete(y1, x1, y2, x2); cy = y1; cx = x1; sel_on = 0;
}
static int has_newline(const char *t, int n) { for (int i = 0; i < n; i++) if (t[i] == '\n') return 1; return 0; }
static void set_clip(const char *t, int n) { free(clip); clip = xmalloc((u32)n + 1); memcpy(clip, t, (u32)n); clip[n] = 0; clip_n = n; }

/* ------------------------------------------------------------ editing commands */
static void clamp_cursor(void)
{
    if (cy < 0) cy = 0;
    if (cy >= nL) cy = nL - 1;
    if (cx < 0) cx = 0;
    if (cx > L[cy].len) cx = L[cy].len;
}
static void type_text(const char *t, int n)
{
    int merge = (n == 1 && t[0] != '\n' && last_typed_end_y == cy && last_typed_end_x == cx && !has_sel() && nU && U[nU - 1].kind == 0 && U[nU - 1].group == group_id);
    if (!merge) group_id++;
    if (has_sel()) delete_selection();
    int ey, ex; do_insert(cy, cx, t, n, &ey, &ex);
    cy = ey; cx = ex; last_typed_end_y = cy; last_typed_end_x = cx; sel_on = 0;
}
static void new_line_indent(void)
{
    int ind = 0; while (ind < L[cy].len && ind < cx && (L[cy].s[ind] == ' ' || L[cy].s[ind] == '\t')) ind++;
    char b[130]; int n = 0; b[n++] = '\n';
    for (int i = 0; i < ind && n < 128; i++) b[n++] = L[cy].s[i];
    group_id++; last_typed_end_y = -1;
    if (has_sel()) delete_selection();
    int ey, ex; do_insert(cy, cx, b, n, &ey, &ex); cy = ey; cx = ex; sel_on = 0;
}
static void backspace(void)
{
    group_id++; last_typed_end_y = -1;
    if (has_sel()) { delete_selection(); return; }
    if (cx > 0) { int p = u8p(cy, cx); do_delete(cy, p, cy, cx); cx = p; }      /* one character (D28) */
    else if (cy > 0) { int px = L[cy - 1].len; do_delete(cy - 1, px, cy, 0); cy--; cx = px; }
}
static void delete_forward(void)
{
    group_id++; last_typed_end_y = -1;
    if (has_sel()) { delete_selection(); return; }
    if (cx < L[cy].len) do_delete(cy, cx, cy, cx + u8n(cy, cx));
    else if (cy + 1 < nL) do_delete(cy, cx, cy + 1, 0);
}
static void cut_line(void)
{
    group_id++; last_typed_end_y = -1;
    char *t = xmalloc((u32)L[cy].len + 2); memcpy(t, L[cy].s, (u32)L[cy].len); t[L[cy].len] = '\n';
    set_clip(t, L[cy].len + 1); free(t);
    if (cy + 1 < nL) do_delete(cy, 0, cy + 1, 0);
    else if (cy > 0) { int py = cy - 1; do_delete(py, L[py].len, cy, L[cy].len); cy = py; }
    else do_delete(0, 0, 0, L[0].len);
    sel_on = 0; cx = 0; clamp_cursor();
}
static void copy_cmd(void)
{
    if (has_sel()) { int n; char *t = get_sel(&n); set_clip(t, n); free(t); snprintf(message, sizeof message, "copied"); }
    else { char *t = xmalloc((u32)L[cy].len + 2); memcpy(t, L[cy].s, (u32)L[cy].len); t[L[cy].len] = '\n'; set_clip(t, L[cy].len + 1); free(t); snprintf(message, sizeof message, "line copied"); }
}
static void cut_cmd(void)
{
    if (has_sel()) { int n; char *t = get_sel(&n); set_clip(t, n); free(t); group_id++; delete_selection(); }
    else cut_line();
}
static void paste_cmd(void)
{
    if (!clip_n) { snprintf(message, sizeof message, "the clipboard is empty"); return; }
    group_id++; last_typed_end_y = -1;
    if (has_sel()) delete_selection();
    int line_paste = clip_n && clip[clip_n - 1] == '\n' && !has_newline(clip, clip_n - 1) ? 1 : 0;
    int ey, ex;
    if (line_paste) { do_insert(cy, 0, clip, clip_n, &ey, &ex); cy = ey; cx = cx > L[cy].len ? L[cy].len : cx; }
    else { do_insert(cy, cx, clip, clip_n, &ey, &ex); cy = ey; cx = ex; }
}
static void dup_line(void)
{
    group_id++; last_typed_end_y = -1;
    char *t = xmalloc((u32)L[cy].len + 2); t[0] = '\n'; memcpy(t + 1, L[cy].s, (u32)L[cy].len);
    int ey, ex; do_insert(cy, L[cy].len, t, L[cy].len + 1, &ey, &ex); cy = ey; free(t);
}
static void indent(int dir)
{
    int y1 = cy, y2 = cy;
    if (has_sel()) { int x1, x2; sel_range(&y1, &x1, &y2, &x2); if (x2 == 0 && y2 > y1) y2--; }
    int gone = 0;                                       /* characters taken from the cursor's line (no selection) */
    group_id++; last_typed_end_y = -1;
    for (int y = y1; y <= y2; y++) {
        if (dir > 0) { int ey, ex; do_insert(y, 0, "\t", 1, &ey, &ex); }
        else if (L[y].len && L[y].s[0] == '\t') { do_delete(y, 0, y, 1); gone = 1; }
        else { int k = 0; while (k < 4 && k < L[y].len && L[y].s[k] == ' ') k++; if (k) do_delete(y, 0, y, k); gone = k; }
    }
    if (!has_sel()) { cx += dir > 0 ? 1 : -(cx < gone ? cx : gone); clamp_cursor(); }
}

/* ------------------------------------------------------------ files */
static int load_file(const char *path)
{
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct rd r; rd_init(&r, fd);
    nL = 0; char *line = 0; u32 cap = 0; int len; int last_had_nl = 1;
    while ((len = rd_getline(&r, &line, &cap)) >= 0) {
        lines_reserve(nL + 1); memset(&L[nL], 0, sizeof L[nL]);
        line_reserve(&L[nL], len); memcpy(L[nL].s, line, (u32)len); L[nL].len = len; nL++;
        last_had_nl = r.pos > 0 || !r.eof ? 1 : 1;
    }
    free(line); sys_close(fd);
    (void)last_had_nl;
    final_newline = 1;
    if (!nL) { lines_reserve(1); memset(&L[0], 0, sizeof L[0]); nL = 1; }
    return 0;
}
static int save_file(const char *path)
{
    char tmp[140]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = sys_open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    struct ustat st; int had = sys_stat(path, &st) == 0;
    for (int y = 0; y < nL; y++) {
        if (L[y].len && sys_write(fd, L[y].s, (u32)L[y].len) != L[y].len) { sys_close(fd); return -1; }
        if ((y + 1 < nL || final_newline) && sys_write(fd, "\n", 1) != 1) { sys_close(fd); return -1; }
    }
    sys_close(fd);
    if (had) sys_chmod(tmp, st.mode & 07777);
    if (sys_rename(tmp, path) < 0) return -1;                /* the new copy replaces the old one only when complete */
    modified = 0;
    return 0;
}

/* ------------------------------------------------------------ the screen: only changed rows are sent */
/* a cell: its character's bytes packed into a long, the first in the top byte (D28: a UTF-8 character is up to 4); 0
 * the right half of a wide character (nothing is sent for it) */
struct cell_row { u32 *c; unsigned char *inv; };
static struct cell_row *shadow, cur_row;     /* the rows as the terminal shows them, and the one being made */
static int use_color;                   /* the status line in colour (inv 2) */
static int shadow_valid;

/* the rows for the terminal's size now (at the start and after a resize): text_rows + 2 and one to make, each
 * SCR_COLS cells, in one block; 0, or -1 when there is no memory */
static int screen_alloc(void)
{
    static void *blk;
    text_rows = zscr_rows - 2;
    if (text_rows < 3) text_rows = 3;
    scr_cols = zscr_cols;
    int n = text_rows + 3;
    u32 one = (u32)SCR_COLS * 5;
    void *b = malloc(n * sizeof *shadow + n * one);
    if (!b) return -1;
    free(blk);
    blk = b;
    shadow = b;
    unsigned char *p = (unsigned char *)(shadow + n);
    for (int i = 0; i < n; i++, p += one) {
        shadow[i].c = (u32 *)p;
        shadow[i].inv = p + (u32)SCR_COLS * 4;
    }
    cur_row = shadow[n - 1];
    shadow_valid = 0;
    return 0;
}
static void flush_row(int r, const struct cell_row *want)
{
    if (shadow_valid && !memcmp(shadow[r].c, want->c, (u32)SCR_COLS * 4) && !memcmp(shadow[r].inv, want->inv, (u32)SCR_COLS))
        return;
    char pos[16]; snprintf(pos, sizeof pos, "\033[%d;1H", r + 1); oputs(pos);
    int cur = 0;
    for (int i = 0; i < SCR_COLS; i++) {
        if (!want->c[i]) continue;
        if (want->inv[i] != cur) { oputs(want->inv[i] == 2 ? "\033[0;7;34;107m" : want->inv[i] ? "\033[0;7m" : "\033[0m"); cur = want->inv[i]; }
        for (u32 v = want->c[i]; v; v <<= 8) oputc((unsigned char)(v >> 24));
    }
    if (cur) oputs("\033[0m");
    memcpy(shadow[r].c, want->c, (u32)SCR_COLS * 4);
    memcpy(shadow[r].inv, want->inv, (u32)SCR_COLS);
}
static u32 pack(const char *s, int n)                  /* a character's n bytes as a cell */
{
    u32 v = 0;
    for (int i = 0; i < 4; i++) v = v << 8 | (i < n ? (unsigned char)s[i] : 0);
    return v;
}
/* the text into cells from col on, by characters (a wide one two cells; one that does not fit is a space) */
static void put_str(struct cell_row *row, int col, const char *s, int n, int inv)
{
    for (int i = 0, k, w; i < n && col < SCR_COLS; i += k) {
        k = zu8next(s + i, (u32)(n - i), &w);
        if (!w) continue;
        row->inv[col] = (unsigned char)inv;
        if (w == 2 && col + 1 >= SCR_COLS) { row->c[col++] = ' ' << 24; break; }
        row->c[col++] = (unsigned char)s[i] < 32 ? '?' << 24 : pack(s + i, k);
        if (w == 2) { row->inv[col] = (unsigned char)inv; row->c[col++] = 0; }
    }
}
static void blank_row(struct cell_row *row, int inv) { for (int i = 0; i < SCR_COLS; i++) row->c[i] = ' ' << 24; memset(row->inv, inv, SCR_COLS); }

static int in_selection(int y, int x)
{
    if (!has_sel()) return 0;
    int y1, x1, y2, x2; sel_range(&y1, &x1, &y2, &x2);
    if (y < y1 || y > y2) return 0;
    if (y == y1 && x < x1) return 0;
    if (y == y2 && x >= x2) return 0;
    return 1;
}
static void render(void)
{
    int g = gutter(), width = SCR_COLS - g;
    clamp_cursor();
    int cc = vcol(cy, cx);
    if (cy < top) top = cy;
    if (cy >= top + text_rows) top = cy - text_rows + 1;
    if (cc < leftcol) leftcol = cc;
    if (cc >= leftcol + width) leftcol = cc - width + 1;
    for (int r = 0; r < text_rows; r++) {
        blank_row(&cur_row, 0);
        int y = top + r;
        if (y < nL) {
            if (g) { char nb[12]; snprintf(nb, sizeof nb, "%*d ", g - 1, y + 1); put_str(&cur_row, 0, nb, g, 0); }
            int col = 0, n;
            for (int i = 0; i <= L[y].len; i += n) {
                int w = i == L[y].len ? (n = 1) : chw(y, i, col, &n);
                int sel = in_selection(y, i) && i < L[y].len;
                unsigned char c0 = i < L[y].len ? (unsigned char)L[y].s[i] : ' ';
                u32 cell = i == L[y].len || c0 == '\t' ? ' ' << 24 : c0 < 32 ? '?' << 24 : pack(L[y].s + i, n);
                for (int k = 0; k < w; k++) {
                    int sc = col + k - leftcol;
                    if (sc < 0 || sc >= width) continue;
                    u32 v = cell;
                    if (w == 2 && cell != (' ' << 24))      /* a wide character: its left cell, then the half (0) */
                        v = k ? 0 : sc + 1 >= width ? ' ' << 24 : cell;     /* cut by the edge: a space */
                    if (w == 2 && k == 1 && sc == 0) v = ' ' << 24;           /* its left half scrolled off */
                    cur_row.c[g + sc] = v; cur_row.inv[g + sc] = (unsigned char)sel;
                }
                col += w;
            }
        }
        flush_row(r, &cur_row);
    }
    /* status line: white on blue where colour is wanted (libz color_auto), else inverse; the blue is written as an
     * inverse of blue on white, so a one-plane display, which shows no colours, still shows it inverse */
    int sl = use_color ? 2 : 1;
    blank_row(&cur_row, sl);
    char st[256];
    int n = snprintf(st, sizeof st, " %s%s", filename[0] ? filename : "(no name)", modified ? " [modified]" : "");
    put_str(&cur_row, 0, st, n, sl);
    n = snprintf(st, sizeof st, "line %d/%d  col %d  Ctrl+S save  Ctrl+Q quit  F1 help ", cy + 1, nL, cc + 1);
    put_str(&cur_row, SCR_COLS - n > 0 ? SCR_COLS - n : 0, st, n, sl);
    flush_row(text_rows, &cur_row);
    /* message line */
    blank_row(&cur_row, 0);
    if (message[0]) put_str(&cur_row, 1, message, (int)strlen(message), 0);
    flush_row(text_rows + 1, &cur_row);
    shadow_valid = 1;
    char pos[16]; snprintf(pos, sizeof pos, "\033[%d;%dH", cy - top + 1, g + cc - leftcol + 1);
    oputs(pos);
    oflush();
}
static void invalidate(void) { shadow_valid = 0; oputs("\033[2J"); }

/* ------------------------------------------------------------ keys */
enum { K_NONE = 0, K_UP = 256, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END, K_DEL, K_INS, K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_BTAB, K_TEXT };
static char kbuf[4]; static int kn;     /* K_TEXT (D28): a UTF-8 character's bytes, as the keyboard sends them */
static unsigned char pq[4]; static int pqn;     /* bytes read ahead that were not part of a character */
#define M_SHIFT 0x1000
#define M_ALT   0x2000
#define M_CTRL  0x4000
static int resized;                     /* a new terminal size came while a key was awaited (SIGWINCH) */
static int midkey;                      /* within a key's bytes: a resize waits for its end */
static int getb(void)
{
    unsigned char c;
    if (pqn) { c = pq[0]; memmove(pq, pq + 1, (u32)--pqn); return c; }
    for (;;) {                          /* the wait notices a resize (libz's screen layer) */
        struct pollfd p = { 0, POLLIN, 0 };
        if (zscr_resized()) resized = 1;
        if (resized && !pqn && !midkey) return -2;
        if (poll(&p, 1, 500) > 0) break;
    }
    return sys_read(0, &c, 1) == 1 ? c : -1;
}
#define K_RESIZE 0x0FFF
static int read_key(void)
{
    midkey = 0;
    int c = getb();
    if (c == -2) { resized = 0; return K_RESIZE; }
    if (c < 0) return -1;
    midkey = 1;
    if (c >= 0xC2 && c <= 0xF4) {                       /* D28: a lead: its continuations come with it */
        int need = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4, w;
        int other = -1;
        kbuf[0] = (char)c; kn = 1;
        while (kn < need) {
            int b = getb();
            if (b < 0) break;
            if ((b & 0xC0) != 0x80) { other = b; break; }
            kbuf[kn++] = (char)b;
        }
        if (zu8next(kbuf, (u32)kn, &w) == kn) {
            if (other >= 0) pq[pqn++] = (unsigned char)other;
            return K_TEXT;
        }
        for (int i = 1; i < kn; i++) pq[pqn++] = (unsigned char)kbuf[i];      /* not a character: byte by byte */
        if (other >= 0) pq[pqn++] = (unsigned char)other;
        kn = 1;
        return c;
    }
    if (c != 27) return c;
    int s1 = getb();
    if (s1 == 'O') { int f = getb(); return f >= 'P' && f <= 'S' ? K_F1 + (f - 'P') : K_NONE; }
    if (s1 != '[') return K_NONE;
    int num = 0, num2 = 0, semi = 0, ch;
    while ((ch = getb()) >= 0 && ((ch >= '0' && ch <= '9') || ch == ';')) { if (ch == ';') semi = 1; else if (semi) num2 = num2 * 10 + (ch - '0'); else num = num * 10 + (ch - '0'); }
    int mods = 0;
    if (semi && num2 > 1) { int m = num2 - 1; if (m & 1) mods |= M_SHIFT; if (m & 2) mods |= M_ALT; if (m & 4) mods |= M_CTRL; }
    int key = K_NONE;
    if (ch == 'A') key = K_UP; else if (ch == 'B') key = K_DOWN; else if (ch == 'C') key = K_RIGHT; else if (ch == 'D') key = K_LEFT;
    else if (ch == 'H') key = K_HOME; else if (ch == 'F') key = K_END;
    else if (ch == 'Z') key = K_BTAB;                                     /* Shift+Tab: ESC [ Z */
    else if (ch == 'P' || ch == 'Q' || ch == 'R' || ch == 'S') key = K_F1 + (ch - 'P');
    else if (ch == '~') {
        if (num == 3) key = K_DEL; else if (num == 2) key = K_INS; else if (num == 1) key = K_HOME; else if (num == 4) key = K_END;
        else if (num == 15) key = K_F5; else if (num == 17) key = K_F6; else if (num == 18) key = K_F7; else if (num == 19) key = K_F8; else if (num == 20) key = K_F9; else if (num == 21) key = K_F10;
    }
    return key ? key | mods : K_NONE;
}

/* a one-line prompt on the message row (libz's line editor: the keys every typed line has); returns 1 with text in
 * out, 0 when cancelled */
static int prompt(const char *label, char *out, int max, const char *initial)
{
    struct zled e;
    zled_init(&e, out, max, initial ? initial : "");
    for (;;) {
        zled_show(&e, text_rows + 2, 1, SCR_COLS - 1, label);
        oflush();
        shadow_valid = 0;
        int c = read_key(), k = c & 0xFFF, r;
        if (c < 0) return 0;
        if (k == K_TEXT) { zled_insert(&e, kbuf, kn); continue; }
        k = k == K_LEFT ? ZK_LEFT : k == K_RIGHT ? ZK_RIGHT : k == K_HOME ? 1 : k == K_END ? 5 : k == K_DEL ? 4 : k;
        if ((c & M_CTRL) && (k == ZK_LEFT || k == ZK_RIGHT)) k = k == ZK_LEFT ? ZK_CLEFT : ZK_CRIGHT;
        if (k == K_NONE || k == K_RESIZE || (k >= 256 && k < ZK_UP)) continue;
        r = zled_key(&e, k);
        if (r == ZLED_DONE) return 1;
        if (r == ZLED_CANCEL) return 0;
    }
}
static int ask(const char *q, const char *keys)         /* one key from keys, or 0 */
{
    char pos[24]; snprintf(pos, sizeof pos, "\033[%d;1H", text_rows + 2); oputs(pos); oputs("\033[K"); oputs(q); oflush();
    shadow_valid = 0;
    for (;;) { int c = read_key(); if (c < 0) return 0; if (c == 27 || c == 3) return 0; if (c < 256 && strchr(keys, tolower(c))) return tolower(c); }
}

/* ------------------------------------------------------------ search and replace */
static void *rx; static char pattern[80]; static int last_dir = 1;
static int compile_pattern(void)
{
    int icase = 1; for (const char *p = pattern; *p; p++) if (isupper((unsigned char)*p)) icase = 0;        /* smart case */
    if (rx) re_free(rx);
    rx = re_comp(pattern, (icase ? RE_ICASE : 0) | RE_LONGEST);
    return rx != 0;
}
/* find the next match from the cursor in direction dir; selects it. Returns 1 if found. */
static int find_match(int dir)
{
    if (!rx) return 0;
    int caps[2 * RE_GROUPS];
    for (int pass = 0; pass < nL; pass++) {
        int y = ((cy + dir * pass) % nL + nL) % nL;
        int from = pass == 0 ? (dir > 0 ? (has_sel() ? (cx > ax ? cx : ax) : cx) : 0) : 0;
        if (dir > 0) {
            if (pass == 0 && has_sel()) { int y1, x1, y2, x2; sel_range(&y1, &x1, &y2, &x2); from = x2; }
            if (re_exec(rx, L[y].s, L[y].len, from, caps)) { ay = y; ax = caps[0]; cy = y; cx = caps[1]; sel_on = 1; return 1; }
        } else {
            int limit = pass == 0 ? (has_sel() ? (cx < ax ? cx : ax) : cx) : L[y].len + 1, best_s = -1, best_e = -1, f = 0;
            while (f <= L[y].len && re_exec(rx, L[y].s, L[y].len, f, caps)) { if (caps[0] >= limit) break; best_s = caps[0]; best_e = caps[1]; f = caps[0] + 1; }
            if (best_s >= 0) { ay = y; ax = best_e; cy = y; cx = best_s; sel_on = 1; return 1; }
        }
    }
    return 0;
}
static void find_cmd(int again, int dir)
{
    if (!again) {
        char in[80]; int has = has_sel() ? 0 : 0; (void)has;
        if (!prompt("find: ", in, sizeof in, pattern[0] ? pattern : 0) || !in[0]) return;
        strcpy(pattern, in);
        if (!compile_pattern()) { snprintf(message, sizeof message, "bad pattern: %s", pattern); return; }
        last_dir = dir;
    } else {
        if (!pattern[0]) { snprintf(message, sizeof message, "no search pattern (Ctrl+F)"); return; }
        dir = dir * last_dir;
    }
    if (!find_match(dir)) snprintf(message, sizeof message, "not found: %s", pattern);
}
static void replace_cmd(void)
{
    char find[80], with[80];
    if (!prompt("replace: ", find, sizeof find, pattern[0] ? pattern : 0) || !find[0]) return;
    if (!prompt("with: ", with, sizeof with, 0)) return;
    strcpy(pattern, find);
    if (!compile_pattern()) { snprintf(message, sizeof message, "bad pattern: %s", pattern); return; }
    int all = 0, count = 0; sel_on = 0; cy = cy; 
    group_id++; last_typed_end_y = -1;
    while (find_match(1)) {
        int ans = 'y';
        if (!all) { render(); ans = ask("replace? (y)es (n)o (a)ll (q)uit ", "ynaq"); if (!ans || ans == 'q') break; if (ans == 'a') { all = 1; ans = 'y'; } }
        if (ans == 'y') {
            int y1, x1, y2, x2; sel_range(&y1, &x1, &y2, &x2);
            do_delete(y1, x1, y2, x2); int ey, ex; do_insert(y1, x1, with, (int)strlen(with), &ey, &ex);
            cy = ey; cx = ex; sel_on = 0; count++;
            if (!*with && x2 == x1) cx += cx < L[cy].len ? u8n(cy, cx) : 1;    /* an empty match: step on */
        } else { sel_on = 0; }
    }
    sel_on = 0;
    snprintf(message, sizeof message, "%d replaced", count);
}

/* ------------------------------------------------------------ movement */
static void move_to(int y, int x, int extend)
{
    if (extend) { if (!sel_on) { sel_on = 1; ay = cy; ax = cx; } } else sel_on = 0;
    cy = y; cx = x; clamp_cursor();
}
static int is_word(int c) { return isalnum(c) || c == '_'; }
static void help_screen(void)
{
    oputs("\033[2J\033[H");
    oputs("  edit - keys\r\n\r\n"
          "  Ctrl+S save          F2 save as           Ctrl+Q quit          F1 this help\r\n"
          "  Ctrl+Z undo          Ctrl+Y redo\r\n"
          "  Ctrl+C copy          Ctrl+X cut           Ctrl+V paste         (nothing selected: the line)\r\n"
          "  Ctrl+K cut line      Ctrl+D duplicate     Ctrl+A select all    Shift+arrows select\r\n"
          "  Ctrl+F find          F3 next  Shift+F3 previous  Ctrl+G next   Ctrl+R replace\r\n"
          "  Ctrl+L go to line    F4 line numbers\r\n"
          "  Ctrl+Left/Right  line start / end        Ctrl+Up/Down  file start / end\r\n"
          "  Alt+Left/Right   word back / on          Alt+Up/Down   page up / down\r\n"
          "  Tab / Shift+Tab  indent / unindent (selected lines too)\r\n"
          "  Find uses regular expressions; a capital letter in the pattern makes it case sensitive.\r\n\r\n"
          "  (any key to go back)");
    oflush(); read_key(); invalidate();
}

/* ------------------------------------------------------------ main */
static int quit_ok(void)
{
    if (!modified) return 1;
    int a = ask("unsaved changes: save before quitting? (y)es (n)o (c)ancel ", "ync");
    if (a == 'y') {
        if (!filename[0]) { char in[ZPATH_MAX]; if (!prompt("save as: ", in, sizeof in, 0) || !in[0]) return 0; strcpy(filename, in); }
        if (save_file(filename) < 0) { snprintf(message, sizeof message, "cannot save %s", filename); return 0; }
        return 1;
    }
    return a == 'n';
}

/* signals (DESIGN D16): the terminal is left as it was found whatever ends or stops the editor. SIGINT, SIGTERM,
 * SIGHUP: cooked mode back, the screen cleared, then the end (128 + the signal; unsaved changes are lost, as in nano
 * without its backup file). SIGTSTP (a kill: Ctrl+Z is undo here): the same, then it stops; SIGCONT: raw mode again and
 * the screen drawn anew. */
static void leave_screen(void) { sys_ttymode(0); oputs("\033[2J\033[H"); oflush(); }
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
    oputs("\033[2J\033[H");
    shadow_valid = 0;
    render();
}
static const char USAGE[] = "usage: edit [+LINE] [file]\n";
int main(int argc, char **argv)
{
    zopt(argc, argv, USAGE);
    int goto_line = 0, i = 1;
    zscr_init();                                                /* the terminal's size, and its changes */
    if (screen_alloc() < 0) { dprintf(2, "edit: out of memory\n"); return 1; }
    for (; i < argc; i++) {
        if (argv[i][0] == '+' && isdigit((unsigned char)argv[i][1])) goto_line = atoi(argv[i] + 1);
        else if (argv[i][0] == '-' && argv[i][1]) zbadopt(argv[i], USAGE);
        else if (!filename[0]) { if (strlen(argv[i]) >= sizeof filename) { dprintf(2, "edit: %s: file name too long\n", argv[i]); return 1; } strcpy(filename, argv[i]); }
    }
    use_color = color_auto(1);
    lines_reserve(1); memset(&L[0], 0, sizeof L[0]); nL = 1;
    if (filename[0]) {
        struct ustat st;
        if (sys_stat(filename, &st) == 0) { if ((st.mode & S_IFMT) == S_IFDIR) { dprintf(2, "edit: %s is a directory\n", filename); return 1; } if (load_file(filename) < 0) { dprintf(2, "edit: cannot read %s\n", filename); return 1; } }
        else snprintf(message, sizeof message, "new file %s", filename);
    }
    if (goto_line > 0) { cy = goto_line - 1; clamp_cursor(); }
    if (!sys_isatty(0)) { dprintf(2, "edit: standard input is not a terminal\n"); return 1; }
    sys_ttymode(TTY_RAW | TTY_NOSIG);
    signal(SIGINT, on_end); signal(SIGTERM, on_end); signal(SIGHUP, on_end);
    signal(SIGTSTP, on_tstp); signal(SIGCONT, on_cont);
    oputs("\033[2J\033[H"); shadow_valid = 0;
    int want = -1;                                              /* the visual column to return to when moving up and down */
    for (;;) {
        if (scr_cols != zscr_cols || text_rows != (zscr_rows - 2 < 3 ? 3 : zscr_rows - 2)) {
            if (screen_alloc() < 0) break;                      /* a new size: the rows made again, all drawn anew */
            invalidate();
        }
        render();
        int k = read_key();
        if (k < 0) break;
        if (k == K_RESIZE) continue;
        int plain = k & 0xFFF, mods = k & 0x7000, extend = (mods & M_SHIFT) != 0;
        message[0] = 0;
        int vertical = 0;
        if (plain == K_UP || plain == K_DOWN) vertical = 1;
        if (k >= 256) {
            switch (plain) {
            case K_UP:
                if (mods & M_CTRL) move_to(0, 0, extend);
                else if (mods & M_ALT) { int y = cy - (text_rows - 1); move_to(y < 0 ? 0 : y, cx, extend); }
                else { if (want < 0) want = vcol(cy, cx); int y = cy > 0 ? cy - 1 : 0; move_to(y, byte_at_vcol(y, want), extend); }
                break;
            case K_DOWN:
                if (mods & M_CTRL) move_to(nL - 1, L[nL - 1].len, extend);
                else if (mods & M_ALT) { int y = cy + text_rows - 1; move_to(y >= nL ? nL - 1 : y, cx, extend); }
                else { if (want < 0) want = vcol(cy, cx); int y = cy + 1 < nL ? cy + 1 : cy; move_to(y, byte_at_vcol(y, want), extend); }
                break;
            case K_LEFT:
                if (mods & M_CTRL) move_to(cy, 0, extend);
                else if (mods & M_ALT) { int x = cx; while (x > 0 && !is_word((unsigned char)L[cy].s[x - 1])) x--; while (x > 0 && is_word((unsigned char)L[cy].s[x - 1])) x--; move_to(cy, x, extend); }
                else if (!extend && has_sel()) { int y1, x1, y2, x2; sel_range(&y1, &x1, &y2, &x2); move_to(y1, x1, 0); }
                else if (cx > 0) move_to(cy, u8p(cy, cx), extend); else if (cy > 0) move_to(cy - 1, L[cy - 1].len, extend);
                break;
            case K_RIGHT:
                if (mods & M_CTRL) move_to(cy, L[cy].len, extend);
                else if (mods & M_ALT) { int x = cx; while (x < L[cy].len && !is_word((unsigned char)L[cy].s[x])) x++; while (x < L[cy].len && is_word((unsigned char)L[cy].s[x])) x++; move_to(cy, x, extend); }
                else if (!extend && has_sel()) { int y1, x1, y2, x2; sel_range(&y1, &x1, &y2, &x2); move_to(y2, x2, 0); }
                else if (cx < L[cy].len) move_to(cy, cx + u8n(cy, cx), extend); else if (cy + 1 < nL) move_to(cy + 1, 0, extend);
                break;
            case K_HOME: move_to(cy, 0, extend); break;
            case K_END: move_to(cy, L[cy].len, extend); break;
            case K_DEL: delete_forward(); break;
            case K_BTAB: indent(-1); break;
            case K_F1: help_screen(); break;
            case K_F2: { char in[ZPATH_MAX]; if (prompt("save as: ", in, sizeof in, filename[0] ? filename : 0) && in[0]) { strcpy(filename, in); if (save_file(filename) < 0) snprintf(message, sizeof message, "cannot save %s", filename); else snprintf(message, sizeof message, "saved %s", filename); } break; }
            case K_F3: find_cmd(1, (mods & M_SHIFT) ? -1 : 1); break;
            case K_F4: show_nums = !show_nums; invalidate(); break;
            default: break;
            }
            if (!vertical) want = -1;
            continue;
        }
        want = -1;
        switch (k) {
        case 17: if (quit_ok()) goto done; break;                                            /* Ctrl+Q */
        case 19:                                                                             /* Ctrl+S */
            if (!filename[0]) { char in[ZPATH_MAX]; if (!prompt("save as: ", in, sizeof in, 0) || !in[0]) break; strcpy(filename, in); }
            if (save_file(filename) < 0) snprintf(message, sizeof message, "cannot save %s", filename); else snprintf(message, sizeof message, "saved %s (%d lines)", filename, nL);
            break;
        case 26: undo_once(); break;                                                         /* Ctrl+Z */
        case 25: redo_once(); break;                                                         /* Ctrl+Y */
        case 3: copy_cmd(); break;                                                           /* Ctrl+C */
        case 24: cut_cmd(); break;                                                           /* Ctrl+X */
        case 22: paste_cmd(); break;                                                         /* Ctrl+V */
        case 11: cut_line(); break;                                                          /* Ctrl+K */
        case 4: dup_line(); break;                                                           /* Ctrl+D */
        case 1: sel_on = 1; ay = 0; ax = 0; cy = nL - 1; cx = L[cy].len; break;             /* Ctrl+A */
        case 6: find_cmd(0, 1); break;                                                       /* Ctrl+F */
        case 7: find_cmd(1, 1); break;                                                       /* Ctrl+G */
        case 18: replace_cmd(); break;                                                       /* Ctrl+R */
        case 12: { char in[32]; if (prompt("go to line: ", in, sizeof in, 0) && in[0]) { int ln = atoi(in), col = 0; char *c = strchr(in, ':'); if (c) col = atoi(c + 1); int y = ln > 0 ? ln - 1 : 0; if (y >= nL) y = nL - 1; move_to(y, byte_at_vcol(y, col > 0 ? col - 1 : 0), 0); } break; }
        case 13: case 10: new_line_indent(); break;
        case 127: case 8: backspace(); break;
        case 9: if (has_sel()) indent(1); else { char t = '\t'; type_text(&t, 1); } break;
        case 27: sel_on = 0; break;
        case K_TEXT: type_text(kbuf, kn); break;                                             /* D28: a UTF-8 letter */
        default:
            if (k >= 32 && k < 256) { char ch = (char)k; type_text(&ch, 1); }
        }
    }
done:
    signal(SIGTSTP, SIG_DFL); signal(SIGCONT, SIG_DFL);
    leave_screen();
    return 0;
}
