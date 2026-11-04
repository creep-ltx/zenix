#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# dlogin, the graphical login (DESIGN D38 revision 3), in FS-UAE through the cage rig (tools/shot.py) on the stock
# A1200 with 4 MB of fast RAM (a test build with DIWV=0x38). guest gets a password (passwd), /etc/console says "login
# graphical" and the machine reboots: init starts dlogin on console 1. A daemon (/etc/daemons: rootfs/tests/dlogin.sh)
# then types through CON_INPUT (gcontest keys) and says each step on the serial port, where a screenshot is taken:
#  - the greeter: the desk and the box in the flat theme
#  - a wrong password: "Login incorrect" after the wait, only the message line changed on the screen
#  - root refused (a text console is root's)
#  - guest's password: the desktop (zdesk) as guest - a terminal's `id` says uid 1001; Log out: the greeter again
#  - the Text console session: guest's shell on the console (`id` typed there), its exit: the greeter again
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-dlogin-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-dlogin.XXXXXX)}; mkdir -p "$OUT"
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
send passwd guest\r
sleep 2
send secret\r
sleep 1
send secret\r
sleep 3
send echo acceleration none > /etc/zdesk/mouse; echo login graphical >> /etc/console\r
sleep 1
send echo 'dlt 0 /bin/sh /tests/dlogin.sh' >> /etc/daemons; sync; reboot\r
EOS
for s in greeter typed wrong desk back rootref textsession back2; do printf 'wait dlt-%s 240\nshot %s\n' "$s" "$s"; done
echo 'wait dlt-end 60'
} > "$OUT/script.txt"
make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
ZOOMOUT=0 SHOT_LIMIT=900 timeout -k 20 1200 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/d" >/dev/null 2>&1
L=$OUT/serial.txt; tr -d '\r' < "$OUT/d.serial.log" > "$L"
for s in columns greeter typed wrong desk back rootref textsession back2; do
    [ -f "$OUT/d-$s.png" ] || { echo "FAIL no screenshot $s (see $OUT/d.serial.log)"; exit 1; }
done
python3 tools/colour-test.py columns "$OUT/d-columns.png" > "$OUT/geom"; check "the text console's geometry ($(head -1 "$OUT/geom"))" $?
px() { s=$1; shift; python3 tools/dpy-test.py px "$OUT/d-$s.png" "$OUT/geom" "$@"; }
dt() { python3 tools/dpy-test.py "$@"; }
# the box: 336 x 112 in the middle (152, 72); its message line at y 164; the clock top right
MSG=160,164,320,8; CLOCK=580,0,60,14
px greeter 20,100,344 155,75,EEE 600,240,344
check "/etc/console login graphical: after a reboot init starts dlogin on console 1, the greeter (the flat theme)" $?
dt same "$OUT/d-typed.png" "$OUT/d-wrong.png" "$OUT/geom" -x $CLOCK -in $MSG
check "a wrong password: \"Login incorrect\", only the message line changed" $?
grep -q "dlogin: login incorrect$" "$L"; check "... and dlogin says so (no name, no password in its log)" $?
px desk 300,5,112 400,200,344; check "guest's password: the desktop (zdesk's bar and desk)" $?
grep -q "dlogin: guest (uid 1001) logged in: Desktop$" "$L"; check "... dlogin started guest's Desktop session" $?
grep -q "^dlt-ids uid=1001(guest)" "$L"; check "... a terminal in it runs as guest (id: uid 1001), never root" $?
grep -q "dlogin: guest's session ended$" "$L" && px back 155,75,EEE 20,100,344; check "Log out: the greeter again" $?
grep -q "dlogin: root refused" "$L"; check "root is refused (a text console is root's)" $?
px textsession 400,200,000; check "the Text console session: the console is text" $?
grep -q "dlogin: guest (uid 1001) logged in: Text console$" "$L" && grep -q "^dlt-ids uid=1001(guest).* / uid=1001(guest)" "$L"
check "... guest's shell there (id: uid 1001)" $?
px back2 155,75,EEE 20,100,344; check "... its exit: the greeter again" $?
exit $((1 - pass))
