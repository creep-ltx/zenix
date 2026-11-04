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
