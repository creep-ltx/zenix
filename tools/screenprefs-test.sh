#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# screenprefs (DESIGN D62, docs/d62-screen-modes.md 10), each face checked for what it must do, in FS-UAE through the
# cage rig (tools/shot.py) on the stock A1200 with 4 MB of fast RAM (a test build with DIWV=0x38), as root:
# `screenprefs-test.sh cli`: the command line, its output compared:
#  - --list and -q on the AGA PAL machine; conprefs -q (the old name's frozen line); usage errors; a user refused;
#  - the video standard switched live (CON_MODE's CONM_NTSC): the console 80 x 25 (SIGWINCH, the grid's rows read
#    back), the kernel's line, then PAL again; the console interlaced and back;
#  - the fall-back: a new mode asked for at the terminal and not answered goes back by itself after 10 s;
#  - -s: /etc/screen's lines written (its comments kept), the older /etc/console's moved over, and a reboot: init
#    applies /etc/screen (NTSC, interlaced, 8 colours), the console 80 x 50.
# `screenprefs-test.sh con`: the text form on the console, typed on the serial line (its keys as escape sequences):
#  Test (8 colours), Cancel, the mode and Use with "keep", the mode and Test left unanswered (back by itself), Save,
#  Esc; screenshots of the form.
# `screenprefs-test.sh desk`: the window under zdesk, clicked with the mouse (gcontest mouse) and keys: interlace and
#  Test kept, super-hires and Test (1280 x 256, every window kept on it), 8 colours (depth 3) and Use, the fall-back
#  unanswered, Cancel; a user's Save into ~/.config/screen/screen (the older ~/.config/zdesk/screen moved), applied by
#  zdesk at the next session; screenshots of the window.
# `screenprefs-test.sh rtg`: a pretend graphics card (dpytest -fb, rtg-test's): --list names the card, the desktop's
#  mode is the card's, -s rtg 1280x960x16 writes the size for zenixboot's RTGMODE.
cd "$(dirname "$0")/.." || exit 1
PART=${1:-cli}
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-sprefs-serial}
OUT=${ZENIX_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/zenix-sprefs.XXXXXX")}; mkdir -p "$OUT"
B=$OUT/build
trap 'rm -rf "$B"' EXIT
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
# a click of the left button at screen pixel X, Y (prefs-test's: no acceleration, speed 200: a count 2 pixels across, a
# line down; interlaced (l) two lines, super-hires (s) half a line: d61-mouse.md 5)
clk() {
    local x=$(($1 >> 1)) y=$2 m="-128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0"
    case ${3:-} in l) y=$((y >> 1)) ;; s) y=$((y << 1)) ;; esac  # a count down: a line, two laced, half in super-hires
    while [ "$x" -gt 0 ] || [ "$y" -gt 0 ]; do
        local dx=$((x > 127 ? 127 : x)) dy=$((y > 127 ? 127 : y))
        m="$m $dx,$dy,0"; x=$((x - dx)); y=$((y - dy))
    done
    echo "send gcontest mouse $m 0,0,1 0,0,0\\r"
    echo "sleep 2"
}

cli_script() {
cat <<'EOS'
wait zenix:/# 120
sleep 1
send cp /etc/console /tmp/c0; echo list-0; screenprefs -l; echo q-0; screenprefs -q; conprefs -q\r
sleep 3
send screenprefs bogus; echo rc=$?; screenprefs -x; echo rc=$?; screenprefs --version; echo rc=$?\r
sleep 2
send echo 'screenprefs -y console hires-lace; echo rc=$?' | su guest; echo user-done\r
sleep 3
send clear; echo ntsc-text; (sleep 3; screenprefs -y video ntsc > /tmp/v1 2>&1) & termtest winch\r
sleep 9
send q
sleep 2
send echo state-1; cat /tmp/v1; screenprefs -q; termtest screen /dev/tty1 ntsc-text 5 | grep -c '^row'\r
sleep 3
shot ntsc
send screenprefs -y video pal; screenprefs -y console hires-lace; echo state-2; screenprefs -q; termtest screen /dev/tty1 state-2 5 | grep -c '^row'\r
sleep 4
send screenprefs -y console hires; echo state-3; screenprefs -q\r
sleep 3
send screenprefs console hires-lace; echo state-4 rc=$?; screenprefs -q\r
sleep 16
send printf 'colours 8\nmode hires\n' >> /etc/console; screenprefs -s -y video ntsc; echo state-5a; grep -v '^#' /etc/console\r
sleep 3
send screenprefs -s -y console hires-lace colours 8; echo state-5; grep -v '^#' /etc/screen; grep -c '^#' /etc/screen; grep -v '^#' /etc/console\r
sleep 4
send echo cli-mid-$((40+2))\r
wait cli-mid-42 30
send reboot\r
wait zenix:/# 120
sleep 1
send echo state-6; screenprefs -q; termtest screen /dev/tty1 state-6 5 | grep -c '^row'\r
sleep 3
shot reboot
send screenprefs -s -y video auto; screenprefs -s -y console hires colours auto; cp /tmp/c0 /etc/console; echo cli-end-$((40+2))\r
wait cli-end-42 30
EOS
}

con_script() {
cat <<'EOS'
wait zenix:/# 120
sleep 1
send cp /etc/console /tmp/c0; screenprefs 2>/tmp/sp.log\r
sleep 4
shot form
send \t
sleep 2
send \x20\x20\x20
sleep 3
send t
sleep 3
send c
sleep 3
send echo state-1; screenprefs -q\r
sleep 2
send screenprefs 2>>/tmp/sp.log\r
sleep 3
send \x20
sleep 2
send u
sleep 3
shot ask
send y
sleep 3
send echo state-2; screenprefs -q; cat /var/run/console\r
sleep 2
send screenprefs 2>>/tmp/sp.log\r
sleep 4
send \x20
sleep 2
send t
sleep 16
send \x1b
sleep 3
send echo state-3; screenprefs -q\r
sleep 2
send screenprefs 2>>/tmp/sp.log\r
sleep 4
send \x20\t
sleep 2
send \x20\x20\x20
sleep 3
send s
sleep 4
send y
sleep 3
send echo state-4; screenprefs -q; grep '^console' /etc/screen; ls /var/run/console\r
sleep 3
send cat /tmp/sp.log; screenprefs -s -y console hires colours auto; cp /tmp/c0 /etc/console; echo con-end-$((40+2))\r
wait con-end-42 30
EOS
}
# the window's layout (display/lib/prefswin.c, 400 wide, the gadgets from 136) with the 8 x 8, its inside at IX, IY (the
# cascade's first, below the 11-line bar: zdesk's flat theme): the middle of row I's gadget, and of button I
gad() { echo "$((IX + 156)) $((IY + 6 + $1 * 14 + 3))"; }
btn() { echo "$((IX + 8 + $1 * 97 + 45)) $((IY + 126))"; }
desk_script() {
cat <<'EOS'
wait zenix:/# 120
sleep 1
send echo acceleration none > /etc/zdesk/mouse; echo speed 200 >> /etc/zdesk/mouse; printf 'right cmd 3600 echo .\\n' > /etc/zdesk/bar\r
sleep 2
send zdesk -v > /tmp/z.log 2>&1 &\r
sleep 8
send screenprefs -W -v > /tmp/w1.log 2>&1 &\r
sleep 5
shot open
EOS
IX=32 IY=31
clk $(gad 4); clk $(btn 0)              # the mode: interlaced; Test
echo "sleep 4"; echo "shot ask"
clk $(btn 0) l                          # Keep
echo "sleep 2"
clk $(gad 4) l; clk $(btn 0) l          # super-hires; Test
echo "sleep 4"
clk $(btn 0) s                          # Keep
echo "sleep 2"; echo "shot shres"
clk $(gad 5) s; clk $(btn 1) s          # 8 colours; Use
echo "sleep 4"
clk $(btn 0) s                          # Keep: the window closes
cat <<'EOS'
sleep 4
shot depth3
send screenprefs -W -v > /tmp/w2.log 2>&1 &\r
sleep 5
EOS
IX=56 IY=43                             # (the cascade's second place)
clk $(gad 4) s; clk $(btn 0) s          # super-hires laced; Test, unanswered
echo "sleep 16"; echo "shot back"
clk $(btn 3) s                          # Cancel
cat <<'EOS'
sleep 4
send screenprefs -W -v > /tmp/w3.log 2>&1 &\r
sleep 5
send gcontest keys 4e ce 4e ce 4e ce 4d cd 4e ce 4d cd 4d cd 4e ce\r
sleep 2
send gcontest keys 21 a1\r
sleep 4
send gcontest keys 15 95\r
sleep 4
shot saved
send echo files-1; grep '^desktop' /etc/screen; cat /etc/zdesk/theme\r
sleep 2
send screenprefs -s -y console hires-lace; echo busy-rc=$?; grep '^console ' /etc/screen; screenprefs -q; screenprefs -s -y console hires\r
sleep 3
send echo 'mkdir -p $HOME/.config/zdesk; echo mode hires-lace > $HOME/.config/zdesk/screen; screenprefs -s desktop superhires 3; echo rc=$?; cat $HOME/.config/screen/screen; ls $HOME/.config/zdesk/screen' | su guest; echo guest-done\r
sleep 4
send cat /tmp/z.log /tmp/w1.log /tmp/w2.log /tmp/w3.log; echo desk-mid-$((40+2))\r
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
send cat /tmp/z2.log; screenprefs -s -y desktop hires auto; echo flat > /etc/zdesk/theme; echo desk-end-$((40+2))\r
wait desk-end-42 30
EOS
}
rtg_script() {
cat <<'EOS'
wait zenix:/# 120
sleep 1
send (sleep 40 | dpytest -fb 1024x768x2 > /tmp/d.log 2>&1) &\r
sleep 10
send echo list-1; screenprefs -l; screenprefs -q\r
sleep 3
shot card
send screenprefs desktop hires-lace; echo rc=$?; screenprefs -s rtg 1280x960x16; echo rc=$?; grep '^rtg' /etc/screen; echo list-2; screenprefs -l | grep '^rtg' | grep '[*]'\r
sleep 4
send screenprefs rtg 800x600; echo rc=$?; screenprefs -s rtg 99x99; echo rc=$?; echo rtg-end-$((40+2))\r
wait rtg-end-42 60
EOS
}

case $PART in
cli) cli_script > "$OUT/script.txt" ;;
con) con_script > "$OUT/script.txt" ;;
desk) desk_script > "$OUT/script.txt" ;;
rtg) rtg_script > "$OUT/script.txt" ;;
*) echo "usage: tools/screenprefs-test.sh cli|con|desk|rtg" >&2; exit 2 ;;
esac
if [ -z "$SPREFS_RECHECK" ]; then       # SPREFS_RECHECK=1 with the same ZENIX_OUT: the checks again on the last run's files
    make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
        { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
    ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=600 timeout -k 20 900 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/p" >/dev/null 2>&1
fi
L=$OUT/serial.txt; tr -d '\r' < "$OUT/p.serial.log" | sed 's/\x1b\[[0-9;]*[mK]//g' > "$L"   # (grep's colours out)
after() { grep -a -m1 -A"$2" "^$1\( \|$\)" "$L" | tail -n +2; }   # (the marker line itself: state-5, not state-5a)

if [ "$PART" = cli ]; then
    cat > "$OUT/list.want" <<'EOW'
video pal (pal ntsc)
console hires 640x256 80x32 *
console hires-lace 640x512 80x64
desktop hires 640x256 depth 3,4 *
desktop hires-lace 640x512 depth 3,4
desktop superhires 1280x256 depth 3,4
desktop superhires-lace 1280x512 depth 3,4
rtg front * (no graphics card in use)
EOW
    after list-0 8 > "$OUT/list.got"
    cmp -s "$OUT/list.want" "$OUT/list.got"; check "--list on the AGA PAL A1200: PAL or NTSC, the console's two modes, the desktop's four (super-hires: AGA), no card" $?
    after q-0 2 > "$OUT/q0"
    grep -q '^screenprefs: video auto (pal shown), console hires, colours auto, font auto, desktop hires, depth auto 4, desktop-font builtin$' "$OUT/q0" &&
        grep -q '^conprefs: colours auto, font auto, mode hires$' "$OUT/q0"
    check "-q: the settings in effect in one line; conprefs -q its old line ($(tr '\n' '|' < "$OUT/q0"))" $?
    grep -a -q '^screenprefs: bogus: not understood here$' "$L" || grep -a -q 'usage: screenprefs' "$L"
    [ "$(grep -a -c '^rc=2$' "$L")" -ge 2 ] && grep -a -q '^screenprefs: unknown option -x' "$L" && grep -a -q '^screenprefs .*[0-9]' "$L"
    check "a wrong word and an unknown option: the usage, status 2; --version" $?
    grep -a -B1 '^user-done' "$L" | grep -q '^rc=1$' && grep -a -q "^screenprefs: console: the console's and the system's settings are root's" "$L"
    check "a user: the console's mode refused (status 1, said)" $?
    s1=$(after state-1 4)
    grep -a -q '^termtest: size 32 x 80' "$L" && grep -a -q '^termtest: winch 25 x 80' "$L" && echo "$s1" | grep -qx 25 &&
        echo "$s1" | grep -q '(ntsc shown)'
    check "video ntsc live: SIGWINCH, the console 80 x 25 (its grid read back), -q says NTSC is shown" $?
    grep -a -q 'display: AGA chips, 4 bitplanes at 64-bit fetch (FMODE 3)' "$L"; check "... the kernel's screen made again" $?
    s2=$(after state-2 2)
    echo "$s2" | grep -q '(pal shown), console hires-lace' && echo "$s2" | grep -qx 64
    check "PAL again, then interlaced: 80 x 64" $?
    after state-3 1 | grep -q '(pal shown), console hires,'; check "hires again" $?
    s4=$(after state-4 1)
    grep -a -q '^screenprefs: keep this mode? y keeps it; back by itself in 10 s$' "$L" &&
        grep -a -q '^screenprefs: console: not kept: back to the mode before$' "$L" && grep -a -A1 '^state-4' "$L" | grep -q 'rc=1' &&
        after state-4 1 | grep -q 'console hires,'
    check "the fall-back: a new mode at the terminal, unanswered: back by itself after 10 s (status 1)" $?
    s5a=$(after state-5a 3)
    echo "$s5a" | grep -qx 'colours 8' && echo "$s5a" | grep -qx 'mode hires'
    check "-s video alone: /etc/console's older console lines stay (moved only with the console's own)" $?
    s5=$(after state-5 14)
    echo "$s5" | grep -qx 'video ntsc' && echo "$s5" | grep -qx 'console hires-lace' && echo "$s5" | grep -qx 'console-colours 8' &&
        echo "$s5" | grep -qx 'console-font auto'
    check "-s: /etc/screen's lines written ($(echo "$s5" | grep -v '^[0-9]*$' | tr '\n' '|'))" $?
    nc=$(grep -c '^#' rootfs/etc/screen); echo "$s5" | grep -qx "$nc"; check "... its $nc comment lines kept" $?
    ! echo "$s5" | grep -q '^colours 8' && echo "$s5" | grep -qx 'login text'
    check "... /etc/console's older colours and mode lines moved out, its login line kept" $?
    s6=$(after state-6 2)
    echo "$s6" | grep -q 'video ntsc (ntsc shown), console hires-lace, colours 8' && echo "$s6" | grep -qx 50
    check "a reboot: init applies /etc/screen: NTSC, interlaced, 8 colours: 80 x 50 ($(echo "$s6" | tr '\n' '|'))" $?
    ! grep -a -q '^init: .*not understood' "$L"; check "... init understood every line" $?
    exit $((1 - pass))
fi

if [ "$PART" = con ]; then
    grep -a -q '^screenprefs: test: video auto, console hires, console-colours 8, console-font auto, desktop hires, desktop-depth auto, desktop-font builtin, theme flat$' "$L"
    check "the form: 8 colours and Test (said on its standard error)" $?
    after state-1 1 | grep -q 'console hires, colours auto,'; check "Cancel: the colours as they were" $?
    s2=$(after state-2 5)
    echo "$s2" | grep -q 'console hires-lace, colours auto' && echo "$s2" | grep -qx 'mode hires-lace'
    check "the mode, Use and y (kept): interlaced, /var/run/console says so" $?
    grep -a -q '^screenprefs: reverted: .*console hires-lace.*: Not kept: back to the mode before$' "$L" &&
        after state-3 1 | grep -q 'console hires-lace,'
    check "the mode and Test unanswered: back by itself after 10 s, then Esc" $?
    s4=$(after state-4 4)
    echo "$s4" | grep -q 'console hires, colours 8' && echo "$s4" | grep -qx 'console hires' &&
        echo "$s4" | grep -qx 'console-colours 8' && after state-4 5 | grep -q 'not found'
    check "Tab and Space: hires and 8 colours, Save and y: in effect, /etc/screen written, /var/run/console gone" $?
    [ -f "$OUT/p-form.png" ] && [ -f "$OUT/p-ask.png" ]; check "screenshots of the form and of its question" $?
    exit $((1 - pass))
fi

if [ "$PART" = desk ]; then
    w=$(grep -a -m1 '^screenprefs: window 32 ' "$L")
    [ "$(echo "$w" | cut -d' ' -f3-6)" = "32 31 400 152" ]; check "the window where zdesk puts the first one: its inside at 32,31, 400 x 152 ($w)" $?
    grep -a -q 'displayd: screen 640x512 interlaced, 4 planes' "$L" && grep -a -q '^screenprefs: test: .*desktop hires-lace, desktop-depth auto' "$L"
    check "the mouse: interlace and Test, kept with the Keep button: 640 x 512" $?
    grep -a -q 'displayd: screen 1280x256, 4 planes' "$L" && grep -a -q '^screenprefs: test: .*desktop superhires, desktop-depth auto' "$L"
    check "super-hires and Test, kept: 1280 x 256 (AGA)" $?
    grep -a -q 'displayd: screen 1280x256, 3 planes' "$L" && grep -a -q '^screenprefs: use: .*desktop superhires, desktop-depth 3' "$L"
    check "8 colours and Use, kept: 1280 x 256 in 3 planes, the window closed" $?
    grep -a -q '^zdesk: screen 1280x256' "$L"; check "... zdesk follows the screen (EV_SCREEN)" $?
    grep -a -q '^screenprefs: reverted: .*desktop superhires, desktop-depth 3.*Not kept' "$L" &&
        grep -a -q 'displayd: screen 1280x512 interlaced, 3 planes' "$L" && [ "$(grep -a -c 'displayd: screen 1280x256, 3 planes' "$L")" -ge 2 ]
    check "super-hires laced and Test unanswered: 1280 x 512, then back to 1280 x 256 by itself after 10 s" $?
    grep -a -q '^screenprefs: cancel: .*desktop superhires, desktop-depth 3' "$L"; check "... Cancel: the window as it opened" $?
    f1=$(after files-1 4)
    echo "$f1" | grep -qx 'desktop hires-lace' && echo "$f1" | grep -qx 'desktop-depth 4' && grep -a -q '^screenprefs: save: .*desktop hires-lace, desktop-depth 4.*theme platinum' "$L" &&
        after files-1 4 | grep -qx platinum
    check "the keys: interlace, 16 colours, platinum, Save and y: /etc/screen's desktop lines, /etc/zdesk/theme" $?
    g=$(grep -a -B4 '^guest-done' "$L")
    echo "$g" | grep -qx 'rc=0' && echo "$g" | grep -qx 'desktop superhires' && echo "$g" | grep -qx 'desktop-depth 3' &&
        echo "$g" | grep -q 'zdesk/screen: no such file'
    check "a user's -s desktop: ~/.config/screen/screen written, the older ~/.config/zdesk/screen moved ($(echo "$g" | tr '\n' '|'))" $?
    b=$(grep -a -A3 '^busy-rc' "$L")
    echo "$b" | grep -qx 'busy-rc=0' && echo "$b" | grep -qx 'console hires-lace' && echo "$b" | grep -q 'console hires,'
    check "-s console with the desktop holding the screen: saved (status 0), not applied (the console stays hires)" $?
    grep -a -q '^dpytest: open 0 screen 640x512 depth 4' "$L"; check "a reboot: the desktop on /etc/screen's saved mode (640 x 512, 16 colours)" $?
    exit $((1 - pass))
fi

if [ "$PART" = rtg ]; then
    l1=$(after list-1 22)
    echo "$l1" | grep -qx 'desktop rtg 1024x768 (the card'"'"'s, zenixboot RTG) \*' && echo "$l1" | grep -qx 'rtg front \*' &&
        echo "$l1" | grep -q 'desktop rtg,'
    check "--list on the pretend card: the desktop's mode is the card's, the card's sizes, front in use" $?
    grep -a -q '^screenprefs: desktop: the desktop is on a graphics card' "$L" && grep -a -q '^rc=1$' "$L"
    check "a chipset mode on the card: refused (status 1)" $?
    grep -a -q '^screenprefs: the card.s size counts from the next start, with zenixboot RTG RTGMODE=1280x960x16$' "$L" &&
        grep -a -qx 'rtg 1280x960x16' "$L" && after list-2 1 | grep -q '^rtg 1280x960x16 .*\*'
    check "-s rtg 1280x960x16: /etc/screen's line, the zenixboot line said, --list marks it" $?
    [ "$(grep -a -c '^rc=2$' "$L")" -ge 2 ]; check "rtg without -s, and 99x99: refused (usage, status 2)" $?
    exit $((1 - pass))
fi
