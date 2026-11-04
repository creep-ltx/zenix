# SPDX-License-Identifier: GPL-2.0-only
# The box (DESIGN D25 revision 3, 10 step 1; tools/box-test.sh): every name the box answers to is started once from
# /bin, without arguments or with a harmless one, stdin empty; none may get the box's "no command named" answer. Then a
# name the box does not know: the list of names and status 2. reboot and bootamigaos are not run (they would restart
# the machine); halt, poweroff and shutdown get a wrong argument, so they answer with their usage.
T=/tmp/box.t
rm -rf $T; mkdir -p $T; cd $T
bad=0; n=0
for c in ls cat cp mv rm mkdir rmdir ln echo pwd ps free uname uptime date clear kill dmesg sleep true false env \
         hostname id whoami touch umount head tail wc ifconfig route ping host netstat \
         df chmod stat du find xargs sed tr cut uniq seq which printf basename dirname tee cmp sort grep less man gzip \
         tar readlink expr pgrep tree cal time more gunzip zcat pkill ifup ifdown; do
    $c < /dev/null > /dev/null 2> $T/err
    if grep -q "no command named" $T/err; then echo "box: $c not dispatched"; bad=$((bad + 1)); fi
    n=$((n + 1))
done
for c in halt poweroff; do
    $c x < /dev/null > /dev/null 2> $T/err
    grep -q "usage: $c" $T/err || { echo "box: $c did not answer as $c"; bad=$((bad + 1)); }
    n=$((n + 1))
done
shutdown -x < /dev/null > /dev/null 2> $T/err
grep -q "usage: shutdown" $T/err || { echo "box: shutdown did not answer as shutdown"; bad=$((bad + 1)); }
n=$((n + 1))
echo "box: $n names run, $bad not dispatched"
ln /bin/ls $T/nosuch
$T/nosuch > $T/out 2>&1
echo "box: unknown name status $?"
grep -c "no command named 'nosuch'" $T/out
grep -c gunzip $T/out
cd /; rm -rf $T
echo box-done
