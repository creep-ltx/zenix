/* SPDX-License-Identifier: GPL-2.0-only */
/* Configurable keys for the desktop's programs (display/lib/keys.c; DESIGN D48 4, D50): Ahiru.keys's format, one key a
 * line, `KEYS ACTION [ARGUMENT]`; KEYS are typed characters or names in <> (<Up> <S-Up> <C-d> <Return> <Esc> <Tab>
 * <Space> <Del> <BS> <Help> <F1>..<F10>), two keys in a row a prefix; `ACTION none` takes a key away; `;` and `#`
 * comments; `[main]` the section read (others skipped). A program's built-in keys are text in this format; the
 * user's ~/.config/PROG/keys is read after them, and written with the built-in keys when it is missing. Shared by
 * files and show (one parser, D13's rule), and with irc and mail, which read keys from a terminal (keys_tty). */
#pragma once
#include "proto.h"

/* the keys' codes: libz's (user/libz.h ZK_*), one numbering for the line editor and every keys file */
#define KM_UP ZK_UP
#define KM_DOWN ZK_DOWN
#define KM_LEFT ZK_LEFT
#define KM_RIGHT ZK_RIGHT
#define KM_SUP ZK_SUP
#define KM_SDOWN ZK_SDOWN
#define KM_SLEFT ZK_SLEFT
#define KM_SRIGHT ZK_SRIGHT
#define KM_HELP ZK_HELP
#define KM_F1 ZK_F1
#define KM_CUP ZK_CUP
#define KM_CDOWN ZK_CDOWN
#define KM_CLEFT ZK_CLEFT
#define KM_CRIGHT ZK_CRIGHT

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
/* a key name with modifiers, "<S-A-Return>" (docs/audit-2026-11-23-one-system.md OS-30: the desktop's global keys in
 * the same names): S Shift, C Ctrl, A the left Amiga key, RA the right one, M Alt; the key's own name (Return, Left,
 * q) into base (16 bytes); s moved past the '>'. The modifiers' bits, or -1 when s is no such name */
#define KMOD_SHIFT  1
#define KMOD_CTRL   2
#define KMOD_AMIGA  4
#define KMOD_RAMIGA 8
#define KMOD_ALT    16
int keys_mods(const char **sp, char *base);
int keys_tty(int fd);           /* a key read from a terminal (irc, mail): a code, 0x10000 + a byte >= 0x80, -1 the end */
