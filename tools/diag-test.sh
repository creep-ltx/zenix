#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# First-boot diagnostics (audit A11/A12), in FS-UAE through the cage rig (tools/shot.py: nothing on the desktop).
#  1. colours: test builds that hang at boot stage N (make DIAG=-DDIAG_STOP=N) must show stage N's colour over the
#     whole screen; a build that panics before the display (DIAG_PANIC=7) and a 68000 must show the split screen
#     (stage colour on top, red below); a panic after the boot (DIAG_PANIC=11) white text on dark red (no bar since
#     audit batch E: the whole screen is text).
#  2. previous-boot log: boot the RAM floppy, run `crash` (the kernel logs "killed"), warm reset with
#     Ctrl+Amiga+Amiga, and check that the next boot shows the previous boot's last lines and `dmesg -p` has them -
#     the last one is the reset warning's "filesystems synced" (FS-UAE's keyboard sends the warning: kbd.c);
#     the same after a kernel panic (DIAG_PANIC=11): "ended here with a kernel panic" and the panic text, twice
#     (the two slots alternate).
#  3. the stack guard's kernel-stack checks, in a STACK_TEST build: `crash kstack` damages the guard of its own kernel
#     stack (killed with "overflowed its kernel stack", the shell goes on), `crash idle` damages pid 0's (a kernel
#     panic); after a warm reset the next boot shows that panic as the previous boot's end.
# usage: tools/diag-test.sh [colours|reset|stack|all]     (ZENIX_SERIAL defaults to /tmp/zenix-diag-serial)
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

echo "sleep 1" > "$OUT/idle.txt"

# shot <shot.py arguments>: one emulator run through the cage rig. shot.py ends its cage and FS-UAE itself (its own
# time limit counts from the emulator's start, not from the wait for the machine's emulator lock; a signal ends them
# too); anything of this run's still alive afterwards (its command line names $OUT) is ended here, also when this
# script ends or is stopped, so no cage or FS-UAE outlives its part and keeps the next test (or session) waiting.
sweep() {
    pkill -TERM -f -- "fs-uae $OUT/" 2>/dev/null; pkill -TERM -f -- "cage -- .* $OUT/" 2>/dev/null
    for _ in 1 2 3 4 5; do pgrep -f -- "fs-uae $OUT/" >/dev/null || break; sleep 1; done
    pkill -KILL -f -- "fs-uae $OUT/" 2>/dev/null; pkill -KILL -f -- "cage -- .* $OUT/" 2>/dev/null
    return 0
}
trap 'sweep' EXIT
trap 'exit 130' INT TERM HUP
shot() { timeout -k 20 1800 python3 tools/shot.py "$@" >/dev/null 2>&1; sweep; }

if [ "$WHAT" = colours ] || [ "$WHAT" = all ]; then
    # stage: colour (kernel/diag.h, boot/bootblock.S, kernel/start.S)
    for sc in 1:00F 2:0FF 3:F0F 4:FF0 5:F80 6:0F0 7:630 8:F9C 9:FFF; do
        n=${sc%%:*}; col=${sc#*:}
        build stop$n "-DDIAG_STOP=$n"
        conf "$OUT/stop$n.fs-uae" "$OUT/stop$n/zenix.adf"
        shot "$OUT/stop$n.fs-uae" "$OUT/idle.txt" "$OUT/stop$n" 18
        px=$(pixel "$OUT/stop$n-18.png" 0.5)
        near "$px" "$col" && check "stage $n stops with colour $col over the screen" ok ||
            check "stage $n stops with colour $col over the screen (got $px, see $OUT/stop$n-18.png)" bad
    done

    build panic7 "-DDIAG_PANIC=7"
    conf "$OUT/panic7.fs-uae" "$OUT/panic7/zenix.adf"
    shot "$OUT/panic7.fs-uae" "$OUT/idle.txt" "$OUT/panic7" 18
    top=$(pixel "$OUT/panic7-18.png" 0.15); bot=$(pixel "$OUT/panic7-18.png" 0.85)
    near "$top" 630 && near "$bot" F00 && check "panic before the display: brown (stage 7) over red" ok ||
        check "panic before the display: brown over red (got $top / $bot, see $OUT/panic7-18.png)" bad
    grep -q 'kernel panic: test panic' "$OUT/panic7.serial.log" && check "the panic text reaches the serial port" ok ||
        check "the panic text reaches the serial port" bad

    # a 68000 is refused at stage 5: orange over red (the standard floppy of the stop builds will do)
    conf "$OUT/a600.fs-uae" "$OUT/stop9/zenix.adf" A600
    sed -i 's/^fast_memory = .*/fast_memory = 0/; s/^chip_memory = .*/chip_memory = 1024/' "$OUT/a600.fs-uae"
    shot "$OUT/a600.fs-uae" "$OUT/idle.txt" "$OUT/a600" 20
    top=$(pixel "$OUT/a600-20.png" 0.15); bot=$(pixel "$OUT/a600-20.png" 0.85)
    near "$top" F80 && near "$bot" F00 && check "68000 refused: orange (stage 5) over red" ok ||
        check "68000 refused: orange over red (got $top / $bot, see $OUT/a600-20.png)" bad
fi

if [ "$WHAT" = reset ] || [ "$WHAT" = all ]; then
    build normal ""
    conf "$OUT/reset.fs-uae" "$OUT/normal/zenix-ram.adf"
    RESET_AT=38 shot "$OUT/reset.fs-uae" tools/tests/diag-reset-test.txt "$OUT/reset" 36 66 78
    L=$OUT/reset.log; tr -d '\r' < "$OUT/reset.serial.log" > "$L"
    grep -q '^klog: no log from a previous boot' "$L" && check "cold start: no previous log" ok || check "cold start: no previous log" bad
    grep -q '^dmesg: no log from a previous boot' "$L" && check "dmesg -p says so on a cold start" ok || check "dmesg -p says so on a cold start" bad
    [ "$(grep -c '^Zenix - 68020+' "$L")" = 2 ] && check "the warm reset booted Zenix again" ok || check "the warm reset booted Zenix again" bad
    grep -q '^klog: boot 2, log kept at' "$L" && check "second boot counts 2" ok || check "second boot counts 2" bad
    grep -q '^=== previous boot (1) ended here without a panic' "$L" && check "previous boot shown, no panic recorded" ok || check "previous boot shown, no panic recorded" bad
    grep -q '^> \[kernel\] pid [0-9]* (crash) killed' "$L" && check "its last lines include the crash" ok || check "its last lines include the crash" bad
    # FS-UAE's keyboard sends the reset warning ($78 twice, HRM p.361): init synced the disks and said so last
    grep -q '^> kbd: reset warning: filesystems synced in [0-9]* ms' "$L" &&
        check "reset warning: the reset waited while init synced the disks (in the previous boot's lines)" ok ||
        check "reset warning: the reset waited while init synced the disks (in the previous boot's lines)" bad
    [ "$(grep -c '^kbd: reset warning: filesystems synced in [0-9]* ms' "$L")" = 2 ] && check "dmesg -p | tail prints the previous boot's last line" ok ||
        check "dmesg -p | tail prints the previous boot's last line" bad

    build panic11 "-DDIAG_PANIC=11" DIWV=0x38      # the picture starts lower: FS-UAE's screenshot shows its top rows
    conf "$OUT/preset.fs-uae" "$OUT/panic11/zenix-ram.adf"
    ZOOMOUT=0 RESET_AT=28,58 shot "$OUT/preset.fs-uae" "$OUT/idle.txt" "$OUT/preset" 26 56 86
    L=$OUT/preset.log; tr -d '\r' < "$OUT/preset.serial.log" > "$L"
    bg=$(pixel "$OUT/preset-26.png" 0.02)
    near "$bg" 800 && check "panic after the boot: the background turns dark red" ok || check "panic after the boot: dark red background (got $bg, see $OUT/preset-26.png)" bad
    grep -q '^=== previous boot (1) ended here with a kernel panic' "$L" && check "next boot: ended with a kernel panic" ok || check "next boot: ended with a kernel panic" bad
    grep -q '^> \*\*\* kernel panic: test panic (DIAG_PANIC)' "$L" && check "next boot shows the panic text" ok || check "next boot shows the panic text" bad
    # a third boot: the log is back in the first slot and boot 2's is the previous one
    grep -q '^klog: boot 3, log kept at' "$L" && grep -q '^=== previous boot (2) ended here with a kernel panic' "$L" &&
        check "third boot: boot 2's log (the other slot) is the previous one" ok ||
        check "third boot: boot 2's log (the other slot) is the previous one" bad
fi
if [ "$WHAT" = stack ] || [ "$WHAT" = all ]; then
    build stack "-DSTACK_TEST"
    conf "$OUT/stack.fs-uae" "$OUT/stack/zenix-ram.adf"
    RESET_AT=56 shot "$OUT/stack.fs-uae" tools/tests/diag-stack-test.txt "$OUT/stack" 50 95
    L=$OUT/stack.log; tr -d '\r' < "$OUT/stack.serial.log" > "$L"
    grep -q '^\[kernel\] pid [0-9]* (crash) overflowed its kernel stack: killed' "$L" &&
        check "a damaged kernel-stack guard: the process is killed with the message" ok ||
        check "a damaged kernel-stack guard: the process is killed with the message" bad
    grep -q '^after-kstack$' "$L" && check "the system goes on after it" ok || check "the system goes on after it" bad
    grep -q '^\*\*\* kernel panic: pid 0 (idle) overflowed its kernel stack' "$L" &&
        check "a damaged guard of pid 0's stack: a kernel panic with the message" ok ||
        check "a damaged guard of pid 0's stack: a kernel panic with the message" bad
    grep -q '^> \*\*\* kernel panic: pid 0 (idle) overflowed its kernel stack' "$L" &&
        check "the next boot shows that panic in the previous boot's lines" ok ||
        check "the next boot shows that panic in the previous boot's lines" bad
fi
echo "logs and screenshots: $OUT"
exit $((1 - pass))
