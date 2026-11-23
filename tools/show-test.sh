#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The picture viewer, show (DESIGN D50, docs/d50-viewers.md), in FS-UAE through the cage rig (tools/shot.py) on the
# stock A1200 with 4 MB of fast RAM (a test build with DIWV=0x38), as root, under zdesk with the flat theme. The
# pictures are tools/mkpics.py's (the test images' /tests/pics): bars.iff (lores, 16 blocks in the theme's own pens,
# so the screen shows exactly them), broken.iff (no BMHD), dots.gif, ham.iff (HAM6), notes.txt (not a picture),
# wide.iff (hires lace, 800 x 600). `show -v` writes what it shows and where (show: shown NAME CW x CH at OX OY in
# WW x WH), its window's place, what it says; the pixels are read from the screenshots (tools/dpy-test.py whole: each
# block of pens 9-14 one rectangle of the size and place the scaling gives):
#  - bars.iff shrunk to fit the window (616 x 123): the blocks pixel for pixel where they must be
#  - n: broken.iff refused in its window ("damaged (no BMHD)"), n: dots.gif (square pixels: half as tall), n:
#    ham.iff (HAM6), n: notes.txt skipped, wide.iff (hires, lace); n at the end: "the last picture"
#  - z: wide.iff at its real size (800 x 300), Right: moved left by a quarter of the window; d: Floyd-Steinberg, d:
#    the nearest pen;
#    gg: the first again; a click on the picture: the next (broken.iff); the keys file written with the built-in keys
#  - the keys file changed (a key added, a bad line): the bad line said, the new key works; q quits
#  - files on /tests/pics, Return on bars.iff: files starts /bin/show (no terminal), and it shows the blocks on a screen of its own
#  - show with no option (DESIGN D55 section 11; its own screen is the default, show -w the window): ham.iff on a screen of its own, every second pixel of every fourth line the
#    generator's own HAM6 decode (tools/screen-test.py ham); s: the window again; a HAM8 picture likewise; Esc
# Printed: the program's size and memory (ps), the rendering's time (emulator figures).
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-show-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-show.XXXXXX)}; mkdir -p "$OUT"
B=$OUT/build
trap 'rm -rf "$B"' EXIT
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
typ() { python3 tools/keycodes.py -c 40 "$1" | while read -r l; do echo "send gcontest keys $l\\r"; echo "sleep 1"; done; }
key() { echo "send gcontest keys $1\\r"; echo "sleep ${2:-2}"; }      # raw codes (4e ce Right)
st() { echo "send t $1 ${2:-6}\\r"; echo "sleep 2"; }  # t: a shell function, defined first
clk() {         # a left click at screen pixel X, Y (files-test's way: the pointer to the top left, then counts)
    local x=$(($1 >> 1)) y=$2 m="-128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0" b="0,0,1 0,0,0"
    while [ "$x" -gt 0 ] || [ "$y" -gt 0 ]; do
        local dx=$((x > 127 ? 127 : x)) dy=$((y > 127 ? 127 : y))
        m="$m $dx,$dy,0"; x=$((x - dx)); y=$((y - dy))
    done
    echo "send gcontest mouse $m $b\\r"
    echo "sleep 5"
}
# the first window zdesk places: bars.iff's 616 x 128 window's inside at 16,31 (zdesk centres one this wide)
IX=16 IY=31
{
cat <<'EOS'
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send t() { echo st-$1; grep '^show: (shown|picture|say|window|rendered|wrote|screen)' /tmp/s.log | tail -$2; }\r
sleep 2
send rm -rf $HOME/.config/show; echo acceleration none > /etc/zdesk/mouse; echo speed 200 >> /etc/zdesk/mouse; zdesk -v > /tmp/z.log 2>&1 &\r
sleep 8
send w() { n=0; until grep -q "$1" /tmp/s.log || [ $n -gt 90 ]; do sleep 1; n=$((n+1)); done; echo rdy-$2; }\r
sleep 2
send show -w -v /tests/pics/bars.iff > /tmp/s.log 2>&1 &\r
sleep 2
send w 'shown bars' $((40+1))\r
wait rdy-41 120
sleep 2
shot bars
EOS
st 1 8
typ n; echo "sleep 3"; st 2 3; echo "shot broken"
typ n; echo "sleep 4"; st 3 3; echo "shot dots"
typ n; echo "sleep 5"; st 4 3; echo "shot ham"
typ n; echo "sleep 8"; st 5 4; echo "shot wide"
typ n; echo "sleep 2"; st 6 2
typ z; echo "sleep 2"; echo "send w 'real size' \$((40+2))\\r"; echo "wait rdy-42 120"; st 7 3
key "4e ce" 4; st 8 2; echo "shot pan"
typ d; echo "sleep 2"; echo "send w 'Floyd' \$((40+3))\\r"; echo "wait rdy-43 120"; st 9 3
typ d; echo "sleep 8"; st 17 3
typ gg; echo "sleep 8"; st 10 3
clk $((IX + 300)) $((IY + 60)); st 11 3
typ q; echo "sleep 3"
cat <<'EOS'
send echo st-12; pgrep show || echo show-gone; grep -c . $HOME/.config/show/keys; grep -x 'gg first' $HOME/.config/show/keys | cat\r
sleep 2
send printf 'x next\\nZ frob\\n' >> $HOME/.config/show/keys; show -w -v /tests/pics/bars.iff > /tmp/s.log 2>&1 &\r
sleep 12
EOS
st 13 4
typ x; echo "sleep 4"; st 14 2
typ q; echo "sleep 3"
cat <<'EOS'
send files -v /tests/pics > /tmp/f.log 2>&1 &\r
sleep 8
send gcontest keys 44 c4\r
sleep 14
shot fromfiles
send echo st-15; grep 'files: start' /tmp/f.log | cat; pgrep show | cat; pgrep term || echo no-term\r
sleep 2
send echo st-16; ps | grep show | cat; ls -l /bin/show\r
sleep 3
EOS
typ q; echo "sleep 3"; typ q; echo "sleep 3"       # off the screen, then quit show (files started it)
cat <<'EOS'
send show -v /tests/pics/ham.iff > /tmp/s.log 2>&1 &\r
sleep 14
shot hamscr
EOS
typ s; echo "sleep 4"; echo "shot hamwin"; st 18 6
typ q; echo "sleep 3"
cat <<'EOS'
send show -s -v /tests/pics8/ham8.iff > /tmp/s.log 2>&1 &\r
sleep 14
shot ham8scr
send gcontest keys 45 c5\r
sleep 4
EOS
st 19 6
typ q; echo "sleep 3"
echo "send cat /tmp/z.log | tail -5; echo end-\$((40+2))\\r"
echo "wait end-42 30"
} > "$OUT/script.txt"
if [ -z "$SHOW_RECHECK" ]; then        # SHOW_RECHECK=1 with the same ZENIX_OUT: the checks again on the last run's files
    make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
        { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
    ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=900 timeout -k 20 1200 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/p" >/dev/null 2>&1
fi
L=$OUT/serial.txt; tr -d '\r' < "$OUT/p.serial.log" > "$L"
python3 tools/colour-test.py columns "$OUT/p-columns.png" > "$OUT/geom"; rc=$?; check "the text console's geometry ($(head -1 "$OUT/geom"))" $rc
after() { awk -v m="st-$1" '$0 == m { on = 1; next } on && /zenix.*# / { exit } on' "$L"; }
shown() { after "$1" | grep -a '^show: shown' | tail -1; }
# bars.iff in shot S: the blocks of pens 9-11 (D54 7C5 EB4: nothing else on the desktop is near them) each one
# rectangle W x H (within the screenshot's blur), the first at X, Y (X empty: wherever it is) and each next W pixels
# right of it; then the middle of each of the 16 blocks in its pen, from there
FLAT="344 112 EEE 223 47B BBC 455 FFF CCD D54 7C5 EB4 69E B7C 5BC 567"
blocks() {
    local s=$1 x=$2 y=$3 w=$4 h=$5 i=0 bad=0 fx= fy=
    for c in D54 7C5 EB4; do
        local o; o=$(python3 tools/dpy-test.py whole "$OUT/p-$s.png" "$OUT/geom" "$c" 0 0)
        local at; at=$(echo "$o" | sed -n 's/^whole: at \([0-9]*\),\([0-9]*\) \([0-9]*\)x\([0-9]*\) (\([0-9]*\)%.*/\1 \2 \3 \4 \5/p')
        local bx= by= gw= gh= pc= r=0; read -r bx by gw gh pc <<< "$at"
        [ -n "$at" ] && [ $((gw - w)) -le 2 ] && [ $((w - gw)) -le 2 ] && [ $((gh - h)) -le 1 ] && [ $((h - gh)) -le 1 ] &&
            [ "$pc" -ge 98 ] || r=1
        [ -z "$fx" ] && { fx=${x:-$bx}; fy=${y:-$by}; }
        local ex=$((fx + i * w))
        if [ "$r" != 0 ] || [ $((bx - ex)) -gt 1 ] || [ $((ex - bx)) -gt 1 ] || [ $((by - fy)) -gt 1 ] || [ $((fy - by)) -gt 1 ]; then
            echo "block $c: $o (wanted at $ex,$fy ${w}x$h)"; bad=1
        fi
        i=$((i + 1))
    done
    local ox=$((fx - w)) oy=$((fy - 61)) spec="" k=0
    for c in $FLAT; do
        spec="$spec $((ox + (k % 8) * w + w / 2)),$((oy + (k / 8) * 62 + 30)),$c"; k=$((k + 1))
    done
    local p; p=$(python3 tools/dpy-test.py px "$OUT/p-$s.png" "$OUT/geom" $spec) || { echo "$p"; bad=1; }
    [ "$bad" = 0 ] && echo "blocks ${w}x$h side by side from $fx,$fy; the 16 in their pens ($p)"
    return $bad
}
w=$(after 1 | grep -a -m1 '^show: window'); s1=$(shown 1)
echo "$s1" | grep -q '^show: shown bars.iff 616 x 123 at 0 2 in 616 x 128 (fit)'
rc=$?; check "bars.iff (320 x 128 lores) shrunk to fit: 616 x 123 in a 616 x 128 window ($s1)" $rc
[ "$(echo "$w" | cut -d' ' -f3-4)" = "$IX $IY" ]; check "show's window where zdesk puts the first one ($w)" $?
after 1 | grep -q '^show: picture bars.iff 320 128 16 colours$'; check "... its mode: 16 colours" $?
blocks bars $((IX + 77)) $((IY + 2 + 61)) 77 62 > "$OUT/q1"; rc=$?; check "... drawn: the blocks pixel for pixel ($(cat "$OUT/q1"))" $rc
after 2 | grep -q '^show: picture broken.iff: damaged (no BMHD)'; check "n: broken.iff refused, said in its window" $?
python3 tools/dpy-test.py px "$OUT/p-broken.png" "$OUT/geom" $((IX + 600)),$((IY + 120)),112 > "$OUT/q2"; rc=$?
check "... the window in the darkest pen ($(cat "$OUT/q2"))" $rc
after 3 | grep -q '^show: picture dots.gif 160 120 16 colours$' && shown 3 | grep -q 'shown dots.gif 160 x 60 at 228 34 in 616 x 128 (fit)'
rc=$?; check "n: dots.gif, square pixels half as tall: 160 x 60, centred ($(shown 3))" $rc
python3 tools/dpy-test.py px "$OUT/p-dots.png" "$OUT/geom" $((IX + 228 + 10)),$((IY + 34 + 7)),344 $((IX + 228 + 30)),$((IY + 34 + 7)),112 \
    $((IX + 228 + 10)),$((IY + 34 + 22)),112 > "$OUT/q3"; rc=$?; check "... its blocks in their pens ($(cat "$OUT/q3"))" $rc
after 4 | grep -q '^show: picture ham.iff 320 64 HAM6$'; rc=$?; check "n: ham.iff, HAM6 ($(shown 4))" $rc
after 5 | grep -q '^show: picture wide.iff 800 600 8 colours, hires, lace$' && ! after 5 | grep -q notes.txt
rc=$?; check "n: notes.txt skipped, wide.iff: 8 colours, hires, lace ($(shown 5))" $rc
after 6 | grep -q '^show: say: the last picture'; check "n at the end: the last picture" $?
shown 7 | grep -q '^show: shown wide.iff 800 x 300 at 0 0 in 616 x 128 (real size)'; rc=$?; check "z: the real size, 800 x 300 ($(shown 7))" $rc
shown 8 | grep -q '^show: shown wide.iff 800 x 300 at -160 0 in 616 x 128 (real size)'; rc=$?; check "Right: moved left by a quarter of the window, in 16s ($(shown 8))" $rc
after 9 | grep -q '^show: rendered 800 x 300 in [0-9]* ms (Floyd-Steinberg)'; rc=$?; check "d: Floyd-Steinberg ($(after 9 | grep rendered | tail -1))" $rc
after 17 | grep -q '^show: rendered 800 x 300 in [0-9]* ms (nearest)'; rc=$?; check "d again: the nearest pen ($(after 17 | grep rendered | tail -1))" $rc
shown 10 | grep -q '^show: shown bars.iff'; check "gg: the first picture again" $?
after 11 | grep -q '^show: picture broken.iff'; check "a click on the picture: the next (broken.iff)" $?
s12=$(after 12); echo "$s12" | grep -qx 'gg first' && echo "$s12" | grep -qx 'show-gone' && [ "$(echo "$s12" | grep -x '[0-9]*' | tail -1)" -gt 20 ]
rc=$?; check "q: show ended; ~/.config/show/keys written with the built-in keys ($(echo "$s12" | grep -x '[0-9]*' | tail -1) lines)" $rc
after 13 | grep -q '^show: say: keys: line [0-9]*: unknown action frob'; check "the keys file changed: its bad line said" $?
after 14 | grep -q '^show: picture broken.iff'; check "... its new key x: next" $?
s15=$(after 15); echo "$s15" | grep -q '^files: start /bin/show /tests/pics/bars.iff' && echo "$s15" | grep -qx 'no-term'
rc=$?; check "files, Return on bars.iff: /bin/show started, no terminal ($(echo $s15))" $rc
spec=""; k=0; for c in $FLAT; do                  # bars.iff on a screen of its own: lores, 8 blocks of 40 x 64 a row
    [ ${#c} = 3 ] && c=${c:0:1}${c:0:1}${c:1:1}${c:1:1}${c:2:1}${c:2:1}
    spec="$spec $(((k % 8) * 40 + 20)),$(((k / 8) * 64 + 32)),$c"; k=$((k + 1)); done
python3 tools/screen-test.py px "$OUT/p-fromfiles.png" "$OUT/geom" 320 $spec > "$OUT/q4"; rc=$?
check "... shows the picture on a screen of its own: the 16 blocks in their pens ($(cat "$OUT/q4"))" $rc
after 18 | grep -q '^show: screen 320 x 64, 6 planes (HAM6)' && after 18 | grep -q '^show: screen closed'
rc=$?; python3 tools/screen-test.py ham "$OUT/p-hamscr.png" "$OUT/geom" 6 > "$OUT/q5"; r2=$?
check "show (no option): ham.iff on a screen of its own by default, HAM6 in full colour ($(cat "$OUT/q5")); s: back to the window" $((rc | r2))
python3 tools/dpy-test.py same "$OUT/p-hamwin.png" "$OUT/p-hamscr.png" "$OUT/geom" > /dev/null; r3=$?
[ $r3 != 0 ]; check "... the window again after s (not the screen)" $?
after 19 | grep -q '^show: screen 320 x 64, 8 planes (HAM8)'; rc=$?
python3 tools/screen-test.py ham "$OUT/p-ham8scr.png" "$OUT/geom" 8 > "$OUT/q6"; r2=$?
check "show -s (the default, still accepted): a HAM8 picture on its own screen ($(cat "$OUT/q6")); Esc: back" $((rc | r2))
echo "emulator figures (FS-UAE, stock A1200 + 4 MB fast; not a real machine's):"
grep -a '^show: rendered' "$L" | sort -u | sed 's/^/  /'
after 16 | grep -a 'show' | sed 's/^/  /'
exit $((1 - pass))
