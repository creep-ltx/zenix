/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* sums - MD5 and the checksum files (.md5, .sfv). Plain C, tested on the
 * host against md5sum and cksum (tests/sumtest.c).
 *
 * MD5 is fed in pieces: md5_init, md5_add as often as needed, md5_end
 * gives the 16 bytes. The CRC-32 is arc_crc32 (core/arc.h), the one zip
 * and SFV use.
 *
 * A checksum file has one file a line. md5sum's shape: 32 hex digits,
 * two spaces (or " *"), the name. SFV's: the name, a space, 8 hex
 * digits; lines starting with ';' are comments. sum_parse reads one
 * line of either: 1 and the name and the expected value (lower case
 * hex) when it is one, 0 for a comment or an empty line, -1 when it
 * makes no sense. */
#ifndef CF_SUMS_H
#define CF_SUMS_H

struct md5 {
    unsigned long a, b, c, d;
    unsigned long lo, hi;           /* the length in bytes */
    unsigned char buf[64];
};

void md5_init(struct md5 *m);
void md5_add(struct md5 *m, const void *data, long n);
void md5_end(struct md5 *m, unsigned char out[16]);
void sum_hex(const unsigned char *b, int n, char *out);   /* out: 2n+1 */

#define SUM_MD5 1
#define SUM_SFV 2
/* line is changed (cut up); name and want point into it. kind is what
 * the file is (by its ending: .md5 or .sfv) */
int sum_parse(char *line, int kind, char **name, char **want);

#endif
