#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Zenix Desktop draws only what changes (DESIGN D38, the A1200's findings of 2026-11-04 and the owner's bar: "Workbench
# 1.3 didn't have artifacts and felt snappy on an A500"), in FS-UAE through the cage rig (tools/shot.py) on the stock
# A1200 with 4 MB of fast RAM (a test build with DIWV=0x38). The server's counters (DPY_STAT through dpytest's stat)
# and screenshots compared pixel by pixel (tools/dpy-test.py same):
#  - a menu over a window: while it is open only its own rectangle differs from the screen before; closed, the screen
#    is as before; drawn once (zdesk -v's "menu drawn"); a highlight moved by a key redraws the two items that change
#    (no damage painted, a few glyphs) and the other items stay as they were
#  - a raise of a partly covered window: one damage painted, its inside one copy from its kept copy
#  - term: a key's echo draws a few glyphs (the cell and the cursor); a horizontal resize (Amiga+Shift+Left, five
#    times) gives the program 75 columns (ptytest size, as stty size); `time seq 1 2000` on the console and in term
#    (printed, emulator figures)
#  - the titles start after a left close gadget (workbench), and a long centred title stays between the gadgets
#    (platinum)
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-zdraw-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-zdraw.XXXXXX)}; mkdir -p "$OUT"
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
send time seq 1 2000 2> /tmp/tt-con\r
sleep 30
send echo acceleration none > /etc/zdesk/mouse; echo speed 200 >> /etc/zdesk/mouse; printf 'right cmd 3600 echo .\n' > /etc/zdesk/bar\r
sleep 1
send : > /tmp/c; zdesk -v > /tmp/z.log 2>&1 &\r
sleep 8
send dpytest -f /tmp/c > /tmp/d.log 2>&1 &\r
sleep 3
send echo win A 300 120 4 >> /tmp/c\r
sleep 2
send echo win B 200 80 9 >> /tmp/c\r
sleep 3
send echo info A >> /tmp/c; echo info B >> /tmp/c\r
sleep 3
shot pre
send echo stat open0 >> /tmp/c\r
sleep 2
send gcontest keys 66 40 c0 e6\r
sleep 3
shot open
send echo stat open >> /tmp/c\r
sleep 2
send gcontest keys 4d cd\r
sleep 2
shot h1
send echo stat h1 >> /tmp/c\r
sleep 2
send gcontest keys 4d cd\r
sleep 2
shot h2
send echo stat h2 >> /tmp/c\r
sleep 2
send gcontest keys 45 c5\r
sleep 3
shot closed
send echo stat closed >> /tmp/c\r
sleep 2
send gcontest keys 66 42 c2 e6\r
sleep 3
send echo stat raise >> /tmp/c\r
sleep 2
shot raised
send gcontest keys 66 44 c4 e6\r
sleep 8
EOS
typ 'ptytest size > /tmp/sz0\n'
cat <<'EOS'
sleep 2
send echo stat term0 >> /tmp/c\r
sleep 2
send gcontest keys 32 b2\r
sleep 3
send echo stat key >> /tmp/c\r
sleep 2
send gcontest keys 41 c1\r
sleep 2
send gcontest keys 66 60 4f cf e0 e6 66 60 4f cf e0 e6 66 60 4f cf e0 e6\r
sleep 2
send gcontest keys 66 60 4f cf e0 e6 66 60 4f cf e0 e6\r
sleep 3
EOS
typ 'ptytest size > /tmp/sz\n'
echo "sleep 2"
echo 'send echo stat seq0 >> /tmp/c\r'
echo "sleep 2"
typ 'time seq 1 2000 2> /tmp/tt-term\n'
cat <<'EOS'
sleep 60
send echo stat seq >> /tmp/c\r
sleep 2
shot term
send gcontest keys 66 40 c0 e6\r
sleep 1
send gcontest keys 4c cc 44 c4\r
sleep 8
send : > /tmp/c; zdesk -theme workbench > /tmp/z2.log 2>&1 &\r
sleep 6
send dpytest -f /tmp/c > /tmp/d2.log 2>&1 &\r
sleep 3
send echo win W 200 60 4 >> /tmp/c; echo win X 100 40 4 >> /tmp/c\r
sleep 3
send echo info W >> /tmp/c\r
sleep 2
shot workbench
send gcontest keys 66 40 c0 e6\r
sleep 1
send gcontest keys 4c cc 44 c4\r
sleep 6
send : > /tmp/c; zdesk -theme platinum > /tmp/z3.log 2>&1 &\r
sleep 6
send dpytest -f /tmp/c > /tmp/d3.log 2>&1 &\r
sleep 3
send echo win ABCDEFGHIJKLMNO 100 40 4 >> /tmp/c; echo win Y 100 40 4 >> /tmp/c\r
sleep 3
send echo info ABCDEFGHIJKLMNO >> /tmp/c\r
sleep 2
shot platinum
send gcontest keys 66 40 c0 e6\r
sleep 1
send gcontest keys 4c cc 44 c4\r
sleep 6
send echo console-$(cat /tmp/tt-con); echo term-$(cat /tmp/tt-term); echo sz0 $(cat /tmp/sz0) sz $(cat /tmp/sz); echo menudrawn=$(grep -c 'menu drawn' /tmp/z.log) menuat=$(grep -c 'menu at' /tmp/z.log); cat /tmp/z.log /tmp/d.log /tmp/d2.log /tmp/d3.log; echo zdr-end-$((40+2))\r
wait zdr-end-42 30
EOS
} > "$OUT/script.txt"
make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
ZOOMOUT=0 SHOT_LIMIT=900 timeout -k 20 1200 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/z" >/dev/null 2>&1
L=$OUT/serial.txt; tr -d '\r' < "$OUT/z.serial.log" > "$L"
for s in columns pre open h1 h2 closed raised term workbench platinum; do
    [ -f "$OUT/z-$s.png" ] || { echo "FAIL no screenshot $s (see $OUT/z.serial.log)"; exit 1; }
done
python3 tools/colour-test.py columns "$OUT/z-columns.png" > "$OUT/geom"; rc=$?; check "the text console's geometry ($(head -1 "$OUT/geom"))" $rc
dt() { python3 tools/dpy-test.py "$@"; }
stat() { grep "^dpytest: stat $1 " "$L" | head -1; }
field() { stat "$1" | sed -n "s/.* $2 \([0-9]*\).*/\1/p"; }
# the menu's rectangle (zdesk -v), the bar's status corner (the clock) left out of every comparison
read -r mx my mw mh < <(sed -n 's/^zdesk: menu at \([0-9]*\) \([0-9]*\) \([0-9]*\) \([0-9]*\)$/\1 \2 \3 \4/p' "$L" | head -1)
[ -n "$mh" ]; check "zdesk says where the menu is ($mx $my $mw $mh)" $?
BAR=320,0,320,11
dt same "$OUT/z-pre.png" "$OUT/z-open.png" "$OUT/geom" -x $BAR -x 0,0,64,11 -x "$((mx - 1)),$((my - 1)),$((mw + 2)),$((mh + 2))" -in "$mx,$my,$mw,$mh"
check "the menu open over window A: only its own rectangle changed (the bar's Zenix title aside)" $?
dt same "$OUT/z-pre.png" "$OUT/z-closed.png" "$OUT/geom" -x $BAR; check "the menu closed: the screen as before it, pixel for pixel" $?
grep -q "^menudrawn=\([0-9]*\) menuat=\1$" "$L"; rc=$?; check "each menu drawn once while it was open ($(grep -o '^menudrawn=.*' "$L"))" $rc
i1=$((my + 2 + 10)); i2=$((my + 2 + 20))
dt same "$OUT/z-h1.png" "$OUT/z-h2.png" "$OUT/geom" -x $BAR -x "$mx,$i1,$mw,10" -x "$mx,$i2,$mw,10"
check "the highlight moved from item 2 to 3: every other item untouched" $?
dt same "$OUT/z-h1.png" "$OUT/z-h2.png" "$OUT/geom" -x $BAR -in "$mx,$i1,$mw,20" >/dev/null; check "... and items 2 and 3 did change" $?
g=$(field h2 glyphs); p=$(field h2 paints)
[ -n "$g" ] && [ "$g" -le 40 ] && [ "$p" = 0 ]; check "a highlight move: $g glyphs (the two items' text), no damage painted ($p)" $?
c=$(field raise copies); p=$(field raise paints)
[ "$c" = 1 ] && [ "$p" = 1 ]; check "Amiga+Tab raises A: one damage painted ($p), its inside one copy ($c)" $?
g=$(field key glyphs)
[ -n "$g" ] && [ "$g" -ge 1 ] && [ "$g" -le 4 ]; check "a key in term: $g glyphs drawn (its echo and the cursor)" $?
read -r r0 c0 r1 c1 < <(sed -n 's/^sz0 \([0-9]*\) \([0-9]*\) sz \([0-9]*\) \([0-9]*\)$/\1 \2 \3 \4/p' "$L" | head -1)
[ -n "$c1" ] && [ "$r1" = "$r0" ] && [ "$c1" = $((c0 - 5)) ]
check "term narrowed by five columns: the program sees $r0 x $c0, then $r1 x $c1 (TIOCGWINSZ)" $?
read -r wx wy < <(sed -n 's/^dpytest: info W \(-*[0-9]*\) \(-*[0-9]*\) .*/\1 \2/p' "$L" | head -1)
dt title "$OUT/z-workbench.png" "$OUT/geom" $((wx + 11)) $((wx + 17)) "$wy"
check "workbench: the title starts after the close gadget and a gap (frame $wx,$wy)" $?
read -r px py pw < <(sed -n 's/^dpytest: info ABCDEFGHIJKLMNO \(-*[0-9]*\) \(-*[0-9]*\) \([0-9]*\) .*/\1 \2 \3/p' "$L" | head -1)
dt title "$OUT/z-platinum.png" "$OUT/geom" $((px + 16)) $((px + 21)) "$py"
check "platinum: a long centred title starts after the close box and a gap (frame $px,$py)" $?
echo "emulator figures (FS-UAE, the stock A1200 with 4 MB fast; not the real machine's): time seq 1 2000"
grep "^console- real\|^term- real" "$L" | head -2
for s in open0 open h1 h2 closed raise term0 key seq0 seq; do stat "$s"; done
grep -m4 "^zdesk: menu drawn in\|^zdesk: highlight moved in" "$L"
exit $((1 - pass))
