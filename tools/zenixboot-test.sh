#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Booting without a floppy (zenixboot, docs/man/zenixboot.txt) in the emulator. No disk in the floppy drive: Kickstart
# boots the FFS partition of an RDB hard disk, its S:Startup-Sequence runs zenixboot, zenixboot starts the kernel, and
# Zenix mounts its ext2 root from the same disk. zenixboot's messages are copied to the serial port, so the AmigaOS
# side is checked on serial too.
#   a1200-31     A1200, 68EC020, 2 MB chip + 4 MB fast, Kickstart 3.1 (kicka1200.rom): build/zenix-boot.hdf as made
#   a1200-040    the same with a 68040 and 8 MB fast (the MMU and data-cache steps)
#   a1200-32     the same as a1200-31 with a Kickstart 3.2 ROM, if ZENIX_KICK32 names one (else SKIP)
#   a600-205     A600 with Kickstart 2.05 and a 68020, chip RAM only (the kernel at 0x40000), if ZENIX_KICK205 names
#                an A600 2.05 ROM (default $ZENIX_KICKSTART_DIR/kick205-a600.rom; else SKIP)
#   a600-68000   the same with its 68000: zenixboot refuses before doing anything (needs the 2.05 ROM too)
#   card-root    Zenix's side of the SD card image (build/zenix-card.img, which FS-UAE's Kickstart cannot boot: it has
#                no Emu68 driver for MBR 0x76): booted from the floppy, the disk server must list the FAT, the 0x76
#                container, the FFS partition inside it and the ext2 partition, and the root must be the ext2 one
#   bootmenu     A1200 3.1, the boot menu's disk: S:Startup-Sequence runs bootmenu, whose default entry (after 1 s)
#                runs zenixboot; Zenix's `reboot` goes through the menu again, and the second boot must show the first
#                as the previous one (`dmesg -p`): the menu takes the whole klog area, so the free-memory header exec
#                writes after an allocation never lands in a log slot (D15 step 0)
#   errors       A1200 3.1, a Startup-Sequence that runs zenixboot five times: a missing kernel, two bad RAM disks
#                (each an error, back to AmigaOS), CHECK (loads, checks, returns), then RAM=ramdisk (Zenix with its
#                root in RAM)
# usage: tools/zenixboot-test.sh [setup...]     (ZENIX_SERIAL, ZENIX_OUT as the other tests)
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
make -s BUILD=$BD $BD/zenix-boot.hdf $BD/bootmenu $BD/zenix-card.img $BD/zenix.adf $BD/root-ram.img >/dev/null 2>&1 || { echo "FAIL build"; exit 1; }
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-zenixboot.XXXXXX)}; mkdir -p "$OUT"
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-zenixboot-serial}
KDIR=$(python3 -c 'import sys; sys.path.insert(0, "tools"); import fsuae_conf; print(fsuae_conf.kickstart_dir())')
K32=${ZENIX_KICK32:-$KDIR/kick32-a1200.rom}
K205=${ZENIX_KICK205:-$KDIR/kick205-a600.rom}
SETUPS="${*:-a1200-31 a1200-040 a1200-32 a600-205 a600-68000 card-root errors bootmenu}"
B=$(cd "$BD" && pwd)
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
has() { grep -aq -- "$2" "$OUT/$1.log" && echo ok || echo bad; }

# the disk of the "errors" setup: its own boot volume (the kernel, zenixboot, a RAM-disk image) and a longer script
cat >"$OUT/errors-ss" <<'EOF'
FailAt 21
SYS:zenixboot nosuchkernel
SYS:zenixboot RAM=S/Startup-Sequence
SYS:zenixboot RAM=zenixboot
SYS:zenixboot CHECK
SYS:zenixboot RAM=ramdisk
EOF
python3 tools/mkffs.py "$OUT/errors-ffs.img" 16632K ZenixBoot "$OUT/errors-ss=S/Startup-Sequence" \
    "$B/zenixboot=zenixboot" "$B/kernel.bin=zenix" "$B/root-ram.img=ramdisk" >/dev/null || { echo "FAIL mkffs"; exit 1; }
python3 tools/mkrdb-multi.py "$OUT/errors.hdf" "$OUT/errors-ffs.img:ffs:ZB0:boot" >/dev/null || { echo "FAIL mkrdb"; exit 1; }
if command -v xdftool >/dev/null; then       # a second opinion on the FFS writer where amitools is installed (dev only)
    xdftool "$B/zenix-boot.hdf" open part=ZB0 + read zenix "$OUT/zenix.back" >/dev/null 2>&1 && cmp -s "$OUT/zenix.back" "$B/kernel.bin" \
        && check "xdftool reads the kernel back from the FFS boot partition" ok || check "xdftool reads the kernel back from the FFS boot partition" bad
fi

# the disk of the "bootmenu" setup: the menu, its configuration (Zenix first, after 1 s), zenixboot, the kernel, the root
printf 'SYS:bootmenu\n' >"$OUT/bm-ss"
printf 'timeout 1\ndefault 1\nZenix = run SYS:zenixboot\nShell = shell\n' >"$OUT/bm-conf"
python3 tools/mkffs.py "$OUT/bm-ffs.img" 16632K ZenixBoot "$OUT/bm-ss=S/Startup-Sequence" "$OUT/bm-conf=S/BootMenu.conf" \
    "$B/bootmenu=bootmenu" "$B/zenixboot=zenixboot" "$B/kernel.bin=zenix" >/dev/null || { echo "FAIL mkffs (bootmenu)"; exit 1; }
python3 tools/mkcard.py ide "$OUT/bootmenu.hdf" "$OUT/bm-ffs.img" "$B/zenix.hdf" >/dev/null || { echo "FAIL mkcard (bootmenu)"; exit 1; }

FLOPPY=; HDTYPE=
conf() {    # name model rom cpu fast hdf (FLOPPY, HDTYPE: extra lines)
    local cpu=""; [ -n "$4" ] && cpu="uae_cpu_model = $4"
    cat >"$OUT/$1.fs-uae" <<EOF
[fs-uae]
accuracy = 1
amiga_model = $2
kickstart_file = $3
chip_memory = 2048
fast_memory = $5
$cpu
$FLOPPY
floppy_drive_speed = 0
serial_port = /tmp/zenix-serial
uae_serial_direct = true
hard_drive_0 = $6
hard_drive_0_controller = ide0
$HDTYPE
joystick_port_0 = none
joystick_port_1 = none
EOF
}

run() {     # name seconds [keyscript] (run-test.sh waits for a free emulator slot)
    tools/run-test.sh "$OUT/$1.fs-uae" "$2" $3 >"$OUT/$1.log" 2>&1
}

booted() {  # name: the whole chain, from the Startup-Sequence to a shell on the IDE root
    local n=$1
    check "$n: Kickstart ran the Startup-Sequence, zenixboot loaded the kernel" "$(has $n 'zenixboot 0.1b1: loading zenix (')"
    check "$n: the kernel started" "$(has $n 'kernel: [0-9]* bytes at')"
    check "$n: fdisk lists the FFS boot partition and the ext2 root" \
        "$(grep -aq '^ide0p1 *ZB0 .*other boot' "$OUT/$n.log" && grep -aq '^ide0p2 *ZR0 .*ext2 mounted' "$OUT/$n.log" && echo ok || echo bad)"
    check "$n: the root is the ext2 partition (ide0p2)" "$(has $n '^ide0p2 */ *ext2')"
    check "$n: a shell prompt" "$(has $n '^zbok$')"
    check "$n: no panic" "$(grep -aq 'panic' "$OUT/$n.log" && echo bad || echo ok)"
}

for s in $SETUPS; do
    case $s in
    a1200-31)  conf $s A1200 '$ZENIX_KICKSTART_DIR/kicka1200.rom' "" 4096 "$B/zenix-boot.hdf"; run $s 120 tools/tests/zenixboot-test.txt; booted $s ;;
    a1200-040) conf $s A1200 '$ZENIX_KICKSTART_DIR/kicka1200.rom' 68040 8192 "$B/zenix-boot.hdf"; run $s 120 tools/tests/zenixboot-test.txt; booted $s
               check "$s: the CPU is a 68040" "$(has $s '^cpu: 68040')" ;;
    a1200-32)
        if [ ! -f "$K32" ]; then echo "SKIP $s (no Kickstart 3.2 ROM: set ZENIX_KICK32)"; continue; fi
        conf $s A1200 "$K32" "" 4096 "$B/zenix-boot.hdf"; run $s 120 tools/tests/zenixboot-test.txt; booted $s ;;
    a600-205)
        if [ ! -f "$K205" ]; then echo "SKIP $s (no Kickstart 2.05 A600 ROM: set ZENIX_KICK205)"; continue; fi
        conf $s A600 "$K205" 68020 0 "$B/zenix-boot.hdf"; run $s 150 tools/tests/zenixboot-test.txt; booted $s
        check "$s: chip RAM only: the kernel runs at 0x40000" "$(has $s 'kernel: [0-9]* bytes at 0x0*40000')" ;;
    a600-68000)
        if [ ! -f "$K205" ]; then echo "SKIP $s (no Kickstart 2.05 A600 ROM: set ZENIX_KICK205)"; continue; fi
        conf $s A600 "$K205" "" 0 "$B/zenix-boot.hdf"; run $s 40
        check "$s: zenixboot refuses a 68000" "$(has $s 'zenixboot: Zenix needs a 68020 or better')"
        check "$s: and starts nothing" "$(grep -aq 'kernel: ' "$OUT/$s.log" && echo bad || echo ok)" ;;
    card-root)
        # hard_drive_0_type = rdb: FS-UAE shows the file as it is (it starts with an MBR, not an RDB)
        FLOPPY="floppy_drive_0 = $B/zenix.adf" HDTYPE="hard_drive_0_type = rdb" conf $s A1200 '$ZENIX_KICKSTART_DIR/kicka1200.rom' "" 4096 "$B/zenix-card.img"
        run $s 120 tools/tests/zenixboot-test.txt
        check "$s: the FAT, the 0x76 container, the FFS boot partition in it and the ext2 root are listed" \
            "$(grep -aq '^ide0p1 .*other' "$OUT/$s.log" && grep -aq '^ide0p2 .*other' "$OUT/$s.log" && grep -aq '^ide0p3 *ZB0 .*other boot' "$OUT/$s.log" && grep -aq '^ide0p4 .*ext2 mounted' "$OUT/$s.log" && echo ok || echo bad)"
        check "$s: the root is the ext2 partition (ide0p4), not anything inside the RDB" "$(has $s '^ide0p4 */ *ext2')"
        check "$s: a shell prompt" "$(has $s '^zbok$')" ;;
    errors)
        conf $s A1200 '$ZENIX_KICKSTART_DIR/kicka1200.rom' "" 4096 "$OUT/errors.hdf"; run $s 150 tools/tests/zenixboot-test.txt
        check "$s: a missing kernel is an AmigaOS error, back to the shell" "$(has $s 'zenixboot: nosuchkernel: AmigaOS error 205')"
        check "$s: a RAM disk that is too small is refused" "$(has $s 'S/Startup-Sequence is too small to be a RAM-disk image')"
        check "$s: a RAM disk that is not ext2 is refused after loading (the screen comes back)" "$(has $s 'zenixboot is not an ext2 filesystem image')"
        check "$s: CHECK loads and checks everything and returns" "$(has $s 'CHECK: everything is loaded and checked')"
        check "$s: then RAM=ramdisk boots Zenix with its root in RAM" \
            "$(grep -aq 'and the RAM disk ramdisk' "$OUT/$s.log" && grep -aq '^ram0  .* mounted' "$OUT/$s.log" && echo ok || echo bad)"
        check "$s: a shell prompt" "$(has $s '^zbok$')" ;;
    bootmenu)
        conf $s A1200 '$ZENIX_KICKSTART_DIR/kicka1200.rom' "" 4096 "$OUT/bootmenu.hdf"; run $s 300 tools/tests/bootmenu-test.txt
        tr -d '\r' <"$OUT/$s.log" | sed 's/\x1b\[[0-9;:]*[mK]//g' >"$OUT/$s.txt"
        check "$s: two boots through the menu" "$([ "$(grep -ac 'zenixboot 0.1b1: loading zenix (' "$OUT/$s.txt")" -ge 2 ] && echo ok || echo bad)"
        check "$s: the second boot is boot 2 and shows boot 1 as the previous one, ended with reboot" \
            "$(grep -aq '^klog: boot 2, ' "$OUT/$s.txt" && grep -aq '^=== previous boot (1) ended here with reboot' "$OUT/$s.txt" && echo ok || echo bad)"
        check "$s: no damaged log reported" "$(grep -aq 'damaged' "$OUT/$s.txt" && echo bad || echo ok)"
        check "$s: dmesg -p has boot 1's whole log (its kernel line)" "$(grep -aq '^prevkernel=1$' "$OUT/$s.txt" && echo ok || echo bad)"
        check "$s: a shell prompt" "$(has $s '^zbok$')" ;;
    *) echo "FAIL unknown setup $s" ; pass=0 ;;
    esac
done
echo "logs in $OUT"
exit $((1 - pass))
