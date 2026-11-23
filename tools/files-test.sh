#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The file view, files (DESIGN D48, docs/d48-fileview.md), in FS-UAE through the cage rig (tools/shot.py) on the stock
# A1200 with 4 MB of fast RAM (a test build with DIWV=0x38), as root, under zdesk. The keys are typed and the mouse
# moved through CON_INPUT (gcontest keys, gcontest mouse); `files -v` writes every row it draws (`files: row N[*]:
# TEXT`, * the cursor's), what it says and starts, and its window's place: the window's text is read from that
# (tail of its log over the serial line at each step), its pixels from the screenshots (tools/dpy-test.py px):
#  - /tmp/fv (a folder, two text files, a program, a hidden file): the rows in order, folders first, sizes as ls -h,
#    the cursor's row in the selection colour; j j, G, gg; l into the folder and h back with the cursor on it; zh
#  - the keys file ~/.config/files/keys written at the first start with the built-in keys
#  - yy and pp into the folder (the copy byte for byte), pp again (the name exists: asked, n: nothing done), cw
#    rename, dd and pp (moved), M a new folder, two marked with Space and dD (asked, y: both gone): each on the disk
#  - the mouse: a click moves the cursor, a double click enters the folder
#  - Return on the text file: term -e less started (the process list); on the program: "Run prog? (y/n)", n
#  - the keys file changed (a key added, a bad line): the bad line said at the start, the new key works
#  - /tmp/big, 500 files: 500 entries, the time from the start to the drawn window and ls's to /dev/null (emulator
#    figures), G on the last name
# Printed: the program's size and memory (ps).
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-files-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-files.XXXXXX)}; mkdir -p "$OUT"
B=$OUT/build
trap 'rm -rf "$B"' EXIT
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
typ() { python3 tools/keycodes.py -c 40 "$1" | while read -r l; do echo "send gcontest keys $l\\r"; echo "sleep 1"; done; }
key() { echo "send gcontest keys $1\\r"; echo "sleep ${2:-2}"; }      # raw codes (44 c4 Return, 40 c0 Space)
st() { echo "send t $1 ${2:-8}\\r"; echo "sleep 2"; }  # t: a shell function, defined first
# a click (two: a double click) of the left button at screen pixel X, Y: the pointer pushed to the top left, then moved
# in counts of at most 127 (no acceleration: a count is 2 pixels across, a line down)
clk() {
    local x=$(($1 >> 1)) y=$2 m="-128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0 -128,-128,0" b="0,0,1 0,0,0"
    [ "$3" = 2 ] && b="$b $b"
    while [ "$x" -gt 0 ] || [ "$y" -gt 0 ]; do
        local dx=$((x > 127 ? 127 : x)) dy=$((y > 127 ? 127 : y))
        m="$m $dx,$dy,0"; x=$((x - dx)); y=$((y - dy))
    done
    echo "send gcontest mouse $m $b\\r"
    echo "sleep 3"
}
# the first window zdesk places: its inside at IX, IY (checked against what files says); row R's middle
IX=32 IY=31
row() { echo "$((IX + 100)) $((IY + $1 * 8 + 4))"; }
{
cat <<'EOS'
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send t() { echo st-$1; grep 'files: (row|say|start|window)' /tmp/f.log | tail -$2; }\r
sleep 2
send mkdir -p /tmp/fv/sub; printf 'hello\\n' > /tmp/fv/a.txt; seq 1 2000 > /tmp/fv/b.txt; cp /tmp/fv/b.txt /tmp/b0\r
sleep 2
send cp /bin/hello /tmp/fv/prog; chmod 755 /tmp/fv/prog; : > /tmp/fv/.hid; echo in > /tmp/fv/sub/inner; ln -s /tmp/fv/sub /tmp/lnk; rm -rf $HOME/.config/files\r
sleep 2
send mkdir /tmp/big; for i in $(seq 1 500); do : > /tmp/big/f$i; done; echo made-$((40+2))\r
wait made-42 120
send echo acceleration none > /etc/zdesk/mouse; echo speed 200 >> /etc/zdesk/mouse; zdesk -v > /tmp/z.log 2>&1 &\r
sleep 8
send files -v /tmp/fv > /tmp/f.log 2>&1 &\r
sleep 6
shot open
EOS
st 1 9
typ jj; st 2; echo "shot moved"
typ G; st 3
typ gg; st 4
typ l; st 5
typ h; st 6
typ zh; st 7 8
typ zh
# yank a.txt, paste into sub
typ j; typ yy; typ gg; typ l; typ pp; echo "sleep 3"; st 8
echo "send echo st-9; cmp /tmp/fv/a.txt /tmp/fv/sub/a.txt && echo copy-same\\r"; echo "sleep 2"
echo "send echo changed > /tmp/fv/sub/a.txt\\r"; echo "sleep 2"
typ pp; st 10
typ n; st 11 1
echo "send echo st-41; cat /tmp/fv/sub/a.txt\\r"; echo "sleep 2"
typ po; echo "sleep 3"; st 42 4
echo "send echo st-43; cmp /tmp/fv/a.txt /tmp/fv/sub/a.txt && echo replaced-same\\r"; echo "sleep 2"
# the safety refusals: a paste into the entry's own folder, and the same folder through a link (/tmp/lnk -> sub)
typ yy; typ pp; st 44 1
typ gt; typ /; typ lnk; key "44 c4" 2; typ l; echo "sleep 2"; typ pp; st 45 1
echo "send echo st-47; grep 'files: folder' /tmp/f.log | tail -1\\r"; echo "sleep 2"
# rename a.txt (first in sub) to c.txt
typ gg; typ cw; typ c.txt; key "44 c4" 3; st 12
echo "send echo st-13; ls -1 /tmp/fv/sub | cat\\r"; echo "sleep 2"
# back; cut b.txt and paste it into sub
typ h; typ jj; typ dd; typ gg; typ l; typ pp; echo "sleep 3"; st 14
echo "send echo st-15; ls -1 /tmp/fv | cat; cmp /tmp/b0 /tmp/fv/sub/b.txt && echo move-same\\r"; echo "sleep 2"
typ M; typ newd; key "44 c4" 3; st 16
# sub: newd/ b.txt c.txt inner -> mark c.txt and inner, delete
typ gg; typ jj; key "40 c0" 1; key "40 c0" 1; typ dD; st 17
typ y; echo "sleep 3"; st 18
echo "send echo st-19; ls -1 /tmp/fv/sub | cat\\r"; echo "sleep 2"
# the mouse: back in /tmp/fv (sub/ a.txt prog): a click on row 2 (a.txt), a double click on row 1 (sub)
typ h; echo "sleep 1"
clk $(row 2); st 20; echo "shot clicked"
clk $(row 1) 2; st 21
typ h; echo "sleep 1"
# open the text file: less in a terminal window
typ j; key "44 c4" 6; st 22 8
echo "send echo st-23; pgrep term\\r"; echo "sleep 2"
echo "shot less"
typ q; echo "sleep 4"
# the program (the terminal closed by the key: the focus back on files): asked
typ j; key "44 c4" 3; st 24 4
typ n; st 25 2
typ q; echo "sleep 3"
cat <<'EOS'
send echo st-26; pgrep files || echo files-gone; grep -c . $HOME/.config/files/keys; grep -x 'gg top' $HOME/.config/files/keys | cat\r
sleep 2
send printf 'w down\\nZ frob\\n' >> $HOME/.config/files/keys; time ls /tmp/big > /dev/null\r
sleep 8
send files -v /tmp/big > /tmp/f.log 2>&1 &\r
sleep 15
shot big
EOS
echo "send echo st-27; grep 'say: keys' /tmp/f.log | cat\\r"; echo "sleep 2"
typ w; st 28
typ J; st 46 12
typ G; echo "sleep 3"; st 29; echo "shot bigend"
cat <<'EOS'
send echo st-30; grep '^files: (window|folder)' /tmp/f.log | cat; ps | grep files | cat; ls -l /bin/files\r
sleep 3
send echo st-31\r
sleep 1
EOS
typ q; echo "sleep 2"
echo "send cat /tmp/z.log | tail -5; echo end-\$((40+2))\\r"
echo "wait end-42 30"
} > "$OUT/script.txt"
if [ -z "$FILES_RECHECK" ]; then       # FILES_RECHECK=1 with the same ZENIX_OUT: the checks again on the last run's files
    make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
        { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }
    ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/run.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=900 timeout -k 20 1200 python3 tools/shot.py "$OUT/run.fs-uae" "$OUT/script.txt" "$OUT/p" >/dev/null 2>&1
fi
L=$OUT/serial.txt; tr -d '\r' < "$OUT/p.serial.log" > "$L"
python3 tools/colour-test.py columns "$OUT/p-columns.png" > "$OUT/geom"; rc=$?; check "the text console's geometry ($(head -1 "$OUT/geom"))" $rc
px() { s=$1; shift; [ -f "$OUT/p-$s.png" ] || { echo "no screenshot $s"; return 1; }; python3 tools/dpy-test.py px "$OUT/p-$s.png" "$OUT/geom" "$@"; }
# what came after "st-N" (the marker's own line, not the typed command's echo), up to the next prompt
after() { awk -v m="st-$1" '$0 == m { on = 1; next } on && /zenix.*# / { exit } on' "$L"; }
cur() { after "$1" | grep -a 'row [0-9]*\*:' | tail -1 | sed 's/^files: row [0-9]*\*: //'; }
hascur() { cur "$1" | grep -q "^ $2"; }

w=$(after 1 | grep -a -m1 '^files: window' || grep -a -m1 '^files: window' "$L")
o1=$(after 1)
echo "$o1" | grep -q 'row 1\*:  sub/' && echo "$o1" | grep -q 'row 2:  a.txt  *6 ' && echo "$o1" | grep -q 'row 3:  b.txt  *8.6K ' &&
    echo "$o1" | grep -q 'row 4:  prog  ' && ! echo "$o1" | grep -q '\.hid'
rc=$?; check "/tmp/fv listed: sub/ first, a.txt 6, b.txt 8.6K, prog; the hidden name left out ($(echo "$o1" | grep -c row) rows)" $rc
px open $((IX + 250)),$((IY + 12)),47B $((IX + 250)),$((IY + 20)),EEE $((IX + 250)),$((IY + 3)),BBC > "$OUT/q1"; rc=$?
check "... the cursor's row in the selection colour, the next plain, the path row above ($(cat "$OUT/q1"))" $rc
hascur 2 b.txt; rc=$?; check "j j: the cursor on b.txt ($(cur 2))" $rc
px moved $((IX + 250)),$((IY + 12)),EEE $((IX + 250)),$((IY + 28)),47B > "$OUT/q2"; rc=$?; check "... drawn there ($(cat "$OUT/q2"))" $rc
hascur 3 prog; check "G: the last (prog)" $?
hascur 4 sub/; check "gg: the first (sub/)" $?
hascur 5 inner; check "l: into sub, its inner" $?
hascur 6 sub/; check "h: back, the cursor on sub/" $?
after 7 | grep -q 'row [0-9]*:  .hid'; check "zh: the hidden name shown" $?
after 8 | grep -q 'say: 1 entry copied'; check "yy, pp in sub: copied" $?
after 9 | grep -q '^copy-same$'; check "... on the disk, byte for byte" $?
after 10 | grep -q 'say: 1 name exists here: replace it? (y/n)'; check "pp again: the name exists, asked" $?
! after 11 | grep -q 'copied'; check "... n: nothing done" $?
after 41 | grep -qx 'changed'; check "... the file there kept its own content" $?
after 42 | grep -q 'say: 1 entry copied'; check "po: replaced without asking" $?
after 43 | grep -qx 'replaced-same'; check "... on the disk, the source's content" $?
after 44 | grep -q 'say: inner is already here'; check "a paste into its own folder (yy on inner, pp): refused" $?
after 45 | grep -q 'say: inner is already here' && after 47 | grep -q '^files: folder /tmp/fv/sub: '
rc=$?; check "... and through a link to it (/tmp/lnk): refused, the folder shown as /tmp/fv/sub ($(after 47 | cut -c1-40))" $rc
after 12 | grep -q 'say: renamed to c.txt'; check "cw c.txt: renamed" $?
s13=$(after 13); echo "$s13" | grep -qx 'c.txt' && ! echo "$s13" | grep -qx 'a.txt'; rc=$?; check "... on the disk: c.txt, no a.txt ($(echo $s13))" $rc
after 14 | grep -q 'say: 1 entry moved'; check "dd, pp in sub: moved" $?
s15=$(after 15); ! echo "$s15" | grep -qx 'b.txt' && echo "$s15" | grep -qx 'move-same'; rc=$?; check "... on the disk: gone from /tmp/fv, the same in sub ($(echo $s15))" $rc
after 16 | grep -q 'say: made newd'; check "M newd: a new folder" $?
after 17 | grep -q 'say: Delete 2 entries? (y/n)'; check "two marked, dD: asked" $?
after 18 | grep -q 'say: 2 entries deleted'; check "... y: deleted" $?
s19=$(after 19); [ "$(echo $s19)" = "b.txt newd" ]; rc=$?; check "... on the disk: b.txt and newd left ($(echo $s19))" $rc
hascur 20 a.txt; rc=$?; check "a click on row 2: the cursor on a.txt ($(cur 20))" $rc
px clicked $((IX + 250)),$((IY + 20)),47B $((IX + 250)),$((IY + 12)),EEE > "$OUT/q3"; rc=$?; check "... drawn there ($(cat "$OUT/q3"))" $rc
after 21 | grep -q 'row 1\*:  newd/'; rc=$?; check "a double click on sub/: into it ($(cur 21))" $rc
after 22 | grep -q 'start /bin/term -H -e /bin/less /tmp/fv/a.txt'; check "Return on a.txt: term -H -e less" $?
[ "$(after 23 | grep -c '^[0-9][0-9]*$')" -ge 1 ]; rc=$?; check "... the terminal holds its window after less printed the short text (term: $(echo $(after 23)))" $rc
after 24 | grep -q 'say: Run prog? (y/n)'; check "Return on the program: asked" $?
! after 25 | grep -q 'start /bin/term -H -e /tmp/fv/prog'; check "... n: not run" $?
s26=$(after 26); echo "$s26" | grep -qx 'gg top' && [ "$(echo "$s26" | grep -x '[0-9]*' | tail -1)" -gt 40 ] && echo "$s26" | grep -qx 'files-gone'
rc=$?; check "q: files ended; ~/.config/files/keys written with the built-in keys ($(echo "$s26" | grep -x '[0-9]*' | tail -1) lines)" $rc
after 27 | grep -q 'say: keys: line [0-9]*: unknown action frob'; check "the keys file changed: its bad line said at the start" $?
hascur 46 f109 && after 46 | grep -q 'row 20:  f125 '; check "J: half a page (the cursor on f109), the rows scrolled in drawn (row 20 f125)" $?
hascur 28 f10; check "... its new key w moves down (f1 -> f10, by name)" $?
s30=$(after 30)
echo "$s30" | grep -q '^files: folder /tmp/big: 500 entries in'; check "/tmp/big: 500 entries" $?
hascur 29 f99; check "... G: the last, f99" $?
wb=$(echo "$s30" | grep -a -m1 '^files: window'); BX=$(echo "$wb" | cut -d' ' -f3) BY=$(echo "$wb" | cut -d' ' -f4)
px big $((BX + 250)),$((BY + 12)),47B > "$OUT/q4"; rc=$?; check "... drawn, the cursor on the first row ($(cat "$OUT/q4"))" $rc
px bigend $((BX + 250)),$((BY + 12)),EEE > "$OUT/q5"; rc=$?; check "... G: the first row plain ($(cat "$OUT/q5"))" $rc
echo "the window: $w"
[ "$(echo "$w" | cut -d' ' -f3-4)" = "$IX $IY" ]; check "files' window where zdesk puts the first one ($IX,$IY)" $?
echo "emulator figures (FS-UAE, stock A1200 + 4 MB fast; not a real machine's): $(echo "$s30" | grep '^files: folder /tmp/big')"
grep -a -A3 'time ls /tmp/big' "$L" | grep -a 'real' | head -1 | sed 's/^/  ls \/tmp\/big > \/dev\/null: /'
echo "$s30" | grep -a 'files' | grep -v '^files:' | sed 's/^/  /'
exit $((1 - pass))
