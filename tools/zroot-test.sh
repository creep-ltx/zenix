#!/usr/bin/env bash
# ZenixFS as the ROOT filesystem, in the emulator. Boot 1: root is ZenixFS (mount/df), files, links, a bigger file, sync.
# Boot 2 (same disk): the data is there after a reboot; then the users-and-permissions test of tools/users-test.sh runs
# against the ZenixFS root. Finally the host checks the root partition with zenixfs-tool.
cd "$(dirname "$0")/.." || exit 1
make -s all build/zenix.adf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
make -s -C zenixfs build/zenixfs-tool >/dev/null 2>&1 || { echo "host tool build failed"; exit 1; }
OUT=/tmp/zenix-zroot; mkdir -p $OUT
# the root disk: ZenixFS root + a second partition (ext2, empty) that /etc/fstab mounts on /data at boot
rm -f build/zenix-zroot.hdf $OUT/data.img
mke2fs -q -t ext2 -E revision=0 -b 1024 -F $OUT/data.img 2M || { echo "mke2fs failed"; exit 1; }
printf '# test fstab\nide0p2  /data  auto  defaults\n' > $OUT/fstab
make -s build/zenix-zroot.hdf >/dev/null 2>&1    # makes the plain one (also builds the programs); then ours replaces it
FSTAB_FILE=$OUT/fstab MOUNTDIRS=data EXTRA="$OUT/data.img:ext2:data" tools/mkroot-zenixfs.sh build/zenix-zroot.hdf build "$(make -s -f Makefile --eval 'pa:;@echo $(UAPPS)' pa 2>/dev/null)" 8 >/dev/null || { echo "zroot disk build failed"; exit 1; }
SER=${ZENIX_SERIAL:-/tmp/zenixfs-serial}
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
clean() { tr -d '\r' | sed 's/\x1b\[K//g'; }
ZENIX_SERIAL=$SER tools/run-test.sh fsuae/zroot.fs-uae 110 tools/tests/zroot-test.txt 2>&1 | clean > $OUT/boot1.log
grep -aqE '^ide0p1 +/ +zenixfs ' $OUT/boot1.log && check "the root filesystem is ZenixFS (mount)" ok || check "the root filesystem is ZenixFS (mount)" bad
grep -aqE '^ide0p2 +/data +ext2 ' $OUT/boot1.log && check "/etc/fstab mounted the second partition at boot" ok || check "/etc/fstab mounted the second partition at boot" bad
grep -aqx 'data-persist' $OUT/boot1.log && check "the fstab-mounted ext2 partition can be written and read" ok || check "the fstab-mounted ext2 partition can be written and read" bad
grep -aqE '^ *5000 +5000 +23893 /big.txt$' $OUT/boot1.log && check "a 23 KB file is written" ok || check "a 23 KB file is written" bad
grep -aqx 'persist-me' $OUT/boot1.log && check "a file is read back through a symbolic link" ok || check "a file is read back through a symbolic link" bad
grep -aqx 'SYNC-DONE' $OUT/boot1.log && check "sync returns" ok || check "sync returns" bad
ZENIX_SERIAL=$SER tools/run-test.sh fsuae/zroot.fs-uae 60 tools/tests/zroot-test2.txt 2>&1 | clean > $OUT/boot2.log
grep -aqx 'data-persist' $OUT/boot2.log && check "the fstab mount and its data are there after a reboot" ok || check "the fstab mount and its data are there after a reboot" bad
grep -aqx 'persist-me' $OUT/boot2.log && grep -aqx 'inner' $OUT/boot2.log && check "files are there after a reboot" ok || check "files are there after a reboot" bad
grep -aqE '^ *5000 +5000 +23893 /big.txt$' $OUT/boot2.log && check "the bigger file is there after a reboot" ok || check "the bigger file is there after a reboot" bad
# users and permissions: main's test script and expectations, on this root
ZENIX_SERIAL=$SER tools/run-test.sh fsuae/zroot.fs-uae 240 tools/tests/users-test.txt 2>&1 | clean > $OUT/users.log
want() { if grep -aqF -- "$2" $OUT/users.log; then check "users: $1" ok; else check "users: $1" bad; fi; }
want "root identity" "uid=0(root) gid=0(root)"
want "su to a user" "uid=1000(user) gid=100(users)"
want "user cannot write to /" "sh: /nofile: permission denied"
want "owner names in ls -l" "-rw-r--r-- 1 user users 5"
want "umask 077 gives mode 600" "-rw------- 1 user users 2"
want "set-user-id shown in ls" "-rwsr-xr-x 1 root root"
want "user cannot remove a root file" "rm: /etc/motd: permission denied"
want "user cannot read a 0600 root file" "cat: /tmp/r1: permission denied"
want "sticky /tmp protects other users' files" "rm: /tmp/r1: permission denied"
want "running a file without x says permission denied" "/etc/passwd: permission denied"
want "passwd stores a password" "passwd: password for guest changed"
want "su with a wrong password fails" "su: wrong password"
want "su with the right password works" "uid=1001(guest) gid=100(users)"
want "chown by name" "-rw-r--r-- 1 guest users 5"
want "sha256sum matches the host" "$(sha256sum rootfs/etc/group | cut -d' ' -f1)"
want "workspace shell runs as the logged-in user" "/home/user"
. tools/users-d-checks.sh
users_d_checks $OUT/users.log "users: " || pass=0
# the host looks at the root partition
python3 - <<'PY' > $OUT/offset.txt
d = open('build/zenix-zroot.hdf', 'rb').read()
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
  dd if=build/zenix-zroot.hdf of=$OUT/root.img bs=512 skip=$(( start / 512 )) status=none
  zenixfs/build/zenixfs-tool fsck $OUT/root.img > $OUT/hostfsck.log 2>&1 && check "host zenixfs-tool fsck of the root: clean" ok || { check "host zenixfs-tool fsck of the root: clean" bad; cat $OUT/hostfsck.log; }
  zenixfs/build/zenixfs-tool cat $OUT/root.img /persist.txt 2>/dev/null | grep -q persist-me && check "host reads the file the Amiga wrote on the root" ok || check "host reads the file the Amiga wrote on the root" bad
else
  check "found the ZenixFS root partition on the disk" bad
fi
exit $((1 - pass))
