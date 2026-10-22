#!/bin/sh
# Build the ext2 root disk: rootfs/ plus the programs from build/*.zx as /bin/<name>.
# usage: mkroot.sh out.hdf builddir "prog prog ..."
set -e
OUT="$1"; B="$2"; PROGS="$3"
T=$(mktemp -d)
cp -r "$(dirname "$0")/../rootfs/." "$T/"
mkdir -p "$T/bin" "$T/tmp"
for p in $PROGS; do
    cp "$B/$p.zx" "$T/bin/$p"
    chmod 755 "$T/bin/$p"
done
rm -f "$OUT"
# revision 0: the original ext2, 128-byte inodes, no optional features
mke2fs -q -t ext2 -E revision=0 -b 1024 -N 512 -d "$T" -F "$OUT" 8M
rm -rf "$T"
e2fsck -fn "$OUT" >/dev/null && echo "$OUT: ext2 root, $(echo $PROGS | wc -w) programs"
