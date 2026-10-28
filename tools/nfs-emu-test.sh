#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# nfscd in FS-UAE (DESIGN D23 11.2, step 2: reading). The stock A1200 (2 MB chip RAM) with FS-UAE's A2065 and slirp, as
# tools/eth-test.sh; the PC's NFS server is tools/nfs-host.py (nfs-sim's portmapper on 111, NFSv3 on 2049, the test
# tree in /export), on 127.0.0.1 of a user and network namespace of the test's own, which the Amiga reaches as
# 10.0.2.2: nothing leaves the PC. The Amiga's script steers the server by looking up .ctl-* names (nfs-host.py).
#   m  one boot for parts e, h, s and k of the note:
#      e  mount -t nfs, the list and df, ls -l, cat, a 100 KB file checksummed (and copied to the root, the PC's sum
#         compared), a symbolic link, a directory and cd into it, a UTF-8 name, a program run from /nas twice with a
#         change on the PC between (the new one runs: D13's key), writes refused (step 2 is read-only), the refused
#         mount points (/etc/x, /sbin), the memory (free, ps), umount
#      h  hard (timeo=3): the NAS silent while cat reads: the log's "not answering", Ctrl-C's SIGINT ends cat (130),
#         kill -9 ends a program that catches SIGINT, mount and df answer, an O_TRUNC open and an unlink killed
#         mid-way end (130); the NAS back: "answers again" and the next cat works
#      s  soft (soft,timeo=2,retrans=2): cat fails with an I/O error while the NAS is silent
#      k  nfscd killed while idle (the next use starts it, the saved root checked), with an open file (that file
#         -ESTALE, the log's "started again", a new open works), a user's raw message to fs.nfs0 (-EACCES), and
#         another export at the same address after a kill (refused, logged)
#   b  a boot with an /etc/fstab line naming the NAS by a host name (/etc/hosts) while no server runs: the login
#      prompt comes with no wait (mount -a is lazy), the mount is listed, the first ls /nas waits until the PC's
#      server starts (60 s after the boot) and then answers
#   q  a quick look by hand (not in the default run): mount, read, umount, ps, free and the log, printed
#   tools/nfs-emu-test.sh [PART...]     (default: m b)
# Needs unprivileged user namespaces (unshare -rn), as eth-test.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-nfs}; mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
BD=$(cd "$BD" && pwd)
unshare -rn true 2>/dev/null || { echo "FAIL unprivileged user namespaces are off (unshare -rn)"; exit 1; }
pass=1
IMG=$OUT/root.hdf
get() { debugfs -R "cat $1" "$IMG" 2>/dev/null | tr -d '\r'; }
put() {     # put FILE PATH MODE: a file on the image, root's
    debugfs -w -R "rm $2" "$IMG" >/dev/null 2>&1
    debugfs -w -R "write $1 $2" "$IMG" >/dev/null 2>&1
    debugfs -w -R "sif $2 uid 0" "$IMG" >/dev/null 2>&1
    debugfs -w -R "sif $2 gid 0" "$IMG" >/dev/null 2>&1
    debugfs -w -R "sif $2 mode 0100$3" "$IMG" >/dev/null 2>&1
}
check() { if [ "$2" = 1 ]; then echo "PASS $1"; else echo "FAIL $1: $3"; pass=0; fi; }
has()   { grep -aqE -- "$2" "$OUT/$1"; }     # has FILE REGEX

# run CONFIG SECONDS SCRIPT DELAY: FS-UAE in a namespace of its own, the NFS server on its loopback DELAY s later
run() {
    rm -f "$OUT/ready" "$OUT/ctl.log"
    ZENIX_BUILD=$BD unshare -rnm bash -c 'ip link set lo up || exit 9
        ( sleep "$5"; exec python3 -I tools/nfs-host.py "$1" "$6/hello.pzx" "$6/echo.zx" ) > "$1/host.log" 2>&1 &
        srv=$!
        if [ "$5" = 0 ]; then for i in $(seq 100); do [ -e "$1/ready" ] && break; sleep 0.1; done; fi
        tools/run-test.sh "$2" "$3" "$4"; r=$?
        kill $srv 2>/dev/null; pkill -P $srv 2>/dev/null; wait $srv 2>/dev/null
        exit $r' "$OUT" "$OUT" "$1" "$2" "$3" "$4" "$BD"
}

image() {   # a fresh root image with the A2065 configured by DHCP, and the test script
    cp "$BD/zenix.hdf" "$IMG"
    debugfs -w -R "mkdir /etc/network" "$IMG" >/dev/null 2>&1
    debugfs -w -R "sif /etc/network uid 0" "$IMG" >/dev/null 2>&1
    printf 'auto eth0\niface eth0 inet dhcp\n    driver a2065\n' > "$OUT/interfaces"
    put "$OUT/interfaces" /etc/network/interfaces 644
    sed '/^\[fs-uae\]/a network_card = a2065' fsuae/a1200-020.fs-uae | sed "s#^hard_drive_0 = .*#hard_drive_0 = $IMG#" > "$OUT/a2065.fs-uae"
}

LEASE='for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    ifconfig eth0 2>/dev/null | grep -q "lease [0-9]" && break
    sleep 1
done'

part_m() {
    image
    cat > "$OUT/m.sh" <<EOS
$LEASE
mkdir -p /nas /soft /etc/x
# ---- e: reading ----
mount -t nfs 10.0.2.2:/export /nas; echo st=\$? > /tmp/e-mount.txt
mount >> /tmp/e-mount.txt; df >> /tmp/e-mount.txt 2>&1
ls -l /nas > /tmp/e-ls.txt 2>&1; echo st=\$? >> /tmp/e-ls.txt
cat /nas/hello.txt > /tmp/e-cat.txt 2>&1
md5sum /nas/big.bin > /tmp/e-md5.txt 2>&1
cp /nas/big.bin /tmp/big.bin; md5sum /tmp/big.bin >> /tmp/e-md5.txt 2>&1
cat /nas/link > /tmp/e-link.txt 2>&1; readlink /nas/link >> /tmp/e-link.txt 2>&1; cat /nas/dir/up >> /tmp/e-link.txt 2>&1
ls /nas/dir > /tmp/e-dir.txt 2>&1; cd /nas/dir && pwd >> /tmp/e-dir.txt && cat b >> /tmp/e-dir.txt; cd /
cat /nas/råksmörgås.txt > /tmp/e-utf.txt 2>&1; cat /nas/åäö.txt >> /tmp/e-utf.txt 2>&1
/nas/prog 1 > /tmp/e-prog.txt 2>&1; echo st=\$? >> /tmp/e-prog.txt
ls /nas/.ctl-prog2 > /dev/null 2>&1; sleep 2
/nas/prog second >> /tmp/e-prog.txt 2>&1; echo st=\$? >> /tmp/e-prog.txt
echo x > /nas/new 2> /tmp/e-ro.txt; mkdir /nas/d2 2>> /tmp/e-ro.txt
mount -t nfs 10.0.2.2:/export /etc/x > /tmp/e-refused.txt 2>&1; mount -t nfs 10.0.2.2:/export /sbin >> /tmp/e-refused.txt 2>&1
mount -t nfs nas /soft >> /tmp/e-refused.txt 2>&1; mount -t nfs -o bogus 10.0.2.2:/export /soft >> /tmp/e-refused.txt 2>&1
free > /tmp/e-free.txt; ps > /tmp/e-ps.txt
umount /nas; echo st=\$? > /tmp/e-umount.txt; mount >> /tmp/e-umount.txt
# ---- h: hard, the NAS silent ----
mount -t nfs -o timeo=3 10.0.2.2:/export /nas; cat /nas/hello.txt > /tmp/h-before.txt 2>&1
ls /nas/.ctl-blackhole-40 > /dev/null 2>&1; sleep 2
cat /nas/big.bin > /dev/null & p=\$!; sleep 6; kill -INT \$p; wait \$p; echo cat=\$? > /tmp/h-kill.txt
svctest catch /nas/dir/a > /tmp/h-catch.txt 2>&1 & p=\$!; sleep 2; kill -INT \$p; sleep 2; echo after-int >> /tmp/h-kill.txt; kill -9 \$p; wait \$p; echo catch=\$? >> /tmp/h-kill.txt
svctest trunc /nas/dir/c & p=\$!; sleep 2; kill -INT \$p; wait \$p; echo trunc=\$? >> /tmp/h-kill.txt
svctest unlink /nas/dir/b & p=\$!; sleep 2; kill -INT \$p; wait \$p; echo unlink=\$? >> /tmp/h-kill.txt
mount > /tmp/h-mount.txt 2>&1; echo mount=\$? >> /tmp/h-kill.txt; df > /tmp/h-df.txt 2>&1; echo df=\$? >> /tmp/h-kill.txt
sync; echo sync=\$? >> /tmp/h-kill.txt
echo done-in-blackhole >> /tmp/h-kill.txt
sleep 40
cat /nas/hello.txt > /tmp/h-after.txt 2>&1; echo st=\$? >> /tmp/h-after.txt
umount /nas
# ---- s: soft ----
mount -t nfs -o soft,timeo=2,retrans=2 10.0.2.2:/export /soft; cat /soft/hello.txt > /tmp/s-before.txt 2>&1
ls /soft/.ctl-blackhole-30 > /dev/null 2>&1; sleep 2
cat /soft/dir/c > /tmp/s-cat.txt 2>&1; echo st=\$? >> /tmp/s-cat.txt
sleep 30; umount /soft
# ---- k: nfscd killed ----
mount -t nfs 10.0.2.2:/export /nas; cat /nas/hello.txt > /dev/null
kill -9 \$(pgrep nfscd); sleep 1; cat /nas/hello.txt > /tmp/k-idle.txt 2>&1; echo st=\$? >> /tmp/k-idle.txt
svctest keep /nas/hello.txt 4 > /tmp/k-open.txt 2>&1 & p=\$!; sleep 2; kill -9 \$(pgrep nfscd); wait \$p
cat /nas/dir/a >> /tmp/k-open.txt 2>&1
echo "svctest call fs.nfs0 4" | su user > /tmp/k-raw.txt 2>&1
ls /nas/.ctl-rootchange > /dev/null 2>&1; sleep 2; kill -9 \$(pgrep nfscd); sleep 1
ls /nas > /tmp/k-root.txt 2>&1; echo ls=\$? >> /tmp/k-root.txt
umount /nas; echo umount=\$? >> /tmp/k-root.txt
dmesg > /tmp/m-k.txt
sync
EOS
    put "$OUT/m.sh" /tests/nfs.sh 755
    cat > "$OUT/m.txt" <<'EOS'
wait zenix:/# 90
sleep 1
send sh /tests/nfs.sh > /tmp/nfs.out 2>&1; echo nfs-do""ne\r
wait nfs-done 600
stop
EOS
    run "$OUT/a2065.fs-uae" 750 "$OUT/m.txt" 0 > "$OUT/m.log" 2>&1
    local rc=$? f k=$OUT/m-k.txt ok
    [ $rc = 0 ] || { echo "FAIL m: the run (exit $rc)"; tail -3 "$OUT/m.log"; pass=0; }
    for f in e-mount e-ls e-cat e-md5 e-link e-dir e-utf e-prog e-ro e-refused e-free e-ps e-umount h-before h-kill \
             h-mount h-df h-after h-catch s-before s-cat k-idle k-open k-raw k-root m-k nfs.out; do get "/tmp/$f.txt" > "$OUT/$f.txt"; done
    get /tmp/nfs.out > "$OUT/nfs.out"
    local sum; sum=$(md5sum < "$OUT/big.bin" | cut -d' ' -f1)
    # e
    ok=0; has e-mount.txt '^st=0$' && has e-mount.txt '^nfs0 +/nas +nfs ' && ok=1
    check "e: mount -t nfs 10.0.2.2:/export /nas; listed as nfs0, type nfs" $ok "$(cat "$OUT/e-mount.txt"; grep -a 'nfs0\|svc: fs.nfs' "$k")"
    ok=0; has e-ls.txt ' hello\.txt$' && has e-ls.txt ' big\.bin$' && has e-ls.txt '^l.* link -> hello\.txt$' && has e-ls.txt '^d.* dir$' && has e-ls.txt '^st=0$' && ok=1
    check "e: ls -l /nas (files, a directory, a link with its target)" $ok "$(cat "$OUT/e-ls.txt")"
    ok=0; [ "$(cat "$OUT/e-cat.txt")" = "hello from the NAS" ] && ok=1
    check "e: cat /nas/hello.txt" $ok "$(cat "$OUT/e-cat.txt")"
    ok=0; [ "$(grep -c "^$sum" "$OUT/e-md5.txt")" = 2 ] && ok=1
    check "e: the 100 KB file's md5 on the Amiga, from /nas and after cp to the root, is the PC's ($sum)" $ok "$(cat "$OUT/e-md5.txt")"
    ok=0; [ "$(tr '\n' '|' < "$OUT/e-link.txt")" = "hello from the NAS|hello.txt|hello from the NAS|" ] && ok=1
    check "e: symbolic links (to a file, readlink, ../ from a subdirectory)" $ok "$(cat "$OUT/e-link.txt")"
    ok=0; [ "$(tr '\n' '|' < "$OUT/e-dir.txt")" = "a|b|c|up|/nas/dir|file b|" ] && ok=1
    check "e: a directory listed, cd into it, a relative name" $ok "$(cat "$OUT/e-dir.txt")"
    ok=0; grep -aq 'no such file' "$OUT/e-utf.txt" && grep -aqx 'räksmörgås' "$OUT/e-utf.txt" && ok=1
    check "e: a UTF-8 name (åäö.txt) read, a missing one -ENOENT" $ok "$(cat "$OUT/e-utf.txt")"
    ok=0; grep -aqE '^hello from pid [0-9]+, round 0$' "$OUT/e-prog.txt" && grep -aqx 'st=7' "$OUT/e-prog.txt" &&
        grep -aqx 'second' "$OUT/e-prog.txt" && grep -aqx 'st=0' "$OUT/e-prog.txt" && ok=1
    check "e: a program run from /nas, then changed on the PC: the new one runs (D13's key from a fresh GETATTR)" $ok "$(cat "$OUT/e-prog.txt")"
    ok=0; grep -aq 'mkdir: /nas/d2: read-only file system' "$OUT/e-ro.txt" && grep -aq '/nas/new: read-only file system' "$OUT/nfs.out" && ok=1
    check "e: writes are refused (step 2 serves reading only): read-only file system" $ok "$(cat "$OUT/e-ro.txt")"
    ok=0; [ "$(grep -ac 'an NFS mount may not be on /, /bin, /sbin, /etc or below them' "$OUT/e-refused.txt")" = 2 ] &&
        grep -aq 'nas: not HOST:/EXPORT' "$OUT/e-refused.txt" && grep -aq 'unknown NFS option bogus' "$OUT/e-refused.txt" && ok=1
    check "e: mount refuses /etc/x and /sbin, a source without :/, an unknown option" $ok "$(cat "$OUT/e-refused.txt")"
    ok=0; has e-umount.txt '^st=0$' && ! has e-umount.txt 'nfs0' && ok=1
    check "e: umount /nas" $ok "$(cat "$OUT/e-umount.txt")"
    echo "nfs-emu-test: memory with one NFS mount, the stock A1200 (2 MB chip RAM, FS-UAE): $(grep -a 'chip RAM free' "$OUT/e-free.txt"); $(grep -a ' nfscd$\| netd$\| a2065$' "$OUT/e-ps.txt" | tr -s ' ' | tr '\n' ';')"
    # h
    ok=0; has m-k.txt 'nfs0: the NAS is not answering, still trying' && ok=1
    check "h: the NAS silent: \"not answering, still trying\" in the log" $ok "$(grep -a 'nfs0' "$k" | tail -8)"
    ok=0; has h-kill.txt '^cat=130$' && ok=1
    check "h: SIGINT (Ctrl-C) ends cat waiting on the silent NAS (130)" $ok "$(cat "$OUT/h-kill.txt")"
    ok=0; has h-kill.txt '^after-int$' && has h-kill.txt '^catch=137$' && [ ! -s "$OUT/h-catch.txt" ] && ok=1
    check "h: a program that catches SIGINT keeps waiting; kill -9 ends it (137)" $ok "$(cat "$OUT/h-kill.txt" "$OUT/h-catch.txt")"
    ok=0; has h-kill.txt '^trunc=130$' && has h-kill.txt '^unlink=130$' && ok=1
    check "h: an O_TRUNC open and an unlink killed mid-way end (130)" $ok "$(cat "$OUT/h-kill.txt")"
    ok=0; has h-kill.txt '^mount=0$' && has h-kill.txt '^df=0$' && has h-kill.txt '^sync=0$' && has h-mount.txt '^nfs0 +/nas ' && has h-kill.txt '^done-in-blackhole$' && ok=1
    check "h: mount, df and sync answer while the NAS is silent" $ok "$(cat "$OUT/h-kill.txt" "$OUT/h-mount.txt")"
    ok=0; [ "$(tr '\n' '|' < "$OUT/h-after.txt")" = "hello from the NAS|st=0|" ] && has m-k.txt 'nfs0: the NAS answers again' && ok=1
    check "h: the NAS back: \"answers again\", the next cat works" $ok "$(cat "$OUT/h-after.txt"; grep -a 'nfs0' "$k" | tail -5)"
    # s
    ok=0; [ "$(cat "$OUT/s-before.txt")" = "hello from the NAS" ] && has s-cat.txt 'input/output error' && ! has s-cat.txt '^st=0$' && ok=1
    check "s: soft: cat fails with an I/O error while the NAS is silent" $ok "$(cat "$OUT/s-before.txt" "$OUT/s-cat.txt")"
    # k
    ok=0; [ "$(tr '\n' '|' < "$OUT/k-idle.txt")" = "hello from the NAS|st=0|" ] && ok=1
    check "k: nfscd killed while idle: the next cat starts it again and works" $ok "$(cat "$OUT/k-idle.txt")"
    ok=0; has k-open.txt 'svctest keep /nas/hello.txt: read 19, deleted -30, again -116' && has k-open.txt '^file a$' &&
        has m-k.txt 'nfs0: started again: 1 open files on the mount can no longer be used \(reopen them\)' && ok=1
    check "k: nfscd killed with a file open: that file -ESTALE (-116), logged; a new open works" $ok "$(cat "$OUT/k-open.txt"; grep -a 'nfs0: started' "$k")"
    ok=0; has k-raw.txt 'svctest call fs.nfs0 4: -13 ' && ok=1
    check "k: a user's own message to fs.nfs0: -EACCES" $ok "$(cat "$OUT/k-raw.txt")"
    ok=0; has m-k.txt "nfs0: the NAS's export is not the one that was mounted: mount it again" && has k-root.txt '^ls=[1-9]' && has k-root.txt '^umount=0$' && ok=1
    check "k: another export at the same address after a restart: refused, logged" $ok "$(cat "$OUT/k-root.txt"; grep -a 'nfs0' "$k" | tail -4)"
    ok=0; ! has m-k.txt 'nfs0: connect to port .*: address already in use' && ok=1
    check "m: a source port still closing is never a failed connection (the next port is taken)" $ok "$(grep -a 'connect to port' "$k")"
    ok=0; [ "$(grep -acE '^\[kernel\]|panic' "$k")" = 0 ] && ok=1
    check "m: no fault in the kernel log" $ok "$(grep -aE '^\[kernel\]|panic' "$k")"
    e2fsck -fn "$IMG" > "$OUT/e2fsck-m.log" 2>&1 && echo "PASS m: the host's e2fsck finds the root clean" || { echo "FAIL m: e2fsck"; pass=0; }
}

part_b() {
    image
    printf '127.0.0.1\tlocalhost\n10.0.2.2\tnas\n' > "$OUT/hosts"
    put "$OUT/hosts" /etc/hosts 644
    printf 'nas:/export /nas nfs defaults\n' > "$OUT/fstab"
    put "$OUT/fstab" /etc/fstab 644
    debugfs -w -R "mkdir /nas" "$IMG" >/dev/null 2>&1
    cat > "$OUT/b.sh" <<EOS
mount > /tmp/b-mount.txt 2>&1
$LEASE
ls /nas > /tmp/b-ls.txt 2>&1; echo st=\$? >> /tmp/b-ls.txt
dmesg > /tmp/b-k.txt
sync
EOS
    put "$OUT/b.sh" /tests/nfsb.sh 755
    cat > "$OUT/b.txt" <<'EOS'
wait zenix:/# 50
send sh /tests/nfsb.sh > /tmp/b.out 2>&1; echo nfsb-do""ne\r
wait nfsb-done 200
stop
EOS
    run "$OUT/a2065.fs-uae" 300 "$OUT/b.txt" 60 > "$OUT/b.log" 2>&1
    local rc=$? f ok
    [ $rc = 0 ] || { echo "FAIL b: the run (exit $rc)"; tail -3 "$OUT/b.log"; pass=0; }
    for f in b-mount b-ls b-k; do get "/tmp/$f.txt" > "$OUT/$f.txt"; done
    ok=0; [ $rc = 0 ] && has b-mount.txt '^nfs0 +/nas +nfs ' && ok=1
    check "b: an fstab line naming the NAS with no server: the login prompt within 50 s, the mount listed (lazy)" $ok "$(cat "$OUT/b-mount.txt"; tail -3 "$OUT/b.log")"
    ok=0; [ "$(tr '\n' '|' < "$OUT/b-ls.txt")" = "big.bin|dir|hello.txt|link|prog|åäö.txt|st=0|" ] && ok=1
    check "b: the first ls /nas waits for the server (started 60 s after the boot), then answers" $ok "$(cat "$OUT/b-ls.txt"; grep -a 'nfs0' "$OUT/b-k.txt")"
    e2fsck -fn "$IMG" > "$OUT/e2fsck-b.log" 2>&1 && echo "PASS b: the host's e2fsck finds the root clean" || { echo "FAIL b: e2fsck"; pass=0; }
}

part_q() {  # a quick look (not in the default run): mount, read, umount, the log
    image
    cat > "$OUT/q.sh" <<EOS
$LEASE
mkdir -p /nas
mount -t nfs 10.0.2.2:/export /nas; echo st=\$?; ps; free; cat /nas/hello.txt; md5sum /nas/big.bin; ps; free; umount /nas; echo st=\$?
dmesg | grep nfs
EOS
    put "$OUT/q.sh" /tests/nfsq.sh 755
    printf 'wait zenix:/# 90\nsend sh /tests/nfsq.sh > /tmp/q.out 2>&1; sync; echo q-do""ne\\r\nwait q-done 200\nstop\n' > "$OUT/q.txt"
    run "$OUT/a2065.fs-uae" 300 "$OUT/q.txt" 0 > "$OUT/q.log" 2>&1
    get /tmp/q.out
}

[ $# = 0 ] && set -- m b
for p in "$@"; do
    case $p in m|b|q) "part_$p" ;; *) echo "nfs-emu-test: unknown part $p"; exit 2 ;; esac
done
rm -f "$IMG"
exit $((1 - pass))
