# SPDX-License-Identifier: GPL-2.0-only
# The one-system conventions (docs/audit-2026-11-23-one-system.md section 3), run by tools/cmds2-test.sh (output compared
# with tools/tests/conv.expected): a usage line goes to the error stream with status 2, an unknown option is refused,
# errors read "prog: name: reason" with the system's reason, a failure gives status 1.
T=/tmp/conv.t; rm -rf $T; mkdir -p $T
# st NAME CMD...: the status, the lines on the output and the lines on the error stream
st() { n=$1; shift; "$@" > $T/o 2> $T/e; s=$?; echo "$n st=$s out=$(wc -l < $T/o) err=$(wc -l < $T/e)"; }
st man-noargs man
st man-k-noword man -k
st sudo-badopt sudo -Z
st wc-badopt wc -Z /etc/motd
st touch-badopt touch -Z $T/x
st tee-badopt tee -Z
st tee-nine tee 1 2 3 4 5 6 7 8 9
st hexdump-badopt hexdump -Z
st umount-usage umount
st mount-usage mount /dev/nothing
st fdisk-usage fdisk
st zpkg-usage zpkg
echo x | tee $T/nodir/f > /dev/null 2> $T/e; s=$?; echo "tee-cannot st=$s $(cat $T/e)" | sed "s|$T|T|"
less $T/none > /dev/null 2> $T/e; s=$?; echo "less-none st=$s $(cat $T/e)" | sed "s|$T|T|"
chmod 644 $T/none 2>&1 | sed "s|$T|T|"
rm $T/none 2>&1 | sed "s|$T|T|"
mkdir $T/d $T/d 2>&1 | sed "s|$T|T|"
: > $T/d/f; rmdir $T/d 2>&1 | sed "s|$T|T|"
stat $T/none 2>&1 | sed "s|$T|T|"
realpath -e $T/none 2>&1 | sed "s|$T|T|"
false; echo "status-before-subst=$? $(true)"
rm -r $T
echo conv-done
