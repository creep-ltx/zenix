/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/* ZINE's assigns (docs/d29-zine.md 16.8): the names a program gives to directories ("LIBS:", "C:"), set by dos
 * Execute of an Assign command (which ZINE does itself: it has no shell), and the paths they stand for. Plain C
 * (zcore.h) with no allocation: the caller keeps the table. Shared by the blob and the host test.
 *
 * A path "NAME:rest" whose NAME is assigned stands for each of NAME's directories in turn ("LIBS:x" for
 * "SYS:LIBS/x", then "PROGDIR:x", ...); a directory that is itself assigned is followed (its first directory), at most
 * ZA_DEPTH times. Names compare without case (Latin-1), as AmigaDOS's. */
#include "zcore.h"

#define ZA_MAX      8                   /* assigns */
#define ZA_TGT      3                   /* directories of one assign (a multi-directory assign) */
#define ZA_NAMEMAX  30                  /* bytes of an assign's name, without its ':' */
#define ZA_PATHMAX  95                  /* bytes of a directory's path */
#define ZA_DEPTH    4                   /* assigns followed through other assigns */

struct za_assign {
    char name[ZA_NAMEMAX + 1];          /* "" when the slot is free */
    zu8 n;                              /* directories */
    char tgt[ZA_TGT][ZA_PATHMAX + 1];
};
struct za_table { struct za_assign a[ZA_MAX]; };

/* What ZINE starts with: LIBS: is the program's volume's LIBS directory, the program's own directory and RAM:. */
void za_init(struct za_table *t);
/* Sets NAME (n bytes, no ':') to the k directories (replacing it; with add, after the ones it has; k 0 and not add:
 * removed). 0, or -ZE_NOSPC (no free slot, or more than ZA_TGT directories), -ZE_NAMETOOLONG, -ZE_INVAL (empty) */
int za_set(struct za_table *t, const char *name, zu32 n, const char *const *dirs, zu32 k, int add);
/* The i-th path path stands for (from 0): 1 and the path in out (max bytes with its 0), or 0 when there is no i-th
 * (an unassigned path stands only for itself, as its 0th) or it would be longer than max - 1. */
int za_expand(const struct za_table *t, const char *path, zu32 i, char *out, zu32 max);
/* The part after "RAM:" (or "Ram Disk:") when path is on ZINE's RAM:, else 0 */
const char *za_ram(const char *path);

/* ZINE's memory files (docs/d29-zine.md 16.9): every file a program writes, on any path, is kept in its memory under
 * one name per file, its key: path (with its assigns followed) in one form, so that "data", "PROGDIR:data",
 * "SYS:data" and "VOLNAME:data" are one file, and "ram:x" and "Ram Disk:x" another. The key is "DEV:rest": DEV is
 * "SYS" for the program's volume (no device, ":", "SYS:", "PROGDIR:", the volume's name vol), "SYS:C/" for the
 * volume's own C:, S:, L:, LIBS:, DEVS:, FONTS: (as zv_find has them), "RAM" for RAM: and "Ram Disk:", else the
 * device's name in capitals ("T", "ENV", "DH0"); rest with '/' or "//" going up one, no '/' at either end. 1 and the
 * key in out (max bytes with its 0), or 0 (longer than max - 1, or above a device's top). Keys compare without case. */
int za_key(const char *path, const char *vol, char *out, zu32 max);

/* An Execute command line that is an Assign ("Assign NAME: DIR...", "C:Assign", "Ram:Assign"; the options ADD,
 * REMOVE; DEFER and PATH taken as a plain assign): */
struct za_cmd {
    char name[ZA_NAMEMAX + 1];
    zu32 n;                             /* directories given */
    char dir[ZA_TGT][ZA_PATHMAX + 1];
    int add, remove;
};
/* 1: an Assign ZINE does (c filled); 0: not an Assign command; -1: an Assign ZINE cannot do (no name: a listing;
 * a word it does not know; a name or path too long; more directories than ZA_TGT). The command ends at its first
 * newline or ';' or after len bytes. */
int za_parse(const char *cmd, zu32 len, struct za_cmd *c);
/* c applied to t: 0 or za_set's error */
int za_apply(struct za_table *t, const struct za_cmd *c);

/* An Execute command line (docs/d29-zine.md 16.11): "[run] [<IN] [>OUT | >>OUT] NAME [<IN] [>OUT] ARGUMENTS", the
 * redirections before the name (after a leading "run") or right after it, as the Shell takes them; a redirection's file
 * in the same word ("> NIL:" too). The line ends at its first newline or ';' (a comment) or after len bytes. */
struct zx_cmd {
    char name[ZA_PATHMAX + 1];          /* the command as given ("1", "C:Assign", "parts/2"; quotes taken off) */
    char in[ZA_PATHMAX + 1], out[ZA_PATHMAX + 1];   /* "" when not redirected */
    int append, run;                    /* ">>", a leading "run" */
    zu32 at;                            /* where the name starts in cmd */
    zu32 args, arglen;                  /* the arguments: cmd[args, args + arglen), blanks at both ends taken off */
    zu32 next;                          /* where the next line starts (len when none) */
};
/* 1: c filled; 0: an empty line (blanks, a comment, a lone "run"), c->next set; -1: a name or file longer than
 * ZA_PATHMAX, or a '<' or '>' with no file (c->next set) */
int zx_parse(const char *cmd, zu32 len, struct zx_cmd *c);
/* 1 when name's last part (after its ':' or '/') is a command of AmigaOS's Shell or C: that ZINE does not run
 * (CD, Echo, Stack, Wait, SetPatch, LoadWB...: they act on the Shell or the system ZINE does not have), compared
 * without case */
int zx_known(const char *name);
