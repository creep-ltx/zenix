#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The text console's output speed (DESIGN D28 21) in FS-UAE: fixed outputs timed with `time` inside Zenix, on the
# screen alone (the serial copy off) and to /dev/null, on the stock A1200 with 4 MB of fast RAM (68020, the machine
# Zenix is made for) and on the 68060 matrix machine, by rootfs/tests/conspeed.sh (typed as one short line: long
# typed lines lose bytes on the serial line); its figures reach the serial log through /dev/ttyS0. Emulator figures,
# not hardware: printed for the docs and compared with each other, never claimed as an Amiga's speed.
#   seq     `seq 1 2000`: short lines, nearly all scrolling
#   long    600 lines of 74 characters: glyphs
#   colour  the same lines with a colour change every 10 characters (SGR sequences)
#   lsR     `ls -laR /`: the real-world benchmark (docs/ROADMAP.md)
# Checks: every figure measured, and the console's part of each (on the screen alone minus to /dev/null) under its
# ceiling below: the figure of 2026-11-07 (D28 21) and 30% (FS-UAE runs the same cycles each time; the figures moved
# by a few percent between runs), so a console that falls back to slow drawing fails. Before D28 21 the parts were
# 020: 8.07 6.77 8.29 6.27 s, 060: 5.66 3.84 4.65 3.70 s (after: 2.35 3.42 5.76 3.66, 1.77 2.29 3.48 2.40).
# usage: tools/conspeed-test.sh [020|060 ...]   (default both)
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD="$BD" all >/dev/null 2>&1 || { echo "FAIL the build"; exit 1; }
rm -f "$BD/zenix.hdf"; make -s BUILD="$BD" "$BD/zenix.hdf" >/dev/null 2>&1
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-conspeed.XXXXXX)}; mkdir -p "$OUT"
RUNS=${*:-020 060}
declare -A CEIL=([020-seq]=3.1 [020-long]=4.5 [020-colour]=7.5 [020-lsR]=4.8
                 [060-seq]=2.3 [060-long]=3.0 [060-colour]=4.5 [060-lsR]=3.2)
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
printf '%s\n' 'wait zenix:/# 120' 'sleep 1' 'send sh /tests/conspeed.sh\r' 'waitecho (?m)^cs-done 1200' 'stop' > "$OUT/script.txt"
for r in $RUNS; do
    case $r in 020) conf=fsuae/a1200-020-fast.fs-uae ;; 060) conf=fsuae/matrix/a1200-060.fs-uae ;;
        *) echo "unknown run $r"; exit 2 ;; esac
    cp "$BD/zenix.hdf" "$OUT/$r.hdf"
    sed "s#^hard_drive_0 = .*#hard_drive_0 = $OUT/$r.hdf#" "$conf" > "$OUT/$r.fs-uae"
    ZENIX_BUILD="$BD" tools/run-test.sh "$OUT/$r.fs-uae" 1500 "$OUT/script.txt" > "$OUT/$r.log" 2>&1
    L=$(tr -d '\r' < "$OUT/$r.log")
    fig() { grep -ao "^cs-$1-[0-9.]*s" <<<"$L" | tail -1 | sed 's/.*-\([0-9.]*\)s/\1/'; }
    printf '%s: %-7s %9s %9s\n' "$r" output screen /dev/null
    for k in seq long colour lsR; do
        s=$(fig $k-scr) n=$(fig $k-nul)
        printf '%s: %-7s %8ss %8ss\n' "$r" "$k" "${s:-?}" "${n:-?}"
        if [ -z "$s" ] || [ -z "$n" ]; then check "$r $k: measured (see $OUT/$r.log)" 1; continue; fi
        c=${CEIL[$r-$k]}
        awk -v s="$s" -v n="$n" -v c="$c" 'BEGIN { exit !(s - n <= c) }'
        check "$r $k: the console's part (screen alone - /dev/null) at most $c s" $?
    done
done
[ $pass = 1 ] && echo "conspeed-test: all passed" || { echo "conspeed-test: FAILED (see $OUT)"; exit 1; }
