#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# ZenixFS in the emulator: partition, mkfs.zenixfs, mount next to an ext2 mount, files / links / rename / a 100 KB file,
# unmount, remount, zfsd (the file /sbin/zfsd, DESIGN D19) killed and started again, the old name @zfsd, fsck.zenixfs; then the HOST checks the same partition with zenixfs-tool (independent implementation
# of the check: the same library source, but a different program, block device and compiler).
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
exit $((1 - pass))
