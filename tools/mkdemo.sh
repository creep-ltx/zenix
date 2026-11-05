#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The demo floppy (DESIGN D25 revision 3, 10 step 3; make demo): the RAM disk's image by tools/mkroot.sh with the
# demo's programs (the everyday commands as the box's links) and its /etc (tools/demo/), then the ADF by
# tools/mkdemo.py (the bootblock, the stage 2, the table, the kernel and the image as one packed stream).
# usage: mkdemo.sh BUILD dd|hd OUT.adf "PROGRAM ..." "BOX NAME ..."
# Says the parts and the space left on the floppy; an image that does not fit is an error saying by how much.
set -e
B="$1"; KIND="$2"; OUT="$3"; PROGS="$4"; BOXNAMES="$5"
case $KIND in dd|hd) ;; *) echo "mkdemo.sh: dd or hd, not '$KIND'" >&2; exit 2;; esac
IMG="${OUT%.adf}.img"
DEMO=$KIND BOX="$BOXNAMES" NOTESTS=1 tools/mkroot.sh "$IMG" "$B" "$PROGS $BOXNAMES" auto auto
e2fsck -fn "$IMG" >/dev/null 2>&1 || { echo "mkdemo.sh: e2fsck finds errors in $IMG" >&2; exit 1; }
python3 tools/mkdemo.py "$B/demoboot.bin" "$B/demoload.zx" "$B/kernel.bin" "$IMG" "$OUT" "$KIND"
