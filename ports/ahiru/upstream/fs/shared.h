/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* shared - the core's few variables that every part reads and writes.
 *
 * They live in one block, reached through a pointer, so that code
 * loaded later (CFile loads its archive code only when it is needed -
 * src/modload.c) works on the same ones: it is handed the pointer. The
 * names read like plain variables everywhere. */
#ifndef CF_SHARED_H
#define CF_SHARED_H

#include <exec/types.h>

struct fsshared {
    char s_opmsg[160];            /* the last failure, for a run's summary */
    int s_cancelok, s_abortop;      /* Esc while an operation runs (fe.h) */
    int s_icons;                  /* a file's .info travels with it */
    ULONG s_statbytes, s_statfiles; /* what a measuring walk found */
    int s_verifycopy;             /* every copy is read back and compared */
    /* a process running an operation beside the front end (Ahiru's
     * background copy) has a block of its own: its Esc, message and
     * counts are not the front's */
    struct fsshared *job;
    APTR jobtask;
};

extern struct fsshared *fssh;

/* the block of the task asking: fssh, or the job's own (a call, not
 * inline: inline it cost 10K over the hundreds of uses) */
struct fsshared *fs_sh(void);
#define FSSH (fs_sh())
#define opmsg (FSSH->s_opmsg)
#define cancelok (FSSH->s_cancelok)
#define abortop (FSSH->s_abortop)
#define icons (FSSH->s_icons)
#define statbytes (FSSH->s_statbytes)
#define statfiles (FSSH->s_statfiles)
#define verifycopy (FSSH->s_verifycopy)

#endif
