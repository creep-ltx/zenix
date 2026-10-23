#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The editor: type, move, save, quit, reopen, replace, undo/redo, all by serial key presses; the saved file is read back
# from the disk image and compared with what it must be.
cd "$(dirname "$0")/.." || exit 1
make -s all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f build/zenix.hdf; make -s build/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-edit}; mkdir -p $OUT
tools/run-test.sh fsuae/a1200-020-fast.fs-uae 70 tools/tests/edit-test.txt > $OUT/edit-regress.log 2>&1
debugfs -R "cat /tmp/e1.txt" build/zenix.hdf 2>/dev/null > $OUT/edit-regress.out
printf 'hellXo there\nsecond line\nthird\n' > $OUT/edit-regress.want
if cmp -s $OUT/edit-regress.out $OUT/edit-regress.want; then echo "PASS: edit typed, moved, saved, reopened, replaced and undone correctly"; else echo "FAIL: saved file differs:"; diff $OUT/edit-regress.want $OUT/edit-regress.out; exit 1; fi
