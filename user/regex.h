#pragma once
/* POSIX extended regular expressions (regex.c, a Pike VM: no pattern can make it slow). */
#define RE_GROUPS 10
#define RE_ICASE 1               /* upper and lower case the same */
#define RE_LONGEST 2             /* POSIX: the longest of the leftmost matches */
#define RE_MULTILINE 4           /* ^ and $ also next to a newline in the text */
void *re_comp(const char *re, int flags);
/* from: where the search starts; caps[2*i], caps[2*i+1] = start, end of group i (RE_GROUPS of them); returns 1 on a match */
int re_exec(void *prog, const char *s, int len, int from, int *caps);
void re_free(void *prog);
