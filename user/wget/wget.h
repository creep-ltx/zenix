/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/*
 * wget's core (DESIGN D21 8, step 7; D22 7.4-7.5), without system calls: the command line (-q, -c, -O FILE), each
 * URL's plan (http: or https:, at most 3 redirects and never from https to http, net/http's rules), the local file's
 * name in GNU wget's basic way, and what to do with each answer, the resume rule included. The program (user/wget.c)
 * owns the sockets, D21's TLS glue (crypto/zc/zc_tls.h), the files and the messages; the HTTP protocol itself is
 * net/http/'s (shared with zpkg).
 *
 * Names (GNU wget's default, without --trust-server-names): the last segment of the URL's path as given on the
 * command line (not of a redirect's), its query kept ("b?x=1"), %XX decoded unless that gives '/', a NUL or another
 * control character (kept as %XX), "index.html" for an empty segment, "." or "..". Without -O and -c an existing
 * file is not overwritten: the next free "NAME.1", "NAME.2"... is used (the program asks wget_nextname).
 * -c (resume, D22 7.5's rule through http_resume): with a file of N > 0 bytes the request asks "bytes=N-"; a 206
 * whose Content-Range is N-(T-1)/T is appended; a 200 starts the file again from its first byte; a 416 means the
 * file is already complete (GNU wget's reading). -O FILE: every URL into FILE ("-": standard output), truncated once
 * at the start (with -c: the first URL resumes FILE; "-" never resumes).
 * Exit statuses (GNU wget's): 0 done, 1 generic, 2 command line, 3 file I/O, 4 network, 5 TLS verification,
 * 7 protocol, 8 a server's error answer; with several failures the lowest of 2-8 wins, 1 only when no other.
 */
#include <stddef.h>
#include "../../net/http/http.h"

#define WGET_URLMAX  16                 /* URLs on one command line */
#define WGET_NAMEMAX 255                /* a local file name (ext2's) */

#define WGET_EXIT_OK      0
#define WGET_EXIT_GENERIC 1
#define WGET_EXIT_USAGE   2
#define WGET_EXIT_FILE    3
#define WGET_EXIT_NET     4
#define WGET_EXIT_TLS     5
#define WGET_EXIT_PROTO   7
#define WGET_EXIT_SERVER  8

struct wget_opts {
    int quiet;                          /* -q */
    int cont;                           /* -c */
    const char *output;                 /* -O FILE ("-" standard output), or null */
    int nurl;
    const char *url[WGET_URLMAX];
};
/* argv[1..argc-1]: "-q", "-c", "-O FILE", "-OFILE", combined "-qc", "--" ends the options. 0, or WGET_EXIT_USAGE
   with *why. */
int wget_args(struct wget_opts *o, int argc, char *const *argv, const char **why);

/* The local name for a URL as above, into out (cap bytes, at most WGET_NAMEMAX + 1 used): 0 or HTTP_EINVAL. */
int wget_name(char *out, size_t cap, const struct http_url *u);
/* "BASE.N" (n >= 1) into out: 0, or HTTP_EINVAL when it does not fit in WGET_NAMEMAX. */
int wget_nextname(char *out, size_t cap, const char *base, unsigned n);
/* The exit status of two failures together (GNU wget's precedence). */
int wget_exit_merge(int a, int b);

/* One URL's fetch. */
struct wget_plan {
    struct http_url url;                /* where the next request goes (changes with each redirect) */
    int hops;                           /* redirects followed */
    char name[WGET_NAMEMAX + 1];        /* the target: a file name, or "-" for standard output */
    int to_stdout;
    int need_size;                      /* the program says the target's size with wget_plan_have before the request */
    int need_free_name;                 /* the program finds a free name with wget_nextname before writing */
    int append;                         /* WGET_WRITE: 1 append to the target, 0 truncate it first */
    unsigned long from;                 /* the request's "bytes=FROM-" (0: the whole file) */
    unsigned long total;                /* the whole file's size when the answer says it (0 unknown) */
};
/* Plan URL index i of o. 0, or an exit status with *why (a bad URL: WGET_EXIT_USAGE). */
int wget_plan_start(struct wget_plan *p, const struct wget_opts *o, int i, const char **why);
/* The target's size (exists: 0 when there is no such file); with -c a size > 0 is resumed from. */
void wget_plan_have(struct wget_plan *p, int exists, unsigned long size);

#define WGET_WRITE  1                   /* the body goes into the target (p->append says how) */
#define WGET_FOLLOW 2                   /* a redirect: read the body away, then request p->url */
#define WGET_DONE   3                   /* nothing to write (416 after -c: the file is complete) */
#define WGET_FAIL   4                   /* *exitcode and *why say why; the body is not written */
/* What the response's headers (http_resp_feed gave HTTP_HEAD) mean for p. */
int wget_plan_answer(struct wget_plan *p, const struct http_resp *r, int *exitcode, const char **why);

/* Can the connection to a carry the request to b (the same scheme, host and port: keep-alive, TLS reuse)? */
int wget_same_server(const struct http_url *a, const struct http_url *b);
