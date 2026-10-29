/* SPDX-License-Identifier: GPL-2.0-only */
/* setlocale on Zenix (DESIGN D28 section 11, D27): picolibc's setlocale(category, "") answers the build's default
 * locale ("C") and never looks at the environment (locale_names.c, __find_locale), where POSIX says "" means LC_ALL,
 * then the category's own variable, then LANG. This wrapper reads them in that order (an empty value counts as not
 * set) and hands picolibc the name, with its own matching: "C" and "POSIX" give C, "C.UTF-8" and "sv_SE.UTF-8" UTF-8;
 * a name with no charset ("sv_SE") is UTF-8 here, Zenix's one character set; an unknown charset fails (NULL, as
 * POSIX allows: the program stays as it was). Nothing set at all: "C". picolibc's own function is compiled as
 * __pico_setlocale (the Makefile's LIBC_DEF_ for its file). The shell sets LANG=C.UTF-8 (sh.c). */
#include <locale.h>
#include <stdlib.h>
#include <string.h>

char *__pico_setlocale(int category, const char *name);

static const char *envv(const char *n)
{
    const char *v = getenv(n);
    return v && *v ? v : 0;
}

char *setlocale(int category, const char *name)
{
    if (name && !*name) {
        const char *v = envv("LC_ALL");
        if (!v && (category == LC_ALL || category == LC_CTYPE))
            v = envv("LC_CTYPE");
        if (!v)
            v = envv("LANG");
        if (!v)
            v = "C";
        if (strcmp(v, "C") && strcmp(v, "POSIX") && !strchr(v, '.') && !strchr(v, '-'))
            v = "C.UTF-8";              /* a language with no charset: UTF-8 */
        name = v;
    }
    return __pico_setlocale(category, name);
}
