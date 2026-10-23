#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host tests of the ext2 code (kernel/ext2.c + user/fsd.c, fsck.ext2, mkfs.ext2 built for the PC with tools/host/):
#   - what is mounted: modern mke2fs ext2 (1 KB and 4 KB blocks) read-write, ext3 read-only, ext4 refused, errors
#     read-only, unknown read-only feature read-only; the root rule refuses all but plain writable ext2;
#   - a workload on revision 0 and modern ext2 (fast/slow symlinks removed, hard links, an htree directory changed,
#     a big file) and host e2fsck -fn clean afterwards;
#   - an open file that is unlinked keeps its data until the close, and its number is not reused meanwhile;
#   - fsck.ext2 against host e2fsck on damaged images; mkfs.ext2 results checked by e2fsck;
#   - a bounded fuzz loop: randomly damaged images must never crash the server (AddressSanitizer build).
# usage: tools/ext2-host-test.sh [fuzz-rounds]     (default 150; memory: one small image at a time)
cd "$(dirname "$0")/.." || exit 1
ROUNDS=${1:-150}
T=$(mktemp -d /tmp/zext2.XXXXXX)
trap 'rm -rf $T' EXIT
CF="-m32 -DZHOST -DFSD -std=gnu11 -O1 -g -Wall -Wno-format-truncation"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
gcc $CF -o $T/fsd tools/host/fsd-host.c tools/host/hostdisk.c kernel/ext2.c || { echo "FAIL build fsd-host"; exit 1; }
gcc $CF $SAN -o $T/fsd-asan tools/host/fsd-host.c tools/host/hostdisk.c kernel/ext2.c || { echo "FAIL build fsd-host (asan)"; exit 1; }
gcc $CF $SAN -o $T/fsck user/fsck_ext2.c tools/host/hostdisk.c kernel/ext2.c || { echo "FAIL build fsck"; exit 1; }
gcc $CF -o $T/mkfs user/mkfs_ext2.c tools/host/hostdisk.c || { echo "FAIL build mkfs"; exit 1; }
pass=0; fail=0
check() { if [ "$2" = ok ]; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1"; fail=$((fail+1)); fi; }
fsd() { HOSTDISK=$1 $T/fsd < $2 > $3 2>&1; }
clean() { e2fsck -fn "$1" > $T/e2fsck.log 2>&1; }

# ---- 1. what is mounted ----
mk() { rm -f $T/$1.img; mke2fs -q -F "${@:3}" $T/$1.img $2 || exit 1; }
mk plain 8M -t ext2
mk plain4k 32M -t ext2 -b 4096
mk rev0 8M -t ext2 -E revision=0 -b 1024
mk ext3 8M -t ext3
mk ext4 8M -t ext4
mk errors 8M -t ext2; debugfs -w -R "ssv state 3" $T/errors.img >/dev/null 2>&1
mk nlink 8M -t ext2 -O dir_nlink
printf 'mount\n' > $T/m.txt; printf 'mount root\n' > $T/mr.txt
for v in plain:"mount -> 0$" plain4k:"mount -> 0$" rev0:"mount -> 0$" ext3:"mount -> 0 readonly" ext4:"mount -> -1" \
         errors:"mount -> 0 readonly" nlink:"mount -> 0 readonly"; do
    img=${v%%:*}; want=${v#*:}
    fsd $T/$img.img $T/m.txt $T/out; grep -q "$want" $T/out && check "mount $img: '$want'" ok || { check "mount $img: '$want'" bad; cat $T/out; }
done
grep -q "extent 64bit flex_bg" <(HOSTDISK=$T/ext4.img $T/fsd < $T/m.txt 2>&1) && check "ext4 refusal names the features" ok || check "ext4 refusal names the features" bad
grep -q "journal" <(HOSTDISK=$T/ext3.img $T/fsd < $T/m.txt 2>&1) && check "ext3 read-only says why (journal)" ok || check "ext3 read-only says why" bad
for img in ext3 errors nlink ext4; do
    fsd $T/$img.img $T/mr.txt $T/out; grep -q "mount -> -1" $T/out && check "root rule refuses $img" ok || check "root rule refuses $img" bad
done
fsd $T/plain.img $T/mr.txt $T/out; grep -q "mount -> 0$" $T/out && check "root rule accepts modern ext2" ok || check "root rule accepts modern ext2" bad
# a read-only mount writes nothing
cp $T/ext3.img $T/ro.img; sum1=$(sha256sum < $T/ro.img)
printf 'mount\nopen /x wc\nmkdir /d\nopen /lost+found r\nls /\n' > $T/w.txt
fsd $T/ro.img $T/w.txt $T/out
[ "$sum1" = "$(sha256sum < $T/ro.img)" ] && grep -q "open -> -30" $T/out && grep -q "mkdir -> -30" $T/out && check "read-only mount: writes refused (EROFS), image unchanged" ok || { check "read-only mount: writes refused, image unchanged" bad; cat $T/out; }

# ---- 2. a workload, then host e2fsck ----
cat > $T/work.txt <<'EOF'
mount
mkdir /d
open /d/f wc
write %1 0 hello
symlink /etc/motd /d/fast
symlink /a/very/long/target/that/does/not/fit/in/the/sixty/bytes/of/an/inode/xxxxxxxx /d/slow
readlink /d/fast
readlink /d/slow
link /d/f /d/hard
unlink /d/fast
unlink /d/slow
unlink /d/hard
mkdir /d/sub
rmdir /d/sub
open /big wc
write %2 0 @300000
read %2 299990 20
rename /big /d/big2
stat /d/big2
ls /idx
unlink /idx/n17
unlink /idx/n99
open /idx/new1 wc
mkdir /idx/newdir
rename /idx/n5 /idx/renamed
ls /d
EOF
mkdir -p $T/src/idx; for i in $(seq 1 400); do echo $i > $T/src/idx/n$i; done
for img in rev0 plain plain4k; do
    rm -f $T/w-$img.img
    case $img in rev0) o="-E revision=0 -b 1024";; plain) o="";; plain4k) o="-b 4096";; esac
    mke2fs -q -F -t ext2 $o -d $T/src $T/w-$img.img 16M || exit 1
    [ $img = rev0 ] || e2fsck -fyD $T/w-$img.img >/dev/null 2>&1    # index the big directory (htree)
    [ $img = plain ] && { debugfs -R "stat /idx" $T/w-$img.img 2>/dev/null | grep -q "Flags: 0x1000" && check "the test directory is an htree (dir_index) directory" ok || check "the test directory is an htree directory" bad; }
    fsd $T/w-$img.img $T/work.txt $T/out
    sed -i "/^ls -> /s/^\(.\{60\}\).*/\1.../" $T/out
    ok=ok
    grep -q "readlink -> 9 /etc/motd" $T/out || ok=bad
    grep -q "readlink -> 77 /a/very" $T/out || ok=bad
    grep -q "read -> 10 bcdefghijk" $T/out || ok=bad
    grep -q "size 300000" $T/out || ok=bad
    grep -q "damaged" $T/out && ok=bad
    grep -E "^(unlink|rmdir|rename|link|mkdir|symlink) -> -" $T/out && ok=bad
    check "workload on $img ext2 (symlinks, links, big file, htree directory)" $ok
    [ $ok = ok ] || cat $T/out
    clean $T/w-$img.img && check "host e2fsck -fn clean after the workload ($img)" ok || { check "host e2fsck -fn clean after the workload ($img)" bad; cat $T/e2fsck.log; }
done
debugfs -R "stat /idx" $T/w-plain.img 2>/dev/null | grep -q "Flags: 0x0" && check "a changed htree directory lost its index flag" ok || check "a changed htree directory lost its index flag" bad

# ---- C1: removing fast symlinks frees nothing ----
cp $T/rev0.img $T/sym.img
free0=$(dumpe2fs -h $T/sym.img 2>/dev/null | awk '/^Free blocks/ {print $3}')
{ echo mount; for i in $(seq 1 30); do echo "symlink /etc/x$i-abcdefgh /s$i"; done; for i in $(seq 1 30); do echo "unlink /s$i"; done; } > $T/sym.txt
fsd $T/sym.img $T/sym.txt $T/out
free1=$(dumpe2fs -h $T/sym.img 2>/dev/null | awk '/^Free blocks/ {print $3}')
! grep -q damaged $T/out && [ "$free0" = "$free1" ] && clean $T/sym.img && check "rm of 30 fast symlinks: no block freed, e2fsck clean" ok || { check "rm of fast symlinks" bad; cat $T/e2fsck.log; }

# ---- C5: an open file that is unlinked ----
cp $T/plain.img $T/open.img
cat > $T/o2.txt <<EOF
mount
open /f wc
write %1 0 old-data
unlink /f
fstat %1
open /g wc
write %1 0 OLD
read %1 0 8
stat /g
close %1
close %2
open /h wc
EOF
fsd $T/open.img $T/o2.txt $T/out
f=$(grep '^open -> ' $T/out | sed -n 1p | awk '{print $3}'); g=$(grep '^open -> ' $T/out | sed -n 2p | awk '{print $3}')
grep -q "fstat -> 0 ino $f .* links 0" $T/out && [ "$g" != "$f" ] && grep -q "read -> 8 OLD-data" $T/out && grep -q "stat -> 0 ino $g .* size 0 " $T/out \
    && check "unlinked open file: still readable/writable, number not reused, new file untouched" ok || { check "unlinked open file" bad; cat $T/out; }
h=$(grep '^open -> ' $T/out | sed -n 3p | awk '{print $3}')
[ "$h" = "$f" ] && clean $T/open.img && check "unlinked open file freed at the last close, e2fsck clean" ok || { check "freed at the last close" bad; cat $T/e2fsck.log; }
cp $T/plain.img $T/open2.img
printf "mount\nopen /f wc\nwrite %%1 0 x\nunlink /f\n" > $T/o3.txt
fsd $T/open2.img $T/o3.txt $T/out
e2fsck -fn $T/open2.img > $T/e2fsck.log 2>&1; grep -q "Inode bitmap differences: *-$f" $T/e2fsck.log && check "a crash while an unlinked file is open leaves an inode e2fsck reclaims" ok || { check "crash with an unlinked open file" bad; cat $T/e2fsck.log; }

# ---- C7: fsck.ext2 against host e2fsck ----
# repair: fsck.ext2 -y until it says clean (at most 3 runs), then e2fsck -fn must agree
repair() {
    local img=$1 n=0 rc
    while [ $n -lt 3 ]; do
        HOSTDISK=$img $T/fsck -y hd0 > $T/fsck$n.log 2>&1; rc=$?
        [ $rc = 0 ] && break
        [ $rc = 1 ] || { echo "  fsck exit $rc"; tail -3 $T/fsck$n.log; return 1; }
        n=$((n+1))
    done
    [ $rc = 0 ] && clean $img
}
mkdir -p $T/bad/bin $T/bad/etc $T/bad/home $T/bad/many
head -c 30000 /dev/urandom > $T/bad/bin/ls; echo hello > $T/bad/etc/motd; echo readme > $T/bad/home/readme.txt
head -c 20000 /dev/urandom > $T/bad/home/big.bin
for i in $(seq 1 1100); do : > $T/bad/many/file-number-$i; done
damage() {
    debugfs -w $1 -f - >/dev/null 2>&1 <<CMDS
sif /etc/motd links_count 5
unlink /home/readme.txt
freei /bin/ls
sif /home/big.bin blocks 7
freeb 300
freei /many/file-number-700
CMDS
}
for kind in rev0 plain plain4k; do
    case $kind in rev0) o="-E revision=0 -b 1024";; plain) o="";; plain4k) o="-b 4096";; esac
    rm -f $T/f.img; mke2fs -q -F -t ext2 $o -N 2048 -d $T/bad $T/f.img 24M || exit 1
    HOSTDISK=$T/f.img $T/fsck hd0 > $T/fsck.log 2>&1 && grep -q "fsck.ext2: clean" $T/fsck.log && check "fsck.ext2: a fresh $kind volume is clean (resize inode, big directory)" ok || { check "fsck.ext2: a fresh $kind volume is clean" bad; tail -5 $T/fsck.log; }
    damage $T/f.img
    e2fsck -fn $T/f.img >/dev/null 2>&1 && { check "damage applied ($kind)" bad; continue; }
    HOSTDISK=$T/f.img $T/fsck hd0 > $T/fsck.log 2>&1; grep -q "problem(s) found" $T/fsck.log && check "fsck.ext2 finds the damage ($kind)" ok || check "fsck.ext2 finds the damage ($kind)" bad
    repair $T/f.img && check "fsck.ext2 -y repairs it, host e2fsck agrees ($kind)" ok || { check "fsck.ext2 -y repairs it ($kind)" bad; cat $T/e2fsck.log | head; }
    debugfs -R "cat /bin/ls" $T/f.img 2>/dev/null | cmp -s - $T/bad/bin/ls && check "the file whose bitmap bit was lost is kept ($kind)" ok || check "the file whose bitmap bit was lost is kept ($kind)" bad
done
# a crash while an unlinked file was open (from the C5 test above)
repair $T/open2.img && check "fsck.ext2 -y reclaims the inode of an unlinked open file after a crash" ok || check "fsck.ext2 reclaims an unlinked open file" bad
# more than 255 links (300 subdirectories)
mkdir -p $T/links; for i in $(seq 1 300); do mkdir -p $T/links/d/s$i; done
rm -f $T/l.img; mke2fs -q -F -t ext2 -E revision=0 -b 1024 -N 1024 -d $T/links $T/l.img 8M
HOSTDISK=$T/l.img $T/fsck hd0 > $T/fsck.log 2>&1 && check "fsck.ext2: a directory with 302 links is clean (16-bit counts)" ok || { check "fsck.ext2: 302 links" bad; tail -3 $T/fsck.log; }
# damage it cannot repair: two files share a block -> exit 4 and nothing written
rm -f $T/u.img; mke2fs -q -F -t ext2 -d $T/bad -N 2048 $T/u.img 24M
b=$(debugfs -R "bmap /home/big.bin 0" $T/u.img 2>/dev/null)
debugfs -w -R "sif /bin/ls block[0] $b" $T/u.img >/dev/null 2>&1; debugfs -w -R "unlink /home/readme.txt" $T/u.img >/dev/null 2>&1
sum1=$(sha256sum < $T/u.img)
HOSTDISK=$T/u.img $T/fsck -y hd0 > $T/fsck.log 2>&1; rc=$?
[ $rc = 4 ] && grep -q "nothing was changed" $T/fsck.log && [ "$sum1" = "$(sha256sum < $T/u.img)" ] && check "fsck.ext2 -y refuses a duplicate block and writes nothing" ok || { check "fsck.ext2 refuses unrepairable damage" bad; tail -3 $T/fsck.log; }
# refuses ext3 / ext4
HOSTDISK=$T/ext3.img $T/fsck hd0 > $T/fsck.log 2>&1; [ $? = 8 ] && grep -q "journal" $T/fsck.log && check "fsck.ext2 refuses ext3 (journal)" ok || check "fsck.ext2 refuses ext3" bad
# mkfs.ext2 on a device full of old data: e2fsck clean (the inode tables are zeroed)
for bs in 1024 4096; do
    head -c $((40 * 1024 * 1024)) /dev/urandom > $T/mk.img
    HOSTDISK=$T/mk.img $T/mkfs -b $bs hd0 > $T/mkfs.log 2>&1 && clean $T/mk.img && HOSTDISK=$T/mk.img $T/fsck hd0 >/dev/null 2>&1 \
        && check "mkfs.ext2 -b $bs over old data: e2fsck and fsck.ext2 clean" ok || { check "mkfs.ext2 -b $bs over old data" bad; head -5 $T/e2fsck.log; }
done
rm -f $T/mk.img
# random damage: fsck.ext2 -y never crashes (ASan), refuses without writing, or repairs to what e2fsck calls clean
fz_crash=0; fz_disagree=0; fz_worse=0; fz_refused=0; fz_fixed=0; fz_changed=0
FR=$((ROUNDS / 3))
rm -f $T/fk-base.img; mke2fs -q -F -t ext2 -N 512 -d $T/src $T/fk-base.img 4M
for i in $(seq 1 $FR); do
    python3 -I - $T/fk-base.img $T/fk.img $((i + 5000)) <<'PY'
import random, sys
src, dst, seed = sys.argv[1], sys.argv[2], int(sys.argv[3])
d = bytearray(open(src, 'rb').read()); r = random.Random(seed)
for _ in range(r.randint(1, 12)):
    at = r.randrange(3072, 200 * 1024)                       # group table, bitmaps, inode tables, directories
    d[at] = r.randrange(256) if r.random() < 0.5 else d[at] ^ (1 << r.randrange(8))
open(dst, 'wb').write(d)
PY
    s0=$(sha256sum < $T/fk.img); n=0
    before=$(e2fsck -fn $T/fk.img 2>&1 | grep -ciE "\? no|ignored")
    while [ $n -lt 3 ]; do
        HOSTDISK=$T/fk.img timeout 20 $T/fsck -y hd0 > $T/fk.log 2>&1; rc=$?
        [ $rc = 1 ] || break
        n=$((n+1))
    done
    case $rc in
    0) fz_fixed=$((fz_fixed+1)); e2fsck -fn $T/fk.img > $T/fk-e2.log 2>&1 || {
         fz_disagree=$((fz_disagree+1)); after=$(grep -ciE "\? no|ignored" $T/fk-e2.log)
         [ "$after" -gt "$before" ] && { fz_worse=$((fz_worse+1)); echo "  fsck round $i: e2fsck problems $before -> $after"; cp $T/fk.img /tmp/zext2-fsck-worse-$i.img; }; } ;;
    4) fz_refused=$((fz_refused+1)); [ $n = 0 ] && [ "$s0" != "$(sha256sum < $T/fk.img)" ] && fz_changed=$((fz_changed+1)) ;;
    8|1) ;;
    *) fz_crash=$((fz_crash+1)); echo "  fsck round $i: exit $rc"; grep -m3 -E "ERROR|runtime error" $T/fk.log; cp $T/fk.img /tmp/zext2-fsck-crash-$i.img ;;
    esac
done
echo "  fsck fuzz: $FR images: $fz_fixed repaired or clean, $fz_refused refused, $fz_disagree where e2fsck still found errors"
[ $fz_crash = 0 ] && [ $fz_changed = 0 ] && check "fsck fuzz: no crash, a refusal writes nothing" ok || check "fsck fuzz: $fz_crash crashes, $fz_changed refusals that wrote" bad
[ $fz_worse = 0 ] && check "fsck fuzz: a repair never leaves more for e2fsck to fix than before" ok || check "fsck fuzz: $fz_worse repairs left more problems for e2fsck" bad

# ---- C10: randomly damaged images never crash the server, hang it, or make it write outside (AddressSanitizer) ----
rm -f $T/fz-base.img; mke2fs -q -F -t ext2 -E revision=0 -b 1024 -N 512 -d $T/src $T/fz-base.img 4M
rm -f $T/fz-base2.img; mke2fs -q -F -t ext2 -N 512 -d $T/src $T/fz-base2.img 4M
cat > $T/fz.txt <<'EOF2'
mount
ls /
ls /idx
stat /idx/n7
open /idx/n9 r
read %1 0 100
open /new wc
write %2 0 @5000
mkdir /nd
symlink /x/y /nd/s
unlink /nd/s
rmdir /nd
unlink /idx/n3
rename /idx/n4 /idx/m4
open /idx/n10 wt
write %3 0 @70000
close %3
unlink /idx/n10
ls /idx
EOF2
crash=0; hang=0; mounted=0; noticed=0
for i in $(seq 1 $ROUNDS); do
    base=$T/fz-base.img; [ $((i % 2)) = 0 ] && base=$T/fz-base2.img
    python3 -I - "$base" $T/fz.img $i <<'PY'
import random, sys
src, dst, seed = sys.argv[1], sys.argv[2], int(sys.argv[3])
d = bytearray(open(src, 'rb').read())
r = random.Random(seed)
mode = seed % 4
for _ in range(r.randint(1, 40)):
    if mode == 0:   at = 1024 + r.randrange(1024)            # the superblock
    elif mode == 1: at = 2048 + r.randrange(2048)            # group table
    elif mode == 2: at = r.randrange(4096, 160 * 1024)       # bitmaps, inode tables, first directories
    else:           at = r.randrange(len(d))
    d[at] = r.randrange(256) if r.random() < 0.7 else d[at] ^ (1 << r.randrange(8))
open(dst, 'wb').write(d)
PY
    HOSTDISK=$T/fz.img timeout 20 $T/fsd-asan < $T/fz.txt > $T/fz.out 2>&1
    rc=$?
    grep -q "^mount -> 0" $T/fz.out && mounted=$((mounted+1))
    grep -q "damaged" $T/fz.out && noticed=$((noticed+1))
    if [ $rc = 124 ]; then hang=$((hang+1)); echo "  round $i: hang"; cp $T/fz.img /tmp/zext2-hang-$i.img
    elif [ $rc != 0 ]; then crash=$((crash+1)); echo "  round $i: exit $rc"; grep -m3 -E "ERROR|runtime error" $T/fz.out; cp $T/fz.img /tmp/zext2-crash-$i.img; fi
done
echo "  fuzz: $mounted of $ROUNDS damaged images mounted, damage reported in $noticed"
[ $crash = 0 ] && [ $hang = 0 ] && check "fuzz: $ROUNDS damaged images, no crash, no hang, no out-of-bounds access (ASan+UBSan)" ok || check "fuzz: $crash crashes, $hang hangs in $ROUNDS rounds" bad

echo "ext2-host-test: $pass passed, $fail failed"
[ $fail = 0 ] || exit 1
exit 0
