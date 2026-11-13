#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host tests of the disk server (user/diskd.c) and fdisk (user/fdisk.c) against image files (tools/host/):
#   - C3: fdisk add/add/del/boot on an RDB laid out as HDToolBox would (other geometries, a FileSysHeader + LSEG
#     chain, a partition in a 16x63 geometry): nothing it does not own changes (tools/mkrdb-hdtb.py check);
#     a table it cannot keep exactly (a bad checksum) is not written at all;
#   - C4: handles stay with their partition across a rescan that changes the table; a device overlapping a
#     mounted one cannot be opened raw or mounted, nor written; a dead server's mount is released;
#   - C6: a disk whose flush does not work has no flush capability and DISK_FLUSH says so;
#   - D44: the CD drive (a model of the ATAPI drive): cd0 read-only, its 2048-byte blocks served as sectors (any sector,
#     any count: the bytes of the image), a disc changed under a mount answers -ESTALE until it is mounted again, a
#     drive becoming ready waited for, no disc, a restart keeps the CD's claim, no drive: no cd0.
cd "$(dirname "$0")/.." || exit 1
T=$(mktemp -d "${TMPDIR:-/tmp}/zdisk.XXXXXX")
[ -n "$KEEP" ] || trap 'rm -rf $T' EXIT
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

# a partition chain that loops (DH2 points back at DH0): listed once, never edited, no hang
python3 tools/mkrdb-hdtb.py make $T/d.hdf
python3 -I - $T/d.hdf <<'PY'
import struct, sys
f = open(sys.argv[1], "r+b"); f.seek(7 * 512); b = list(struct.unpack(">128L", f.read(512)))
b[4] = 1; b[2] = 0; b[2] = (-sum(b[:b[1]])) & 0xFFFFFFFF
f.seek(7 * 512); f.write(struct.pack(">128L", *b))
PY
cp $T/d.hdf $T/before.hdf
timeout 10 env HOSTDISK_BE=1 HOSTDISK=$T/d.hdf $T/fdisk add ide0 ZX1 1 > $T/fdisk.log 2>&1
grep -q "loops" $T/fdisk.log && cmp -s $T/d.hdf $T/before.hdf && check "a looping partition chain: fdisk refuses, image unchanged" ok || { check "looping chain refused" bad; cat $T/fdisk.log; }
printf 'list\n' | timeout 10 env HOSTDISK=$T/d.hdf $T/diskd > $T/loop.out 2>&1
[ "$(grep -c '^list -> ide0p' $T/loop.out)" = 3 ] && check "diskd lists a looping chain's partitions once" ok || { check "diskd and a looping chain" bad; cat $T/loop.out; }

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

# ---- D11 step 2: a restarted diskd keeps its slots and claims (the table it saved), refuses a damaged one ----
python3 tools/mkrdb-hdtb.py make $T/d.hdf
cat > $T/r.txt <<'EOF2'
open ide0p2 mount 5
open ide0p3 plain 7
sh python3 -I tools/host/rdb-del-first.py $HOSTDISK
rescan
restart
list
open ide0p1 raw 7
open ide0p1 mount 6
open ide0p1 mount 5
write 2 0 5
read 3 0
dead 5
restart
open ide0p1 raw 7
damage
restart
EOF2
HOSTDISK=$T/d.hdf $T/diskd < $T/r.txt > $T/r.out 2>&1
rwant() { grep -q -- "$2" $T/r.out && check "diskd restart: $1" ok || { check "diskd restart: $1" bad; cat $T/r.out; }; }
rwant "restored after a rescan renamed the mounted partition" "^restart -> 0$"
rwant "the mounted DH1 keeps handle 2 (a fresh scan would give it 1)" "list -> ide0p1 h2 start 12800 sectors 6400 .* mounted 5 "
rwant "the claim survived: a raw open of the mounted partition is refused" "^open ide0p1 raw 7 -> -16$"
rwant "... and so is another server's mount" "^open ide0p1 mount 6 -> -16$"
rwant "the holder of the claim asking again gets its handle back" "^open ide0p1 mount 5 -> 2$"
rwant "the old handle still writes its own partition" "^write 2 0 5 -> 0$"
rwant "a handle another program held still reads" "^read 3 0 -> 0$"
grep -A1 "^dead 5" $T/r.out | grep -q "^restart -> 0$" && grep -q "^open ide0p1 raw 7 -> 2$" $T/r.out \
    && check "diskd restart: a claim of a server that died is not kept" ok || { check "diskd restart: dead claim" bad; cat $T/r.out; }
rwant "a damaged saved table is refused" "^restart -> -1$"

# ---- C6: flush capability ----
python3 tools/mkrdb-hdtb.py make $T/d.hdf
printf 'list\nopen ide0p1 mount 5\nflush 1\n' | NOFLUSH=1 HOSTDISK=$T/d.hdf $T/diskd > $T/c6.out 2>&1
grep -q "list -> ide0 h0 start 0 sectors 32768 type 00000000 mounted 0 caps 0" $T/c6.out && grep -q "flush 1 -> -95" $T/c6.out \
    && check "a disk whose flush fails has no flush capability, DISK_FLUSH answers -95" ok || { check "flush capability" bad; cat $T/c6.out; }
printf 'list\nopen ide0p1 mount 5\nflush 1\n' | HOSTDISK=$T/d.hdf $T/diskd > $T/c6.out 2>&1
grep -q "list -> ide0 h0 .* caps 1" $T/c6.out && grep -q "flush 1 -> 0" $T/c6.out && check "a disk whose flush works has the capability" ok || { check "flush capability (working)" bad; cat $T/c6.out; }

# ---- D: only root talks to the disk server (raw sectors bypass every file permission) ----
python3 tools/mkrdb-hdtb.py make $T/d.hdf
printf 'open ide0p1 mount 5\nas 1000\nlist\nopen ide0 plain 7\nopen ide0p2 raw 7\nread 0 0\nwrite 2 0 7\nflush 1\nrescan\nunmount 1 5\nas 0\nread 0 0\n' | HOSTDISK=$T/d.hdf $T/diskd > $T/d.out 2>&1
grep -q "^list -> ide0p1 " $T/d.out && grep -q "^open ide0 plain 7 -> -13$" $T/d.out && grep -q "^open ide0p2 raw 7 -> -13$" $T/d.out && grep -q "^read 0 0 -> -13$" $T/d.out \
    && grep -q "^write 2 0 7 -> -13$" $T/d.out && grep -q "^flush 1 -> -13$" $T/d.out && grep -q "^rescan -> -13$" $T/d.out && grep -q "^unmount 1 5 -> -13$" $T/d.out \
    && check "diskd: a user may list devices, but not open, read, write, flush, rescan or unmount (-13)" ok || { check "diskd refuses users" bad; cat $T/d.out; }
grep -q "^read 0 0 -> 0$" $T/d.out && check "diskd: root still reads" ok || { check "diskd: root still reads" bad; cat $T/d.out; }

# ---- D44: the CD drive ----
python3 tools/mkrdb-hdtb.py make $T/d.hdf
python3 -c "import random; open('$T/cd.iso','wb').write(random.Random(44).randbytes(2048 * 300))"
cat > $T/cd.txt <<EOF2
list
open cd0 mount 5
readx 1 65 3 $T/x1
readx 1 0 128 $T/x2
readx 1 1197 3 $T/x3
readx 1 1198 3 $T/x4
readx 1 0 129 $T/x5
write 1 0 5
open cd0 raw 7
cd change
read 1 64 5
read 1 64 5
read 1 64
unmount 1 5
open cd0 plain 7
read 1 64
cd becoming 3
open cd0 mount 6
read 1 64 6
cd eject
rescan
list
cd insert
read 1 64 6
unmount 1 6
open cd0 mount 6
read 1 64 6
restart
open cd0 raw 7
open cd0 mount 6
read 1 64 6
EOF2
HOSTCD=$T/cd.iso HOSTDISK=$T/d.hdf $T/diskd < $T/cd.txt > $T/cd.out 2>&1
cwant() { grep -q -- "$2" $T/cd.out && check "cd0: $1" ok || { check "cd0: $1" bad; cat $T/cd.out; }; }
after() { grep -A"$2" -- "^$1" $T/cd.out | grep -q -- "$3"; }
part() { dd if=$T/cd.iso bs=512 skip=$1 count=$2 status=none | cmp -s - $3; }
cwant "listed with the disc's size in sectors, read-only (caps 8)" "list -> cd0 h1 start 0 sectors 1200 type 00000000 mounted 0 caps 8"
part 65 3 $T/x1 && check "cd0: sectors 65-67 (inside two 2048-byte blocks) are the image's" ok || check "cd0: sectors 65-67" bad
part 0 128 $T/x2 && check "cd0: 128 sectors at once" ok || check "cd0: 128 sectors at once" bad
part 1197 3 $T/x3 && check "cd0: the last sectors" ok || check "cd0: the last sectors" bad
cwant "past the end refused" "readx 1 1198 3 .* -> -22"
cwant "more than 128 sectors at once refused" "readx 1 0 129 .* -> -22"
cwant "a write refused: read-only" "write 1 0 5 -> -30"
cwant "no raw open while mounted" "open cd0 raw 7 -> -16"
[ "$(grep -A2 '^cd change' $T/cd.out | grep -c -- '-> -116')" = 2 ] && check "cd0: a disc changed under the mount: -ESTALE for its server, and stays so" ok || { check "cd0: changed disc" bad; cat $T/cd.out; }
after "cd change" 3 "^read 1 64 -> 0" && check "cd0: ... another reader reads the new disc" ok || check "cd0: another reader after a change" bad
after "unmount 1 5" 2 "^read 1 64 -> 0" && check "cd0: unmounted, the new disc reads" ok || check "cd0: after the change" bad
after "cd becoming 3" 2 "^read 1 64 6 -> 0" && check "cd0: a drive becoming ready is waited for at the open" ok || check "cd0: becoming ready" bad
cwant "no disc: listed with 0 sectors" "list -> cd0 h1 start 0 sectors 0 "
after "cd insert" 1 "^read 1 64 6 -> -116" && check "cd0: a disc put in under a mount: -ESTALE" ok || check "cd0: disc put in under a mount" bad
after "unmount 1 6" 2 "^read 1 64 6 -> 0" && check "cd0: ... it reads after a new mount" ok || check "cd0: disc put in, new mount" bad
after restart 3 "^open cd0 raw 7 -> -16" && after restart 3 "^open cd0 mount 6 -> 1" && after restart 3 "^read 1 64 6 -> 0" \
    && check "cd0: a restarted diskd keeps the CD's claim, and the mount reads on" ok || { check "cd0: restart" bad; cat $T/cd.out; }
printf 'list\n' | HOSTDISK=$T/d.hdf $T/diskd > $T/nocd.out 2>&1
grep -q "cd0" $T/nocd.out && check "no CD drive: no cd0" bad || check "no CD drive: no cd0" ok

# ---- D46: a USB stick through the USB server (a stub of usbd behind the posts): usb0 and its partitions, read-only ----
python3 - "$T/stick.img" <<'EOP'
import struct, sys
n = 20000                                   # 10 MB: an MBR, a FAT32 partition (0x0C) and a Linux one (0x83)
img = bytearray(n * 512)
for i in range(n): img[i * 512:i * 512 + 4] = struct.pack('<I', i)       # every sector says its number
def ent(boot, typ, start, size): return bytes([boot, 0, 0, 0, typ, 0, 0, 0]) + struct.pack('<II', start, size)
mbr = bytearray(img[0:512]); mbr[446:446 + 32] = ent(0x80, 0x0C, 2048, 8192) + ent(0, 0x83, 10240, 9760)
mbr[510] = 0x55; mbr[511] = 0xAA; img[0:512] = mbr
open(sys.argv[1], 'wb').write(img)
EOP
H=$(printf 'usb hello\nusb add\nlist\n' | HOSTUSB=$T/stick.img HOSTDISK=$T/d.hdf $T/diskd 2>&1 | sed -n 's/^list -> usb0p1 h\([0-9]*\) .*/\1/p')
cat > $T/usb.txt <<EOF2
list
usb hello-other
usb hello
usb add
list
open usb0p1 mount 30
readx $H 0 128 $T/u1
readx $H 8100 92 $T/u2
readx $H 8150 50 $T/u3
readx $H 0 129 $T/u4
write $H 0 30
usb fail 1
read $H 5 30
open fd0 plain 1
rescan
list
usb dead
read $H 5 30
usb alive
unmount $H 30
usb add
open usb0p1 mount 31
usb del
read $H 5 31
list
unmount $H 31
list
usb add
open usb0p1 mount 30
restart
usb beat
open usb0p1 mount 30
read $H 5 30
list
usb late 10
read $H 6 30
usb late 100000
read $H 7 30
usb late 0
usb add
usb flood 12
list
EOF2
HOSTUSB=$T/stick.img HOSTDISK=$T/d.hdf $T/diskd < $T/usb.txt > $T/usb.out 2>&1
uwant() { grep -q -- "$2" $T/usb.out && check "usb0: $1" ok || { check "usb0: $1" bad; cat $T/usb.out; }; }
upart() { dd if=$T/stick.img bs=512 skip=$1 count=$2 status=none | cmp -s - $3; }
[ "$(sed -n '1,/^usb hello /p' $T/usb.out | grep -c 'list -> usb0')" = 0 ] && check "usb0: nothing before the USB server's hello" ok || check "usb0: before hello" bad
uwant "a hello from a process that does not own port usb refused" "usb hello-other -> -1"
uwant "the server's hello answered" "^usb hello -> 0"
uwant "the stick listed with its label, read-only (caps 8)" "list -> usb0 h[0-9]* start 0 sectors 20000 type 00000000 mounted 0 caps 8"
uwant "its FAT32 partition (MBR type 0x0C)" "list -> usb0p1 h$H start 2048 sectors 8192 type 4d42520c"
uwant "its FAT32 partition says read-only too (caps 8: fatd mounts it -o ro by itself, D30)" "list -> usb0p1 h$H start 2048 sectors 8192 type 4d42520c mounted 0 caps 8"
uwant "its Linux partition (ext2)" "list -> usb0p2 h[0-9]* start 10240 sectors 9760 type 4c4e5800"
upart 2048 128 $T/u1 && check "usb0p1: 128 sectors at once are the stick's" ok || check "usb0p1: 128 sectors" bad
upart 10148 92 $T/u2 && check "usb0p1: its last 92 sectors" ok || check "usb0p1: the last sectors" bad
uwant "past the partition's end refused" "readx $H 8150 50 .* -> -22"
uwant "more than the server's 128 sectors at once refused" "readx $H 0 129 .* -> -22"
uwant "a write refused: read-only" "write $H 0 30 -> -30"
uwant "the server's error comes back (-EIO)" "read $H 5 30 -> -5"
[ "$(sed -n '/^rescan/,/^usb dead/p' $T/usb.out | grep -c 'list -> usb0')" = 3 ] && check "usb0: a rescan for df0 or the table keeps the stick's devices" ok || { check "usb0: rescan keeps them" bad; cat $T/usb.out; }
grep -A2 '^usb dead' $T/usb.out | grep -q -- '-> -5' && check "usb0: a server that stops answering: -EIO after the wait, nothing hangs" ok || check "usb0: dead server" bad
grep -A1 '^usb del' $T/usb.out | grep -q -- "read $H 5 31 -> -116" && check "usb0: the stick pulled under a mount: -ESTALE" ok || check "usb0: pulled under a mount" bad
[ "$(sed -n "/^unmount $H 31/,/^usb add/p" $T/usb.out | grep -c 'list -> usb0')" = 0 ] && check "usb0: unmounted, gone from the list" ok || check "usb0: gone after unmount" bad
grep -A4 '^restart' $T/usb.out | grep -q "open usb0p1 mount 30 -> $H" && grep -A5 '^restart' $T/usb.out | grep -q "read $H 5 30 -> 0" \
    && check "usb0: a restarted diskd keeps the mount's slot; the server's heartbeat finds no window, says hello and brings the stick back to it" ok || { check "usb0: restart" bad; cat $T/usb.out; }
uwant "an answer that comes while diskd waits: read" "read $H 6 30 -> 0"
grep -A2 "^usb late 100000" $T/usb.out | grep -q "read $H 7 30 -> -5" && check "usb0: an answer that never comes: -EIO after the 5 s wait, the stick dropped" ok || check "usb0: no answer" bad
grep -q "records from the USB server were lost" $T/usb.out && [ "$(sed -n '/^usb flood/,$p' $T/usb.out | grep -c 'list -> usb0 ')" = 1 ] \
    && check "usb0: more news than the window holds: said, the newest taken (the stick there)" ok || { check "usb0: flood" bad; cat $T/usb.out; }
printf 'list\nusb add\nlist\n' | HOSTUSB=$T/stick.img HOSTDISK=$T/d.hdf $T/diskd > $T/nousb.out 2>&1
grep -q "usb0" $T/nousb.out && check "usb0: no hello, no stick (news before a hello ignored)" bad || check "usb0: no hello, no stick (news before a hello ignored)" ok

echo "disk-host-test: $pass passed, $fail failed"
[ $fail = 0 ]
