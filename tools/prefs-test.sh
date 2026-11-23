#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The preference programs (docs/d38-desktop.md "Preferences", docs/d28-console-text.md section 22): Test, Use, Save
# and Cancel, AmigaOS's four buttons, each checked for what it must do. In FS-UAE through the cage rig (tools/shot.py)
# on the stock A1200 with 4 MB of fast RAM (a test build with DIWV=0x38), as root. `prefs-test.sh desk` (the default):
#  - deskprefs from zdesk, clicked with the mouse (gcontest mouse): the font gadget to zbad 11 (a damaged font, the
#    tools/mkamifont.py test fonts; the TrueType fonts set aside first, so the list holds only these) and Test: refused, said in the window, the 8 x 8 kept; on to ztest 11 and Test:
#    the font changed live (DPY_FONT_SET): displayd and zdesk say so, the bar and the title bars 3 lines taller in
#    the screenshot; the mode to interlace and Test: 640 x 512; Cancel: hires and the 8 x 8 again, the bar 11 lines;
#  - with the keyboard: ztest 11 and the platinum theme, Use: applied, the theme through zdesk's SIGHUP, no file
#    written but the session's theme.use; Esc (Cancel) at once: nothing changes; interlace and Save: /etc/zdesk/screen
#    and /etc/zdesk/theme written (the file's comments kept), theme.use gone;
#  - a reboot: zdesk starts on the saved settings (interlaced, ztest 11, platinum), read back from the files.
# `prefs-test.sh con`: conprefs on the text console, typed on the serial line (its arrows as escape sequences):
#  - 8 colours and Test: the console has 8 (conprefs -q); Cancel: 16 again; 8 colours and no Unicode font, Use: in
#    effect (a block character drawn as the box, in the screenshot), /var/run/console written, /etc/console not;
#    Cancel at once: nothing changes; 4 colours and the font auto, Save: /etc/console's lines replaced, its comments
#    kept, /var/run/console gone;
#  - D28 22, the console's mode: `termtest mode 1` (CON_MODE, as conprefs' Test) under `termtest winch` in the
#    foreground: SIGWINCH and 64 x 80, the kernel's log line, the grid of 64 rows with the text written before, a
#    block at row 1 and one at row 64 in the interlaced screenshot; conprefs' mode line, hires and Use: hires again;
#  - three broken lines and "mode hires-lace" added to /etc/console, a reboot: init says one line, the saved
#    settings are in effect (4 colours, the font loaded: the block drawn solid, interlaced by init),
#    /var/run/console gone.
# Pixels are read with tools/dpy-test.py on a geometry from the text console (colour-test.py columns).
cd "$(dirname "$0")/.." || exit 1
PART=${1:-desk}
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-prefs-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-prefs.XXXXXX)}; mkdir -p "$OUT"
B=$OUT/build
trap 'rm -rf "$B"' EXIT
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
# a click of the left button at screen pixel X, Y: the pointer pushed to the top left first, then moved there in
# counts of at most 127 (no acceleration, /etc/zdesk/mouse: a count is 2 pixels across, a line down; interlaced (a
# third argument 1) two lines)
clk() {
    local x=$(($1 >> 1)) y=$(($2 >> ${3:-0})) m="-128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0"
    while [ "$x" -gt 0 ] || [ "$y" -gt 0 ]; do
        local dx=$((x > 127 ? 127 : x)) dy=$((y > 127 ? 127 : y))
        m="$m $dx,$dy,0"; x=$((x - dx)); y=$((y - dy))
    done
    echo "send gcontest mouse $m 0,0,1 0,0,0\\r"
    echo "sleep 2"
}
# deskprefs' layout (display/prefs/deskprefs.c) for a font of LH lines, its inside at IX, IY: the middle of row I's
# gadget, and of button I (Test, Use, Save, Cancel)
gad() { echo "$((IX + 200)) $((IY + 6 + $1 * (LH + 6) + 3))"; }
btn() { echo "$((IX + 8 + $1 * 72 + 33)) $((IY + 6 + 3 * (LH + 6) + 4 + 4))"; }
# the first window zdesk places (the cascade's first, below the 11-line bar): its frame at 32, 19; the flat theme's
# inside 1 right and 12 down (15 with an 11-line font: the title bar 3 taller)
desk_script() {
cat <<'EOS'
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send cp -r /tests/fonts/* /usr/share/fonts/amiga/; mkdir -p /tmp/ttf; mv /usr/share/fonts/truetype/*.ttf /tmp/ttf/; echo acceleration none > /etc/zdesk/mouse; echo speed 200 >> /etc/zdesk/mouse; printf 'right cmd 3600 echo .\\n' > /etc/zdesk/bar\r
sleep 2
send cp /etc/zdesk/screen /tmp/s0; rm -f /etc/zdesk/theme $HOME/.config/zdesk/theme.use; zdesk -v > /tmp/z.log 2>&1 &\r
sleep 8
send deskprefs -v > /tmp/p1.log 2>&1 &\r
sleep 5
shot open
EOS
IX=32 IY=31 LH=8
clk $(gad 1)                            # the font: zbad 11
clk $(btn 0)                            # Test: refused
echo "sleep 2"; echo "shot refused"
clk $(gad 1); clk $(gad 1)              # zfixed 9, ztest 11
clk $(btn 0)                            # Test: the font now
echo "sleep 3"; echo "shot font"
IY=34 LH=11
clk $(gad 0)                            # the mode: interlaced
clk $(btn 0)                            # Test
echo "sleep 4"; echo "shot lace"
clk $(btn 3) 1                          # Cancel (on the interlaced screen)
cat <<'EOS'
sleep 4
shot cancelled
send echo files-1; cat /etc/zdesk/screen | grep -v '^#'; ls /etc/zdesk/theme $HOME/.config/zdesk/theme.use\r
sleep 2
send deskprefs -v > /tmp/p2.log 2>&1 &\r
sleep 5
send gcontest keys 4d cd 4e ce 4e ce 4e ce 4d cd 4e ce\r
sleep 2
send gcontest keys 16 96\r
sleep 5
shot used
send echo files-2; cmp /etc/zdesk/screen /tmp/s0 && echo screen-unchanged; cat $HOME/.config/zdesk/theme.use\r
sleep 2
send deskprefs -v > /tmp/p3.log 2>&1 &\r
sleep 5
send gcontest keys 45 c5\r
sleep 3
send deskprefs -v > /tmp/p4.log 2>&1 &\r
sleep 5
send gcontest keys 4e ce\r
sleep 1
send gcontest keys 21 a1\r
sleep 6
shot saved
send echo files-3; grep -v '^#' /etc/zdesk/screen; grep -c '^#' /etc/zdesk/screen; cat /etc/zdesk/theme; ls $HOME/.config/zdesk/theme.use\r
sleep 2
send cat /tmp/z.log /tmp/p1.log /tmp/p2.log /tmp/p3.log /tmp/p4.log; echo desk-mid-$((40+2))\r
wait desk-mid-42 30
send kill $(pgrep zdesk)\r
sleep 5
send reboot\r
wait zenix:/# 120
sleep 1
send echo quit > /tmp/c2; zdesk -v > /tmp/z2.log 2>&1 &\r
sleep 10
send dpytest -f /tmp/c2\r
sleep 3
shot reboot
send cat /tmp/z2.log; cp /tmp/s0 /etc/zdesk/screen; rm -f /etc/zdesk/theme; echo desk-end-$((40+2))\r
wait desk-end-42 30
EOS
}
con_script() {
cat <<'EOS'
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send cp /etc/console /tmp/c0; echo state-0; conprefs -q\r
sleep 2
send conprefs 2>/tmp/cp.log\r
sleep 3
send \x1b[D
sleep 1
send t
sleep 3
send c
sleep 3
send echo state-1; conprefs -q\r
sleep 2
send conprefs 2>>/tmp/cp.log\r
sleep 3
send \x1b[D
sleep 1
send \x1b[B
sleep 1
send \x1b[C
sleep 1
send u
sleep 3
send echo state-2; conprefs -q; cat /var/run/console; cmp /etc/console /tmp/c0 && echo console-unchanged\r
sleep 2
send clear; printf '\342\226\210\342\226\210\342\226\210\342\226\210\n'; sleep 8\r
sleep 3
shot nofont
sleep 6
send conprefs 2>>/tmp/cp.log\r
sleep 3
send c
sleep 3
send echo state-3; conprefs -q\r
sleep 2
send conprefs 2>>/tmp/cp.log\r
sleep 3
send \x1b[D
sleep 1
send \x1b[B
sleep 1
send \x1b[D
sleep 1
send s
sleep 4
send echo state-4; conprefs -q; grep -v '^#' /etc/console; grep -c '^#' /etc/console; ls /var/run/console\r
sleep 2
send clear; echo lace-text; (sleep 3; termtest mode 1 > /tmp/m1) & termtest winch\r
sleep 9
send q
sleep 2
send echo state-6; cat /tmp/m1; conprefs -q; termtest screen /dev/tty1 lace-text 5 | grep -c '^row'\r
sleep 4
send printf '\\033[2J\\033[1;1H\342\226\210\\033[64;1H\342\226\210\\033[2;1H'; sleep 6\r
sleep 3
shot lace
sleep 5
send conprefs 2>>/tmp/cp.log\r
sleep 3
send \x1b[B
sleep 1
send \x1b[B
sleep 1
send \x1b[C
sleep 1
send u
sleep 4
send echo state-7; conprefs -q; cat /var/run/console\r
sleep 2
send cat /tmp/cp.log; printf 'bogus 1\nbogus 2\ncolours 7\nmode hires-lace\n' >> /etc/console; echo con-mid-$((40+2))\r
wait con-mid-42 30
send reboot\r
wait zenix:/# 120
sleep 1
send echo state-5; conprefs -q; ls /var/run/console\r
sleep 2
send clear; printf '\342\226\210\342\226\210\342\226\210\342\226\210\n'; sleep 8\r
sleep 3
shot font
sleep 6
send cp /tmp/c0 /etc/console; echo con-end-$((40+2))\r
wait con-end-42 30
EOS
}
if [ "$PART" = con ]; then con_script > "$OUT/script.txt"; else desk_script > "$OUT/script.txt"; fi
if [ -z "$PREFS_RECHECK" ]; then       # PREFS_RECHECK=1 with the same ZENIX_OUT: the checks again on the last run's files
    make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
        { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
    ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=600 timeout -k 20 900 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/p" >/dev/null 2>&1
fi
L=$OUT/serial.txt; tr -d '\r' < "$OUT/p.serial.log" > "$L"
python3 tools/colour-test.py columns "$OUT/p-columns.png" > "$OUT/geom"; rc=$?; check "the text console's geometry ($(head -1 "$OUT/geom"))" $rc
awk '{ t = $3 - 7.5 * $4; h = $4 / 2; print $1, $2, t + 7.5 * h, h }' "$OUT/geom" > "$OUT/geom2"   # interlaced
px() { s=$1 g=$2; shift 2; [ -f "$OUT/p-$s.png" ] || { echo "no screenshot $s"; return 1; }; python3 tools/dpy-test.py px "$OUT/p-$s.png" "$OUT/$g" "$@"; }

if [ "$PART" = con ]; then
    grep -a -q '^conprefs: colours 16, font auto, mode hires$' "$L"; check "conprefs -q at the start: 16 colours (AGA with fast RAM), the font auto, hires" $?
    grep -a -q '^conprefs: test: colours 8, font auto, mode hires$' "$L"; check "8 colours and Test: applied (said on its standard error)" $?
    [ "$(grep -a -A1 '^state-1' "$L" | tail -1)" = "conprefs: colours 16, font auto, mode hires" ]; check "Cancel: 16 colours again" $?
    [ "$(grep -a -A1 '^state-2' "$L" | tail -1)" = "conprefs: colours 8, font none, mode hires" ]; check "8 colours, no Unicode font, Use: both in effect (conprefs -q)" $?
    grep -a -A5 '^state-2' "$L" | grep -q '^console-unchanged' && grep -a -A4 '^state-2' "$L" | grep -q '^font none'
    check "... /var/run/console holds them, /etc/console unchanged" $?
    # a block character (U+2588) at row 0, columns 0-3: without the font the box (its middle empty: the background)
    px nofont geom 3,3,000 11,3,000 3,1,BBB 3,6,BBB > "$OUT/p1"; rc=$?; check "... the block drawn as the box without the font ($(cat "$OUT/p1"))" $rc
    [ "$(grep -a -A1 '^state-3' "$L" | tail -1)" = "conprefs: colours 8, font none, mode hires" ]; check "Cancel at once: nothing changes" $?
    [ "$(grep -a -A1 '^state-4' "$L" | tail -1)" = "conprefs: colours 4, font auto, mode hires" ]; check "4 colours, the font auto, Save: in effect" $?
    grep -a -A4 '^state-4' "$L" | grep -q '^colours 4$' && grep -a -A4 '^state-4' "$L" | grep -q '^font auto$' &&
        grep -a -A6 '^state-4' "$L" | grep -q '^mode hires$' &&
        grep -a -A6 '^state-4' "$L" | grep -q '^login text$' && grep -a -A7 '^state-4' "$L" | grep -q 'not found'
    check "... /etc/console's lines replaced (mode hires too), its login line kept, /var/run/console gone" $?
    nc=$(grep -c '^#' rootfs/etc/console)
    [ "$(grep -a -A6 '^state-4' "$L" | grep -x '[0-9]*' | head -1)" = "$nc" ]; check "... its $nc comment lines kept" $?
    grep -a -q '^conprefs: save: colours 4, font auto, mode hires$' "$L"; check "... said on its standard error" $?
    # D28 22: the console interlaced at run time (termtest mode 1, as conprefs' Test does), then hires by conprefs' Use
    grep -a -q '^termtest: size 32 x 80' "$L" && grep -a -q '^termtest: winch 64 x 80' "$L"
    check "CON_MODE interlaced: the foreground (termtest winch) gets SIGWINCH, the console 64 x 80" $?
    s6=$(grep -a -A3 '^state-6' "$L")
    echo "$s6" | grep -q '^mode 1$' && echo "$s6" | grep -q '^conprefs: colours 4, font auto, mode hires-lace$' && echo "$s6" | grep -qx 64
    check "... CON_MODE says so, conprefs -q too, the grid has 64 rows (CON_READ), the text written before kept" $?
    grep -a -q '^display: AGA chips, 2 bitplanes at 64-bit fetch (FMODE 3), interlaced' "$L"; check "... the kernel says it on its log" $?
    px lace geom2 3,3,BBB 3,507,BBB 3,250,000 > "$OUT/p3"; rc=$?
    check "... the screen interlaced: a block at row 1 and one at row 64, black between ($(cat "$OUT/p3"))" $rc
    s7=$(grep -a -A4 '^state-7' "$L")
    echo "$s7" | grep -q '^conprefs: colours 4, font auto, mode hires$' && echo "$s7" | grep -q '^mode hires$'
    check "conprefs' mode line, Use: hires again, /var/run/console says it" $?
    n=$(grep -a -c '^init: /etc/console' "$L")
    grep -a -q '^init: /etc/console: unknown setting bogus (and more lines not understood)$' "$L" && [ "$n" = 1 ]
    check "a broken /etc/console after a reboot: init says one line ($n)" $?
    [ "$(grep -a -A1 '^state-5' "$L" | tail -1)" = "conprefs: colours 4, font auto, mode hires-lace" ] &&
        grep -a -A2 '^state-5' "$L" | grep -q 'not found'
    check "... the saved settings in effect after the reboot (4 colours, the font, mode hires-lace by init), /var/run/console gone" $?
    px font geom2 3,3,BBB 11,3,BBB > "$OUT/p2"; rc=$?; check "... the font loaded at boot: the block drawn solid, interlaced ($(cat "$OUT/p2"))" $rc
    exit $((1 - pass))
fi

# ---- the desktop ----
w=$(grep -a -m1 '^deskprefs: window' "$L")
[ "$(echo "$w" | cut -d' ' -f3-6)" = "32 31 300 82" ]; check "deskprefs' window where zdesk puts the first one: its inside at 32,31, 300 x 82 ($w)" $?
px open geom 140,40,47B 140,54,BBC 50,40,EEE > "$OUT/q1"; rc=$?; check "... its gadgets: the first line chosen ($(cat "$OUT/q1"))" $rc
grep -a -q '^deskprefs: Test: mode Hires, font zbad 11, colours flat: That font cannot be read$' "$L"
check "a damaged font, Test: refused, said in the window (and on its output)" $?
grep -a -q '^displayd: /usr/share/fonts/amiga/zbad/11: cut short in its hunk: the built-in 8x8 is used' "$L"; check "... displayd says why, in one line" $?
grep -a -q '^deskprefs: test: mode Hires, font ztest 11, colours flat$' "$L" && grep -a -q '^displayd: font ztest 11: 11 lines' "$L"
check "ztest 11, Test: the desktop's font changed live (DPY_FONT_SET)" $?
grep -a -q '^zdesk: screen 640x256, font ztest 11 (11 lines: bar 14, title bar 14)' "$L"; check "... zdesk: the bar and the title bars 3 lines taller (EV_SCREEN)" $?
px font geom 600,13,112 600,14,344 > "$OUT/q2"; rc=$?; check "... the bar's line at line 13, the desktop from 14 ($(cat "$OUT/q2"))" $rc
grep -a -q '^deskprefs: test: mode Hires interlaced, font ztest 11, colours flat$' "$L" && grep -a -q '^displayd: screen 640x512 interlaced' "$L"
check "interlace, Test: 640 x 512" $?
px lace geom2 600,13,112 600,500,344 > "$OUT/q3"; rc=$?; check "... the screen interlaced, the bar's line at 13 ($(cat "$OUT/q3"))" $rc
grep -a -q '^deskprefs: cancel: ' "$L" && grep -a -q '^displayd: font 8x8$' "$L" && grep -a -q '^displayd: screen 640x256$' "$L"
check "Cancel: hires and the 8 x 8 again" $?
px cancelled geom 600,10,112 600,11,344 140,40,344 > "$OUT/q4"; rc=$?; check "... the bar 11 lines again, the window gone ($(cat "$OUT/q4"))" $rc
grep -a -A3 '^files-1' "$L" | grep -q 'theme.use: not found' && ! grep -a -A3 '^files-1' "$L" | grep -q '^font ztest'
check "... nothing written: the screen file as it was, no theme files" $?
grep -a -q '^deskprefs: use: mode Hires, font ztest 11, colours platinum$' "$L" && grep -a -q '^zdesk: theme platinum$' "$L"
check "the keyboard: ztest 11 and platinum, Use: applied (the theme by zdesk's SIGHUP)" $?
px used geom 400,200,67A 600,13,000 > "$OUT/q5"; rc=$?; check "... the platinum desktop, the bar 14 lines ($(cat "$OUT/q5"))" $rc
grep -a -A3 '^files-2' "$L" | grep -q '^screen-unchanged' && grep -a -A3 '^files-2' "$L" | grep -q '^platinum$'
check "... /etc/zdesk/screen unchanged, the session's theme in ~/.config/zdesk/theme.use" $?
grep -a -q '^deskprefs: cancel: mode Hires, font ztest 11, colours platinum$' "$L"; check "Esc (Cancel) at once: everything as it was" $?
grep -a -q '^deskprefs: save: mode Hires interlaced, font ztest 11, colours platinum$' "$L"; check "interlace, Save: applied" $?
f3=$(grep -a -A8 '^files-3' "$L")
echo "$f3" | grep -q '^mode hires-lace$' && echo "$f3" | grep -q '^font ztest 11$' && echo "$f3" | grep -q '^platinum$' &&
    echo "$f3" | grep -q 'theme.use: not found'
check "... /etc/zdesk/screen and /etc/zdesk/theme written (root's Save), theme.use gone" $?
nc=$(grep -c '^#' rootfs/etc/zdesk/screen)
echo "$f3" | grep -qx "$nc"; check "... the screen file's $nc comment lines kept" $?
grep -a -q '^zdesk: running, theme platinum, .*font ztest 11 (11 lines: bar 14, title bar 14)' "$L" &&
    grep -a -q '^dpytest: open 0 screen 640x512 depth 4' "$L"
check "after a reboot: zdesk on the saved settings (platinum, ztest 11, 640 x 512 interlaced)" $?
px reboot geom2 400,300,67A > "$OUT/q6"; rc=$?; check "... the platinum desktop on the interlaced screen ($(cat "$OUT/q6"))" $rc
exit $((1 - pass))
