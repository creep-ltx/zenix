/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* os - the ONE place that knows which Kickstart this is.
 *
 * Every call that exists only from V36 (2.0) on goes through here with
 * a 1.3 road beside it, chosen at run time by the library versions.
 * The rest of CFile never calls a V36 function directly. (E resolved
 * library calls as raw LVOs with no check, and a V36 call on 1.3 jumps
 * through unowned memory - the same is true in C: audit, don't test.)
 */
#ifndef CF_OS_H
#define CF_OS_H

#include <exec/types.h>
#include <dos/dos.h>

extern UWORD v_exec, v_dos, v_int, v_gfx;    /* lib_Version of each */

/* open the libraries CFile needs on every Kickstart; FALSE = cannot run */
int os_init(void);
void os_exit(void);

/* ---- memory: AllocVec is V36; this keeps the size itself ---------- */
void *xalloc(ULONG size);           /* cleared; NULL when out of memory */
void xfree(void *p);

/* ---- directory scan: ExAll (V36, batched) or Examine/ExNext ------- */
struct dent {
    char name[108];
    LONG type;      /* fib_DirEntryType: > 0 dirs (and links), < 0 files */
    ULONG size;
    ULONG prot;
    LONG days, mins;
};
struct dscan;
struct dscan *ds_open(const char *path);    /* NULL: IoErr() says why */
int ds_next(struct dscan *d, struct dent *e);   /* 1 entry, 0 end, -1 error */
void ds_close(struct dscan *d);

/* ---- locks and names --------------------------------------------- */
/* the canonical name of a lock ("Work:a/b"); FALSE when it will not fit */
int os_lockname(BPTR lock, char *dst, int size);
/* 0 different volumes, 1 same volume, 2 the same object */
int os_samelock(BPTR a, BPTR b);
/* 0 missing, 1 file, 2 directory */
int os_pathtype(const char *path);
void os_quiet(int on);           /* no "insert volume" requesters while on */
/* free bytes on the volume holding path, or -1 (capped at 2^31-1) */
LONG os_freebytes(const char *path);
/* the text for a DOS error ("object not found"), never empty */
void os_fault(LONG err, char *dst, int size);

/* set a file's date: SetFileDate from V36; on 1.3 the ACTION_SET_DATE
 * packet sent by hand (the CFile13 road). TRUE when it took. */
int os_setdate(const char *path, const struct DateStamp *ds);
/* TRUE when the LAST part of path is a link, not the object itself
 * (Lock follows links). Always FALSE before V36 - 1.3 has no links. */
int os_islink(const char *path);

/* ---- the volume list: volumes, then assigns ----------------------- */
#define VL_VOLUME 0
#define VL_ASSIGN 1
/* calls add(ctx, "Name:", kind) for each; returns how many */
int os_volumes(void (*add)(void *ctx, const char *name, int kind), void *ctx);

/* ---- patterns: prepare once, match many (2.0's full AmigaDOS
 * patterns; 1.3: * ? #? #x). FALSE from prep: not a valid pattern */
int os_patprep(const char *pat);
int os_patmatch(const char *name);
void os_patfree(void);

/* a command started in dir and left running (output to NIL:); TRUE = it started */
int os_launch(const char *dir, const char *cmd);

#endif
