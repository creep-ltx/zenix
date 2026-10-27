/* SPDX-License-Identifier: GPL-2.0-only */
/* ssh's core, inside (net/ssh/): the message numbers, the wire readers and writers, the transport's calls for the
   authentication and channel layers. Not for callers: they use ssh.h. */
#ifndef SSH_INT_H
#define SSH_INT_H

#include "ssh.h"

/* RFC 4250 4.1 */
#define M_DISCONNECT 1
#define M_IGNORE 2
#define M_UNIMPLEMENTED 3
#define M_DEBUG 4
#define M_SERVICE_REQUEST 5
#define M_SERVICE_ACCEPT 6
#define M_EXT_INFO 7
#define M_KEXINIT 20
#define M_NEWKEYS 21
#define M_KEX_ECDH_INIT 30
#define M_KEX_ECDH_REPLY 31
#define M_USERAUTH_REQUEST 50
#define M_USERAUTH_FAILURE 51
#define M_USERAUTH_SUCCESS 52
#define M_USERAUTH_BANNER 53
#define M_USERAUTH_PK_OK 60             /* also PASSWD_CHANGEREQ and INFO_REQUEST: by the method in flight */
#define M_USERAUTH_INFO_RESPONSE 61
#define M_GLOBAL_REQUEST 80
#define M_REQUEST_SUCCESS 81
#define M_REQUEST_FAILURE 82
#define M_CHANNEL_OPEN 90
#define M_CHANNEL_OPEN_CONFIRMATION 91
#define M_CHANNEL_OPEN_FAILURE 92
#define M_CHANNEL_WINDOW_ADJUST 93
#define M_CHANNEL_DATA 94
#define M_CHANNEL_EXTENDED_DATA 95
#define M_CHANNEL_EOF 96
#define M_CHANNEL_CLOSE 97
#define M_CHANNEL_REQUEST 98
#define M_CHANNEL_SUCCESS 99
#define M_CHANNEL_FAILURE 100

/* a reader over untrusted bytes: every get checks the length; bad sticks */
struct rd {
    const unsigned char *p;
    long n;
    int bad;
};
unsigned rd_u8(struct rd *r);
unsigned long rd_u32(struct rd *r);
const unsigned char *rd_str(struct rd *r, long *len);  /* a string's bytes (not NUL-terminated), 0 if bad */
int rd_bool(struct rd *r);

/* a writer into the send buffer (struct ssh_wr): bad sticks when it runs out of room */
#define wr ssh_wr
void wr_u8(struct wr *w, unsigned v);
void wr_u32(struct wr *w, unsigned long v);
void wr_bytes(struct wr *w, const void *b, long n);
void wr_str(struct wr *w, const void *b, long n);
void wr_cstr(struct wr *w, const char *s);

/* name-lists */
int nl_has(const unsigned char *list, long n, const char *name);

/* the transport (transport.c) */
struct wr *pkt_begin(struct ssh *s, unsigned type);     /* the payload's writer; 0 if the session failed */
void pkt_send(struct ssh *s);                           /* pad, encrypt, send */
void ssh_fail(struct ssh *s, const char *why);
void ssh_failf(struct ssh *s, const char *what, const unsigned char *srv, long n);  /* what: cleaned server text */
void ssh_say(struct ssh *s, int kind, const char *text);

/* the layers above (auth.c, channel.c) */
void auth_begin(struct ssh *s);                         /* after the first NEWKEYS both ways */
int auth_packet(struct ssh *s, unsigned type, struct rd *r);    /* 1 handled, 0 not this layer's */
void chan_begin(struct ssh *s);                         /* after USERAUTH_SUCCESS */
int chan_packet(struct ssh *s, unsigned type, struct rd *r);
void chan_after_kex(struct ssh *s);                     /* window adjusts held back during a rekey */

#endif
