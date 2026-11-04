// SPDX-License-Identifier: GPL-2.0-only
#include "libz.h"
typedef unsigned char u8;
typedef unsigned short u16;
/* gcontest: the test of a console in graphics mode (DESIGN D38 step 2, SYS_console CON_GRAB and CON_GACK), as the
 * display server will use it. `gcontest ack|noack [SECONDS]` grabs the console of its descriptor 0 with a Copper
 * list of its own (one blank plane: the whole screen green; with no plane at all FS-UAE shows no sprite, found
 * 2026-11-04) and a 16 x 16 red pointer (sprite 0, hot spot at
 * its top left), then writes every record of the ring as a line ("gcontest: key 0x20 qual 0x0 ch 61", "gcontest:
 * release 1", ...); with ack it answers every GEV_RELEASE (CON_GACK), with late 2 s later (after the kernel's 1 s
 * deadline: the answer that lets the kernel use the blitter again), with noack never. It also tries to free the
 * grab's memory (refused, EBUSY).
 * With ACCEL (hex, after SECONDS) it sets the mouse's acceleration (CON_MOUSE: THRESHOLD | MULTIPLIER << 8), after
 * trying a wrong one (0x900: refused, EINVAL). It ends after SECONDS (default 60) or at the q key, giving the console
 * back. `gcontest keys CODE...` types raw key
 * codes (hex) through CON_INPUT on descriptor 0, as the keyboard would (root); `gcontest mouse DX,DY,B ...` moves the
 * mouse and sets its buttons the same way (CONI_MOUSE). tools/gcon-test.sh runs it. */

static void say(const char *fmt, ...)
{
    char b[120];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    sys_write(1, b, (u32)n);
}

static int keys(int argc, char **argv)
{
    u8 c[256];
    int n = 0;
    for (int i = 2; i < argc && n < 256; i++)
        c[n++] = (u8)strtoul(argv[i], 0, 16);
    for (int at = 0; at < n;) {         /* a call may stop early (a terminal write's bound): the rest again */
        int r = sys_coninput(0, CONI_RAW, c + at, (u32)(n - at));
        if (r <= 0) {
            dprintf(2, "gcontest: keys: %d of %d taken (%s)\n", at, n, zwhy("refused"));
            return 1;
        }
        at += r;
    }
    return 0;
}

/* gcontest mouse DX,DY,BUTTONS ...: each record through CON_INPUT's mouse (D38), 3 ticks apart so the 100 Hz tick
 * sees every button change */
static int mouse(int argc, char **argv)
{
    for (int i = 2; i < argc; i++) {
        char *s = argv[i];
        u8 r[3];
        long vx = strtol(s, &s, 10);
        if (*s == ',') s++;
        long vy = strtol(s, &s, 10);
        if (vx < -128 || vx > 127 || vy < -128 || vy > 127) {
            dprintf(2, "gcontest: mouse: %s: a move is -128..127 counts\n", argv[i]);
            return 2;
        }
        r[0] = (u8)vx;
        r[1] = (u8)vy;
        if (*s == ',') s++;
        r[2] = (u8)strtol(s, &s, 10);
        if (sys_coninput(0, CONI_MOUSE, r, 3) != 3) {
            dprintf(2, "gcontest: mouse: %s\n", zwhy("refused"));
            return 1;
        }
        sys_sleep(3);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "keys"))
        return keys(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "mouse"))
        return mouse(argc, argv);
    if (argc < 2 || (strcmp(argv[1], "ack") && strcmp(argv[1], "noack") && strcmp(argv[1], "late"))) {
        dprintf(2, "usage: gcontest ack|late|noack [SECONDS [ACCEL]] | gcontest keys CODE... | gcontest mouse DX,DY,B...\n");
        return 2;
    }
    int ack = !strcmp(argv[1], "ack"), late = !strcmp(argv[1], "late");
    u32 secs = argc > 2 ? (u32)atoi(argv[2]) : 60;
    u16 *mem = sys_memalloc_chip(512 + 80);
    if (!mem) {
        dprintf(2, "gcontest: no chip RAM\n");
        return 1;
    }
    u16 *spr = mem, *null = mem + 2 * (16 + 2), *cop = mem + 64, *line = mem + 256;
    memset(line, 0, 80);
    spr[0] = spr[1] = 0;
    for (int i = 0; i < 16; i++) {
        spr[2 + 2 * i] = 0xFFFF;                 /* colour 1 of sprite 0 (colour 17): every pixel */
        spr[3 + 2 * i] = 0x0000;
    }
    spr[34] = spr[35] = 0;
    null[0] = null[1] = 0;
    u16 *c = cop;
#define MOVE(r, v) do { *c++ = (u16)(r); *c++ = (u16)(v); } while (0)
    MOVE(0x100, 0x9200);                         /* BPLCON0: hires, one plane (one blank line shown on every line) */
    MOVE(0x1FC, 0);                              /* FMODE: 16-bit fetch */
    MOVE(0x102, 0);
    MOVE(0x108, 0xFFB0);                         /* BPL1MOD -80: the same 80 bytes on every line */
    MOVE(0x092, 0x3C);                           /* DDFSTRT, DDFSTOP: hires */
    MOVE(0x094, 0xD4);
    MOVE(0x0E0, (u32)line >> 16);                /* BPL1PT */
    MOVE(0x0E2, (u32)line);
    MOVE(0x180, 0x00F0);                         /* COLOR00: green */
    MOVE(0x1A2, 0x0F00);                         /* COLOR17-19: sprite 0, red */
    MOVE(0x1A4, 0x0F00);
    MOVE(0x1A6, 0x0F00);
    MOVE(0x120, (u32)spr >> 16);                 /* SPR0PT: the pointer */
    MOVE(0x122, (u32)spr);
    for (int i = 1; i < 8; i++) {                /* SPR1PT .. SPR7PT: an empty sprite */
        MOVE(0x120 + 4 * i, (u32)null >> 16);
        MOVE(0x122 + 4 * i, (u32)null);
    }
    *c++ = 0xFFFF;
    *c++ = 0xFFFE;
#undef MOVE
    struct ucongrab g;
    memset(&g, 0, sizeof g);
    g.size = sizeof g;
    g.cop = (u32)cop;
    g.sprite = (u32)spr;
    g.sprh = 16;
    int gfd = sys_congrab(0, &g);
    if (gfd < 0) {
        dprintf(2, "gcontest: CON_GRAB: %s\n", zwhy("refused"));
        say("gcontest: grab refused %d\n", gfd);
        return 1;
    }
    say("gcontest: grabbed fd %d %ux%u depth %u chips %u\n", gfd, g.width, g.height, g.depth, g.chips);
    say("gcontest: memfree of the grab's memory -> %d\n", sys_memfree(mem));
    if (argc > 3) {                     /* the mouse's acceleration (CON_MOUSE): a wrong one first, then ACCEL */
        u32 acc = (u32)strtoul(argv[3], 0, 16);
        say("gcontest: accel 0x900 -> %d\n", sys_conmouse(gfd, 0x900));
        say("gcontest: accel 0x%x -> %d\n", acc, sys_conmouse(gfd, acc));
    }
    u32 end = sys_ticks() + secs * 100;
    int quit = 0;
    while (!quit && (int)(sys_ticks() - end) < 0) {
        struct pollfd p = { gfd, POLLIN, 0 };
        if (poll(&p, 1, 100) <= 0)
            continue;
        struct ugevent ev[8];
        int n = sys_read(gfd, ev, sizeof ev);
        for (int i = 0; n > 0 && i < n / (int)sizeof ev[0]; i++) {
            struct ugevent *e = &ev[i];
            switch (e->type) {
            case GEV_KEY:
                say("gcontest: key 0x%x qual 0x%x ch %x %x\n", e->code, e->qual, e->ch[0], e->ch[1]);
                if (e->code == 0x10)
                    quit = 1;
                break;
            case GEV_POINTER:
                say("gcontest: pointer %d %d qual 0x%x\n", e->x, e->y, e->qual);
                break;
            case GEV_RELEASE:
                say("gcontest: release %d\n", e->x);
                if (late)
                    sys_sleep(200);             /* 2 s: past the kernel's deadline; the answer still counts */
                if (ack || late) {
                    int r = sys_congack(gfd, (unsigned short)e->x);
                    say("gcontest: ack %d -> %d\n", e->x, r);
                }
                break;
            case GEV_ACQUIRE:
                say("gcontest: acquire\n");
                break;
            case GEV_LOST:
                say("gcontest: lost %d\n", e->x);
                break;
            default:
                say("gcontest: record %d\n", e->type);
            }
        }
    }
    sys_close(gfd);
    say("gcontest: end\n");
    return 0;
}
