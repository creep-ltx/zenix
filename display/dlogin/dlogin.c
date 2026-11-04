/* SPDX-License-Identifier: GPL-2.0-only */
/* dlogin, the graphical login (DESIGN D38 revision 3; docs/man/dlogin.txt). Started by init on console 1 instead of
 * the text login when /etc/console says "login graphical". It opens the display (displayd starts and takes console
 * 1), becomes its window manager and shows the greeter in zdesk's theme: the computer's name, the clock, Name,
 * Password (never shown) and Session (/etc/dlogin's list: the desktop, a text console). The password is checked by
 * the code login uses (pw_auth: the same rules and the same wait after a wrong one). A right one starts the session
 * through `login -f -s PROGRAM NAME`, so login's own code sets it up (environment, groups, uid, home) and the session
 * never runs as root; root is refused here (a text console is root's). During the session dlogin stays connected,
 * so displayd stays up; it has given the window manager's role away and has named the user to the server
 * (DPY_SESSION), so only that user's programs (and root's) may use the screen. When the session's program ends,
 * DPY_SESSION back to root drops whatever of the user's is still connected and the greeter comes back. A text
 * session closes the display (displayd ends, the console is text again) until it ends. Without a display (too
 * little chip RAM, no displayd), init's text login takes the console instead. */
#include "pw.h"
#include "../zdesk/zdesk.h"

int depth, verbose;                     /* conf.c's (zdesk's globals) */
static int logfd = 2, handover, win = -1, field, nsess, sess, confirmed;
static char host[64] = "zenix", name[32], pass[100], msg[48];
#define NSESS 4
/* prog "text": the user's shell on the console; a label (21 at most) fits the field's 25 cells with "< " and " >" */
static struct { char label[22], prog[64]; } sessions[NSESS];
static char theme[16];
/* "autologin NAME SESSION" (DESIGN D25 revision 3, 7.3: the demo floppy): that user's session starts without the
 * greeter, once (the marker file below says it happened); never root */
static char auto_name[32], auto_sess[22];
#define AUTO_MARK "/tmp/.dlogin-autologin"
#define F_NAME 0
#define F_PASS 1
#define F_SESS 2
#define BW 336
#define BH 112

static void read_hostname(void)         /* as login (DESIGN D35): /etc/hostname's first line */
{
    int fd = sys_open("/etc/hostname", O_RDONLY);
    if (fd < 0) return;
    char b[64];
    int n = sys_read(fd, b, sizeof b - 1);
    sys_close(fd);
    if (n <= 0) return;
    int k = 0;
    while (k < n && ((b[k] >= 'a' && b[k] <= 'z') || (b[k] >= '0' && b[k] <= '9') || b[k] == '-')) k++;
    if (k && (k == n || b[k] == '\n')) { memcpy(host, b, (u32)k); host[k] = 0; }
}

/* /etc/dlogin: "session LABEL = PROGRAM" (an absolute path, or text), "theme NAME"; without one the desktop and
 * the text console */
static void read_conf(void)
{
    int fd = sys_open("/etc/dlogin", O_RDONLY);
    char b[1024];
    int n = fd < 0 ? 0 : sys_read(fd, b, sizeof b - 1);
    if (fd >= 0) sys_close(fd);
    b[n > 0 ? n : 0] = 0;
    if (n == (int)sizeof b - 1)
        dprintf(logfd, "dlogin: /etc/dlogin: only its first %d bytes are read\n", n);
    int ln = 0;
    for (char *l = b, *e; *l; l = e) {
        for (e = l; *e && *e != '\n'; e++)
            ;
        if (*e) *e++ = 0;
        ln++;
        while (*l == ' ' || *l == '\t') l++;
        for (char *t = l + strlen(l); t > l && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r'); )
            *--t = 0;                   /* nor at the end */
        if (!*l || *l == '#') continue;
        if (!strncmp(l, "theme ", 6) && strlen(l + 6) < sizeof theme) { strcpy(theme, l + 6); continue; }
        if (!strncmp(l, "autologin ", 10)) {
            char *u = l + 10, *sp;
            while (*u == ' ') u++;
            sp = strchr(u, ' ');
            if (sp && sp - u < (int)sizeof auto_name && strlen(sp + 1) < sizeof auto_sess) {
                memcpy(auto_name, u, (u32)(sp - u)); auto_name[sp - u] = 0;
                for (sp++; *sp == ' '; sp++)
                    ;
                strcpy(auto_sess, sp);
                continue;
            }
            dprintf(logfd, "dlogin: /etc/dlogin: line %d skipped\n", ln);
            continue;
        }
        char *eq = strstr(l, " = ");
        if (strncmp(l, "session ", 8) || !eq || nsess == NSESS) {
            dprintf(logfd, "dlogin: /etc/dlogin: line %d skipped\n", ln);
            continue;
        }
        *eq = 0;
        char *lab = l + 8, *prog = eq + 3;
        while (*lab == ' ') lab++;
        if (!*lab || strlen(lab) >= sizeof sessions[0].label || strlen(prog) >= sizeof sessions[0].prog ||
            (prog[0] != '/' && strcmp(prog, "text"))) {
            dprintf(logfd, "dlogin: /etc/dlogin: line %d skipped\n", ln);
            continue;
        }
        strcpy(sessions[nsess].label, lab);
        strcpy(sessions[nsess].prog, prog);
        nsess++;
    }
    if (!nsess) {
        strcpy(sessions[0].label, "Desktop"); strcpy(sessions[0].prog, "/bin/zdesk");
        strcpy(sessions[1].label, "Text console"); strcpy(sessions[1].prog, "text");
        nsess = 2;
    }
}

/* ---- drawing ---- */
static int bx(void) { return (dpy_w - BW) / 2; }
static int by(void) { return (dpy_h - BH) / 2; }
static void text(int x, int y, int fg, int bg, int attr, const char *s)
{
    dpy_text(win, x, y, fg, bg, attr, s, (int)strlen(s));
}
static void draw_clock(void)
{
    u32 s = sys_time() % 86400;
    char t[8];
    snprintf(t, sizeof t, "%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60));
    text(dpy_w - 48, 4, pen(R_CLOCK), pen(R_DESK), 0, t);
}
/* a field (26 cells and a 2-pixel margin): every pixel once - its text's cells, the cursor, the rest filled */
static void draw_field(int f)
{
    int x = bx() + 96, y = by() + 36 + f * 16, on = f == field, n = 0, cur = 0;
    int fg = pen(on ? R_SELFG : R_WINFG), bg = pen(on ? R_SELBG : R_MENUBG);
    char s[40];
    if (f == F_NAME) { strcpy(s, name); cur = on; }
    else if (f == F_PASS) { s[0] = 0; cur = on; }  /* never a sign of what or how much was typed */
    else snprintf(s, sizeof s, "< %s >", sessions[sess].label);
    n = (int)strlen(s);
    if (n > 25) n = 25;
    dpy_fill(win, x - 2, y - 2, 26 * 8 + 4, 2, bg);
    dpy_fill(win, x - 2, y + 8, 26 * 8 + 4, 2, bg);
    dpy_fill(win, x - 2, y, 2, 8, bg);
    if (n)
        dpy_text(win, x, y, fg, bg, 0, s, n);
    if (cur)
        dpy_fill(win, x + n * 8, y, 8, 8, fg);
    int e = x + (n + cur) * 8;
    dpy_fill(win, e, y, x + 26 * 8 + 2 - e, 8, bg);
}
static void draw_msg(void);
static void draw_box(void)
{
    int x = bx(), y = by();
    dpy_fill(win, x - 1, y - 1, BW + 2, BH + 2, pen(R_BARLINE));
    dpy_fill(win, x, y, BW, BH, pen(R_WINBG));
    text(x + (BW - (int)strlen(host) * 8) / 2, y + 8, pen(R_WINFG), pen(R_WINBG), TA_BOLD, host);
    dpy_fill(win, x + 8, y + 22, BW - 16, 1, pen(R_BARLINE));
    static const char *const labels[3] = { "Name", "Password", "Session" };
    for (int f = 0; f < 3; f++) {
        text(x + 16, y + 36 + f * 16, pen(R_WINFG), pen(R_WINBG), 0, labels[f]);
        draw_field(f);
    }
    draw_msg();
}
static void draw_all(void)              /* the whole screen (its first showing, an expose): the desk around the box */
{
    int x = bx() - 1, y = by() - 1, d = pen(R_DESK);
    dpy_fill(win, 0, 0, dpy_w, y, d);
    dpy_fill(win, 0, y + BH + 2, dpy_w, dpy_h - y - BH - 2, d);
    dpy_fill(win, 0, y, x, BH + 2, d);
    dpy_fill(win, x + BW + 2, y, dpy_w - x - BW - 2, BH + 2, d);
    static const char hint[] = "Tab: next field   Left/Right: session   Return: log in";
    text((dpy_w - (int)(sizeof hint - 1) * 8) / 2, dpy_h - 14, pen(R_CLOCK), pen(R_DESK), 0, hint);
    draw_clock();
    draw_box();
}
static void draw_msg(void)              /* the message line, every pixel once */
{
    int x = bx(), y = by() + 92, n = (int)strlen(msg), tx = x + (BW - n * 8) / 2, bg = pen(R_WINBG);
    dpy_fill(win, x + 8, y, tx - x - 8, 8, bg);
    if (n)
        dpy_text(win, tx, y, pen(R_WINFG), bg, TA_BOLD, msg, n);
    dpy_fill(win, tx + n * 8, y, x + BW - 8 - tx - n * 8, 8, bg);
}
static void say(const char *s)
{
    strncpy(msg, s, sizeof msg - 1);
    draw_msg();
    dpy_flush();
}

/* ---- the display: open, the manager's role, the greeter's window ---- */
static int dpy_up;                      /* connected */
static int greeter_up(void)
{
    int r;
    if (!dpy_up) {
        if ((r = dpy_open("dlogin")) < 0)
            return r;
        dpy_up = 1;
        depth = dpy_depth;
    }
    if ((r = dpy_call(DPY_SESSION, 0, 0, 0, 0)) < 0)      /* root's again: the last session's programs leave */
        return r;
    struct dpyev e;
    while (dpy_next(&e, 0) > 0)                         /* what came while a session ran */
        ;
    dpy_lost();
    if ((r = dpy_call(DPY_WM_HELLO, 0, 0, 0, 0)) < 0)
        return r;
    struct dpystyle st = th.st;                         /* the theme's colours (no frames: the greeter has none) */
    if (depth < 4)
        for (int i = 8; i < 16; i++) st.palette[i] = st.palette[th.map8[i]];
    dpy_call(DPY_WM_STYLE, (u32)&st, 0, 0, 0);
    if ((win = dpy_window(WK_DESKTOP, "dlogin", 0, 0, dpy_w, dpy_h, 0)) < 0)
        return win;
    dpy_call(DPY_WM_FOCUS, (u32)win, 0, 0, 0);
    field = name[0] ? F_PASS : F_NAME;
    draw_all();
    dpy_flush();
    if (!confirmed) {                   /* console 1's login screen is up: this boot works (as login, DESIGN D15) */
        struct uupdate u;
        if (sys_update(UPD_INFO, (u32)&u, sizeof u) == 0) sys_update(UPD_NOTE, UPDNOTE_CONFIRMED, u.kvnum);
        confirmed = 1;
    }
    return 0;
}

/* ---- the session ---- */
static void wipe(void)
{
    volatile char *p = pass;
    for (u32 i = 0; i < sizeof pass; i++) p[i] = 0;
}
static void session(const struct pw *p)
{
    int text_s = !strcmp(sessions[sess].prog, "text");
    dpy_destroy(win);                   /* the greeter goes: the session's manager never adopts dlogin's window */
    win = -1;
    dpy_call(DPY_WM_BYE, 0, 0, 0, 0);
    if (!text_s)
        dpy_call(DPY_SESSION, p->uid, 0, 0, 0);
    else {
        dpy_close();                    /* the console is text again (displayd ends with its last client) */
        dpy_up = 0;
    }
    char *av[] = { "/bin/login", "-f", "-s", sessions[sess].prog, name, 0 };
    char *tv[] = { "/bin/login", "-f", name, 0 };
    int pid = sys_spawn("/bin/login", text_s ? tv : av, 0), st = -1;
    if (pid > 0)
        st = sys_wait(pid);
    else
        dprintf(logfd, "dlogin: cannot start /bin/login: %s\n", zwhy("failed"));
    if (st != 0) {                      /* login (or the session's program) did not start: said, not hidden */
        dprintf(logfd, "dlogin: %s's session could not start (%s: status %d)\n", name, sessions[sess].prog, st);
        strcpy(msg, "The session could not start");
    }
}

static void run_session(const struct pw *p);
static void submit(void)
{
    struct pw p;
    say("Checking...");
    int ok = name[0] && pw_auth(name, pass, &p);
    wipe();
    struct pw r;
    if (name[0] && pw_byname(name, &r) == 0 && r.uid == 0)
        ok = -1;                        /* root: refused whatever the password (no answer about it) */
    if (ok <= 0) {                      /* said on its output too (the kernel log, under init) */
        dprintf(logfd, "dlogin: %s\n", ok < 0 ? "root refused (a text console is root's)" : "login incorrect");
        say(ok < 0 ? "root logs in on a text console" : "Login incorrect");
        sys_sleep(PW_FAIL_DELAY);
        struct dpyev e;
        int all = 0;
        while (dpy_next(&e, 0) > 0)     /* what was typed during the wait is not the next try */
            all |= e.type == EV_EXPOSE;
        all |= dpy_lost();              /* an expose meanwhile (a console switch): the whole screen again */
        int old = field;
        field = F_PASS;
        if (all)
            draw_all();
        else {
            if (old != F_PASS) draw_field(old);
            draw_field(F_PASS);
        }
        dpy_flush();
        return;
    }
    run_session(&p);
}

/* the user is known (a right password, or autologin): the session, then the greeter again */
static void run_session(const struct pw *p)
{
    msg[0] = 0;                         /* (session() may set it: shown on the greeter that comes back) */
    dprintf(logfd, "dlogin: %s (uid %u) logged in: %s\n", name, (unsigned)p->uid, sessions[sess].label);
    session(p);
    dprintf(logfd, "dlogin: %s's session ended\n", name);
    name[0] = 0;
    if (greeter_up() < 0) {             /* the display is gone: init starts the text login */
        dprintf(logfd, "dlogin: the display could not be opened again\n");
        sys_exit(1);
    }
}

/* /etc/dlogin's autologin, at the first start of dlogin while the marker does not exist (on a RAM root, as the demo
 * floppy's, that is once a boot); the marker is made first, so a session that fails or a dlogin started again by init
 * shows the greeter. Never root, never an unknown user or session (said in the log). */
static void autologin(void)
{
    if (!auto_name[0] || !handover) return;
    int fd = sys_open(AUTO_MARK, O_WRONLY | O_CREAT | O_EXCL);
    if (fd < 0) return;                 /* it happened already (or /tmp is not writable: no autologin) */
    sys_close(fd);
    struct pw p;
    int i;
    for (i = 0; i < nsess && strcmp(sessions[i].label, auto_sess); i++)
        ;
    if (i == nsess) { dprintf(logfd, "dlogin: autologin: no session '%s'\n", auto_sess); return; }
    if (pw_byname(auto_name, &p) != 0) { dprintf(logfd, "dlogin: autologin: no user '%s'\n", auto_name); return; }
    if (p.uid == 0) { dprintf(logfd, "dlogin: autologin: never root\n"); return; }
    strcpy(name, auto_name);
    sess = i;
    dprintf(logfd, "dlogin: autologin\n");
    run_session(&p);
}

static void edit(char *s, u32 room, const struct dpyev *e)
{
    u32 n = strlen(s);
    unsigned char c = e->ch[0];
    if (e->code == 0x41) {              /* Backspace: one character (a UTF-8 one whole) */
        while (n && ((unsigned char)s[n - 1] & 0xC0) == 0x80) n--;
        if (n) n--;
        s[n] = 0;
    } else if (c == 0x15) {             /* Ctrl+U */
        s[0] = 0;
    } else if (c >= 0x20 && c != 0x7F) {
        u32 k = 1;
        while (k < 4 && e->ch[k]) k++;
        if (s == name && (k > 1 || c == ' ' || c == ':'))
            return;                     /* a login name is plain (pw_name_ok's letters) */
        if (n + k < room) { memcpy(s + n, e->ch, k); s[n + k] = 0; }
    }
}

static void key(const struct dpyev *e)
{
    int c = e->code;
    if (c & 0x80) return;
    int old = field;
    if (c == 0x42)                      /* Tab, Shift+Tab */
        field = (field + ((e->qual & (GQ_LSHIFT | GQ_RSHIFT)) ? 2 : 1)) % 3;
    else if (c == 0x4C) field = (field + 2) % 3;
    else if (c == 0x4D) field = (field + 1) % 3;
    else if ((c == 0x4E || c == 0x4F) && field == F_SESS) sess = (sess + (c == 0x4E ? 1 : nsess - 1)) % nsess;
    else if (c == 0x44 || c == 0x43) {  /* Return, Enter */
        if (field == F_NAME && name[0]) field = F_PASS;
        else if (name[0]) { submit(); return; }
    } else if (c == 0x45) {             /* Esc: start again */
        name[0] = 0; wipe(); msg[0] = 0;
        int old2 = field;
        field = F_NAME;
        draw_field(F_NAME);
        if (old2 != F_NAME) draw_field(old2);
        draw_msg();
        dpy_flush();
        return;
    } else if (field == F_NAME) edit(name, sizeof name, e);
    else if (field == F_PASS) edit(pass, sizeof pass, e);
    if (old != field) draw_field(old);
    draw_field(field);
    dpy_flush();
}

static void click(const struct dpyev *e)
{
    if (e->code != 1) return;
    int f = (e->y - by() - 34) / 16;
    if (e->y < by() + 34 || f > 2 || e->x < bx() + 94 || e->x > bx() + 96 + 26 * 8) return;
    int old = field;
    field = f;
    if (f == F_SESS) sess = (sess + (e->x < bx() + 96 + 13 * 8 ? nsess - 1 : 1)) % nsess;
    draw_field(old);
    draw_field(field);
    dpy_flush();
}

int main(int argc, char **argv)
{
    setpgid(0, 0);                      /* a group of its own, as login's: `kill 0` in a session never reaches init */
    if (argc > 1 && !strcmp(argv[1], "--init")) handover = 1;
    else if (argc > 1) { dprintf(2, "usage: dlogin [--init]\n"); return 2; }
    if (sys_getuid() != 0) { dprintf(2, "dlogin: root's (it starts other users' sessions)\n"); return 1; }
    if (handover) {                     /* init's: what it says goes into the kernel log (dmesg), as a daemon's */
        int k = sys_open("/dev/kmsg", O_WRONLY);
        if (k >= 0) logfd = k;
    }
    read_hostname();
    read_conf();
    int r = conf_theme(theme[0] ? theme : 0) < 0 ? -ENOENT : 0;    /* (conf.c says which file) */
    if (r == 0) r = greeter_up();
    if (r < 0) {
        dprintf(logfd, "dlogin: %s (%s): the text login instead\n", r == -ENOENT ? "no theme" : "no display",
                zstrerror(r) ? zstrerror(r) : "failed");
        if (!handover)
            return 1;
        char *lv[] = { "/bin/login", "--init", 0 };
        int pid = sys_spawn("/bin/login", lv, 0);
        if (pid < 0) return 1;
        sys_exit_handing(ST_HANDED | pid, pid);  /* init waits for it and its shell, as for its own login */
    }
    autologin();
    for (;;) {
        struct dpyev e;
        u32 s = sys_time() % 60;
        int got = dpy_next(&e, (int)(60 - s) * 100);
        if (got < 0) {
            dprintf(logfd, "dlogin: the display server is gone (%d)\n", got);
            return 1;                   /* init starts dlogin again */
        }
        if (!got) { draw_clock(); dpy_flush(); continue; }
        if (dpy_lost()) { draw_all(); dpy_flush(); }
        if (e.win != win) continue;
        if (e.type == EV_EXPOSE) { draw_all(); dpy_flush(); }
        else if (e.type == EV_KEY) key(&e);
        else if (e.type == EV_BUTTON) click(&e);
    }
}
