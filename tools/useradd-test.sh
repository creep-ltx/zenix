#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# useradd, userdel, gpasswd and sudo in the emulator: rootfs/tests/useradd.sh runs as the autologin root; its output is compared here,
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
wait (?m)^finished-ua$ 180
EOS
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 220 $OUT/script.txt > $OUT/run.log 2>&1
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
want "gpasswd -a" "gp-add=0"
want "gpasswd -a twice is fine" "gp-again=0"
want "group members before" "users::100:user,guest,anna"
want "userdel -r" "del-anna=0"
want "the user's line is gone" "0"
want "userdel takes the user out of the groups" "users::100:user,guest"
want "-r removes the home" "anna-home=1"
want "userdel" "del-dave=0"
want "root cannot be removed" "del-root=1"
want "an unknown user" "del-none=1"
want "the other users stay" "3"
want "sudo for a wheel member without a password" "uid=0(root) gid=0(root)"
want "sudo-user" "sudo-user=0"
want "sudo refused outside wheel" "sudo: guest is not in the group wheel (root can add: gpasswd -a guest wheel)"
want "sudo-guest" "sudo-guest=1"
want "useradd -G" "add-fay=0"
want "-G adds to wheel" "wheel::10:user,fay"
want "passwd for fay" "pw-fay=0"
want "sudo with the right password" "sudo-pw=0"
want "three wrong passwords" "sudo-wrong=1"
grep -ac '^sudo: wrong password$' $OUT/ua.out | grep -qx 3 && echo "PASS three tries" || { echo "FAIL three tries"; pass=0; }
want "sudo -u" "uid=1001(guest) gid=100(users)"
want "unknown command" "sudo-nocmd=127"
want "the command's status" "sudo-false=1"
want "unknown target" "sudo-nouser=1"
want "gpasswd -a guest wheel, then sudo works" "root"
want "gpasswd -d" "gp-del=0"
want "gpasswd -d of a non-member" "gp-del-again=1"
want "gpasswd needs root" "gp-user=1"
want "userdel takes fay out of wheel" "wheel::10:user"
want "the script ran to the end" "useradd-done"
debugfs -R "cat /etc/passwd" $BD/zenix.hdf 2>/dev/null | cmp -s - rootfs/etc/passwd && echo "PASS /etc/passwd is as shipped again" || { echo "FAIL /etc/passwd differs from rootfs"; pass=0; }
e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1 && echo "PASS host e2fsck clean" || { echo "FAIL e2fsck"; pass=0; }
exit $((1 - pass))
