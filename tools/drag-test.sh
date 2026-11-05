#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# A whole window dragged in Zenix Desktop shows whole at one place in every frame (DESIGN D38, the owner's A1200 of
# 2026-11-05: "dragging a window tears and flickers", its contents at two places at once mid-move). In FS-UAE through
# the cage rig (tools/shot.py) on the stock A1200 with 4 MB of fast RAM (a test build with DIWV=0x38), zdesk with the
# flat theme set to "drag whole": window A (400 x 140, pen 9) over window B (300 x 100, pen 10) is dragged by its
# title bar with the mouse (gcontest mouse: the real mouse's path, a record every 3 ticks) for about five seconds,
# right and down, back, and again; screenshots are taken all through it. Checked on each:
#  - A's inside is one rectangle of exactly 400 x 140 pixels of its pen, nowhere else (a torn frame shows parts of it
#    at two places: a taller or wider box, or a hole);
#  - the screen outside A's frame is the desktop, B or the bar: no copy of A's pen anywhere else;
#  - the server's own check of every show against the beam (DPY_STAT's late) is 0.
# And after it: A where the drag ended, every part of B and the desktop as before. Printed: the drag's cost per move
# (blits, pixels, copies onto the screen, the E-clock time of a move and of a show, the time waited for the beam;
# emulator figures, not the real machine's).
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-drag-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-drag.XXXXXX)}; mkdir -p "$OUT"
B=$OUT/build
trap 'rm -rf "$B"' EXIT
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
NSHOT=14
{
cat <<'EOS'
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send sed 's/^drag auto/drag whole/' /etc/zdesk/themes/flat.theme > /etc/zdesk/themes/whole.theme\r
sleep 1
send echo acceleration none > /etc/zdesk/mouse; printf 'right cmd 3600 echo .\n' > /etc/zdesk/bar\r
sleep 1
send : > /tmp/c; zdesk -v -theme whole > /tmp/z.log 2>&1 &\r
sleep 8
send dpytest -f /tmp/c > /tmp/d.log 2>&1 &\r
sleep 3
send echo win B 300 100 10 >> /tmp/c\r
sleep 2
send echo win A 400 140 9 >> /tmp/c\r
sleep 3
send echo info A >> /tmp/c; echo info B >> /tmp/c\r
sleep 2
shot pre
send : > /tmp/m.sh\r
sleep 1
send echo 'gcontest mouse 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1' >> /tmp/m.sh\r
sleep 1
send echo 'gcontest mouse 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1' >> /tmp/m.sh\r
sleep 1
send echo 'gcontest mouse -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1' >> /tmp/m.sh\r
sleep 1
send echo 'gcontest mouse -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1 -2,-1,1' >> /tmp/m.sh\r
sleep 1
send echo 'gcontest mouse 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1' >> /tmp/m.sh\r
sleep 1
send echo 'gcontest mouse 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1 2,1,1' >> /tmp/m.sh\r
sleep 1
send echo 'gcontest mouse 0,0,0; echo drag-done-$((1+1))' >> /tmp/m.sh\r
sleep 1
send gcontest mouse -128,-128,0 -128,-128,0 78,36,0 0,0,1\r
sleep 2
send echo stat d0 >> /tmp/c\r
sleep 2
send sh /tmp/m.sh &\r
EOS
for i in $(seq 1 $NSHOT); do echo "shot d$i"; echo "sleep 0.2"; done
cat <<'EOS'
wait drag-done-2 120
sleep 2
send echo stat d1 >> /tmp/c\r
sleep 2
send echo info A >> /tmp/c\r
sleep 2
shot post
send cat /tmp/z.log /tmp/d.log; echo drg-end-$((40+2))\r
wait drg-end-42 30
EOS
} > "$OUT/script.txt"
make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
ZOOMOUT=0 SHOT_LIMIT=600 timeout -k 20 800 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/z" >/dev/null 2>&1
L=$OUT/serial.txt; tr -d '\r' < "$OUT/z.serial.log" > "$L"
for s in columns pre post; do
    [ -f "$OUT/z-$s.png" ] || { echo "FAIL no screenshot $s (see $OUT/z.serial.log)"; exit 1; }
done
python3 tools/colour-test.py columns "$OUT/z-columns.png" > "$OUT/geom"; check "the text console's geometry ($(head -1 "$OUT/geom"))" $?
grep -a -q "^zdesk: running, theme whole, .* drag whole" "$L"; check "zdesk runs with the whole-window drag" $?
grep -a -q "drag-done-2" "$L"; check "the drag's mouse records were all sent" $?
info() { grep -a -o "dpytest: info $1 [-0-9 ]*" "$L" | sed -n "$2p" | cut -d' ' -f4-7; }
read -r ax ay aw ah _ < <(info A 1)
read -r bx by bw bh _ < <(info B 1)
read -r ex ey _ < <(info A 2)
[ -n "$ah" ] && [ -n "$bh" ] && [ -n "$ey" ]; check "the windows' places (A $ax,$ay ${aw}x$ah, B $bx,$by ${bw}x$bh; A at the end $ex,$ey)" $?
read -r rw rh < <(python3 tools/dpy-test.py whole "$OUT/z-pre.png" "$OUT/geom" D54 0 0 | sed -n 's/.* \([0-9]*\)x\([0-9]*\) .*/\1 \2/p')
[ -n "$rh" ] && [ $((rw - 400)) -ge -2 ] && [ $((rw - 400)) -le 0 ] && [ $((rh - 140)) -ge -1 ] && [ $((rh - 140)) -le 0 ]
check "before the drag A's inside measures ${rw}x$rh in the screenshot (400 x 140 on the Amiga)" $?
n=0 torn=0
for i in $(seq 1 $NSHOT); do
    f=$OUT/z-d$i.png
    [ -f "$f" ] || continue
    n=$((n + 1))
    python3 tools/dpy-test.py whole "$f" "$OUT/geom" D54 "$rw" "$rh" > "$OUT/w$i" || { torn=$((torn + 1)); echo "  d$i: $(cat "$OUT/w$i")"; }
done
[ "$n" -ge $((NSHOT - 2)) ] && [ "$torn" = 0 ]; check "during the drag $n screenshots, each with A whole at one place ($torn torn)" $?
python3 tools/dpy-test.py whole "$OUT/z-post.png" "$OUT/geom" D54 "$rw" "$rh" > "$OUT/wp"; r=$?
read -r px py < <(sed -n 's/.* at \([0-9]*\),\([0-9]*\) .*/\1 \2/p' "$OUT/wp")
[ "$r" = 0 ] && [ -n "$py" ] && [ $((px - ex - 1)) -ge 0 ] && [ $((px - ex - 1)) -le 1 ] && [ "$py" = $((ey + 12)) ]
check "after the drag A is whole where it ended ($(cat "$OUT/wp"))" $?
stat() { grep -a -o "dpytest: stat $1 [^(]*([-0-9]*)" "$L" | head -1; }
field() { stat "$1" | sed -n "s/.* $2 \([0-9]*\).*/\1/p"; }
late=$(field d1 late)
[ "$late" = 0 ]; check "every show met the beam's rule (late $late)" $?
mv=$(field d1 moves)
echo "emulator figures (FS-UAE, the stock A1200 with 4 MB fast; not the real machine's): the drag"
stat d1
if [ -n "$mv" ] && [ "$mv" -gt 0 ]; then
    awk -v m="$mv" -v b="$(field d1 blits)" -v p="$(field d1 pixels)" -v c="$(field d1 copies)" -v t="$(field d1 movet)" \
        -v s="$(field d1 shows)" -v st="$(field d1 showt)" -v w="$(field d1 waitt)" 'BEGIN {
        printf "per move (%d moves): %.1f blits, %d pixels, %.1f copies onto the screen, %.2f ms drawing", m, b / m, p / m, c / m, t / m / 709.379
        if (s > 0) printf ", %.2f ms showing (%d shows), %.2f ms waiting for the beam", st / s / 709.379, s, w / s / 709.379
        printf "\n" }'
fi
exit $((1 - pass))
