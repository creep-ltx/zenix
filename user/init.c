/* SPDX-License-Identifier: GPL-2.0-only */
#include "libz.h"

/* init: print /etc/motd, mount what /etc/fstab lists, then keep a login prompt on the virtual consoles, like a
 * Unix init with a getty on each terminal. Console 1 (/dev/tty1) gets its login at once; console N (2..10) the first
 * time it is shown (Amiga+FN: the kernel tells init through sys_waitcon, like Linux's autovt). A login that lets a user
 * in starts the shell and steps aside (login --init, DESIGN D18): the shell becomes init's child, and its end (the user
 * logged out) starts a new login on its console. /etc/autologin applies to console 1 only (login.c). */
#define NCON 10
static int pid[NCON + 1];               /* the session on console n, its login and then its shell; 0 = none */

static void start(int n, int again)
{
    char dev[12] = "/dev/tty";
    if (n == 10) { dev[8] = '1'; dev[9] = '0'; dev[10] = 0; }
    else { dev[8] = (char)('0' + n); dev[9] = 0; }
    int fd = sys_open(dev, O_RDWR);
    if (fd < 0) { printf("init: cannot open %s\n", dev); oflush(); return; }
    if (again) fprintf(fd, "init: session ended, starting another\n");
    char *argv[] = { "/bin/login", "--init", 0 };
    int io[3] = { fd, fd, fd };
    pid[n] = sys_spawn("/bin/login", argv, io);
    if (pid[n] < 0) {                   /* said on that console and on init's own output (flushed: init never ends) */
        const char *why = zstrerror(pid[n]); if (!why) why = "failed";
        fprintf(fd, "init: cannot start /bin/login: %s\n", why);
        printf("init: cannot start /bin/login on %s: %s\n", dev, why);
        oflush();
        pid[n] = 0;
    }
    sys_close(fd);
}

/* /etc/console (D14 section 11): "colours N" (2, 4, 8 or 16; "colors" too) sets the text console's colours, read
 * once here, before anything else takes chip RAM; no file, no such line or "auto": the machine's default. Read at
 * every start of init, so a change counts from the next boot. */
static void console_setup(void)
{
    char b[512];
    int fd = sys_open("/etc/console", O_RDONLY), n = fd < 0 ? 0 : sys_read(fd, b, sizeof b - 1);
    if (fd >= 0) sys_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    for (char *l = b, *e; *l; l = e) {
        for (e = l; *e && *e != '\n'; e++)
            ;
        if (*e) *e++ = 0;
        char *w = l, *v;
        while (*w == ' ' || *w == '\t') w++;
        if (!*w || *w == '#') continue;
        for (v = w; *v && *v != ' ' && *v != '\t'; v++)
            ;
        if (*v) *v++ = 0;
        while (*v == ' ' || *v == '\t') v++;
        if (strcmp(w, "colours") && strcmp(w, "colors")) { printf("init: /etc/console: unknown setting %s\n", w); continue; }
        if (!strncmp(v, "auto", 4)) continue;
        int c = atoi(v), r = c ? sys_console(CON_COLOURS, c) : -EINVAL;
        if (r == -EINVAL) printf("init: /etc/console: colours %s: 2, 4, 8 or 16 (or auto)\n", v);
        else if (r < 0) printf("init: /etc/console: colours %d: %s, staying at %d\n", c, zstrerror(r), sys_console(CON_COLOURS, 0));
        else if (r != c) printf("init: /etc/console: colours %d: this machine shows %d\n", c, r);
    }
    oflush();
}

int main(void)
{
    console_setup();
    int fd = sys_open("/etc/motd", 0);
    if (fd >= 0) {
        char b[256]; int n;
        while ((n = sys_read(fd, b, sizeof b)) > 0) sys_write(1, b, (u32)n);
        sys_close(fd);
    }
    int fs = sys_open("/etc/fstab", 0);                 /* mount what /etc/fstab lists (see mount(8)) */
    if (fs >= 0) {
        sys_close(fs);
        char *ma[] = { "/bin/mount", "-a", 0 };
        int mp = sys_spawn("/bin/mount", ma, 0);
        if (mp >= 0) sys_wait(mp);
    }
    sys_setfg(0);
    int again = 0;
    for (;;) {
        while (!pid[1]) {                               /* console 1 always has a login */
            if (again) sys_sleep(50);                   /* a login that fails at once must not keep the CPU busy */
            start(1, again);
            if (!pid[1]) sys_sleep(500);
        }
        again = 1;
        int r = sys_waitcon();                          /* a child ended (taken: r is its status), or a console wants a login */
        if (r >= ST_CONSOLE + 1 && r <= ST_CONSOLE + NCON) {
            int n = r - ST_CONSOLE;
            if (!pid[n]) start(n, 0);
            continue;
        }
        for (int n = 1; n <= NCON; n++) {               /* which session's process ended? */
            if (!pid[n]) continue;
            int st = sys_waitnb(pid[n]);
            if (st == -2) continue;                     /* still running */
            if (st == -1) st = r;                       /* the one waitcon took (each call takes one: exactly one -1) */
            if (st & ST_HANDED) {                       /* login handed its shell to us: the session goes on in it */
                int sh = st & ~ST_HANDED;
                if (sys_waitnb(sh) == -2) { pid[n] = sh; continue; }
            }                                           /* else (or the shell already ended): the session is over */
            pid[n] = 0;
            if (n > 1) { sys_sleep(50); start(n, 1); }  /* console 1: the loop above */
        }
    }
}
