#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The mouse (DESIGN D61, docs/d61-mouse.md 9) in FS-UAE through the cage rig, on the A1200 with fast RAM (a test build
# with DIWV=0x38, as gcon-test's). `mouse-test.sh k` (the kernel, through gcontest: CON_MOUSE's struct umouse):
#  - a wrong setting refused (a gain of 0: EINVAL); a slow sweep of one count a frame at a gain of one pixel across and
#    half a line down: every record one pixel further, the lines every second count (the fraction kept), the sprite
#    where the ring says; the acceleration's moves (threshold 4, multiplier 2) as tools/mouse-host-test.c's arithmetic
#    computes them (here again in Python, independently)
#  - interlaced units (640 x 512, the sprite's line y / 2): one lace line a count, the sprite on the frame line
#  - a graphics card's units (1280 x 960, the sprite at x / 2, y / 4): single card pixels, clamped at 0 and at 1279,
#    959, the place set by the setting itself
cd "$(dirname "$0")/.." || exit 1
PARTS=${*:-k}
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-mouse-serial}
OUT=${ZENIX_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/zenix-mouse.XXXXXX")}; mkdir -p "$OUT"
B=$OUT/build
trap 'rm -rf "$B"' EXIT
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
# the moves a record of DX counts makes, one frame each (tools/mouse-host-test.c's arithmetic, kernel/pointer.h):
# "expect P0 THR MULT GAIN LIM D..." -> the places, in units, after each
expect() {
    python3 - "$@" <<'PY'
import sys
p, thr, mult, gain, lim = (int(v, 0) for v in sys.argv[1:6])
p = p << 8 | 0x80                    # a place set by CON_MOUSE is the pixel's middle
out = []
for d in (int(v) for v in sys.argv[6:]):
    a = min(abs(d), 255)
    v = (thr << 8) + (a - thr) * mult if a > thr else a << 8
    v = v * gain >> 8
    p += -v if d < 0 else v
    p = max(0, min(p, (lim << 8) - 1))
    out.append(str(p >> 8))
print(" ".join(out))
PY
}
rec() { local r="" i; for ((i = 0; i < $1; i++)); do r="$r $2"; done; echo "$r"; }

run_k() {
    cat > "$OUT/k-script.txt" <<EOS
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send gcontest ack 300 m 640,256,0x100,0x80,4,0x200,0,0,0 > /tmp/k1.log &\r
sleep 4
send gcontest mouse$(rec 20 1,1,0)\r
sleep 3
shot slow
send gcontest mouse 1,0,0 2,0,0 3,0,0 4,0,0 5,0,0 6,0,0 8,0,0 12,0,0 20,0,0\r
sleep 3
send kill \$(pgrep gcontest)\r
sleep 3
send gcontest ack 300 m 640,512,0x100,0x100,0,0x100,4,0,0 > /tmp/k2.log &\r
sleep 4
send gcontest mouse$(rec 9 1,1,0) 0,1,0\r
sleep 3
shot lace
send kill \$(pgrep gcontest)\r
sleep 3
send gcontest ack 300 m 1280,960,0x100,0x100,0,0x100,9,1000,800 > /tmp/k3.log &\r
sleep 4
send gcontest mouse$(rec 5 1,1,0)\r
sleep 3
shot card
send gcontest mouse -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 127,127,0$(rec 12 127,127,0)\r
sleep 4
send kill \$(pgrep gcontest)\r
sleep 3
send cat /tmp/k1.log; echo k1-end; cat /tmp/k2.log; echo k2-end; cat /tmp/k3.log; echo mouse-end-\$((40+2))\r
wait mouse-end-42 60
shot end
EOS
    ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/k.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=300 timeout -k 20 600 python3 tools/shot.py "$OUT/k.fs-uae" "$OUT/k-script.txt" "$OUT/k" >/dev/null 2>&1
    local L=$OUT/k-serial.txt
    tr -d '\r' < "$OUT/k.serial.log" > "$L"
    for s in columns slow lace card; do
        [ -f "$OUT/k-$s.png" ] || { echo "FAIL no screenshot k-$s (see $OUT/k.serial.log)"; pass=0; return; }
    done
    python3 tools/colour-test.py columns "$OUT/k-columns.png" > "$OUT/geom"; check "the text console's geometry" $?
    # each log's pointer records, "x,y" a line, in order (the first is the setting's place)
    sed -n '1,/^k1-end/p' "$L" | awk '/^gcontest: pointer/ { print $3 "," $4 }' > "$OUT/k1.pos"
    sed -n '/^k1-end/,/^k2-end/p' "$L" | awk '/^gcontest: pointer/ { print $3 "," $4 }' > "$OUT/k2.pos"
    sed -n '/^k2-end/,$p' "$L" | awk '/^gcontest: pointer/ { print $3 "," $4 }' > "$OUT/k3.pos"
    grep -q "^gcontest: set gain 0 -> -22" "$L" && [ "$(grep -c '^gcontest: set .* -> 0' "$L")" = 3 ]
    check "CON_MOUSE with a struct umouse: a gain of 0 refused (EINVAL), each of the three settings taken" $?
    local xs ys want got
    xs=$(expect 0 4 0x200 0x100 640 $(rec 20 1)); ys=$(expect 0 4 0x200 0x80 256 $(rec 20 1))
    want="0,0 $(paste -d, <(tr ' ' '\n' <<<"$xs") <(tr ' ' '\n' <<<"$ys") | tr '\n' ' ')"
    got=$(head -21 "$OUT/k1.pos" | tr '\n' ' ')
    [ "$got" = "$want" ]; check "a slow sweep, a count a frame: one pixel across each (1..20), a line down every second (got $got)" $?
    xs=$(expect 20 4 0x200 0x100 640 1 2 3 4 5 6 8 12 20)
    got=$(sed -n '22,30p' "$OUT/k1.pos" | cut -d, -f1 | tr '\n' ' ')
    [ "$got" = "$xs " ]; check "the acceleration (beyond 4 counts a frame doubled): $xs (got $got)" $?
    python3 tools/gcon-test.py grabbed "$OUT/k-slow.png" "$OUT/geom" 20 10; check "the sprite where the ring says (20, 10)" $?
    want="0,0 1,1 2,2 3,3 4,4 5,5 6,6 7,7 8,8 9,9 9,10 "
    got=$(tr '\n' ' ' < "$OUT/k2.pos")
    [ "$got" = "$want" ]; check "interlaced units: one lace line a count (got $got)" $?
    python3 tools/gcon-test.py grabbed "$OUT/k-lace.png" "$OUT/geom" 9 5; check "... the sprite on frame line 10 / 2 = 5" $?
    got=$(head -6 "$OUT/k3.pos" | tr '\n' ' ')
    [ "$got" = "1000,800 1001,801 1002,802 1003,803 1004,804 1005,805 " ]
    check "a card's units (1280 x 960): single pixels from the setting's place 1000, 800 (got $got)" $?
    python3 tools/gcon-test.py grabbed "$OUT/k-card.png" "$OUT/geom" 502 201; check "... the sprite at 1005 / 2, 805 / 4" $?
    grep -q "^0,0$" "$OUT/k3.pos" && [ "$(tail -1 "$OUT/k3.pos")" = "1279,959" ]
    check "... clamped at 0, 0 and at 1279, 959" $?
}

for p in $PARTS; do
    case $p in
    k) run_k ;;
    *) echo "mouse-test: no part $p"; exit 2 ;;
    esac
done
exit $((1 - pass))
