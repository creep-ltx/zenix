/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* modload - code a program loads only when it is used (docs/core.md).
 * Shared: CFile and Ahiru both use it, with the same archive module (the
 * core's archive code) and a picture and sound module of their own.
 *
 * A module is linked as a small program of its own. It is found beside
 * the program as a file of its own, PROGRAM.NAME ("CFile.arc") - the
 * quick way on a floppy, where every byte of the program's file is read
 * when it starts - or else inside the program's file (tools/embedmod.py
 * puts it in a debug block, which the system's own loader skips). The
 * first call into one reads it; when the program waits for a key and
 * the module holds nothing, it goes again. tools/mkmod.py makes the
 * stubs on both sides. */
#ifndef CF_MODLOAD_H
#define CF_MODLOAD_H

struct cfmod {
    const char *name;           /* its tag in the file: "arc", "media" */
    void **exports;             /* (offset 4: the stubs read it) its table, 0 = not loaded */
    void *hunks;                /* its memory, as loaded */
    int nhunks;
    void *const *imports;       /* what it may call in CFile */
    int nexports;
    struct cfmod *next;         /* the loaded ones, for mod_idle */
};

/* what a module is handed at start (src/modinit.c) */
struct modlink {
    void *const *imports;
    void *fssh;
    void *dos, *gfx, *intui, *console;
    unsigned short v_exec, v_dos, v_int, v_gfx;
};

void mod_self(int argc, char **argv);  /* at start: where the program's own file is */
void **mod_need(struct cfmod *m);       /* its table, loaded if need be; 0 = cannot */
void mod_idle(void);                    /* waiting for a key: drop what is not in use, when memory is short */
void mod_exit(void);
extern int mod_quiet;                   /* a test hook: no loading messages */
/* console.device, when the program has it open: handed to the modules */
extern struct Device *mod_console;

#endif
