#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The console's ring of rows (DESIGN D28 16) in FS-UAE through the cage rig (tools/shot.py: nothing on the desktop),
# on the A1200 with fast RAM (4 planes, a ring of 64 rows) and without (3 planes, 48 rows), both built with DIWV=0x38
# as tools/colour-test.sh is. Screenshots taken when the script gets there, checked by tools/colour-test.py:
#  - `seq 1 2000` (the ring goes round 30 times and more): termtest's CON_LINE picture of the display (the kernel's
#    own rows, through the ring) is the screenshot of the same moment, pixel by pixel - so the Copper's two sets of
#    pointers show the rows where the kernel drew them; and the grid (CON_READ) holds 1970..2000, the last row empty
#  - scrolled back a page after it (Amiga+Up's serial key): the very picture of the same lines printed afresh
#    (1940..1971, the cursor hidden)
#  - `termtest scroll` (full-screen scrolls, then a scroll region with inserted and deleted lines, CSI S, ESC M) three
#    times, each from another place in the ring (7 and 13 lines between): the screen afterwards is what a redraw from
#    the grid gives, and CON_LINE's picture is the screenshot
# The script knows that a grab has ended (the screen must not change before then) by a line written to /dev/ttyS0,
# which reaches the serial log without touching the screen.
# usage: tools/scroll-test.sh [fast|chip ...]   (default both)
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-scroll-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-scroll.XXXXXX)}; mkdir -p "$OUT"
RUNS=${*:-fast chip}
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
ct() { python3 tools/colour-test.py "$@"; }
B=$OUT/build
trap 'rm -rf "$B"' EXIT
make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL the build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
cp "$B/zenix.hdf" "$OUT/clean.hdf"

cat > "$OUT/script.txt" <<'EOF'
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send clear; seq 1 2000; termtest grab /tmp/g1.zg; termtest screen /dev/tty1 2000 2 > /tmp/s1.txt; echo g1""ok > /dev/ttyS0; sleep 12; echo seq""done\r
wait g1ok 300
sleep 1
shot seq
wait seqdone 60
sleep 2
send \x1dU
sleep 3
shot back
send \x1dD
sleep 1
send printf '\\033[?25l\\033[2J\\033[H'; seq 1940 1970; printf 1971; sleep 10; printf '\\033[?25h\\n'\r
sleep 6
shot backref
sleep 6
send termtest scroll; termtest grab /tmp/g2.zg; echo g2""ok > /dev/ttyS0; sleep 12; echo r1""done\r
wait g2ok 120
sleep 1
shot region1
send \x1du
sleep 2
send \x1dd
sleep 3
shot region1r
wait r1done 60
sleep 1
send seq 1 7; termtest scroll; termtest grab /tmp/g3.zg; echo g3""ok > /dev/ttyS0; sleep 12; echo r2""done\r
wait g3ok 120
sleep 1
shot region2
send \x1du
sleep 2
send \x1dd
sleep 3
shot region2r
wait r2done 60
sleep 1
send seq 1 13; termtest scroll; termtest grab /tmp/g4.zg; echo g4""ok > /dev/ttyS0; sleep 12; echo r3""done\r
wait g4ok 120
sleep 1
shot region3
send \x1du
sleep 2
send \x1dd
sleep 3
shot region3r
wait r3done 60
sleep 1
send sync; echo all""done\r
wait alldone 60
sleep 2
EOF

for r in $RUNS; do
    case $r in fast) conf=fsuae/a1200-020-fast.fs-uae; planes=4 ;; chip) conf=fsuae/a1200-020.fs-uae; planes=3 ;;
        *) echo "unknown run $r"; exit 2 ;; esac
    cp "$OUT/clean.hdf" "$B/zenix.hdf"
    ZENIX_BUILD="$B" python3 tools/fsuae_conf.py $conf "$OUT/$r.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=600 timeout -k 20 900 python3 tools/shot.py "$OUT/$r.fs-uae" "$OUT/script.txt" "$OUT/$r" >/dev/null 2>&1
    tr -d '\r' < "$OUT/$r.serial.log" | grep -q "display: 640x256, $planes bitplanes, 80x32 text"
    check "$r: $planes planes (the kernel's log)" $?
    for s in columns seq back backref region1 region1r region2 region2r region3 region3r; do
        [ -f "$OUT/$r-$s.png" ] || { echo "FAIL $r: no screenshot $s (see $OUT/$r.serial.log)"; pass=0; continue 2; }
    done
    ct columns "$OUT/$r-columns.png" > "$OUT/$r-geom"; check "$r: the geometry from termtest columns" $?
    for g in 1 2 3 4; do debugfs -R "cat /tmp/g$g.zg" "$B/zenix.hdf" > "$OUT/$r-g$g.zg" 2>/dev/null; done
    debugfs -R "cat /tmp/s1.txt" "$B/zenix.hdf" 2>/dev/null | tr -d '\r' > "$OUT/$r-s1.txt"
    ct grab "$OUT/$r-seq.png" "$OUT/$r-geom" "$OUT/$r-g1.zg"
    check "$r: after seq 1 2000 the screen is CON_LINE's picture of the ring, pixel by pixel" $?
    want=$(for n in $(seq 1970 2000); do printf 'row %02d|%s|\n' $((n - 1970)) $n; done; echo 'row 31||')
    got=$(grep '^row ' "$OUT/$r-s1.txt")
    [ "$got" = "$want" ]; check "$r: after seq 1 2000 the grid holds 1970..2000 and an empty last row" $?
    ct same "$OUT/$r-back.png" "$OUT/$r-backref.png"
    check "$r: scrolled back a page after the ring went round: 1940..1971, as printed afresh" $?
    for k in 1 2 3; do
        ct same "$OUT/$r-region$k.png" "$OUT/$r-region${k}r.png"
        check "$r: termtest scroll (round $k, another place in the ring): the screen is a redraw from the grid" $?
        ct grab "$OUT/$r-region$k.png" "$OUT/$r-geom" "$OUT/$r-g$((k + 1)).zg"
        check "$r: ... and CON_LINE's picture of it, pixel by pixel" $?
    done
done
[ $pass = 1 ] && echo "scroll-test: all passed" || { echo "scroll-test: FAILED (see $OUT)"; exit 1; }
