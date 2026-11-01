#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build a ZenixFS root disk: tools/mkroot.sh's tree with ROOTFS=zenixfs, wrapped in an RDB so that the disk server sees a
# ZenixFS partition.
# usage: mkroot-zenixfs.sh out.hdf builddir "prog prog ..." [size-in-MB]
# optional environment: as mkroot.sh (FSTAB_FILE, MOUNTDIRS, PKGDB, NOMAN ...), and EXTRA="image:type:label ..."  more
#                       partitions after the root (see mkrdb-multi.py); RAWIMG=1  out.hdf is the filesystem image itself,
#                       no RDB around it (tools/mkcard.py puts it on a card's layout)
set -e
OUT="$1"; B="$2"; PROGS="$3"; MB="${4:-8}"
HERE="$(cd "$(dirname "$0")" && pwd)"
IMG=$(mktemp)
trap 'rm -f "$IMG"' EXIT
ROOTFS=zenixfs "$HERE/mkroot.sh" "$IMG" "$B" "$PROGS" "${MB}M" >/dev/null
if [ -n "$RAWIMG" ]; then cp "$IMG" "$OUT"
elif [ -n "$EXTRA" ]; then python3 "$HERE/mkrdb-multi.py" "$OUT" "$IMG:zenixfs:root" $EXTRA >/dev/null
else python3 "$HERE/mkrdb.py" "$IMG" "$OUT" zenixfs >/dev/null; fi
echo "$OUT: ZenixFS root, $(echo $PROGS | wc -w) programs"
