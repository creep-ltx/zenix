/* Copyright (c) 2026 Tobias Karlsson. MIT License: ports/ahiru/upstream/LICENSE (this notice is Zenix's copy's) */
/* SPDX-License-Identifier: MIT */
/* util - more of the core: things CFile needs that know nothing about
 * the Amiga. Bounded formatting, path strings, AmigaDOS-style patterns,
 * dates. Builds on the host and under vamos (tests/coretest.c). */
#ifndef CF_UTIL_H
#define CF_UTIL_H

/* fmt() with a size: never writes more than size bytes, NUL included.
 * Returns the length written (at most size-1). Same conversions as
 * fmt: flags '-' '0', width, precision, 'l', d i u x X c s %. */
int sfmt(char *dst, int size, const char *f, ...);

/* ---- paths: AmigaDOS shapes ("Work:", "Work:a/b", "a/b") ---------
 * addpart joins name onto path (a '/' unless path ends in ':' or '/'
 * or is empty). FALSE, and path untouched, when it would not fit in
 * size bytes - a walker that went on would act on the PARENT. */
int addpart(char *path, const char *name, int size);
/* the last component: after the last '/' or ':' */
const char *filepart(const char *path);
/* length of the directory part: up to the last '/', or just past the
 * ':' when there is no '/' (the V36 PathPart contract) */
int pathlen(const char *path);
/* the parent: cut the last component off in place. Returns the cut
 * name's start in the ORIGINAL string (copy it first if needed), or
 * -1 at a device root ("Work:") or for "". */
int pathup(char *path);
/* TRUE when path is a device/volume root ("Work:", "DH0:") */
int isroot(const char *path);

/* ---- patterns ------------------------------------------------------
 * Whole-name, case-insensitive. '#?' and '*' match any run, '?' one
 * character, '#x' any number of x; everything else is literal. The
 * same answer on every Kickstart (1.3 has no ParsePattern). */
int pmatch(const char *pat, const char *name);
/* TRUE when s holds a pattern character (#, ?, *) */
int haswild(const char *s);

/* ---- dates: a DOS datestamp counts days from 1.1.1978 ------------- */
void dsplit(long days, int *y, int *m, int *d);
/* "DD-Mon-YY" */
void datestr(long days, char *dst);
/* "DDMon" - the compact form for a narrow column */
void ddmon(long days, char *dst);
/* "HH:MM" */
void timestr(long mins, char *dst);

#endif
