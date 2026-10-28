#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# ZenixFS in the emulator: partition, mkfs.zenixfs, mount next to an ext2 mount, files / links / rename / a 100 KB file,
# unmount, remount, zfsd (the file /sbin/zfsd, DESIGN D19) killed and started again, the old name @zfsd, fsck.zenixfs; then the HOST checks the same partition with zenixfs-tool (independent implementation
# of the check: the same library source, but a different program, block device and compiler).
# Then (audit F15) the PC writes a 300-byte link target into both volumes and a second boot checks that neither
# server follows a prefix of it ("file name too long") and that readlink prints it whole. A third boot checks DESIGN
# D26's symbolic links across mounts (ZenixFS next to ext2, tools/tests/zenixfs-links-test.txt).
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
make -s BUILD=$BD all $BD/zenix-mount.adf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
make -s -C zenixfs build/zenixfs-tool >/dev/null 2>&1 || { echo "host tool build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-zenixfstest}; mkdir -p $OUT
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
dd if=/dev/zero of=$BD/scratch.hdf bs=1M count=16 status=none
ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenixfs-serial} tools/run-test.sh fsuae/mounts.fs-uae 380 tools/tests/zenixfs-test.txt 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;:]*[mK]//g' > $OUT/run.log
# the log also holds the keystrokes the test typed, so a check must match a whole OUTPUT line, never a substring of the typing
wantline() { if grep -aqx -- "$2" $OUT/run.log; then check "$1" ok; else check "$1" bad; fi; }
wantre() { if grep -aqE -- "$2" $OUT/run.log; then check "$1" ok; else check "$1" bad; fi; }
wantre "fdisk shows a zenixfs partition" '^ide0p2 .* zenixfs$'
wantre "mkfs.zenixfs runs" '^mkfs.zenixfs: ide0p2: [0-9]+ blocks of [0-9]+ bytes$'
wantre "mount lists the zenixfs volume (type read from the partition table)" '^ide0p2 +/mnt +zenixfs '
wantline "file read back through a symbolic link" "hello-zenixfs"
wantre "ls -l shows the symbolic link" '^lrwx.* link -> a.txt$'
wantre "hard link count 2 for the file" '^-rw-r--r-- 2 root root +14 .* a.txt$'
wantre "wc sees the 100 KB file: 20000 lines, 108894 bytes" '^ *20000 +20000 +108894 /mnt/seq.txt$'
wantline "copy of the large file is identical" "SAME-SEQ"
wantline "copy to an ext2 mount is identical" "SAME-ACROSS-MOUNTS"
wantre "mv across mounts leaves the file on the ext2 side" '^ *hello-zenixfs$'
wantre "pwd inside the mounted filesystem" '^/mnt/dir2$'
wantre "data survives unmount and mount (wc again)" '^ *20000 +20000 +108894 /mnt/seq.txt$'
wantline "fsck.zenixfs reports clean" "fsck.zenixfs: clean"
# zfsd is a file since DESIGN D19 step 1: mount registers /sbin/zfsd (D11 6.4's file service), a killed one starts again
# from the file, and the frozen built-in name @zfsd an older mount registers starts the same file (the one alias)
wantre "mount starts the ZenixFS server from the file /sbin/zfsd" '^svc: fs\.ide0p2: started /sbin/zfsd \(pid [0-9]+, for pid [0-9]+ mount\)$'
wantre "zfsd killed: the log says it ended" '^svc: fs\.ide0p2: zfsd \(pid [0-9]+\) ended, status 143$'
wantre "... and the next use started it again from /sbin/zfsd" '^svc: fs\.ide0p2: started /sbin/zfsd \(pid [0-9]+, for pid [0-9]+ cat\)$'
before() { sed -n "/^$1\$/q;p" $OUT/run.log | tail -4 | grep -aqx "$2" && echo ok || echo bad; }
check "... the file reads back through the restarted zfsd" "$(before kill-done hello-zenixfs)"
wantre "the old built-in name @zfsd starts /sbin/zfsd (the alias)" '^svc: fs\.ide0p2: started @zfsd \(pid [0-9]+, for pid [0-9]+ svctest\)$'
check "... and serves the volume" "$(before alias-done hello-zenixfs)"
# the host looks at the partition: find the superblock ('ZNFS' at byte 32 of its first block) on the scratch disk
python3 - "$BD/scratch.hdf" <<'PY' > $OUT/offset.txt
import sys
d = open(sys.argv[1], 'rb').read()
off = 0
while True:
    i = d.find(b'ZNFS', off)
    if i < 0: break
    if i % 512 == 32 and d[i-28:i-26] == b'ZB':
        print(i - 32); break
    off = i + 1
PY
if [ -s $OUT/offset.txt ]; then
  start=$(cat $OUT/offset.txt)
  size=$(( 16*1024*1024 - start ))
  dd if=$BD/scratch.hdf of=$OUT/p2.img bs=512 skip=$(( start / 512 )) count=$(( size / 512 )) status=none
  zenixfs/build/zenixfs-tool fsck $OUT/p2.img > $OUT/hostfsck.log 2>&1 && check "host zenixfs-tool fsck: clean" ok || { check "host zenixfs-tool fsck: clean" bad; cat $OUT/hostfsck.log; }
  zenixfs/build/zenixfs-tool ls $OUT/p2.img / > $OUT/hostls.log 2>&1
  grep -q seq.txt $OUT/hostls.log && grep -q dir2 $OUT/hostls.log && check "host sees the files the Amiga wrote" ok || check "host sees the files the Amiga wrote" bad
  zenixfs/build/zenixfs-tool cat $OUT/p2.img /dir2/b.txt 2>/dev/null | grep -q hello-zenixfs && check "host reads a file the Amiga wrote" ok || check "host reads a file the Amiga wrote" bad
  seq 1 20000 | cmp -s - <(zenixfs/build/zenixfs-tool cat $OUT/p2.img /seq.txt) && check "host reads the 100 KB file byte for byte" ok || check "host reads the 100 KB file byte for byte" bad
else
  check "found the ZenixFS superblock on the scratch disk" bad
fi
# F15 (audit 2026-10-26): a link target over 255 bytes, written on the PC into both volumes. Its first 255 bytes name
# the file aaa (f15-wrong), the whole target aaabbb... (f15-right): following a prefix would open the wrong file.
# Both servers must answer "file name too long" instead; readlink prints the whole target.
if [ -s $OUT/offset.txt ]; then
  T=$(printf './%.0s' $(seq 126))aaa$(printf 'b%.0s' $(seq 45)); R=aaa$(printf 'b%.0s' $(seq 45))
  echo f15-wrong > $OUT/w.txt; echo f15-right > $OUT/r.txt
  p1=$(( 1008 * 512 )); p1n=$(( 4 * 1008 ))      # ide0p1: cylinder 1, 4 cylinders of 1008 sectors
  dd if=$BD/scratch.hdf of=$OUT/p1.img bs=512 skip=1008 count=$p1n status=none
  for c in "write $OUT/w.txt aaa" "write $OUT/r.txt $R" "symlink long $T"; do debugfs -w -R "$c" $OUT/p1.img >/dev/null 2>&1; done
  zenixfs/build/zenixfs-tool put $OUT/p2.img $OUT/w.txt /aaa && zenixfs/build/zenixfs-tool put $OUT/p2.img $OUT/r.txt /$R && \
    zenixfs/build/zenixfs-tool ln $OUT/p2.img "$T" /long || check "F15: the PC wrote the long links" bad
  dd if=$OUT/p1.img of=$BD/scratch.hdf bs=512 seek=1008 conv=notrunc status=none
  dd if=$OUT/p2.img of=$BD/scratch.hdf bs=512 seek=$(( start / 512 )) conv=notrunc status=none
  ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenixfs-serial} tools/run-test.sh fsuae/mounts.fs-uae 150 tools/tests/zenixfs-long-test.txt 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;:]*[mK]//g' > $OUT/long.log
  L=$OUT/long.log
  lre() { if grep -aqE -- "$2" $L; then check "$1" ok; else check "$1" bad; fi; }
  lre "F15: ext2: a link target over 255 bytes is not followed (file name too long)" '^cat: /ext/long: file name too long$'
  lre "F15: ZenixFS: the same" '^cat: /mnt/long: file name too long$'
  lre "F15: ... cat fails on both" '^(.*# )?e15=1 z15=1$'
  [ "$(grep -acx f15-right $L)" = 2 ] && check "F15: the right files read directly on both volumes" ok || check "F15: the right files read directly on both volumes" bad
  grep -aqx f15-wrong $L && check "F15: no prefix of a target was followed (f15-wrong never read)" bad || check "F15: no prefix of a target was followed (f15-wrong never read)" ok
  [ "$(grep -acxF -- "$T" $L)" = 2 ] && check "F15: readlink prints the whole 300-byte target on both" ok || check "F15: readlink prints the whole 300-byte target on both" bad
  lre "F15: ls -l shows a long target cut, ending in ..." '/ext/long -> (\./){30}\.\.\.$'
  lre "F15: tree the same" 'long -> (\./){78}\.\.\.$'
  lre "F15: realpath cannot resolve it (never a prefix)" '^realpath: /ext/long: cannot resolve$'
  lre "F15: tar reports and skips a target over 100 bytes" "^tar: /ext/long: link target longer than 100 bytes \(the tar header's limit\): skipped$"
  lre "F15: ... and exits 1" '^(.*# )?t15=1$'
  lre "F15: the run ended" '^(.*# )?f15-done$'
  dd if=$BD/scratch.hdf of=$OUT/p1.img bs=512 skip=1008 count=$p1n status=none
  e2fsck -fn $OUT/p1.img > $OUT/e2fsck15.log 2>&1 && check "F15: host e2fsck: the ext2 partition is clean" ok || check "F15: host e2fsck: the ext2 partition is clean" bad
fi
# DESIGN D26 (audit F12): symbolic links across mounts with ZenixFS on /mnt next to ext2 on /ext (a third boot on the
# same scratch disk, tools/tests/zenixfs-links-test.txt)
ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenixfs-serial} tools/run-test.sh fsuae/mounts.fs-uae 300 tools/tests/zenixfs-links-test.txt 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;:]*[mK]//g' > $OUT/links.log
L=$OUT/links.log
v() { grep -a "^\(.*# \)\?$1=" $L | tail -1 | sed "s/^.*$1=//"; }
is() { [ "$(v $2)" = "$3" ] && check "$1" ok || check "$1 (got '$(v $2)', want '$3')" bad; }
is "D26 1 ZenixFS: a link on the root to a file on the ZenixFS mount" z1a z-file
is "D26 1 ZenixFS: ... readlink shows it" z1b /mnt/w/f
is "D26 1 ZenixFS: a link on ZenixFS to a file on the ext2 mount" z1c e-file
is "D26 2 ZenixFS: an absolute target on the root" z2a root-file
is "D26 2 ZenixFS: ... and on the same mount" z2b z-file
is "D26 3 ZenixFS: a relative link at the mount's root climbing out" z3a root-file
is "D26 3 ZenixFS: ... one on the root passing the mount point" z3b z-file
is "D26 3 ZenixFS: ... one climbing from ZenixFS into the ext2 mount" z3c e-file
is "D26 4 ZenixFS: cat through a directory link" z4a z-file
is "D26 4 ZenixFS: pwd after cd through it: the resolved text" z4b /mnt/w
is "D26 4 ZenixFS: cd ..: the mount's parent" z4c /mnt
is "D26 4 ZenixFS: umount busy while a shell is there" z4d 1
is "D26 5 ZenixFS: echo > through a directory link" z5a x5
is "D26 5 ZenixFS: O_CREAT on a link to nothing on the ext2 mount fails" z5b 1
is "D26 5 ZenixFS: ... and creates nothing" z5c 1
is "D26 5 ZenixFS: a 25-link chain between ZenixFS and ext2 with O_TRUNC" z5d trunc25
is "D26 6 ZenixFS: rm removes the link" z6a 1
is "D26 6 ZenixFS: ... not its file" z6b z-file
is "D26 6 ZenixFS: ln onto another mount through a link: -EXDEV" z6c 1
is "D26 6 ZenixFS: ln from ZenixFS (through a link) to ext2: -EXDEV" z6d 1
is "D26 7 ZenixFS: a loop between ZenixFS and ext2: -ELOOP" z7a 1
is "D26 7 ZenixFS: a chain of 40 works" z7b old
is "D26 7 ZenixFS: ... 41 is -ELOOP" z7c 1
is "D26 7 ZenixFS: an O_TRUNC open of a 40-chain" z7d t40
is "D26 7 ZenixFS: an unlink through a 40-chain of directory links" z7e 0
is "D26 7 ZenixFS: ... the file is gone" z7f 1
is "D26 8 ZenixFS: a user cannot read a link in a ZenixFS directory without x" z8a 1
is "D26 10 ZenixFS: a link on ZenixFS to /dev/null" z10 0
grep -aqE '^ide0p1 +/mnt/mp +ext2' $L && check "D26 12 ZenixFS: ext2 mounted through a link onto a ZenixFS directory: the resolved mount point" ok || check "D26 12 ZenixFS: mount through a link onto ZenixFS" bad
is "D26 12 ZenixFS: ... the files there" z12a e-file
is "D26 12 ZenixFS: ... umount by the resolved name" z12b 0
is "D26 15 ZenixFS: '..' after a link on the text" z15 root-file
is "D26 ZenixFS: unmounted at the end" um-end 0
grep -aqE '^(.*# )?zlinks-done$' $L && check "D26 ZenixFS: the links script ran to the end" ok || check "D26 ZenixFS: the links script ran to the end" bad
exit $((1 - pass))
