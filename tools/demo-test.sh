#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The demo floppy in FS-UAE (DESIGN D25 revision 3, 10 steps 2-3; make demo), through the cage rig (tools/shot.py):
#   d   the DD disk on the A1200 with 4 MB of fast RAM: the loader's screen with its bar (a screenshot while it loads),
#       the kernel booted from the unpacked RAM disk (ram0, the image's block count), dlogin's autologin of guest into the desktop with the welcome window
#       (a screenshot), and the ADF's SHA-256 unchanged afterwards (nothing is written to the floppy)
#   h   the HD disk in an HD drive (22 sectors a track), the same
#   c   the DD disk on an A1200 with 2 MB of chip RAM only: refused on the screen (red, the reason) and the serial port
#   x   a damaged DD disk (bytes changed in the middle of the packed stream): refused, the reason named
#   y   the bootblock's track check: a DD disk whose bootblock expects HD tracks (offset 1016 = 11264, the checksum
#       made right) in the DD drive: the whole screen yellow (what the HD disk shows in a drive Kickstart runs as DD,
#       a Gotek on the owner's A1200, 2026-11-05)
#   n   the network by itself (D19 6.3, the card probe): the DD disk with an A2065 (FS-UAE's slirp, in a network
#       namespace of its own): netd finds the board with no /etc/network/interfaces and DHCP gives 10.0.2.15; the
#       genet and ne2000 drivers found nothing and were not started again. (d and h have no card: netd looked, said
#       so and stopped.)
#   t   a term window's program starts with the size the window got (the full test image, zdesk from the serial
#       shell): 80 columns and a frame do not fit the screen, zdesk gives 78 or 79, and the program must see that
#       (ctop drew 80 into them, its line ends wrapping into stray digits: "ru40ing", 2026-11-05)
# usage: demo-test.sh [d h c x y t n]   (all by default). Screenshots in ZENIX_OUT.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-/tmp/zenix-demo-test}; mkdir -p "$OUT"
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-demo-serial}
PARTS=${*:-d h c x y t n}
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
    want "$what: no card: netd looked for one by itself, quietly, and stopped again (its memory given back): one line" "$S" "netd: no network card found (no card for genet, a2065, ne2000"
    grep -aq "looking for its card\|started /sbin/net/\|^genet: \|^a2065: \|^ne2000: \|^plipbox: " "$S" && no "$what: the probe's drivers said nothing on the serial port (a quiet console)" || ok "$what: the probe's drivers said nothing on the serial port (a quiet console)"
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
case " $PARTS " in *" y "*)
    python3 - "$BD/zenix-demo.adf" "$OUT/yellow.adf" <<'PY'
import struct, sys
d = bytearray(open(sys.argv[1], "rb").read())
d[1016:1020] = struct.pack(">I", 11264)
d[4:8] = bytes(4)
s = 0
for (v,) in struct.iter_unpack(">I", bytes(d[:1024])):
    s += v
    if s > 0xFFFFFFFF: s = (s & 0xFFFFFFFF) + 1
d[4:8] = struct.pack(">I", 0xFFFFFFFF - s)
open(sys.argv[2], "wb").write(d)
PY
    printf 'sleep 25\nshot tracks\n' > "$OUT/y.txt"
    boot y fsuae/demo/demo.fs-uae "$OUT/yellow.adf" "$OUT/y.txt" $BD/zenix-demo.adf 120
    python3 -c 'import sys; from PIL import Image; i = Image.open(sys.argv[1]).convert("RGB"); w, h = i.size; p = i.getpixel((w // 3, h // 2)); sys.exit(0 if p[0] > 180 and p[1] > 180 and p[2] < 90 else 1)' "$OUT/y-tracks.png" &&
        ok "a disk whose tracks are not the drive's: the bootblock's yellow screen" || no "a disk whose tracks are not the drive's: the bootblock's yellow screen"
    grep -aq "^demo:" "$OUT/y.serial.txt" && no "... and the stage 2 did not run" || ok "... and the stage 2 did not run";;
esac
case " $PARTS " in *" t "*)
    make -s BUILD=$BD $BD/zenix.adf $BD/zenix.hdf >/dev/null 2>&1
    B=$OUT/t-build; rm -rf "$B"; mkdir -p "$B"; cp $BD/zenix.adf $BD/zenix.hdf "$B/"
    cat > "$OUT/t.txt" <<'EOS'
wait zenix:/# 120
sleep 1
send zdesk > /tmp/z.log 2>&1 &\r
sleep 4
send term -e sh -c 'termtest size > /tmp/sz' &\r
sleep 6
send term -c 60 -r 10 -e sh -c 'termtest size > /tmp/sz3' &\r
sleep 6
send cat /tmp/sz /tmp/sz3; echo size-done\r
wait (?m)^size-done 60
EOS
    ZENIX_SERIAL=${ZENIX_SERIAL}-t ZENIX_BUILD=$B tools/run-test.sh fsuae/a1200-020-fast.fs-uae 120 "$OUT/t.txt" 2>&1 | tr -d '\r' > "$OUT/t.log"
    grep -aqE "^termtest size: rows 2[0-9] columns 7[89]$" "$OUT/t.log" && ok "term: an 80-column window the screen shrinks: its program starts with the columns it got" || { no "term: the program's size at its start"; grep -a "termtest size" "$OUT/t.log"; }
    grep -aqx "termtest size: rows 10 columns 60" "$OUT/t.log" && ok "term -c 60 -r 10: the program sees 10 rows, 60 columns" || no "term -c 60 -r 10";;
esac
case " $PARTS " in *" n "*)
    B=$OUT/n-build; rm -rf "$B"; mkdir -p "$B"; cp $BD/zenix-demo.adf "$B/"
    sed '/^\[fs-uae\]/a network_card = a2065' fsuae/demo/demo.fs-uae > "$OUT/n.fs-uae"
    printf 'wait demo: the Zenix demo disk.s loader 120\nwait (?m)netd: eth0: DHCP gave 300\nwait (?m)netd: card probe done 120\nsleep 2\n' > "$OUT/n.txt"
    # a network namespace of its own: FS-UAE's slirp answers DHCP itself and reaches nothing outside
    ZENIX_SERIAL=${ZENIX_SERIAL}-n ZENIX_BUILD=$B unshare -rn tools/run-test.sh "$OUT/n.fs-uae" 400 "$OUT/n.txt" 2>&1 | tr -d '\r' > "$OUT/n.log"
    want "network: ... the A2065's driver found the board: up with DHCP, as the kernel log says" "$OUT/n.log" "netd: eth0: /sbin/net/a2065 found a card: up with DHCP"
    want "network: ... DHCP gave it 10.0.2.15" "$OUT/n.log" "netd: eth0: DHCP gave 10.0.2.15"
    want "network: ... the probe's one line: the A2065 found, nothing for genet and ne2000 (the floppy has no plipbox driver)" "$OUT/n.log" "netd: card probe done: found a2065; no card for genet, ne2000"
    grep -aq "netd stops" "$OUT/n.log" && no "network: netd stays (it serves eth0)" || ok "network: netd stays (it serves eth0)"
    [ "$(grep -ac 'card probe done' "$OUT/n.log")" = 1 ] && ! grep -aq 'started /sbin/net/genet' "$OUT/n.log" && ok "network: one probe, its drivers quiet" || no "network: the probe ran more than once, or a driver it tried was logged";;
esac
rm -rf "$OUT"/*-build
exit $((1 - pass))
