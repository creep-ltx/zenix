#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Zenix Desktop's window manager and terminal (DESIGN D38 steps 5-6) in FS-UAE through the cage rig (tools/shot.py), on
# the stock A1200 with 4 MB of fast RAM (a test build with DIWV=0x38). zdesk is started from the shell (in the
# background, so the shell still takes the script's commands from the serial line); dpytest (-f, commands from a
# file) gives it windows; the keys are typed and the mouse moved through CON_INPUT (gcontest keys, gcontest mouse:
# the real keyboard's and mouse's paths). Screenshots are checked pixel by pixel (tools/dpy-test.py):
#  - the bar (its colour, the clock and memory blocks; a command block, its interval raised to the 1 s floor), the
#    desktop, two windows placed below the bar by zdesk, the active one's title in the active colour
#  - a drag of a title bar by the mouse (an outline on the 68020, the window moved at the release); one of a window
#    with all 16 pens (the outline) far up and left, then right and down: it stops under the bar, then with its title
#    bar above the bottom (D38 "Windows stay on the screen")
#  - the Zenix menu by a click on the bar (it stays), About picked by a click; About's OK by a click
#  - the menus held with the right button, Shell picked: a terminal; a double click on the Shell icon: another
#  - the terminal: a shell on a pty; ls and echo typed into it, their output in a file the test reads
#  - the keys: minimise, workspace 2 and back, a window sent to workspace 3, close (EV_CLOSE to the program), next
#  - Log out from the menu: every program ends, displayd ends, the console is text again
#  - the themes workbench and platinum: the title bars in their colours
# Printed (emulator figures): the memory of displayd, zdesk and term, and a raise's and a move's round trip.
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-zdesk-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-zdesk.XXXXXX)}; mkdir -p "$OUT"
B=$OUT/build
trap 'rm -rf "$B"' EXIT
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
typ() { python3 tools/keycodes.py -c 40 "$1" | while read -r l; do echo "send gcontest keys $l\\r"; echo "sleep 1"; done; }
{
cat <<'EOS'
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send echo acceleration none > /etc/zdesk/mouse\r
sleep 1
send echo 'right memory clock ; cmd 0 echo hello-bar' > /etc/zdesk/bar\r
send : > /tmp/c; zdesk -v > /tmp/z.log 2>&1 &\r
sleep 8
send dpytest -f /tmp/c > /tmp/d.log 2>&1 &\r
sleep 3
send echo win A 200 80 4 >> /tmp/c\r
sleep 2
send echo win B 160 64 9 >> /tmp/c\r
sleep 3
shot wins
send gcontest mouse -110,-103,0 0,0,1 50,60,1 0,0,0\r
sleep 3
shot dragged
send echo pens A >> /tmp/c\r
sleep 2
send gcontest mouse 0,0,1 -128,-128,1 -128,-128,1\r
sleep 2
send gcontest mouse 0,0,0\r
sleep 2
send echo info A >> /tmp/c\r
sleep 2
shot ol-post
send gcontest mouse 20,16,0 0,0,1 127,127,1 127,127,1 0,0,0\r
sleep 3
send echo info A >> /tmp/c\r
sleep 2
send gcontest mouse -87,-85,0 -87,-85,0\r
sleep 1
send gcontest mouse -90,-80,0 0,0,1 0,0,0\r
sleep 2
shot menuclick
send gcontest mouse 5,52,0 0,0,1 0,0,0\r
sleep 3
shot about
send gcontest mouse 93,45,0 92,44,0 0,0,1 0,0,0\r
sleep 2
shot aboutgone
send gcontest mouse 0,0,2\r
sleep 1
send gcontest mouse -95,-71,2 -95,-70,2\r
sleep 1
send gcontest mouse 5,12,2\r
sleep 1
send gcontest mouse 0,0,0\r
sleep 6
shot heldmenu
send gcontest keys 66 37 b7 e6\r
sleep 2
send gcontest mouse 5,15,0 0,0,1 0,0,0 0,0,1 0,0,0\r
sleep 6
shot twoterms
EOS
typ 'ls > /tmp/t1\n'
echo "sleep 2"
typ 'echo term-$((6*7)) >> /tmp/t1\n'
cat <<'EOS'
sleep 3
shot typed
send gcontest keys 66 37 b7 e6\r
sleep 2
send gcontest keys 66 02 82 e6\r
sleep 2
shot ws2
send gcontest keys 66 01 81 e6\r
sleep 3
send gcontest keys 66 42 c2 e6\r
sleep 1
send gcontest keys 66 60 03 83 e0 e6\r
sleep 2
send gcontest keys 66 10 90 e6\r
sleep 2
send echo time A 10 >> /tmp/c; echo mem >> /tmp/c; echo events >> /tmp/c\r
sleep 8
send cat /tmp/t1; free; ps\r
sleep 4
shot before-logout
send gcontest keys 66 40 c0 e6\r
sleep 1
send gcontest keys 4c cc 44 c4\r
sleep 8
shot text
send ps\r
sleep 2
send rm /etc/zdesk/bar; : > /tmp/c; zdesk -theme workbench > /tmp/z2.log 2>&1 &\r
sleep 6
send dpytest -f /tmp/c > /tmp/d2.log 2>&1 &\r
sleep 3
send echo win A 200 80 4 >> /tmp/c\r
sleep 3
shot workbench
send gcontest keys 66 40 c0 e6\r
sleep 1
send gcontest keys 4c cc 44 c4\r
sleep 6
send : > /tmp/c; zdesk -theme platinum > /tmp/z3.log 2>&1 &\r
sleep 6
send dpytest -f /tmp/c > /tmp/d3.log 2>&1 &\r
sleep 3
send echo win A 200 80 4 >> /tmp/c\r
sleep 3
shot platinum
send gcontest keys 66 40 c0 e6\r
sleep 1
send gcontest keys 4c cc 44 c4\r
sleep 6
send cat /tmp/z.log /tmp/d.log; echo zd-end-$((40+2))\r
wait zd-end-42 30
EOS
} > "$OUT/script.txt"
make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
ZOOMOUT=0 SHOT_LIMIT=600 timeout -k 20 900 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/z" >/dev/null 2>&1
L=$OUT/serial.txt; tr -d '\r' < "$OUT/z.serial.log" > "$L"
for s in columns wins dragged menuclick about aboutgone heldmenu twoterms typed ws2 before-logout text workbench platinum; do
    [ -f "$OUT/z-$s.png" ] || { echo "FAIL no screenshot $s (see $OUT/z.serial.log)"; exit 1; }
done
python3 tools/colour-test.py columns "$OUT/z-columns.png" > "$OUT/geom"; rc=$?; check "the text console's geometry ($(head -1 "$OUT/geom"))" $rc
px() { s=$1; shift; python3 tools/dpy-test.py px "$OUT/z-$s.png" "$OUT/geom" "$@"; }
grep -q "^zdesk: running, theme flat, [0-9]* keys, 2 icons, drag outline" "$L"; check "zdesk runs: the flat theme, the keys, two icons, the outline drag on a 68020" $?
px wins 300,5,112 400,200,344 100,25,BBC 150,37,47B 120,72,D54; check "the bar, the desktop, A and B placed below the bar, B active" $?
grep -q "^zdesk: bar right: chip [0-9.]*[KMG] fast [0-9.]*[KMG]  [0-9][0-9]:[0-9][0-9]  hello-bar" "$L"
check "the bar's right zone: memory, the clock, a command block's line" $?
grep -q "zdesk: the bar: a command every 0 s: every 1 s, the least" "$L"; check "... a command block's interval raised to the 1 s floor" $?
px dragged 200,150,47B 40,100,344; check "A's title dragged by the mouse: A moved 100 right and 60 down, its old place the desktop" $?
# D38 "Windows stay on the screen": A with all 16 pens (the chips cannot lift it: the outline) dragged far up and left
read -r ox oy _ < <(grep -a -o "dpytest: info A [-0-9 ]*" "$L" | head -1 | cut -d' ' -f4-5)
[ "$ox,$oy" = "7,11" ]; check "A dragged by an outline far up and left: dropped at $ox,$oy (7,11: its title bar under the bar, on the screen)" $?
px ol-post 100,17,47B 300,5,112; check "... its title bar shown (active) below the bar" $?
read -r ox oy _ < <(grep -a -o "dpytest: info A [-0-9 ]*" "$L" | sed -n 2p | cut -d' ' -f4-5)
[ "$ox,$oy" = "431,244" ]; check "... then far right and down: dropped at $ox,$oy (431,244: wholly across, its title bar above the bottom)" $?
px menuclick 80,13,EEE 80,53,EEE; check "a click on Zenix: the menu opens below it and stays" $?
px about 420,100,EEE 380,84,47B; check "About picked with a click: its window, active" $?
px aboutgone 420,100,344; check "About's OK clicked: it is gone" $?
px heldmenu 300,100,112; check "the right button held, Shell picked: a terminal (its dark inside)" $?
grep -q "^term-42" "$L" && grep -q "^bin$" "$L"; check "the terminal runs a shell: ls and echo typed into it, their output in the file" $?
px twoterms 300,100,112; check "the terminal minimised, a double click on the Shell icon: another terminal" $?
px ws2 300,100,344 150,150,344; check "minimised, then workspace 2: nothing but the desktop" $?
grep -q "^ev 7 " "$L"; check "Amiga+Q on a dpytest window: its program gets EV_CLOSE" $?
grep -q "^zdesk: workspace 2" "$L" && grep -q "^zdesk: workspace 1" "$L"; check "the workspaces switched by the keys" $?
grep -q "^zdesk: leaving" "$L" && grep -q "display: console 1 given back" "$L"; check "Log out: zdesk leaves, the console is given back" $?
px text 400,200,000; check "... and is text again" $?
px workbench 100,25,68B; check "the workbench theme: the active title Workbench blue" $?
px platinum 100,22,DDD 400,200,67A; check "the platinum theme: the active title platinum, the desktop blue" $?
grep "^dpytest: time\|^dpytest: mem\|^chip RAM free\|^fast RAM free\| displayd$\| zdesk$\| term$" "$L" | head -12
exit $((1 - pass))
