#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Mount tests in the emulator: two ext2 partitions mounted on /mnt and /mnt/sub, data across unmount/mount.
# Verified afterwards with the host's e2fsck. Then DESIGN D26's symbolic links across mounts (a third boot). Mounts are services (DESIGN D11 6.4): a mount's server runs at nice -10
# with no parent, comes back after a kill, ends for good at umount; a failed mount leaves no service; ordinary ports
# used up and all twelve service slots in use; a floppy without a volume id is not served again after a restart; the
# servers' own messages go to the kernel log (dmesg).
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
make -s BUILD=$BD all $BD/zenix-mount.adf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-mounttest}; mkdir -p $OUT
# the RAM disk's free blocks are the room these tests write into (mount points and files in / and /tmp: about 13
# blocks); with less, dozens of checks fail for want of space (2026-11-06: 1 block left), so say that instead
free=$(dumpe2fs -h $BD/root-mount.img 2>/dev/null | awk -F: '/^Free blocks/ { print $2 + 0 }')
[ "${free:-0}" -ge 16 ] || { echo "FAIL root-mount.img has ${free:-?} free blocks, the mount tests need 16 (Makefile RAMDISK_MOUNT_K, MOUNTAPPS)"; exit 1; }
echo "INFO root-mount.img: $free blocks free"
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
dd if=/dev/zero of=$BD/scratch.hdf bs=1M count=16 status=none
# df1: an ext2 floppy whose superblock carries no uuid (zeros), with one file
head -c 901120 /dev/zero > $OUT/df1.adf
mke2fs -q -t ext2 -E revision=0 -b 1024 -F $OUT/df1.adf >/dev/null 2>&1 || { echo "FAIL mke2fs"; exit 1; }
echo floppy-one > $OUT/f.txt
debugfs -w -R "write $OUT/f.txt f.txt" $OUT/df1.adf >/dev/null 2>&1
dd if=/dev/zero of=$OUT/df1.adf bs=1 seek=1128 count=16 conv=notrunc status=none
rm -rf $OUT/states; mkdir -p $OUT/states
sed "s#^floppy_drive_0 = .*#&\nfloppy_drive_1 = $OUT/df1.adf\nfloppy_drive_count = 2\nwritable_floppy_images = 1\nsave_states_dir = $OUT/states#" fsuae/mounts.fs-uae > $OUT/mounts.fs-uae
ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenixfs-serial} tools/run-test.sh $OUT/mounts.fs-uae 300 tools/tests/mount-test.txt > $OUT/mount.log 2>&1
grep -q 'ide0p1 .*/mnt .*ext2' $OUT/mount.log && check "mount lists the first filesystem" ok || check "mount lists the first filesystem" bad
grep -q 'ide0p2 .*/mnt/sub .*ext2' $OUT/mount.log && check "nested mount works" ok || check "nested mount works" bad
grep -aq '^fdisk: ide0 is in use (a partition on it is mounted)' $OUT/mount.log && check "fdisk does not write a disk with a mounted partition" ok || check "fdisk does not write a disk with a mounted partition" bad
grep -qx 'hello-one' $OUT/mount.log && grep -qx 'hello-two' $OUT/mount.log && check "files read back through both mounts" ok || check "files read back through both mounts" bad
grep -q '^/mnt/sub' $OUT/mount.log && check "pwd inside a mount" ok || check "pwd inside a mount" bad
sed -n '/END-OF-BUSY-TEST/q;p' $OUT/mount.log | grep -q 'is busy' && check "umount refuses a mount with another mount below it" ok || check "umount refuses a mount with another mount below it" bad
sed -n '/END-OF-BUSY-TEST/,$p' $OUT/mount.log | grep -q 'is busy' && check "no other umount was refused" bad || check "no other umount was refused" ok
tr -d '\r' < $OUT/mount.log | awk '/MOUNTED ON/{buf=""} {buf=buf"\n"$0} END{print buf}' | grep -q 'ide0p' && check "everything is unmounted at the end" bad || check "everything is unmounted at the end" ok
L=$OUT/mount.log
has() { if tr -d '\r' < $L | grep -aqE -- "$2"; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
has "a mount's server runs with no parent at nice -10 (two fsd lines)" '^ +[0-9]+ +0 +[0-9]+ +-10 [a-z]+ +[0-9]+ +[0-9]+ fsd$'
[ "$(tr -d '\r' < $L | grep -acE '^ +[0-9]+ +0 +[0-9]+ +-10 [a-z]+ +[0-9]+ +[0-9]+ fsd$')" -ge 2 ] && check "... the root's and the mount's" ok || check "... the root's and the mount's" bad
has "the mount's server killed: the log says it ended" '^svc: fs\.ide0p1: fsd \(pid [0-9]+\) ended, status 143$'
has "... and the next use started it again" '^svc: fs\.ide0p1: started @fsd \(pid [0-9]+, for pid [0-9]+ cat\)$'
has "... a file written before the kill reads back" '^(.*# )?kept-across$'
has "umount ends it for good (only the root's fsd left)" '^(.*# )?fsd-left=1$'
has "... and its name is gone" 'svctest call fs\.ide0p1 1: -1 in [0-9]+ ms$'
has "a failed mount says why" '^mount: cannot mount ide0p2: its filesystem server did not start \(see dmesg\)$'
has "... and leaves no service (the name is free again)" 'svctest port fs\.ide0p2: [0-9]+$'
has "ordinary ports used up" 'svctest ports: 16 made, then -2$'
[ "$(tr -d '\r' < $L | grep -acE '^(.*# )?kept-across$')" -ge 3 ] && check "... root's mount still works meanwhile; umount then mount at once with all twelve slots in use works" ok || check "... root's mount still works meanwhile; umount then mount at once with all twelve slots in use works" bad
has "a thirteenth service is refused" 'svctest reg d10: -16$'
has "... and mount says so" '^mount: too many filesystems mounted$'
has "a floppy reads through its mount" '^(.*# )?floppy-one$'
has "a floppy without a volume id is not served again after a restart" '^fs\.df1: a floppy without a volume id is not served again after a restart: mount it again$'
has "the servers' messages are in the kernel log: diskd's (a dead server's claim released at the restart)" '^(.*# )?inlog-diskd=[1-9]$'
has "... zfsd's (not a ZenixFS volume)" '^(.*# )?inlog-zfsd=[1-9]$'
has "... fsd's (the floppy refused at its restart)" '^(.*# )?inlog-fsd=[1-9]$'
has "the script ran to the end" '^(.*# )?mount-svc-done$'
# F13 (audit 2026-10-26): a path through a mount checks search permission on the directories above the mount point
has "F13: another user reads through mounts below an open directory (normal paths unchanged)" '^(.*# )?open-g=0$'
has "F13: the owner of the 700 directory reads through both mounts" '^(.*# )?owner=0$'
has "F13: root reads through both mounts" '^(.*# )?root=0$'
for g in g1 g2 g3 g4 g5; do
  has "F13: another user cannot pass the 700 directory above the mounts ($g)" "^(.*# )?$g=[1-9][0-9]*\$"
done
has "F13: ... and is told 'permission denied'" '^cat: /tmp/al/mnt/s\.txt: permission denied'
[ "$(tr -d '\r' < $L | grep -acx 'f13-one')" = 3 ] && [ "$(tr -d '\r' < $L | grep -acx 'f13-two')" = 3 ] && \
  check "F13: the files were read exactly three times (guest before the chmod, the owner, root)" ok || \
  check "F13: the files were read exactly three times (guest before the chmod, the owner, root)" bad
has "F13: the section ran to the end" '^(.*# )?f13-done$'
# umount while a program is being loaded from the mount (audit A3): refused, the load completes; a floppy's reads are
# slow enough to umount in the middle
head -c 901120 /dev/zero > $OUT/df1l.adf
mke2fs -q -t ext2 -E revision=0 -b 1024 -F $OUT/df1l.adf >/dev/null 2>&1 || { echo "FAIL mke2fs"; exit 1; }
debugfs -w -R "write $BD/awk.zx awk" $OUT/df1l.adf >/dev/null 2>&1
debugfs -w -R "sif awk mode 0100755" $OUT/df1l.adf >/dev/null 2>&1
cp $BD/zenix.hdf $OUT/lroot.hdf
sed "s#^floppy_drive_0 = .*#&\nfloppy_drive_1 = $OUT/df1l.adf\nfloppy_drive_count = 2#; s#^floppy_drive_speed = .*#floppy_drive_speed = 100#; s#^hard_drive_0 = .*#hard_drive_0 = $OUT/lroot.hdf#" fsuae/a1200-020-fast.fs-uae > $OUT/load.fs-uae
ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenixfs-serial} tools/run-test.sh $OUT/load.fs-uae 220 tools/tests/mount-load-test.txt > $OUT/load.log 2>&1
L=$OUT/load.log
has "umount during a program load from the mount is refused" '^(.*# )?um=1$'
has "... the program loaded and ran" '^(.*# )?loaded-ok$'
has "... and the umount after it works" '^(.*# )?um2=0$'
# F13's cost, for the record (FS-UAE, not hardware): 200 stats each; the user's through the mount pay one more call
tr -d '\r' < $L | awk '/^m-(user|root)-(mnt|root)$/ { m = $0 } /^real/ && m { print "INFO F13 200 stats: " m " " $2; m = "" }'
rm -f $OUT/df1l.adf $OUT/lroot.hdf
# ---- DESIGN D26 (audit F12): symbolic links across mounts, on the same scratch disk (ide0p1, ide0p2 from the first
# run) and a read-only ext2 floppy whose links the PC made (tools/tests/mount-links-test.txt) ----
head -c 901120 /dev/zero > $OUT/dfro.adf
mke2fs -q -t ext2 -E revision=0 -b 1024 -F $OUT/dfro.adf >/dev/null 2>&1 || { echo "FAIL mke2fs"; exit 1; }
echo ro-file > $OUT/ro.txt
for c in "write $OUT/ro.txt f.txt" "write $BD/hello.pzx hp" "sif hp mode 0100755" "write $BD/free.zx free" "sif free mode 0100755" \
         "write $BD/msgpingd.zx mpd" "sif mpd mode 0100755" "write $BD/time.zx time" "sif time mode 0100755" \
         "write $BD/sha256sum.zx sha256sum" "sif sha256sum mode 0100755" \
         "symlink abs /mnt/w" "symlink up ../mnt/w" \
         "symlink long250 /tmp/$(printf 'a%.0s' $(seq 245))" "symlink l256 /$(printf 'r%.0s' $(seq 255))" "symlink empty x" \
         "sif empty size 0" "ssv state 3"; do
    debugfs -w -R "$c" $OUT/dfro.adf >/dev/null 2>&1
done
rosum=$(sha256sum < $OUT/dfro.adf)
sed "s#^floppy_drive_0 = .*#&\nfloppy_drive_1 = $OUT/dfro.adf\nfloppy_drive_count = 2\nwritable_floppy_images = 1#" fsuae/mounts.fs-uae > $OUT/links.fs-uae
ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenixfs-serial} tools/run-test.sh $OUT/links.fs-uae 330 tools/tests/mount-links-test.txt > $OUT/links.log 2>&1
L=$OUT/links.log
v() { tr -d '\r' < $L | grep -a "^\(.*# \)\?$1=" | tail -1 | sed "s/^.*$1=//"; }   # the value a line NAME=VALUE printed
is() { [ "$(v $2)" = "$3" ] && check "$1" ok || { check "$1 (got '$(v $2)', want '$3')" bad; }; }
is "D26 4: a chdir through a link before the first mount records the resolved text" c4e /mnt
is "D26 4: ... so umount sees the working directory inside the mount (busy)" c4f 1
is "D26 1: a link on the root to a file on the mount: cat reads it" c1a mnt-file
is "D26 1: ... readlink shows the link" c1b /mnt/w/f
has "D26 1: ... ls -l shows the link" '^l[rwx-]{9} .* /tmp/l1 -> /mnt/w/f$'
is "D26 2: a link on the mount with an absolute target on the root" c2a root-file
is "D26 2: ... and one with an absolute target on the same mount" c2b mnt-file
is "D26 3: a relative link at the mount's root climbing out (../tmp/r.txt)" c3a root-file
is "D26 3: ... one on the root passing the mount point (../mnt/w/f from /tmp)" c3b mnt-file
is "D26 3: ... the same on the read-only floppy (../mnt/w from /ro)" c3c mnt-file
is "D26 2: ... an absolute directory link on the floppy into the other mount" c3d mnt-file
is "D26 4: cat through a directory link into the mount" c4a mnt-file
is "D26 4: cd through it: pwd shows the resolved text" c4b /mnt/w
is "D26 4: cd .. then: the mount's parent" c4c /mnt
is "D26 4: umount is busy while a shell is there" c4d 1
is "D26 5: echo > through a directory link lands on the mount" c5a x5
is "D26 5: O_CREAT on a link to nothing on another mount fails (-ENOENT)" c5b 1
is "D26 5: ... and creates nothing" c5c 1
is "D26 5: a 25-link chain across the mounts with O_TRUNC" c5d trunc25
is "D26 6: rm removes the link" c6a 1
is "D26 6: ... not what it points to" c6b mnt-file
is "D26 6: ln onto another mount through a link: -EXDEV" c6c 1
is "D26 6: ... ln on the same mount works" c6d 0
is "D26 6: ln of a name through a link into another mount: -EXDEV" c6g 1
has "D26 6: ... said as a cross-device link" '^ln: .*cross-device link'
is "D26 6: mv between textual mounts that resolve to one (the deviation): it copies" c6e a6
is "D26 6: ... and the other way" c6f a6
is "D26 6: ... nothing left behind" c6h 1
is "D26 7: two links pointing at each other across mounts: -ELOOP" c7a 1
has "D26 7: ... said as too many levels" '^cat: /tmp/la: too many levels of symbolic links$'
is "D26 7: a chain of 40 across the two mounts works" c7b old
is "D26 7: ... 41 is -ELOOP" c7c 1
is "D26 7: an O_TRUNC open of a 40-chain (FS_STAT, then FS_OPEN from there)" c7d t40
is "D26 7: an unlink through a 40-chain of directory links (FS_LSTAT, then FS_UNLINK)" c7e 0
is "D26 7: ... the file is gone" c7f 1
is "D26 8: a user cannot read a link in a directory without x" c8a 1
has "D26 8: ... permission denied" '^cat: /tmp/nx/l: permission denied$'
is "D26 8: a link into a mount below a directory without x: -EACCES (F13)" c8b 1
has "D26 8: ... permission denied" '^cat: /tmp/l8: permission denied$'
is "D26 8: ... root reads it" c8c p8
fv2() { tr -d '\r' < $L | sed -n "/^==$1\$/,\$p" | grep -m1 -oE 'shared code: [0-9]+ KB, sharing saves [0-9]+ KB' | grep -oE '[0-9]+' | tr '\n' ' '; }
set -- $(fv2 s2); s2s=${1:-0}; s2v=${2:-0}; set -- $(fv2 s3); s3s=${1:-0}
[ "$s2v" -gt 0 ] && check "D26 9: a program run twice through a link into the mount is shared (D13: saves $s2v KB)" ok || check "D26 9: a program run twice through a link is shared (saves '$s2v')" bad
is "D26 9: >> through a link to the running program: text file busy" c9a 1
has "D26 9: ... cp over it through the link: text file busy" '^cp: /tmp/lhp: text file busy$'
is "D26 9: ... cp fails" c9b 1
is "D26 9: rm through a directory link while it runs" c9c 0
[ "$s3s" -gt "$s2s" ] && check "D26 9: the file replaced on the mount: a new start loads a new copy" ok || check "D26 9: a new start after the replacement loads a new copy ($s2s -> $s3s)" bad
is "D26 10: a link on the mount to /dev/null: writing works" c10a 0
is "D26 10: ... and reads nothing" c10b '[]'
is "D26 11: a continuation over 255 bytes: -ENAMETOOLONG" c11a 1
has "D26 11: ... file name too long" '^cat: /ro/long250/abcdefghij: file name too long$'
is "D26 11: a 256-byte target: -ENAMETOOLONG" c11b 1
has "D26 11: ... file name too long" '^cat: /ro/l256: file name too long$'
is "D26 11: an empty target: -ENOENT" c11c 1
has "D26 11: ... no such file" '^cat: /ro/empty/x: no such file or directory$'
has "D26 12: mount through a link: mount -l shows the resolved mount point" '^ide0p2 +/tmp/mp +ext2'
is "D26 12: ... umount by the resolved name" c12a 0
has "D26 12: mount through a directory link: resolved too" '^ide0p2 +/tmp/real/mp2 +ext2'
is "D26 12: ... umount by the name through the link" c12b 0
is "D26 12: a link to / is refused" c12c 1
is "D26 12: a link to an existing mount point is refused" c12d 1
has "D26 12: ... busy, both" '^mount: cannot mount ide0p2 on /tmp/lm2: device or resource busy$'
is "D26 12: a resolved mount point over 47 bytes is refused" c12e 1
has "D26 12: ... file name too long" '^mount: cannot mount ide0p2 on /tmp/ls: file name too long$'
is "D26 13: umount of a stopped mount service by its exact text" c13a 0
is "D26 13: ... started no server" c13b 0
is "D26 14: chmod through a link from the read-only volume into the writable one" c14a 0
is "D26 14: ... mkdir" c14b 0
is "D26 14: ... rm" c14c 0
is "D26 14: ... the directory is on the writable volume" c14d 0
is "D26 14: ... the file is gone from it" c14e 1
is "D26 14: ... while a chmod on the read-only volume itself is refused" c14f 1
[ "$rosum" = "$(sha256sum < $OUT/dfro.adf)" ] && check "D26 14: ... and nothing was written to the read-only floppy" ok || check "D26 14: the read-only floppy was written" bad
is "D26 15: '..' directly after a link is taken on the text (the pinned deviation)" c15 root-file
has "D26 17: a service whose program is a link into a mount starts and serves" '^svctest call lp 3: 1 in [0-9]+ ms$'
has "D26 4: a server's FS_LINKED with bits outside the table: -EIO" '^cat: /tmp/ml/x: input/output error$'
has "D26 4: ... one to a request that never asks (a read): -EIO" '^sha256sum: /tmp/ml/f: input/output error$'
has "D26 4: ... a valid one goes on at its text, from /" '^stat: /tmp/ml/z: not found$'
is "D26 4: ... and that server unmounts" cml 0
is "D26: everything unmounted at the end" um-end 0
has "D26: the links script ran to the end" '^(.*# )?links-done$'
tr -d '\r' < $L | awk '/^lk-(link|direct)$/ { m = $0 } /^real/ && m { print "INFO D26 200 test -e (FS-UAE, not hardware): " m " " $2; m = "" }'
rm -f $OUT/dfro.adf

# the unmounted partitions are clean ext2 as seen by the host (partitions start at cylinder 1 and 14 of 1008 sectors)
for spec in "1 13104" "14 18144"; do set -- $spec
  dd if=$BD/scratch.hdf of=$OUT/p$1.img bs=512 skip=$(( $1 * 1008 )) count=$2 status=none
  e2fsck -fn $OUT/p$1.img >$OUT/e2fsck$1.log 2>&1 && check "host e2fsck: partition at cylinder $1 is clean" ok || check "host e2fsck: partition at cylinder $1 is clean" bad
done
exit $((1 - pass))
