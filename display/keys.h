/* SPDX-License-Identifier: GPL-2.0-only */
/* Configurable keys for the desktop's programs (display/lib/keys.c; DESIGN D48 4, D50): Ahiru.keys's format, one key a
 * line, `KEYS ACTION [ARGUMENT]`; KEYS are typed characters or names in <> (<Up> <S-Up> <C-d> <Return> <Esc> <Tab>
 * <Space> <Del> <BS> <Help> <F1>..<F10>), two keys in a row a prefix; `ACTION none` takes a key away; `;` and `#`
 * comments; `[main]` the section read (others skipped). A program's built-in keys are text in this format; the
 * user's ~/.config/PROG/keys is read after them, and written with the built-in keys when it is missing. Shared by
 * files and show (one parser, D13's rule), and with irc and mail, which read keys from a terminal (keys_tty). */
#pragma once
#include "proto.h"

#define KM_UP 300
#define KM_DOWN 301
#define KM_LEFT 302
#define KM_RIGHT 303
#define KM_SUP 304
#define KM_SDOWN 305
#define KM_SLEFT 306
#define KM_SRIGHT 307
#define KM_HELP 308
#define KM_F1 310
#define KM_CUP 320
#define KM_CDOWN 321
#define KM_CLEFT 322
#define KM_CRIGHT 323

struct kbind { unsigned short k1, k2; unsigned char act; char arg[27]; };
struct keyset {
    const char *const *names;   /* the actions' names, by number (0: "none") */
    int nact;
    struct kbind *b;            /* the bindings (room for max) */
    int n, max;
    char bad[80];               /* the first problem in the user's file, said by the program ("keys: line 12: ...") */
};

void keys_text(struct keyset *ks, const char *t, int file);    /* lines added; file: problems noted in ks->bad */
/* the built-in keys, then ~/.config/PROG/keys (written with def, deflen bytes, when missing) -> 1 when written */
int keys_load(struct keyset *ks, const char *def, unsigned deflen, const char *prog);
int keys_prefix(const struct keyset *ks, int k);               /* k starts a two-key binding */
const struct kbind *keys_find(const struct keyset *ks, int k1, int k2);
int keys_code(const struct dpyev *e);                           /* a key event -> a key code (0: none) */
void keys_name(int k, char *o);                                 /* a key code -> its name (12 bytes) */
int keys_tty(int fd);           /* a key read from a terminal (irc, mail): a code, 0x10000 + a byte >= 0x80, -1 the end */
