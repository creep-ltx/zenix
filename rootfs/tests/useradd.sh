# SPDX-License-Identifier: GPL-2.0-only
# useradd and userdel, run as root by tools/useradd-test.sh (the output is checked there)
useradd -c "Test User" anna; echo add-anna=$?
grep '^anna:' /etc/passwd
ls -ld /home/anna
useradd anna; echo add-again=$?
useradd -u 1000 bob; echo add-taken-uid=$?
useradd 9bad; echo add-bad-name=$?
useradd -g nosuch carl; echo add-no-group=$?
useradd -M -u 2000 -g 100 -s /bin/sh dave; echo add-dave=$?
grep '^dave:' /etc/passwd
ls -d /home/dave; echo dave-home=$?
echo 'id' | su anna
echo 'useradd eve; echo user-add=$?' | su user
sed 's/^users::100:.*/&,anna/' /etc/group > /tmp/g; cp /tmp/g /etc/group; rm /tmp/g
echo; grep '^users:' /etc/group
userdel -r anna; echo del-anna=$?
grep -c '^anna:' /etc/passwd
echo; grep '^users:' /etc/group
ls -d /home/anna; echo anna-home=$?
userdel dave; echo del-dave=$?
userdel root; echo del-root=$?
userdel nosuch; echo del-none=$?
grep -c . /etc/passwd
echo useradd-done
