/* SPDX-License-Identifier: GPL-2.0-only */
/* A script's #! line (DESIGN D53, docs/d53-shebang.md section 3): no dependencies, so tools/shebang-host-test.c
 * includes it too. */
#ifndef ZENIX_SHEBANG_H
#define ZENIX_SHEBANG_H

#define SHEBANG_MAX 128         /* bytes read from the file; the line (#! included, the newline not) is at most 127 */

/* B (SHEBANG_MAX bytes) holds the file's first bytes, the rest of it zeros. The line ends at a newline, a 0 byte (so
 * also at the end of a shorter file) or not within SHEBANG_MAX bytes (refused). It is cut in place: *INTERP the
 * interpreter's path, *ARG the one optional argument (spaces inside it kept, the ones at its end removed) or 0.
 * 0, or why the line is refused. The file may have changed since its first bytes were looked at: "#!" is checked. */
static inline const char *shebang_parse(char *b, char **interp, char **arg)
{
    if (b[0] != '#' || b[1] != '!')
        return "changed while starting";
    unsigned i = 2;
    while (i < SHEBANG_MAX && b[i] != '\n' && b[i])
        i++;
    if (i >= SHEBANG_MAX)
        return "#! line over 127 bytes";
    b[i] = 0;
    char *p = b + 2;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p)
        return "#! line without an interpreter";
    *interp = p;
    while (*p && *p != ' ' && *p != '\t')
        p++;
    if (*p)
        *p++ = 0;
    while (*p == ' ' || *p == '\t')
        p++;
    char *e = p;
    while (*e)
        e++;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    *e = 0;
    *arg = *p ? p : 0;
    return 0;
}

#endif
