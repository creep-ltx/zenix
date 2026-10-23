#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# First-boot diagnostics (audit A11/A12), in FS-UAE through the cage rig (tools/shot.py: nothing on the desktop).
#  1. colours: test builds that hang at boot stage N (make DIAG=-DDIAG_STOP=N) must show stage N's colour over the
#     whole screen; a build that panics before the display (DIAG_PANIC=7) and a 68000 must show the split screen
#     (stage colour on top, red below); a panic after the boot (DIAG_PANIC=11) a red bar.
#  2. previous-boot log: boot the RAM floppy, run `crash` (the kernel logs "killed"), warm reset with
#     Ctrl+Amiga+Amiga, and check that the next boot shows the previous boot's last lines and `dmesg -p` has them;
#     the same after a kernel panic (DIAG_PANIC=11): "ended here with a kernel panic" and the panic text.
# usage: tools/diag-test.sh [colours|reset|all]     (ZENIX_SERIAL defaults to /tmp/zenix-diag-serial)
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
WHAT=${1:-all}
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-diag-serial}
OUT=$(mktemp -d /tmp/zenix-diag.XXXXXX)
KICK=$(sed -n 's/^kickstart_file = //p' fsuae/a1200-020-fast.fs-uae | tail -n 1)
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }

# build <name> <DIAG flags> [make variables]: a full build of its own in $OUT/<name>
build() {
    make -s -j8 BUILD="$OUT/$1" DIAG="$2" $3 "$OUT/$1/zenix.adf" "$OUT/$1/zenix-ram.adf" >"$OUT/$1.build.log" 2>&1 ||
        { echo "FAIL build $1 ($2):"; grep -v 'RWX permissions' "$OUT/$1.build.log" | tail -15; exit 1; }
}

# conf <file> <adf> [model]: an A1200 (or A600) with 4 MB fast RAM booting that floppy, no hard disk
conf() {
    local model=${3:-A1200}
    cat > "$1" <<EOF
[fs-uae]
accuracy = 1
amiga_model = $model
kickstart_file = $KICK
chip_memory = 2048
fast_memory = 4096
floppy_drive_0 = $2
floppy_drive_speed = 0
serial_port = /tmp/zenix-serial
uae_serial_direct = true
window_width = 960
window_height = 768
zoom = full
joystick_port_0 = none
joystick_port_1 = none
EOF
    if [ "$model" = A600 ]; then
        local k6; k6=$(sed -n 's/^kickstart_file = //p' fsuae/matrix/a600-68000.fs-uae | tail -n 1)
        sed -i "s#^kickstart_file = .*#kickstart_file = $k6#" "$1"
    fi
}

# pixel <png> <y fraction> -> "r g b" in the middle column at that height
pixel() { python3 -c "
from PIL import Image
im = Image.open('$1').convert('RGB'); w, h = im.size
print(*im.getpixel((w // 2, int(h * $2))))"; }
# near <"r g b"> <12-bit colour, e.g. F80>: each channel within 40 of the colour's 8-bit value
near() { python3 -c "
got = [int(x) for x in '$1'.split()]; want = [int(c, 16) * 17 for c in '$2']
exit(0 if all(abs(a - b) <= 40 for a, b in zip(got, want)) else 1)"; }

wait_free() { while pgrep -x fs-uae >/dev/null; do sleep 1; done; }
echo "sleep 1" > "$OUT/idle.txt"

if [ "$WHAT" = colours ] || [ "$WHAT" = all ]; then
    # stage: colour (kernel/diag.h, boot/bootblock.S, kernel/start.S)
    for sc in 1:00F 2:0FF 3:F0F 4:FF0 5:F80 6:0F0 7:630 8:F9C 9:FFF; do
        n=${sc%%:*}; col=${sc#*:}
        build stop$n "-DDIAG_STOP=$n"
        conf "$OUT/stop$n.fs-uae" "$OUT/stop$n/zenix.adf"
        wait_free
        timeout 60 python3 tools/shot.py "$OUT/stop$n.fs-uae" "$OUT/idle.txt" "$OUT/stop$n" 18 >/dev/null 2>&1
        px=$(pixel "$OUT/stop$n-18.png" 0.5)
        near "$px" "$col" && check "stage $n stops with colour $col over the screen" ok ||
            check "stage $n stops with colour $col over the screen (got $px, see $OUT/stop$n-18.png)" bad
    done

    build panic7 "-DDIAG_PANIC=7"
    conf "$OUT/panic7.fs-uae" "$OUT/panic7/zenix.adf"
    wait_free
    timeout 60 python3 tools/shot.py "$OUT/panic7.fs-uae" "$OUT/idle.txt" "$OUT/panic7" 18 >/dev/null 2>&1
    top=$(pixel "$OUT/panic7-18.png" 0.15); bot=$(pixel "$OUT/panic7-18.png" 0.85)
    near "$top" 630 && near "$bot" F00 && check "panic before the display: brown (stage 7) over red" ok ||
        check "panic before the display: brown over red (got $top / $bot, see $OUT/panic7-18.png)" bad
    grep -q 'kernel panic: test panic' "$OUT/panic7.serial.log" && check "the panic text reaches the serial port" ok ||
        check "the panic text reaches the serial port" bad

    # a 68000 is refused at stage 5: orange over red (the standard floppy of the stop builds will do)
    conf "$OUT/a600.fs-uae" "$OUT/stop9/zenix.adf" A600
    sed -i 's/^fast_memory = .*/fast_memory = 0/; s/^chip_memory = .*/chip_memory = 1024/' "$OUT/a600.fs-uae"
    wait_free
    timeout 60 python3 tools/shot.py "$OUT/a600.fs-uae" "$OUT/idle.txt" "$OUT/a600" 20 >/dev/null 2>&1
    top=$(pixel "$OUT/a600-20.png" 0.15); bot=$(pixel "$OUT/a600-20.png" 0.85)
    near "$top" F80 && near "$bot" F00 && check "68000 refused: orange (stage 5) over red" ok ||
        check "68000 refused: orange over red (got $top / $bot, see $OUT/a600-20.png)" bad
fi

if [ "$WHAT" = reset ] || [ "$WHAT" = all ]; then
    build normal ""
    conf "$OUT/reset.fs-uae" "$OUT/normal/zenix-ram.adf"
    wait_free
    RESET_AT=38 timeout 120 python3 tools/shot.py "$OUT/reset.fs-uae" tools/tests/diag-reset-test.txt "$OUT/reset" 36 66 78 >/dev/null 2>&1
    L=$OUT/reset.log; tr -d '\r' < "$OUT/reset.serial.log" > "$L"
    grep -q '^klog: no log from a previous boot' "$L" && check "cold start: no previous log" ok || check "cold start: no previous log" bad
    grep -q '^dmesg: no log from a previous boot' "$L" && check "dmesg -p says so on a cold start" ok || check "dmesg -p says so on a cold start" bad
    [ "$(grep -c '^Zenix - 68020+' "$L")" = 2 ] && check "the warm reset booted Zenix again" ok || check "the warm reset booted Zenix again" bad
    grep -q '^klog: boot 2, log kept at' "$L" && check "second boot counts 2" ok || check "second boot counts 2" bad
    grep -q '^=== previous boot (1) ended here without a panic' "$L" && check "previous boot shown, no panic recorded" ok || check "previous boot shown, no panic recorded" bad
    grep -q '^> \[kernel\] pid [0-9]* (crash) killed' "$L" && check "its last lines include the crash" ok || check "its last lines include the crash" bad
    [ "$(grep -c '^\[kernel\] pid [0-9]* (crash) killed' "$L")" = 2 ] && check "dmesg -p | tail prints the previous boot's last line" ok ||
        check "dmesg -p | tail prints the previous boot's last line" bad

    build panic11 "-DDIAG_PANIC=11" DIWV=0x38      # the picture starts lower: FS-UAE's screenshot shows the bar
    conf "$OUT/preset.fs-uae" "$OUT/panic11/zenix-ram.adf"
    wait_free
    ZOOMOUT=0 RESET_AT=28 timeout 100 python3 tools/shot.py "$OUT/preset.fs-uae" "$OUT/idle.txt" "$OUT/preset" 26 56 >/dev/null 2>&1
    L=$OUT/preset.log; tr -d '\r' < "$OUT/preset.serial.log" > "$L"
    bar=$(pixel "$OUT/preset-26.png" 0.02)
    near "$bar" F00 && check "panic after the boot: the bar turns red" ok || check "panic after the boot: red bar (got $bar, see $OUT/preset-26.png)" bad
    grep -q '^=== previous boot (1) ended here with a kernel panic' "$L" && check "next boot: ended with a kernel panic" ok || check "next boot: ended with a kernel panic" bad
    grep -q '^> \*\*\* kernel panic: test panic (DIAG_PANIC)' "$L" && check "next boot shows the panic text" ok || check "next boot shows the panic text" bad
fi
echo "logs and screenshots: $OUT"
exit $((1 - pass))
