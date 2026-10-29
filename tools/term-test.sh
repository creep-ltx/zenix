#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host test of the terminal (kernel/term.c) against a model of the display: tools/term-test.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p build
python3 tools/mkconfont.py widths build/conwidth.h >/dev/null     # DESIGN D28: the kernel's width table
python3 tools/mkconfont.py font build/zenix-8x8.zcf >/dev/null    # ... and the default font (CON_UFONT's checks)
cc -Ibuild -Wall -Wextra -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -O1 -g -fsanitize=address,undefined -o build/term-test tools/term-test.c
build/term-test
# DESIGN D28 9: the kernel's widths against picolibc's own wcwidth.c and tables, compiled here with a stand-in for its
# locale part (the non-CJK locale Zenix has)
W=build/cwtest
rm -rf "$W"; mkdir -p "$W"
cp libc/picolibc/newlib/libc/string/wcwidth.c libc/picolibc/newlib/libc/string/*.t "$W"/
printf '#include <wchar.h>\nint __wcwidth(wint_t);\nstatic inline int __locale_cjk_lang(void) { return 0; }\n' > "$W/local.h"
cc -O1 -D__MB_CAPABLE -Dwcwidth=pico_wcwidth -c "$W/wcwidth.c" -o "$W/wcwidth.o"
cc -Wall -Wextra -Ibuild -O1 -o build/conwidth-test tools/conwidth-test.c "$W/wcwidth.o"
build/conwidth-test
