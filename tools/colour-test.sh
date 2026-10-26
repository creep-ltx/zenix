#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The colour console (DESIGN D14) in FS-UAE through the cage rig (tools/shot.py: nothing on the desktop), at 1, 3 and
# 4 bitplanes (test builds with make GFXDEPTH=n DIWV=0x38, on the A1200 with fast RAM; 3 planes there use the
# OCS/ECS fetch). For each depth, screenshots taken when the script gets there (tools/colour-test.py checks them):
#  - `termtest columns`: all 80 columns visible, in order, nothing repeated, and the picture's edges the very pixels
#    one plane has (at 4 planes this is the test of FMODE 3's fetch window)
#  - `termtest colours`: the 16 colour blocks against the palette (3 planes: colour & 7; 1 plane: no colours)
#  - `termtest scroll` (coloured lines scrolling the screen three times, so the blitter copies the rows back,
#    background-colour erases, a scroll region with inserted and deleted lines): the screen afterwards must be the
#    very picture a redraw from the grid gives (the scrollback one line up and back down redraws) - no garbage
#  - scrolled back a page: the rows keep their colours (3 and 4 planes)
# usage: tools/colour-test.sh [DEPTH...]      (default 1 3 4; the 1-plane run comes first: its edges are the reference)
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-colour-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-colour.XXXXXX)}; mkdir -p "$OUT"
DEPTHS=${*:-1 3 4}
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
ct() { python3 tools/colour-test.py "$@"; }
# the test builds (about 25 MB each) go when the script ends; the logs, configs and screenshots stay
trap 'for d in $DEPTHS; do rm -rf "$OUT/d$d"; done' EXIT

cat > "$OUT/script.txt" <<'EOF'
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send termtest colours\r
sleep 5
shot colours
send q
sleep 2
send termtest scroll\r
wait zenix:/# 120
sleep 3
shot scrolled
send \x1du
sleep 2
send \x1dd
sleep 3
shot redrawn
send \x1dU
sleep 3
shot back
send \x1dD
sleep 1
EOF

for d in $DEPTHS; do
    B=$OUT/d$d
    make -s -j8 BUILD="$B" GFXDEPTH=$d DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/d$d.build.log" 2>&1 ||
        { echo "FAIL build of the $d-plane kernel:"; grep -v 'RWX permissions' "$OUT/d$d.build.log" | tail -15; pass=0; continue; }
    ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/d$d.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=400 timeout -k 20 900 python3 tools/shot.py "$OUT/d$d.fs-uae" "$OUT/script.txt" "$OUT/d$d" >/dev/null 2>&1
    want="$d bitplane"; [ $d = 1 ] || want="$d bitplanes"
    tr -d '\r' < "$OUT/d$d.serial.log" | grep -q "display: 640x256, $want, 80x32 text"
    check "$d planes: the kernel says so on the log" $?
    for s in columns colours scrolled redrawn back; do
        [ -f "$OUT/d$d-$s.png" ] || { echo "FAIL $d planes: no screenshot $s (see $OUT/d$d.serial.log)"; pass=0; continue 2; }
    done
    ref=""; [ $d = 1 ] || [ ! -f "$OUT/edges1" ] || ref="$OUT/edges1"
    ct columns "$OUT/d$d-columns.png" $ref > "$OUT/geom$d"; r=$?
    check "$d planes: termtest columns: all 80 columns in order${ref:+, the edges exactly where one plane has them} ($(head -1 "$OUT/geom$d"))" $r
    [ $d = 1 ] && head -1 "$OUT/geom$d" > "$OUT/edges1"
    ct palette "$OUT/d$d-colours.png" "$OUT/geom$d" $d; check "$d planes: termtest colours: the 16 blocks as $d planes show them" $?
    ct same "$OUT/d$d-scrolled.png" "$OUT/d$d-redrawn.png"; check "$d planes: after termtest scroll the screen is what a redraw from the grid gives" $?
    if [ $d != 1 ]; then
        ct rows "$OUT/d$d-back.png" "$OUT/geom$d" $d; check "$d planes: scrolled back a page, every row keeps its colour" $?
    fi
done
echo "logs and screenshots: $OUT"
exit $((1 - pass))
