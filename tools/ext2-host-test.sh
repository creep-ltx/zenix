#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Host tests of the ext2 code (kernel/ext2.c + user/fsd.c, fsck.ext2, mkfs.ext2 built for the PC with tools/host/):
#   - what is mounted: modern mke2fs ext2 (1 KB and 4 KB blocks) read-write, ext3 read-only, ext4 refused, errors
#     read-only, unknown read-only feature read-only; the root rule refuses all but plain writable ext2;
#   - a workload on revision 0 and modern ext2 (fast/slow symlinks removed, hard links, an htree directory changed,
#     a big file) and host e2fsck -fn clean afterwards; the same with the small block cache of a machine without fast
#     RAM (DESIGN D18): the same answers;
#   - an open file that is unlinked keeps its data until the close, and its number is not reused meanwhile;
#   - the permission rules of audit D: set-group-id directories, a non-owner write clears set-user-id, utimes, chmod
#     and set-group-id, O_EXCL, and that raw messages from a user (not from the kernel) are refused;
#   - a rename cut by a power cut after every block write leaves at worst a leak (e2fsck -fn);
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
fsd() { HOSTDISK=$1 $T/fsd < $2 > $3 2>&1; grep '^unmarked-writes' $3 | sed "s#^#$(basename $2): #" >> $T/unmarked; }   # (D17: never)
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

# the "needs a check" mark of a restart after a crash mid-request (D11 5.4, audit A14: errno values, not -1)
cp $T/plain.img $T/uc.img; printf 'mount\nunclean\n' > $T/uc.txt; fsd $T/uc.img $T/uc.txt $T/out
grep -q "unclean -> 0" $T/out && dumpe2fs -h $T/uc.img 2>/dev/null | grep -q '^Filesystem state: *not clean' && check "the needs-a-check mark: 0, and the host sees the volume not clean" ok || { check "the needs-a-check mark: 0, and the host sees the volume not clean" bad; cat $T/out; }
cp $T/ext3.img $T/uc.img; fsd $T/uc.img $T/uc.txt $T/out
grep -q "unclean -> -30" $T/out && check "... on a read-only mount it is refused with -EROFS" ok || { check "... on a read-only mount it is refused with -EROFS" bad; cat $T/out; }
# FS_MARK (DESIGN D15: zup marks the root "needs a check" while it installs): the state before is answered ("needed no
# check": 1); FS_MARK 0 takes the mark back and the volume is clean again at once (DESIGN D17 5)
cp $T/plain.img $T/uc.img; printf 'mount\nmark 1\nmark 1\nstate\nmark 0\nstate\n' > $T/mk.txt; fsd $T/uc.img $T/mk.txt $T/out
grep -q "^mark -> 1$" $T/out && [ "$(grep -c '^mark -> 0$' $T/out)" = 2 ] && grep -q '^state -> 0 ' $T/out && grep -q '^state -> 1 ' $T/out &&
    dumpe2fs -h $T/uc.img 2>/dev/null | grep -q '^Filesystem state: *clean' && clean $T/uc.img &&
    check "FS_MARK 1: the state before (clean, then not), marked on the disk; FS_MARK 0: clean again (e2fsck -fn agrees)" ok || { check "FS_MARK 1 / 0" bad; cat $T/out; }
# a volume that was not clean when it was mounted needs a check: FS_MARK 0 does not clear that (D17 5)
printf 'mount\nmark 1\n' > $T/mk.txt; fsd $T/uc.img $T/mk.txt $T/out
printf 'mount\nmark 0\nsync\n' > $T/mk.txt; fsd $T/uc.img $T/mk.txt $T/out
grep -q "^mark -> 0$" $T/out && dumpe2fs -h $T/uc.img 2>/dev/null | grep -q '^Filesystem state: *not clean' &&
    check "FS_MARK 0 on a volume mounted not clean: it answers 0 and the volume stays marked" ok || { check "FS_MARK 0 on a volume mounted not clean" bad; cat $T/out; }
rm -f $T/uc.img

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
    cp $T/w-$img.img $T/s-$img.img                    # (the same image for the small cache's run below)
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
    cp $T/out $T/out-$img
done
# the small block cache of a machine without fast RAM (DESIGN D18: 24 KB, at least 12 blocks): the same answers, the
# same image
for img in plain plain4k; do
    HOSTDISK_NOFAST=1 fsd $T/s-$img.img $T/work.txt $T/out
    sed -i "/^ls -> /s/^\(.\{60\}\).*/\1.../" $T/out
    cmp -s $T/out $T/out-$img && clean $T/s-$img.img && check "the small cache (no fast RAM): the workload's answers the same, e2fsck -fn clean ($img)" ok ||
        { check "the small cache (no fast RAM): the workload's answers the same, e2fsck -fn clean ($img)" bad; diff $T/out-$img $T/out | head; }
done
debugfs -R "stat /idx" $T/w-plain.img 2>/dev/null | grep -q "Flags: 0x0" && check "a changed htree directory lost its index flag" ok || check "a changed htree directory lost its index flag" bad

# ---- bench's create/delete: every request is on the disk when it is answered (write-through, no sync) ----
# (the PiStorm bench showed 25 us per create on the SD card, too fast for that: this checks the server's side)
{ echo mount; echo mkdir /bench; echo writes
  for i in $(seq 10 59); do echo "open /bench/f$i wc"; echo "write %$((i-9)) 0 hello"; echo "close %$((i-9))"; done; echo writes
  for i in $(seq 10 59); do echo "unlink /bench/f$i"; done; echo writes; echo "open /bench/last wc"; echo "write %51 0 kept"; } > $T/wt.txt
cp $T/plain.img $T/wt.img
fsd $T/wt.img $T/wt.txt $T/out                  # ends without sync or quit: as if the power went
w=($(grep '^writes -> ' $T/out | awk '{print $3}'))
cs=$(( (w[1] - w[0]) / 50 )); ds=$(( (w[2] - w[1]) / 50 ))
[ "$(debugfs -R 'cat /bench/last' $T/wt.img 2>/dev/null)" = kept ] && ! debugfs -R 'ls /bench' $T/wt.img 2>/dev/null | grep -q f10 \
    && clean $T/wt.img && ! grep -qE ' -> -[0-9]' $T/out \
    && check "50 creates and deletes are on the image when answered, no sync ($cs sectors per create, $ds per delete)" ok \
    || { check "creates and deletes written through" bad; cat $T/e2fsck.log; }

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

# ---- D: permission rules of the server (kernel/fsperm.h) and who may talk to it ----
cp $T/plain.img $T/perm.img
cat > $T/perm.txt <<'EOF'
mount
mkdir /sg
chmod /sg 2777
as 1000 100
open /sg/f wc
mkdir /sg/d
stat /sg/f
stat /sg/d
as 0 0
mkdir /pub
chmod /pub 777
open /pub/prog wc
chmod /pub/prog 6777
as 1000 100
open /pub/prog w
write %3 0 changed
stat /pub/prog
utimes /pub/prog 12345
utimes /pub/prog now
as 0 0
utimes /pub/prog 12345
stat /pub/prog
as 1001 200
open /pub/own wc
as 0 0
chown /pub/own 1001 0
as 1001 200
chmod /pub/own 2755
stat /pub/own
as 0 0
open /x1 wcx
open /x1 wcx
symlink /nowhere /dangling
open /dangling wcx
lstat /dangling
as 1000 100
raw on
open /x1 r
read 12 0 4
quit
raw off
as 0 0
raw on
stat /x1
raw off
EOF
fsd $T/perm.img $T/perm.txt $T/out
st() { grep '^stat -> ' $T/out | sed -n ${1}p; }
st 1 | grep -q "gid 0 " && st 2 | grep -q "mode 42755 .* gid 0 " && check "set-group-id directory: new file and directory get its group, the directory keeps the bit" ok || { check "set-group-id directory" bad; cat $T/out; }
st 3 | grep -q "mode 100777 " && check "a write by a non-owner clears set-user-id and set-group-id" ok || { check "a write by a non-owner clears set-user-id" bad; cat $T/out; }
ut=$(grep '^utimes -> ' $T/out | tr '\n' ' ')
[ "$ut" = "utimes -> -13 utimes -> 0 utimes -> 0 " ] && st 4 | grep -q "mtime 12345$" && check "utimes: a given time only for the owner and root, now also for a writer" ok || { check "utimes rules ($ut)" bad; cat $T/out; }
st 5 | grep -q "mode 100755 " && check "chmod by a non-root owner drops set-group-id for a group not theirs" ok || { check "chmod set-group-id rule" bad; cat $T/out; }
o=$(grep '^open -> ' $T/out | sed -n '5,7p' | awk '{print $3}' | tr '\n' ' ')
case "$o" in [0-9]*" -17 -17 ") check "O_EXCL: creates once, then -17; a dangling symbolic link counts as existing" ok;; *) check "O_EXCL ($o)" bad; cat $T/out;; esac
grep -q "^lstat -> 0 .* mode 120777" $T/out && check "O_EXCL left the dangling link alone" ok || check "O_EXCL left the dangling link alone" bad
[ "$(grep '^open -> ' $T/out | sed -n 8p)" = "open -> -13" ] && grep -q "^read -> -13" $T/out && grep -q "^quit -> -13" $T/out \
    && check "raw messages from a user (not from the kernel) are refused: FS_OPEN, FS_READ by inode, FS_QUIT" ok || { check "raw messages from a user refused" bad; cat $T/out; }
tail -3 $T/out | grep -q "^stat -> 0 ino" && check "raw messages from root are served" ok || { check "raw messages from root are served" bad; cat $T/out; }
clean $T/perm.img && check "host e2fsck -fn clean after the permission test" ok || { check "e2fsck after the permission test" bad; cat $T/e2fsck.log; }

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
    4) fz_refused=$((fz_refused+1)); [ $n = 0 ] && grep -q 'nothing was changed' $T/fk.log && [ "$s0" != "$(sha256sum < $T/fk.img)" ] && fz_changed=$((fz_changed+1)) ;;
    8|1) ;;
    *) fz_crash=$((fz_crash+1)); echo "  fsck round $i: exit $rc"; grep -m3 -E "ERROR|runtime error" $T/fk.log; cp $T/fk.img /tmp/zext2-fsck-crash-$i.img ;;
    esac
done
echo "  fsck fuzz: $FR images: $fz_fixed repaired or clean, $fz_refused refused, $fz_disagree where e2fsck still found errors"
[ $fz_crash = 0 ] && [ $fz_changed = 0 ] && check "fsck fuzz: no crash, a refusal writes nothing" ok || check "fsck fuzz: $fz_crash crashes, $fz_changed refusals that wrote" bad
[ $fz_worse = 0 ] && check "fsck fuzz: a repair never leaves more for e2fsck to fix than before" ok || check "fsck fuzz: $fz_worse repairs left more problems for e2fsck" bad

# ---- a power cut in the middle of a rename (D15 step 0): stop the disk after N sectors, for every N, then e2fsck ----
# At worst a link count one too high or a leaked (nameless) inode; never a missing target, a moving file without a
# name, a count below its names, or a wrong file-type byte. Files and directories, replacing a name and a new name.
cat > $T/rc-setup.txt <<'EOF2'
mount
mkdir /d
mkdir /e
open /d/a wc
write %1 0 AAAA
open /d/b wc
write %2 0 BBBB
open /d/c wc
write %3 0 CCCC
link /d/c /e/c2
mkdir /d/sub
open /d/sub/x wc
write %4 0 XXXX
EOF2
rm -f $T/rc-base.img; mke2fs -q -F -t ext2 -b 1024 $T/rc-base.img 4M || exit 1
fsd $T/rc-base.img $T/rc-setup.txt $T/out
clean $T/rc-base.img || { check "rename power-cut test image" bad; cat $T/out $T/e2fsck.log; }
ino_of() { debugfs -R "stat $2" "$1" 2>/dev/null | sed -n 's/^Inode: \([0-9]*\).*/\1/p'; }
rc_bad=0; rc_runs=0
for cs in "file replaces a file:/d/a:/d/b" "file replaces a file in another directory:/d/a:/e/b" \
          "file replaces a file with two links:/d/a:/d/c" "file to a new name in another directory:/d/a:/e/new" \
          "directory to a new name:/d/sub:/d/sub2" "directory replaces a file:/d/sub:/d/b"; do
    what=${cs%%:*}; rest=${cs#*:}; src=${rest%%:*}; dst=${rest#*:}
    sino=$(ino_of $T/rc-base.img $src); tino=$(ino_of $T/rc-base.img $dst)
    n=0; done_=0
    while [ $done_ = 0 ] && [ $n -le 40 ]; do
        cp $T/rc-base.img $T/rc.img
        printf 'mount\ncut %s\nrename %s %s\nwrites\n' $n $src $dst > $T/rc.txt
        fsd $T/rc.img $T/rc.txt $T/rc.out          # finished: the rename said 0 and no write was refused
        lim=$(sed -n 's/^cut -> //p' $T/rc.out); wr=$(sed -n 's/^writes -> //p' $T/rc.out)
        grep -q "^rename -> 0" $T/rc.out && [ "$wr" -lt "$lim" ] && done_=1
        rc_runs=$((rc_runs+1))
        e2fsck -fn $T/rc.img > $T/rc-e2.log 2>&1; e2rc=$?
        s_now=$(ino_of $T/rc.img $src); d_now=$(ino_of $T/rc.img $dst)
        why=$(python3 -I - $T/rc-e2.log "$sino" "$tino" "$s_now" "$d_now" $done_ $e2rc <<'PY'
import re, sys
log, sino, tino, s_now, d_now, done, e2rc = open(sys.argv[1]).read(), *sys.argv[2:]
bad = []
for i, have, want in re.findall(r"Inode (\d+) ref count is (\d+), should be (\d+)", log):
    if int(have) < int(want): bad.append("inode %s count %s below its %s names" % (i, have, want))
for pat in ("deleted/unused inode", "Multiply-claimed", "incorrect filetype", "bad inode", "Unconnected directory"):
    if pat in log: bad.append("e2fsck: " + pat)
if sino not in (s_now, d_now): bad.append("the moving inode has no name")
if tino and d_now not in (sino, tino): bad.append("the target name is missing or wrong")
if done == "1" and (e2rc != "0" or d_now != sino or s_now): bad.append("the finished rename is not clean")
print("; ".join(bad))
PY
)
        [ -n "$why" ] && { rc_bad=$((rc_bad+1)); echo "  $what, cut after $n sectors: $why"; grep -E "count|Entry|Unattached|filetype" $T/rc-e2.log | head -5; }
        n=$((n+2))
    done
    [ $done_ = 1 ] || { rc_bad=$((rc_bad+1)); echo "  $what: never finished"; cat $T/rc.out; }
done
[ $rc_bad = 0 ] && check "rename cut by a power cut after every block write ($rc_runs runs, 6 cases): e2fsck finds at worst a leak" ok || check "rename power cuts: $rc_bad bad states" bad

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

# ---- the clean mark (DESIGN D17, audit 2026-10-26 F6) ----
# st IMG: the state word's bit 0 on the image (1 clean)
st() { dumpe2fs -h "$1" 2>/dev/null | grep -q '^Filesystem state: *clean' && echo 1 || echo 0; }
bits() { sed -n 's/^state -> \([0-9]\).*/\1/p' "$1" | tr -d '\n'; }
cp $T/plain.img $T/cm.img
printf 'mount\nstate\nopen /a r\nstat /\nls /\nstate\nopen /f wc\nstate\nwrite %%1 0 hello\nsync\nstate\n' > $T/cm.txt
fsd $T/cm.img $T/cm.txt $T/out
[ "$(bits $T/out)" = "1101" ] && [ "$(st $T/cm.img)" = 1 ] && clean $T/cm.img &&
    check "the mark: reads leave the volume clean, the first change marks it on the disk, sync makes it clean again" ok || { check "the mark: reads, a change, sync" bad; cat $T/out; }
printf 'mount\nopen /g wc\nwrite %%1 0 data\nstate\nidle\nstate\n' > $T/cm.txt; fsd $T/cm.img $T/cm.txt $T/out
[ "$(bits $T/out)" = "01" ] && [ "$(st $T/cm.img)" = 1 ] &&
    check "the mark: the quiet moment after a change makes it clean again" ok || { check "the mark: the quiet moment" bad; cat $T/out; }
printf 'mount\nopen /o wc\nwrite %%1 0 orphan\nunlink /o\nsync\nidle\nstate\nclose %%1\nsync\nstate\n' > $T/cm.txt; fsd $T/cm.img $T/cm.txt $T/out
[ "$(sed -n 's/^state -> \([0-9]\) .* orphans \([0-9]\).*/\1\2/p' $T/out | tr -d '\n')" = "0110" ] && [ "$(st $T/cm.img)" = 1 ] && clean $T/cm.img &&
    check "the mark: an open file whose last name went keeps the volume marked through sync, until its close" ok || { check "the mark: an open orphan" bad; cat $T/out; }
cp $T/cm.img $T/cm-o.img; printf 'mount\nopen /o2 wc\nwrite %%1 0 orphan\nunlink /o2\nsync\n' > $T/cm.txt; fsd $T/cm-o.img $T/cm.txt $T/out
[ "$(st $T/cm-o.img)" = 0 ] && ! e2fsck -fn $T/cm-o.img >/dev/null 2>&1 &&
    check "... a cut with the orphan open: e2fsck finds it, and the volume says not clean" ok || { check "... a cut with the orphan open" bad; cat $T/out; }
cp $T/cm.img $T/cm-e.img; printf 'mount\ncut 0\nopen /h wc\nsync\nstate\n' > $T/cm.txt; fsd $T/cm-e.img $T/cm.txt $T/out
grep -q '^open -> -5$' $T/out && grep -q '^state -> 1 marked 0 need 1' $T/out && [ "$(st $T/cm-e.img)" = 1 ] && clean $T/cm-e.img &&
    check "the mark: when the mark cannot be written the request changes nothing (-EIO), and fsd keeps a need for a check" ok || { check "the mark: a mark that cannot be written" bad; cat $T/out; }
cp $T/cm.img $T/cm-f.img; printf 'mount\nmkdir /n\nsync\ncut 2\nopen /n/x wc\nwrite %%1 0 @5000\nsync\nidle\nstate\n' > $T/cm.txt; fsd $T/cm-f.img $T/cm.txt $T/out
[ "$(st $T/cm-f.img)" = 0 ] && grep -q '^state -> 0 marked 1 need 1' $T/out &&
    check "the mark: a block write that fails in the middle of a request: the volume stays marked, sync and the quiet moment do not clean it" ok || { check "the mark: a failed write mid-request" bad; cat $T/out; }
cp $T/cm.img $T/cm-m.img; printf 'mount\nmark 1\ncut 0\nopen /z wc\nmark 0\nsync\nidle\nstate\n' > $T/cm.txt; fsd $T/cm-m.img $T/cm.txt $T/out
[ "$(st $T/cm-m.img)" = 0 ] && grep -q '^mark -> 1$' $T/out &&
    check "FS_MARK 0 does not clean a volume that needs a check for another reason (marked by a crash during the install)" ok || { check "FS_MARK 0 after a crash mark" bad; cat $T/out; }

# a workload cut by a power cut after every block write: wherever e2fsck finds anything, the volume says not clean
# (D17's invariant, with quiet moments and syncs inside the workload). On the old code the volume always said clean.
cat > $T/wl.txt <<'EOF'
mount
mkdir /w
open /w/a wc
write %1 0 @3000
close %1
idle
open /w/b wc
write %2 0 @20000
link /w/b /w/c
rename /w/a /w/d
sync
symlink /w/d /w/s
open /w/d rw
unlink /w/d
write %3 0 more
close %3
idle
open /w/b wt
write %4 0 short
unlink /w/c
mkdir /w2
rename /w/b /w2/b
unlink /w/s
rmdir /w
idle
writes
EOF
mke2fs -q -F -t ext2 -b 1024 -N 128 $T/wl-base.img 2M
cp $T/wl-base.img $T/wl.img; HOSTDISK=$T/wl.img $T/fsd < $T/wl.txt > $T/wl.out 2>&1
total=$(sed -n 's/^writes -> //p' $T/wl.out); wl_bad=0; wl_cuts=0; wl_leaks=0
clean $T/wl.img && [ "$(st $T/wl.img)" = 1 ] || { check "the workload without a cut: e2fsck clean, the state clean" bad; cat $T/wl.out; }
fk_bad=0; fk_runs=0; fk_cut_bad=0; fk_cut_runs=0
for n in $(seq 0 2 $((total + 2))); do
    cp $T/wl-base.img $T/wl.img
    { echo mount; echo "cut $n"; sed 1d $T/wl.txt; } > $T/wlc.txt
    HOSTDISK=$T/wl.img $T/fsd < $T/wlc.txt > $T/wlc.out 2>&1
    grep -h '^unmarked-writes' $T/wlc.out >> $T/unmarked
    wl_cuts=$((wl_cuts + 1))
    e2fsck -fn $T/wl.img > $T/wl-e2.log 2>&1 && continue
    wl_leaks=$((wl_leaks + 1))
    [ "$(st $T/wl.img)" = 0 ] || { wl_bad=$((wl_bad + 1)); echo "  cut after $n sectors: e2fsck finds something on a volume that says clean"; }
    # fsck.ext2 -y repairs it in one run and marks it clean (D17 4)
    cp $T/wl.img $T/wl-cut.img
    fk_runs=$((fk_runs + 1))
    HOSTDISK=$T/wl.img $T/fsck -y hd0 > $T/fk1.log 2>&1; rc=$?
    { [ $rc -le 1 ] && e2fsck -fn $T/wl.img > $T/fk-e2.log 2>&1 && [ "$(st $T/wl.img)" = 1 ]; } ||
        { fk_bad=$((fk_bad + 1)); echo "  workload cut after $n: fsck.ext2 -y status $rc, state $(st $T/wl.img)"; }
    # a cut inside fsck.ext2's own repair, after each of its writes (the first few leaky images): never marked clean
    # early, and the next run finishes it
    [ $fk_runs -le 5 ] || continue
    fw=$(HOSTDISK_COUNT=1 HOSTDISK=$T/wl-cut.img $T/fsck -y hd0 2>&1 | sed -n 's/^hostdisk-writes //p'); cp $T/wl-cut.img $T/wl.img
    for k in $(seq 0 2 $((fw + 2))); do
        cp $T/wl-cut.img $T/wl.img
        HOSTDISK=$T/wl.img HOSTDISK_CUT=$k $T/fsck -y hd0 > /dev/null 2>&1
        fk_cut_runs=$((fk_cut_runs + 1))
        if [ "$(st $T/wl.img)" = 1 ] && ! e2fsck -fn $T/wl.img >/dev/null 2>&1; then fk_cut_bad=$((fk_cut_bad + 1)); echo "  fsck cut after $k sectors: marked clean, but not clean"; fi
        HOSTDISK=$T/wl.img $T/fsck -y hd0 > $T/fk2.log 2>&1; rc=$?
        { [ $rc -le 1 ] && e2fsck -fn $T/wl.img >/dev/null 2>&1 && [ "$(st $T/wl.img)" = 1 ]; } || { fk_cut_bad=$((fk_cut_bad + 1)); echo "  fsck cut after $k sectors: the next run did not finish it (status $rc)"; }
    done
done
[ -n "$total" ] && [ $wl_cuts -gt 20 ] && [ $wl_leaks -gt 3 ] && [ $wl_bad = 0 ] &&
    check "a workload cut after each of its $total sectors written ($wl_cuts cuts, $wl_leaks left something): never on a volume that says clean" ok ||
    check "workload cuts: $wl_bad left something on a volume that says clean ($wl_cuts cuts, $wl_leaks leaky)" bad
[ $fk_runs -gt 3 ] && [ $fk_bad = 0 ] &&
    check "fsck.ext2 -y repairs each of the $fk_runs leaky cuts in one run: e2fsck -fn clean, the state clean" ok || check "fsck.ext2 -y after workload cuts: $fk_bad of $fk_runs not repaired in one run" bad
[ $fk_cut_runs -gt 10 ] && [ $fk_cut_bad = 0 ] &&
    check "fsck.ext2 cut after each of its repair writes ($fk_cut_runs cuts): never marked clean early, the next run finishes" ok || check "fsck.ext2 cut inside its repair: $fk_cut_bad bad of $fk_cut_runs" bad
# every block write fsd made in all of these tests was inside the mark (fs_blk_write counts the others)
[ ! -s $T/unmarked ] && check "every block write fsd made in these tests came after the mark (a host-build count)" ok || { check "block writes outside the mark" bad; sort $T/unmarked | uniq -c | head; }

echo "ext2-host-test: $pass passed, $fail failed"
[ $fail = 0 ] || exit 1
exit 0
