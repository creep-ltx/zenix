#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# FAT in the emulator (DESIGN D30): an MBR card as the Pi's (FAT, then the ext2 root; FAT16 and FAT32 rounds) and a
# FAT12 floppy image without a partition table. mount, list, read, write, a long name with Swedish letters, rename,
# delete, what FAT refuses, the mount options, a program run from FAT, the server killed. The images checked afterwards on
# the PC with fsck.fat and mtools.
# usage: tools/fat-test.sh [16] [32]    (both by default)
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-fat-test}; mkdir -p $OUT
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-fat-serial}
export LC_ALL=C.UTF-8 MTOOLS_SKIP_CHECK=1
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
has() { if tr -d '\r' < $L | grep -aqE -- "$2"; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
hasnot() { if tr -d '\r' < $L | grep -aqE -- "$2"; then echo "FAIL $1"; pass=0; else echo "PASS $1"; fi; }
[ $# = 0 ] && set -- 16 32
for bits in "$@"; do
    L=$OUT/fat$bits.log
    CARD=$BD/fat-card.hdf
    tools/mkmbr.py $CARD $BD/zenix.hdf >/dev/null || { echo "FAIL mkmbr"; exit 1; }
    P=$OUT/part.img
    dd if=$CARD of=$P bs=512 skip=2048 count=32768 status=none
    # mkmbr's fake boot files on the volume this round asks for
    if [ $bits = 32 ]; then mkfs.fat -F 32 -s 1 -n EMU68BOOT $P >/dev/null 2>&1; else mkfs.fat -F 16 -s 1 -n EMU68BOOT $P >/dev/null; fi
    echo "fake config" > $OUT/c.txt; mcopy -i $P $OUT/c.txt ::config.txt; mcopy -i $P $OUT/c.txt ::kick.rom
    echo "written by linux" > $OUT/l.txt; mcopy -i $P $OUT/l.txt "::from-linux-long-name.txt"
    dd if=$P of=$CARD bs=512 seek=2048 conv=notrunc status=none
    F=$OUT/df1.adf
    head -c 901120 /dev/zero > $F; mkfs.fat -F 12 -n FLOP $F >/dev/null
    echo on-the-floppy > $OUT/fl.txt; mcopy -i $F $OUT/fl.txt ::ON-FLOP.TXT
    # hard_drive_0_type = rdb: without it FS-UAE hides a file that has no RDB behind a virtual RDB of its own
    sed -e "s#hard_drive_0 = .*#hard_drive_0 = ../$(basename $BD)/fat-card.hdf\nhard_drive_0_type = rdb#" \
        -e "s#^floppy_drive_0 = .*#&\nfloppy_drive_1 = $(realpath $F)\nfloppy_drive_count = 2\nwritable_floppy_images = 1#" fsuae/a1200-020-fast.fs-uae > $OUT/fat.fs-uae
    case $BD in /*) sed -i "s#\.\./$(basename $BD)/fat-card.hdf#$BD/fat-card.hdf#" $OUT/fat.fs-uae ;; esac
    tools/run-test.sh $OUT/fat.fs-uae 300 tools/tests/fat-test.txt > $L 2>&1
    echo "--- FAT$bits: in the emulator"
    has "the script ran to the end" '^(.*# )?fat-test-done$'
    has "fdisk lists the FAT partition" 'ide0p1'
    has "mount finds the type from the MBR" 'ide0p1 +/mnt +fat '
    has "reads a file from Linux, any case" '^fake config$'
    has "... a long name" '^written by linux$'
    has "writes and reads back" '^hello-from-zenix$'
    has "a Swedish long name is listed" 'räksmörgås långt namn\.txt'
    has "... and found in other case" '^svenska$'
    has "a program copied byte for byte" '^(.*# )?same-copy$'
    has "ln -s is refused" 'ln: .*(not permitted)'
    has "chmod 777 is refused" 'chmod: /mnt/ls\.bin: cannot change'
    has "chown is refused" 'chown: /mnt/ls\.bin: cannot change owner'
    has "a name FAT cannot hold is refused" 'a:b.*(invalid argument)'
    has "a file without x does not run" 'mnt/ls\.bin: .*permission denied|permission denied.*ls\.bin'
    has "options: fmode and uid show" 'rwxr-xr-x +1 +[^ ]*7? .*ls\.bin'
    has "... and the program runs from FAT" '^(.*# )?run-from-fat=0$'
    has "a bad option: mount says so" 'cannot mount ide0p1: its filesystem server did not start'
    has "... and the log why" 'fatd: bad option at: nonsense'
    has "saved and clean by the timers alone: a kill later, the restart stays writable" '^(.*# )?wrote-after-clean$'
    has "killed with a change not committed: the restart says it" 'was not unmounted cleanly: mounted read-only'
    has "... and refuses writes" 'refused\.txt.*read-only'
    has "the floppy (FAT12, no partition table) reads" '^on-the-floppy$'
    has "... lists what was written" 'skriven\.txt'
    has "paths: a tree 225 bytes deep on FAT: mkdir -p" '^(.*# )?fd1=0$'
has "... cp -r" '^(.*# )?fd2=0$'
has "... find" '^(.*# )?fd3=9$'
has "... rm -r" '^(.*# )?fd4=0$'
has "... the copy whole" '^(.*# )?fd5=9$'
hasnot "fatd did not crash" 'fatd.*(exception|crash|bus error|address error)'
    echo "--- FAT$bits: back on the PC"
    dd if=$CARD of=$P bs=512 skip=2048 count=32768 status=none
    cp $P $OUT/before-ro.img
    fsck.fat -n $P > $OUT/fsck.log 2>&1 && check "fsck.fat finds nothing" ok || { check "fsck.fat finds nothing" bad; head -20 $OUT/fsck.log; }
    g() { mtype -i $P "::$1" 2>/dev/null; }
    [ "$(g 'Mappen/räksmörgås långt namn.txt')" = svenska ] && check "Linux reads the Swedish long name" ok || check "Linux reads the Swedish long name" bad
    [ "$(g Mappen/moved.txt)" = hello-from-zenix ] && check "... the renamed file" ok || check "... the renamed file" bad
    [ "$(g timer.txt)" = by-the-timer ] && check "Linux reads what the commit timer saved" ok || check "Linux reads what the commit timer saved" bad
    [ "$(g after-clean.txt)" = still-writable ] && check "... and what was written after that restart" ok || check "... and what was written after that restart" bad
    [ "$(g forced.txt)" = fixed ] && check "... what was written after -o force" ok || check "... what was written after -o force" bad
    mcopy -n -i $P ::ls.bin $OUT/ls.bin && cmp -s $OUT/ls.bin $BD/ls.zx && check "... the copied program, byte for byte" ok || check "... the copied program, byte for byte" bad
    mdir -i $P ::kick.rom >/dev/null 2>&1 && check "a deleted file is gone" bad || check "a deleted file is gone" ok
    mdir -i $P ::NEW.TXT >/dev/null 2>&1 && check "a renamed file left its old name" bad || check "a renamed file left its old name" ok
    python3 - $P <<'PY' && check "the volume is marked clean" ok || check "the volume is marked clean" bad
import struct, sys
d = open(sys.argv[1], "rb").read()
res = struct.unpack_from("<H", d, 14)[0]
fatsz16 = struct.unpack_from("<H", d, 22)[0]
if fatsz16: v = struct.unpack_from("<H", d, res * 512 + 2)[0] & 0x8000
else: v = struct.unpack_from("<I", d, res * 512 + 4)[0] & 0x08000000
sys.exit(0 if v else 1)
PY
    # D30: the same card again, the FAT partition mounted -o ro, every change tried; the partition byte for byte the same
    L=$OUT/fat$bits-ro.log
    tools/run-test.sh $OUT/fat.fs-uae 200 tools/tests/fat-ro-test.txt > $L 2>&1
    echo "--- FAT$bits: mounted -o ro"
    has "ro: the script ran to the end" '^(.*# )?fat-ro-done$'
    has "ro: mounted" 'ide0p1 +/mnt +fat '
    has "ro: reads a file" '^fake config$'
    has "ro: reads what was written before" '^still-writable$'
    has "ro: df works" 'ide0p1 +[0-9]+'
    for i in 1 2 3 4 5 6 7 8 9 10 11; do has "ro: change $i refused" "^(.*# )?ro$i=[1-9]"; done
    has "ro: the message says read-only" 'read-only file system'
    hasnot "ro: fatd did not crash" 'fatd.*(exception|crash|bus error|address error)'
    dd if=$CARD of=$OUT/after-ro.img bs=512 skip=2048 count=32768 status=none
    cmp -s $OUT/before-ro.img $OUT/after-ro.img && check "ro: the FAT partition is byte for byte unchanged" ok || check "ro: the FAT partition is byte for byte unchanged" bad
    fsck.fat -n $OUT/after-ro.img > $OUT/fsck-ro.log 2>&1 && check "ro: fsck.fat finds nothing" ok || { check "ro: fsck.fat finds nothing" bad; head -20 $OUT/fsck-ro.log; }
    fsck.fat -n $F > $OUT/fsck-f.log 2>&1 && check "the floppy image: fsck.fat finds nothing" ok || { check "the floppy image: fsck.fat finds nothing" bad; head -20 $OUT/fsck-f.log; }
    [ "$(mtype -i $F ::skriven.txt 2>/dev/null)" = written-on-floppy ] && check "the floppy image: Linux reads what was written" ok || check "the floppy image: Linux reads what was written" bad
done
rm -f $OUT/fat.fs-uae $BD/fat-card.hdf $OUT/part.img $OUT/before-ro.img $OUT/after-ro.img
exit $((1 - pass))
