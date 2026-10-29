#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The colour console (DESIGN D14) in FS-UAE through the cage rig (tools/shot.py: nothing on the desktop), at 1, 2, 3
# and 4 bitplanes (test builds with make GFXDEPTH=n DIWV=0x38, on the A1200 with fast RAM; 2 to 4 planes there at
# AGA's 64-bit fetch, FMODE 3). For each depth, screenshots taken when the script gets there (tools/colour-test.py
# checks them):
#  - `termtest columns`: all 80 columns visible, in order, nothing repeated, and the picture's edges the very pixels
#    one plane has (at 4 planes this is the test of FMODE 3's fetch window)
#  - `termtest colours`: the 16 colour blocks against the palette (3 planes: colour & 7; 2: the four colours; 1 plane:
#    no colours)
#  - D24 6.2: `termtest colours`'s g writes the display through CON_LINE (pens and palette): every pixel against the
#    colours screenshot (those inside one colour: the scaled screenshot blurs the edges)
#  - `termtest scroll` (coloured lines scrolling the screen three times, so the blitter copies the rows back,
#    background-colour erases, a scroll region with inserted and deleted lines): the screen afterwards must be the
#    very picture a redraw from the grid gives (the scrollback one line up and back down redraws) - no garbage
#  - scrolled back a page: the rows keep their colours (3 and 4 planes)
#  - edit's status line: white on blue (3 and 4 planes; 2: on cyan), inverse (1 plane: the colours are written as an
#    inverse)
#  - on the serial port, raw: the prompt's colour codes; less showing a line with colour codes and 79 visible
#    characters as one row, the codes passed on (zero width, as GNU less -R)
#  - DESIGN D20 step 1, a console's font and rules: `termtest pcfont` (its own test font, whose glyphs name
#    themselves): all 256 bytes drawn in the loaded font, CLRHOME, WRAPNOW, LFONLY, the CGA palette, the bright
#    foreground and the iCE background as the planes show them; console 2 meanwhile in Zenix's palette (termtest
#    colours there); back on console 1 the page as before; at 3 planes root's switch to 16 colours while the page is
#    shown (the font's table built, the page as 4 planes show it); `x` (termtest ends without cleaning up): the kernel
#    drops the font and the rules (Zenix's palette, Topaz); `q`: the screen cleared before the font goes; at 4 planes
#    the real CP437 font from the disk, every glyph against its bitmap
# The setting (D14 section 11, /etc/console read by init), on the A1200 without fast RAM (8 colours by default), with the
# default depth's build (DIWV=0x38 only): "colours 4" switches to 2 planes and "colours 16" to 4 (the kernel says so,
# and the colour page and a scrolled-back page are what those planes show: the switch draws the console again), and
# "colours 7" is refused with a message, the 3 planes staying.
# usage: tools/colour-test.sh [DEPTH...] [s4 s16 s7]   (default 1 2 3 4 s4 s16 s7; the 1-plane run comes first: its
#        edges are the reference)
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-colour-serial}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-colour.XXXXXX)}; mkdir -p "$OUT"
ARGS=${*:-1 2 3 4 s4 s16 s7}
DEPTHS=$(for a in $ARGS; do case $a in s*) ;; *) echo $a;; esac; done)
SETS=$(for a in $ARGS; do case $a in s*) echo ${a#s};; esac; done)
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
ct() { python3 tools/colour-test.py "$@"; }
# the test builds (about 25 MB each) go when the script ends; the logs, configs and screenshots stay
trap 'for d in $DEPTHS; do rm -rf "$OUT/d$d"; done; rm -rf "$OUT/ds"' EXIT

cat > "$OUT/script.txt" <<'EOF'
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send termtest colours\r
sleep 5
shot colours
send g
sleep 8
send q
sleep 2
send sync\r
sleep 2
send termtest scroll\r
wait zenix:/# 120
sleep 3
shot scrolled
send \x1du
sleep 2
send \x1dd
sleep 3
shot redrawn
send \x1dU
sleep 3
shot back
send \x1dD
sleep 1
send edit\r
sleep 4
shot edit
send \x11
sleep 2
send printf '\\033[31mxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\\033[0myyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy\\nSECOND\\n' > /tmp/l.txt; seq 1 40 >> /tmp/l.txt; less /tmp/l.txt\r
sleep 4
send q
sleep 2
EOF

pcscript() {   # the D20 part of a depth's script; at 3 planes root switches to 16 colours while the page is shown
    cat "$OUT/script.txt"
    printf '%s\n' 'send termtest pcfont\r' 'sleep 5' 'shot pcfont' 'send \x1d2' 'wait zenix login: 30' 'send root\r' 'sleep 4' \
        'send termtest colours\r' 'sleep 5' 'shot pccon2' 'send q' 'sleep 2' 'send \x1d1' 'sleep 3' 'shot pcback'
    [ "$1" = 3 ] && printf '%s\n' 'send 6' 'sleep 4' 'shot pc16'
    printf '%s\n' 'send x' 'sleep 4' 'shot pcgone' 'send termtest pcfont\r' 'sleep 5' 'send q' 'sleep 4' 'shot pcquit'
    [ "$1" = 4 ] && printf '%s\n' 'send termtest pcfont /usr/share/consolefonts/cp437-8x8.psf\r' 'sleep 5' 'shot pccp437' \
        'send q' 'sleep 3'
}
for d in $DEPTHS; do
    B=$OUT/d$d
    pcscript $d > "$OUT/script$d.txt"
    make -s -j8 BUILD="$B" GFXDEPTH=$d DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/d$d.build.log" 2>&1 ||
        { echo "FAIL build of the $d-plane kernel:"; grep -v 'RWX permissions' "$OUT/d$d.build.log" | tail -15; pass=0; continue; }
    ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae "$OUT/d$d.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=480 timeout -k 20 900 python3 tools/shot.py "$OUT/d$d.fs-uae" "$OUT/script$d.txt" "$OUT/d$d" >/dev/null 2>&1
    want="$d bitplane"; [ $d = 1 ] || want="$d bitplanes"
    tr -d '\r' < "$OUT/d$d.serial.log" | grep -q "display: 640x256, $want, 80x32 text"
    check "$d planes: the kernel says so on the log" $?
    for s in columns colours scrolled redrawn back edit; do
        [ -f "$OUT/d$d-$s.png" ] || { echo "FAIL $d planes: no screenshot $s (see $OUT/d$d.serial.log)"; pass=0; continue 2; }
    done
    ref=""; [ $d = 1 ] || [ ! -f "$OUT/edges1" ] || ref="$OUT/edges1"
    ct columns "$OUT/d$d-columns.png" $ref > "$OUT/geom$d"; r=$?
    check "$d planes: termtest columns: all 80 columns in order${ref:+, the edges exactly where one plane has them} ($(head -1 "$OUT/geom$d"))" $r
    [ $d = 1 ] && head -1 "$OUT/geom$d" > "$OUT/edges1"
    ct palette "$OUT/d$d-colours.png" "$OUT/geom$d" $d; check "$d planes: termtest colours: the 16 blocks as $d planes show them" $?
    debugfs -R "cat /tmp/grab.zg" "$B/zenix.hdf" > "$OUT/d$d-grab.zg" 2>/dev/null
    ct grab "$OUT/d$d-colours.png" "$OUT/geom$d" "$OUT/d$d-grab.zg"
    check "$d planes: CON_LINE's picture (termtest's g) is the screenshot of the same moment, pixel by pixel (D24 6.2)" $?
    ct same "$OUT/d$d-scrolled.png" "$OUT/d$d-redrawn.png"; check "$d planes: after termtest scroll the screen is what a redraw from the grid gives" $?
    if [ $d = 2 ]; then
        ct rows "$OUT/d$d-back.png" "$OUT/geom$d" $d; check "$d planes: scrolled back a page, every row keeps its colour" $?
        ct pixel "$OUT/d$d-edit.png" "$OUT/geom$d" 31 20 2BB; check "$d planes: edit's status line is cyan (blue's stand-in)" $?
    elif [ $d != 1 ]; then
        ct rows "$OUT/d$d-back.png" "$OUT/geom$d" $d; check "$d planes: scrolled back a page, every row keeps its colour" $?
        ct pixel "$OUT/d$d-edit.png" "$OUT/geom$d" 31 20 45E; check "$d planes: edit's status line is blue" $?
    else
        ct pixel "$OUT/d$d-edit.png" "$OUT/geom$d" 31 20 BBB; check "$d planes: edit's status line is inverse" $?
    fi
    ct serial "$OUT/d$d.serial.log"; check "$d planes: the prompt's colour codes and less's zero-width codes on the serial port" $?
    for s in pcfont pccon2 pcback pcgone pcquit; do
        [ -f "$OUT/d$d-$s.png" ] || { echo "FAIL $d planes: no screenshot $s (see $OUT/d$d.serial.log)"; pass=0; continue 2; }
    done
    ct pcfont "$OUT/d$d-pcfont.png" "$OUT/geom$d" $d
    check "$d planes: termtest pcfont: the loaded font's 256 glyphs and the BBS rules (D20)" $?
    ct palette "$OUT/d$d-pccon2.png" "$OUT/geom$d" $d; check "$d planes: console 2 meanwhile: Zenix's palette, Topaz" $?
    ct pcfont "$OUT/d$d-pcback.png" "$OUT/geom$d" $d; check "$d planes: back on console 1: its font and palette again" $?
    gd=$d
    if [ $d = 3 ]; then
        gd=4
        [ -f "$OUT/d$d-pc16.png" ] && ct pcfont "$OUT/d$d-pc16.png" "$OUT/geom$d" 4
        check "3 planes: root switches to 16 colours under the page: the font and the rules as 4 planes show them" $?
    fi
    ct pcgone "$OUT/d$d-pcgone.png" "$OUT/geom$d" $gd; check "$d planes: termtest ended without cleaning up: the kernel dropped its font and rules" $?
    ct cleared "$OUT/d$d-pcquit.png" "$OUT/geom$d"; check "$d planes: termtest's q: the screen cleared, then the font dropped" $?
    if [ $d = 4 ]; then
        [ -f "$OUT/d$d-pccp437.png" ] && ct pcfont "$OUT/d$d-pccp437.png" "$OUT/geom$d" 4 "$B/cp437-8x8.psf"
        check "4 planes: the CP437 font from the disk (/usr/share/consolefonts), every glyph against its bitmap" $?
    fi
done
# ---- the setting: /etc/console on the A1200 without fast RAM ----
cat > "$OUT/sscript.txt" <<'EOF'
wait zenix:/# 120
sleep 1
send termtest columns\r
sleep 5
shot columns
send q
sleep 2
send termtest colours\r
sleep 5
shot colours
send q
sleep 2
send termtest scroll\r
wait zenix:/# 120
sleep 3
send \x1dU
sleep 3
shot back
send \x1dD
sleep 1
send free\r
sleep 3
EOF
if [ -n "$SETS" ]; then
    B=$OUT/ds
    make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/ds.build.log" 2>&1 ||
        { echo "FAIL build of the default-depth kernel:"; grep -v 'RWX permissions' "$OUT/ds.build.log" | tail -15; pass=0; SETS=""; }
    cp "$B/zenix.hdf" "$OUT/ds-clean.hdf"
fi
for c in $SETS; do
    cp "$OUT/ds-clean.hdf" "$B/zenix.hdf"
    printf '# a test of the setting\ncolours %s\n' $c > "$OUT/console$c"
    debugfs -w -R "rm /etc/console" "$B/zenix.hdf" >/dev/null 2>&1
    debugfs -w -R "write $OUT/console$c /etc/console" "$B/zenix.hdf" >/dev/null 2>&1
    ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020.fs-uae "$OUT/s$c.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=400 timeout -k 20 900 python3 tools/shot.py "$OUT/s$c.fs-uae" "$OUT/sscript.txt" "$OUT/s$c" >/dev/null 2>&1
    log=$(tr -d '\r' < "$OUT/s$c.serial.log")
    echo "$log" | grep -q "display: 640x256, 3 bitplanes, 80x32 text"
    check "colours $c: without fast RAM the kernel starts with 3 planes (8 colours)" $?
    case $c in 4) d=2;; 16) d=4;; *) d=3;; esac
    if [ $d = 3 ]; then
        echo "$log" | grep -q "init: /etc/console: colours 7: 2, 4, 8 or 16 (or auto)" && ! echo "$log" | grep -q "display: AGA chips, [124] bitplane"
        check "colours $c: init says the value is wrong, the 3 planes stay" $?
        continue
    fi
    echo "$log" | grep -q "display: AGA chips, $d bitplanes at 64-bit fetch (FMODE 3)"
    check "colours $c: init switches the display to $d planes (the kernel says so)" $?
    for s in columns colours back; do
        [ -f "$OUT/s$c-$s.png" ] || { echo "FAIL colours $c: no screenshot $s (see $OUT/s$c.serial.log)"; pass=0; continue 2; }
    done
    ref=""; [ ! -f "$OUT/edges1" ] || ref="$OUT/edges1"
    ct columns "$OUT/s$c-columns.png" $ref > "$OUT/sgeom$c"; check "colours $c: all 80 columns after the switch ($(head -1 "$OUT/sgeom$c"))" $?
    ct palette "$OUT/s$c-colours.png" "$OUT/sgeom$c" $d; check "colours $c: termtest colours as $d planes show them" $?
    ct rows "$OUT/s$c-back.png" "$OUT/sgeom$c" $d; check "colours $c: scrolled back a page, every row keeps its colour" $?
    echo "$log" | grep -a "chip RAM free" | tail -1 | sed "s/^/  colours $c: /"
done
echo "logs and screenshots: $OUT"
exit $((1 - pass))
