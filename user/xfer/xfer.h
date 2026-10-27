/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Tobias Karlsson. From CTerm (src/xfer.h at 1cbb967, the
 * owner's AmigaOS terminal, MIT licence: see the notice at the end of
 * zmodem.c). Zenix's changes (DESIGN D20 5): only Zmodem is kept (CTerm's
 * Xmodem, Ymodem and Hydra are later, owner question 5); wresume (a part is
 * gone on from only when it is the same file, by ZCRC), escctl (-e) and
 * xfer_crc32 added.
 *
 * xfer - file transfers with a host (Zmodem first): the protocol engine
 * knows nothing of the system. It talks to the host and to files through
 * the calls in XferIO, which Zenix's rz/sz give (standard input and output,
 * the file rules of rzrules.h) and the PC tests give (a socketpair to
 * lrzsz, stdio files).
 */
#ifndef XFER_H
#define XFER_H

typedef struct XferIO XferIO;
struct XferIO {
    void *u;
    /* Up to max bytes from the host, waiting at most ms: >0 bytes,
     * 0 nothing came, <0 the connection is gone. */
    long (*get)(void *u, unsigned char *buf, long max, long ms);
    /* Bytes to the host as they are (the telnet layer escapes IAC). */
    void (*put)(void *u, const unsigned char *b, long n);
    /* Nonzero: the person asked to stop. */
    int (*stop)(void *u);
    /* How it goes: file name, bytes done, its size (0: not known);
     * what: 0 going, 1 file done, 2 skipped, 3 failed. */
    void (*show)(void *u, const char *name, unsigned long done,
                 unsigned long size, int what);
    /* What is still to come, when the protocol says (Zmodem's ZFILE): the
     * files and bytes left, this one's included. May be NULL. */
    void (*batch)(void *u, long files, unsigned long bytes);
    /* Files. A download is made under the name it came with (the caller
     * cleans it and makes it unique, and writes the name it chose into
     * made); the handle is opaque; NULL: not taken (skipped). size: what is
     * coming (0: not known). have: NULL when the protocol cannot go on from
     * a part, else where it goes on - the bytes a part of the same file from
     * an earlier try already holds (0: from the start). Writes go after
     * those. */
    void *(*wopen)(void *u, const char *name, unsigned long size,
                   char *made, int madelen, unsigned long *have);
    long (*wwrite)(void *u, void *f, const unsigned char *b, long n);
    /* ok: the file is complete. Not ok: it is kept as a part that a
     * later try may go on from ("name.part"). */
    void (*wclose)(void *u, void *f, int ok);
    /* (Zenix) wopen gave a part (*have > 0): the engine asked the sender
     * for the CRC-32 of its first *have bytes (ZCRC). answered: 1 and its
     * crc, or 0 when the sender did not answer. Returns the file to write:
     * f itself to go on from *have (the part is the same file), or another
     * one from the start (*have = 0, made the new name; f is the caller's
     * to close), or NULL (skipped). NULL in XferIO: the part is gone on
     * from without asking (CTerm's way). */
    void *(*wresume)(void *u, void *f, int answered, unsigned long crc,
                     char *made, int madelen, unsigned long *have);
    /* An upload: open by the path the person chose; its size, its name. */
    void *(*ropen)(void *u, const char *path, unsigned long *size);
    long (*rread)(void *u, void *f, unsigned char *b, long n);
    int (*rseek)(void *u, void *f, unsigned long pos);
    void (*rclose)(void *u, void *f);
    /* At the end: bytes the protocol read from the host but did not use
     * (what the host sent after the session, a BBS prompt): for the
     * screen. May be called more than once, in order; may be NULL. */
    void (*rest)(void *u, const unsigned char *b, long n);
    /* (Zenix) 1: every control character escaped, as lrzsz's -e - a
     * receiver asks the sender for it in ZRINIT (ESCCTL), a sender does it
     * whatever the receiver says. For a telnet line without BINARY, where
     * CR NUL is folded to CR (DESIGN D20 5). */
    int escctl;
};

/* The start of a Zmodem session in what the host sent: 1 the host sends
 * (ZRQINIT: we receive), 2 the host receives (ZRINIT: we send), 0 none.
 * Looks for "**" ZDLE "B0" and the type; *at is where it begins. */
int zm_detect(const unsigned char *b, long n, long *at);

/* Receive what the host sends; done: the bytes seen since the start
 * header (they are fed in first). Returns the files received, -1 when
 * it was cancelled or failed. */
int zm_receive(XferIO *io, const unsigned char *done, long n);

/* Send files (paths) to a host whose receiver is waiting: its ZRINIT may
 * be in done. Returns the files sent, -1 cancelled or failed. */
int zm_send(XferIO *io, const char *const *paths, int npaths,
            const unsigned char *done, long n);

/* Zmodem's CRC-32 (IEEE, as ZCRC's answer): start with crc 0xFFFFFFFF,
 * feed the bytes, and the answer is the result's complement (~crc). */
unsigned long xfer_crc32(unsigned long crc, const unsigned char *b, long n);

/* A start split across two reads (detect.c): tail is SPLIT_TAIL + 1
 * bytes the caller keeps per connection, zeroed at first. Returns the
 * kind (1, 2) of a start that began in the read before and ends in b;
 * *had: how many of its bytes that read had. */
#define SPLIT_TAIL 5                    /* Zmodem's 6-byte start less one */
int split_detect(unsigned char *tail, const unsigned char *b, long n,
                 int *had);

#endif
