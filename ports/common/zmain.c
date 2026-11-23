/* SPDX-License-Identifier: GPL-2.0-only */
/* The one-system command line for the ported programs (docs/audit-2026-11-23-one-system.md section 3): --help and
 * --version answered before upstream's main runs, without a change to upstream's source: a function in the init
 * array (the C library's start runs it before main, the arguments in _zargv), this file built for each program with
 * -DZPORT_NAME (the Makefile's ZMAIN rules) and linked with it; the usage lines are
 * the manual pages' SYNOPSIS. What upstream does with other options (refusing an unknown one) stays upstream's.
 * A program with a keys file (ZPORT_KEYS) also answers -K: its built-in keys, upstream's own table (made global as
 * zport_keys by the Makefile's objcopy), the text it writes as the keys file when there is none. */
#include <zenix.h>

#define ZP(n) ZP_##n
#define ZPX(n) ZP(n)
#define ZP_sumi       "usage: sumi [+LINE] [FILE ...]\n       sumi -K\n"
#define ZP_maki       "usage: maki [-N] [-S] [+LINE | +/TEXT] [FILE | DIRECTORY | - ...]\n       command | maki\n       maki -K\n"
#define ZP_markdown   "usage: markdown [-e] [+LINE] [FILE ...]\n       markdown -K\n"
#define ZP_ahiru      "usage: ahiru [PATH] [LOG] [KEYS=...]\n       ahiru -K\n"
#define ZP_ctop       "usage: ctop [-d SECONDS]\n       ctop -K\n"
#define ZP_lua        "usage: lua [options] [script [argument...]]\n       lua [options] -\n"
#define ZP_make       "usage: make [--posix] [-ehiknpqrSst] [-C dir] [-f makefile] [-j n]\n" \
                      "            [-x pragma] [macro=value...] [target...]\n"
#define ZP_tcc        "usage: tcc [options] -o program file... [-lname...]\n       tcc [options] -c file.c [-o file.o]\n" \
                      "       tcc [options] -E file.c\n"
#define ZP_frotz      "usage: frotz [-m] [-q] [-p] [-w COLS] [-h ROWS] [-s SEED] [-u SLOTS] [-L SAVE]\n" \
                      "             [-Z LEVEL] STORY\n"
#define ZP_adventure  "usage: adventure [SAVEFILE]\n"
#define ZP_arithmetic "usage: arithmetic [-o +-x/] [-r RANGE]\n"
#define ZP_banner     "usage: banner [-dt] [-w WIDTH] [MESSAGE...]\n"
#define ZP_fortune    "usage: fortune [-aefilosw] [-m PATTERN] [[N%] FILE|DIRECTORY|all]...\n"
#define ZP_hangman    "usage: hangman [-d WORDLIST] [-m MINLEN]\n"
#define ZP_number     "usage: number [-l] [NUMBER...]\n"
#define ZP_rain       "usage: rain [-d DELAY]\n"
#define ZP_robots     "usage: robots [-Aajnrst] [MAXIMUM] [SCOREFILE]\n"
#define ZP_tetris     "usage: tetris [-bps] [-k KEYS] [-l LEVEL]\n"
#define ZP_worm       "usage: worm [LENGTH]\n"
#define ZP_worms      "usage: worms [-ft] [-d DELAY] [-l LENGTH] [-n NUMBER] [-S SEED]\n"
#define ZP_wump       "usage: wump [-h] [-a ARROWS] [-b BATS] [-p PITS] [-r ROOMS] [-t TUNNELS]\n"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef ZPORT_KEYS
extern const char zport_keys[];
#endif

static const char zp_usage[] = ZPX(ZPORT_NAME);
#ifdef ZPORT_KEYS
static const char zp_k[] = { '-', 'K', 0 };
#endif
extern char **_zargv;                   /* the C library's start: the arguments, set before the constructors run */
static void zport_cmdline(void)
{
    int argc = 0;
    while (_zargv && _zargv[argc])
        argc++;
    zopt(argc, _zargv, zp_usage);
#ifdef ZPORT_KEYS
    if (argc == 2 && !strcmp(_zargv[1], zp_k)) {
        if (write(1, zport_keys, strlen(zport_keys)) < 0)
            exit(1);
        exit(0);
    }
#endif
}
/* run from the init array (libc/zenix/start.c), as user/zenv.c's: gcc's constructor attribute would use .ctors, which
 * the link scripts drop */
static void (*const zport_cmdline_p)(void) __attribute__((section(".init_array"), used)) = zport_cmdline;
