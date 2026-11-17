/* SPDX-License-Identifier: GPL-2.0-only */
/* mail's core (DESIGN D59, docs/man/mail.txt): the e-mail protocols (IMAP4rev1, POP3, SMTP) over a connection the
 * program gives (struct mconn: plain or TLS, the program's business), a message read (MIME: multipart, quoted-
 * printable and base64, ISO-8859-1/-15, Windows-1252 and UTF-8, HTML made text, attachments), a message written (the
 * headers, RFC 2047 words, quoted-printable) and the settings. No system calls: shared by the Zenix program
 * (user/mail.c: the screen, the keys, the socket, the TLS helper) and the PC's tests (tools/host/mail-client.c,
 * mail-core-test.c). Everything from a server or in a message is untrusted: lengths and counts are checked, lines over
 * MAIL_LINEMAX are cut, and the text given to the program is valid UTF-8 with no control character but the newline
 * (no escape sequence from a message ever reaches the console). */
#ifndef MAIL_H
#define MAIL_H

#ifdef MAIL_HOST
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned int u32;
#else
#include "../../user/libz.h"
#endif

#define MAIL_VERSION   "0.1b1"
#define MAIL_LINEMAX   8192            /* a protocol line kept (the rest of a longer one read and dropped) */
#define MAIL_LITMAX    (16u << 20)     /* a literal or message claimed bigger: a protocol error, the connection ends */
#define MAIL_MSGMAX    (512u << 10)    /* a message read whole up to this; a bigger one only its first part */
#define MAIL_HDRMAX    2048            /* the index's header fields kept per message */
#define MAIL_TEXTMAX   (1u << 20)      /* a message's text as shown (decoded), at most */
#define MAIL_PARTMAX   32              /* a message's parts listed */
#define MAIL_DEPTH     8               /* multipart inside multipart */
#define MAIL_BOXMAX    200             /* folders listed */
#define MAIL_RCPTMAX   50              /* recipients of one message */
#define MAIL_CONFMAX   8192

/* ---- a growing string (every allocation checked; a cap that is hit sets full) ---- */
struct sb { char *p; u32 n, cap, max; int full, nomem; };
void sb_init(struct sb *b, u32 max);
void sb_put(struct sb *b, const char *s, u32 n);
void sb_puts(struct sb *b, const char *s);
void sb_putc(struct sb *b, int c);
void sb_printf(struct sb *b, const char *fmt, ...);
void sb_u8(struct sb *b, u32 cp);      /* a code point as UTF-8 */
char *sb_take(struct sb *b);           /* the text, 0-terminated (the caller frees; 0 when out of memory) */
void sb_free(struct sb *b);

/* ---- the settings: ~/.config/mail/config, one `SETTING VALUE` a line, # comments (docs/man/mail.txt) ---- */
enum { TLS_OFF, TLS_ON, TLS_STARTTLS };
struct mail_cfg {
    char *imap, *pop, *user, *pass, *passcmd, *smtp, *smtp_user, *smtp_pass, *smtp_passcmd;
    char *from, *sent, *editor, *folder;
    int port, tls, smtp_port, smtp_tls, limit, plain_auth, has_pass;   /* has_pass: a password in the file */
    char *text;                         /* the file's text (the strings point into it) */
    u32 textn;
};
/* 0, or -1 with the line's number and the reason in err; text is kept (mail_cfg_free) */
int mail_config(struct mail_cfg *c, char *text, char *err, int errn);
void mail_cfg_free(struct mail_cfg *c);

/* ---- a connection: the program's (plain socket or TLS); the core reads lines and literals through it ---- */
struct mconn {
    void *u;
    int (*rd)(void *u, char *b, int n);         /* bytes (> 0), 0 the end, < 0 an error or no answer in time */
    int (*wr)(void *u, const char *b, int n);   /* all n bytes: 0, or < 0 */
    int (*starttls)(void *u, char *err, int errn);   /* TLS from here on: 0, or < 0 with a reason */
    void (*trace)(void *u, int out, const char *l, int n);   /* every line both ways, a password's line hidden (may be 0) */
    char b[2048];
    int bp, bn, dead;
    int cut;                                    /* the last line was longer than the caller's room */
};
int mc_line(struct mconn *c, char *out, int cap);   /* a line without CR LF: its length, < 0 the connection is gone */
int mc_read(struct mconn *c, char *out, u32 n);     /* exactly n bytes (out may be 0: read and dropped) */
int mc_send(struct mconn *c, const char *fmt, ...);  /* a line (CR LF added): 0 or < 0 */

/* ---- reading mail: an IMAP or a POP3 session ---- */
enum { MF_SEEN = 1, MF_DELETED = 2, MF_ANSWERED = 4, MF_ATTACH = 8, MF_FLAGGED = 16 };
struct mitem {                          /* a message in the index */
    u32 uid, seq, size, date;           /* date: seconds since 1970, UTC (0: not known) */
    unsigned char flags;
    char *from, *subj;                  /* clean UTF-8, one allocation (from) */
};
struct mbox { char *name, *shown; };    /* the server's name (modified UTF-7) and as shown (UTF-8), one allocation */
enum { ME_NET = -1, ME_NO = -2, ME_PROTO = -3, ME_MEM = -4 };
struct msess {
    struct mconn *c;
    int pop, tag, caps;                 /* caps: CAP_* the server said */
    char err[200];                      /* the last failure, said */
    struct mbox *box;
    int nbox;
    struct mitem *m;                    /* newest first */
    int nm, exists;
    char cur[256];                      /* the folder selected (the server's name) */
    void *u;
    void (*progress)(void *u, int done, int total);   /* while the index comes (may be 0) */
};
#define CAP_STARTTLS 1
#define CAP_LOGINDIS 2
/* the greeting, STARTTLS when asked, the login: 0, or ME_* with s->err */
int ms_login(struct msess *s, int tls, const char *user, const char *pass, int plain_ok);
int ms_folders(struct msess *s);                    /* s->box: the folders that can be opened */
int ms_select(struct msess *s, const char *name, int limit);   /* the folder, and the index of its newest messages */
/* message i of the index, whole up to MAIL_MSGMAX (*cut: 1 when only its first part came); *raw malloc'd */
int ms_fetch(struct msess *s, int i, char **raw, u32 *n, int *cut);
int ms_flag(struct msess *s, int i, int flag, int on);   /* \Seen \Deleted \Answered on or off (POP3: \Deleted) */
int ms_copy(struct msess *s, int i, const char *folder);
int ms_expunge(struct msess *s);                    /* the deleted messages gone (POP3: at the end) */
int ms_append(struct msess *s, const char *folder, const char *msg, u32 n);   /* a copy of a sent message */
void ms_logout(struct msess *s);
void ms_free(struct msess *s);
int ms_findbox(const struct msess *s, const char *shown);   /* by its shown or its server name: index, -1 */
const char *imap_quote(char *out, int cap, const char *s);  /* an IMAP string ("..."; 0 when it needs a literal) */
int mutf7_decode(const char *in, char *out, int cap);       /* a folder's name as UTF-8: its length, -1 bad */

/* ---- sending: SMTP (EHLO, STARTTLS, AUTH PLAIN or LOGIN, MAIL, RCPT, DATA) ---- */
int smtp_send(struct mconn *c, int tls, const char *user, const char *pass, int plain_ok, const char *from,
              char *const *rcpt, int nrcpt, const char *msg, u32 n, char *err, int errn);

/* ---- a message read ---- */
struct mpart {
    char type[48];                      /* lower case: "text/plain" */
    char charset[24];
    char name[96];                      /* the file's name (clean UTF-8, no '/'), "" none */
    int enc;                            /* ENC_* */
    const char *body;                   /* the encoded bytes, inside the raw message */
    u32 len, size;                      /* encoded bytes, decoded bytes (about) */
};
enum { ENC_7BIT, ENC_QP, ENC_B64 };
struct mmsg {
    char *from, *to, *cc, *subject, *date, *replyto, *msgid, *refs;   /* clean UTF-8 ("" when missing) */
    char *text;                         /* the text to show: valid UTF-8, '\n' its only control */
    u32 tlen;
    int html, cut;                      /* the text came from HTML; the text hit MAIL_TEXTMAX */
    struct mpart att[MAIL_PARTMAX];      /* the attachments */
    int natt, moreatt;                  /* moreatt: parts over MAIL_PARTMAX, not listed */
    const char *hdr;                    /* the raw header block (for "all headers"), hdrlen bytes */
    u32 hdrlen;
};
int mime_parse(struct mmsg *m, const char *raw, u32 n);     /* 0, or ME_MEM */
void mime_free(struct mmsg *m);
/* an attachment decoded: malloc'd bytes, *n; 0 when out of memory */
char *mime_decode_part(const struct mpart *p, u32 *n);
/* a header's value (unfolded, RFC 2047 words decoded) as clean UTF-8, malloc'd ("" when missing) */
char *mime_header(const char *hdr, u32 hlen, const char *name);
char *mime_hdecode(const char *v, u32 n);                   /* the same for a value already found */
u32 mime_date(const char *v);                               /* an RFC 5322 date -> seconds since 1970 UTC, 0 bad */
/* bytes in a charset as clean UTF-8 appended to b (an unknown charset: UTF-8 where valid, else ISO-8859-1) */
void mime_text(struct sb *b, const char *in, u32 n, const char *charset);
void mime_keep(struct sb *b, const char *in, u32 n);         /* the same as UTF-8 (else ISO-8859-1), tabs kept */
void mime_html(struct sb *b, const char *in, u32 n);        /* clean UTF-8 HTML made text */
u32 b64_decode(const char *in, u32 n, char *out);           /* out: n * 3 / 4 + 3 bytes at least */
u32 qp_decode(const char *in, u32 n, char *out, int hdr);   /* hdr: '_' a space (RFC 2047 Q) */
int b64_encode(const unsigned char *in, int n, char *out);  /* 4 * ((n + 2) / 3) + 1 bytes */

/* ---- a message written ---- */
struct maddr { char *name, *addr; };
/* addresses from a header's text ("A <a@b>, c@d"): their count (at most max), -1 a bad one (err says which) */
int addr_parse(const char *s, struct maddr *a, int max, char *buf, int bufn, char *err, int errn);
int addr_parse_some(const char *s, struct maddr *a, int max, char *buf, int bufn);   /* the good ones, the rest skipped */
/* the editor's text (headers, a blank line, the body) as the message sent: malloc'd, CR LF lines, with Date,
 * Message-ID, MIME headers, Bcc dropped; *rcpt the envelope's recipients (pointing into rbuf). 0, or -1 with err */
char *mail_build(const char *edited, u32 now, u32 rnd, u32 *n, char **rcpt, int *nrcpt, char *rbuf, int rbufn,
                 char *from, int fromn, char *err, int errn);
/* the text the editor starts with: new (m 0), a reply (all: to everyone), a forward */
enum { DRAFT_NEW, DRAFT_REPLY, DRAFT_ALL, DRAFT_FWD };
char *mail_draft(int kind, const struct mmsg *m, const char *from, const char *to);
void mail_civil(u32 t, int *y, int *mo, int *d, int *h, int *mi, int *s, int *wd);   /* UTC */
int mail_clean(const char *in, u32 n, char *out, int cap);  /* one line clean (UTF-8, no controls): its length */

#endif
