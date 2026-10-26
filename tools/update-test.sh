#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Zenix updating itself, in the emulator (DESIGN D15, docs/d15-self-updates.md section 11). A1200, Kickstart 3.1,
# 2 MB chip + 8 MB fast, an IDE disk with the FFS boot partition (zenixboot, Zenix/zenix = this build's kernel,
# version V1) and the ext2 root of this build, on which "the owner" added a line to /etc/passwd. The bundle (version
# V2) has a kernel built with ZVNUM=V2 and this build's packages, with a new /etc/passwd and /etc/motd and a /bin/hello
# that says HELLO. Each setup is one disk and one or more FS-UAE runs; the boot partition is read back on the PC
# (tools/ffsread.py) and the root checked with e2fsck -fn after each.
#   install      the kernel replaced (zenix.old kept), the FFS steps in their order with flushes, the bundle in memory
#                for zup, the root half (the owner's passwd kept, passwd.new written, motd and hello replaced), the
#                bundle's memory back in the pool (free), shells on two consoles keep working while /bin/sh is renamed
#                over; a reboot: the update note read, the bundle renamed .done; then the same bundle offered again:
#                retired at once, the real previous kernel still zenix.old
#   cold         a trial kernel that never confirms, the emulator switched off, a cold start: no evidence, no
#                rollback, a message instead
#   panic, hang  a trial kernel that panics (or resets without confirming) where it would confirm: the warm reset's
#                log is evidence, the 10 s question defaults to the previous version, both halves go back
#   previous     "Zenix (previous)" after a cut right after its first step (the bundle renamed .rejected): the old
#                kernel the default again, the root rolled back
#   powercut     the emulator killed while zup installs: the next start finishes it or rolls it back and installs
#   bad          a damaged bundle and a .part: both renamed .bad, the installed system starts
#   refused      a bundle zup refuses (it needs a newer base than any): the kernel comes up and confirms, but the
#                bundle is kept (not .done) and handed over again at the next start, where zup refuses it again
#   nozup        a root without /sbin/zup: boot says so and leaves the note FAILED; the bundle is kept the same way
# usage: tools/update-test.sh [setup...]     (ZENIX_SERIAL, ZENIX_OUT, ZENIX_BUILD as the other tests)
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-$(mktemp -d /tmp/zenix-update.XXXXXX)}; mkdir -p "$OUT"
SER=${ZENIX_SERIAL:-/tmp/zenix-update-serial}
SETUPS="${*:-install cold panic hang previous powercut bad refused nozup}"
BG=; [ $# = 0 ] && BG=1                 # all of them: side by side (each takes an emulator slot); named ones: in turn
V1=$(sed -n 's/^#define ZENIX_VNUM \([0-9]*\)u$/\1/p' kernel/version.h); V2=$((V1 + 101))
make -s BUILD=$BD $BD/zenixboot $BD/kernel.bin $BD/zenix.hdf $BD/pkg/list >"$OUT/make.log" 2>&1 || { tail "$OUT/make.log"; echo "FAIL build"; exit 1; }
B=$(cd "$BD" && pwd)
# the new version, and the trial kernels of the test builds (kernel/syscall.c DIAG_TRIAL: 1 panics, 2 resets, 3 never
# confirms where login would confirm)
make -s BUILD=$OUT/b2 ZVNUM=$V2 $OUT/b2/kernel.bin $OUT/b2/pkg/list >>"$OUT/make.log" 2>&1 || { echo "FAIL build V2"; exit 1; }
for t in 1 2 3; do
    make -s BUILD=$OUT/b2t$t ZVNUM=$V2 DIAG=-DDIAG_TRIAL=$t $OUT/b2t$t/kernel.bin >>"$OUT/make.log" 2>&1 || { echo "FAIL build trial $t"; exit 1; }
done
P=$OUT/pkg; rm -rf $P; mkdir -p $P; cp $OUT/b2/pkg/* $P/
cp rootfs/etc/passwd $OUT/passwd2; echo 'news::9:9::/var/news:/bin/sh' >>$OUT/passwd2
echo ' Zenix updated (update-test)' >$OUT/motd2
python3 -c 'import sys; d = open(sys.argv[1], "rb").read(); assert d.count(b"hello from pid") == 1
open(sys.argv[2], "wb").write(d.replace(b"hello from pid", b"HELLO from pid"))' $OUT/b2/hello.zx $OUT/hello2
python3 tools/mkzup.py repack $P base /etc/passwd=$OUT/passwd2 /etc/motd=$OUT/motd2 >/dev/null &&
python3 tools/mkzup.py repack $P utils /bin/hello=$OUT/hello2 >/dev/null || { echo "FAIL repack"; exit 1; }
for k in b2 b2t1 b2t2 b2t3; do
    python3 tools/mkzup.py bundle $OUT/$k.zup --kernel $OUT/$k/kernel.bin $P zup base utils man >/dev/null || { echo "FAIL bundle $k"; exit 1; }
done
python3 tools/mkzup.py bundle $OUT/b2min.zup --kernel $OUT/b2/kernel.bin --min 4000000000 $P zup base utils man >/dev/null ||
    { echo "FAIL bundle b2min"; exit 1; }
# the owner's root: this build's, with a line added to /etc/passwd
cp $B/zenix.hdf $OUT/root.img
cp rootfs/etc/passwd $OUT/passwd1; echo 'owner::1002:100::/home/owner:/bin/sh' >>$OUT/passwd1
printf 'rm /etc/passwd\nwrite %s /etc/passwd\nsif /etc/passwd uid 0\nsif /etc/passwd gid 0\nsif /etc/passwd mode 0100644\n' \
    $OUT/passwd1 >$OUT/dbg
debugfs -w -f $OUT/dbg $OUT/root.img >/dev/null 2>&1
cp $OUT/root.img $OUT/root-nozup.img; debugfs -w -R 'rm /sbin/zup' $OUT/root-nozup.img >/dev/null 2>&1   # setup nozup

# disk NAME BUNDLE [STARTUP]: the setup's disk (its own directory $OUT/NAME)
disk() {
    local d=$OUT/$1; rm -rf $d; mkdir -p $d/boot/S $d/boot/Zenix
    printf '%s\n' "${3:-SYS:zenixboot}" >$d/boot/S/Startup-Sequence
    cp $B/zenixboot $d/boot/zenixboot; cp $B/kernel.bin $d/boot/Zenix/zenix
    [ -n "$2" ] && cp $2 $d/boot/Zenix/zenix-update.zup
    ffs_put $1 || return 1
}
# ffs_put NAME: the boot partition made again from $OUT/NAME/boot (the files; what the owner would copy in AmigaOS)
ffs_put() {
    local d=$OUT/$1 args=()
    while IFS= read -r f; do args+=("$d/boot/$f=$f"); done < <(cd $d/boot && find . -type f | sed 's#^\./##' | sort)
    python3 tools/mkffs.py $d/ffs.img 16632K ZenixBoot "${args[@]}" >/dev/null || return 1
    if [ -f $d/disk.hdf ]; then python3 tools/ffsread.py $d/disk.hdf --part ZB0 load $d/ffs.img
    else python3 tools/mkcard.py ide $d/disk.hdf $d/ffs.img ${ROOTIMG:-$OUT/root.img} >/dev/null; fi
}
ffs_get() { rm -rf $OUT/$1/boot; mkdir -p $OUT/$1/boot; python3 tools/ffsread.py $OUT/$1/disk.hdf --part ZB0 extract $OUT/$1/boot; }
run() {     # NAME RUN SECONDS SCRIPT
    local d=$OUT/$1
    cat >$d/conf.fs-uae <<EOF
[fs-uae]
accuracy = 1
amiga_model = A1200
kickstart_file = \$ZENIX_KICKSTART_DIR/kicka1200.rom
chip_memory = 2048
fast_memory = 8192
floppy_drive_speed = 0
serial_port = /tmp/zenix-serial
uae_serial_direct = true
hard_drive_0 = $d/disk.hdf
hard_drive_0_controller = ide0
joystick_port_0 = none
joystick_port_1 = none
EOF
    ZENIX_SERIAL=$SER-$1 tools/run-test.sh $d/conf.fs-uae $3 $4 >$d/$2.log 2>&1
    tr -d '\r' <$d/$2.log | sed 's/\x1b\[[0-9;:]*[mK]//g' >$d/$2.txt
    ffs_get $1
    python3 tools/ffsread.py $d/disk.hdf --part ZR0 dump $d/root-$2.img
}
R=""                                    # the results, in order
check() { if [ "$3" = ok ]; then echo "PASS $1: $2"; else echo "FAIL $1: $2"; fi >>$OUT/$1.res; }
has() { grep -aqE -- "$3" $OUT/$1/$2.txt && echo ok || echo bad; }
same() { cmp -s "$1" "$2" && echo ok || echo bad; }
rootok() { e2fsck -fn $OUT/$1/root-$2.img >$OUT/$1/e2fsck-$2.log 2>&1 && echo ok || echo bad; }
rfile() { debugfs -R "cat $3" $OUT/$1/root-$2.img 2>/dev/null; }
rhas() { debugfs -R "stat $3" $OUT/$1/root-$2.img 2>/dev/null | grep -q '^Inode:' && echo yes || echo no; }
inorder() {   # NAME RUN REGEX...: the lines appear in this order
    local f=$OUT/$1/$2.txt n=0 l; shift 2
    for re in "$@"; do l=$(grep -anE -- "$re" $f | awk -F: -v n=$n '$1 > n { print $1; exit }'); [ -n "$l" ] || { echo bad; return; }; n=$l; done
    echo ok
}

S_PROMPT='wait zenix:/# 600'
setup_install() {
    local n=install
    disk $n $OUT/b2.zup || { check $n "the disk" bad; return; }
    cat >$OUT/$n/s1.txt <<EOF
$S_PROMPT
sleep 2
send uname; grep -c owner /etc/passwd; cat /etc/passwd.new | grep -c news; hello | head -1; free\r
sleep 5
send \x1d2
wait zenix login: 30
send root\r
wait zenix:/# 30
send echo c2-\$((1+1))\r
sleep 2
send \x1d1
sleep 1
send cp /bin/sh /tmp/sh.new && chmod 755 /tmp/sh.new && mv /tmp/sh.new /bin/sh && echo renamed-\$((2+2))\r
sleep 3
send \x1d2
sleep 1
send echo c2-\$((2+3)); ls /etc/motd\r
sleep 3
send \x1d1
sleep 1
send reboot\r
wait (?m)^klog: boot [0-9]+, 300
$S_PROMPT
sleep 2
send uname; free; echo zb\x6fk\r
sleep 4
stop
EOF
    run $n 1 900 $OUT/$n/s1.txt
    local b=$OUT/$n/boot/Zenix
    check $n "the order on the boot partition: state, zenix.new written and flushed, zenix -> zenix.old, flushed, zenix.new -> zenix, flushed, state trial" \
        "$(inorder $n 1 'update: installing version '$V2 'state installing' 'writing zenix.new' 'update: flushed' 'renamed zenix to zenix.old' 'update: flushed' 'renamed zenix.new to zenix' 'update: flushed' 'state trial')"
    check $n "the kernel reserved the bundle and gave it back after zup" "$(inorder $n 1 'update: a bundle of [0-9]+ bytes at 0x[0-9a-f]+, kept for zup' '^zup: installing zup '$V2' first' '^zup: installed Zenix .*\('$V2'\)' "update: the bundle's [0-9]+ bytes are free again")"
    check $n "the new kernel runs (uname)" "$(has $n 1 "^Zenix 0\.2 \($V2\) m68k")"
    check $n "the root half: the owner's passwd kept, passwd.new written (news), hello replaced, motd replaced" \
        "$(grep -aqx 1 $OUT/$n/1.txt && grep -aq 'HELLO from pid' $OUT/$n/1.txt && grep -aq 'Zenix updated (update-test)' $OUT/$n/1.txt && echo ok || echo bad)"
    check $n "login shows the result line, with the kept passwd" "$(has $n 1 '^zup: installed Zenix 0\.2 \('$V2'\).*kept your /etc/passwd')"
    check $n "shells on two consoles keep working while /bin/sh is renamed over" "$(grep -aq '^c2-2$' $OUT/$n/1.txt && grep -aq '^renamed-4$' $OUT/$n/1.txt && grep -aq '^c2-5$' $OUT/$n/1.txt && echo ok || echo bad)"
    local f1 f2
    f1=$(grep -a '^fast RAM free' $OUT/$n/1.txt | sed -n 1p | grep -oE '[0-9]+'); f2=$(grep -a '^fast RAM free' $OUT/$n/1.txt | sed -n 2p | grep -oE '[0-9]+')
    check $n "the bundle's memory is back in the pool: free after the update ($f1 KB) is free without one ($f2 KB)" "$([ -n "$f1" ] && [ "$f1" = "$f2" ] && echo ok || echo bad)"
    check $n "the next start reads the confirmation and renames the bundle .done" "$(inorder $n 1 'reboot: filesystems synced' 'version '$V2' came up' 'renamed zenix-update.zup to zenix-update.done' 'state confirmed')"
    check $n "boot partition: zenix is the new kernel, zenix.old the old one, the bundle .done, state confirmed" \
        "$([ "$(same $b/zenix $OUT/b2/kernel.bin)" = ok ] && [ "$(same $b/zenix.old $B/kernel.bin)" = ok ] && [ -f $b/zenix-update.done ] && [ ! -f $b/zenix-update.zup ] && grep -q '^phase confirmed$' $b/state && echo ok || echo bad)"
    check $n "the boot partition's bitmap is valid after the flushes (root block flag -1)" "$([ "$(python3 tools/ffsread.py $OUT/$n/disk.hdf --part ZB0 rootflag)" = -1 ] && echo ok || echo bad)"
    check $n "the root: e2fsck -fn clean" "$(rootok $n 1)"
    check $n "a shell prompt after the second boot" "$(has $n 1 '^zbok$')"
    # the same bundle offered again: retired at once, nothing installed, the real previous kernel kept
    cp $OUT/b2.zup $b/zenix-update.zup; ffs_put $n
    cat >$OUT/$n/s2.txt <<EOF
$S_PROMPT
sleep 2
send echo zb\x6fk\r
sleep 3
stop
EOF
    run $n 2 600 $OUT/$n/s2.txt
    check $n "the bundle offered again: retired at once, not installed again" "$(has $n 2 'version '$V2' is installed and confirmed already')"
    check $n "... and zenix.old is still the real previous kernel" "$([ "$(same $b/zenix.old $B/kernel.bin)" = ok ] && [ "$(same $b/zenix $OUT/b2/kernel.bin)" = ok ] && echo ok || echo bad)"
    check $n "... a shell prompt" "$(has $n 2 '^zbok$')"
}
setup_cold() {
    local n=cold
    disk $n $OUT/b2t3.zup || { check $n "the disk" bad; return; }
    printf '%s\nsleep 3\nstop\n' "$S_PROMPT" >$OUT/$n/s1.txt
    run $n 1 900 $OUT/$n/s1.txt
    check $n "the trial kernel came up (it never confirms: a test build)" "$(has $n 1 'diag: DIAG_TRIAL=3: not confirming')"
    printf '%s\nsleep 2\nsend uname; echo zb\\x6fk\\r\nsleep 3\nstop\n' "$S_PROMPT" >$OUT/$n/s2.txt
    run $n 2 900 $OUT/$n/s2.txt
    check $n "a cold start in the trial: no evidence, no question, a message" \
        "$(grep -aq 'was installed and not yet confirmed; if it misbehaves, choose Zenix (previous)' $OUT/$n/2.txt && ! grep -aq 'start the previous version' $OUT/$n/2.txt && echo ok || echo bad)"
    check $n "... the new kernel starts again, nothing rolled back" "$(grep -aq "^Zenix 0\.2 ($V2) m68k" $OUT/$n/2.txt && [ "$(same $OUT/$n/boot/Zenix/zenix $OUT/b2t3/kernel.bin)" = ok ] && grep -q '^phase trial$' $OUT/$n/boot/Zenix/state && grep -q '^handovers 2$' $OUT/$n/boot/Zenix/state && echo ok || echo bad)"
    check $n "... the root half still in place (passwd.new)" "$([ "$(rhas $n 2 /etc/passwd.new)" = yes ] && echo ok || echo bad)"
    check $n "the root: e2fsck -fn clean" "$(rootok $n 2)"
}
setup_fail() {   # panic or hang
    local n=$1 t=$2 what=$3
    disk $n $OUT/b2t$t.zup || { check $n "the disk" bad; return; }
    printf '%s\nsleep 2\nsend uname; grep -c owner /etc/passwd; ls /etc/passwd.new; hello | head -1; echo zb\\x6fk\\r\nsleep 4\nstop\n' "wait (?m)^klog: boot 2, 600
$S_PROMPT" >$OUT/$n/s1.txt
    run $n 1 900 $OUT/$n/s1.txt
    check $n "the trial kernel installed the root half, then failed instead of confirming" "$(inorder $n 1 '^zup: installed Zenix .*\('$V2'\)' 'diag: DIAG_TRIAL='$t': not confirming')"
    check $n "the warm reset's log is evidence: the question, the previous version by default" \
        "$(inorder $n 1 'not confirming' "Zenix $V2 $what - start the previous version\\? \\(Y in 10 s\\) previous" 'renamed zenix-update.zup to zenix-update.rejected' 'renamed zenix to zenix.rejected' 'renamed zenix.old to zenix' 'state rejected')"
    check $n "the old kernel runs and zup put the root back (the flag)" "$(inorder $n 1 'Zenix \(previous\) was chosen: putting the version before '$V2' back' "^Zenix 0\\.2 \\($V1\\) m68k")"
    check $n "the root is the old version: the owner's passwd, no passwd.new, hello says hello" \
        "$(grep -aqx 1 $OUT/$n/1.txt && grep -aq 'passwd.new: not found' $OUT/$n/1.txt && grep -aq '^hello from pid' $OUT/$n/1.txt && echo ok || echo bad)"
    local b=$OUT/$n/boot/Zenix
    check $n "boot partition: zenix the old kernel, zenix.rejected the new one, the bundle .rejected" \
        "$([ "$(same $b/zenix $B/kernel.bin)" = ok ] && [ "$(same $b/zenix.rejected $OUT/b2t$t/kernel.bin)" = ok ] && [ -f $b/zenix-update.rejected ] && [ ! -f $b/zenix.old ] && grep -q '^phase rejected$' $b/state && echo ok || echo bad)"
    check $n "the root: e2fsck -fn clean" "$(rootok $n 1)"
    check $n "a shell prompt" "$(has $n 1 '^zbok$')"
}
setup_previous() {
    local n=previous
    disk $n $OUT/b2t3.zup || { check $n "the disk" bad; return; }
    printf '%s\nsleep 3\nstop\n' "$S_PROMPT" >$OUT/$n/s1.txt
    run $n 1 900 $OUT/$n/s1.txt
    check $n "the update installed (a trial kernel that never confirms)" "$(has $n 1 '^zup: installed Zenix .*\('$V2'\)')"
    # PREVIOUS cut right after its first step: the bundle is .rejected, the kernels not yet swapped; then PREVIOUS again
    mv $OUT/$n/boot/Zenix/zenix-update.zup $OUT/$n/boot/Zenix/zenix-update.rejected
    printf 'SYS:zenixboot PREVIOUS\n' >$OUT/$n/boot/S/Startup-Sequence
    ffs_put $n
    printf '%s\nsleep 2\nsend uname; ls /etc/passwd.new; hello | head -1; echo zb\\x6fk\\r\nsleep 4\nstop\n' "$S_PROMPT" >$OUT/$n/s2.txt
    run $n 2 900 $OUT/$n/s2.txt
    check $n "PREVIOUS after a cut that renamed the bundle: the old kernel the default again" \
        "$(inorder $n 2 'renamed zenix to zenix.rejected' 'renamed zenix.old to zenix' 'state rejected')"
    check $n "... the old kernel runs and zup rolled the root back" "$(grep -aq "^Zenix 0\\.2 ($V1) m68k" $OUT/$n/2.txt && grep -aq 'passwd.new: not found' $OUT/$n/2.txt && grep -aq '^hello from pid' $OUT/$n/2.txt && echo ok || echo bad)"
    check $n "... boot partition: zenix old, zenix.rejected new" "$([ "$(same $OUT/$n/boot/Zenix/zenix $B/kernel.bin)" = ok ] && [ "$(same $OUT/$n/boot/Zenix/zenix.rejected $OUT/b2t3/kernel.bin)" = ok ] && echo ok || echo bad)"
    check $n "the root: e2fsck -fn clean" "$(rootok $n 2)"
}
setup_powercut() {
    local n=powercut d
    for d in 0 3 8 15; do               # the cut must land inside zup's install: try later and later
        disk $n $OUT/b2t3.zup || { check $n "the disk" bad; return; }
        printf 'wait (?m)^zup: installing %s \\(0\\.2\\) 900\nsleep %s\nstop\n' $V2 $d >$OUT/$n/s1.txt
        run $n 1 900 $OUT/$n/s1.txt
        [ "$(rhas $n 1 /var/lib/zpkg/journal)" = yes ] && break
    done
    check $n "the emulator killed while zup installs (a journal left; cut ${d}s into it)" "$([ "$(rhas $n 1 /var/lib/zpkg/journal)" = yes ] && echo ok || echo bad)"
    printf '%s\nsleep 2\nsend uname; grep -c owner /etc/passwd; ls /etc/passwd.new; hello | head -1; echo zb\\x6fk\\r\nsleep 4\nstop\n' "$S_PROMPT" >$OUT/$n/s2.txt
    run $n 2 900 $OUT/$n/s2.txt
    check $n "the next start: the journal finished or rolled back, then installed" \
        "$(inorder $n 2 'a journal from an install that did not end' '(finished the install of|rolled the unfinished install of) '$V2 "update: the bundle's [0-9]+ bytes are free again")"
    check $n "... the root is the new version" "$(grep -aqx 1 $OUT/$n/2.txt && grep -aq '^/etc/passwd.new' $OUT/$n/2.txt && grep -aq 'HELLO from pid' $OUT/$n/2.txt && echo ok || echo bad)"
    check $n "... no journal left, no .zup-new" "$([ "$(rhas $n 2 /var/lib/zpkg/journal)" = no ] && ! debugfs -R 'ls -l /bin' $OUT/$n/root-2.img 2>/dev/null | grep -q zup-new && echo ok || echo bad)"
    check $n "the root: e2fsck -fn clean" "$(rootok $n 2)"
}
setup_bad() {
    local n=bad
    python3 -c 'import sys; d = bytearray(open(sys.argv[1], "rb").read()); d[len(d) // 2] ^= 0x10; open(sys.argv[2], "wb").write(d)' $OUT/b2.zup $OUT/bad.zup
    disk $n $OUT/bad.zup || { check $n "the disk" bad; return; }
    head -c 1000 $OUT/b2.zup >$OUT/$n/boot/Zenix/zenix-update.zup.part; ffs_put $n
    printf '%s\nsleep 2\nsend uname; echo zb\\x6fk\\r\nsleep 3\nstop\n' "$S_PROMPT" >$OUT/$n/s1.txt
    run $n 1 900 $OUT/$n/s1.txt
    check $n "a .part (a copy cut off) is renamed .bad" "$(has $n 1 'zenix-update.zup.part was not copied to the end: renamed zenix-update.bad')"
    check $n "a damaged bundle is renamed .bad with the reason" "$(has $n 1 'zenix-update.zup is not used: a part does not match its SHA-256')"
    check $n "the installed system starts" "$(grep -aq "^Zenix 0\\.2 ($V1) m68k" $OUT/$n/1.txt && [ "$(same $OUT/$n/boot/Zenix/zenix $B/kernel.bin)" = ok ] && [ ! -f $OUT/$n/boot/Zenix/zenix.old ] && echo ok || echo bad)"
}
# refused / nozup: the root half fails (zup refuses the bundle, or there is no zup), the kernel confirms (login on
# console 1); the next start must keep the bundle and hand it over again (audit3 B1: it was retired as done)
setup_rootfail() {
    local n=$1 zup=$2 why=$3
    disk $n $zup || { check $n "the disk" bad; return; }
    cat >$OUT/$n/s1.txt <<EOF
$S_PROMPT
sleep 2
send reboot\r
wait (?m)^klog: boot 2, 300
wait (?m)^login: warning: root has no password 900
sleep 3
send uname; echo zb\x6fk\r
sleep 3
stop
EOF
    run $n 1 1500 $OUT/$n/s1.txt
    local b=$OUT/$n/boot/Zenix
    check $n "the kernel installed, the root half failed ($why)" "$(inorder $n 1 'update: installing version '$V2 'state trial' "$why")"
    check $n "the next start: it came up, but the bundle is kept and handed over again" \
        "$(inorder $n 1 'reboot: filesystems synced' 'Zenix '$V2' came up, but its root half was not installed' 'version '$V2' in trial: the bundle goes to Zenix again' "$why")"
    check $n "... not retired: no 'the update is done', no .done" "$(! grep -aq 'the update is done' $OUT/$n/1.txt && [ -f $b/zenix-update.zup ] && [ ! -f $b/zenix-update.done ] && grep -q '^phase trial$' $b/state && echo ok || echo bad)"
    check $n "... the new kernel runs, a shell prompt" "$(grep -aq "^Zenix 0\.2 ($V2) m68k" $OUT/$n/1.txt && grep -aq '^zbok$' $OUT/$n/1.txt && echo ok || echo bad)"
    check $n "the root: e2fsck -fn clean" "$(rootok $n 1)"
}

one() {
    case $1 in
    install) setup_install ;;
    cold) setup_cold ;;
    panic) setup_fail panic 1 crashed ;;
    hang) setup_fail hang 2 'did not come up \(reset before its login\)' ;;
    previous) setup_previous ;;
    powercut) setup_powercut ;;
    bad) setup_bad ;;
    refused) setup_rootfail refused $OUT/b2min.zup 'zup: the bundle needs base 4000000000 or newer' ;;
    nozup) ROOTIMG=$OUT/root-nozup.img setup_rootfail nozup $OUT/b2.zup 'boot: no /sbin/zup: the update is not installed' ;;
    *) echo "FAIL unknown setup $1" >$OUT/$1.res ;;
    esac
}
for s in $SETUPS; do
    rm -f $OUT/$s.res
    if [ -n "$BG" ]; then one $s & sleep 2; else one $s; fi
done
wait
pass=1
for s in $SETUPS; do cat $OUT/$s.res 2>/dev/null || { echo "FAIL $s: no result"; pass=0; }; grep -q '^FAIL' $OUT/$s.res 2>/dev/null && pass=0; done
echo "logs in $OUT"
exit $((1 - pass))
