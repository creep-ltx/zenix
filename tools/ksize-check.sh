#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The kernel's size budget (audit 2026-10-29 B2-13, network audit O14): every part of the kernel (its object file)
# against its line in tools/ksize-budget, kernel.bin against TOTAL, and the room left under KERNEL_MAX_MOUNT against
# ROOM. Run by the Makefile's kernel.bin rule; a part over its budget fails the build and names it.
#   tools/ksize-check.sh BUILD KERNEL_BIN KERNEL_MAX_MOUNT
# Variant kernels (DIAG, GFXDEPTH set) are not checked: the Makefile passes KSIZE_SKIP=1 for them.
B=$1 BIN=$2 MAXM=$3
[ -n "$KSIZE_SKIP" ] && { echo "ksize-check: a variant kernel, not checked"; exit 0; }
SIZE=${TOOLPREFIX}size
bad=0 total=0
while read -r name code bss; do
    case "$name" in ''|'#'*) continue ;; TOTAL) total=$code; continue ;; ROOM) room=$code; continue ;; esac
    read -r t d b _ < <($SIZE "$B/$name.o" | awk 'NR == 2 { print $1, $2, $3 }') || { echo "ksize-check: $B/$name.o: no size"; exit 1; }
    [ -n "$t" ] || { echo "ksize-check: $B/$name.o: no size"; exit 1; }
    if [ $((t + d)) -gt "$code" ]; then echo "ksize-check: $name: code + data $((t + d)) bytes, over its budget of $code (tools/ksize-budget)"; bad=1; fi
    if [ "$b" -gt "$bss" ]; then echo "ksize-check: $name: bss $b bytes, over its budget of $bss (tools/ksize-budget)"; bad=1; fi
done < "$(dirname "$0")/ksize-budget"
s=$(wc -c < "$BIN")
if [ "$s" -gt "$total" ]; then echo "ksize-check: kernel.bin is $s bytes, over the budget of $total"; bad=1; fi
if [ $((MAXM - s)) -lt "$room" ]; then echo "ksize-check: kernel.bin leaves $((MAXM - s)) bytes on the mount tests' floppy, under $room"; bad=1; fi
[ $bad = 0 ] && echo "ksize-check: every part within its budget; kernel.bin $s of $total"
exit $bad
