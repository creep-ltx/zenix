#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The system update from Zenix itself (DESIGN D22 4.5, as built 2026-10-31): zpkg -Syu writes the system bundle onto
# the AmigaDOS boot partition (Zenix/zenix-update.zup and its .sig, user/zpkg/bootfs.c), zenixboot installs it at the
# next start (D15). In FS-UAE, against a test repository (the test channel, signed with the TEST key, tools/mkrepo.py)
# served by tools/host/http-test-server.py in a user, mount and network namespace (the Amiga reaches it as 10.0.2.2
# through FS-UAE's A2065), on an A1200 (68EC020; part b with 8 MB of fast RAM as the PiStorm and the 060 have, part m
# with 2 MB of chip RAM alone). On 2 MB of chip RAM alone AmigaOS's start reuses the top of chip RAM, so zenixboot
# never sees the new system's confirmation and retires an update after its third start (D15's rule): found here on
# 2026-10-31; zpkg's message for a waiting update says "at most three starts later".
#   b   the owner's way, from the system the owner runs (an image built from git commit $ZENIX_SYSUP_OLD, default
#       381e9b1: Zenix 2026103000, whose zpkg has no boot partition writer and whose zenixboot is the one on the owner's
#       card): an IDE disk with an RDB (ZB0 the FFS boot partition with that zenixboot and kernel, ZR0 the ext2 root);
#       "zpkg -Syu" (the old zpkg) brings zpkg-new and only downloads the system; "zpkg-new -Syu" writes it to ZB0;
#       "reboot": zenixboot installs it; "zpkg -Syu" after the restart installs what waited for it; then a second
#       update V2 with the new /bin/zpkg alone: refused while the last one is still on the partition ("restart"),
#       written after the restart, installed at the next. After each step the boot partition is checked on the PC
#       (tools/ffsread.py check: every block, chain and checksum, the bitmap) and by amitools' xdfscan; the root by
#       e2fsck -fn
#   m   the owner's card layout (tools/mkcard.py sd: an MBR, the RDB inside its partition of type 0x76, the root a PC
#       partition), Zenix started from the floppy: zpkg -Syu finds the boot partition inside the MBR's RDB and writes
#       the update there; then a disk without a boot partition: zpkg says what it cannot do, writes nothing
#   z   a ZenixFS root (DESIGN D34; the disk made by mkcard.py ide, kernel-zfs behind zenixboot, version $VZ): zpkg -Syu takes the
#       list's systemz line: a program installed live, zenixz-$V1 written to the boot partition; reboot: zenixboot installs it,
#       zup the root half; then zpkg -Syu installs what waited; the root's fsck on the PC clean
# usage: tools/sysupdate-test.sh [PART...]     (ZENIX_SERIAL, ZENIX_OUT, ZENIX_BUILD as the other tests)
# Needs git (the old commit), debugfs, e2fsck, resize2fs, openssl, unprivileged user namespaces (as zpkg-emu-test);
# amitools' xdfscan where installed.
cd "$(dirname "$0")/.." || exit 1
grep() { /usr/bin/grep "$@"; }
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-/tmp/zenix-sysup}; mkdir -p "$OUT"
OLD=${ZENIX_SYSUP_OLD:-381e9b1}
SER=${ZENIX_SERIAL:-/tmp/zenix-sysup-serial}
export ZSIGN_TEST_DIR=$PWD/build/sign-test          # one TEST key for the old build, the new ones and the repository
make -s BUILD=$BD all $BD/zenixboot >"$OUT/make.log" 2>&1 || { tail "$OUT/make.log"; echo "FAIL the build"; exit 1; }
OUT=$(cd "$OUT" && pwd); BD=$(cd "$BD" && pwd)
for t in openssl debugfs e2fsck resize2fs git; do command -v $t >/dev/null || { echo "FAIL $t is not installed"; exit 1; }; done
unshare --map-root-user --map-auto -nm true 2>/dev/null || { echo "FAIL unshare --map-root-user --map-auto fails (newuidmap, /etc/subuid)"; exit 1; }
pass=1
KEY=$ZSIGN_TEST_DIR/zenix-test.key
python3 tools/zsign.py testkey >/dev/null || { echo "FAIL the test key"; exit 1; }
today=$(date +%Y%m%d)
R() { echo "$1" >>"$OUT/results"; case $1 in FAIL*) pass=0 ;; esac; echo "$1"; }
check() { if [ "$2" = ok ]; then R "PASS $1"; else R "FAIL $1"; fi; }
: >"$OUT/results"

# ---- the builds: the owner's (old), this one as V1, and as V2 (with a new motd in base) ----
V0=2026103000
V1=2026103101; V2=2026103102
O=$OUT/old
if [ ! -f "$O/build/zenix.hdf" ] || [ "$(cat "$O/commit" 2>/dev/null)" != "$(git rev-parse "$OLD")" ]; then
    rm -rf "$O"; mkdir -p "$O/src"
    git archive "$OLD" | tar -x -C "$O/src" || { echo "FAIL git archive $OLD"; exit 1; }
    SOURCE_DATE_EPOCH=$(git log -1 --format=%ct "$OLD") make -s -C "$O/src" BUILD="$O/build" -j8 \
        "$O/build/zenixboot" "$O/build/kernel.bin" "$O/build/zenix.hdf" "$O/build/pkg/list" >"$OUT/make-old.log" 2>&1 ||
        { tail "$OUT/make-old.log"; echo "FAIL the build of $OLD"; exit 1; }
    git rev-parse "$OLD" >"$O/commit"
    rm -rf "$O/src"
fi
OV=$(python3 -I -c 'import sys; sys.path.insert(0, "tools"); import mkzup; print(mkzup.kernel_version(open(sys.argv[1], "rb").read())[0])' "$O/build/kernel.bin")
[ "$OV" = $V0 ] || { echo "FAIL the old build says version $OV, not $V0"; exit 1; }
# One build date for V1 and V2 (2026-11-04): the Makefile takes it from the tree's last commit, and wget, sntp, reboot
# (sysutils) and zpkg carry it; a commit in the main tree between the two builds made those three packages "changed"
# in V2, so they waited for V2 and the last zpkg -Syu installed them (seen once in a fast-mode run, read as a timing
# question). The repository check below says so at once if V2 ever changes more than the base group again.
SDE=$(git log -1 --format=%ct)
SOURCE_DATE_EPOCH=$SDE make -s BUILD=$OUT/b1 ZVNUM=$V1 $OUT/b1/kernel.bin $OUT/b1/kernel-zfs.bin $OUT/b1/pkg/list >>"$OUT/make.log" 2>&1 || { echo "FAIL build V1"; exit 1; }
SOURCE_DATE_EPOCH=$SDE make -s BUILD=$OUT/b2 ZVNUM=$V2 $OUT/b2/kernel.bin $OUT/b2/pkg/list >>"$OUT/make.log" 2>&1 || { echo "FAIL build V2"; exit 1; }
echo ' Zenix updated twice (sysupdate-test)' >$OUT/motd2
python3 tools/mkzup.py repack $OUT/b2/pkg base /etc/motd=$OUT/motd2 >/dev/null || { echo "FAIL repack"; exit 1; }

# ---- the repository: r = the old system, then V1; r2 = r, then V2 ----
W=$OUT/www; rm -rf $W; mkdir -p $W
{ python3 tools/mkrepo.py add $W/r test $O/build --key $KEY --date $today &&
  python3 tools/mkrepo.py add $W/r test $OUT/b1 --key $KEY --date $today &&
  cp -a $W/r $W/r2 && python3 tools/mkrepo.py add $W/r2 test $OUT/b2 --key $KEY --date $today &&
  python3 tools/mkrepo.py check $W/r2 --test-pub $ZSIGN_TEST_DIR/zenix-test.pub; } >$OUT/repo.log 2>&1 ||
    { cat $OUT/repo.log; echo "FAIL the repository"; exit 1; }
SYS1=$W/r/pool/zenix-$V1.zup; SYS2=$W/r2/pool/zenix-$V2.zup
# V2 differs from V1 only in base (its motd): any other package mkrepo calls changed would wait for V2 and make the
# last "nothing to do" fail for a reason that is not the update path's
grep -q "^test: system $V2 (new); changed: none$" $OUT/repo.log ||
    { grep "changed:" $OUT/repo.log | tail -1; echo "FAIL the repository: V2 changes more than the base group (the builds differ)"; exit 1; }
grep -q "^pkg zpkg-new" $W/r/test/index-* && grep -A8 "^pkg sysutils" $(ls $W/r/test/index-* | grep -v sig | sort | tail -1) | grep -q "^depends zpkg-new" ||
    { echo "FAIL the repository has no zpkg-new, or sysutils does not depend on it"; exit 1; }
echo "sysupdate-test: the repository: system $V0 -> $V1 -> $V2; $(grep -c '^pkg ' $(ls $W/r/test/index-* | grep -v sig | sort | tail -1)) packages"

get() { debugfs -R "cat $2" $1 2>/dev/null | tr -d '\r'; }
put() {     # put IMG FILE PATH MODE
    debugfs -w -R "rm $3" $1 >/dev/null 2>&1
    debugfs -w -R "write $2 $3" $1 >/dev/null 2>&1
    debugfs -w -R "sif $3 uid 0" $1 >/dev/null 2>&1
    debugfs -w -R "sif $3 gid 0" $1 >/dev/null 2>&1
    debugfs -w -R "sif $3 mode 0100$4" $1 >/dev/null 2>&1
}
mkd() { debugfs -w -R "mkdir $2" $1 >/dev/null 2>&1; debugfs -w -R "sif $2 uid 0" $1 >/dev/null 2>&1; debugfs -w -R "sif $2 mode 040755" $1 >/dev/null 2>&1; }
root_img() {    # IMG FROM: a root of 16 MB (room for the downloads and the kept versions), the network, the scripts
    local img=$1
    cp $2 $img
    e2fsck -fy $img >/dev/null 2>&1; resize2fs $img 16M >/dev/null 2>&1 || return 1
    mkd $img /etc/network; mkd $img /tests
    printf 'auto eth0\niface eth0 inet dhcp\n    driver a2065\n' >$OUT/interfaces
    put $img $OUT/interfaces /etc/network/interfaces 644
    for f in $OUT/s*.sh $OUT/conf.*; do put $img $f /tests/$(basename $f) 644; done
}
# the scripts on the Amiga: each prints its markers and keeps its output in /tests/oN (read back on the PC)
cat >$OUT/net.sh <<'EOS'
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    ifconfig 2>/dev/null | grep -q 'lease [0-9]\|inet 10.0.2.15' && break
    sleep 1
done
EOS
step() {        # step N CMD...: one command, its output and status into /tests/oN and onto the console
    printf 'echo ==o%s; %s > /tests/o%s 2>&1; echo r%s=$? >> /tests/o%s; cat /tests/o%s; sync\n' "$1" "$2" "$1" "$1" "$1" "$1"
}
{ cat $OUT/net.sh; echo 'cp /tests/conf.r /etc/zpkg.conf; zpkg -Qi zpkg > /tests/o0 2>&1'
  step 1 'zpkg -Syu'; step 2 'zpkg-new -Syu'; step 3 'zpkg -Qi zpkg-new sysutils'; echo 'echo s1-d""one'; } >$OUT/s1.sh
{ cat $OUT/net.sh; step 4 'uname -a'; step 5 'zpkg -Syu'; step 6 'zpkg -Qi zpkg sysutils base'
  echo 'cp /tests/conf.r2 /etc/zpkg.conf'; step 7 'zpkg -Syu'; echo 'echo s2-d""one'; } >$OUT/s2.sh
{ cat $OUT/net.sh; step 8 'zpkg -Syu'; echo 'echo s3-d""one'; } >$OUT/s3.sh
{ cat $OUT/net.sh; step 9 'uname -a'; step 10 'cat /etc/motd'; step 11 'zpkg -Qi base zpkg'; step 12 'zpkg -Syu'
  echo 'dmesg > /tests/dmesg; sync; echo s4-d""one'; } >$OUT/s4.sh
{ cat $OUT/net.sh; echo 'cp /tests/conf.r /etc/zpkg.conf'; step 21 'zpkg -Syu'; echo 'echo m-d""one'; } >$OUT/sm.sh

boot_check() {  # NAME DISK PARTIMG WHAT FRESH: the FFS boot partition on the PC: the whole volume, and amitools agrees
                # (nothing it says of the fresh volume FRESH it did not say before)
    local n=$1 img=$3
    python3 tools/ffsread.py $2 --part ZB0 dump $img 2>/dev/null || python3 tools/ffsread.py $2 dump $img
    local o; o=$(python3 tools/ffsread.py $img check 2>&1)
    local x=ok
    if command -v xdfscan >/dev/null; then    # (its complaints about the fresh image, mkffs's: bits past the end, ignored)
        cp $img $img.hdf; xdfscan -v $img.hdf 2>&1 | grep -a 'ERROR\|WARN' | sort >$img.x; rm -f $img.hdf
        cp $5 $img.hdf; xdfscan -v $img.hdf 2>&1 | grep -a 'ERROR\|WARN' | sort >$img.x0; rm -f $img.hdf
        [ -z "$(comm -23 $img.x $img.x0)" ] || x=bad
    fi
    check "$n: $4: the boot partition is a valid volume on the PC ($o; amitools' xdfscan: $x)" \
        "$([ "$o" = "ok, 0 leaked blocks" ] && [ $x = ok ] && echo ok || echo bad)"
}
has() { grep -aqE -- "$2" "$1" && echo ok || echo bad; }
inorder() {   # FILE REGEX...: the lines appear in this order
    local f=$1 n=0 l; shift
    for re in "$@"; do l=$(grep -anE -- "$re" $f | awk -F: -v n=$n '$1 > n { print $1; exit }'); [ -n "$l" ] || { echo bad; return; }; n=$l; done
    echo ok
}

# runs inside the namespace: the test server, then FS-UAE (inner NAME CONF SECONDS KEYSCRIPT PORT)
inner() {
    cat >$OUT/inner-$1.sh <<EOS
#!/usr/bin/env bash
ip link set lo up
python3 -I tools/host/http-test-server.py $W $OUT/port-$1 $5 & srv=\$!
for i in \$(seq 50); do [ -s $OUT/port-$1 ] && break; sleep 0.1; done
ZENIX_SERIAL=$SER-$1 ZENIX_BUILD=$BD tools/run-test.sh "$2" $3 "$4"
rc=\$?
kill \$srv
exit \$rc
EOS
    chmod +x $OUT/inner-$1.sh
}
fsconf() {      # OUT DISK [FLOPPY]: the A1200 with the A2065 (and FAST KB of fast RAM), booted from DISK (and FLOPPY)
    sed -e '/^\[fs-uae\]/a network_card = a2065' -e "s|^hard_drive_0 = .*|hard_drive_0 = $2|" \
        -e "s|^fast_memory = .*|fast_memory = ${FAST:-0}|" fsuae/a1200-020.fs-uae |
        if [ -n "$3" ]; then sed "s|^floppy_drive_0 = .*|floppy_drive_0 = $3|"; else grep -v '^floppy_drive_0'; fi >$1
}
# the server's port: a fixed one per part (each run has a network namespace of its own)
conf_files() {
    printf 'channel test\nserver http://10.0.2.2:%s/r\n' $1 >$OUT/conf.r
    printf 'channel test\nserver http://10.0.2.2:%s/r2\n' $1 >$OUT/conf.r2
}

part_b() {
    local n=b d=$OUT/b; rm -rf $d; mkdir -p $d/boot/S $d/boot/Zenix
    conf_files 8098
    root_img $d/root.img $O/build/zenix.hdf || { R "FAIL b: the root image"; return; }
    printf 'SYS:zenixboot\n' >$d/boot/S/Startup-Sequence
    cp $O/build/zenixboot $d/boot/zenixboot; cp $O/build/kernel.bin $d/boot/Zenix/zenix
    python3 tools/mkffs.py $d/ffs.img 16632K ZenixBoot $d/boot/S/Startup-Sequence=S/Startup-Sequence \
        $d/boot/zenixboot=zenixboot $d/boot/Zenix/zenix=Zenix/zenix >/dev/null &&
        python3 tools/mkcard.py ide $d/disk.hdf $d/ffs.img $d/root.img >/dev/null || { R "FAIL b: the disk"; return; }
    FAST=8192 fsconf $d/b.fs-uae $d/disk.hdf      # (on 2 MB of chip RAM alone the confirmation is lost: see the notes)
    cat >$d/keys.txt <<EOF
wait zenix:/# 900
sleep 2
send sh /tests/s1.sh\r
wait (?m)^s1-done 3000
send reboot\r
wait (?m)^klog: boot [0-9]+, 300
wait zenix:/# 1500
sleep 2
send sh /tests/s2.sh\r
wait (?m)^s2-done 3000
send reboot\r
wait (?m)^klog: boot [0-9]+, 300
wait zenix:/# 900
sleep 2
send sh /tests/s3.sh\r
wait (?m)^s3-done 3000
send reboot\r
wait (?m)^klog: boot [0-9]+, 300
wait zenix:/# 1500
sleep 2
send sh /tests/s4.sh\r
wait (?m)^s4-done 3000
stop
EOF
    inner b $d/b.fs-uae 9000 $d/keys.txt 8098
    unshare --map-root-user --map-auto -nm $OUT/inner-b.sh >$d/run.log 2>&1 || R "FAIL b: the run ($(grep -a 'FAIL\|timed out' $d/run.log | head -2 | tr '\n' ' '))"
    tr -d '\r' <$d/run.log | sed 's/\x1b\[[0-9;:]*[mK]//g' >$d/log.txt
    python3 tools/ffsread.py $d/disk.hdf --part ZR0 dump $d/root-after.img
    for i in 0 1 2 3 4 5 6 7 8 9 10 11 12; do get $d/root-after.img /tests/o$i >$d/o$i; done
    get $d/root-after.img /tests/dmesg >$d/dmesg
    local L=$d/log.txt
    check "b: the old system (zpkg $V0, the image of $OLD) runs" "$(has $d/o0 "^Version .*\($V0\)")"
    check "b: 1. zpkg -Syu with the old zpkg: zpkg-new installed live, the rest waits for Zenix $V1, the system only downloaded" \
        "$([ "$(has $d/o1 '^zpkg: 1 package installed')" = ok ] && [ "$(has $d/o1 'wait.* for the system update')" = ok ] &&
            [ "$(has $d/o1 "^r1=0")" = ok ] && [ "$(grep -ac "^Version .*($V1)\|^Version .*($V0)" $d/o3)" = 2 ] &&
            [ "$(has $d/o3 "^Name *: zpkg-new")" = ok ] && echo ok || echo bad)"
    check "b: 2. zpkg-new -Syu writes Zenix $V1 to the boot partition (read back and checked)" \
        "$([ "$(inorder $d/o2 "writing Zenix .*\($V1\) to the boot partition \(ide0p1, Zenix/zenix-update.zup" "Zenix .*\($V1\) is ready on the boot partition")" = ok ] &&
            [ "$(has $d/o2 '^r2=0')" = ok ] && echo ok || echo bad)"
    local nw; nw=$(grep -ac '^zpkg:   ' $d/o2)
    check "b: ... its last line says what is left: reboot, then zpkg -Syu again ($nw packages wait)" \
        "$([ "$nw" -gt 0 ] && [ "$(grep -a '^zpkg: ' $d/o2 | tail -1)" = "zpkg: to finish: reboot, then zpkg -Syu again ($nw packages wait for the new system)" ] && echo ok || echo bad)"
    check "b: 3. reboot: the owner's zenixboot checks the signature, installs the kernel, zup the root half" \
        "$(inorder $L 'sh /tests/s1.sh' 'update: signed by test key' "update: installing version $V1" 'renamed zenix.new to zenix' "^zup: installed Zenix .*\($V1\)")"
    check "b: ... the new system runs (uname $V1)" "$(has $d/o4 "\($V1\) m68k")"
    check "b: 4. zpkg -Syu after the restart: what waited is installed (sysutils $V1), zpkg and base are $V1" \
        "$([ "$(has $d/o5 '^r5=0')" = ok ] && [ "$(has $d/o5 "^zpkg: $nw packages installed")" = ok ] && [ "$(grep -ac "^Version .*($V1)" $d/o6)" = 3 ] && echo ok || echo bad)"
    check "b: 5. a second update while the first is still on the partition: refused (restart first), nothing written" \
        "$([ "$(has $d/o7 "holds Zenix $V1, which zenixboot has not finished with")" = ok ] && [ "$(has $d/o7 '^r7=1')" = ok ] && echo ok || echo bad)"
    check "b: ... the next start retires the first one (came up, renamed .done)" \
        "$(inorder $L 'sh /tests/s2.sh' "version $V1 came up" 'renamed zenix-update.zup to zenix-update.done')"
    check "b: 6. zpkg -Syu (the new /bin/zpkg) writes Zenix $V2 onto the volume AmigaOS has written to since (nothing waits for it)" \
        "$([ "$(has $d/o8 "Zenix .*\($V2\) is ready on the boot partition")" = ok ] && [ "$(has $d/o8 '^r8=0')" = ok ] && [ "$(has $d/o8 'wait|to finish')" = bad ] && echo ok || echo bad)"
    check "b: 7. reboot: Zenix $V2 installed and running, base $V2 with the new motd" \
        "$([ "$(inorder $L 'sh /tests/s3.sh' "update: installing version $V2" "^zup: installed Zenix .*\($V2\)")" = ok ] &&
            [ "$(has $d/o9 "\($V2\) m68k")" = ok ] && [ "$(has $d/o10 'Zenix updated twice')" = ok ] && [ "$(grep -ac "^Version .*($V2)" $d/o11)" = 2 ] && echo ok || echo bad)"
    check "b: ... zpkg -Syu then has nothing to do" "$([ "$(has $d/o12 'nothing to do')" = ok ] && [ "$(has $d/o12 '^r12=0')" = ok ] && echo ok || echo bad)"
    ffs_get_b() { rm -rf $d/bootx; mkdir -p $d/bootx; python3 tools/ffsread.py $d/disk.hdf --part ZB0 extract $d/bootx; }
    ffs_get_b
    local z=$d/bootx/Zenix
    check "b: the boot partition at the end: zenix is V2's kernel, zenix.old V1's, V1's bundle .done, V2's still there (its trial)" \
        "$(cmp -s $z/zenix-update.zup $SYS2 && cmp -s $z/zenix-update.done $SYS1 && cmp -s $z/zenix-update.zup.sig $SYS2.sig &&
           python3 -I -c '
import sys; sys.path.insert(0, "tools"); import mkzup
k, o = open(sys.argv[1], "rb").read(), open(sys.argv[2], "rb").read()
sys.exit(0 if mkzup.kernel_version(k)[0] == int(sys.argv[3]) and mkzup.kernel_version(o)[0] == int(sys.argv[4]) else 1)' $z/zenix $z/zenix.old $V2 $V1 &&
           grep -q '^phase trial$' $z/state && echo ok || echo bad)"
    boot_check b $d/disk.hdf $d/zb0.img "after both updates" $d/ffs.img
    e2fsck -fn $d/root-after.img >$d/e2fsck.log 2>&1 && R "PASS b: the root: e2fsck -fn clean" || R "FAIL b: the root: e2fsck (log $d/e2fsck.log)"
    check "b: no process faulted" "$(grep -aq '\[kernel\]' $d/dmesg && echo bad || echo ok)"
}

part_m() {
    local n=m d=$OUT/m; rm -rf $d; mkdir -p $d
    conf_files 8099
    root_img $d/root.img $BD/zenix.hdf || { R "FAIL m: the root image"; return; }
    printf 'SYS:zenixboot\n' >$d/ss
    python3 tools/mkffs.py $d/ffs.img 16632K ZenixBoot $d/ss=S/Startup-Sequence $BD/zenixboot=zenixboot \
        $BD/kernel.bin=Zenix/zenix >/dev/null &&
        python3 tools/mkcard.py sd $d/sd.img $d/ffs.img $d/root.img >/dev/null || { R "FAIL m: the card image"; return; }
    fsconf $d/m.fs-uae $d/sd.img $BD/zenix.adf
    printf 'wait zenix:/# 900\nsleep 2\nsend sh /tests/sm.sh\\r\nwait (?m)^m-done 3000\nstop\n' >$d/keys.txt
    inner m $d/m.fs-uae 4000 $d/keys.txt 8099
    unshare --map-root-user --map-auto -nm $OUT/inner-m.sh >$d/run.log 2>&1 || R "FAIL m: the run ($(grep -a 'FAIL\|timed out' $d/run.log | head -2 | tr '\n' ' '))"
    # the card's p2 (the RDB) and p3 (the root) out of the MBR image
    local p2 p3
    p2=$(sfdisk -d $d/sd.img 2>/dev/null | grep 'type=76' | sed 's/.*start= *\([0-9]*\), size= *\([0-9]*\).*/\1 \2/')
    p3=$(sfdisk -d $d/sd.img 2>/dev/null | grep 'type=83' | sed 's/.*start= *\([0-9]*\), size= *\([0-9]*\).*/\1 \2/')
    dd if=$d/sd.img of=$d/p2.img bs=512 skip=${p2% *} count=${p2#* } status=none
    dd if=$d/sd.img of=$d/root-after.img bs=512 skip=${p3% *} count=${p3#* } status=none
    get $d/root-after.img /tests/o21 >$d/o21
    check "m: the card's layout (the RDB inside the MBR's partition 0x76): zpkg -Syu writes Zenix $V1 there" \
        "$([ "$(has $d/o21 "Zenix .*\($V1\) is ready on the boot partition")" = ok ] && [ "$(has $d/o21 '^r21=0')" = ok ] && echo ok || echo bad)"
    rm -rf $d/bootx; mkdir -p $d/bootx; python3 tools/ffsread.py $d/p2.img --part ZB0 extract $d/bootx
    check "m: ... the bundle and its .sig on the PC as the repository's" \
        "$(cmp -s $d/bootx/Zenix/zenix-update.zup $SYS1 && cmp -s $d/bootx/Zenix/zenix-update.zup.sig $SYS1.sig && echo ok || echo bad)"
    boot_check m $d/p2.img $d/zb0.img "the card layout" $d/ffs.img
    e2fsck -fn $d/root-after.img >$d/e2fsck.log 2>&1 && R "PASS m: the root: e2fsck -fn clean" || R "FAIL m: the root: e2fsck (log $d/e2fsck.log)"
    # a disk without a boot partition (the root alone, started from the floppy): said, nothing written
    root_img $d/root2.img $BD/zenix.hdf || { R "FAIL m: the second root image"; return; }
    fsconf $d/m2.fs-uae $d/root2.img $BD/zenix.adf
    inner m2 $d/m2.fs-uae 3000 $d/keys.txt 8099
    unshare --map-root-user --map-auto -nm $OUT/inner-m2.sh >$d/run2.log 2>&1 || R "FAIL m: the second run ($(grep -a 'FAIL\|timed out' $d/run2.log | head -2 | tr '\n' ' '))"
    get $d/root2.img /tests/o21 >$d/o21b
    check "m: no boot partition (a floppy start): zpkg says it puts no system update there, exit 1" \
        "$([ "$(has $d/o21b 'no AmigaDOS boot partition with a Zenix folder')" = ok ] && [ "$(has $d/o21b '^r21=1')" = ok ] && echo ok || echo bad)"
}

part_z() {
    local n=z d=$OUT/z; rm -rf $d; mkdir -p $d/boot/S $d/boot/Zenix
    local VZ=$V0 BZ=$OUT/bz P Z=zenixfs/build/zenixfs-tool
    conf_files 8097
    make -s -j8 BUILD=$BZ ZVNUM=$VZ all $BZ/kernel-zfs.bin $BZ/zenixboot $BZ/pkg/list >>"$OUT/make.log" 2>&1 || { R "FAIL z: build"; return; }
    P=$(make -s BUILD=$BZ -f Makefile --eval 'pa:;@echo $(UAPPS) $(SBINAPPS) $(NETDRVS:%=drv-%)' pa 2>/dev/null)
    PKGDB=1 RAWIMG=1 tools/mkroot-zenixfs.sh $d/root.img $BZ "$P" 48 >/dev/null || { R "FAIL z: the root"; return; }
    $Z mkdir $d/root.img /etc/network
    printf 'auto eth0\niface eth0 inet dhcp\n    driver a2065\n' >$OUT/interfaces
    $Z put $d/root.img $OUT/interfaces /etc/network/interfaces
    { cat $OUT/net.sh; echo 'cp /tests/conf.r /etc/zpkg.conf; zpkg -Qi base > /tests/o30 2>&1'; step 31 'zpkg -Syu'; echo 'echo z1-d""one'; } >$OUT/sz1.sh
    { cat $OUT/net.sh; step 32 'uname -a'; step 33 'zpkg -Syu'; step 34 'zpkg -Qi base sysutils'; step 35 mount; echo 'sync; echo z2-d""one'; } >$OUT/sz2.sh
    $Z put $d/root.img $OUT/sz1.sh /tests/sz1.sh; $Z put $d/root.img $OUT/sz2.sh /tests/sz2.sh; $Z put $d/root.img $OUT/conf.r /tests/conf.r
    printf 'SYS:zenixboot\n' >$d/boot/S/Startup-Sequence
    python3 tools/mkffs.py $d/ffs.img 16632K ZenixBoot $d/boot/S/Startup-Sequence=S/Startup-Sequence \
        $BZ/zenixboot=zenixboot $BZ/kernel-zfs.bin=Zenix/zenix >/dev/null &&
        python3 tools/mkcard.py ide $d/disk.hdf $d/ffs.img $d/root.img >/dev/null || { R "FAIL z: the disk"; return; }
    FAST=8192 fsconf $d/z.fs-uae $d/disk.hdf
    cat >$d/keys.txt <<EOF
wait zenix:/# 900
sleep 2
send sh /tests/sz1.sh\r
wait (?m)^z1-done 3000
send reboot\r
wait (?m)^klog: boot [0-9]+, 300
wait zenix:/# 1500
sleep 2
send sh /tests/sz2.sh\r
wait (?m)^z2-done 3000
stop
EOF
    inner z $d/z.fs-uae 6000 $d/keys.txt 8097
    unshare --map-root-user --map-auto -nm $OUT/inner-z.sh >$d/run.log 2>&1 || R "FAIL z: the run ($(grep -a 'FAIL\|timed out' $d/run.log | head -2 | tr '\n' ' '))"
    tr -d '\r' <$d/run.log | sed 's/\x1b\[[0-9;:]*[mK]//g' >$d/log.txt
    python3 tools/ffsread.py $d/disk.hdf --part ZR0 dump $d/root-after.img
    for i in 30 31 32 33 34 35; do $Z cat $d/root-after.img /tests/o$i >$d/o$i 2>/dev/null; done
    check "z: zpkg -Syu on a ZenixFS root: the system with the ZenixFS kernel is written to the boot partition" \
        "$([ "$(has $d/o31 "Zenix .*\($V1\) is ready on the boot partition")" = ok ] && [ "$(has $d/o31 '^r31=0')" = ok ] && echo ok || echo bad)"
    rm -rf $d/bootx; mkdir -p $d/bootx; python3 tools/ffsread.py $d/disk.hdf --part ZB0 extract $d/bootx
    check "z: reboot: zenixboot takes it, the kernel has the ZenixFS server, zup installs the root half" \
        "$(inorder $d/log.txt 'sh /tests/sz1.sh' 'update: signed by test key' "update: installing version $V1" "^zup: installed Zenix .*\($V1\)")"
    check "z: ... the kernel on the boot partition is the repository's systemz" \
        "$(cmp -s $d/bootx/Zenix/zenix-update.zup $W/r/pool/zenixz-$V1.zup && echo ok || echo bad)"
    check "z: the new system runs on ZenixFS" "$([ "$(has $d/o32 "\($V1\) m68k")" = ok ] && [ "$(has $d/log.txt '^ide0p[0-9]+ +/ +zenixfs ')" = ok ] && echo ok || echo bad)"
    check "z: zpkg -Syu after the restart: what waited is installed live on ZenixFS" "$([ "$(has $d/o33 '^r33=0')" = ok ] && [ "$(grep -ac "^Version .*($V1)" $d/o34)" = 2 ] && echo ok || echo bad)"
    $Z fsck $d/root-after.img >$d/fsck.log 2>&1 && R "PASS z: the root: the PC's fsck clean" || R "FAIL z: the root: fsck (log $d/fsck.log)"
}

[ $# = 0 ] && set -- b m z
for p in "$@"; do
    case $p in b|m|z) "part_$p" ;; *) echo "sysupdate-test: unknown part $p"; exit 2 ;; esac
done
exit $((1 - pass))
