#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# The development package's files (DESIGN D52 section 2, "A development package"; docs/man/zld.txt), staged in
# BUILD/devel as they lie on Zenix, for tools/mkzup.py's family "devel" (which also carries /bin/zld):
#   /usr/lib/libc.a, libc-pure.a   the C library for plain and shared-code programs, debug sections stripped
#   /usr/lib/libgcc.a              gcc's helpers (the 68060 multilib, soft float: what the Makefile links), stripped
#   /usr/lib/crt0.o, crt0-pure.o   the start-up code (ZEXE, ZEX2)
#   /usr/lib/zenix/user.ld, pure.ld  the two layouts, for reference (zld has them built in)
#   /usr/include                   the port's headers (libc/zenix/include), then under /usr/include/pico the rest of
#                                  the Makefile's LIBC_INC in its order: gen (the generated constants), m68k, stdio
#                                  (tinystdio), locale, picolibc.h itself, include (newlib's), libm
# Usage: tools/mkdevel.sh BUILD LIBGCC   (TOOLPREFIX from the environment, as the Makefile exports it)
set -e
B=$1
LIBGCC=$2
P=${TOOLPREFIX:-m68k-elf-}
D=$B/devel
rm -rf "$D.new"
mkdir -p "$D.new/usr/lib/zenix" "$D.new/usr/include/pico"
for f in libc.a libc-pure.a crt0.o crt0-pure.o; do
    "${P}objcopy" --strip-debug "$B/$f" "$D.new/usr/lib/$f"
done
"${P}objcopy" --strip-debug "$LIBGCC" "$D.new/usr/lib/libgcc.a"
cp link/user.ld link/pure.ld "$D.new/usr/lib/zenix/"
# a header tree, its directories as they are (the headers include each other by these paths)
tree() {
    (cd "$1" && find . -name '*.h' -type f) | sort | while read -r h; do
        mkdir -p "$(dirname "$2/$h")"
        cp "$1/$h" "$2/$h"
    done
}
tree libc/zenix/include "$D.new/usr/include"
mkdir -p "$D.new/usr/include/pico/gen" "$D.new/usr/include/pico/locale" "$D.new/usr/include/pico/libm"
cp "$B/libc-inc/zenix-consts.h" "$D.new/usr/include/pico/gen/"
tree libc/picolibc/newlib/libc/machine/m68k "$D.new/usr/include/pico/m68k"
tree libc/picolibc/newlib/libc/tinystdio "$D.new/usr/include/pico/stdio"
cp libc/picolibc/newlib/libc/locale/*.h "$D.new/usr/include/pico/locale/"
cp libc/picolibc/picolibc.h "$D.new/usr/include/pico/"
tree libc/picolibc/newlib/libc/include "$D.new/usr/include/pico/include"
cp libc/picolibc/newlib/libm/common/*.h "$D.new/usr/include/pico/libm/"
rm -rf "$D"
mv "$D.new" "$D"
