#!/bin/sh
# Build the ext2 root disk: rootfs/ plus the programs from build/*.zx as /bin/<name>.
# usage: mkroot.sh out.hdf builddir "prog prog ..."
set -e
OUT="$1"; B="$2"; PROGS="$3"; SIZE="${4:-8M}"; INODES="${5:-512}"
T=$(mktemp -d)
cp -r "$(dirname "$0")/../rootfs/." "$T/"
mkdir -p "$T/bin" "$T/tmp"
for p in $PROGS; do
    name=$(echo "$p" | sed 's/^mkfs_/mkfs./; s/^fsck_/fsck./')    # mkfs_ext2 -> mkfs.ext2
    cp "$B/$p.zx" "$T/bin/$name"
    chmod 755 "$T/bin/$name"
done
mkdir -p "$T/usr/man"
cp "$(dirname "$0")/../docs/man/"*.txt "$T/usr/man/"      # the manual pages live in docs/man
python3 "$(dirname "$0")/mkman.py" "$T/usr/man.gen" $PROGS
for f in "$T"/usr/man.gen/*.txt; do b=$(basename "$f"); [ -e "$T/usr/man/$b" ] || cp "$f" "$T/usr/man/$b"; done
rm -rf "$T/usr/man.gen"
ln -s less "$T/bin/more"
ln -s gzip "$T/bin/gunzip"; ln -s gzip "$T/bin/zcat"            # more is less
rm -f "$OUT"
# revision 0: the original ext2, 128-byte inodes, no optional features
mke2fs -q -t ext2 -E revision=0 -b 1024 -N "$INODES" -d "$T" -F "$OUT" "$SIZE"
rm -rf "$T"
e2fsck -fn "$OUT" >/dev/null && echo "$OUT: ext2 root, $(echo $PROGS | wc -w) programs"
