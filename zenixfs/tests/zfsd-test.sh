#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The ZenixFS server (user/zenixfsd.c) on the PC: tools/host/fsd-host.c built with -DZFSD drives it on an image made by
# zenixfs-tool. Checks DESIGN D26's FS_LINKED answer (docs/d26-links-across-mounts.md 4, 10): every request of the
# table with UMSG_LINKS, TEXT and the bits, S and E of a parent walk, absolute and relative targets, a TEXT too long,
# a 256-byte target, nothing changed before the answer, FS_SYMLINK never rewrites a0, permissions before the link,
# and today's answers without the bit or for a relative path. (The empty and NUL targets are fsl_text's, the same
# code as the ext2 server's: tools/ext2-host-test.sh.)
# usage: bash tests/zfsd-test.sh build/zenixfs-tool    (from zenixfs/, as `make test` runs it)
TOOL=$(realpath "${1:-build/zenixfs-tool}")
cd "$(dirname "$0")/../.." || exit 1
T=$(mktemp -d /tmp/zfsdt.XXXXXX)
trap 'rm -rf $T' EXIT
gcc -m32 -DZHOST -DZFSD -std=gnu11 -O1 -g -Wall -Wno-format-truncation -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o $T/zfsd tools/host/fsd-host.c tools/host/hostdisk.c user/zenixfs_disk.c zenixfs/zenixfs.c || { echo "FAIL build zfsd-host"; exit 1; }
pass=0; fail=0
check() { if [ "$2" = ok ]; then echo "PASS $1"; pass=$((pass+1)); else echo "FAIL $1"; fail=$((fail+1)); fi; }
run() { HOSTDISK=$1 $T/zfsd < $2 > $3 2>&1; }

"$TOOL" mkfs $T/z.img 8 > /dev/null || { echo "FAIL mkfs"; exit 1; }
L150=$(printf 'p%.0s' $(seq 1 150)); L158=$(printf 'q%.0s' $(seq 1 158)); L256=$(printf 'r%.0s' $(seq 1 255))
"$TOOL" ln $T/z.img "/$L256" /l256 > /dev/null 2>&1 || { echo "FAIL a 256-byte target"; exit 1; }
cat > $T/s.txt <<EOF
mount
mkdir /d
mkdir /d/sub
open /d/sub/f wc
close %1
symlink /etc/motd /d/abs
symlink ../x/y /d/rel
symlink sub /d/s2
symlink /$L150 /d/l150
symlink /$L158 /d/l158
mkdir /nox
symlink /d /nox/in
chmod /nox 700
sync
EOF
run $T/z.img $T/s.txt $T/out
grep -q ' -> -' $T/out && { check "setting up the image" bad; cat $T/out; }
cp $T/z.img $T/base.img
cat > $T/t.txt <<EOF
mount
links on
stat /d/abs/x
stat /d/abs
lstat /d/abs
readlink /d/abs
open /d/abs r
open /d/abs/m/n r
open /d/abs wc
open /d/abs/q wcx
stat /d/rel/z
chdir /d/s2
mkdir /d/s2/new
unlink /d/s2/f
rmdir /d/s2/x
rename /d/sub/f /d/s2/g
rename /d/s2/f /d/sub/g
link /d/abs /d/sub/h
link /d/sub/f /d/s2/h
symlink some/target /d/s2/l
chmod /d/abs 600
chown /d/abs 5 5
utimes /d/abs 1000
stat /d/l150
stat /d/l150/x
stat /d/l158/x
stat /l256/x
stat d/abs/x
dirty
ls /d/sub
open /d/abs wcx
links off
stat /d/abs/x
stat /d/s2/f
readlink /l256
EOF
run $T/z.img $T/t.txt $T/out
want=$(cat <<EOF
links -> 1
stat -> -1024 a0 /etc/motd/x
stat -> -1024 a0 /etc/motd
lstat -> 0 ino
readlink -> 9 /etc/motd
open -> -1026 a0 /etc/motd
open -> -1024 a0 /etc/motd/m/n
open -> -1026 a0 /etc/motd
open -> -1024 a0 /etc/motd/q
stat -> -1024 a0 d/../x/y/z
chdir -> -1024 a0 d/sub
mkdir -> -1024 a0 d/sub/new
unlink -> -1024 a0 d/sub/f
rmdir -> -1024 a0 d/sub/x
rename -> -1025 a1 d/sub/g
rename -> -1024 a0 d/sub/f
link -> -1024 a0 /etc/motd
link -> -1025 a1 d/sub/h
symlink -> -1025 a1 d/sub/l
chmod -> -1024 a0 /etc/motd
chown -> -1024 a0 /etc/motd
utimes -> -1024 a0 /etc/motd
stat -> -1024 a0 /$L150
stat -> -1024 a0 /$L150/x
stat -> -36
stat -> -36
stat -> -2
EOF
)
got=$(grep -v '^mount\|^dirty\|^ls ->\|^links -> 0' $T/out | sed 's/^\(lstat -> 0 ino\).*/\1/' | head -27)
[ "$got" = "$want" ] && check "D26 zfsd: FS_LINKED for every request of the table: TEXT, the bits, S and E of a parent walk, absolute and relative targets, a TEXT too long, a 256-byte target, a relative path answered as today" ok ||
    { check "D26 zfsd: the FS_LINKED answers" bad; diff <(echo "$want") <(echo "$got"); }
grep -q '^dirty -> 0$' $T/out && grep -q '^ls -> . .. f (3, 0)$' $T/out && grep -q '^open -> -17$' $T/out &&
    check "D26 zfsd: the bounced requests changed nothing (no change waits for a commit); O_CREAT|O_EXCL on a link is -EEXIST" ok ||
    { check "D26 zfsd: bounced requests change nothing" bad; grep '^dirty\|^ls\|^open -> -17' $T/out; }
tail -3 $T/out | tr '\n' '|' | grep -q '^stat -> -2|stat -> 0 ino [0-9]* mode 100644.*|readlink -> 255 /rrr' &&
    check "D26 zfsd: without the bit, links are followed inside the volume as before" ok ||
    { check "D26 zfsd: without the bit, today's answers" bad; tail -3 $T/out; }
cp $T/base.img $T/p.img
printf 'mount\nlinks on\nas 1000 100\nstat /nox/in/sub\nrename /nox/in/a /d/s2/b\nas 0 0\nstat /nox/in/sub\n' > $T/p.txt
run $T/p.img $T/p.txt $T/out
[ "$(grep -v '^mount\|^links\|^as' $T/out | tr '\n' '|')" = "stat -> -13|rename -> -13|stat -> -1024 a0 /d/sub|" ] &&
    check "D26 zfsd: a user cannot read a link in a directory without x (-EACCES); root gets the FS_LINKED answer" ok ||
    { check "D26 zfsd: permissions before FS_LINKED" bad; cat $T/out; }
# supplementary groups (UMSG_GROUPS, 2026-10-31): each group the caller is in counts, as its gid does
cp $T/base.img $T/g.img
cat > $T/g.txt <<EOF
mount
mkdir /g
chmod /g 777
open /g/a wc
chown /g/a 0 10
chmod /g/a 640
open /g/b wc
chown /g/b 0 20
chmod /g/b 660
mkdir /g/d
chown /g/d 0 20
chmod /g/d 770
as 1000 100
groups 10,20
open /g/a r
open /g/a w
open /g/b rw
open /g/d/new wc
groups
open /g/a r
open /g/b rw
open /g/d/new2 wc
sync
EOF
run $T/g.img $T/g.txt $T/out
o=$(grep '^open -> ' $T/out | sed -n '3,$p' | awk '{print ($3 > 0) ? "ok" : $3}' | tr '\n' ' ')
[ "$o" = "ok -13 ok ok -13 -13 -13 " ] && check "zfsd: supplementary groups read a 640 file, write a 660 one, create in a 770 directory; not without them" ok ||
    { check "zfsd: supplementary groups ($o)" bad; cat $T/out; }
"$TOOL" fsck $T/g.img > $T/fsck.log 2>&1 && check "zfsd: zenixfs-tool fsck clean after the groups test" ok || { check "zfsd: fsck after the groups test" bad; cat $T/fsck.log; }
"$TOOL" fsck $T/z.img > $T/fsck.log 2>&1 && check "D26 zfsd: zenixfs-tool fsck clean afterwards" ok || { check "D26 zfsd: fsck afterwards" bad; cat $T/fsck.log; }

echo "zfsd-test: $pass passed, $fail failed"
[ $fail = 0 ]
