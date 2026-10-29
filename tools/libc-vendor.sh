#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Copy picolibc's files that Zenix builds into libc/picolibc/ (DESIGN D27 section 4): the sources named in
# libc/picolibc/sources.mk, every file they include (found with the cross compiler's -MM; no build script of
# picolibc's is run) and the public headers (newlib/libc/include, what programs include). For an update: unpack the official release into a new empty directory, check its checksum, then
#   tools/libc-vendor.sh /path/to/picolibc-X.Y.Z
# and update the version in libc/picolibc/README and picolibc.h. The old files are removed first.
set -e
cd "$(dirname "$0")/.."
S=$(cd "$1" && pwd); [ -f "$S/COPYING.picolibc" ] || { echo "libc-vendor: $1 is not a picolibc release"; exit 1; }
CC=${TOOLPREFIX:-$HOME/opt/m68k-elf/bin/m68k-elf-}gcc
D=libc/picolibc
srcs=$(sed -n 's/^\s*\(newlib\/[^ ]*\.[cS]\).*/\1/p' $D/sources.mk)
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
INC="-Ilibc/zenix/include -I$T -I$S/newlib/libc/machine/m68k -I$S/newlib/libc/tinystdio -I$S/newlib/libc/locale -I$S -I$S/newlib/libc/include -I$S/newlib/libm/common"
cp $D/picolibc.h "$T/"
bash tools/libc-consts.sh "$T/zenix-consts.h" >/dev/null
: > "$T/deps"
for f in $srcs; do
    $CC -m68020-60 -msoft-float -ffreestanding -D_LIBC $INC -MM "$S/$f" >> "$T/deps"
done
{ tr ' \\' '\n\n' < "$T/deps" | grep "^$S/" | sed "s#^$S/##"
  (cd "$S" && find newlib/libc/include -type f -name '*.h'); } | sed 's#[^/]*/\.\./##g' | sort -u > "$T/files"
find $D/newlib -type f -delete 2>/dev/null || true
while read -r f; do mkdir -p "$D/$(dirname "$f")"; cp "$S/$f" "$D/$f"; done < "$T/files"
cp "$S/COPYING.picolibc" "$S/COPYING.NEWLIB" $D/
echo "libc-vendor: $(wc -l < "$T/files") files copied"
