#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host tests of the disk server (user/diskd.c) and fdisk (user/fdisk.c) against image files (tools/host/):
#   - C3: fdisk add/add/del/boot on an RDB laid out as HDToolBox would (other geometries, a FileSysHeader + LSEG
#     chain, a partition in a 16x63 geometry): nothing it does not own changes (tools/mkrdb-hdtb.py check);
#     a table it cannot keep exactly (a bad checksum) is not written at all;
#   - C4: handles stay with their partition across a rescan that changes the table; a device overlapping a
#     mounted one cannot be opened raw or mounted, nor written; a dead server's mount is released;
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d /tmp/zdisk.XXXXXX)
trap 'rm -rf $T' EXIT
CF="-m32 -DZHOST -std=gnu11 -O1 -g -Wall -Wno-format-truncation -fsanitize=address,undefined"
gcc $CF -o $T/fdisk user/fdisk.c tools/host/hostdisk.c || { echo "FAIL build fdisk"; exit 1; }
gcc $CF -o $T/diskd tools/host/diskd-host.c tools/host/hostdisk.c -Ituser || { echo "FAIL build diskd"; exit 1; }
pass=0; fail=0
check() { if [ "$2" = ok ]; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1"; fail=$((fail+1)); fi; }
fd() { HOSTDISK_BE=1 HOSTDISK=$T/d.hdf $T/fdisk "$@" > $T/fdisk.log 2>&1; }

# ---- C3: an HDToolBox-style RDB ----
python3 tools/mkrdb-hdtb.py make $T/d.hdf; cp $T/d.hdf $T/before.hdf
fd add ide0 ZX1 1 && fd add ide0 ZX2 rest && fd del ide0 4 && fd boot ide0 2 on; rc=$?
python3 tools/mkrdb-hdtb.py check $T/before.hdf $T/d.hdf > $T/check.log 2>&1 && [ $rc = 0 ] \
    && check "fdisk add/add/del/boot keeps every partition, the FileSysHeader/LSEG chain and the rest of the RDB" ok || { check "fdisk keeps an HDToolBox RDB" bad; cat $T/check.log $T/fdisk.log; }
grep -q "chain after: DH0@1:256+8192\* DH1@2:12800+6400\* DH2@7:20160+10080 ZX2@9:30336+2432$" $T/check.log \
    && check "new partitions go into free space in sectors (ZX2 after DH2, which is in another geometry)" ok || { check "placement of the new partitions" bad; cat $T/check.log; }
cp $T/before.hdf $T/d.hdf; fd add ide0 ZX1 1; python3 tools/mkrdb-hdtb.py check $T/before.hdf $T/d.hdf > $T/check.log 2>&1
grep -q "ZX1@8:8448+2048" $T/check.log && check "a partition fits the gap between DH0 and DH1" ok || { check "gap placement" bad; cat $T/check.log; }
fd add ide0 BIG 9; grep -q "not enough free space" $T/fdisk.log && check "too big a partition is refused" ok || { check "too big refused" bad; cat $T/fdisk.log; }
python3 tools/mkrdb-hdtb.py make $T/d.hdf badsum; cp $T/d.hdf $T/before.hdf
fd add ide0 ZX1 1; [ $? = 1 ] && grep -q "will not change a table" $T/fdisk.log && cmp -s $T/d.hdf $T/before.hdf \
    && check "a table with a bad checksum is not edited (image unchanged)" ok || { check "bad checksum refused" bad; cat $T/fdisk.log; }

# ---- C4: the disk server ----
python3 tools/mkrdb-hdtb.py make $T/d.hdf
cat > $T/c4.txt <<'EOF2'
list
open ide0p2 mount 5
open ide0p1 mount 6
open ide0 raw 7
open ide0 mount 7
open ide0p2 raw 7
open ide0 plain 7
write 0 100 7
write 0 13000 7
read 0 13000
sh python3 -I tools/host/rdb-del-first.py $HOSTDISK
rescan
list
open ide0p2 plain 8
write 3 0 5
write 2 0 5
dead 5
open ide0p1 raw 9
open ide0p2 raw 9
EOF2
HOSTDISK=$T/d.hdf $T/diskd < $T/c4.txt > $T/c4.out 2>&1
want() { grep -q -- "$2" $T/c4.out && check "diskd: $1" ok || { check "diskd: $1" bad; }; }
want "partitions in their own geometry (DH1 at sector 12800)" "list -> ide0p2 h2 start 12800 sectors 6400"
want "a mount" "open ide0p2 mount 5 -> 2"
want "a second partition can be mounted by another server" "open ide0p1 mount 6 -> 1"
want "raw open of the whole disk refused while a partition is mounted" "open ide0 raw 7 -> -16"
want "mount of the whole disk refused while a partition is mounted" "open ide0 mount 7 -> -16"
want "raw open of a mounted partition refused" "open ide0p2 raw 7 -> -16"
want "a plain open (reading) is allowed" "open ide0 plain 7 -> 0"
want "a write through the whole disk outside the mounts works" "write 0 100 7 -> 0"
want "a write through the whole disk into a mounted partition is refused" "write 0 13000 7 -> -16"
grep -A4 "^rescan" $T/c4.out | grep -q "list -> ide0p1 h2 start 12800" && check "diskd: after the table changed (DH0 deleted), the mounted DH1 keeps handle 2 and its sectors (now named ide0p1)" ok || { check "diskd: stable handle across a rescan" bad; cat $T/c4.out; }
want "the mounted handle still writes its own partition" "write 2 0 5 -> 0"
want "another partition can be written by its own handle" "write 3 0 5 -> 0"
want "the mount of a server that died is released" "pid 5) is gone: mount released"
want "... and the partition can be opened again" "open ide0p1 raw 9 -> 2"

echo "disk-host-test: $pass passed, $fail failed"
[ $fail = 0 ]
