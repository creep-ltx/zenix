#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# TrueType fonts on the desktop (DESIGN D51, docs/d51-truetype.md "Tests"): in FS-UAE through the cage rig
# (tools/shot.py) on the stock A1200 with 4 MB of fast RAM (68EC020, no FPU; a test build with DIWV=0x38).
#  - ttf2font draws go.ttf at 8, 11, 13 and 16 lines and the console's 8 x 8 font; its times are printed (FS-UAE's
#    cycle-exact 68020, not a real machine's) and its files read back: each size's font file has the stamp;
#  - a damaged .ttf (cut short) named by /etc/zdesk/screen: ttf2font and displayd each say why in one line, and the
#    desktop runs with the 8 x 8;
#  - "font go 11" with no cache file: displayd runs ttf2font once ("drawn at 11 lines") and reads the file; the bar
#    and title bars 3 lines taller; a window titled "Zenix" shows its title as the host's renderer
#    draws it (tools/ttf-host-test.c text: the same files, read back by the same reader);
#  - the desktop started again: the cache file is used (no second drawing); the .ttf touched (its time changed): drawn
#    again.
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-ttf-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-ttf.XXXXXX)}; mkdir -p "$OUT"
B=$OUT/build
trap 'rm -rf "$B"' EXIT
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
cc -O1 -Idisplay/ttf -Idisplay/server tools/ttf-host-test.c display/ttf/ttf.c display/ttf/ttfont.c \
    display/server/amifont.c -o "$OUT/tht" || { echo "FAIL the host renderer's build"; exit 1; }
cat > "$OUT/script.txt" <<'EOS'
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send echo acceleration none > /etc/zdesk/mouse; echo speed 200 >> /etc/zdesk/mouse; printf 'right cmd 3600 echo .\\n' > /etc/zdesk/bar; rm -rf /var/cache/fonts\r
sleep 2
send ttf2font -o /tmp/tf /usr/share/fonts/truetype/go.ttf 8 11 13 16; echo ttf-sizes-$?\r
wait ttf-sizes- 600
send ttf2font -c /usr/share/fonts/truetype/go.ttf /tmp/go.zcf; echo ttf-con-$?\r
wait ttf-con- 300
send ls -l /tmp/tf /tmp/tf/go; hexdump -C /tmp/tf/go/11 | tail -3\r
sleep 3
send head -c 5000 /usr/share/fonts/truetype/go.ttf > /usr/share/fonts/truetype/zcut.ttf\r
sleep 1
send grep -v '^font' /etc/zdesk/screen > /tmp/s0; cp /tmp/s0 /tmp/scr; echo font zcut 11 >> /tmp/scr; cp /tmp/scr /etc/zdesk/screen\r
sleep 1
send zdesk -v > /tmp/z1.log 2>&1 &\r
sleep 10
shot bad
send gcontest keys 66 40 c0 e6\r
sleep 1
send gcontest keys 4c cc 44 c4\r
sleep 8
send cp /tmp/s0 /tmp/scr; echo font go 11 >> /tmp/scr; cp /tmp/scr /etc/zdesk/screen\r
sleep 1
send : > /tmp/c; zdesk -v > /tmp/z2.log 2>&1 &\r
wait displayd: font go 11 600
sleep 6
send dpytest -f /tmp/c > /tmp/d2.log 2>&1 &\r
sleep 3
send echo font >> /tmp/c; echo win Zenix 200 60 9 100 60 >> /tmp/c\r
sleep 3
send echo win B 120 40 4 420 120 >> /tmp/c\r
sleep 3
send echo info Zenix >> /tmp/c\r
sleep 3
shot good
send gcontest keys 66 40 c0 e6\r
sleep 1
send gcontest keys 4c cc 44 c4\r
sleep 8
send zdesk -v > /tmp/z3.log 2>&1 &\r
wait displayd: font go 11 120
sleep 4
send gcontest keys 66 40 c0 e6\r
sleep 1
send gcontest keys 4c cc 44 c4\r
sleep 8
send touch /usr/share/fonts/truetype/go.ttf; zdesk -v > /tmp/z4.log 2>&1 &\r
wait displayd: font go 11 600
sleep 4
send gcontest keys 66 40 c0 e6\r
sleep 1
send gcontest keys 4c cc 44 c4\r
sleep 8
send cat /tmp/z1.log /tmp/z2.log /tmp/d2.log /tmp/z3.log /tmp/z4.log; cp /tmp/s0 /etc/zdesk/screen; echo ttf-end-$((40+2))\r
wait ttf-end-42 30
EOS
make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
ZOOMOUT=0 SHOT_LIMIT=600 timeout -k 20 1500 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/f" >/dev/null 2>&1
L=$OUT/serial.txt; tr -d '\r' < "$OUT/f.serial.log" > "$L"
for s in columns bad good; do
    [ -f "$OUT/f-$s.png" ] || { echo "FAIL no screenshot $s (see $OUT/f.serial.log)"; exit 1; }
done
python3 tools/colour-test.py columns "$OUT/f-columns.png" > "$OUT/geom"; rc=$?; check "the text console's geometry ($(head -1 "$OUT/geom"))" $rc
# ttf2font by hand: four sizes and the console's font, with their times
for s in 8 11 13 16; do
    line=$(grep -a -o "^ttf2font: /tmp/tf/go/$s: 191 glyphs, $s lines (baseline [0-9]*), proportional, [0-9.]* s" "$L" | head -1)
    [ -n "$line" ]; check "ttf2font go.ttf $s lines: ${line#ttf2font: }" $?
done
grep -a -q "^ttf-sizes-0" "$L"; check "... ttf2font exits 0" $?
line=$(grep -a -o "^ttf2font: /tmp/go.zcf: [0-9]* glyphs, [0-9]* bytes, [0-9.]* s" "$L" | head -1)
[ -n "$line" ] && grep -a -q "^ttf-con-0" "$L"; check "the console's font: ${line#ttf2font: }" $?
grep -a -q "go.font" "$L" && grep -a -q "5a 54 54 46" "$L"; check "... /tmp/tf/go.font made, the font file ends in its stamp (ZTTF)" $?
# the damaged one
grep -a -q "^ttf2font: /usr/share/fonts/truetype/zcut.ttf: " "$L"; check "a cut .ttf: ttf2font says why ($(grep -a -o "^ttf2font: /usr/share/fonts/truetype/zcut.ttf: .*" "$L" | head -1))" $?
[ "$(grep -a -c "^displayd: /usr/share/fonts/truetype/zcut.ttf: ttf2font could not draw it: the built-in 8x8 is used" "$L")" = 1 ]
check "... displayd says it in one line and uses the built-in 8x8" $?
grep -a -q "^zdesk: running, .*font 8x8 (8 lines: bar 11, title bar 11)" "$L"; check "... zdesk's bar as with the 8 x 8 (11 lines)" $?
# go 11: drawn once, then from the cache, then again after a touch
d=$(grep -a -o "^displayd: /usr/share/fonts/truetype/go.ttf drawn at 11 lines in [0-9.]* s" "$L")
[ "$(echo "$d" | grep -c drawn)" = 2 ]; check "go 11 drawn twice in three starts (the first, and after the .ttf changed): $(echo "$d" | head -1 | sed 's/^displayd: //')" $?
[ "$(grep -a -c "^displayd: font go 11: 11 lines, proportional, 191 glyphs" "$L")" = 3 ]
check "... read in all three starts (11 lines, proportional, 191 glyphs)" $?
grep -a -q "^zdesk: running, .*font go 11 (11 lines: bar 14, title bar 14)" "$L"; check "zdesk: the bar and title bars 3 lines taller (14)" $?
info() { grep -a -o "dpytest: info $1 [-0-9 ]*" "$L" | tail -1 | cut -d' ' -f4-9; }
read -r fx fy _ _ ix iy < <(info Zenix)
# the inactive window's title (B has the focus): the text from the frame's x + 6, its line 0 at y + 2 (as
# tools/font-test.sh), against the host's renderer (the same files, read back by the same reader). The screenshot
# shows an Amiga pixel 1.28 of its pixels wide, so a stroke one pixel wide blurs into its neighbours: each Amiga
# pixel of the text's box is read at its middle and counted as ink when it is nearer the inactive title's colour 455
# than the bar's BBC (the flat theme). At most 2% may fall on the wrong side (a middle near a blurred edge; measured
# 3 of 484), and the text one pixel left, right, up or down must be at least 10 times as wrong: the glyphs are those
# pixels at that place.
if [ -n "$fy" ] && "$OUT/tht" text display/ttf/go.ttf 11 Zenix > "$OUT/want"; then
    python3 - "$OUT/want" "$OUT/f-good.png" "$OUT/geom" "$((fx + 6))" "$((fy + 2))" > "$OUT/p3" <<'EOF2'
import sys
from PIL import Image
want, shot, geom, X0, Y0 = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
on = {tuple(int(v) for v in l.split()) for l in open(want) if not l.startswith("width")}
W = int(next(l for l in open(want) if l.startswith("width")).split()[1])
x0, x1, yc, h = (float(v) for v in open(geom).read().split()[:4])
pw, top, lh = (x1 - x0 + 1) / 640, yc - 7.5 * h, h / 8
px = Image.open(shot).convert("RGB").load()
ink, bg = (0x44, 0x55, 0x55), (0xBB, 0xBB, 0xCC)
def dark(x, y):
    p = px[int(x0 + (x + 0.5) * pw), int(top + (y + 0.5) * lh)]
    return sum(bg[i] - p[i] for i in range(3)) / sum(bg[i] - ink[i] for i in range(3))
def wrong(dx, dy):
    return sum((dark(X0 + dx + x, Y0 + dy + y) >= 0.5) != ((x, y) in on) for y in range(11) for x in range(W))
w0, n = wrong(0, 0), 11 * W
shifted = min(wrong(dx, dy) for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)))
print("%d of %d pixels on the wrong side, %d ink; moved a pixel: at least %d" % (w0, n, len(on), shifted))
sys.exit(0 if 50 * w0 <= n and shifted >= 10 * max(w0, 1) else 1)
EOF2
    rc=$?
else rc=1; fi
check "the title Zenix in go 11 as the host renders it: $(cat "$OUT/p3" 2>/dev/null)" $rc
exit $((1 - pass))
