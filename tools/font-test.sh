#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The desktop's font (docs/d38-desktop.md "Fonts"): an AmigaOS bitmap font in /usr/share/fonts/amiga, named by
# /etc/zdesk/screen's "font NAME SIZE", read by displayd and used for the window titles and zdesk's bar. In FS-UAE
# through the cage rig (tools/shot.py) on the stock A1200 with 4 MB of fast RAM (a test build with DIWV=0x38). The
# fonts are tools/mkamifont.py's (the test images' /tests/fonts): ztest 11 (proportional blocks: a glyph is a block
# 2 + code % 5 pixels wide on lines 1-8, drawn a pixel right of the pen, the pen moving its width + 2; "W" kerned 2;
# no "."), and zbad 11 (its file cut short).
#  - first zbad: displayd says why it is refused, in one line, and the desktop runs with the 8 x 8: the bar 11 lines;
#  - then ztest: displayd says it read it; DPY_FONT_INFO's widths (dpytest's font) are the font's; zdesk's bar and
#    title bars are 3 lines taller (11 - 8); a window titled "D.D" shows its title in the font where the server's
#    place for it and the font's widths say - the first D's block, the "." from the 8 x 8 (its dot low, at the
#    font's baseline: the middle lines empty), the second D 8 pixels after the first's advance;
#  - text a client draws in the font (dpytest's utext) at its place, the same blocks.
# Pixels are read with tools/dpy-test.py on a geometry from the text console (colour-test.py columns).
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-font-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-font.XXXXXX)}; mkdir -p "$OUT"
B=$OUT/build
trap 'rm -rf "$B"' EXIT
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
cat > "$OUT/script.txt" <<'EOS'
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send cp -r /tests/fonts/* /usr/share/fonts/amiga/; echo acceleration none > /etc/zdesk/mouse; echo speed 200 >> /etc/zdesk/mouse; printf 'right cmd 3600 echo .\\n' > /etc/zdesk/bar\r
sleep 2
send grep -v '^font' /etc/zdesk/screen > /tmp/s0; cp /tmp/s0 /tmp/scr; echo font zbad 11 >> /tmp/scr; cp /tmp/scr /etc/zdesk/screen\r
sleep 1
send : > /tmp/c; zdesk -v > /tmp/z1.log 2>&1 &\r
sleep 8
send dpytest -f /tmp/c > /tmp/d1.log 2>&1 &\r
sleep 3
send echo font >> /tmp/c; echo win A 200 60 9 200 60 >> /tmp/c\r
sleep 3
shot bad
send gcontest keys 66 40 c0 e6\r
sleep 1
send gcontest keys 4c cc 44 c4\r
sleep 8
send cp /tmp/s0 /tmp/scr; echo font ztest 11 >> /tmp/scr; cp /tmp/scr /etc/zdesk/screen\r
sleep 1
send : > /tmp/c; zdesk -v > /tmp/z2.log 2>&1 &\r
sleep 8
send dpytest -f /tmp/c > /tmp/d2.log 2>&1 &\r
sleep 3
send echo font >> /tmp/c; echo win D.D 200 60 9 100 60 >> /tmp/c\r
sleep 3
send echo win B 120 40 4 420 120 >> /tmp/c\r
sleep 3
send echo utext B 10 10 DW >> /tmp/c; echo info D.D >> /tmp/c; echo info B >> /tmp/c\r
sleep 3
shot good
send cat /tmp/z1.log /tmp/d1.log /tmp/z2.log /tmp/d2.log; cp /tmp/s0 /etc/zdesk/screen; echo font-end-$((40+2))\r
wait font-end-42 30
EOS
make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
ZOOMOUT=0 SHOT_LIMIT=300 timeout -k 20 600 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/f" >/dev/null 2>&1
L=$OUT/serial.txt; tr -d '\r' < "$OUT/f.serial.log" > "$L"
for s in columns bad good; do
    [ -f "$OUT/f-$s.png" ] || { echo "FAIL no screenshot $s (see $OUT/f.serial.log)"; exit 1; }
done
python3 tools/colour-test.py columns "$OUT/f-columns.png" > "$OUT/geom"; rc=$?; check "the text console's geometry ($(head -1 "$OUT/geom"))" $rc
px() { s=$1; shift; python3 tools/dpy-test.py px "$OUT/f-$s.png" "$OUT/geom" "$@"; }
# zbad: refused in one line, the 8 x 8 kept
[ "$(grep -a -c "^displayd: /usr/share/fonts/amiga/zbad/11: cut short in its hunk: the built-in 8x8 is used" "$L")" = 1 ]
check "a damaged font: one line saying why, the built-in 8x8 used" $?
grep -a -q "^dpytest: font 0 0 8 6 8 2 - widths 8 8 8 8 8" "$L"; check "... DPY_FONT_INFO gives the 8 x 8 (id 0, 8 x 8, built in)" $?
grep -a -q "^zdesk: running, .*font 8x8 (8 lines: bar 11, title bar 11)" "$L"; check "... zdesk's bar and title bars as before (11 lines)" $?
px bad 600,10,112 600,11,344 > "$OUT/p1"; rc=$?; check "... the bar's line at line 10, the desktop from 11 ($(cat "$OUT/p1"))" $rc
# ztest
grep -a -q "^displayd: font ztest 11: 11 lines, proportional, 99 glyphs in [0-9]* bytes" "$L"
check "ztest 11 read: 11 lines, proportional, 99 glyphs" $?
grep -a -q "^dpytest: font 0 1 11 8 8 1 ztest 11 widths 4 7 7 8 4" "$L"
check "DPY_FONT_INFO: id 1, 11 lines, baseline 8, proportional, the widths of A D W (the font's), . (the 8 x 8's) and space" $?
grep -a -q "^zdesk: running, .*font ztest 11 (11 lines: bar 14, title bar 14)" "$L"; check "zdesk: the bar and the title bars 3 lines taller (14)" $?
px good 600,13,112 600,14,344 > "$OUT/p2"; rc=$?; check "the bar's line at line 13, the desktop from 14 ($(cat "$OUT/p2"))" $rc
info() { grep -a -o "dpytest: info $1 [-0-9 ]*" "$L" | tail -1 | cut -d' ' -f4-9; }
read -r fx fy _ _ ix iy < <(info D.D)
read -r bx by _ _ bix biy < <(info B)
[ -n "$fy" ] && [ $((iy - fy)) = 15 ]; check "the title bar 3 lines taller: the inside 15 lines below the frame's top (12 with the 8 x 8): $fx,$fy inside $ix,$iy" $?
# the title "D.D" of the inactive window (B has the focus): the text at the frame's x + 6 (after the border and the
# gap), its line 0 at y + 2 (the 14-line bar's room); its line 4 (y + 6): the first D's block (x0+1..x0+5), then
# the 8 x 8's "." (pen x0+7..x0+14, its dot below line 4), then the second D (x0+16..x0+20); the bar's colour
# BBC, the inactive title's 455 (the flat theme)
if [ -n "$fy" ]; then
    x0=$((fx + 6)) ty=$((fy + 6))
    px good "$((x0 + 3)),$ty,455" "$((x0 + 10)),$ty,BBC" "$((x0 + 12)),$ty,BBC" "$((x0 + 18)),$ty,455" \
        "$((x0 + 23)),$ty,BBC" "$((x0 + 3)),$((fy + 2)),BBC" "$((x0 + 3)),$((fy + 10)),455" > "$OUT/p3"; rc=$?
else rc=1; fi
check "the title D.D in ztest 11: the D blocks where the widths say, the 8 x 8's . between ($(cat "$OUT/p3" 2>/dev/null))" $rc
# utext B 10 10 DW: D at 11..15, W (kerned 2) at 7 + 2 = 9 past the D's pen: 19..22; lines 1..8 of the text, pen 1
# (112) on 2 (EEE)
if [ -n "$biy" ]; then
    tx=$((bix + 10)) tyy=$((biy + 10 + 4))
    px good "$((tx + 3)),$tyy,112" "$((tx + 7)),$tyy,EEE" "$((tx + 10)),$tyy,112" "$((tx + 13)),$tyy,EEE" \
        "$((tx + 3)),$((biy + 10)),EEE" > "$OUT/p4"; rc=$?
else rc=1; fi
check "a client's text in the font (utext DW): its blocks where the widths say ($(cat "$OUT/p4" 2>/dev/null))" $rc
exit $((1 - pass))
