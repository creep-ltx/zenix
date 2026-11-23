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
# `mouse-test.sh d` (displayd's DPY_MOUSE, through dpytest, no zdesk):
#  - the defaults (100%, low, normal, 0.5 s); a setting out of range refused; at 100% without acceleration a count is
#    one pixel across and half a line down (4 counts: 4, 2); the acceleration "low" on 10 counts: 4 + 6 x 2
#  - a press, a release and a press within the double-click time: the second EV_BUTTON has arg 2; with 0.1 s and the
#    second press 15 ticks later: none; the buttons swapped: the left button's press arrives as button 2
#  - interlaced (DPY_MODE): the same place on the glass in lace lines (y doubled), a count one lace line down at 100%
#  - a card (dpytest -fb 1280x960x4): single card pixels (3 counts: 3 pixels), the arrow drawn there (its outline
#    black at the hot spot, the face white below), the pointer's shows counted (DPY_STAT's D61 three)
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

run_d() {
    cat > "$OUT/d-script.txt" <<EOS
wait zenix:/# 120
sleep 1
send dpytest\r
wait dpytest: open 60
sleep 2
send mget\r
send mset 5 0 100 0 50\r
send mset 100 0 100 0 50\r
send mouse -128 -128\r
send mouse -128 -128\r
send mouse -128 -128\r
send mouse -128 -128\r
send mouse -128 -128\r
send mget\r
send mouse 1 1\r
send mouse 1 1\r
send mouse 1 1\r
send mouse 1 1\r
send mget\r
send mset 100 4 200 0 50\r
send mouse 10 0\r
send mget\r
send mset 100 0 100 0 50\r
send win A 200 100 4\r
sleep 2
send info A\r
send mouse 70 116\r
send mget\r
send events\r
send clicks 2 6\r
sleep 1
send events\r
send mset 100 0 100 0 10\r
send clicks 2 15\r
sleep 1
send events\r
send mset 100 0 100 1 50\r
send clicks 1 6\r
sleep 1
send events\r
send mset 100 0 100 0 50\r
send mode lace\r
sleep 3
send mget\r
send mouse 0 1\r
send mget\r
send mode hires\r
sleep 3
send mget\r
shot chipset
send quit\r
sleep 3
send dpytest -fb 1280x960x4\r
wait dpytest: open 60
sleep 2
send mget\r
send mset 100 0 100 0 50\r
send pstat\r
send mouse 1 0\r
send mouse 1 0\r
send mouse 1 0\r
sleep 1
send mget\r
send peek 643 480\r
send peek 644 482\r
send peek 640 480\r
send pstat\r
send quit\r
sleep 2
send echo mouse-end-\$((40+2))\r
wait mouse-end-42 60
shot end
EOS
    ZENIX_FSUAE_SET="fast_memory=8192" ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae \
        "$OUT/d.fs-uae" || exit 1           # (the card's 4.9 MB in fast RAM, as rtg-test's)
    ZOOMOUT=0 SHOT_LIMIT=400 timeout -k 20 700 python3 tools/shot.py "$OUT/d.fs-uae" "$OUT/d-script.txt" "$OUT/d" >/dev/null 2>&1
    local L=$OUT/d-serial.txt m
    tr -d '\r' < "$OUT/d.serial.log" > "$L"
    grep -a "^dpytest: mget" "$L" > "$OUT/d.mget"
    m() { sed -n "$1p" "$OUT/d.mget" | awk '{ print $3, $4 }'; }
    sed -n 1p "$OUT/d.mget" | grep -q "^dpytest: mget 320 128 speed 100 thresh 4 mult 200 flags 0 dclick 50 hot 0 0 (0)"
    check "the defaults: the middle, 100%, low (4, 2 times), normal buttons, 0.5 s, the arrow's hot spot" $?
    grep -a -q "^dpytest: mset -22" "$L" && [ "$(grep -a -c '^dpytest: mset 0' "$L")" -ge 6 ]
    check "a speed of 5% refused (EINVAL), the others taken" $?
    local m2 m3 m4 m5 m6 m7 m8 m9 m10
    m2=$(m 2); m3=$(m 3); m4=$(m 4); m5=$(m 5); m6=$(m 6); m7=$(m 7); m8=$(m 8); m9=$(m 9); m10=$(m 10)
    [ "$m2" = "0 0" ] && [ "$m3" = "4 2" ]
    check "100% without acceleration: 4 counts from the corner are 4 pixels across, 2 lines down (got $m3)" $?
    [ "$m4" = "20 2" ]; check "acceleration low: 10 counts in a frame go 4 + 6 x 2 = 16 pixels further (got $m4)" $?
    [ "$m5" = "90 60" ]; check "70, 116 counts: 70 across, 58 down (got $m5)" $?
    local seq
    seq=$(grep -a "^ev 2 A " "$L" | awk '{ printf "%s%s ", $4, $9 == "arg" ? "+" $10 : "" }')
    case "$seq" in "1 129 1+2 129 "*) true ;; *) false ;; esac
    check "a click and a second within 0.5 s: the second press has arg 2 (a double click), the first none (got $seq)" $?
    case "$seq" in "1 129 1+2 129 1 129 1 129 "*) true ;; *) false ;; esac
    check "the double-click time 0.1 s and the second press 15 ticks later: two single clicks" $?
    [ "${seq#1 129 1+2 129 1 129 1 129 }" = "2 130 " ]
    check "the buttons swapped: the left button arrives as button 2 (down and up)" $?
    [ "$m6" = "90 120" ] && [ "$m7" = "90 121" ] && [ "$m8" = "90 60" ]
    check "interlaced: the same place in lace lines (60 -> 120), a count one lace line down at 100%, back (got $m6, $m7, $m8)" $?
    [ "$m9" = "640 480" ] && [ "$m10" = "643 480" ]
    check "a card 1280 x 960: the middle, then 3 counts are 3 card pixels (got $m10)" $?
    grep -a -q "^dpytest: peek 643 480 00000000" "$L" && grep -a -q "^dpytest: peek 644 482 00FFFFFF" "$L" &&
        ! grep -a -q "^dpytest: peek 640 480 00000000" "$L"
    check "... the arrow drawn there: black at its hot spot, white inside, the old place no longer black" $?
    local ps
    ps=$(grep -a "^dpytest: pstat" "$L" | tail -1)
    awk '{ exit !($4 >= 1 && $8 <= 2) }' <<<"$ps"
    check "... the card's pointer shows counted, each within 2 ticks of the kernel's record ($ps)" $?
}

# `mouse-test.sh l`: the card's pointer under load (docs/d61-mouse.md 8): a dpytest on a 1280 x 960 card opens and
# closes six windows of 1200 x 900 (each a big redraw converted onto the card) while the mouse moves a count every 3 ticks;
# the pointer's shows and their ticks from the kernel's record to the glass (DPY_STAT's D61 three) are said, and
# checked to be counted. An emulator's figures, for the comparison before and after only.
run_l() {
    cat > "$OUT/l-script.txt" <<EOS
wait zenix:/# 120
sleep 1
send echo mset 100 0 100 0 50 > /tmp/c; dpytest -fb 1280x960x4 -f /tmp/c > /tmp/l.log &\r
sleep 10
send echo pstat >> /tmp/c; (sleep 1; gcontest mouse$(rec 50 1,0,0)) &\r
send (sleep 4; gcontest mouse$(rec 50 -1,0,0)) &\r
send for i in 1 2 3 4 5 6; do echo win C\$i 1200 900 \$((i + 2)) >> /tmp/c; echo close C\$i >> /tmp/c; done\r
sleep 20
sleep 15
send echo pstat >> /tmp/c; echo quit >> /tmp/c\r
sleep 4
send cat /tmp/l.log; echo mouse-end-\$((40+2))\r
wait mouse-end-42 60
shot end
EOS
    ZENIX_FSUAE_SET="fast_memory=8192" ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae \
        "$OUT/l.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=300 timeout -k 20 600 python3 tools/shot.py "$OUT/l.fs-uae" "$OUT/l-script.txt" "$OUT/l" >/dev/null 2>&1
    local L=$OUT/l-serial.txt ps
    tr -d '\r' < "$OUT/l.serial.log" > "$L"
    ps=$(grep -a "^dpytest: pstat" "$L" | tail -1)
    echo "mouse-test: the card's pointer under load: $ps"
    awk '{ exit !($4 >= 1) }' <<<"$ps"; check "the pointer's shows under load counted ($ps)" $?
}

# `mouse-test.sh p` (mouseprefs, docs/man/mouseprefs.txt, through zdesk, as root; /etc/zdesk/mouse first "acceleration
# none" and "speed 200": two pixels a count across, a line down, as the desktop's tests click):
#  - the buttons swapped and Test: a press of the left button now arrives as the right one (mouseprefs -v says each
#    press), the window stays; Cancel, clicked with the right button (swapped): normal again
#  - with the keyboard: the double click to 0.6 s and the pointer to the next file (cross), a pixel painted in colour 3
#    with the mouse (the editor), the double-click test area clicked twice (a screenshot), Save: /etc/zdesk/mouse has
#    doubleclick 60 and pointer custom (its comments kept), /etc/zdesk/pointers/custom.pointer has the cross with the
#    painted pixel; DPY_MOUSE says 0.6 s and the cross's hot spot 7, 7
#  - Esc at once: nothing changes; the speed to 250% and Save (s): /etc/zdesk/mouse says it; zdesk started again (a
#    new session) applies the file: 250%, 0.6 s
# the left button (or B) clicked at screen pixel X, Y: to the top left, then there in counts of at most 127 (two pixels
# a count across, a line down)
pclk() {
    local x=$(($1 >> 1)) y=$2 b=${3:-1} m="-128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0"
    while [ "$x" -gt 0 ] || [ "$y" -gt 0 ]; do
        local dx=$((x > 127 ? 127 : x)) dy=$((y > 127 ? 127 : y))
        m="$m $dx,$dy,0"; x=$((x - dx)); y=$((y - dy))
    done
    echo "send gcontest mouse $m 0,0,$b 0,0,0\\r"
    echo "sleep 2"
}
run_p() {
    # mouseprefs' layout (display/prefs/mouseprefs.c, display/lib/prefswin.c) with the 8 x 8 font, its inside at
    # 32, 31 (zdesk's first window, the flat theme; the second cascades to 56, 43): row i's gadget, button i, the
    # editor's cell, the colours
    local IX=32 IY=31
    row() { echo "$((IX + 132)) $((IY + 6 + $1 * 14 + 3))"; }
    pbtn() { echo "$((IX + 8 + $1 * 97 + 45)) $((IY + 184))"; }
    {
    cat <<'EOS'
wait zenix:/# 120
sleep 1
send echo acceleration none > /etc/zdesk/mouse; echo speed 200 >> /etc/zdesk/mouse; echo '# a comment kept' >> /etc/zdesk/mouse; printf 'right cmd 3600 echo .\\n' > /etc/zdesk/bar\r
sleep 2
send cp /etc/zdesk/mouse /tmp/mouse0; zdesk -v > /tmp/z.log 2>&1 &\r
sleep 8
send mouseprefs -v > /tmp/m1.log 2>&1 &\r
sleep 5
shot open
EOS
    pclk $(row 4)                       # Buttons: swapped
    pclk $(pbtn 0)                      # Test
    echo "sleep 2"
    pclk $(row 4)                       # the left button on Buttons again: it arrives as button 2, nothing cycles
    pclk $(pbtn 3) 2                    # Cancel, with the right button (the left one's job now)
    cat <<'EOS'
sleep 3
send echo mget > /tmp/c; echo quit >> /tmp/c; dpytest -f /tmp/c > /tmp/w2.log\r
sleep 3
send mouseprefs -v > /tmp/m2.log 2>&1 &\r
sleep 5
send gcontest keys 4d cd 4d cd 4d cd 4d cd 4d cd 4e ce 4d cd 4e ce\r
sleep 3
EOS
    IX=56 IY=43
    pclk $((IX + 116 + 3 * 22 + 9)) $((IY + 108 + 9))      # colour 3 to paint with
    pclk $((IX + 8 + 2 * 6 + 3)) $((IY + 108 + 3 * 4 + 1)) # the cell 2, 3 painted
    cat <<'EOS'
sleep 1
send gcontest mouse 0,0,1 0,0,0 0,0,1 0,0,0\r
sleep 2
shot edited
send gcontest keys 21 a1\r
sleep 4
send echo mget > /tmp/c; echo quit >> /tmp/c; dpytest -f /tmp/c > /tmp/w3.log\r
sleep 3
send echo files-1; grep -v '^#' /etc/zdesk/mouse; grep -c '^#' /etc/zdesk/mouse; cat /etc/zdesk/pointers/custom.pointer\r
sleep 3
send mouseprefs -v > /tmp/m3.log 2>&1 &\r
sleep 5
send gcontest keys 45 c5\r
sleep 3
send mouseprefs -v > /tmp/m4.log 2>&1 &\r
sleep 5
send gcontest keys 4e ce\r
sleep 1
send gcontest keys 21 a1\r
sleep 4
send echo files-2; sed -n '/^speed/p' /etc/zdesk/mouse; kill $(pgrep zdesk)\r
sleep 6
send zdesk -v > /tmp/z2.log 2>&1 &\r
sleep 8
send echo mget > /tmp/c; echo quit >> /tmp/c; dpytest -f /tmp/c > /tmp/w4.log\r
sleep 3
send kill $(pgrep zdesk)\r
sleep 4
send cat /tmp/m1.log /tmp/m2.log /tmp/m3.log /tmp/m4.log /tmp/w.log /tmp/w2.log /tmp/w3.log /tmp/w4.log; cp /tmp/mouse0 /etc/zdesk/mouse; echo mouse-end-$((40+2))\r
wait mouse-end-42 60
shot end
EOS
    } > "$OUT/p-script.txt"
    ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/p.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=500 timeout -k 20 800 python3 tools/shot.py "$OUT/p.fs-uae" "$OUT/p-script.txt" "$OUT/p" >/dev/null 2>&1
    local L=$OUT/p-serial.txt
    tr -d '\r' < "$OUT/p.serial.log" > "$L"
    [ -f "$OUT/p-edited.png" ] || { echo "FAIL no screenshot p-edited (see $OUT/p.serial.log)"; pass=0; return; }
    grep -a -q "^mouseprefs: window 32 31 400 210 row0 164 40" "$L" && grep -a -q "^mouseprefs: window 56 43 400 210 " "$L"
    check "mouseprefs opens where the test clicks (its inside at 32, 31, then 56, 43; 400 x 210)" $?
    grep -a -q "^mouseprefs: test: .*buttons swapped (left hand)" "$L"
    check "the buttons swapped and Test: applied, the window stays" $?
    [ "$(grep -a '^mouseprefs: button' "$L" | head -4 | awk '{ printf "%s ", $3 }')" = "1 1 2 1 " ]
    check "... the left button then arrives as button 2 (mouseprefs -v says each press)" $?
    grep -a -q "^mouseprefs: cancel: .*buttons normal" "$L" && grep -a -q "^dpytest: mget .* flags 0 " "$L"
    check "Cancel, clicked with the right button (swapped then): the buttons normal again" $?
    grep -a -q "^mouseprefs: save: .*double click 0.60 s, pointer custom (edited)" "$L"
    check "with the keys and the editor: 0.6 s, the next pointer, a pixel painted, Save" $?
    sed -n '/^files-1/,/^hot/p' "$L" > "$OUT/files1"
    grep -q "^doubleclick 60$" "$OUT/files1" && grep -q "^pointer custom$" "$OUT/files1" && grep -q "^buttons normal$" "$OUT/files1" && grep -q "^speed 200$" "$OUT/files1"
    check "/etc/zdesk/mouse: doubleclick 60, pointer custom, the rest as it was" $?
    awk '/^files-1/ { f = 1 } f && /^[0-9]+$/ { print; exit }' "$L" | grep -q '^[1-9]'
    check "... its comments kept" $?
    sed -n '/^hot 7 7$/,$p' "$L" | sed -n 6p | grep -q '^\.\.x\.\.\.owo\.\.\.\.\.\.\.$'
    check "/etc/zdesk/pointers/custom.pointer: the cross (hot spot 7, 7) with the pixel at 2, 3 in colour 3" $?
    grep -a -q "^dpytest: mget .* dclick 60 hot 7 7 (0)" "$L"
    check "DPY_MOUSE in effect: 0.6 s and the cross's hot spot" $?
    grep -a -q "^mouseprefs: cancel: " "$L" && [ "$(grep -a -c '^mouseprefs: cancel: ' "$L")" = 2 ]
    check "Esc at once: Cancel, nothing changed" $?
    grep -a -A1 "^files-2" "$L" | grep -q "^speed 250$"
    check "the speed to 250% with the keys and s: saved" $?
    grep -a "^dpytest: mget" "$L" | tail -1 | grep -q "speed 250 thresh 0 mult 100 flags 0 dclick 60 hot 7 7"
    check "zdesk started again: the saved file applied (250%, no acceleration, 0.6 s, the pointer)" $?
}

for p in $PARTS; do
    case $p in
    l) run_l ;;
    k) run_k ;;
    d) run_d ;;
    p) run_p ;;
    *) echo "mouse-test: no part $p"; exit 2 ;;
    esac
done
exit $((1 - pass))
