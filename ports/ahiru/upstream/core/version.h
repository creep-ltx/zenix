/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* version - what version a file says it is, as the Version command
 * reads it: the first "$VER:" string; for a library or device without
 * one, its RomTag's version byte (a RomTag has no revision).
 *
 * The program reads the file in chunks that overlap by VER_OVERLAP
 * bytes and hands each to ver_scan(); a string that starts in the last
 * VER_OVERLAP bytes is left for the next chunk, which holds it whole.
 * Plain C, no Amiga calls: tested on the host.
 */
#ifndef CF_VERSION_H
#define CF_VERSION_H

#define VER_OVERLAP 80

struct verinfo {
    int found;                  /* 0 none, 1 a $VER string, 2 a RomTag's version */
    int ver, rev;               /* rev -1: none given */
    long days;                  /* the build date in the string, AmigaDOS days; -1 none */
    char text[64];              /* what follows "$VER: " (for showing) */
};

/* one chunk; last = it ends at the end of the file. 1 = found (stop) */
int ver_scan(const unsigned char *b, long n, int last, struct verinfo *v);

/* the file's first chunk: a RomTag in its first code hunk (a library,
 * a device, a resource) - its version byte. 1 = found */
int ver_romtag(const unsigned char *b, long n, struct verinfo *v);

/* "name 40.2 (date)" as it follows "$VER:" - the first number.number */
void ver_parse(const char *s, int n, struct verinfo *v);

/* <0 a is older, 0 the same or not comparable, >0 a is newer */
int ver_cmp(const struct verinfo *a, const struct verinfo *b);

/* "40.2", "40" or ""; with the build date: "40.2 (24-Apr-10)" */
void ver_str(const struct verinfo *v, char *dst, int size);

/* the date part of a version string: "(24.4.2010)", "(24.4.10)",
 * "(2026-07-31)", "Jan  3 2011" - as AmigaDOS days, -1 none */
long ver_date(const char *s, int n);

#endif
