/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* keymap - every key a program knows is an action bound in a text file
 * (CFile.keys, Ahiru.keys). Shared by CFile and Ahiru: the program gives
 * its actions, its sections and its built-in keys (the same text as the
 * file), the engine reads the file, writes it when missing, finds keys,
 * and makes the prefix panels and the help from what is bound.
 *
 * The file: one key a line, KEYS ACTION [ARGUMENT]. KEYS are typed
 * characters or names in <>: <Up> <S-Up> (Shift) <C-Up> (Ctrl) <C-d>
 * <Return> <Esc> <Tab> <Space> <Del> <BS> <S-BS> <C-BS> <Help> <F1>..<F10>. Two keys
 * in a row make a prefix. ACTION none takes a key away. [name] starts a
 * section (a context). */
#ifndef CF_KEYMAP_H
#define CF_KEYMAP_H

/* the key codes: characters as they are, these for the rest */
#define KM_UP 300
#define KM_DOWN 301
#define KM_LEFT 302
#define KM_RIGHT 303
#define KM_SUP 304              /* Shift + the arrows */
#define KM_SDOWN 305
#define KM_SLEFT 306
#define KM_SRIGHT 307
#define KM_HELP 308
#define KM_BREAK 309            /* (Ctrl-C reached a loop: not bindable) */
#define KM_F1 310               /* .. KM_F1 + 9 */
#define KM_CUP 320              /* Ctrl + the arrows */
#define KM_CDOWN 321
#define KM_CLEFT 322
#define KM_CRIGHT 323
#define KM_SBS 324              /* Shift + Backspace */
#define KM_CBS 325              /* Ctrl + Backspace */

struct kmact {
    const char *name, *desc;
    unsigned char ctx;          /* 0: every context (the action "none") */
};

/* ctxnames: the section names ("main", "view"), ctxheads: their help
 * headings ("In the file list:"); context n is ctxnames[n - 1] */
void km_setup(const struct kmact *acts, int nacts, const char *const *ctxnames,
              const char *const *ctxheads, int nctx);
/* the contexts (bit n for context n) whose keys are looked up one at a
 * time: a two-key line there is refused in the bad report. Before km_load. */
void km_single(unsigned mask);
/* the built-in keys, then the file (written with them when missing);
 * a line not understood is said in bad, the rest still work; so is a key
 * that also starts a two-key prefix (its own binding is never reached) */
void km_load(const char *file, const char *defaults, char *bad, int badsize);
int km_isprefix(int ctx, int k);
int km_find(int ctx, int k1, int k2, const char **arg);   /* the action, 0 none */
int km_hints(int ctx, int k, const char **items, int max, char (*lab)[12], char (*desc)[48]);
char *km_help(const char *head, const char *tail);        /* xalloc'ed: xfree it */
#define KM_FIRSTMAX 12                                     /* km_first's dst holds this */
void km_first(int ctx, int act, char *dst);               /* its first key's name, "?" none */
int km_action(const char *name, int ctx);                 /* by name, <= 0 none */
void km_keyname(int k, char *dst);                        /* "j", "<Up>", "<C-d>" */
void km_free(void);

#endif
