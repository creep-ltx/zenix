#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# ZenixFS as the root the way a real machine would have it (DESIGN D34), in the emulator, on test images only:
#   c  the owner's card layout (tools/mkcard.py sd: MBR, p1 FAT, p2 0x76 with an RDB, p3 0x83 ZenixFS root, plus p4 0x83 ext2
#      /home), the ZenixFS kernel from the floppy: the root is found, mount and df, /home, files, sync, reboot, killed without
#      sync 6 s after a write, with a read after it: FS-UAE holds the last write of a disk until the next command, so an idle
#      disk would not show the commit timer's work; p1 and p2 byte for byte unchanged, fsck and e2fsck on the PC
#   p  the fault test: the emulator killed at different moments while files are written, renamed over and deleted, with a file
#      deleted while open: after each cut the PC's fsck is clean and every file synced before the cut compares exact, what
#      came after is whole or a prefix, never other bytes; the next start needs no check and goes on on the same disk
#   r  damage: a damaged inner tree node (the zenix-mount floppy's fsck.zenixfs finds it, -y repairs it with nothing lost, then
#      the disk boots); a destroyed superblock (the kernel stops and says what to do, the floppy repairs it); the two newest
#      commit records destroyed (read-only, boot's choices without "read-write", a shell, writes refused, nothing written)
#   m  2 MB of chip RAM alone: boots, works, the free memory printed and held to a floor; a root of 4 KB blocks too
#   w  tools/write-root.sh's ZenixFS mode on a card image file (no device): only p3 changes, settings carried, the way back
# usage: tools/zroot-card-test.sh [PART...]    (ZENIX_SERIAL, ZENIX_OUT, ZENIX_BUILD as the other tests)
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-/tmp/zenix-zrootcard}; mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
SER=${ZENIX_SERIAL:-/tmp/zenix-zrootcard-serial}
PARTS="${*:-w c p r m}"
make -s BUILD=$BD all $BD/zenix-zfs.adf $BD/zenix-mount.adf $BD/zenixboot >"$OUT/make.log" 2>&1 || { tail "$OUT/make.log"; echo "FAIL the build"; exit 1; }
make -s -C zenixfs build/zenixfs-tool >/dev/null 2>&1 || { echo "FAIL the host tool"; exit 1; }
BD=$(cd "$BD" && pwd)
Z=zenixfs/build/zenixfs-tool
PROGS=$(make -s BUILD=$BD -f Makefile --eval 'pa:;@echo $(UAPPS) $(SBINAPPS) $(NETDRVS:%=drv-%)' pa 2>/dev/null)
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
yes_if() { if "$@" >/dev/null 2>&1; then echo ok; else echo bad; fi; }
clean() { tr -d '\r' | sed 's/\x1b\[[0-9;:]*[mK]//g'; }
# mkimg OUT MB: a root image with the full tree (and /home as a mount point, the fstab given)
mkimg() { PKGDB=1 RAWIMG=1 FSTAB_FILE=$3 tools/mkroot-zenixfs.sh "$1" "$BD" "$PROGS" "$2" >/dev/null; }
# conf FILE DISK FLOPPY [chip fast]
conf() {
    cat >"$1" <<EOF
[fs-uae]
accuracy = 1
amiga_model = A1200
kickstart_file = \$ZENIX_KICKSTART_DIR/kicka1200.rom
chip_memory = ${4:-2048}
fast_memory = ${5:-8192}
floppy_drive_0 = $3
floppy_drive_speed = 0
serial_port = /tmp/zenix-serial
uae_serial_direct = true
hard_drive_0 = $2
hard_drive_0_controller = ide0
joystick_port_0 = none
joystick_port_1 = none
EOF
}
run() {     # CONF SECONDS SCRIPT LOG
    ZENIX_SERIAL=$SER tools/run-test.sh "$1" "$2" "$3" 2>&1 | clean >"$4"
}
has() { grep -aqE -- "$2" "$1" && echo ok || echo bad; }
# the partition of a card image (sfdisk): "START SIZE" of the Nth partition
part() { sfdisk -d "$1" 2>/dev/null | grep "^$1$2 " | sed 's/.*start= *\([0-9]*\), size= *\([0-9]*\).*/\1 \2/'; }
pget() { local p; p=$(part "$1" "$2"); dd if="$1" of="$3" bs=512 skip=${p% *} count=${p#* } status=none; }

part_c() {
    local d=$OUT/c; rm -rf $d; mkdir -p $d
    printf '# test fstab\nide0p5  /home  ext2  defaults\n' >$d/fstab
    mkimg $d/root.img 96 $d/fstab || { check "c: the root image" bad; return; }
    printf 'SYS:zenixboot\n' >$d/ss
    python3 tools/mkffs.py $d/ffs.img 16632K ZenixBoot $d/ss=S/Startup-Sequence $BD/zenixboot=zenixboot $BD/kernel-zfs.bin=Zenix/zenix >/dev/null &&
        python3 tools/mkcard.py sd $d/sd.img $d/ffs.img $d/root.img >/dev/null || { check "c: the card image" bad; return; }
    # p4: /home, ext2, as the owner's card has it
    mke2fs -q -t ext2 -E revision=0 -b 1024 -F $d/home.img 16M || { check "c: /home" bad; return; }
    python3 -I - $d/sd.img $d/home.img <<'PY' || { check "c: p4" bad; return; }
import os, struct, sys
img, home = sys.argv[1:3]
size = os.path.getsize(img) // 512
start = (size + 2047) // 2048 * 2048
data = open(home, "rb").read()
with open(img, "r+b") as f:
    f.seek(446 + 3 * 16)
    f.write(struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", 0x83, b"\xfe\xff\xff", start, len(data) // 512))
    f.seek(start * 512)
    f.write(data)
PY
    pget $d/sd.img 2 $d/p2-before.img
    conf $d/m.fs-uae $d/sd.img $BD/zenix-zfs.adf
    cat >$d/k1.txt <<'EOF'
wait zenix:/# 600
sleep 2
send mount; df; ls /home; uname -a\r
sleep 6
send echo card-persist > /card.txt; seq 1 4000 > /seq.txt; echo home-persist > /home/h.txt; ln -s card.txt /clink; mkdir /cdir; sync; echo CSYNC-DONE\r
wait (?m)^CSYNC-DONE 120
sleep 1
send reboot\r
wait (?m)^klog: boot [0-9]+, 300
wait zenix:/# 600
sleep 2
send cat /clink /home/h.txt; wc -l /seq.txt; echo unsynced > /late.txt; echo LATE-WRITTEN\r
wait (?m)^LATE-WRITTEN 60
sleep 6
send wc -l /usr/man/ls.txt; dmesg | tail -1; echo READ-DONE\r
wait (?m)^READ-DONE 30
sleep 1
stop
EOF
    run $d/m.fs-uae 900 $d/k1.txt $d/1.log
    check "c: the root is found on the card's layout: ide0p4 (the MBR's p3), ZenixFS" "$(has $d/1.log '^ide0p4 +/ +zenixfs ')"
    check "c: /home from the fstab: the MBR's p4, ext2" "$(has $d/1.log '^ide0p5 +/home +ext2 ')"
    check "c: df names the root" "$(has $d/1.log '^ide0p4 +[0-9]+ +[0-9]+ +[0-9]+ +[0-9]+% .*/$')"
    check "c: sync returned" "$(has $d/1.log '^CSYNC-DONE')"
    check "c: after the reboot: a symbolic link, a file on /home" "$(grep -aq '^card-persist' $d/1.log && grep -aq '^home-persist' $d/1.log && echo ok || echo bad)"
    check "c: ... and a bigger file" "$(has $d/1.log '^ *4000 /seq.txt')"
    pget $d/sd.img 2 $d/p2-after.img; pget $d/sd.img 3 $d/p3-after.img; pget $d/sd.img 4 $d/p4-after.img
    check "c: the Amiga partition (p2) byte for byte unchanged" "$(yes_if cmp -s $d/p2-before.img $d/p2-after.img)"
    check "c: p1 (FAT) never written: all holes" "$(python3 -I -c 'import sys
f = open(sys.argv[1], "rb"); f.seek(8192 * 512); d = f.read(1 << 20); sys.exit(any(d))' $d/sd.img && echo ok || echo bad)"
    check "c: the PC's fsck of the root, clean" "$(yes_if $Z fsck $d/p3-after.img)"
    check "c: the file written 6 s before the cut, no sync: the commit timer wrote it" "$($Z cat $d/p3-after.img /late.txt 2>/dev/null | grep -q unsynced && echo ok || echo bad)"
    check "c: /home: e2fsck -fn clean" "$(yes_if e2fsck -fn $d/p4-after.img)"
}

# the guest's writer: rounds of a file per round (its content a function of the round), one rename-over, one delete, a sync
# and a line on the console; the host computes the same contents
part_p() {
    local d=$OUT/p r; rm -rf $d; mkdir -p $d
    mkimg $d/root.img 64 /dev/null || { check "p: the root image" bad; return; }
    mkdir -p $d/t; cat >$d/t/w.sh <<'EOF'
# the writer: sh /w.sh FIRST
n=$1
mkdir -p /t
while true; do
    seq $n $((n * 40 + 200)) > /t/f$n
    seq $n $((n + 300)) > /t/cur.tmp
    mv /t/cur.tmp /t/cur
    [ $((n % 5)) = 0 ] && rm /t/f$((n - 3))
    echo $n > /t/synced.tmp; mv /t/synced.tmp /t/lastsync
    sync
    echo SYNCED $n
    n=$((n + 1))
done
EOF
    $Z put $d/root.img $d/t/w.sh /w.sh || { check "p: the writer" bad; return; }
    python3 tools/mkrdb.py $d/root.img $d/disk.hdf zenixfs >/dev/null
    conf $d/m.fs-uae $d/disk.hdf $BD/zenix-zfs.adf
    local first=1 cuts="2 5 9 14 20"
    for r in $cuts; do
        cat >$d/k$r.txt <<EOF
wait zenix:/# 600
sleep 2
send sleep 500 < /etc/motd &\r
sleep 1
send cp /etc/motd /t-open 2>/dev/null; (sleep 500 < /t-open &) ; rm -f /t-open; sh /w.sh $first\r
wait (?m)^SYNCED $((first + 1)) 300
sleep $r
stop
EOF
        run $d/m.fs-uae 600 $d/k$r.txt $d/$r.log
        python3 -I - $d/disk.hdf $d/cut-$r.img <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
i = d.find(b"ZNFS", 512)
while i >= 0 and not (i % 512 == 32 and d[i - 28:i - 26] == b"ZB"):
    i = d.find(b"ZNFS", i + 1)
open(sys.argv[2], "wb").write(d[i - 32:])
PY
        check "p: cut $r s into the writes: the PC's fsck, clean" "$(yes_if $Z fsck $d/cut-$r.img)"
        # the round before the last the console saw: FS-UAE keeps the disk's last write back until the next command (a kill
        # loses it, whatever the flush said), so only a sync with a disk command after it is known to be on the image. The
        # host's replay (zenixfs/tests/ztest.c powercut) is what proves the flush order
        last=$(( $(grep -a '^SYNCED ' $d/$r.log | tail -1 | cut -d' ' -f2) - 1 ))
        rm -rf $d/out; mkdir -p $d/out
        $Z get $d/cut-$r.img -r /t $d/out/t >$d/get-$r.log 2>&1 || check "p: cut $r: every file readable" bad
        python3 -I - $d/out/t "$last" <<'PY' >$d/cmp-$r.log 2>&1
import os, sys
t, last = sys.argv[1], int(sys.argv[2])
def seq(a, b): return "".join(f"{i}\n" for i in range(a, b + 1)).encode()
bad = 0
# everything up to the last round the console saw synced: exact (or deleted by a later round)
for n in range(1, last + 1):
    p = os.path.join(t, f"f{n}")
    if (n + 3) % 5 == 0:                   # round n + 3 removes it: gone if that round is synced, either way if it came after
        if n + 3 <= last:
            if os.path.exists(p):
                print("removed in a synced round, but there:", p); bad += 1
            continue
        if not os.path.exists(p):
            continue
    if not os.path.exists(p) or open(p, "rb").read() != seq(n, n * 40 + 200):
        print("missing or wrong:", p); bad += 1
# later rounds: a file is whole or a prefix (a commit may come between two writes), never other bytes
for f in os.listdir(t):
    if f.startswith("f"):
        n = int(f[1:]);
        if n > last and not seq(n, n * 40 + 200).startswith(open(os.path.join(t, f), "rb").read()):
            print("not a prefix:", f); bad += 1
# the renamed-over file is always one whole version, never a mix
c = open(os.path.join(t, "cur"), "rb").read() if os.path.exists(os.path.join(t, "cur")) else b""
first = int(c.split(b"\n")[0]) if c else 0
if c and c != seq(first, first + 300):
    print("cur is not one whole version:", first); bad += 1
if first < last:
    print("cur older than the last sync:", first, last); bad += 1
sys.exit(1 if bad else 0)
PY
        check "p: cut $r: all $last synced rounds exact; later ones whole or a prefix; the renamed file one whole version" "$([ $? = 0 ] && [ -n "$last" ] && echo ok || echo bad)"
        first=$((last + 1))        # the round the cut may have lost is done again
    done
    # the last cut's disk boots without a check, the orphan freed, and goes on
    cat >$d/k-end.txt <<'EOF'
wait zenix:/# 600
sleep 2
send mount; echo after > /t/after; sync; echo END-DONE\r
wait (?m)^END-DONE 60
sleep 1
stop
EOF
    run $d/m.fs-uae 300 $d/k-end.txt $d/end.log
    check "p: the next start: no check, no read-only, a write works" "$(! grep -aq 'fsck\|read-only' $d/end.log && grep -aq '^ide0p1 *.*zenixfs\|^ide0 *.*zenixfs' $d/end.log && grep -aq '^END-DONE' $d/end.log && echo ok || echo bad)"
    python3 -I -c '
import sys
d = open(sys.argv[1], "rb").read()
i = d.find(b"ZNFS", 512)
while i >= 0 and not (i % 512 == 32 and d[i - 28:i - 26] == b"ZB"):
    i = d.find(b"ZNFS", i + 1)
open(sys.argv[2], "wb").write(d[i - 32:])' $d/disk.hdf $d/end.img
    check "p: the PC's fsck after it, clean" "$(yes_if $Z fsck $d/end.img)"
}

# find a live inner node of an image (not the root), for damage
inner_node() {
    python3 -I - "$1" <<'PY'
import struct, sys
d = open(sys.argv[1], "rb").read()
bs = struct.unpack(">I", d[40:44])[0]; B = 1 << bs
recs = sorted((struct.unpack(">Q", d[n * B + 16:n * B + 24])[0], n) for n in range(1, 9) if d[n * B + 4:n * B + 7] == b"ZB\x02")
root = struct.unpack(">I", d[recs[-1][1] * B + 32:recs[-1][1] * B + 36])[0]
nitems = struct.unpack(">H", d[root * B + 24:root * B + 26])[0]
if d[root * B + 6] != 5:
    sys.exit(1)
ch = struct.unpack(">I", d[root * B + 32 + 16 * (nitems // 2) + 12:root * B + 32 + 16 * (nitems // 2) + 16])[0]
if d[ch * B + 6] != 5:
    sys.exit(2)
print(ch, B)
PY
}
smash() {   # IMAGE BLOCK B: garbage over one block of the root image
    dd if=/dev/urandom of="$1" bs="$3" seek="$2" count=1 conv=notrunc status=none
}
part_r() {
    local d=$OUT/r; rm -rf $d; mkdir -p $d
    # a tree of three levels: the full root at 1 KB blocks has them
    mkimg $d/root.img 15 /dev/null || { check "r: the root image" bad; return; }
    read -r node blk < <(inner_node $d/root.img) || true
    if [ -z "$node" ]; then check "r: the image has an inner node to damage" bad; return; fi
    check "r: the image has an inner node to damage (block $node of $blk bytes)" ok
    rm -rf $d/ref; mkdir -p $d/ref; $Z get $d/root.img -r / $d/ref 2>/dev/null
    cp $d/root.img $d/n.img; smash $d/n.img $node $blk
    python3 tools/mkrdb.py $d/n.img $d/n.hdf zenixfs >/dev/null
    conf $d/m1.fs-uae $d/n.hdf $BD/zenix-mount.adf
    cat >$d/k1.txt <<'EOF'
wait zenix:/# 600
sleep 2
send fsck.zenixfs ide0p1; echo rc1=$?\r
wait (?m)^rc1= 300
sleep 1
send fsck.zenixfs -y ide0p1; echo rc2=$?\r
wait (?m)^rc2= 600
sleep 1
send fsck.zenixfs ide0p1; echo rc3=$?\r
wait (?m)^rc3= 300
sleep 1
stop
EOF
    run $d/m1.fs-uae 1500 $d/k1.txt $d/1.log
    check "r: a damaged inner node: the floppy's fsck.zenixfs finds it (status 1)" "$(grep -aq 'rc1=1' $d/1.log && grep -aq 'tree block .* unreadable' $d/1.log && echo ok || echo bad)"
    check "r: fsck.zenixfs -y: new inner nodes, nothing lost, status 0" "$(grep -aq 'damaged inner tree nodes replaced' $d/1.log && grep -aq 'rc2=0' $d/1.log && grep -aq 'rc3=0' $d/1.log && echo ok || echo bad)"
    python3 -I -c '
import sys
d = open(sys.argv[1], "rb").read()
i = d.find(b"ZNFS", 512)
while i >= 0 and not (i % 512 == 32 and d[i - 28:i - 26] == b"ZB"):
    i = d.find(b"ZNFS", i + 1)
open(sys.argv[2], "wb").write(d[i - 32:])' $d/n.hdf $d/n2.img
    rm -rf $d/out; mkdir -p $d/out; $Z get $d/n2.img -r / $d/out 2>/dev/null
    check "r: the PC: fsck clean, every file of the root compares" "$($Z fsck $d/n2.img >/dev/null 2>&1 && diff -r --no-dereference $d/ref $d/out >$d/diff.log 2>&1 && echo ok || echo bad)"
    conf $d/m2.fs-uae $d/n.hdf $BD/zenix-zfs.adf
    printf 'wait zenix:/# 600\nsleep 2\nsend mount; echo boots-ok\\r\nwait (?m)^boots-ok 60\nsleep 1\nstop\n' >$d/k2.txt
    run $d/m2.fs-uae 400 $d/k2.txt $d/2.log
    check "r: ... and the disk boots" "$(has $d/2.log '^boots-ok')"
    # the superblock: the kernel stops and says so; the floppy repairs it
    cp $d/root.img $d/s.img; smash $d/s.img 0 $blk
    python3 tools/mkrdb.py $d/s.img $d/s.hdf zenixfs >/dev/null
    conf $d/m3.fs-uae $d/s.hdf $BD/zenix-zfs.adf
    printf 'waitopt fsck.zenixfs -y 240\nsleep 3\nstop\n' >$d/k3.txt
    run $d/m3.fs-uae 300 $d/k3.txt $d/3.log
    # (with no ZenixFS superblock boot cannot tell the type: then fsd says there is no filesystem)
    check "r: a destroyed superblock: boot stops with a message, it does not write" "$(grep -aq 'no writable ext2 filesystem\|cannot be mounted' $d/3.log && echo ok || echo bad)"
    conf $d/m4.fs-uae $d/s.hdf $BD/zenix-mount.adf
    printf 'wait zenix:/# 600\nsleep 2\nsend fsck.zenixfs -y ide0p1; echo rc4=$?\\r\nwait (?m)^rc4= 600\nsleep 1\nstop\n' >$d/k4.txt
    run $d/m4.fs-uae 800 $d/k4.txt $d/4.log
    check "r: the floppy repairs it: fsck.zenixfs -y, status 0" "$(grep -aq 'superblock rewritten' $d/4.log && grep -aq 'rc4=0' $d/4.log && echo ok || echo bad)"
    # two newest commit records destroyed: read-only, the choices, the shell
    read -r gen1 gen2 < <(python3 -I -c '
import struct, sys
d = open(sys.argv[1], "rb").read()
B = 1 << struct.unpack(">I", d[40:44])[0]
r = sorted(((struct.unpack(">Q", d[n * B + 16:n * B + 24])[0], n) for n in range(1, 9) if d[n * B + 4:n * B + 7] == b"ZB\x02"), reverse=True)
print(r[0][1], r[1][1])' $d/root.img)
    cp $d/root.img $d/c.img; smash $d/c.img $gen1 $blk; smash $d/c.img $gen2 $blk
    python3 tools/mkrdb.py $d/c.img $d/c.hdf zenixfs >/dev/null
    cp $d/c.hdf $d/c-before.hdf
    conf $d/m5.fs-uae $d/c.hdf $BD/zenix-zfs.adf
    cat >$d/k5.txt <<'EOF'
wait Choose \(man rescue\) 600
sleep 0.5
send 2
sleep 2
send 3
wait zenix:/# 120
sleep 1
send echo x > /nofile; echo w=$?; cat /etc/motd >/dev/null; echo r=$?; fsck.zenixfs ide0p1; echo rc=$?\r
wait (?m)^rc= 300
sleep 1
send fsck.zenixfs -y ide0p1; echo ry=$?\r
wait (?m)^ry= 300
sleep 1
stop
EOF
    run $d/m5.fs-uae 800 $d/k5.txt $d/5.log
    check "r: two newest commits destroyed: boot's choices, no read-write among them" "$(grep -aq 'mounted read-only. Choose' $d/5.log && ! grep -aq 'go on read-write anyway' $d/5.log && echo ok || echo bad)"
    check "r: a write in the rescue shell is refused; a read works" "$(grep -aq 'read-only file system' $d/5.log && grep -aq '^r=0' $d/5.log && echo ok || echo bad)"
    check "r: fsck.zenixfs -y refuses to build on this state: nothing written to the disk" "$(grep -aq 'nothing changed' $d/5.log && cmp -s $d/c.hdf $d/c-before.hdf && echo ok || echo bad)"
    # --old: on the owner's word, build on it; the shell's exit goes on read-write
    cat >$d/k6.txt <<'EOF'
wait Choose \(man rescue\) 600
sleep 0.5
send 3
wait zenix:/# 120
sleep 1
send fsck.zenixfs --old ide0p1; echo ro=$?\r
wait (?m)^ro= 300
sleep 1
send exit\r
wait the root is clean now 300
wait zenix:/# 300
sleep 2
send echo after-old > /after-old.txt; sync; cat /after-old.txt; dmesg | tail -1; echo OLD-DONE\r
wait (?m)^OLD-DONE 60
sleep 1
stop
EOF
    cp $d/c-before.hdf $d/c2.hdf
    conf $d/m6.fs-uae $d/c2.hdf $BD/zenix-zfs.adf
    run $d/m6.fs-uae 800 $d/k6.txt $d/6.log
    check "r: fsck.zenixfs --old: built on the old state with a warning, read-write after the shell, a file written" "$(grep -aq 'ro=0' $d/6.log && grep -aq '^after-old' $d/6.log && echo ok || echo bad)"
    python3 -I -c '
import sys
d = open(sys.argv[1], "rb").read()
i = d.find(b"ZNFS", 512)
while i >= 0 and not (i % 512 == 32 and d[i - 28:i - 26] == b"ZB"):
    i = d.find(b"ZNFS", i + 1)
open(sys.argv[2], "wb").write(d[i - 32:])' $d/c2.hdf $d/c2.img
    check "r: ... the PC's fsck, clean, the file there" "$($Z fsck $d/c2.img >/dev/null 2>&1 && $Z cat $d/c2.img /after-old.txt | grep -q after-old && echo ok || echo bad)"
    rm -rf $d/out2; check "r: the PC's salvage still brings the files back" "$($Z salvage $d/c.img $d/out2 >/dev/null 2>&1 && [ -f $d/out2/etc/motd ] && echo ok || echo bad)"
}

part_m() {
    local d=$OUT/m; rm -rf $d; mkdir -p $d
    mkimg $d/root.img 96 /dev/null || { check "m: the root image" bad; return; }
    python3 tools/mkrdb.py $d/root.img $d/disk.hdf zenixfs >/dev/null
    conf $d/m.fs-uae $d/disk.hdf $BD/zenix-zfs.adf 2048 0
    cat >$d/k.txt <<'EOF'
wait zenix:/# 900
sleep 3
send free; mount; ls /bin | wc -l; seq 1 3000 > /m.txt; sync; wc -l /m.txt; free; echo M-DONE\r
wait (?m)^M-DONE 300
sleep 1
stop
EOF
    run $d/m.fs-uae 1200 $d/k.txt $d/1.log
    local kb; kb=$(grep -a 'chip RAM free' $d/1.log | head -1 | grep -oE '[0-9]+' | head -1)
    echo "m: free memory, 2 MB chip alone, ZenixFS root of 2 KB blocks: $kb KB (FS-UAE)"
    check "m: boots, works, sync" "$(grep -aq '^M-DONE' $d/1.log && grep -aq '^ *3000 /m.txt' $d/1.log && echo ok || echo bad)"
    check "m: at least 900 KB free with one login" "$([ -n "$kb" ] && [ "$kb" -ge 900 ] && echo ok || echo bad)"
    # 4 KB blocks (a root over 256 MB)
    rm -f $d/big.img; PKGDB=1 RAWIMG=1 tools/mkroot-zenixfs.sh $d/big.img "$BD" "$PROGS" 300 >/dev/null
    python3 tools/mkrdb.py $d/big.img $d/big.hdf zenixfs >/dev/null
    conf $d/m2.fs-uae $d/big.hdf $BD/zenix-zfs.adf 2048 0
    run $d/m2.fs-uae 1200 $d/k.txt $d/2.log
    local kb2; kb2=$(grep -a 'chip RAM free' $d/2.log | head -1 | grep -oE '[0-9]+' | head -1)
    echo "m: free memory, 2 MB chip alone, ZenixFS root of 4 KB blocks: $kb2 KB (FS-UAE)"
    check "m: 4 KB blocks: boots, works; at least 850 KB free" "$(grep -aq '^M-DONE' $d/2.log && [ -n "$kb2" ] && [ "$kb2" -ge 850 ] && echo ok || echo bad)"
}
# tools/write-root.sh's ZenixFS mode, on a card image (no device, no sudo: its self-test mode): only p3 changes, the settings
# are carried, the way back gives the old root bit for bit; without the owner's word it refuses
part_w() {
    local d=$OUT/w; rm -rf $d; mkdir -p $d/etc
    mke2fs -q -t ext2 -E revision=0 -b 1024 -F $d/old.img 64M || { check "w: the old root" bad; return; }
    printf 'root::0:0::/root:/bin/sh\nowner::1002:100:Owner:/home/owner:/bin/sh\n' >$d/etc/passwd
    printf 'ide0p5 /home ext2 defaults\n' >$d/etc/fstab
    echo 'the owner wrote this' >$d/etc/note
    debugfs -w -f - $d/old.img >/dev/null 2>&1 <<EOF
mkdir /etc
write $d/etc/passwd /etc/passwd
write $d/etc/fstab /etc/fstab
write $d/etc/note /etc/note
EOF
    dd if=/dev/urandom of=$d/ffs.img bs=1k count=504 status=none
    python3 -I - $d/card.img $d/old.img <<'PY' || { check "w: the card image" bad; return; }
import os, struct, sys
out, old = sys.argv[1:3]
data = open(old, "rb").read()
rnd = lambda n: os.urandom(n)
mbr = bytearray(512)
parts = [(0x0c, 2048, 4096), (0x76, 8192, 4096), (0x83, 16384, len(data) // 512), (0x83, 16384 + len(data) // 512 + 2048, 4096)]
for i, (t, s, n) in enumerate(parts):
    mbr[446 + 16 * i:462 + 16 * i] = struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", t, b"\xfe\xff\xff", s, n)
mbr[510:512] = b"\x55\xaa"
with open(out, "wb") as f:
    f.write(mbr)
    for t, s, n in parts:
        f.seek(s * 512)
        f.write(data if t == 0x83 and s == 16384 else rnd(n * 512))
PY
    pget $d/card.img 1 $d/p1; pget $d/card.img 2 $d/p2; pget $d/card.img 4 $d/p4
    mkimg $d/new.img 32 /dev/null || { check "w: the new root" bad; return; }
    rc=0; ZENIX_WRITE_ROOT_SELFTEST=1 bash tools/write-root.sh $d/card.img $d/new.img $d/bk >$d/no.log 2>&1 || rc=$?
    check "w: a ZenixFS image without the owner's word: refused" "$([ $rc != 0 ] && grep -aq 'only with ZENIX_WRITE_ROOT_ZENIXFS=1' $d/no.log && echo ok || echo bad)"
    ZENIX_WRITE_ROOT_SELFTEST=1 ZENIX_WRITE_ROOT_ZENIXFS=1 bash tools/write-root.sh $d/card.img $d/new.img $d/bk >$d/yes.log 2>&1
    check "w: written, with the saved copy" "$(grep -aq '^write-root: done' $d/yes.log && ls $d/bk/zenix-root-*.img.zst >/dev/null 2>&1 && echo ok || echo bad)"
    pget $d/card.img 3 $d/p3
    check "w: p3 is the ZenixFS root, the PC's fsck clean" "$(yes_if $Z fsck $d/p3)"
    check "w: the owner's users, fstab and file carried" "$($Z cat $d/p3 /etc/passwd | grep -q '^owner:' && $Z cat $d/p3 /etc/fstab | grep -q '/home' && $Z cat $d/p3 /etc/note | grep -q 'the owner wrote' && echo ok || echo bad)"
    pget $d/card.img 1 $d/p1b; pget $d/card.img 2 $d/p2b; pget $d/card.img 4 $d/p4b
    check "w: p1, p2 and p4 byte for byte unchanged" "$(cmp -s $d/p1 $d/p1b && cmp -s $d/p2 $d/p2b && cmp -s $d/p4 $d/p4b && echo ok || echo bad)"
    ZENIX_WRITE_ROOT_SELFTEST=1 bash tools/write-root.sh $d/card.img --restore $d/bk/zenix-root-*.img.zst >$d/back.log 2>&1
    pget $d/card.img 3 $d/p3b
    check "w: --restore: the old root back, bit for bit" "$(cmp -s $d/old.img $d/p3b && echo ok || echo bad)"
}
for p in $PARTS; do part_$p; done
exit $((1 - pass))
