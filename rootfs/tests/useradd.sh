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
grep '^users:' /etc/group
userdel -r anna; echo del-anna=$?
grep -c '^anna:' /etc/passwd
grep '^users:' /etc/group
ls -d /home/anna; echo anna-home=$?
userdel dave; echo del-dave=$?
userdel root; echo del-root=$?
userdel nosuch; echo del-none=$?
grep -c . /etc/passwd
# sudo: user is in wheel (shipped), guest is not; fay is made a wheel member with -G and gets a password
echo 'sudo id; echo sudo-user=$?' | su user
echo 'sudo id; echo sudo-guest=$?' | su guest
useradd -G wheel,users fay; echo add-fay=$?
grep '^wheel:' /etc/group
printf 'pw1\npw1\n' | passwd fay > /dev/null; echo pw-fay=$?
echo 'echo pw1 | sudo id; echo sudo-pw=$?' | su fay
echo 'printf "a\nb\nc\n" | sudo id; echo sudo-wrong=$?' | su fay
# the 5 minutes after a right password (owner 2026-10-31): the same shell's next sudo asks nothing, -k forgets
echo 'echo pw1 | sudo true; printf "x\n" | sudo true; echo sudo-stamp=$?; sudo -k; printf "x\n" | sudo true; echo sudo-forgot=$?' | su fay
echo 'echo pw1 | sudo -u guest id' | su fay
echo 'echo pw1 | sudo nosuchcmd; echo sudo-nocmd=$?' | su fay
echo 'echo pw1 | sudo false; echo sudo-false=$?' | su fay
echo 'echo pw1 | sudo -u nobody id; echo sudo-nouser=$?' | su fay
gpasswd -a guest wheel; echo 'sudo whoami' | su guest
gpasswd -d guest wheel; echo gp-del=$?
gpasswd -d guest wheel; echo gp-del-again=$?
echo 'gpasswd -a guest wheel; echo gp-user=$?' | su user
userdel -r fay; echo del-fay=$?
grep '^wheel:' /etc/group
# supplementary groups (2026-10-31): gail's own group is users, wheel and zine are her second and third; su (as login
# and sudo) gives a process every group /etc/group lists the user in, and each one counts for the files
useradd -G wheel,zine gail; echo add-gail=$?
mkdir /tmp/gt; echo w > /tmp/gt/w; chgrp wheel /tmp/gt/w; chmod 640 /tmp/gt/w
echo z > /tmp/gt/z; chgrp zine /tmp/gt/z; chmod 660 /tmp/gt/z; echo r > /tmp/gt/r; chmod 640 /tmp/gt/r
echo 'id | cut -d" " -f3; cat /tmp/gt/w; echo z2 >> /tmp/gt/z; echo zw=$?; cat /tmp/gt/r; echo r=$?' | su gail
cat /tmp/gt/z
echo 'sudo id | cut -d" " -f1,2; echo sudo-gail=$?' | su gail
id gail | cut -d" " -f3
echo 'id | cut -d" " -f3' | su guest
gpasswd -d gail wheel > /dev/null; echo 'cat /tmp/gt/w; echo w-after=$?' | su gail
userdel gail; echo del-gail=$?
rm -r /tmp/gt
# sh with a pipe as its input (audit 2026-10-26 F4): no prompt, so a piped su's output is whole lines; -i gives the
# prompt (and the line editor) all the same
echo 'echo via-pipe' | sh > /tmp/ua.sh1 2>&1; echo "pipe-out=$(cat /tmp/ua.sh1)"
echo 'echo via-i' | sh -i > /tmp/ua.sh2 2>&1; grep -q 'zenix:/# ' /tmp/ua.sh2 && echo sh-i-prompt=yes || echo sh-i-prompt=no
grep -c '^via-i$' /tmp/ua.sh2 | sed 's/^/sh-i-out=/'
rm -f /tmp/ua.sh1 /tmp/ua.sh2
echo useradd-done
