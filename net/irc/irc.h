/* SPDX-License-Identifier: GPL-2.0-only */
/* irc's core (DESIGN D58, docs/man/irc.txt): the IRC protocol and the client's model, with no system calls, shared by
 * the Zenix program (user/irc.c: the screen, the keys, the socket, the TLS helper) and the PC's tests
 * (tools/host/irc-client.c). The program feeds it the server's bytes (irc_input) and the lines typed (irc_command);
 * it answers through two callbacks: send (a whole line to the server, CR LF included, already in the charset chosen)
 * and shown (a line added to a window). Every byte from the server is untrusted: lines over 512 bytes are cut, the
 * text is made valid UTF-8 (a line that is not UTF-8 is read as ISO-8859-1), and no control character (C0, C1, DEL;
 * mIRC's colour and format codes) ever reaches a window's text. */
#ifndef IRC_H
#define IRC_H

#ifdef IRC_HOST
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int u32;
#else
#include "../../user/libz.h"
#endif

#define IRC_LINEMAX   512       /* RFC 1459: a message with its CR LF */
#define IRC_TEXTMAX   400       /* a typed message is sent in pieces of at most this many bytes */
#define IRC_WINMAX    20        /* windows: the status window, channels and queries */
#define IRC_NAMEMAX   64        /* a channel's or a nick's name */
#define IRC_QUERYMAX  (IRC_WINMAX - 4)   /* windows a stranger's message may open: 4 kept for channels */
#define IRC_WORDS     8         /* highlight words, autojoin channels */
#define IRC_VERSION   "0.1b1"

enum { IRC_W_STATUS, IRC_W_CHAN, IRC_W_QUERY };
/* a line's kind: the screen's colour (user/irc.c) */
enum { IRC_K_INFO, IRC_K_MSG, IRC_K_OWN, IRC_K_HI, IRC_K_ACT, IRC_K_ERR, IRC_K_NOTICE };
/* a window's activity since it was last shown */
enum { IRC_ACT_NONE, IRC_ACT_INFO, IRC_ACT_MSG, IRC_ACT_HI };
/* what the core asks of the program (irc->want, the program clears it) */
enum { IRC_WANT_NONE, IRC_WANT_QUIT, IRC_WANT_CONNECT, IRC_WANT_HELP };

struct irc_line {
    u32 t;                      /* seconds since 1970 (UTC) */
    unsigned char kind;
    unsigned short nick;        /* text[0..nick) is the "<nick>" part to colour (0: none) */
    char text[];                /* valid UTF-8, no control characters */
};

struct irc_win {
    char name[IRC_NAMEMAX];     /* "" for the status window */
    int kind, act, joined, names_open, rejoin;
    struct irc_line **ring;     /* the scrollback: cap lines, oldest replaced */
    int cap, head, n;
    int scroll;                 /* the program's: lines scrolled back from the newest */
    char *topic;
    char **nicks;               /* members: [0] the mode prefix ('@', '+', ... or ' '), then the nick */
    int nn, ncap;
};

struct irc_cfg {
    char *server, *nick, *user, *realname, *pass, *sasl_user, *sasl_pass, *quitmsg;
    char *autojoin[IRC_WORDS], *hilite[IRC_WORDS];
    int port, tls, latin1, scrollback, reconnect, bell, nautojoin, nhilite;
    char *text;                 /* the config file's text (the strings above point into it) */
};

struct irc {
    struct irc_cfg c;
    void *u;
    void (*send)(void *u, const char *line, int n);
    void (*shown)(void *u, int w, const struct irc_line *l);
    u32 now;                    /* the program sets it before every call */
    struct irc_win *w[IRC_WINMAX];
    int nw, cur;
    char me[IRC_NAMEMAX];       /* our nick */
    int registered, tries, sasl, connected;
    char prefix[8], pmodes[8];  /* PREFIX=(ov)@+ */
    char chanmodes[4][24];      /* CHANMODES=A,B,C,D */
    char chantypes[8];
    char in[IRC_LINEMAX + 1];   /* the server's line being assembled */
    int inn, cut;
    u32 ctcp_at;                /* the last CTCP answer (one in 2 s at most) */
    u32 ping_sent;              /* our lag PING's time (0: none out) */
    int lag;                    /* seconds, -1 unknown */
    int want;                   /* IRC_WANT_* */
    char want_host[IRC_NAMEMAX];
    int want_port, want_tls;
    char quit[IRC_TEXTMAX];
};

/* the configuration (a file's text, taken: freed by irc_free): 0, or -1 with a message (line number) in err */
int irc_config(struct irc *s, char *text, char *err, int errn);
void irc_init(struct irc *s);                   /* after the callbacks and the config are set: the status window */
void irc_free(struct irc *s);
void irc_login(struct irc *s);                  /* a new connection: CAP (SASL), PASS, NICK, USER */
void irc_input(struct irc *s, const char *b, int n);   /* the server's bytes */
void irc_lost(struct irc *s, const char *why);  /* the connection ended */
void irc_command(struct irc *s, const char *typed);    /* a line typed in the current window (at most 1,023 bytes) */
void irc_keepalive(struct irc *s);              /* a PING to measure the lag (the program's timer) */
void irc_print(struct irc *s, int w, int kind, const char *fmt, ...);
int irc_win_open(struct irc *s, const char *name, int kind);   /* the window's number (existing or new), -1 full */
int irc_win_find(const struct irc *s, const char *name);
void irc_win_close(struct irc *s, int w);
void irc_select(struct irc *s, int w);          /* the current window (its activity cleared) */
/* the nth (0-based) member of window w whose nick starts with the n bytes at p (any case), or 0 */
const char *irc_nick_match(const struct irc *s, int w, const char *p, int n, int nth);
int irc_casecmp(const char *a, const char *b, int n);   /* RFC 1459's case: [ ] \ ~ are { } | ^; n < 0: whole */
int irc_utf8(const char *in, int n, char *out, int cap);   /* to clean UTF-8 (ISO-8859-1 if not UTF-8): its length */
int irc_b64(const unsigned char *in, int n, char *out);    /* base64 into out (4 * ((n + 2) / 3) + 1 bytes) */

#endif
