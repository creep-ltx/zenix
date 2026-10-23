#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Create the Zenix IDE test disk: 16MB whole-disk FAT16 with a hello file.
set -e
OUT="$1"
dd if=/dev/zero of="$OUT" bs=1M count=16 status=none
mkfs.fat -F 16 -n ZENIX "$OUT" >/dev/null
T=$(mktemp)
printf 'Hello from the host! If Zenith can read this, M2 storage works.\n' > "$T"
mcopy -i "$OUT" "$T" ::HELLO.TXT
rm -f "$T"
echo "$OUT: 16MB FAT16 with HELLO.TXT"
