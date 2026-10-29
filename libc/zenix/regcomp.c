/* SPDX-License-Identifier: GPL-2.0-only */
/* POSIX regular expressions (DESIGN D27 section 5): regcomp, regexec, regerror and regfree, a thin wrapper over
 * user/regex.c (the tree's engine, MIT, from CUtils: a Pike VM, so no pattern can make it slow), so Zenix keeps one
 * engine. A basic expression (no REG_EXTENDED) is translated as sed's are (zhelp.c). POSIX's leftmost-longest match
 * (RE_LONGEST). Limits (docs/limits.md): at most 9 groups are reported (re_nsub counts them all), REG_NEWLINE makes ^
 * and $ match at newlines but . and [^...] still match a newline, REG_NOTEOL answers REG_INVARG; REG_NOTBOL and
 * REG_STARTEND work. */
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include "../../user/regex.h"
#include "zshare.h"

#define ZX_RE_MAGIC 0x5a524500u         /* "ZRE" + the flags regexec needs */
#define ZX_RE_NOSUB 1u

/* the groups in an extended expression: every ( outside a bracket and not after a backslash */
static size_t groups(const char *s)
{
    size_t n = 0;
    while (*s) {
        if (*s == '\\') { s += s[1] ? 2 : 1; continue; }
        if (*s == '[') {                            /* as regex.c reads a bracket: [^]...] and [:class:] */
            s++;
            if (*s == '^') s++;
            if (*s == ']') s++;
            while (*s && *s != ']') {
                if (s[0] == '[' && s[1] == ':') { const char *e = strstr(s + 2, ":]"); if (e) { s = e + 2; continue; } }
                s++;
            }
            if (*s) s++;
            continue;
        }
        if (*s == '(') n++;
        s++;
    }
    return n;
}

int regcomp(regex_t *re, const char *pattern, int cflags)
{
    if (cflags & ~(REG_EXTENDED | REG_ICASE | REG_NOSUB | REG_NEWLINE)) return REG_INVARG;
    char *ere = 0;
    const char *p = pattern;
    if (!(cflags & REG_EXTENDED)) {
        if (!(ere = zs_bre_to_ere(pattern))) return REG_ESPACE;
        p = ere;
    }
    size_t ng = groups(p);
    void *prog = re_comp(p, RE_LONGEST | (cflags & REG_ICASE ? RE_ICASE : 0) | (cflags & REG_NEWLINE ? RE_MULTILINE : 0));
    free(ere);
    if (!prog) return REG_BADPAT;
    re->re_magic = ZX_RE_MAGIC | (cflags & REG_NOSUB ? ZX_RE_NOSUB : 0);
    re->re_nsub = ng;
    re->re_endp = 0;
    re->re_g = prog;
    return 0;
}

int regexec(const regex_t *re, const char *s, size_t nmatch, regmatch_t pm[], int eflags)
{
    if ((re->re_magic & ~0xFFu) != ZX_RE_MAGIC) return REG_BADPAT;
    if (eflags & ~(REG_NOTBOL | REG_STARTEND)) return REG_INVARG;
    size_t so = 0, eo;
    if (eflags & REG_STARTEND) { so = (size_t)pm[0].rm_so; eo = (size_t)pm[0].rm_eo; }
    else eo = strlen(s);
    if (eo < so) return REG_INVARG;
    const char *t = s + so;
    int len = (int)(eo - so), from = 0, caps[2 * RE_GROUPS];
    char *copy = 0;
    if (eflags & REG_NOTBOL) {          /* the text after a byte that is no newline and no word character: ^ cannot
                                         * match at its start (regex.c's ^ is the text's first byte) */
        if (!(copy = malloc((size_t)len + 1))) return REG_ESPACE;
        copy[0] = 1;
        memcpy(copy + 1, t, (size_t)len);
        t = copy;
        len++;
        from = 1;
    }
    int r = re_exec(re->re_g, t, len, from, caps);
    free(copy);
    if (r < 0) return REG_ESPACE;
    if (!r) return REG_NOMATCH;
    if (re->re_magic & ZX_RE_NOSUB) return 0;
    for (size_t i = 0; i < nmatch; i++) {
        if (i <= re->re_nsub && i < RE_GROUPS && caps[2 * i] >= 0) {
            pm[i].rm_so = (regoff_t)(caps[2 * i] - from) + (regoff_t)so;
            pm[i].rm_eo = (regoff_t)(caps[2 * i + 1] - from) + (regoff_t)so;
        } else
            pm[i].rm_so = pm[i].rm_eo = -1;
    }
    return 0;
}

size_t regerror(int code, const regex_t *re, char *buf, size_t size)
{
    static const char *const text[] = {
        [0] = "success", [REG_NOMATCH] = "no match", [REG_BADPAT] = "invalid regular expression",
        [REG_ECOLLATE] = "invalid collating element", [REG_ECTYPE] = "invalid character class",
        [REG_EESCAPE] = "trailing backslash", [REG_ESUBREG] = "invalid back reference",
        [REG_EBRACK] = "unmatched [", [REG_EPAREN] = "unmatched (", [REG_EBRACE] = "unmatched {",
        [REG_BADBR] = "invalid repetition count", [REG_ERANGE] = "invalid range",
        [REG_ESPACE] = "out of memory", [REG_BADRPT] = "repetition not preceded by an expression",
        [REG_EMPTY] = "empty expression", [REG_ASSERT] = "internal error", [REG_INVARG] = "invalid argument",
    };
    (void)re;
    const char *m = code >= 0 && code < (int)(sizeof text / sizeof text[0]) && text[code] ? text[code] : "unknown error";
    size_t l = strlen(m) + 1;
    if (size) {
        size_t k = l < size ? l : size;
        memcpy(buf, m, k - 1);
        buf[k - 1] = 0;
    }
    return l;
}

void regfree(regex_t *re)
{
    if ((re->re_magic & ~0xFFu) != ZX_RE_MAGIC) return;
    re_free(re->re_g);
    re->re_g = 0;
    re->re_magic = 0;
}
