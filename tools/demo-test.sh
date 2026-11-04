#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The demo floppy in FS-UAE (DESIGN D25 revision 3, 10 steps 2-3; make demo), through the cage rig (tools/shot.py):
#   d   the DD disk on the A1200 with 4 MB of fast RAM: the loader's screen with its bar (a screenshot while it loads),
#       the kernel booted from the unpacked RAM disk (ram0, the image's block count), dlogin's autologin of guest into the desktop with the welcome window
#       (a screenshot), and the ADF's SHA-256 unchanged afterwards (nothing is written to the floppy)
#   h   the HD disk in an HD drive (22 sectors a track), the same
#   c   the DD disk on an A1200 with 2 MB of chip RAM only: refused on the screen (red, the reason) and the serial port
#   x   a damaged DD disk (bytes changed in the middle of the packed stream): refused, the reason named
# usage: demo-test.sh [d h c x]   (all by default). Screenshots in ZENIX_OUT.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-/tmp/zenix-demo-test}; mkdir -p "$OUT"
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-demo-serial}
PARTS=${*:-d h c x}
make -s BUILD=$BD $BD/zenix-demo.adf $BD/zenix-demo-hd.adf > "$OUT/build.log" 2>&1 || { echo "FAIL build"; tail "$OUT/build.log"; exit 1; }
grep -h "adf:" "$OUT/build.log" | sed 's/^/  /'
pass=1
ok() { echo "PASS $1"; }
no() { echo "FAIL $1"; pass=0; }
want() { if tr -d '\r' < "$2" | grep -aq -- "$3"; then ok "$1"; else no "$1"; fi; }
# boot NAME CONFIG ADF SCRIPT: a run of its own with a copy of the ADF in a build folder of its own
boot() {
    local n=$1 conf=$2 adf=$3 script=$4 B=$OUT/$1-build
    rm -rf "$B"; mkdir -p "$B"; cp "$adf" "$B/$(basename "$5")"
    ZENIX_BUILD=$B python3 tools/fsuae_conf.py "$conf" "$OUT/$n.fs-uae" || return 1
    ZOOMOUT=0 SHOT_LIMIT=${6:-400} timeout -k 20 900 python3 tools/shot.py "$OUT/$n.fs-uae" "$script" "$OUT/$n" >/dev/null 2>&1
    tr -d '\r' < "$OUT/$n.serial.log" > "$OUT/$n.serial.txt"
}
cat > "$OUT/boot.txt" <<'EOS'
wait demo: the Zenix demo disk's loader 120
sleep 8
shot loading
wait display: console 1 handed to 300
sleep 25
shot desktop
EOS
for kind in d h; do
    case " $PARTS " in *" $kind "*) ;; *) continue;; esac
    if [ $kind = d ]; then adf=$BD/zenix-demo.adf; conf=fsuae/demo/demo.fs-uae; what=DD; else adf=$BD/zenix-demo-hd.adf; conf=fsuae/demo/demo-hd.fs-uae; what=HD; fi
    sum=$(sha256sum < "$adf")
    boot $kind $conf "$adf" "$OUT/boot.txt" "$adf"
    S=$OUT/$kind.serial.txt
    blocks=$(dumpe2fs -h "${adf%.adf}.img" 2>/dev/null | awk -F: '/^Block count/ { print $2 + 0 }')
    want "$what: the loader unpacks the disk and hands over" "$S" "^demo: unpacked "
    want "$what: the kernel boots with the unpacked RAM disk as its root ($blocks blocks)" "$S" "^ext2: 1 KB blocks, $blocks blocks"
    want "$what: init runs, the demo's motd shows" "$S" "Zenix demo disk - everything runs from memory"
    want "$what: dlogin starts the display server on console 1" "$S" "display: console 1 handed to"
    want "$what: guest is logged in on the desktop by itself (dlogin's autologin)" "$S" "^dlogin: guest (uid 1001) logged in: Desktop"
    grep -aq "session ended\|could not start\|not found\|no window" "$S" && no "$what: the session runs (no errors on the serial port)" || ok "$what: the session runs (no errors on the serial port)"
    [ -f "$OUT/$kind-loading.png" ] && [ -f "$OUT/$kind-desktop.png" ] && ok "$what: screenshots of the loading bar and the desktop" || no "$what: screenshots"
    [ "$(sha256sum < "$OUT/$kind-build/$(basename "$adf")")" = "$sum" ] && ok "$what: the ADF is unchanged after the boot" || no "$what: the ADF changed"
done
case " $PARTS " in *" c "*)
    printf 'wait demo: 120\nsleep 10\nshot refused\n' > "$OUT/c.txt"
    boot c fsuae/demo/demo-chip.fs-uae $BD/zenix-demo.adf "$OUT/c.txt" $BD/zenix-demo.adf 200
    want "2 MB chip only: refused, saying it needs 4 MB of fast RAM" "$OUT/c.serial.txt" "^demo: This Zenix demo disk needs 4 MB of fast RAM"
    grep -aq "^Zenix - 68020" "$OUT/c.serial.txt" && no "... and Zenix did not start" || ok "... and Zenix did not start"
    [ -f "$OUT/c-refused.png" ] && ok "2 MB chip only: screenshot of the refusal" || no "2 MB chip only: screenshot";;
esac
case " $PARTS " in *" x "*)
    python3 - "$BD/zenix-demo.adf" "$OUT/bad.adf" <<'PY'
import struct, sys
d = bytearray(open(sys.argv[1], "rb").read())
t = 1024 + struct.unpack(">I", d[8:12])[0]
off, n = struct.unpack(">2I", d[t + 8:t + 16])
for i in range(16): d[off + n // 2 + i] ^= 0x5A
open(sys.argv[2], "wb").write(d)
PY
    printf 'wait demo: The packed 300\nsleep 5\nshot damaged\n' > "$OUT/x.txt"
    boot x fsuae/demo/demo.fs-uae "$OUT/bad.adf" "$OUT/x.txt" $BD/zenix-demo.adf 400
    want "a damaged disk: refused, the reason on the serial port" "$OUT/x.serial.txt" "^demo: The packed system on this disk is damaged: "
    grep -aq "^Zenix - 68020" "$OUT/x.serial.txt" && no "... and Zenix did not start" || ok "... and Zenix did not start";;
esac
rm -rf "$OUT"/*-build
exit $((1 - pass))
