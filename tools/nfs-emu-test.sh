#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# nfscd in FS-UAE (DESIGN D23 11.2, steps 2-4: reading, writing, users). The stock A1200 (2 MB chip RAM) with FS-UAE's A2065 and slirp, as
# tools/eth-test.sh; the PC's NFS server is tools/nfs-host.py (nfs-sim's portmapper on 111, NFSv3 on 2049, the test
# tree in /export), on 127.0.0.1 of a user and network namespace of the test's own, which the Amiga reaches as
# 10.0.2.2: nothing leaves the PC. The Amiga's script steers the server by looking up .ctl-* names (nfs-host.py).
#   m  one boot for parts e, h, s and k of the note:
#      e  mount -t nfs, the list and df, ls -l, cat, a 100 KB file checksummed (and copied to the root, the PC's sum
#         compared), a symbolic link, a directory and cd into it, a UTF-8 name, a program run from /nas twice with a
#         change on the PC between (the new one runs: D13's key), the refused mount points (/etc/x, /sbin), the
#         memory (free, ps), umount
#      h  hard (timeo=3): the NAS silent while cat reads: the log's "not answering", Ctrl-C's SIGINT ends cat (130),
#         kill -9 ends a program that catches SIGINT, mount and df answer, an O_TRUNC open and an unlink killed
#         mid-way end (130); the NAS back: "answers again" and the next cat works
#      s  soft (soft,timeo=2,retrans=2): cat fails with an I/O error while the NAS is silent
#      k  nfscd killed while idle (the next use starts it, the saved root checked), with an open file (that file
#         -ESTALE, the log's "started again", a new open works), a user's raw message to fs.nfs0 (-EACCES), and
#         another export at the same address after a kill (refused, logged)
#   w  writing (step 3), one boot: e's writes (a 108 KB file copied in and out, checksums on the Amiga and on the PC;
#      mkdir, append, mv, ln -s, ln, touch, chmod, rm, rmdir; chmod 4755 refused; an unlinked open file still read
#      and gone from the NAS after its close; df; -o ro refuses writes) and h with writes (the NAS restarts while a
#      file is open with unsaved data: "sent again" in the log and the file whole; a copy while the NAS is silent and
#      then restarts: whole after sync); the PC's tree compared; the memory after writing
#   r  reboot (step 3): an NFS mount whose NAS went silent with unsaved data, mounted before an ext2 floppy: reboot;
#      the floppy and the root are clean (the network mount synced last, bounded) and the floppy's file is there
#   u  users and nosuid (step 4): files made by two Zenix users carry their numbers on the NAS; chown by a user
#      refused; a set-user-id root program on the share runs as the caller (the same program on the root runs as
#      root); with the NAS's squash every user's files are the NAS user's
#   b  a boot with an /etc/fstab line naming the NAS by a host name (/etc/hosts) while no server runs: the login
#      prompt comes with no wait (mount -a is lazy), the mount is listed, the first ls /nas waits until the PC's
#      server starts (60 s after the boot) and then answers
#   q  a quick look by hand (not in the default run): mount, read, umount, ps, free and the log, printed
#   tools/nfs-emu-test.sh [PART...]     (default: m b w r u)
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
        ( sleep "$5"; exec python3 -I tools/nfs-host.py "$1" "$6/hello.pzx" "$6/echo.zx" "$6/id.zx" ) > "$1/host.log" 2>&1 &
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
    for f in e-mount e-ls e-cat e-md5 e-link e-dir e-utf e-prog e-refused e-free e-ps e-umount h-before h-kill \
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
    ok=0; has k-open.txt 'svctest keep /nas/hello.txt: read 19, deleted 0, again -116' && has k-open.txt '^file a$' &&
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
    ok=0; [ "$(tr '\n' '|' < "$OUT/b-ls.txt")" = "big.bin|dir|hello.txt|link|prog|suidid|åäö.txt|st=0|" ] && ok=1
    check "b: the first ls /nas waits for the server (started 60 s after the boot), then answers" $ok "$(cat "$OUT/b-ls.txt"; grep -a 'nfs0' "$OUT/b-k.txt")"
    e2fsck -fn "$IMG" > "$OUT/e2fsck-b.log" 2>&1 && echo "PASS b: the host's e2fsck finds the root clean" || { echo "FAIL b: e2fsck"; pass=0; }
}

part_w() {   # writing (step 3): e's writes and h with writes
    image
    cat > "$OUT/w.sh" <<EOS
$LEASE
mkdir -p /nas /ro
mount -t nfs 10.0.2.2:/export /nas; echo st=\$? > /tmp/w-mount.txt
ls /nas > /dev/null; free > /tmp/w-free0.txt; ps > /tmp/w-ps0.txt
# ---- e: writing ----
seq 1 20000 > /tmp/seq.txt
cp /tmp/seq.txt /nas/seq.txt; echo cp=\$? > /tmp/w-cp.txt
md5sum /tmp/seq.txt /nas/seq.txt >> /tmp/w-cp.txt 2>&1
cp /nas/seq.txt /tmp/seq2.txt; md5sum /tmp/seq2.txt >> /tmp/w-cp.txt 2>&1
mkdir /nas/wd; echo first > /nas/wd/a; echo second >> /nas/wd/a; mv /nas/wd/a /nas/wd/b
ln -s ../hello.txt /nas/wd/l; ln /nas/wd/b /nas/wd/hard; touch /nas/wd/t; chmod 640 /nas/wd/b; rm /nas/wd/t
mkdir /nas/wd/gone; rmdir /nas/wd/gone
cat /nas/wd/l > /tmp/w-ops.txt 2>&1; cat /nas/wd/b >> /tmp/w-ops.txt 2>&1; ls -l /nas/wd >> /tmp/w-ops.txt 2>&1
chmod 4755 /nas/wd/b >> /tmp/w-ops.txt 2>&1; echo chmod=\$? >> /tmp/w-ops.txt
echo tmp-data > /nas/wd/tmp; svctest keep /nas/wd/tmp 2 > /tmp/w-keep.txt 2>&1 & sleep 1; ls -a /nas/wd > /tmp/w-keep1.txt; wait
sleep 1; ls -a /nas/wd > /tmp/w-keep2.txt
df /nas > /tmp/w-df.txt 2>&1
free > /tmp/w-free.txt; ps > /tmp/w-ps.txt
umount /nas; echo umount=\$? >> /tmp/w-mount.txt
mount -t nfs -o ro 10.0.2.2:/export /ro; echo x > /ro/new 2> /tmp/w-ro.txt; mkdir /ro/d2 2>> /tmp/w-ro.txt; cat /ro/hello.txt >> /tmp/w-ro.txt; umount /ro
# ---- h with writes ----
mount -t nfs -o timeo=3 10.0.2.2:/export /nas
sh -c 'echo part1; sleep 8; echo part2' > /nas/hw1.txt &
sleep 2; ls /nas/.ctl-restart > /dev/null 2>&1; wait
cat /nas/hw1.txt > /tmp/w-h1.txt 2>&1
ls /nas/.ctl-blackrestart-12 > /dev/null 2>&1; sleep 2
cp /tmp/seq.txt /nas/hw2.txt; echo cp=\$? > /tmp/w-h2.txt; md5sum /nas/hw2.txt >> /tmp/w-h2.txt 2>&1
sync; echo sync=\$? >> /tmp/w-h2.txt
ls /nas/.ctl-dump-w > /dev/null 2>&1; sleep 2
umount /nas; echo umount=\$? >> /tmp/w-h2.txt
dmesg > /tmp/w-k.txt
sync
EOS
    put "$OUT/w.sh" /tests/nfsw.sh 755
    cat > "$OUT/w.txt" <<'EOS'
wait zenix:/# 90
sleep 1
send sh /tests/nfsw.sh > /tmp/nfsw.out 2>&1; echo nfsw-do""ne\r
wait nfsw-done 400
stop
EOS
    run "$OUT/a2065.fs-uae" 500 "$OUT/w.txt" 0 > "$OUT/w.log" 2>&1
    local rc=$? f ok k=$OUT/w-k.txt t=$OUT/tree-w.txt sum
    [ $rc = 0 ] || { echo "FAIL w: the run (exit $rc)"; tail -3 "$OUT/w.log"; pass=0; }
    for f in w-mount w-free0 w-ps0 w-cp w-ops w-keep w-keep1 w-keep2 w-df w-free w-ps w-ro w-h1 w-h2 w-k nfsw.out; do get "/tmp/$f.txt" > "$OUT/$f.txt"; done
    get /tmp/nfsw.out > "$OUT/nfsw.out"
    sum=$(seq 1 20000 | md5sum | cut -d' ' -f1)
    tl() { grep -a "^$1 " "$t"; }               # the PC's line for a path
    ok=0; has w-mount.txt '^st=0$' && has w-mount.txt '^umount=0$' && ok=1
    check "w: mount and umount" $ok "$(cat "$OUT/w-mount.txt")"
    ok=0; has w-cp.txt '^cp=0$' && [ "$(grep -c "^$sum" "$OUT/w-cp.txt")" = 3 ] && [ "$(tl /seq.txt | cut -d' ' -f7,8)" = "$sum $sum" ] && ok=1
    check "e: a 108 KB file copied to /nas and back: the same md5 on the Amiga and on the PC, committed there ($sum)" $ok "$(cat "$OUT/w-cp.txt"; tl /seq.txt)"
    ok=0; [ "$(head -3 "$OUT/w-ops.txt" | tr '\n' '|')" = "hello from the NAS|first|second|" ] && has w-ops.txt '^-rw-r----- +2 .* b$' &&
        has w-ops.txt ' hard$' && has w-ops.txt '^l.* l -> \.\./hello\.txt$' && ! has w-ops.txt ' t$' && ! has w-ops.txt ' gone$' &&
        [ "$(tl /wd/b | cut -d' ' -f3)" = 640 ] && tl /wd/l | grep -q -- '-> \.\./hello\.txt$' && [ -z "$(tl /wd/a)$(tl /wd/t)$(tl /wd/gone)" ] && ok=1
    check "e: mkdir, append, mv, ln -s, ln, touch, chmod, rm, rmdir (on the Amiga and in the PC's tree)" $ok "$(cat "$OUT/w-ops.txt"; grep -a '^/wd' "$t")"
    ok=0; has w-ops.txt '/nas/wd/b: (cannot change|operation not permitted)' && has w-ops.txt '^chmod=1$' && [ "$(tl /wd/b | cut -d' ' -f3)" = 640 ] && ok=1
    check "e: chmod 4755 on the NAS refused (nosuid)" $ok "$(tail -2 "$OUT/w-ops.txt")"
    ok=0; has w-keep.txt 'svctest keep /nas/wd/tmp: read 9, deleted 0, again 9: the same bytes' && has w-keep1.txt '^\.nfs[0-9a-f]{12}$' &&
        ! has w-keep1.txt '^tmp$' && ! has w-keep2.txt '^\.nfs' && ! has w-keep2.txt '^tmp$' && ! grep -aq '^/wd/\.nfs\|^/wd/tmp ' "$t" && ok=1
    check "e: an unlinked open file reads on (a .nfs name meanwhile), gone from the NAS after its close" $ok "$(cat "$OUT/w-keep.txt"; echo --; cat "$OUT/w-keep1.txt"; echo --; cat "$OUT/w-keep2.txt")"
    ok=0; has w-df.txt '^nfs0 ' && ok=1
    check "e: df /nas" $ok "$(cat "$OUT/w-df.txt")"
    ok=0; has w-ro.txt 'mkdir: /ro/d2: read-only file system' && has nfsw.out '/ro/new: read-only file system' && has w-ro.txt '^hello from the NAS$' &&
        [ -z "$(tl /new)$(tl /d2)" ] && ok=1
    check "e: mount -o ro reads and refuses writes (read-only file system)" $ok "$(cat "$OUT/w-ro.txt"; grep -a '/ro/' "$OUT/nfsw.out")"
    echo "nfs-emu-test: memory mounted and idle, the stock A1200 (2 MB chip RAM, FS-UAE): $(grep -a 'chip RAM free' "$OUT/w-free0.txt"); $(grep -a ' nfscd$\| netd$\| a2065$' "$OUT/w-ps0.txt" | tr -s ' ' | tr '\n' ';')"
    echo "nfs-emu-test: memory after writing, the stock A1200 (2 MB chip RAM, FS-UAE): $(grep -a 'chip RAM free' "$OUT/w-free.txt"); $(grep -a ' nfscd$\| netd$\| a2065$' "$OUT/w-ps.txt" | tr -s ' ' | tr '\n' ';')"
    ok=0; [ "$(tr '\n' '|' < "$OUT/w-h1.txt")" = "part1|part2|" ] && has w-k.txt 'nfs0: the NAS restarted: unsaved data sent again' &&
        [ "$(tl /hw1.txt | cut -d' ' -f6)" = 12 ] && [ "$(tl /hw1.txt | cut -d' ' -f7)" = "$(tl /hw1.txt | cut -d' ' -f8)" ] && ok=1
    check "h: the NAS restarts while a file holds unsaved data: sent again (the verifier changed), the file whole" $ok "$(cat "$OUT/w-h1.txt"; tl /hw1.txt; grep -a 'nfs0' "$k" | tail -6)"
    ok=0; has w-h2.txt '^cp=0$' && has w-h2.txt "^$sum" && has w-h2.txt '^sync=0$' && has w-h2.txt '^umount=0$' &&
        [ "$(tl /hw2.txt | cut -d' ' -f7,8)" = "$sum $sum" ] && ok=1
    check "h: a copy while the NAS is silent, then restarts: whole on the NAS after sync" $ok "$(cat "$OUT/w-h2.txt"; tl /hw2.txt)"
    ok=0; ! has w-k.txt 'connect to port .*: address already in use' && ok=1
    check "w: every reconnect binds its source port again (no \"address already in use\")" $ok "$(grep -a 'connect to port' "$k")"
    ok=0; [ "$(grep -acE '^\[kernel\]|panic' "$k")" = 0 ] && ok=1
    check "w: no fault in the kernel log" $ok "$(grep -aE '^\[kernel\]|panic' "$k")"
    e2fsck -fn "$IMG" > "$OUT/e2fsck-w.log" 2>&1 && echo "PASS w: the host's e2fsck finds the root clean" || { echo "FAIL w: e2fsck"; pass=0; }
}

part_r() {  # reboot with a silent NAS holding unsaved data, an ext2 floppy mounted after it (step 3)
    image
    local df=$OUT/df1.adf
    head -c 901120 /dev/zero > "$df"
    mke2fs -q -t ext2 -E revision=0 -b 1024 -F "$df" >/dev/null 2>&1 || { echo "FAIL r: mke2fs"; pass=0; return; }
    sed "s#^floppy_drive_0 = .*#&\nfloppy_drive_1 = $df\nfloppy_drive_count = 2\nwritable_floppy_images = 1#" "$OUT/a2065.fs-uae" > "$OUT/r.fs-uae"
    cat > "$OUT/r.sh" <<EOS
$LEASE
mkdir -p /nas /f
mount -t nfs -o timeo=3 10.0.2.2:/export /nas; echo st=\$? > /tmp/r-mount.txt
mount -t ext2 df1 /f; echo st=\$? >> /tmp/r-mount.txt; mount >> /tmp/r-mount.txt
sh -c 'echo pending; sleep 200' > /nas/r.txt &
sleep 1; ls /nas/.ctl-blackhole-300 > /dev/null 2>&1; sleep 2
echo local-data > /f/local.txt; echo more > /tmp/r-root.txt
sync
reboot
EOS
    put "$OUT/r.sh" /tests/nfsr.sh 755
    cat > "$OUT/r.txt" <<'EOS'
wait zenix:/# 90
sleep 1
send sh /tests/nfsr.sh > /tmp/nfsr.out 2>&1\r
wait reboot: filesystems synced 120
sleep 2
stop
EOS
    run "$OUT/r.fs-uae" 300 "$OUT/r.txt" 0 > "$OUT/r.log" 2>&1
    local rc=$? ok
    [ $rc = 0 ] || { echo "FAIL r: the run (exit $rc)"; tail -3 "$OUT/r.log"; pass=0; }
    get /tmp/r-mount.txt > "$OUT/r-mount.txt"
    ok=0; [ "$(grep -c '^st=0$' "$OUT/r-mount.txt")" = 2 ] && [ "$(grep -n '^nfs0 \|^df1 ' "$OUT/r-mount.txt" | head -1 | cut -d' ' -f1 | cut -d: -f2)" = nfs0 ] && ok=1
    check "r: the NFS mount, then an ext2 floppy after it in the table" $ok "$(cat "$OUT/r-mount.txt")"
    ok=0; [ $rc = 0 ] && grep -aq 'reboot: filesystems synced' "$OUT/r.log" && grep -aq 'nfs0: sync: the NAS is not answering: 1 KB kept unsaved' "$OUT/r.log" && ok=1
    check "r: reboot with the NAS silent ends, the unsaved NFS data said in the log (the network mount's sync bounded)" $ok "$(grep -a 'reboot\|nfs0' "$OUT/r.log" | tail -5)"
    ok=0; e2fsck -fn "$df" > "$OUT/e2fsck-r-df1.log" 2>&1 && [ "$(debugfs -R 'cat /local.txt' "$df" 2>/dev/null)" = local-data ] && ok=1
    check "r: the ext2 floppy mounted after the NFS mount is clean, its file written" $ok "$(cat "$OUT/e2fsck-r-df1.log")"
    e2fsck -fn "$IMG" > "$OUT/e2fsck-r.log" 2>&1 && [ "$(get /tmp/r-root.txt)" = more ] && echo "PASS r: the host's e2fsck finds the root clean" || { echo "FAIL r: e2fsck"; pass=0; }
    rm -f "$df"
}

part_u() {  # users and nosuid (step 4)
    image
    cat > "$OUT/u.sh" <<EOS
$LEASE
mkdir -p /nas
mount -t nfs 10.0.2.2:/export /nas; echo st=\$? > /tmp/u-mount.txt
mkdir /nas/pub; chmod 777 /nas/pub
echo "echo by-user > /nas/pub/u1.txt; id > /nas/pub/id.txt" | su user
echo "echo by-guest > /nas/pub/u2.txt" | su guest
echo "chown guest /nas/pub/u1.txt" | su user > /tmp/u-chown.txt 2>&1
ls -l /nas/suidid > /tmp/u-suid.txt 2>&1
echo /nas/suidid | su user >> /tmp/u-suid.txt 2>&1
cp /nas/suidid /tmp/suidid; chmod 4755 /tmp/suidid; echo /tmp/suidid | su user > /tmp/u-local.txt 2>&1
ls /nas/.ctl-squash-1026-100 > /dev/null 2>&1; sleep 2
echo "echo squashed > /nas/pub/u3.txt" | su guest
echo squashed-root > /nas/pub/u4.txt
ls /nas/.ctl-nosquash > /dev/null 2>&1; sleep 2
ls -l /nas/pub > /tmp/u-ls.txt 2>&1
ls /nas/.ctl-dump-u > /dev/null 2>&1; sleep 2
umount /nas; echo umount=\$? >> /tmp/u-mount.txt
dmesg > /tmp/u-k.txt
sync
EOS
    put "$OUT/u.sh" /tests/nfsu.sh 755
    cat > "$OUT/u.txt" <<'EOS'
wait zenix:/# 90
sleep 1
send sh /tests/nfsu.sh > /tmp/nfsu.out 2>&1; echo nfsu-do""ne\r
wait nfsu-done 300
stop
EOS
    run "$OUT/a2065.fs-uae" 400 "$OUT/u.txt" 0 > "$OUT/u.log" 2>&1
    local rc=$? f ok k=$OUT/u-k.txt t=$OUT/tree-u.txt
    [ $rc = 0 ] || { echo "FAIL u: the run (exit $rc)"; tail -3 "$OUT/u.log"; pass=0; }
    for f in u-mount u-chown u-suid u-local u-ls u-k; do get "/tmp/$f.txt" > "$OUT/$f.txt"; done
    get /tmp/nfsu.out > "$OUT/nfsu.out"
    tl() { grep -a "^$1 " "$t"; }
    ok=0; has u-mount.txt '^st=0$' && has u-mount.txt '^umount=0$' && ok=1
    check "u: mount and umount" $ok "$(cat "$OUT/u-mount.txt")"
    ok=0; [ "$(tl /pub/u1.txt | cut -d' ' -f4,5)" = "1000 100" ] && [ "$(tl /pub/u2.txt | cut -d' ' -f4,5)" = "1001 100" ] &&
        tl /pub/id.txt >/dev/null && ok=1
    check "u: files made by two users (user, guest) carry their numbers on the NAS (1000, 1001)" $ok "$(grep -a '^/pub' "$t")"
    ok=0; has u-chown.txt 'operation not permitted|permission denied' && [ "$(tl /pub/u1.txt | cut -d' ' -f4)" = 1000 ] && ok=1
    check "u: chown by a user is refused (root only)" $ok "$(cat "$OUT/u-chown.txt")"
    ok=0; has u-suid.txt '^-rwxr-xr-x .* /nas/suidid$' && has u-suid.txt '^uid=1000\(user\) gid=100\([a-z]*\)( groups=[0-9a-z(),]*)?$' && ! has u-suid.txt 'euid' &&
        has u-local.txt 'euid=0\(root\)' && ok=1
    check "u: nosuid: a set-user-id root program on the share shows no s bit and runs as the caller (on the root: as root)" $ok "$(cat "$OUT/u-suid.txt" "$OUT/u-local.txt")"
    ok=0; [ "$(tl /pub/u3.txt | cut -d' ' -f4,5)" = "1026 100" ] && [ "$(tl /pub/u4.txt | cut -d' ' -f4,5)" = "1026 100" ] &&
        has u-ls.txt ' 1026 +users .* u3\.txt$' && ok=1
    check "u: with the NAS's squash (map all users to 1026) files of a user and of root are the NAS user's" $ok "$(cat "$OUT/u-ls.txt"; grep -a '^/pub' "$t")"
    ok=0; [ "$(grep -acE '^\[kernel\]|panic' "$k")" = 0 ] && ok=1
    check "u: no fault in the kernel log" $ok "$(grep -aE '^\[kernel\]|panic' "$k")"
    e2fsck -fn "$IMG" > "$OUT/e2fsck-u.log" 2>&1 && echo "PASS u: the host's e2fsck finds the root clean" || { echo "FAIL u: e2fsck"; pass=0; }
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

[ $# = 0 ] && set -- m b w r u
for p in "$@"; do
    case $p in m|b|w|r|u|q) "part_$p" ;; *) echo "nfs-emu-test: unknown part $p"; exit 2 ;; esac
done
rm -f "$IMG"
exit $((1 - pass))
