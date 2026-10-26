#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# useradd and userdel in the emulator: rootfs/tests/useradd.sh runs as the autologin root; its output is compared here,
# then the host checks /etc/passwd and /etc/group in the image (e2fsck clean).
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-useradd}; mkdir -p $OUT
cat > $OUT/script.txt <<EOS
wait zenix:/# 90
sleep 1
send sh /tests/useradd.sh > /tmp/ua.out 2>&1; echo finished-ua\r
wait (?m)^finished-ua$ 120
EOS
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 160 $OUT/script.txt > $OUT/run.log 2>&1
debugfs -R "cat /tmp/ua.out" $BD/zenix.hdf > $OUT/ua.out 2>/dev/null
pass=1
want() { if grep -aqxF -- "$2" $OUT/ua.out; then echo "PASS $1"; else echo "FAIL $1 (want: $2)"; pass=0; fi; }
want "useradd adds a user" "add-anna=0"
want "the line in /etc/passwd" "anna::1002:100:Test User:/home/anna:/bin/sh"
grep -aq '^drwx------ .* anna users .* /home/anna$' $OUT/ua.out && echo "PASS the home is made, the user's, mode 700" || { echo "FAIL home"; pass=0; }
want "a name that exists is refused" "add-again=1"
want "a user number in use is refused" "add-taken-uid=1"
want "a bad name is refused" "add-bad-name=2"
want "an unknown group is refused" "add-no-group=1"
want "-M -u -g -s" "add-dave=0"
want "dave's line" "dave::2000:100::/home/dave:/bin/sh"
want "-M makes no home" "dave-home=1"
want "the new user can log in (su)" "uid=1002(anna) gid=100(users)"
want "a normal user may not add users" "useradd: only root may add users"
want "group members before" "users::100:user,guest,anna"
want "userdel -r" "del-anna=0"
want "the user's line is gone" "0"
want "userdel takes the user out of the groups" "users::100:user,guest"
want "-r removes the home" "anna-home=1"
want "userdel" "del-dave=0"
want "root cannot be removed" "del-root=1"
want "an unknown user" "del-none=1"
want "the other users stay" "3"
want "the script ran to the end" "useradd-done"
debugfs -R "cat /etc/passwd" $BD/zenix.hdf 2>/dev/null | cmp -s - rootfs/etc/passwd && echo "PASS /etc/passwd is as shipped again" || { echo "FAIL /etc/passwd differs from rootfs"; pass=0; }
e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1 && echo "PASS host e2fsck clean" || { echo "FAIL e2fsck"; pass=0; }
exit $((1 - pass))
