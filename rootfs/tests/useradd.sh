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
gpasswd -a anna users; echo gp-add=$?
gpasswd -a anna users; echo gp-again=$?
echo; grep '^users:' /etc/group
userdel -r anna; echo del-anna=$?
grep -c '^anna:' /etc/passwd
echo; grep '^users:' /etc/group
ls -d /home/anna; echo anna-home=$?
userdel dave; echo del-dave=$?
userdel root; echo del-root=$?
userdel nosuch; echo del-none=$?
grep -c . /etc/passwd
# sudo: user is in wheel (shipped), guest is not; fay is made a wheel member with -G and gets a password
echo; echo 'sudo id; echo sudo-user=$?' | su user
echo 'sudo id; echo sudo-guest=$?' | su guest
useradd -G wheel,users fay; echo add-fay=$?
echo; grep '^wheel:' /etc/group
printf 'pw1\npw1\n' | passwd fay > /dev/null; echo pw-fay=$?
echo 'echo pw1 | sudo id; echo sudo-pw=$?' | su fay
echo 'printf "a\nb\nc\n" | sudo id; echo sudo-wrong=$?' | su fay
echo 'echo pw1 | sudo -u guest id' | su fay
echo 'echo pw1 | sudo nosuchcmd; echo sudo-nocmd=$?' | su fay
echo 'echo pw1 | sudo false; echo sudo-false=$?' | su fay
echo 'echo pw1 | sudo -u nobody id; echo sudo-nouser=$?' | su fay
gpasswd -a guest wheel; echo 'sudo whoami' | su guest
gpasswd -d guest wheel; echo gp-del=$?
gpasswd -d guest wheel; echo gp-del-again=$?
echo 'gpasswd -a guest wheel; echo gp-user=$?' | su user
userdel -r fay; echo del-fay=$?
echo; grep '^wheel:' /etc/group
echo useradd-done
