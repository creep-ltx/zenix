# SPDX-License-Identifier: GPL-2.0-only
# Audit D checks run as a normal user by tools/users-test.sh (root left a "sleep" running): raw messages, kill and
# stop of others' and the system's processes, the raw disk, the clock; root prepared /tmp/sgd (a set-group-id
# directory, group root, with a 6777 program) and /tmp/mvt (a root directory the user may not write, in a parent
# the user may) and /tmp/priv (0700: the user may not look inside). The output is compared there.
badptr ipc
kill 1; echo kill-1=$?
kill $(pgrep sleep); echo kill-other=$?
dd if=ide0 of=/tmp/raw count=1; echo dd=$?
rm -f /tmp/raw
date -s "2030-01-01 00:00:00" > /dev/null; echo date=$?
touch /tmp/sgd/f; mkdir /tmp/sgd/d
echo more >> /tmp/sgd/prog
chmod 2755 /tmp/sgd/f
ls -l /tmp/sgd/f /tmp/sgd/prog
ls -ld /tmp/sgd/d
mv /tmp/mvt/src/dd /tmp/mvt/dst/dd 2> /dev/null; echo mv-dir=$?
test -d /tmp/mvt/src/dd; echo mv-dir-stays=$?
find /tmp/priv/x; du /tmp/priv/x; cp /tmp/priv/x /tmp/px; grep a /tmp/priv/x; diff /tmp/priv/x /etc/group
echo 'echo rc-ran-as-$USER' > /home/user/.shrc
echo perm-user-done
