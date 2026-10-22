#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Round trip through zenixfs-tool: make an image, copy a tree in and out, links, rename, remove, check.
set -e
T=${1:-build/zenixfs-tool}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
I=$W/disk.zenixfs
mkdir -p "$W/src/sub/deeper"
head -c 300000 /dev/urandom > "$W/src/big.bin"
head -c 100 /dev/urandom > "$W/src/small.bin"
: > "$W/src/empty"
printf 'line one\nline two\n' > "$W/src/sub/text.txt"
head -c 5000 /dev/urandom > "$W/src/sub/deeper/mid.bin"
ln -s sub/text.txt "$W/src/link"
for bs in 10 11 12; do
  rm -f "$I"
  "$T" mkfs "$I" 20 -b $bs -l test >/dev/null
  "$T" put "$I" -r "$W/src" / >/dev/null
  "$T" fsck "$I" >/dev/null
  rm -rf "$W/out"; "$T" get "$I" -r / "$W/out"
  diff -r "$W/src" "$W/out"
  "$T" ln "$I" -h /big.bin /big-hard
  "$T" mv "$I" /sub/text.txt /sub/renamed.txt
  "$T" rm "$I" /small.bin
  "$T" rmdir "$I" /sub/deeper 2>&1 | grep -q "not empty"
  "$T" rm "$I" /sub/deeper/mid.bin
  "$T" rmdir "$I" /sub/deeper
  [ "$("$T" cat "$I" /sub/renamed.txt)" = "$(printf 'line one\nline two')" ]
  cmp <("$T" cat "$I" /big-hard) "$W/src/big.bin"
  "$T" fsck "$I" >/dev/null
  "$T" mkfs "$W/utf.zenixfs" 4 -c utf8 | grep -q UTF-8
done
echo "tool test: ok"
