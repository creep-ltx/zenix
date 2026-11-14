#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Screens of their own (DESIGN D55, docs/d55-screens.md) in FS-UAE through the cage rig (tools/shot.py), on the stock
# A1200 with 4 MB of fast RAM (a test build with DIWV=0x38). dpytest (docs/man/dpytest.txt) reads its commands from a
# file the shell appends to; its screen commands open a screen (two pictures of interleaved planes in one chip RAM
# region of its own), change it, and the screenshots are checked pixel by pixel (tools/screen-test.py):
#  - CON_VBLANK: 100 frames against the E-clock (PAL: 50 a second), a wait for a far count ends after 100 ms
#  - a lores 4-plane screen: dpytest's blocks in its palette; a Copper list of a colour a line (a red ramp in colour 0,
#    lines 0-31); the same colours changed in place (SS_COLOURS: green), a WAIT's place refused; a base colour in place
#  - SS_PLANES: the second picture; a Copper list that mirrors the lower half (pointers at a line, a negative modulo)
#  - eight bad lists, each refused with its instruction's index (-1: the fetch outside the memory)
#  - the planes' memfree refused while shown; Left Amiga+N: the desktop as it was; again: the screen; Amiga+F2 and
#    Amiga+F1: the screen again
#  - closed: the desktop as it was; memory that is not the client's region refused
#  - the fetch measured (a line's bytes): lores and hires at the 64-bit fetch; an interlaced screen on the
#    non-interlaced desktop, then the desktop as it was (COP1LC handed back)
#  - a second client's window, a screen, its program killed: the desktop at once (GEV_GONE), the chip RAM back
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-screen-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-screen.XXXXXX)}; mkdir -p "$OUT"
B=$OUT/build
trap 'rm -rf "$B"' EXIT
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
{
cat <<'EOS'
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send d() { echo "$*" >> /tmp/c; }; e() { echo "$*" >> /tmp/c2; }; : > /tmp/c; : > /tmp/c2\r
sleep 1
send dpytest -f /tmp/c &\r
wait dpytest: open 60
sleep 2
send d win A 200 80 4\r
sleep 3
shot desk
send d vblank 100\r
wait dpytest: vblank 60
send d scr 320 256 4 0 300\r
wait dpytest: scr 30
sleep 2
shot blocks
send d scrcop bars\r
wait dpytest: scrcop bars 30
sleep 2
shot bars
send d scrcol\r
wait dpytest: scrcol 30
send d scrpal 9 FF0000\r
wait dpytest: scrpal 30
sleep 2
shot green
send d scrpal 9 FF88EE\r
wait dpytest: scrpal 30
send d scrplanes 1\r
wait dpytest: scrplanes 30
sleep 2
shot pic1
send d scrplanes 0\r
send d scrcop mirror\r
wait dpytest: scrcop mirror 30
sleep 2
shot mirror
send d scrcop bad1\r
send d scrcop bad2\r
send d scrcop bad3\r
send d scrcop bad4\r
send d scrcop bad5\r
send d scrcop bad6\r
send d scrcop bad7\r
send d scrcop bad8\r
wait dpytest: scrcop bad8 30
send d scrfree\r
wait dpytest: scrfree 30
sleep 1
send gcontest keys 66 36 b6 e6\r
sleep 3
shot behind
send gcontest keys 66 36 b6 e6\r
sleep 3
shot again
send (sleep 4; gcontest keys 66 50 d0 e6) & gcontest keys 66 51 d1 e6\r
sleep 10
shot console
send d scrclose\r
wait dpytest: scrclose 30
sleep 2
shot closed
send d scrnotmine\r
wait dpytest: scrnotmine 30
send d scrclose\r
send d scr 320 256 1 0 0 fetch\r
wait dpytest: scr 30
sleep 2
shot fetch
send d scrclose\r
send d scr 640 256 1 0 0 fetch\r
wait dpytest: scr 30
sleep 2
shot fetchh
send d scrclose\r
send d scr 320 512 4 1 0\r
wait dpytest: scr 30
sleep 2
shot lace
send d scrclose\r
wait dpytest: scrclose 30
sleep 2
shot unlace
send dpytest -f /tmp/c2 &\r
sleep 4
send e win B 160 64 9\r
sleep 3
send e mem\r
sleep 2
send d scr 320 256 4 0 0\r
wait dpytest: scr 30
sleep 2
shot killme
send kill -9 $(pgrep dpytest | head -1); sleep 1; echo killed-$((40+2))\r
wait killed-42 30
sleep 3
shot killed
send e mem\r
sleep 2
send e quit\r
sleep 3
send echo scr-end-$((40+2))\r
wait scr-end-42 30
EOS
} > "$OUT/script.txt"
if [ -z "$SCR_RECHECK" ]; then
    make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
        { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
    ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=600 timeout -k 20 900 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/s" >/dev/null 2>&1
fi
L=$OUT/serial.txt; tr -d '\r' < "$OUT/s.serial.log" > "$L"
for s in columns desk blocks bars green pic1 mirror behind again console closed fetch fetchh lace unlace killme killed; do
    [ -f "$OUT/s-$s.png" ] || { echo "FAIL no screenshot $s (see $OUT/s.serial.log)"; exit 1; }
done
python3 tools/colour-test.py columns "$OUT/s-columns.png" > "$OUT/geom"; rc=$?; check "the text console's geometry ($(head -1 "$OUT/geom"))" $rc
T() { python3 tools/screen-test.py "$1" "$OUT/s-$2.png" "$OUT/geom" "${@:3}"; }
same() { python3 tools/dpy-test.py same "$OUT/s-$1.png" "$OUT/s-$2.png" "$OUT/geom" -x 0,0,640,4; }
v=$(grep -ao -m1 'dpytest: vblank.*' "$L"); read -r _ _ fr _ ticks _ _ far ftk _ <<< "$v"
[ "$fr" = 100 ] && [ "$ticks" -ge 1390000 ] && [ "$ticks" -le 1450000 ] && [ "$far" = 1 ] && [ "$ftk" -ge 65000 ] && [ "$ftk" -le 80000 ]
check "CON_VBLANK: 100 frames in $ticks E-clock ticks (2 s at 50 a second: 1,418,758), a far count given up after $ftk (100 ms)" $?
grep -aq 'dpytest: scr [0-9][0-9]*$' "$L"; rc=$?; check "DPY_SCREEN: a lores 4-plane screen opened, the answer a frame's count ($(grep -ao -m1 'dpytest: scr [0-9-]*' "$L"))" $rc
T blocks blocks 320 4 0 > "$OUT/q1"; rc=$?; check "... its blocks in its palette ($(cat "$OUT/q1"))" $rc
T bars bars r > "$OUT/q2"; rc=$?; check "a Copper list: a red ramp in colour 0 a line ($(cat "$OUT/q2"))" $rc
T blocks bars 320 4 0 32 255 > "$OUT/q2b"; rc=$?; check "... colour 0 its own again from line 32 ($(cat "$OUT/q2b"))" $rc
T bars green g > "$OUT/q3"; grep -aq 'dpytest: scrcol 1 too-many 1' "$L"
rc=$?; check "SS_COLOURS: the ramp green in place (the WAITs passed over); one colour too many refused ($(cat "$OUT/q3"))" $rc
T px green 320 60,48,FF0000 20,48,88DD11 > "$OUT/q3b"
rc=$?; check "SS_PALETTE: colour 9 red in place, its neighbour as it was ($(cat "$OUT/q3b"))" $rc
T blocks pic1 320 4 1 > "$OUT/q4"; rc=$?; check "SS_PLANES: the second picture ($(cat "$OUT/q4"))" $rc
T mirror mirror > "$OUT/q5"; T blocks mirror 320 4 0 0 127 >> "$OUT/q5"; rc=$?; check "a mirror: pointers at line 128, a negative modulo ($(tr '\n' ' ' < "$OUT/q5"))" $rc
want="bad1:1 bad2:-1 bad3:-1 bad4:1 bad5:2 bad6:31 bad7:0 bad8:-1"
got=""; for b in bad1 bad2 bad3 bad4 bad5 bad6 bad7 bad8; do
    got="$got $b:$(grep -a -m1 "dpytest: scrcop $b refused at" "$L" | awk '{print $NF}')"; done
[ "$(echo $got)" = "$want" ]; rc=$?; check "eight bad lists refused with their index ($(echo $got))" $rc
grep -aq 'dpytest: scrfree -16' "$L"; check "the planes' memfree refused while the screen shows them (-EBUSY)" $?
same desk behind > "$OUT/q6"; rc=$?; check "Left Amiga+N: the desktop as it was ($(cat "$OUT/q6"))" $rc
T blocks again 320 4 0 32 127 > "$OUT/q7"; rc=$?; check "Left Amiga+N again: the screen ($(cat "$OUT/q7"))" $rc
T blocks console 320 4 0 32 127 > "$OUT/q8"; rc=$?; check "Amiga+F2, Amiga+F1: the screen again ($(cat "$OUT/q8"))" $rc
same desk closed > "$OUT/q9"; rc=$?; check "DPY_SCR_CLOSE: the desktop as it was ($(cat "$OUT/q9"))" $rc
grep -aq 'dpytest: scrnotmine -22' "$L"; check "planes that are not a region of the client's refused (-EINVAL)" $?
T fetch fetch 320 > "$OUT/q10"; rc=$?; check "the fetch, lores at 64 bits: every line starts where it should ($(cat "$OUT/q10"))" $rc
T fetch fetchh 640 > "$OUT/q11"; rc=$?; check "... hires ($(cat "$OUT/q11"))" $rc
DPY_LINES=256 T blocks lace 320 4 0 0 0 > /dev/null; same desk unlace > "$OUT/q12"
rc=$?; check "an interlaced screen over the hires desktop, then the desktop as it was ($(cat "$OUT/q12"))" $rc
T blocks killme 320 4 0 > "$OUT/q13"; rc=$?; check "a screen in front of a second client's window ($(cat "$OUT/q13"))" $rc
grep -aq 'displayd: a screen.s program ended' "$L"; r1=$?
python3 tools/screen-test.py blocks "$OUT/s-killed.png" "$OUT/geom" 320 4 0 > /dev/null; r2=$?
[ $r1 = 0 ] && [ $r2 != 0 ]; check "its program killed: displayd told at once (GEV_GONE), the screen off the glass" $?
c0=$(grep -ao 'dpytest: mem chip [0-9]*' "$L" | sed -n 1p | awk '{print $4}'); c1=$(grep -ao 'dpytest: mem chip [0-9]*' "$L" | sed -n 2p | awk '{print $4}')
[ -n "$c0" ] && [ -n "$c1" ] && [ "$c1" -ge "$c0" ]; check "... its chip RAM back: $c0 KB free before its screen, $c1 KB after it was killed" $?
grep -ao "dpytest: mem .*" "$L" | sed 's/^/  /'
exit $((1 - pass))
