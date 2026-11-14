#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The Zenix intro, zintro (DESIGN D55 section 13, docs/d55-screens.md), in FS-UAE through the cage rig on the stock
# A1200 with 4 MB of fast RAM (a test build with DIWV=0x38), in check.sh's STOCK (screenshots on the PC's clock). A
# dpytest window keeps the desktop up; zintro is started from the shell three times:
#  - 20 s: the logo pixel for pixel in two screenshots 8 s apart (tools/screen-test.py logo, tools/mkintro.py's own
#    picture), the stars' band changed between them (it moves); sndd's state (play -S) twice: one stream, three or
#    four channels on, its frames counting; Esc: the desktop as it was; its -v line: the frames drawn and missed
#  - without music (-q), a click: ended, the desktop as it was
#  - kill -9: the desktop as it was (the kernel hands its planes to displayd)
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-intro-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-intro.XXXXXX)}; mkdir -p "$OUT"
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
send : > /tmp/c; dpytest -f /tmp/c &\r
wait dpytest: open 60
sleep 2
send echo win A 200 80 4 >> /tmp/c\r
sleep 3
shot desk
send zintro -v > /tmp/i.log 2>&1 &\r
sleep 10
shot intro1
send play -S; echo st-$((40+1))\r
wait st-41 20
sleep 2
send play -S; echo st-$((40+2))\r
wait st-42 20
sleep 2
shot intro2
send gcontest keys 45 c5\r
sleep 5
shot after1
send cat /tmp/i.log; echo log-$((40+3))\r
wait log-43 20
send zintro -q -v > /tmp/i2.log 2>&1 &\r
sleep 10
send gcontest mouse 0,0,1 0,0,0\r
sleep 5
shot after2
send pgrep zintro || echo gone-$((40+4)); cat /tmp/i2.log\r
sleep 2
send zintro -q &\r
sleep 10
shot intro3
send kill -9 $(pgrep zintro); sleep 3; echo killed-$((40+5))\r
wait killed-45 30
sleep 2
shot after3
send ls -l /bin/zintro; echo end-$((40+6))\r
wait end-46 30
EOS
if [ -z "$INTRO_RECHECK" ]; then
    make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
        { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
    ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=600 timeout -k 20 900 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/i" >/dev/null 2>&1
fi
L=$OUT/serial.txt; tr -d '\r' < "$OUT/i.serial.log" > "$L"
for s in columns desk intro1 intro2 after1 after2 intro3 after3; do
    [ -f "$OUT/i-$s.png" ] || { echo "FAIL no screenshot $s (see $OUT/i.serial.log)"; exit 1; }
done
python3 tools/colour-test.py columns "$OUT/i-columns.png" > "$OUT/geom"; rc=$?; check "the text console's geometry ($(head -1 "$OUT/geom"))" $rc
same() { python3 tools/dpy-test.py same "$OUT/i-$1.png" "$OUT/i-$2.png" "$OUT/geom" -x 0,0,640,4 "${@:3}"; }
python3 tools/screen-test.py logo "$OUT/i-intro1.png" "$OUT/geom" > "$OUT/q1"; rc=$?; check "zintro: its logo pixel for pixel ($(cat "$OUT/q1"))" $rc
python3 tools/screen-test.py logo "$OUT/i-intro2.png" "$OUT/geom" > "$OUT/q2"; rc=$?; check "... 8 s later too ($(cat "$OUT/q2"))" $rc
python3 tools/dpy-test.py same "$OUT/i-intro1.png" "$OUT/i-intro2.png" "$OUT/geom" -x 0,0,640,256 -in 0,100,640,100 > "$OUT/q3"
rc=$?; check "... the stars' band moved between them ($(cat "$OUT/q3"))" $rc
s1=$(grep -a -B5 '^st-41' "$L"); s2=$(grep -a -B5 '^st-42' "$L")
f1=$(echo "$s1" | grep -a '^sndd:' | sed 's/.* \([0-9]*\) frame(s).*/\1/'); f2=$(echo "$s2" | grep -a '^sndd:' | sed 's/.* \([0-9]*\) frame(s).*/\1/')
on=$(echo "$s2" | grep -ac '^ch[0-3]: on ')
echo "$s2" | grep -aq '^sndd: PAL.* 1 stream' && [ "$on" -ge 3 ] && [ -n "$f1" ] && [ "$f2" -gt "$f1" ]
check "sndd plays its module: one stream, $on channels on (the lead rests in the first patterns), frames $f1 -> $f2" $?
same desk after1 > "$OUT/q4"; rc=$?; check "Esc: the desktop as it was ($(cat "$OUT/q4"))" $rc
log=$(grep -a '^zintro: [0-9]* frames drawn' "$L"); echo "$log" | grep -q 'music on'
rc=$?; check "its log: $(grep -a '^zintro: screen' "$L" | head -1); $log" $rc
same desk after2 > "$OUT/q5"; grep -aq '^gone-44' "$L"; r=$?
[ $r = 0 ] && grep -q 'same: 0 pixels' "$OUT/q5"; rc=$?; check "a click: ended, the desktop as it was ($(cat "$OUT/q5"))" $rc
python3 tools/screen-test.py logo "$OUT/i-intro3.png" "$OUT/geom" > /dev/null && same desk after3 > "$OUT/q6"
rc=$?; check "kill -9: the desktop as it was ($(cat "$OUT/q6"))" $rc
grep -a 'zintro$' "$L" | grep -a '^-' | sed 's/^/  /'
echo "  emulator figures (FS-UAE's stock A1200 with 4 MB fast, not a real machine's):"
grep -a '^zintro: ' "$L" | sed 's/^/  /'
exit $((1 - pass))
