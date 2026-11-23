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
# every command in /bin (audit OS-20, OS-24, OS-26): --help writes the usage to the output with status 0 and nothing
# to the error stream; --version writes "NAME 0.2" with status 0; an unknown option is refused with status 2, nothing
# on the output and "NAME: unknown option --bogus" on the error stream. Only failures are listed, then the counts.
# Not run: the system's own programs (boot, diskd, init, the test daemons) and gridrun (a Lua script). Not asked the
# unknown option: the commands whose arguments are data (echo, true, false, printf, expr, yes, seq, chmod's modes,
# kill's signals, find's expression), less (it takes other pagers' options and ignores them), the test programs, the ported programs (upstream's own rules), and reboot's
# names and zinstall (a mistake there would not be harmless). Each is run as /bin/NAME: echo, pwd, true, false and
# kill are the shell's own too.
skip=" boot diskd init msgpingd socktestd gridrun "
nobogus=" echo true false printf expr yes seq chmod kill find hello spin crash badptr msgping bench termtest priotest "
nobogus="$nobogus sigtest ptytest gcontest dpytest nettest cryptotest svctest socktest sumi maki markdown ahiru ctop "
nobogus="$nobogus lua make tcc frotz adventure arithmetic banner fortune hangman number rain robots tetris worm worms "
nobogus="$nobogus wump reboot halt poweroff shutdown bootamigaos zinstall less more "
nh=0; nv=0; nb=0
for c in $(ls /bin); do
    case "$skip" in *" $c "*) continue;; esac
    /bin/$c --help > $T/o 2> $T/e; s=$?
    if [ $s != 0 ] || [ "$(wc -l < $T/e)" != 0 ] || [ "$(head -c 7 $T/o)" != "usage: " ]; then
        echo "help-bad $c st=$s err=$(wc -l < $T/e) first=$(head -1 $T/o)"
    else nh=$((nh + 1)); fi
    /bin/$c --version > $T/o 2> $T/e; s=$?
    if [ $s != 0 ] || [ "$(cat $T/o)" != "$c 0.2" ] || [ "$(wc -l < $T/e)" != 0 ]; then
        echo "version-bad $c st=$s out=$(cat $T/o)"
    else nv=$((nv + 1)); fi
    case "$nobogus" in *" $c "*) continue;; esac
    /bin/$c --bogus < /dev/null > $T/o 2> $T/e; s=$?
    if [ $s != 2 ] || [ "$(wc -l < $T/o)" != 0 ] || ! grep -q "^$c: unknown option --bogus\$" $T/e; then
        echo "bogus-bad $c st=$s out=$(wc -l < $T/o) err=$(head -1 $T/e)"
    else nb=$((nb + 1)); fi
done
echo "conventions: --help $nh, --version $nv, unknown options refused $nb"
rm -r $T
echo conv-done
