#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The desktop's screen modes (DESIGN D38 "Screen modes"): hires and hires interlaced, on PAL and on NTSC. In FS-UAE
# through the cage rig (tools/shot.py) on the stock A1200 with 4 MB of fast RAM (a test build with DIWV=0x38), PAL by
# default, `tools/lace-test.sh ntsc` FS-UAE's NTSC A1200 (fsuae/matrix/a1200-ntsc.fs-uae). /etc/zdesk/screen says
# "mode hires-lace"; zdesk starts displayd, dpytest (docs/man/dpytest.txt) opens windows, a term is started:
#  - the screen is 640 x 512 (NTSC 640 x 400): dpytest's hello says so, and a window placed in the lower half shows
#    there, its inside's colour at its lines, the desktop below it to the last line, the bar at the top, the term's
#    frame where the server says it is;
#  - the pointer pushed down past the bottom edge: its tip on the screen's last line (D61: the kernel's pointer is in
#    the screen's lines; the sprite on the frame line), the pointer's colours there;
#  - DPY_MODE at run time (dpytest's "mode", as a preferences program's Test and Cancel): hires, 640 x 256 (200
#    NTSC), every window kept on the screen (the low one moved up), drawn again; then interlaced again;
# Pixels are read with tools/dpy-test.py on a geometry from the text console (colour-test.py columns), its lines
# halved for the interlaced screenshots (FS-UAE shows both fields woven).
cd "$(dirname "$0")/.." || exit 1
TV=${1:-pal}
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-lace-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-lace.XXXXXX)}; mkdir -p "$OUT"
B=$OUT/build
trap 'rm -rf "$B"' EXIT
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
if [ "$TV" = ntsc ]; then H=200 CONF=fsuae/matrix/a1200-ntsc.fs-uae; else H=256 CONF=fsuae/a1200-020-fast.fs-uae; fi
H2=$((2 * H))
AY=$((H2 - 160))                        # window A's frame top: low on the interlaced screen, below hires' bottom
cat > "$OUT/script.txt" <<EOS
wait zenix:/# 120
sleep 1
send termtest columns\\r
sleep 5
shot columns
send q
sleep 2
send grep -v '^mode' /etc/zdesk/screen > /tmp/scr; echo mode hires-lace >> /tmp/scr; cp /tmp/scr /etc/zdesk/screen; echo acceleration none > /etc/zdesk/mouse; echo speed 200 >> /etc/zdesk/mouse\\r
sleep 1
send printf 'right cmd 3600 echo .\\\\n' > /etc/zdesk/bar\\r
sleep 1
send : > /tmp/c; zdesk -v > /tmp/z.log 2>&1 &\\r
sleep 8
send dpytest -f /tmp/c > /tmp/d.log 2>&1 &\\r
sleep 3
send echo win A 200 80 9 100 $AY >> /tmp/c\\r
sleep 3
send term -p 320,40 -c 36 -r 30 -e sh &\\r
sleep 6
send echo info A >> /tmp/c\\r
sleep 2
shot desk
send gcontest mouse 40,127,0 0,127,0 0,127,0 0,127,0 0,127,0\\r
sleep 2
shot bottom
send gcontest mouse 0,0,1 0,0,1 0,0,0\\r
sleep 2
send echo mode hires >> /tmp/c\\r
sleep 4
send echo info A >> /tmp/c\\r
sleep 2
shot hires
send echo mode lace >> /tmp/c\\r
sleep 4
send echo info A >> /tmp/c\\r
sleep 2
shot lace
send cat /tmp/z.log /tmp/d.log; echo lace-end-\$((40+2))\\r
wait lace-end-42 30
EOS
make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
ZENIX_BUILD="$B" python3 tools/fsuae_conf.py "$CONF" "$OUT/run.fs-uae" || exit 1
ZOOMOUT=0 SHOT_LIMIT=300 timeout -k 20 600 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/l" >/dev/null 2>&1
L=$OUT/serial.txt; tr -d '\r' < "$OUT/l.serial.log" > "$L"
for s in columns desk bottom hires lace; do
    [ -f "$OUT/l-$s.png" ] || { echo "FAIL no screenshot $s (see $OUT/l.serial.log)"; exit 1; }
done
python3 tools/colour-test.py columns "$OUT/l-columns.png" > "$OUT/geom"; rc=$?; check "the text console's geometry ($(head -1 "$OUT/geom"))" $rc
# the same screenshot's geometry with lines half as tall: "x0 x1 yc h", the top yc - 7.5 h kept
awk '{ t = $3 - 7.5 * $4; h = $4 / 2; print $1, $2, t + 7.5 * h, h }' "$OUT/geom" > "$OUT/geom2"
px() { s=$1 g=$2; shift 2; python3 tools/dpy-test.py px "$OUT/l-$s.png" "$OUT/$g" "$@"; }
grep -a -q "^dpytest: open 0 screen 640x$H2 depth 4" "$L"; check "/etc/zdesk/screen's hires-lace: the screen is 640 x $H2" $?
grep -a -q "displayd: screen 640x$H, [34] planes$" "$L" && grep -a -q "displayd: screen 640x$H2 interlaced" "$L"
check "DPY_MODE at run time: hires (640 x $H), then interlaced (640 x $H2) again" $?
grep -a -q "^dpytest: mode 0 screen 640x$H" "$L" && grep -a -q "^dpytest: mode 1 screen 640x$H2" "$L"
check "... each client told the new size (EV_SCREEN: dpy_w x dpy_h follows)" $?
info() { grep -a -o "dpytest: info A [-0-9 ]*" "$L" | sed -n "$1p" | cut -d' ' -f4-7; }
read -r ax ay aw ah < <(info 1)
read -r hx hy _ < <(info 2)
read -r lx ly _ < <(info 3)
[ -n "$ah" ] && [ -n "$hy" ] && [ -n "$ly" ]; check "window A's places: $ax,$ay (${aw}x$ah), in hires $hx,$hy, interlaced again $lx,$ly" $?
# A placed by zdesk where dpytest asked; the frame's inside starts 12 lines below its top (the flat theme)
[ -n "$ah" ] && px desk geom2 "$((ax + 40)),$((ay + 12 + ah / 2)),D54" "$((ax + 40)),$((H2 - 4)),344" "300,4,112" > "$OUT/pd"
rc=$?; check "interlaced: A's inside at its lines, the desktop on the last lines, the bar ($(cat "$OUT/pd"))" $rc
[ -n "$hy" ] && [ $((hy + 12 + 80 + 1)) -le "$H" ] && px hires geom "$((hx + 40)),$((hy + 12 + 40)),D54" "$((hx + 40)),$((hy - 4)),344" > "$OUT/ph"
rc=$?; check "hires: A kept on the screen (its frame's bottom at most line $H), drawn again there, the desktop above it ($(cat "$OUT/ph" 2>/dev/null))" $rc
[ -n "$ly" ] && px lace geom2 "$((lx + 40)),$((ly + 12 + 40)),D54" "300,4,112" > "$OUT/pl"
rc=$?; check "interlaced again: A drawn ($(cat "$OUT/pl" 2>/dev/null))" $rc
# the pointer from the middle (320) 40 counts right (2 pixels a count), pushed past the bottom: its tip on the last
# line pair, H2-2 and H2-1 (one frame line: a sprite line shows in both fields; FS-UAE's view cuts the last lines,
# so the picture shows the desktop just above it), and a click there reaches zdesk at the last line, H2-1
# (EV_WM_CLICK; D61: the pointer is in the screen's own lines, so it reaches the odd last line too, not only H2-2)
px bottom geom2 "400,$((H2 - 3)),344" "400,$((H2 - 6)),344" > "$OUT/pb"
rc=$?; grep -a -q "zdesk: event 38 win 1 code 1 arg 0 at 400,$((H2 - 1))" "$L" || rc=1
check "the pointer pushed past the bottom: a click there at 400,$((H2 - 1)), the last line ($(cat "$OUT/pb"))" $rc
exit $((1 - pass))
