#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The desktop on a graphics card's screen (DESIGN D43, docs/d43-rtg.md 10) in FS-UAE through the cage rig, on an A1200
# (68EC020) with 8 MB of fast RAM. No Picasso96 runs in the tests, so `dpytest -fb` makes a pretend card in fast RAM
# and writes its record into the boot's hardware table as `zenixboot RTG` does; the display server it starts takes
# the card exactly as it would take zenixboot's. The card's pixels are read back (`dpytest peek`) and compared:
#  - the hello: the card's size, 16 pens (depth 4); displayd's log names the card
#  - every pixel shown (none left as the card's fill), the desktop's colour, two windows, B over A, A's title
#  - moved, raised, hidden and shown, scrolled, closed: as dpy-test.sh checks on the chipset, on the card
#  - the pointer drawn at the middle, and after the mouse moved (CON_INPUT) at its new place, the old place clean
#  - DPY_MODE answers DPYM_RTG and changes nothing
#  - the chipset's screen one colour while the card has the desktop (a screenshot); text again at the end
#  - the record cleared when its server ended: the next desktop is the chipset's
# Two setups: 32 (800 x 600, A8R8G8B8) and 16 (1024 x 768, R5G6B5).
# usage: tools/rtg-test.sh [32] [16]     (ZENIX_SERIAL, ZENIX_BUILD, ZENIX_OUT as the other tests)
cd "$(dirname "$0")/.." || exit 1
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-rtg-serial}
OUT=${ZENIX_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/zenix-rtg.XXXXXX")}; mkdir -p "$OUT"
B=$OUT/build
trap 'rm -rf "$B"' EXIT
pass=1
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
make -s -j8 BUILD="$B" DIWV=0x38 "$B/zenix.adf" "$B/zenix.hdf" >"$OUT/build.log" 2>&1 ||
    { echo "FAIL build:"; grep -v 'RWX permissions' "$OUT/build.log" | tail -15; exit 1; }

# the card's bytes for a 12-bit colour: 4 bytes A8R8G8B8, 2 bytes R5G6B5 (as dpytest peek prints them)
hexof() {
    python3 -c 'import sys
v, b = int(sys.argv[1], 16), int(sys.argv[2]); r, g, bl = ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17
print("00%02X%02X%02X" % (r, g, bl) if b == 4 else "%04X" % ((r >> 3) << 11 | (g >> 2) << 5 | bl >> 3))' "$1" "$2"
}

run() {         # run NAME W H BYTES
    local n=$1 w=$2 h=$3 b=$4 L
    cat >"$OUT/$n-script.txt" <<EOS
wait zenix:/# 120
sleep 1
send dpytest -fb ${w}x${h}x$b\r
wait dpytest: open 60
sleep 2
send peek $((w - 1)) $((h - 1))\r
send peek 0 0\r
send win A 200 80 4\r
sleep 2
send win B 160 64 9\r
sleep 2
send info A\r
send info B\r
send peek 120 72\r
send peek 210 60\r
send peek 400 200\r
send peek 100 20\r
send peek $((w / 2)) $((h / 2))\r
send peek $((w / 2 + 1)) $((h / 2 + 2))\r
send mode\r
send mode lace\r
send mem\r
sleep 2
shot native
send mouse 10 5\r
sleep 2
send peek $((w / 2)) $((h / 2))\r
send peek $(((340 * w / 640))) $(((133 * h / 256)))\r
send peek $(((340 * w / 640) + 1)) $(((133 * h / 256) + 2))\r
send win C 100 40 5 900 700\r
sleep 2
send info C\r
send close C\r
sleep 1
send wm\r
send move A 100 100\r
sleep 2
send peek 210 60\r
send peek 30 20\r
send peek 250 150\r
send peek 120 72\r
send peek 150 102\r
send raise A\r
sleep 2
send peek 150 102\r
send peek 250 150\r
send lower A\r
sleep 1
send raise A\r
sleep 1
send hide A\r
sleep 1
send show A\r
sleep 2
send peek 150 102\r
send peek 250 150\r
send peek 120 72\r
send scroll A 16\r
sleep 2
send peek 250 185\r
send peek 250 150\r
send close B\r
sleep 2
send peek 120 72\r
send peek 250 150\r
send events\r
wait dpytest: events end 30
send quit\r
wait dpytest: quit 30
sleep 4
shot text
send dpytest\r
wait dpytest: open 60
sleep 2
send quit\r
wait dpytest: quit 30
sleep 4
send echo rtg-end-\$((40+2))\r
wait rtg-end-42 30
EOS
    ZENIX_FSUAE_SET="fast_memory=8192" ZENIX_BUILD="$B" python3 tools/fsuae_conf.py fsuae/a1200-020-fast.fs-uae \
        "$OUT/$n.fs-uae" || exit 1
    ZOOMOUT=0 SHOT_LIMIT=400 timeout -k 20 700 python3 tools/shot.py "$OUT/$n.fs-uae" "$OUT/$n-script.txt" "$OUT/$n" \
        >/dev/null 2>&1
    L=$OUT/$n-serial.txt; tr -d '\r' < "$OUT/$n.serial.log" > "$L"
    pk() {      # pk X Y RGB12 [the Nth such peek, default the last]: the card's pixel at X, Y has that colour
        local got want
        want=$(hexof "$3" "$b")
        got=$(grep "dpytest: peek $1 $2 " "$L" | sed -n "${4:-\$}p" | awk '{ print $5 }')
        [ "$got" = "$want" ] || { echo "  peek $1 $2: $got, wanted $want ($3)"; return 1; }
    }
    pks() {     # pks N X,Y,RGB ...: each peek, the Nth of its place
        local k=$1 ok=0; shift
        for a in "$@"; do IFS=, read -r x y c <<<"$a"; pk "$x" "$y" "$c" "$k" || ok=1; done
        return $ok
    }
    grep -q "dpytest: card ${w}x${h}x$b at " "$L" && grep -q "dpytest: open 0 screen ${w}x${h} depth 4" "$L"
    check "$n: dpytest's pretend card, and the hello: ${w} x ${h}, 16 pens" $?
    grep -q "displayd: RTG screen ${w}x${h}, format [0-9]*, $b bytes a pixel, at 0x[0-9a-f]*, $((w * b)) bytes a row (a test's pretend card)" "$L"
    check "$n: displayd takes the card from the hardware table" $?
    pk $((w - 1)) $((h - 1)) 344 && pk 0 0 344
    check "$n: every pixel shown: the desktop's colour in the corners (none left as the card's fill)" $?
    grep -q "dpytest: info A 23 16 200 80 24 28 1" "$L" && grep -q "dpytest: info B 39 28 160 64 40 40 1" "$L"
    check "$n: no window manager: the windows placed one below the other, as on the chipset" $?
    pks 1 120,72,D54 210,60,47B 400,200,344 100,20,BBC
    check "$n: two windows: B over A, A's uncovered part, the desktop, A's title (the pens' colours on the card)" $?
    pk $((w / 2)) $((h / 2)) 000 1 && pk $((w / 2 + 1)) $((h / 2 + 2)) FFF
    check "$n: the pointer's arrow at the middle (its outline black, its face white)" $?
    [ "$(grep -c "dpytest: mode 2 screen ${w}x${h}" "$L")" = 2 ]
    check "$n: DPY_MODE answers DPYM_RTG (2) and changes nothing" $?
    pk $((w / 2)) $((h / 2)) 344 2 && pk $((340 * w / 640)) $((133 * h / 256)) 000 && \
        pk $((340 * w / 640 + 1)) $((133 * h / 256 + 2)) FFF
    check "$n: the mouse moved 10, 5 counts: the pointer at its new place (scaled), the old place the desktop again" $?
    grep -o "dpytest: info C .*" "$L" | head -1 | awk -v w="$w" -v h="$h" '{ exit !($4 >= 0 && $5 >= 0 &&
        $8 + $6 <= w && $9 + $7 <= h && $6 == 100 && $7 == 40) }'
    check "$n: a window asking for 900,700 opens wholly on the screen ($(grep -o 'dpytest: info C .*' "$L" | head -1))" $?
    pks 2 210,60,344 && pks 1 30,20,344 250,150,47B && pks 2 120,72,D54 && pks 1 150,102,D54
    check "$n: A moved: its old place is the desktop, it is under B where they overlap" $?
    pks 2 150,102,BBC && pks 2 250,150,47B
    check "$n: A raised: in front of B" $?
    pks 3 150,102,BBC 250,150,47B 120,72,D54
    check "$n: lowered, raised, hidden, shown: A as it was" $?
    pks 1 250,185,EEE && pks 4 250,150,47B
    check "$n: scrolled 16 lines up (O_COPY), the freed lines filled" $?
    pks 4 120,72,344 && pks 5 250,150,47B
    check "$n: B closed: the desktop where it was" $?
    [ "$(grep -c '^ev 5 A ' "$L")" = 1 ] && [ "$(grep -c '^ev 5 B ' "$L")" = 1 ]
    check "$n: smart refresh on the card: each program asked to draw once ($(grep -c '^ev 5 ' "$L") EV_EXPOSE)" $?
    grep -q "display: console 1 given back" "$L"; check "$n: the last client gone: displayd ends, the console given back" $?
    grep -q "dpytest: open 0 screen 640x[0-9]* depth [34]" "$L"
    check "$n: the pretend card's record cleared with its server: the next desktop is the chipset's" $?
    for s in native text; do [ -f "$OUT/$n-$s.png" ] || { echo "FAIL $n: no screenshot $s"; pass=0; return; }; done
    python3 - "$OUT/$n-native.png" "$OUT/$n-text.png" <<'EOF'
import sys
from PIL import Image
def inner(im):           # the emulator's picture (the screenshot's black around it left out), inside its edges
    x0, y0, x1, y1 = im.getbbox()
    return im.crop((x0 + 16, y0 + 16, x1 - 16, y1 - 16))
a = Image.open(sys.argv[1]).convert("RGB")
box = inner(a)
cols = box.getcolors(1 << 20)
top = max(cols)[1]
ok = all(max(abs(c[i] - top[i]) for i in range(3)) <= 40 for n, c in cols) and abs(top[0] - 0x33) <= 40
t = inner(Image.open(sys.argv[2]).convert("RGB")).getcolors(1 << 20)
print("  the chipset's screen during the RTG desktop: %d colours, the main one %s; at the end %d colours" %
      (len(cols), top, len(t)))
sys.exit(0 if ok and len(t) > 1 else 1)
EOF
    check "$n: the chipset's screen one colour (the desktop's 344) while the card has the desktop; text again at the end" $?
    grep "dpytest: mem\|displayd: RTG" "$L" | head -3
}

for s in ${*:-32 16}; do
    case $s in
    32) run 32 800 600 4 ;;
    16) run 16 1024 768 2 ;;
    *) echo "rtg-test: unknown setup $s"; exit 2 ;;
    esac
done
exit $((1 - pass))
