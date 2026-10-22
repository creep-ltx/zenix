#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build a ZenixFS root disk: the same files as tools/mkroot.sh (rootfs/ plus the programs from build/*.zx as /bin/<name>),
# put into a ZenixFS image with the host tool, wrapped in an RDB so that the disk server sees a ZenixFS partition.
# usage: mkroot-zenixfs.sh out.hdf builddir "prog prog ..." [size-in-MB]
# optional environment: FSTAB_FILE=file  installed as /etc/fstab;  MOUNTDIRS="dir dir"  created in the root (mount points);
#                       EXTRA="image:type:label ..."  more partitions after the root (see mkrdb-multi.py)
set -e
OUT="$1"; B="$2"; PROGS="$3"; MB="${4:-8}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TOOL="$HERE/../zenixfs/build/zenixfs-tool"
[ -x "$TOOL" ] || make -s -C "$HERE/../zenixfs" build/zenixfs-tool
T=$(mktemp -d); IMG=$(mktemp)
cp -r "$HERE/../rootfs/." "$T/"
mkdir -p "$T/bin" "$T/tmp" "$T/home/user" "$T/home/guest"
for p in $PROGS; do
    name=$(echo "$p" | sed 's/^mkfs_/mkfs./; s/^fsck_/fsck./')
    cp "$B/$p.zx" "$T/bin/$name"
    chmod 755 "$T/bin/$name"
done
if [ -z "$NOMAN" ]; then
    mkdir -p "$T/usr/man"
    cp "$HERE/../docs/man/"*.txt "$T/usr/man/"
    python3 "$HERE/mkman.py" "$T/usr/man.gen" $PROGS
    for f in "$T"/usr/man.gen/*.txt; do b=$(basename "$f"); [ -e "$T/usr/man/$b" ] || cp "$f" "$T/usr/man/$b"; done
    rm -rf "$T/usr/man.gen"
fi
[ -n "$FSTAB_FILE" ] && cp "$FSTAB_FILE" "$T/etc/fstab"
for d in $MOUNTDIRS; do mkdir -p "$T/$d"; done
ln -s less "$T/bin/more"
ln -s gzip "$T/bin/gunzip"; ln -s gzip "$T/bin/zcat"; ln -s chown "$T/bin/chgrp"
# modes (the tool keeps the modes of the host files; everything is owned by root unless set below)
chmod 1777 "$T/tmp"
chmod 4755 "$T/bin/su" "$T/bin/passwd"
chmod 644 "$T/etc/passwd" "$T/etc/group"
"$TOOL" mkfs "$IMG" "$MB" -l zenix-root >/dev/null
"$TOOL" put "$IMG" -r "$T" /
"$TOOL" chown "$IMG" 1000 100 /home/user
"$TOOL" chown "$IMG" 1001 100 /home/guest
"$TOOL" fsck "$IMG" >/dev/null
if [ -n "$EXTRA" ]; then python3 "$HERE/mkrdb-multi.py" "$OUT" "$IMG:zenixfs:root" $EXTRA >/dev/null
else python3 "$HERE/mkrdb.py" "$IMG" "$OUT" zenixfs >/dev/null; fi
rm -rf "$T" "$IMG"
echo "$OUT: ZenixFS root, $(echo $PROGS | wc -w) programs"
