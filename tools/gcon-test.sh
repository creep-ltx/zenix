#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# A console handed to a display server (DESIGN D38 step 2) in FS-UAE, through the cage rig (tools/shot.py) and
# gcontest (docs/man/gcontest.txt), on the A1200 with fast RAM (a test build with DIWV=0x38, as colour-test's):
#  - CON_GRAB: console 1 shows gcontest's Copper list (green) and its pointer, sprite 0, where the kernel put it (the
#    screen's middle); the kernel says so on the log
#  - keys go to the ring with the keymap's character and the qualifiers (a, Shift+a), not to the tty (the shell's next
#    command arrives clean)
#  - Amiga+F2 (raw codes through CON_INPUT, the keyboard's path): GEV_RELEASE, gcontest answers (CON_GACK), console 2
#    is shown as text; back to console 1 (the serial hook): GEV_ACQUIRE, the green screen and the pointer again
#  - gcontest ends: console 1 is text again (no green, no sprite left)
#  - a server that answers late (gcontest late, 2 s): the switch happens anyway after 1 s, the kernel log says so,
#    and the late answer is taken (the kernel uses the blitter again); the grab's memory cannot be freed meanwhile
#  - the console, kbd, colour and tui tests run separately (tools/check.sh)
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-gcon-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-gcon.XXXXXX)}; mkdir -p "$OUT"
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
send gcontest ack 300 > /tmp/g1.log &\r
sleep 4
shot grabbed
send gcontest mouse 10,5,0 0,0,1 0,0,0\r
sleep 2
shot mouse
send gcontest keys 20 a0 60 20 a0 e0\r
sleep 2
send echo tty-clean-$((1+1))\r
sleep 2
send gcontest keys 66 51 d1 e6\r
sleep 4
shot console2
send \x1d1
sleep 3
shot back
send kill $(pgrep gcontest)\r
sleep 3
shot givenback
send gcontest late 300 > /tmp/g2.log &\r
sleep 4
send gcontest keys 66 51 d1 e6\r
sleep 5
shot noack
send \x1d1
sleep 3
shot noackback
send kill $(pgrep gcontest)\r
sleep 3
send cat /tmp/g1.log /tmp/g2.log; echo letgo=$(dmesg | grep -c 'did not let go'); echo gcon-end-$((40+2))\r
wait gcon-end-42 60
sleep 1
EOS
make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
ZOOMOUT=0 SHOT_LIMIT=300 timeout -k 20 600 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/g" >/dev/null 2>&1
L=$OUT/serial.txt; tr -d '\r' < "$OUT/g.serial.log" > "$L"
for s in columns grabbed mouse console2 back givenback noack noackback; do
    [ -f "$OUT/g-$s.png" ] || { echo "FAIL no screenshot $s (see $OUT/g.serial.log)"; exit 1; }
done
python3 tools/colour-test.py columns "$OUT/g-columns.png" > "$OUT/geom"; check "the text console's geometry ($(head -1 "$OUT/geom"))" $?
gt() { python3 tools/gcon-test.py "$@"; }
grep -q "^gcontest: grabbed fd [0-9]* 640x256 depth 4 chips 1" "$L"; check "CON_GRAB: a descriptor, the screen 640 x 256, AGA" $?
grep -q "display: console 1 handed to pid [0-9]* (gcontest)" "$L"; check "the kernel log says console 1 was handed over" $?
gt grabbed "$OUT/g-grabbed.png" "$OUT/geom" 320 128; check "console 1 shows gcontest's Copper list and its pointer (sprite 0) in the middle" $?
gt grabbed "$OUT/g-mouse.png" "$OUT/geom" 340 133; check "the mouse moved 10, 5 counts (CONI_MOUSE): the pointer 20 pixels right, 5 lines down" $?
grep -q "^gcontest: pointer 340 133 qual 0x0" "$L" && grep -q "^gcontest: pointer 340 133 qual 0x4000" "$L"
check "... and in the ring: the move, then the left button held (GQ_LBUTTON)" $?
grep -q "^gcontest: key 0x20 qual 0x0 ch 61 0" "$L" && grep -q "^gcontest: key 0xa0 qual 0x0 ch 0 0" "$L"
check "a key goes to the ring: down with its character (a), up with none" $?
grep -q "^gcontest: key 0x60 qual 0x1 ch 0 0" "$L" && grep -q "^gcontest: key 0x20 qual 0x1 ch 41 0" "$L"
check "Shift+a: the left shift's own code, then A with GQ_LSHIFT" $?
grep -q "^tty-clean-2" "$L" && ! grep -q "aecho\|Aecho" "$L"; check "... and no key reached the tty (the shell's next command arrived clean)" $?
grep -q "^gcontest: release 1" "$L" && grep -q "^gcontest: ack 1 -> 0" "$L"; check "Amiga+F2: GEV_RELEASE 1, answered (CON_GACK 0)" $?
gt text "$OUT/g-console2.png" "$OUT/geom"; check "... then console 2 is shown as text" $?
[ "$(grep -c '^gcontest: acquire' "$L")" -ge 3 ]; check "GEV_ACQUIRE at the grab and at each return (3 or more)" $?
gt grabbed "$OUT/g-back.png" "$OUT/geom" 340 133; check "back on console 1: gcontest's screen and the pointer again, untouched" $?
gt text "$OUT/g-givenback.png" "$OUT/geom"; check "gcontest ended: console 1 is text again, no sprite left" $?
grep -q "display: console 1 given back by pid" "$L"; check "the kernel log says console 1 was given back" $?
grep -q "display: pid [0-9]* did not let go of console 1 within 1 s: switched anyway" "$L"
check "a server that answers late: the kernel switches after 1 s and says so" $?
[ "$(grep -c '^gcontest: ack 1 -> 0' "$L")" = 2 ]; check "... the late answer is taken (0), as the prompt one was" $?
[ "$(grep -c '^gcontest: memfree of the grab.s memory -> -16' "$L")" = 2 ]; check "the grab's memory cannot be freed while the grab lasts (EBUSY)" $?
gt text "$OUT/g-noack.png" "$OUT/geom"; check "... console 2 is shown as text" $?
gt grabbed "$OUT/g-noackback.png" "$OUT/geom" 320 128; check "... and back on console 1 the server has its screen again" $?
grep -q "^letgo=1$" "$L"; check "the answered switch did not time out (one such line, the noack one)" $?
exit $((1 - pass))
