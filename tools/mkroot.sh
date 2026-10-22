#!/bin/sh
# Build the ext2 root disk: rootfs/ plus the programs from build/*.zx as /bin/<name>.
# usage: mkroot.sh out.hdf builddir "prog prog ..."
set -e
OUT="$1"; B="$2"; PROGS="$3"; SIZE="${4:-8M}"; INODES="${5:-512}"
T=$(mktemp -d)
# the image is built as a temporary file next to the output and moved into place only when mke2fs and e2fsck
# succeeded: a failure (the programs no longer fit, say) must not leave a half-written image that the next make
# takes for up to date. Any failure also removes the staging directory.
TMPOUT="$OUT.tmp.$$"
trap 'rm -rf "$T" "$TMPOUT"' EXIT
cp -r "$(dirname "$0")/../rootfs/." "$T/"
[ -n "$NOTESTS" ] && rm -rf "$T/tests"      # NOTESTS=1: leave the test scripts out (the small RAM-disk image)
mkdir -p "$T/bin" "$T/tmp"
for p in $PROGS; do
    name=$(echo "$p" | sed 's/^mkfs_/mkfs./; s/^fsck_/fsck./')    # mkfs_ext2 -> mkfs.ext2
    cp "$B/$p.zx" "$T/bin/$name"
    chmod 755 "$T/bin/$name"
done
if [ -z "$NOMAN" ]; then          # NOMAN=1: leave the manual pages out (small install / test images)
    mkdir -p "$T/usr/man"
    cp "$(dirname "$0")/../docs/man/"*.txt "$T/usr/man/"      # the manual pages live in docs/man
    python3 "$(dirname "$0")/mkman.py" "$T/usr/man.gen" $PROGS
    for f in "$T"/usr/man.gen/*.txt; do b=$(basename "$f"); [ -e "$T/usr/man/$b" ] || cp "$f" "$T/usr/man/$b"; done
    rm -rf "$T/usr/man.gen"
fi
ln -s less "$T/bin/more"
ln -s gzip "$T/bin/gunzip"; ln -s gzip "$T/bin/zcat"; ln -s chown "$T/bin/chgrp"; ln -s pgrep "$T/bin/pkill"            # more is less
rm -f "$TMPOUT"
# revision 0: the original ext2, 128-byte inodes, no optional features
# ownership and modes under fakeroot, so the image holds uid 0 (not whoever runs the build): root owns everything,
# /tmp is sticky and open to all, a normal user has a home, su and passwd are set-user-id root
mkdir -p "$T/home/user" "$T/home/guest"
fakeroot sh -c '
    chown -R 0:0 "$1"
    chown 1000:100 "$1/home/user"
    chown 1001:100 "$1/home/guest"
    chmod 1777 "$1/tmp"
    chmod 4755 "$1/bin/su" "$1/bin/passwd"
    chmod 644 "$1/etc/passwd" "$1/etc/group"
    mke2fs -q -t ext2 -E revision=0 -b 1024 -N "$2" -d "$1" -F "$3" "$4"
' sh "$T" "$INODES" "$TMPOUT" "$SIZE"
e2fsck -fn "$TMPOUT" >/dev/null
mv -f "$TMPOUT" "$OUT"
echo "$OUT: ext2 root, $(echo $PROGS | wc -w) programs"
