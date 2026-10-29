/* SPDX-License-Identifier: GPL-2.0-only */
/* the size table: the UTF-8 functions (setlocale, mbrtowc, wcwidth) */
#define _GNU_SOURCE
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
int main(int argc, char **argv)
{
    const char *s = argc > 1 ? argv[1] : "\xc3\xa5\xe4\xb8\x80";
    mbstate_t st; wchar_t w; size_t n, len = strlen(s); int cols = 0;
    setlocale(LC_ALL, "C.UTF-8");
    memset(&st, 0, sizeof st);
    while (len && (n = mbrtowc(&w, s, len, &st)) > 0 && n <= len) { cols += wcwidth(w); s += n; len -= n; }
    printf("%d\n", cols);
    return 0;
}
