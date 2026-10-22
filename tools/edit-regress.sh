#!/usr/bin/env bash
# The editor: type, move, save, quit, reopen, replace, undo/redo, all by serial key presses; the saved file is read back
# from the disk image and compared with what it must be.
cd "$(dirname "$0")/.." || exit 1
make -s all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f build/zenix.hdf; make -s build/zenix.hdf >/dev/null 2>&1
tools/run-test.sh fsuae/a1200-020-fast.fs-uae 70 tools/tests/edit-test.txt > /tmp/edit-regress.log 2>&1
debugfs -R "cat /tmp/e1.txt" build/zenix.hdf 2>/dev/null > /tmp/edit-regress.out
printf 'hellXo there\nsecond line\nthird\n' > /tmp/edit-regress.want
if cmp -s /tmp/edit-regress.out /tmp/edit-regress.want; then echo "PASS: edit typed, moved, saved, reopened, replaced and undone correctly"; else echo "FAIL: saved file differs:"; diff /tmp/edit-regress.want /tmp/edit-regress.out; exit 1; fi
