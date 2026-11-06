#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The display server and its library (DESIGN D38 steps 3-4) in FS-UAE through the cage rig (tools/shot.py), on the
# stock A1200 with 4 MB of fast RAM (the minimum D38 is made for; a test build with DIWV=0x38): dpytest
# (docs/man/dpytest.txt) is started from the shell, which starts displayd (the "display" service) on console 1;
# commands typed into dpytest open, draw, move, raise, lower, scroll, hide, show and close windows, and the
# screenshots are checked pixel by pixel (tools/dpy-test.py):
#  - two windows, the second over the first: the frames, the insides' colours, the desktop around them
#  - moved (the window manager's DPY_WM_PLACE): the old place is the desktop again, the window at the new place drawn
#    from its kept copy, under the other where they overlap
#  - raised, lowered and raised again, hidden and shown: always from the kept copy - the program is asked to draw
#    (EV_EXPOSE) only once for each window, when it first appeared (smart refresh, D38 3.2)
#  - scrolled (O_COPY) and the freed lines filled; a broken order refused with its offset
#  - a window opened at 900,400 and one placed at -300,-50 and 600,250 kept on the screen (D38 "Windows stay on the
#    screen")
#  - closed: the desktop again; the last client gone: displayd ends and the console is text again
# Measured and printed (emulator figures, not hardware speeds): the round trip of a raise and of a move, and the memory
# of displayd and dpytest with two windows.
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-dpy-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-dpy.XXXXXX)}; mkdir -p "$OUT"
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
send free\r
sleep 2
send dpytest\r
wait dpytest: open 60
sleep 2
send win A 200 80 4\r
sleep 2
send win B 160 64 9\r
sleep 2
send info A\r
send info B\r
send mem\r
sleep 2
shot two
send win C 100 40 5 900 400\r
sleep 2
send info C\r
send close C\r
sleep 1
send wm\r
send move A -300 -50\r
sleep 1
send info A\r
send move A 600 250\r
sleep 1
send info A\r
send move A 100 100\r
sleep 2
shot moved
send raise A\r
sleep 2
shot raised
send lower A\r
sleep 1
send raise A\r
sleep 1
send hide A\r
sleep 1
send show A\r
sleep 2
shot shown
send scroll A 16\r
send bad A\r
sleep 2
shot scrolled
send time A 10\r
wait dpytest: time 60
sleep 1
send close B\r
sleep 2
shot closed
send events\r
wait dpytest: events end 30
send quit\r
sleep 4
shot text
send echo dpy-end-$((40+2))\r
wait dpy-end-42 30
EOS
make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
ZOOMOUT=0 SHOT_LIMIT=300 timeout -k 20 600 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/d" >/dev/null 2>&1
L=$OUT/serial.txt; tr -d '\r' < "$OUT/d.serial.log" > "$L"
for s in columns two moved raised shown scrolled closed text; do
    [ -f "$OUT/d-$s.png" ] || { echo "FAIL no screenshot $s (see $OUT/d.serial.log)"; exit 1; }
done
python3 tools/colour-test.py columns "$OUT/d-columns.png" > "$OUT/geom"; rc=$?; check "the text console's geometry ($(head -1 "$OUT/geom"))" $rc
px() { s=$1; shift; python3 tools/dpy-test.py px "$OUT/d-$s.png" "$OUT/geom" "$@"; }
grep -q "^svc: display: started /sbin/displayd" "$L" && grep -q "^dpytest: open 0 screen 640x256 depth 4" "$L"
check "dpytest starts displayd (the display service) and says hello: 640 x 256, 4 planes" $?
grep -q "^dpytest: info A 23 16 200 80 24 28 1" "$L" && grep -q "^dpytest: info B 39 28 160 64 40 40 1" "$L"
check "no window manager: the windows placed one below the other, the insides on 8-pixel columns" $?
grep -q "^dpytest: info C 535 244 100 40 " "$L"
check "a window asking for 900,400 opens on the screen: wholly across, its title bar above the bottom (535,244)" $?
grep -q "^dpytest: info A 7 0 200 80 " "$L" && grep -q "^dpytest: info A 431 244 200 80 " "$L"
check "placed at -300,-50 and at 600,250: kept on the screen (7,0 and 431,244; D38 'Windows stay on the screen')" $?
px two 120,72,D54 210,60,47B 400,200,344 100,20,BBC; check "two windows: B over A, A's uncovered part, the desktop, A's title" $?
px moved 210,60,344 30,20,344 250,150,47B 120,72,D54 150,102,D54; check "A moved: its old place is the desktop, it is under B where they overlap" $?
px raised 150,102,BBC 250,150,47B; check "A raised: in front of B" $?
px shown 150,102,BBC 250,150,47B 120,72,D54; check "lowered, raised, hidden, shown: A as it was" $?
px scrolled 250,185,EEE 250,150,47B; check "scrolled 16 lines up (O_COPY), the freed lines filled" $?
grep -q "^dpytest: bad -22 at 12" "$L"; check "a broken order is refused with its offset (12 bytes)" $?
px closed 120,72,344 250,150,47B; check "B closed: the desktop where it was" $?
[ "$(grep -c '^ev 5 A ' "$L")" = 1 ] && [ "$(grep -c '^ev 5 B ' "$L")" = 1 ]
rc=$?; check "smart refresh: each program asked to draw once, when its window appeared ($(grep -c '^ev 5 ' "$L") EV_EXPOSE)" $rc
grep -q "^svc: display: displayd (pid [0-9]*) stopped\|display: console 1 given back" "$L"; check "the last client gone: displayd ends, the console given back" $?
px text 400,200,000; check "... and the console is text again" $?
grep "^dpytest: time\|^dpytest: mem\|^chip RAM free\|^fast RAM free" "$L" | head -8
exit $((1 - pass))
