#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Lua's zenix module and gridrun (roadmap block 7 item 7; docs/man/zenix-lua.txt, gridrun.txt) in FS-UAE through the
# cage rig (tools/shot.py) on the stock A1200 with 4 MB of fast RAM, as root, under zdesk with the flat theme:
#  - gridrun opens its window, draws the grid, makes its sounds and its module, starts the music (its log, GRIDRUN_LOG);
#  - space starts a round, an arrow turns: the screenshot shows the window, a run of the player's cyan and of the
#    daemon's magenta across the grid (the module's window, fills, lines and pictures on the screen);
#  - while it plays, the sound server's state (play -S) shows one stream, frames played and the channels on with the
#    module's periods (the module and the samples reached sndd and Paula);
#  - a round ends in the log, q ends gridrun, its stream is gone from sndd;
#  - before the desktop, rootfs/tests/games/zbad.lua: the module's calls with bad arguments raise Lua's errors (or
#    answer false and why), never crash; z.time and z.sleep work without a window.
# Printed: gridrun's memory (ps), /bin/lua's size (emulator figures).
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-luagame-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-luagame.XXXXXX)}; mkdir -p "$OUT"
B=$OUT/build
trap 'rm -rf "$B"' EXIT
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
{
cat <<'EOF'
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send cd /tests/games; lua zbad.lua > /tmp/zb.txt 2>&1; echo zbst=$?; echo ==z""out; cat /tmp/zb.txt; echo ==z""end\r
wait ==zend 120
send cd /; echo acceleration none > /etc/zdesk/mouse; zdesk -v > /tmp/z.log 2>&1 &\r
sleep 10
send w() { n=0; until grep -q "$1" /tmp/g.log || [ $n -gt 120 ]; do sleep 1; n=$((n+1)); done; echo rdy-$2; }\r
sleep 2
send GRIDRUN_LOG=1 GRIDRUN_SEED=7 gridrun > /tmp/g.log 2>&1 &\r
sleep 2
send w 'music' $((40+1))\r
wait rdy-41 200
sleep 3
shot ready
send gcontest keys 40 c0\r
sleep 1
send gcontest keys 4c cc\r
sleep 1
send gcontest keys 4e ce\r
sleep 1
shot ride
send play -S; echo ==s""end\r
wait ==send 60
send w 'round' $((40+2))\r
wait rdy-42 200
sleep 2
shot over
send echo ==p""s; ps | grep -E 'gridrun|sndd' | cat; ls -l /bin/lua /bin/gridrun; echo ==p""end\r
wait ==pend 60
send gcontest keys 10 90\r
sleep 3
send w 'bye' $((40+3))\r
wait rdy-43 60
send sleep 2; play -S; echo ==e""nd; cat /tmp/g.log; echo ==g""end\r
wait ==gend 60
EOF
} > "$OUT/script.txt"
if [ -z "$LUAGAME_RECHECK" ]; then     # LUAGAME_RECHECK=1 with the same ZENIX_OUT: the checks again on the last files
    make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
        { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
    ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=900 timeout -k 20 1200 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/p" >/dev/null 2>&1
fi
L=$OUT/serial.txt; tr -d '\r' < "$OUT/p.serial.log" | sed 's/\x1b\[[0-9;?]*[A-Za-z]//g' > "$L"
python3 tools/colour-test.py columns "$OUT/p-columns.png" > "$OUT/geom"; rc=$?; check "the text console's geometry ($(head -1 "$OUT/geom"))" $rc
part() { awk -v a="==$1" -v b="==$2" '$0 == a { on = 1; next } $0 == b { exit } on' "$L"; }
# the module's error checks (zbad.lua: each bad call caught by pcall, its message printed)
zb=$(part zout zend)
grep -q 'zbst=0' "$L" && [ "$(echo "$zb" | grep -c '^ok ')" -ge 8 ] && ! echo "$zb" | grep -q '^BAD'
rc=$?; check "zbad.lua: every bad call a Lua error, no crash ($(echo "$zb" | grep -c '^ok ') ok, $(echo "$zb" | grep -c '^BAD') bad)" $rc
g=$(part end gend)
echo "$g" | grep -q "^gridrun:	music	on$"; check "gridrun made its sounds and module and started the music" $?
echo "$g" | grep -q "^gridrun:	key	space$" && echo "$g" | grep -q "^gridrun:	key	up$"
check "gridrun read the keys (space, up) from its window" $?
echo "$g" | grep -qE "^gridrun:	round	(DEREZZED|DAEMON PURGED|BOTH DEREZZED)	" && echo "$g" | grep -q "^gridrun:	bye"
rc=$?; check "a round ended, q ended gridrun ($(echo "$g" | grep -E 'round|bye' | tr '\t\n' '  '))" $rc
# sndd while the music played: one stream, frames, at least three channels on with periods in the module's range
s=$(awk '/^sndd: PAL/ { n++ } n == 1' "$L" | head -6)
echo "$s" > "$OUT/sndd1.txt"
fr=$(echo "$s" | sed -n 's/.* \([0-9]*\) frame(s).*/\1/p' | head -1)
on=$(echo "$s" | grep -cE '^ch[0-3]: on +per [0-9]+ ')
echo "$s" | grep -q " 1 stream(s)" && [ "${fr:-0}" -gt 10 ] && [ "$on" -ge 3 ]
check "sndd played gridrun's music: one stream, $fr frames, $on channels on" $?
e=$(awk '/^sndd: PAL/ { n++ } n == 2' "$L" | head -1)
echo "$e" | grep -q " 0 stream(s)"; check "gridrun's stream closed at its end ($e)" $?
# the screenshot while riding: runs of the player's cyan (the flat theme's 5BC, pen 14) and the daemon's B7C (pen 13)
python3 - "$OUT/p-ride.png" "$OUT/geom" > "$OUT/ride.txt" <<'PY'
import sys
sys.path.insert(0, "tools")
import importlib
d = importlib.import_module("dpy-test")
P = d.load(sys.argv[1], sys.argv[2])
def runs(rgb):
    want = d.rgb12(rgb)
    best = (0, 0, 0)
    for y in range(256):
        n = 0
        for x in range(640):
            if all(abs(p - q) <= 40 for p, q in zip(P(x, y), want)):
                n += 1
                if n > best[0]:
                    best = (n, x - n + 1, y)
            else:
                n = 0
    return best
c, m = runs("5BC"), runs("B7C")
print("cyan %d at %d,%d; magenta %d at %d,%d" % (c + m))
sys.exit(0 if c[0] >= 48 and m[0] >= 48 else 1)
PY
rc=$?; check "the window shows both trails: $(cat "$OUT/ride.txt")" $rc
ps=$(part ps pend | grep -E ' gridrun$')
echo "     (emulator figures: $(echo "$ps" | awk '{print "gridrun owns " $6 " KB"}'); $(part ps pend | grep -E '/bin/lua$' | awk '{print "/bin/lua " $5 " bytes"}'))"
grep -q "serial-tap: wait timed out\|the run ended during a wait" "$OUT/p.serial.log"; [ $? != 0 ]; check "every wait was met" $?
[ $pass = 1 ] && rm -rf "$OUT/p-columns.png" "$OUT/run.states" 2>/dev/null
[ $pass = 1 ]
