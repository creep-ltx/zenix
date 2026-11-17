#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Run rootfs/tests/cmds.sh under Zenix (output to a file on its disk) and under the host's bash with GNU tools
# (LC_ALL=C); the two outputs must be identical.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
# ZENIX_BOX=1 (tools/box-test.sh): the image's everyday commands are the box's links (DESIGN D25 revision 3, 10 step 1)
MKBOX=${ZENIX_BOX:+BOX=1}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $MKBOX $BD/zenix.hdf >/dev/null 2>&1
if [ -n "$MKBOX" ]; then [ "$(debugfs -R "stat /bin/ls" $BD/zenix.hdf 2>/dev/null | grep -o 'Links: [0-9]*')" = "Links: 65" ] || { echo "FAIL: the image's /bin/ls is not the box (65 links)"; exit 1; }; fi
OUT=${ZENIX_OUT:-/tmp/zenix-cmds}; mkdir -p $OUT
# GNU ls and grep with their built-in colours (the colour lines compare bytes; DESIGN D14). The script's scratch folder
# is /tmp/cmds.t on the PC too (its name is in the expected output): two runs at once on this PC (two sessions, or
# tools/mmu-suite.sh in two emulators) take turns there, or one's files land in the other's expected output
# (2026-11-17: "ln: ... col/link: File exists" expected from the other run's folder)
( cd rootfs/home/t && flock /tmp/zenix-cmds-host.lock env -u LS_COLORS -u GREP_COLORS -u GREP_COLOR LC_ALL=C \
    bash ../../tests/cmds.sh "$PWD" > $OUT/expected.txt 2>&1 )
cat > $OUT/script.txt <<EOS
wait zenix:/# 90
sleep 1
send sh /tests/cmds.sh > /tmp/cmds.out 2>&1; echo finished-cmds\r
wait (?m)^finished-cmds$ ${CMDS_WAIT:-100}
EOS
tools/run-test.sh fsuae/a1200-020-fast.fs-uae $(( ${CMDS_WAIT:-100} + 30 )) $OUT/script.txt > $OUT/run.log 2>&1
debugfs -R "cat /tmp/cmds.out" $BD/zenix.hdf > $OUT/actual.txt 2>/dev/null
if diff $OUT/expected.txt $OUT/actual.txt > $OUT/diff.txt; then echo "PASS: all command outputs identical to GNU ($(wc -l < $OUT/expected.txt) lines)"; else echo "FAIL: differences:"; head -60 $OUT/diff.txt; exit 1; fi
# the disk after the run (C1: rm of fast symbolic links used to free their text as block numbers)
if e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1; then echo "PASS: host e2fsck -fn clean after the run"; else echo "FAIL: e2fsck finds errors (see $OUT/e2fsck.log)"; exit 1; fi
