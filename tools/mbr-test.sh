#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Boot Zenix from disks that carry a PC partition table (like an Emu68 SD card) in the emulator:
#   1. MBR with a FAT boot partition and an ext2 partition (type 0x83)
#   2. MBR with a FAT boot partition and a type 0x76 partition holding an Amiga RDB with the ext2 root
# The system can only come up if the disk server found the partition and the filesystem server mounted it.
# usage: tools/mbr-test.sh   (uses ZENIX_SERIAL, default /tmp/zenix-mbr-serial, so it can run beside other tests)
cd "$(dirname "$0")/.." || exit 1
make -s all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=/tmp/zenix-mbr; mkdir -p $OUT
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-mbr-serial}
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
# hard_drive_0_type = rdb: without it FS-UAE hides a file that has no RDB behind a virtual RDB of its own
sed -e "s#hard_drive_0 = .*#hard_drive_0 = ../build/zenix-mbr.hdf\nhard_drive_0_type = rdb#" fsuae/a1200-020-fast.fs-uae > fsuae/mbr-test.fs-uae
for mode in plain rdb; do
    if [ $mode = rdb ]; then tools/mkmbr.py build/zenix-mbr.hdf build/zenix.hdf rdb >/dev/null; else tools/mkmbr.py build/zenix-mbr.hdf build/zenix.hdf >/dev/null; fi
    while pgrep -x fs-uae >/dev/null; do sleep 1; done          # FS-UAE allows one instance at a time
    tools/run-test.sh fsuae/mbr-test.fs-uae 50 tools/tests/mbr-test.txt > $OUT/$mode.log 2>&1
    grep -q 'ext2: 1 KB blocks' $OUT/$mode.log && check "$mode: root mounted from the partition" ok || check "$mode: root mounted from the partition" bad
    grep -q 'ide0p1' $OUT/$mode.log && check "$mode: FAT partition listed" ok || check "$mode: FAT partition listed" bad
    grep -q '^bin' $OUT/$mode.log && check "$mode: shell works" ok || check "$mode: shell works" bad
    grep -q 'init would destroy them' $OUT/$mode.log && check "$mode: fdisk init refuses to wipe an MBR disk" ok || check "$mode: fdisk init refuses to wipe an MBR disk" bad
    grep -q 'fdisk edits Amiga tables' $OUT/$mode.log && check "$mode: fdisk add explains it does not edit an MBR" ok || check "$mode: fdisk add explains it does not edit an MBR" bad
done
rm -f fsuae/mbr-test.fs-uae
exit $((1 - pass))
