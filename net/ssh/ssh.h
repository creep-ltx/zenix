/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ssh's core (DESIGN D21 4, docs/d21-crypto-ssh-https.md): the client side of the SSH protocol (RFC 4250-4254,
 * RFC 8308, RFC 8731, OpenSSH's strict key exchange and chacha20-poly1305@openssh.com) with step 3's algorithms
 * (3.3): curve25519-sha256, ssh-ed25519 host keys, chacha20-poly1305@openssh.com, publickey (Ed25519),
 * keyboard-interactive and password logins, one session channel; and the parsers ssh and ssh-keygen need: OpenSSH
 * key files (openssh-key-v1, also passphrase-protected), public key lines and fingerprints, known_hosts, the
 * ~/.ssh/config subset, and the cleaner for text from the server.
 *
 * Plain C without system calls and without memory of its own (the caller holds the struct and the buffers), as
 * net/telnet/: the same files build for Zenix's ssh (D21 10 step 3) and for the PC tests and fuzzing
 * (tools/host/crypto-test.sh, tools/host/ssh-client.c). Bytes leave through the caller's callbacks only.
 *
 * Use: fill the caller's fields, ssh_start(); then feed every byte from the socket to ssh_input() (it takes what it
 * can and says how many) and look at need and state after each call: answer SSH_NEED_HOSTKEY with ssh_hostkey(),
 * SSH_NEED_PASSWORD with ssh_password(), SSH_NEED_KBDINT with ssh_kbdint(); in SSH_ST_SESSION send keys with
 * ssh_write(). Data from the session arrives through the data callback. ssh_tick() with the time in seconds starts
 * the client's rekeying after an hour.
 */
#ifndef SSH_H
#define SSH_H

#include "../../crypto/zc/zc.h"
#include "../../crypto/bearssl/inc/bearssl_hash.h"

#define SSH_PKTMAX     35000            /* RFC 4253 6.1: a longer packet ends the connection */
#define SSH_RBUF_MIN   (18 * 1024)      /* D21 7.2: the receive buffer starts here, grows to SSH_RBUF_FULL */
#define SSH_RBUF_FULL  (SSH_PKTMAX + 4 + ZC_CHAPOL_TAG + 256)
#define SSH_SBUF_MIN   (8 * 1024)       /* the send buffer */
#define SSH_WINDOW     (64 * 1024)      /* our receive window (4.2) */
#define SSH_MAXPKT     (16 * 1024)      /* our maximum channel packet */
#define SSH_LINEMAX    255              /* a version line or preamble line, without CR LF */
#define SSH_PREAMBLE_MAX 1024           /* lines before the version line (OpenSSH's SSH_MAX_PRE_BANNER_LINES) */
#define SSH_HOSTKEY_MAX 64              /* an ssh-ed25519 key blob is 51 bytes */
#define SSH_KBD_MAX    2048             /* keyboard-interactive's name, instruction and prompts, cleaned */
#define SSH_KBD_PROMPTS 8               /* more prompts than this in one request: the method is given up */
#define SSH_REKEY_BYTES (1UL << 30)     /* the client rekeys after 1 GB either way ... */
#define SSH_REKEY_SECS  3600UL          /* ... or an hour (4.2) */

/* state */
#define SSH_ST_VERSION 0                /* waiting for the server's version line */
#define SSH_ST_KEX     1                /* the first key exchange */
#define SSH_ST_AUTH    2                /* user authentication */
#define SSH_ST_OPEN    3                /* opening the session channel, pty-req, shell or exec */
#define SSH_ST_SESSION 4                /* running: ssh_write, data through the callback */
#define SSH_ST_CLOSED  5                /* the channel closed; exit_status holds the remote command's status */
#define SSH_ST_FAILED  6                /* err says why; nothing more is sent */

/* need: an answer the caller gives before anything moves on (ssh_input takes no bytes meanwhile) */
#define SSH_NEED_NONE     0
#define SSH_NEED_HOSTKEY  1             /* hostkey/hostkeylen hold the verified host key: ssh_hostkey(s, yes) */
#define SSH_NEED_PASSWORD 2             /* ssh_password(s, password), or 0 to give the method up */
#define SSH_NEED_KBDINT   3             /* kbd_*: ssh_kbdint(s, answers), or 0 to give the method up */

/* the methods the caller allows (methods) */
#define SSH_AUTH_PUBKEY   1
#define SSH_AUTH_KBDINT   2
#define SSH_AUTH_PASSWORD 4

/* the text callback's kinds (all of it cleaned by ssh_clean before the call) */
#define SSH_TEXT_PREAMBLE   1           /* a line the server sent before its version line */
#define SSH_TEXT_BANNER     2           /* SSH_MSG_USERAUTH_BANNER */
#define SSH_TEXT_DEBUG      3           /* SSH_MSG_DEBUG: shown with -v, or always with SSH_TEXT_ALWAYS or'ed in */
#define SSH_TEXT_ALWAYS     0x100       /* (the server's always_display flag) */
#define SSH_TEXT_DISCONNECT 4           /* the server's reason, before SSH_ST_FAILED */
#define SSH_TEXT_PROGRESS   5           /* our own steps for -v ("key exchange", "host key ok", ...) */
#define SSH_TEXT_STDERR     6           /* (not used by the core; for callers that print EXTENDED_DATA apart) */

/* the packet writer (inside the struct so the core keeps no memory of its own) */
struct ssh_wr {
    unsigned char *p;
    long n, cap;
    int bad;
};

struct ssh {
    /* ---- the caller's: set before ssh_start ---- */
    void *u;
    void (*send)(void *u, const unsigned char *b, long n);             /* bytes for the socket, all of them */
    void (*data)(void *u, int ext, const unsigned char *b, long n);    /* the session's output: ext 0, stderr 1 */
    void (*text)(void *u, int kind, const char *s, long n);            /* may be NULL */
    unsigned char *(*grow)(void *u, unsigned char *old, long need);    /* a bigger receive buffer with old's bytes,
                                                                          or 0; may be NULL (then rcap is all) */
    struct zc_rng *rng;                 /* seeded: padding, cookies, the ephemeral keys */
    const char *version;                /* our version line without CR LF ("SSH-2.0-..."), at most 253 bytes */
    const char *user;
    unsigned char *rbuf;                /* the receive buffer: rcap >= SSH_RBUF_MIN */
    long rcap;
    unsigned char *sbuf;                /* the send buffer: scap >= SSH_SBUF_MIN */
    long scap;
    const unsigned char *id_sk;         /* an Ed25519 key (64 bytes: seed, public half) for publickey, or 0 */
    int methods;                        /* SSH_AUTH_* allowed (publickey only when id_sk is set) */
    int pty;                            /* ask for a terminal: term, cols, rows, the modes of 4.2 */
    const char *term;
    int cols, rows;
    const char *command;                /* exec this; 0: a shell */
    unsigned long rekey_bytes;          /* 0: SSH_REKEY_BYTES */
    unsigned long rekey_secs;           /* 0: SSH_REKEY_SECS */

    /* ---- results ---- */
    int state;                          /* SSH_ST_* */
    int need;                           /* SSH_NEED_* */
    const char *err;                    /* why SSH_ST_FAILED (points at errbuf or a constant) */
    char errbuf[200];
    int exit_status;                    /* the remote command's, from exit-status; 255 after exit-signal; -1 none */
    unsigned char hostkey[SSH_HOSTKEY_MAX];     /* the server's host key blob (SSH_NEED_HOSTKEY and later) */
    int hostkeylen;
    char server_version[SSH_LINEMAX + 1];
    char auth_methods[160];             /* "authentications that can continue" from the last failure */
    char kbd_name[128], kbd_instruction[512];   /* keyboard-interactive's request, cleaned */
    char *kbd_prompt[SSH_KBD_PROMPTS];  /* point into kbd_text */
    int kbd_echo[SSH_KBD_PROMPTS];
    int kbd_n;
    char kbd_text[SSH_KBD_MAX];
    unsigned long kex_count;            /* key exchanges done (1 after the first) */
    char server_sig_algs[256];          /* EXT_INFO's server-sig-algs */

    /* ---- the core's ---- */
    long rlen;                          /* bytes held in rbuf */
    unsigned long rpkt;                 /* the current packet's length once read (0: not yet) */
    int lines;                          /* preamble lines seen */
    int strict;                         /* the server listed kex-strict-s-v00@openssh.com (always, or refused) */
    int ignore_next;                    /* first_kex_packet_follows with a wrong guess */
    int kex_ours, kex_theirs;           /* KEXINIT sent / received in this exchange */
    int kex_waitreply;                  /* ECDH_INIT sent */
    int kex_newkeys_out;                /* our NEWKEYS sent (until the server's arrives) */
    int in_kex;                         /* from our KEXINIT until our NEWKEYS: only key-exchange messages go out */
    unsigned char ikex[512];            /* our KEXINIT payload until the exchange hash has it */
    int ikexlen;
    br_sha256_context hctx;             /* the exchange hash H, fed as the parts arrive */
    unsigned char eph_sk[32], eph_pk[32];
    unsigned char hk_pk[32];            /* the first exchange's host key: a rekey must show the same */
    unsigned char sid[32];
    unsigned char kmp[37];              /* K as an mpint, from the reply until NEWKEYS */
    int kmplen;
    unsigned char h[32];
    struct zc_chapol tx, rx, rx_next;
    int tx_on, rx_on;
    unsigned long tx_seq, rx_seq;
    unsigned long kex_bytes;            /* bytes both ways since the last exchange */
    unsigned long now, kex_time;        /* seconds, from ssh_tick */
    int auth_step;                      /* the authentication sequence (auth.c) */
    int auth_tried;                     /* SSH_AUTH_* already given up */
    int ext_info_ok;                    /* EXT_INFO may still come (after NEWKEYS, before USERAUTH_SUCCESS) */
    unsigned long ch_remote, ch_rwin, ch_rmax;  /* the server's channel number, window, maximum packet */
    unsigned long ch_lwin, ch_consumed; /* our window left, bytes delivered since the last adjust */
    int ch_step;                        /* the channel's sequence (channel.c) */
    int ch_eof_out, ch_eof_in, ch_close_out, ch_close_in;
    int pend_gfail, pend_cfail;         /* request failures owed, held back during a rekey */
    int pw_tries;                       /* passwords sent (three, as OpenSSH's NumberOfPasswordPrompts) */
    struct ssh_wr w;
};

/* Set up (zeroes the core's part; the caller's fields are kept) and send our version line. */
void ssh_start(struct ssh *s);

/* Bytes from the socket. Takes what it can (nothing while need is set, or when the buffer is full of an unfinished
 * packet the buffer cannot grow for) and returns how many; the caller keeps the rest for the next call. -1 when
 * the session has failed (state SSH_ST_FAILED, err says why). */
long ssh_input(struct ssh *s, const unsigned char *b, long n);

/* The answers to need. ssh_hostkey: 1 to go on, 0 to end (state SSH_ST_FAILED). ssh_password: the password, or 0
 * to give the method up; it is copied into the packet at once and the caller wipes its own. ssh_kbdint: kbd_n
 * answers, or 0 to give the method up. */
void ssh_hostkey(struct ssh *s, int ok);
void ssh_password(struct ssh *s, const char *password);
void ssh_kbdint(struct ssh *s, const char *const *answers);

/* Keys or a command's input to the session: returns the bytes taken (0 during a key exchange or while the
 * server's window is closed; the caller keeps the rest). */
long ssh_write(struct ssh *s, const unsigned char *b, long n);
void ssh_eof(struct ssh *s);            /* no more input (CHANNEL_EOF) */
void ssh_close(struct ssh *s);          /* end the session (CHANNEL_CLOSE) */
void ssh_winsize(struct ssh *s, int cols, int rows);       /* window-change, when a pty was asked for */
void ssh_tick(struct ssh *s, unsigned long now);           /* the time in seconds: the client's rekeying */
int ssh_rekey(struct ssh *s);           /* start a key exchange now (0, or -1 when one is running or not yet) */
void ssh_keepalive(struct ssh *s);      /* keepalive@openssh.com with want-reply (ServerAliveInterval) */
void ssh_wipe(struct ssh *s);           /* wipe every key and secret the struct holds */

/* ---- text from the server (4.3) ---- */
/* Clean n bytes for the screen (dst may be src): valid UTF-8 kept, tab and newline kept, CR LF becomes LF, every
 * other control character (C0, DEL, C1) and every invalid UTF-8 byte becomes '?'. Returns the length (<= n). */
long ssh_clean(char *dst, const char *src, long n);

/* ---- keys (4.4; OpenSSH's formats) ---- */
#define SSH_KEY_OK        0
#define SSH_KEY_ENOTKEY  -1             /* not an OpenSSH private key (or not a public key line) */
#define SSH_KEY_EFORMAT  -2             /* damaged: a length, the check numbers, the padding */
#define SSH_KEY_ETYPE    -3             /* not ssh-ed25519 */
#define SSH_KEY_ENEEDPASS -4            /* protected: a passphrase is needed */
#define SSH_KEY_EBADPASS -5             /* the passphrase is wrong */
#define SSH_KEY_ECIPHER  -6             /* a cipher or key derivation other than aes256-ctr with bcrypt */
#define SSH_KEY_ESPACE   -7             /* the work or output buffer is too small */
#define SSH_KEY_EROUNDS  -8             /* bcrypt rounds outside 1..SSH_KEY_ROUNDS_MAX */
#define SSH_KEY_ROUNDS_MAX 1024         /* OpenSSH's default is 16 (minutes on a stock A1200, D21 4.4) */
#define SSH_KEY_WORK     4096           /* work space for an Ed25519 key file */

/* Read an OpenSSH private key file (text, n bytes): the Ed25519 key into sk (seed, public half), its comment into
 * comment (cap bytes, may be 0). pass may be 0. work: SSH_KEY_WORK bytes of scratch (wiped before return). */
int ssh_key_read(const char *text, long n, const char *pass, unsigned char sk[64], char *comment, long cap,
                 unsigned char *work, long worklen);
/* Write one: out gets the PEM-armoured text (returns its length, or SSH_KEY_ESPACE). pass 0 or "": unprotected;
 * else aes256-ctr with bcrypt_pbkdf over salt (16 random bytes) and rounds. check: 4 random bytes. */
long ssh_key_write(char *out, long cap, const unsigned char sk[64], const char *comment, const char *pass,
                   const unsigned char salt[16], unsigned rounds, const unsigned char check[4], unsigned char *work,
                   long worklen);
/* The public half as a key blob (string "ssh-ed25519", string key): 51 bytes */
#define SSH_ED25519_BLOB 51
void ssh_key_blob(unsigned char blob[SSH_ED25519_BLOB], const unsigned char pub[32]);
/* "ssh-ed25519 AAAA... comment" (no newline); returns the length or SSH_KEY_ESPACE */
long ssh_pubkey_line(char *out, long cap, const unsigned char *blob, long bloblen, const char *comment);
/* A public key line (authorized_keys, .pub): the blob into blob (cap bytes); returns its length or an error */
long ssh_pubkey_parse(const char *line, long n, unsigned char *blob, long cap);
/* "SHA256:" and the unpadded base64 of the blob's SHA-256 (51 bytes with the NUL) */
#define SSH_FP_LEN 51
void ssh_fingerprint(char out[SSH_FP_LEN], const unsigned char *blob, long bloblen);

/* ---- known_hosts (4.4) ---- */
#define SSH_KH_UNKNOWN   0              /* not known: ask (trust on first use) */
#define SSH_KH_OK        1              /* known with this key */
#define SSH_KH_CHANGED   2              /* known with another key of the same type: refuse, name the line */
#define SSH_KH_REVOKED   3              /* this key is @revoked: refuse */
#define SSH_KH_OTHERTYPE 4              /* known only with keys of other types (OpenSSH asks, with a warning) */
/* Look host (and port, 22 the default) up in a known_hosts file's text; *line gets the deciding line (1-based). */
int ssh_kh_check(const char *text, long n, const char *host, int port, const unsigned char *blob, long bloblen,
                 long *line);
/* The line to append for a newly trusted host (plain name, "[host]:port" for other ports), with a newline;
 * returns its length or SSH_KEY_ESPACE */
long ssh_kh_line(char *out, long cap, const char *host, int port, const unsigned char *blob, long bloblen);

/* ---- ~/.ssh/config, the subset of 4.4 (Host, HostName, User, Port, IdentityFile, ServerAliveInterval) ---- */
struct ssh_cfg {
    char hostname[256];
    char user[64];
    int port;                           /* 0: not given */
    char identity[256];
    int alive;                          /* ServerAliveInterval in seconds, -1: not given */
};
/* Fill c for host from the config text (OpenSSH's rule: the first value given wins); report (may be NULL) is
 * called for each unknown or unsupported keyword and each bad value, with its line. Returns the count of those. */
int ssh_config(const char *text, long n, const char *host, struct ssh_cfg *c,
               void (*report)(void *u, long line, const char *what), void *u);

/* ---- base64 (shared by the parsers) ---- */
long ssh_b64_decode(unsigned char *out, long cap, const char *in, long n);    /* -1 on a bad character or room */
long ssh_b64_encode(char *out, long cap, const unsigned char *in, long n, int pad);   /* -1 on room; no NUL */

#endif
