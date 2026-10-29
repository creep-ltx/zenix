/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once
/*
 * The HTTP/1.1 client's core (DESIGN D22 7.4), without system calls: URLs and redirects, the GET request, the
 * response parser (status, the headers zpkg needs, Content-Length and chunked bodies) and the resume rule (7.5).
 * The socket layer (getaddrinfo through netd, poll for the timeouts, D21's TLS wrapper for https:) feeds it bytes;
 * zpkg and later wget link it. GET only; no proxies, cookies or authentication. Host-tested and fuzzed on the PC
 * (tools/host/zpkg-test.sh: http, fuzz).
 * Limits: a response header of at most 4 KB, at most 3 redirects and never from https to http, a body never longer
 * than the caller's limit (a server sending more is cut off), chunk sizes checked against overflow.
 */
#include <stddef.h>

#define HTTP_HDRMAX   4096          /* the status line and headers together (and a chunked body's trailer) */
#define HTTP_CLINEMAX 256           /* a chunk-size line, extensions included (audit 2026-10-29 B2-9, network audit O5) */
#define HTTP_HOSTMAX  255
#define HTTP_PATHMAX  1023
#define HTTP_MAXREDIR 3

/* the core's answers: >= 0 events, < 0 errors (negative errno numbers, Linux's as Zenix's) */
#define HTTP_NEED   0               /* everything given was taken; give more */
#define HTTP_HEAD   1               /* the headers are complete (status and fields are set) */
#define HTTP_DATA   2               /* *body / *blen: body bytes (pointing into the input) */
#define HTTP_END    3               /* the response is complete; the bytes after *used belong to the next one */
#define HTTP_EIO     (-5)           /* the connection ended before the response did */
#define HTTP_E2BIG   (-7)           /* a header over 4 KB, or a body longer than the limit or than announced */
#define HTTP_EINVAL  (-22)          /* a bad URL, or a request that does not fit */
#define HTTP_ELOOP   (-40)          /* more than 3 redirects */
#define HTTP_EPROTO  (-71)          /* not HTTP, or a response this client does not take */
#define HTTP_EPERM   (-1)           /* a redirect from https to http (or to a form this client does not follow) */
#define HTTP_ENOTSUP (-95)          /* a transfer coding other than chunked */

struct http_url {
    int tls;                        /* https */
    unsigned port;
    char host[HTTP_HOSTMAX + 1];
    char path[HTTP_PATHMAX + 1];
};
/* "http://HOST[:PORT][/PATH]" or https: HOST of [A-Za-z0-9.-], PATH printable without spaces */
int http_url_parse(struct http_url *u, const char *s);
/* follow a Location (an absolute http(s) URL or an absolute path) from u; *hops counts (at most 3) */
int http_redirect(struct http_url *u, const char *location, int *hops);
/* the same, and also every relative reference (RFC 3986 5.2: "x/y", "../y", "?q", "//HOST[:PORT]/PATH" with the
 * same scheme), resolved against u with "." and ".." taken out: wget's (GNU wget follows them); zpkg keeps
 * http_redirect's refusal. Another scheme (ftp:...) is HTTP_EINVAL */
int http_redirect_rel(struct http_url *u, const char *location, int *hops);
/* is the status one http_redirect follows (301, 302, 303, 307, 308)? */
int http_is_redirect(int status);
/* the GET request for u into buf (cap bytes); from > 0 asks for "bytes=FROM-"; close asks the server to close.
 * The length, or HTTP_EINVAL when it does not fit */
int http_request(char *buf, size_t cap, const struct http_url *u, unsigned long from, int close);

struct http_resp {
    /* the answer, set when HTTP_HEAD is returned */
    int status;
    int chunked;
    int close;                      /* the connection cannot carry another request */
    int has_len;
    unsigned long len;              /* Content-Length */
    int has_range;
    unsigned long range_first, range_last, range_total;   /* Content-Range bytes FIRST-LAST/TOTAL */
    char location[HTTP_HOSTMAX + HTTP_PATHMAX + 16];      /* "" when none */
    /* the parser's state */
    int st, head, minor;
    unsigned long limit, got, left;
    unsigned hbytes, llen;
    unsigned long cbytes;           /* chunk-size lines and the CRLFs after chunks: at most 8 a body byte + 4 KB (B2-9) */
    char line[HTTP_HDRMAX];
};
/* limit: the most body bytes taken (the list's size of the file); head: the request was HEAD (no body) */
void http_resp_init(struct http_resp *r, unsigned long limit, int head);
/* feed n bytes; *used: how many were taken (always the whole input for HTTP_NEED). Call again with the rest after
 * HTTP_HEAD and HTTP_DATA; HTTP_END ends the response, HTTP_E* the connection */
int http_resp_feed(struct http_resp *r, const unsigned char *in, size_t n, size_t *used, const unsigned char **body,
                   size_t *blen);
/* the connection was closed by the server: HTTP_END if that ends the body, else HTTP_EIO */
int http_resp_eof(struct http_resp *r);

/* a resumed download (7.5): the answer to "bytes=FROM-" for a file of total bytes */
#define HTTP_RESUME   1             /* 206 with the right Content-Range: append */
#define HTTP_RESTART  2             /* 200: the whole file again, from the start */
int http_resume(const struct http_resp *r, unsigned long from, unsigned long total);
