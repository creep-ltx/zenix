#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# The development package's files (DESIGN D52 section 2, "A development package"; docs/man/zld.txt), staged in
# BUILD/devel as they lie on Zenix, for tools/mkzup.py's family "devel" (which also carries /bin/zld):
#   /usr/lib/libc.a, libc-pure.a   the C library for plain and shared-code programs, debug sections stripped
#   /usr/lib/libgcc.a              gcc's helpers (the 68060 multilib, soft float: what the Makefile links), stripped
#   /usr/lib/crt0.o, crt0-pure.o   the start-up code (ZEXE, ZEX2)
#   /usr/lib/zenix/user.ld, pure.ld  the two layouts, for reference (zld has them built in)
#   /usr/lib/tcc/include           tcc's own headers (ports/tcc, the Makefile's patched copy in BUILD/tcc/src)
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
    "${P}objcopy" -D --strip-debug "$B/$f" "$D.new/usr/lib/$f"
done
"${P}objcopy" -D --strip-debug "$LIBGCC" "$D.new/usr/lib/libgcc.a"
cp link/user.ld link/pure.ld "$D.new/usr/lib/zenix/"
# tcc's own headers (stdarg.h, stddef.h, float.h, ...; tccdefs.h is built into the program), from the Makefile's patched
# copy of ports/tcc: /usr/lib/tcc/include, the first directory tcc searches
mkdir -p "$D.new/usr/lib/tcc/include"
for h in "$B"/tcc/src/include/*.h; do
    [ "$(basename "$h")" = tccdefs.h ] || cp "$h" "$D.new/usr/lib/tcc/include/"
done
# a header tree, its directories as they are (the headers include each other by these paths)
tree() {
    (cd "$1" && find . -name '*.h' -type f) | sort | while read -r h; do
        mkdir -p "$(dirname "$2/$h")"
        cp "$1/$h" "$2/$h"
    done
}
tree libc/zenix/include "$D.new/usr/include"
# two of the port's headers reach into the source tree for the one table they share with the kernel and libz
# (sys/errno.h: kernel/errno.h; sys/socket.h: user/socket.h): those go to /usr/include/zenix, the includes pointed there
mkdir -p "$D.new/usr/include/zenix"
cp kernel/errno.h "$D.new/usr/include/zenix/errno.h"
cp user/socket.h "$D.new/usr/include/zenix/socket.h"
sed -i 's|^#include "\.\./\.\./\.\./\.\./kernel/errno\.h"$|#include <zenix/errno.h>|' "$D.new/usr/include/sys/errno.h"
sed -i 's|^#include "\.\./\.\./\.\./\.\./user/socket\.h"$|#include <zenix/socket.h>|' "$D.new/usr/include/sys/socket.h"
# (zenix/socket.h's libz branch, never taken in the C library, keeps its "../kernel/netproto.h")
if grep -rn '#include "\.\./\.\./\.\./' "$D.new/usr/include" >&2; then
    echo "mkdevel.sh: a header still includes a path outside /usr/include" >&2
    exit 1
fi
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
