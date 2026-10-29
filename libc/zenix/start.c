/* SPDX-License-Identifier: GPL-2.0-only */
/* The start and the end of a C library program (DESIGN D27 section 5.3). crt0 is libz's, unchanged: one call to
 * _zstart with argc and argv. exit runs the atexit functions and destructors (picolibc keeps them in the fini array),
 * then flushes the streams and Zenix's output buffer only if the program used them (pointers their first use sets,
 * so a program that does not use stdio links none of it), then ends. The environment is loaded at the start when the
 * program links it at all: a raw program carries none of its 1.3 KB. */
#include <stdlib.h>
#include <unistd.h>
#include <sys/_initfini.h>
#include "zshare.h"

char **_zargv;
void zenv_load(void) __attribute__((weak));   /* zenv.o is linked only if the program uses the environment */
extern u32 __zstart_tick;
void (*__zx_stdio_flush)(void);         /* stdio.c: set when a stream is first used */
int main(int argc, char **argv);
void _zstart(int argc, char **argv) __attribute__((noreturn));
void _zstart(int argc, char **argv)
{
    __zstart_tick = sys_ticks();
    _zargv = argv;
    if (zenv_load) zenv_load();         /* environ, for a program that uses it (getenv, setenv, environ) */
    __libc_init_array();
    exit(main(argc, argv));
}

void exit(int code)
{
    __libc_fini_array();
    if (__zx_stdio_flush) __zx_stdio_flush();
    if (zout_at_exit) zout_at_exit();
    _exit(code);
}
