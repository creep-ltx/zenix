/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * telnet's core (DESIGN D20 3 and 4.4): the protocol (RFC 854, options by RFC 855 with RFC 1143's Q method, BINARY,
 * ECHO, SGA, TERMINAL-TYPE, NAWS), the stream filter of the three modes, the key translation and the CP437 tables.
 * Plain C without system calls and without memory of its own (the caller holds the structs), as netd's core: the
 * same files build for Zenix's telnet (user/telnet.c, D20 9 step 2) and for the PC tests and fuzzing
 * (tools/host/telnet-test.sh). Bytes leave through the caller's callbacks only.
 */
#ifndef TELNET_H
#define TELNET_H

/* the modes (3.5) */
#define TN_PC    1                      /* a PC board: CP437, -p */
#define TN_AMIGA 2                      /* an Amiga board: Latin-1 in Topaz, -a, the default */
#define TN_UNIX  3                      /* a Unix host: xterm-like, -x */

/* telnet's bytes (RFC 854) and the options this client knows */
#define TN_IAC  255
#define TN_DONT 254
#define TN_DO   253
#define TN_WONT 252
#define TN_WILL 251
#define TN_SB   250
#define TN_SE   240
#define TO_BINARY 0                     /* RFC 856 */
#define TO_ECHO   1                     /* RFC 857 */
#define TO_SGA    3                     /* RFC 858 */
#define TO_TTYPE  24                    /* RFC 1091 */
#define TO_NAWS   31                    /* RFC 1073 */

/* what telnet writes to the console when a call ends or is suspended (audit 2026-10-28 S12): CAN ends a string or
 * sequence the board left open, then plain colours, the whole screen as the scroll region (ESC 7/8 around it: ESC[r
 * homes the cursor), the cursor shown, normal cursor keys */
#define TN_TERM_RESET "\030\033[0m\0337\033[r\0338\033[?25h\033[?1l"
#define TN_NOPT   40                    /* options with a state (every one above is below this); others are refused */
#define TN_SBMAX  64                    /* a longer subnegotiation is dropped whole */

/* RFC 1143's states (us: our side, WILL/WONT; him: theirs, DO/DONT) and queue bits */
#define TQ_NO      0
#define TQ_YES     1
#define TQ_WANTNO  2
#define TQ_WANTYES 3

struct tn {
    /* the caller's */
    void *u;
    void (*send)(void *u, const unsigned char *b, long n);     /* to the other side as they are (already encoded) */
    int mode;                           /* TN_PC, TN_AMIGA, TN_UNIX: the terminal type's name */
    int cols, rows;                     /* the window size for NAWS */
    int raw;                            /* telnet -r: no decoding, no encoding, no options (a plain TCP service) */
    /* the protocol's */
    unsigned char us[TN_NOPT], usq[TN_NOPT], him[TN_NOPT], himq[TN_NOPT];
    int started;                        /* our asks were sent (port 23 at once, else at the other side's first IAC) */
    int st, verb;                       /* the decoder's state */
    int cr;                             /* the last data byte was a CR (CR NUL folding) */
    unsigned char sb[TN_SBMAX];
    int sbn, sbover;
};

/* Set up (zeroes the struct's state; the caller's fields are kept). port: the TCP port, 23 starts negotiating at once,
 * any other only after the other side's first IAC (BSD telnet's rule). */
void tn_init(struct tn *t, int port);

/* Bytes from the other side -> data for the screen (or a transfer) in out (room for n). IAC commands are answered
 * through send; IAC IAC is 0xFF; CR NUL is CR unless their BINARY is on. Returns the data bytes. */
long tn_decode(struct tn *t, const unsigned char *in, long n, unsigned char *out);

/* Data (keys, a transfer's bytes) -> bytes for the other side in out (room for 2n): 0xFF doubled; CR -> CR NUL unless
 * our BINARY is on. Returns the count. */
long tn_encode(const struct tn *t, const unsigned char *in, long n, unsigned char *out);

/* A new window size: told at once when NAWS is agreed (and at its agreement). */
void tn_size(struct tn *t, int cols, int rows);

/* Is an option on? which: 0 ours (WILL), 1 theirs (DO). */
int tn_on(const struct tn *t, int which, int opt);

/* Local echo: only in the Unix mode, when the host does not echo (3.1). */
int tn_local_echo(const struct tn *t);

/* ---- the stream filter (4.4): between the decoder and the screen; no screen model ---- */

#define TF_EV_ICE  1                    /* CSI ? 33 h/l: arg 1 on, 0 off (the console's CONR_ICE) */
#define TF_EV_MODE 2                    /* SyncTERM's font switch: arg TN_PC or TN_AMIGA (the filter has switched) */
#define TF_MUSICMAX 1024                /* SyncTERM's musicbuf: a longer music string ends there */
#define TF_SEQMAX   64                  /* a longer escape sequence is passed through as it is */

struct tf {
    /* the caller's */
    void *u;
    void (*put)(void *u, const unsigned char *b, long n);      /* to the screen, at most 256 bytes a call */
    void (*answer)(void *u, const unsigned char *b, long n);   /* to the board (the caller encodes it) */
    void (*event)(void *u, int ev, int arg);                   /* may be NULL */
    int mode;                           /* TN_PC, TN_AMIGA, TN_UNIX */
    int fallback;                       /* the PC mode without the console's PC font: CP437 -> Latin-1 (3.5) */
    int cols, rows;                     /* for the Amiga window bounds report */
    /* the filter's */
    int st;
    int musicmode;                      /* 0 only CSI |, 1 BANSI (CSI | and CSI N, the default), 2 all (CSI M too) */
    int musicn;
    unsigned char seq[TF_SEQMAX];
    int seqn;
    unsigned long ucs;                  /* the Unix mode's UTF-8 decoding */
    int need;
    unsigned char ob[256];
    int obn;
};

void tf_init(struct tf *f, int mode);   /* zeroes the state, keeps the caller's fields; BANSI music */
void tf_write(struct tf *f, const unsigned char *b, long n);
void tf_flush(struct tf *f);            /* hand what is held to put (tf_write does at its end) */
void tf_setmode(struct tf *f, int mode);

/* ---- keys (3.3) ---- */

/* The console's keys -> what the board gets (before tn_encode), in out (room for 2n). *bell counts the keys with no
 * form in the mode (a Latin-1 letter CP437 lacks): not sent. The escape key is the caller's (looked for first). */
long tk_keys(int mode, const unsigned char *in, long n, unsigned char *out, int *bell);

/* the tables (cp437.c) */
extern const unsigned char tn_cp437_latin1[256];
extern const unsigned char tn_latin1_cp437[256];

#endif
