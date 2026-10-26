#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The service manager (DESIGN D11, docs/d11-service-manager.md section 9) in the emulator, headless. Boots:
#   a  the normal ext2 root: boot 1, diskd 2, fsd 3, no zfsd; registered names are refused to root too; fsd and diskd
#      killed while idle come back on the next use (the root is still writable: the restart used -root), dmesg says so
#   z  the ZenixFS root (zenix-zroot.hdf): zfsd serves it as pid 3, and comes back after a kill
#   b  two ext2 partitions, the second marked bootable: it is the root (rootpick's rule, fsd no longer picks)
#   n  a disk with no filesystem and no RAM disk: the panic text at once
#   r  the RAM floppy with no hard disk: ram0 is the root
#   d  diskd's saved table (step 2): killed while idle it keeps the claims (a raw open of the mounted root is still
#      refused) and the handles (a floppy probed on demand, df1, keeps its slot); killed twice during a long cp (the
#      second kill ends it at once, even mid-request: fsd sends the failed request again) the copy is right and the
#      host's e2fsck finds the root clean
#   o  the ext2 root's restart (step 3): a deleted file held open reads the same bytes after fsd restarted, and its
#      space comes back at its last close (the open counts came from the kernel, SVC_OPENS); rm after a restart frees
#      space; fsd killed in the middle of a copy (killed again, at most 3 times, until the log says "during
#      request": a kill can land between two requests): the next one marks the volume "needs a check" (the host's
#      dumpe2fs: not clean)
#   zo the same on the ZenixFS root, and a file created less than 3 s before zfsd died (not committed) is gone for its
#      open file: the read fails and never reaches the next new file
#   u  a dd copy of the root partition beside it (the same uuid): the restart serves the original (the place first)
#   l  the limits (step 4) with pingd as a file service: started on demand at its nice, a quit is a stop and the next
#      call starts it again; five crashes in a request (-104 each) with the back-off (the 3rd, 4th, 5th call wait 1, 2,
#      4 s more), then given up: -104 at once; registering again clears it; a start-up calling its own port fails at
#      once; a loader killed in its back-off passes the role on (the next call works); a cycle of two services fails
#      after the 30 s deadline
#   s  the sync paths (step 5): reboot with the root server dead and a loader asleep in a 4 s back-off: the sync skips
#      the dead server at once (no error), the machine resets and boots again
#   h  reboot with a filesystem server that never answers its sync (pingd -fs, audit A7): after 5 s the machine is
#      reset anyway, the log names the server, and it boots again
#   t  /etc/svctab (step 7): a service listed there starts on the first call at its nice; ping's quit is a stop and
#      the next ping starts it again; a missing program fails at once each time, logged, never counted (no give-up);
#      a bad line is reported and skipped; a crash during a request: that client gets -104, a client queued behind it
#      is served by the next server; the root server stopped while a file service must be read from it: the load
#      starts the root server too (two loads nested) and the call works
#   f  a call whose start fails (audit A1, A2): the caller is no longer in a call - killed while it waits after the
#      failure, or in its back-off, it ends at once; a nice 19 loader boosted by a nice -20 client queued in its
#      back-off is back at its own level (PRI 31) after the start failed
#   dmg  a test-only kernel damages diskd's saved table: diskd stops with its message, the kernel panics
# tools/svc-test.sh [PART...] runs only those parts (default: all).
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
PARTS=" ${*:-a z b n r d o zo u l f s h t dmg} "
want_part() { case "$PARTS" in *" $1 "*) return 0;; esac; return 1; }
make -s BUILD=$BD all $BD/zenix-zroot.hdf $BD/zenix-ram.adf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-svc}; mkdir -p "$OUT"
SER=${ZENIX_SERIAL:-/tmp/zenix-svc-serial}
pass=1
clean() { tr -d '\r' | sed 's/\x1b\[[0-9;:]*[mK]//g'; }
run() {    # run NAME CONFIG SECONDS SCRIPT
    ZENIX_SERIAL=$SER tools/run-test.sh "$2" "$3" "$4" 2>&1 | clean > "$OUT/$1.log"
}
line() { if grep -aqxF -- "$3" "$OUT/$1.log"; then echo "PASS $2"; else echo "FAIL $2"; pass=0; fi; }
rx()   { if grep -aqxE -- "$3" "$OUT/$1.log"; then echo "PASS $2"; else echo "FAIL $2"; pass=0; fi; }
none() { if grep -aqxE -- "$3" "$OUT/$1.log"; then echo "FAIL $2"; pass=0; else echo "PASS $2"; fi; }
same() {   # same LOG WHAT KEY1 KEY2: two "KEY=N" lines hold the same number
    local a b; a=$(grep -aoxE "$3=[0-9]+" "$OUT/$1.log" | head -1); b=$(grep -aoxE "$4=[0-9]+" "$OUT/$1.log" | head -1)
    if [ -n "$a" ] && [ "${a#*=}" = "${b#*=}" ]; then echo "PASS $2 (${a#*=})"; else echo "FAIL $2 ($a $b)"; pass=0; fi
}
conf() {   # conf NAME HDF: the fast A1200 with another hard disk
    sed "s#^hard_drive_0 = .*#hard_drive_0 = $2#" fsuae/a1200-020-fast.fs-uae > "$OUT/$1.fs-uae"
}

# ---- a: the normal root ----
if want_part a; then
run a fsuae/a1200-020-fast.fs-uae 120 tools/tests/svc-test.txt
rx   a "boot is pid 1" " +1 +0 +16 +0 [a-z]+ +[0-9]+ +[0-9]+ boot"
rx   a "diskd is pid 2, started by boot's first call, at nice -10" " +2 +0 +8 +-10 msg +[0-9]+ +[0-9]+ diskd"
rx   a "fsd is pid 3, at nice -10" " +3 +0 +8 +-10 msg +[0-9]+ +[0-9]+ fsd"
none a "no zfsd on an ext2 boot" " +[0-9]+ +[0-9]+ +[0-9]+ +-?[0-9]+ [a-z]+ +[0-9]+ +[0-9]+ zfsd"
line a "the log: diskd started for boot" "svc: disk: started @diskd (pid 2, for pid 1 boot)"
line a "the log: fsd started for boot" "svc: fs: started @fsd (pid 3, for pid 1 boot)"
line a "root cannot create the registered port fs" "svctest port fs: -16"
line a "... nor disk" "svctest port disk: -16"
rx   a "an unregistered fs.* name is still root's to create" "svctest port fs\.x: [0-9]+"
line a "fsd killed while idle: the log says it ended" "svc: fs: fsd (pid 3) ended, status 143"
rx   a "... and the next use started it again" "svc: fs: started @fsd \(pid [0-9]+, for pid [0-9]+ [a-z]+\)"
same a "... files are there after the restart" n1 n2
line a "... the root is still writable (the restart used -root)" "write-ok"
rx   a "... the new fsd runs at nice -10" " +[0-9]{2,} +0 +8 +-10 msg +[0-9]+ +[0-9]+ fsd"
line a "diskd killed while idle: the log says it ended" "svc: disk: diskd (pid 2) ended, status 143"
rx   a "... and the next use started it again" "svc: disk: started @diskd \(pid [0-9]+, for pid [0-9]+ [a-z]+\)"
same a "... files are there after the restart" n1 n3
line a "the script ran to the end" "svc-a-done"
fi

# ---- z: the ZenixFS root ----
if want_part z; then
run z fsuae/zroot.fs-uae 120 tools/tests/svc-zroot-test.txt
rx   z "zfsd serves the ZenixFS root as pid 3" " +3 +0 +8 +-10 msg +[0-9]+ +[0-9]+ zfsd"
none z "no fsd serves the root on a ZenixFS boot (the fsd in ps is fstab's ext2 mount)" "svc: fs: started @fsd.*"
line z "zfsd killed while idle: the log says it ended" "svc: fs: zfsd (pid 3) ended, status 143"
rx   z "... and the next use started it again" "svc: fs: started @zfsd \(pid [0-9]+, for pid [0-9]+ [a-z]+\)"
same z "... files are there after the restart" n1 n2
line z "... the root is still writable" "write-ok"
line z "the script ran to the end" "svc-z-done"
fi

# ---- b: the bootable flag picks the root ----
if want_part b; then
B=$OUT/two.hdf
mke2fs -q -t ext2 -E revision=0 -b 1024 -F "$OUT/decoy.img" 2M || { echo "FAIL mke2fs"; exit 1; }
python3 tools/mkrdb-multi.py "$B" "$OUT/decoy.img:ext2:DH0:-" $BD/zenix.hdf:ext2:DH1:boot >/dev/null || { echo "FAIL mkrdb-multi"; exit 1; }
conf b "$B"
run b "$OUT/b.fs-uae" 100 tools/tests/svc-boot-test.txt
rx   b "with two ext2 partitions the bootable second one is the root" "ide0p2 +/ +ext2 .*"
line b "the script ran to the end" "svc-b-done"
rm -f "$B" "$OUT/decoy.img"
fi

# ---- n: no root filesystem at all ----
if want_part n; then
N=$OUT/none.hdf
head -c 4194304 /dev/zero > "$N"
conf n "$N"
run n "$OUT/n.fs-uae" 40 ""
line n "fsd says why" "fsd: no writable ext2 filesystem on ide0 for the root"
line n "the panic names the reason at once" "*** kernel panic: svc: an essential server ended before it served"
rm -f "$N"
fi

# ---- r: the RAM floppy, no hard disk ----
if want_part r; then
grep -v '^hard_drive_0' fsuae/a1200-020-fast.fs-uae | sed 's#^floppy_drive_0 = .*#floppy_drive_0 = ../build/zenix-ram.adf#' > "$OUT/r.fs-uae"
run r "$OUT/r.fs-uae" 100 tools/tests/svc-ram-test.txt
rx   r "with no hard disk the RAM disk is the root (df)" "ram0 .* /"
line r "the script ran to the end" "svc-r-done"
fi

# ---- d: diskd keeps its slots and claims across a restart ----
if want_part d; then
cp $BD/zenix.hdf "$OUT/root.hdf"
head -c 901120 /dev/zero > "$OUT/df1.adf"
conf d "$OUT/root.hdf"
printf 'floppy_drive_1 = %s\nfloppy_drive_count = 2\n' "$OUT/df1.adf" >> "$OUT/d.fs-uae"
run d "$OUT/d.fs-uae" 150 tools/tests/svc-disk-test.txt
line d "the mounted root cannot be opened raw" "svctest raw ide0: -16"
rx   d "diskd killed while idle comes back" "svc: disk: started @diskd \(pid [0-9]+, for pid [0-9]+ [a-z]+\)"
if [ "$(grep -acx 'svctest raw ide0: -16' "$OUT/d.log")" -ge 2 ]; then echo "PASS ... the claims survived: still refused after the restart"; else echo "FAIL ... the claims survived: still refused after the restart"; pass=0; fi
line d "... and mount says the root's device is in use" "mount: ide0 is already in use"
rx   d "a floppy probed on demand (df1) keeps its handle across the restart" ".*svctest hold df1: handle [0-9]+, read 0, after the wait 0"
rx   d "diskd killed twice during a copy ends at once" "svc: disk: diskd \(pid [0-9]+\) ended, status 143(, during request [0-9]+ for pid [0-9]+)?"
line d "... the copy is right (cmp)" "cmp-ok"
line d "the script ran to the end" "svc-d-done"
if e2fsck -fn "$OUT/root.hdf" > "$OUT/e2fsck.log" 2>&1; then echo "PASS the host's e2fsck finds the root clean after the restarts"; else echo "FAIL the host's e2fsck finds the root clean after the restarts (see $OUT/e2fsck.log)"; pass=0; fi
rm -f "$OUT/root.hdf" "$OUT/df1.adf"
fi

free_grew() {   # free_grew LOG WHAT A B MIN: the number in B=... exceeds the one in A=... by at least MIN
    local a b; a=$(grep -aoxE "$3=[0-9]+" "$OUT/$1.log" | head -1); b=$(grep -aoxE "$4=[0-9]+" "$OUT/$1.log" | head -1)
    a=${a#*=}; b=${b#*=}
    if [ -n "$a" ] && [ -n "$b" ] && [ $((b - a)) -ge "$5" ]; then echo "PASS $2 ($a -> $b KB)"; else echo "FAIL $2 ($a -> $b KB)"; pass=0; fi
}

# ---- o: the ext2 root's restart rules ----
if want_part o; then
cp $BD/zenix.hdf "$OUT/root.hdf"
conf o "$OUT/root.hdf"
run o "$OUT/o.fs-uae" 200 tools/tests/svc-open-test.txt
rx   o "a deleted file held open reads the same after fsd restarted" ".*svctest keep /tmp/keep: read 4096, deleted 0, again 4096: the same bytes"
rx   o "... the restart happened meanwhile" "svc: fs: started @fsd \\(pid [0-9]+, for pid [0-9]+ [a-z]+\\)"
free_grew o "... its space came back at its last close" f1 f2 100
free_grew o "rm after a restart gives the space back" f3 f4 100
# a kill can land while fsd is idle between requests (about 2% of runs, audit3 T1): correctly no mark then, so the
# script kills again (at most 3 times) until the log says "during request", and only then is the mark required
rx   o "fsd killed in the middle of a request (within 3 tries)" "midreq-tries=[123]"
rx   o "... the log says so" ".*svc: fs: fsd \(pid [0-9]+\) ended, status [0-9]+, during request [0-9]+ for pid [0-9]+"
line o "fsd killed in the middle of a change: the next one says the volume needs a check" "fsd: ide0 was being changed when its server ended: check it with fsck.ext2"
line o "the script ran to the end" "svc-o-done"
if dumpe2fs -h "$OUT/root.hdf" 2>/dev/null | grep -q '^Filesystem state: *not clean'; then echo "PASS ... the host sees the state bit cleared (dumpe2fs: not clean)"; else echo "FAIL ... the host sees the state bit cleared (dumpe2fs: not clean)"; pass=0; fi
rm -f "$OUT/root.hdf"
fi

# ---- zo: the ZenixFS root's restart rules ----
if want_part zo; then
cp $BD/zenix-zroot.hdf "$OUT/zroot.hdf"
conf zo "$OUT/zroot.hdf"
run zo "$OUT/zo.fs-uae" 200 tools/tests/svc-zopen-test.txt
rx   zo "a deleted file held open (an orphan) reads the same after zfsd restarted" ".*svctest keep /tmp/keep: read 4096, deleted 0, again 4096: the same bytes"
free_grew zo "... and is freed at its last close" f1 f2 100
rx   zo "a file not yet committed when zfsd died: its open file reads an error" ".*svctest lost /tmp/lw: wrote 4, read -[0-9]+ \"\""
none zo "... never the next new file's bytes" ".*svctest lost /tmp/lw: wrote 4, read 4 \"BBBB\""
line zo "the new file is fine" "BBBB"
line zo "the script ran to the end" "svc-zo-done"
rm -f "$OUT/zroot.hdf"
fi

# ---- u: a copy with the same uuid beside the root ----
if want_part u; then
python3 tools/mkrdb-multi.py "$OUT/dup.hdf" $BD/zenix.hdf:ext2:DH0:- $BD/zenix.hdf:ext2:DH1:boot >/dev/null || { echo "FAIL mkrdb-multi"; exit 1; }
conf u "$OUT/dup.hdf"
run u "$OUT/u.fs-uae" 100 tools/tests/svc-dup-test.txt
rx   u "with a copy beside it the restarted fsd still serves the original (the place first)" "svc: fs: started @fsd \\(pid [0-9]+, for pid [0-9]+ [a-z]+\\)"
line u "... a file written before the restart is there (not the copy's root)" "original"
line u "... and the root is writable again" "write-ok"
line u "the script ran to the end" "svc-u-done"
rm -f "$OUT/dup.hdf"
fi

# ---- l: limits, back-off, deadline ----
if want_part l; then
run l fsuae/a1200-020-fast.fs-uae 260 tools/tests/svc-limit-test.txt
line l "registering a file service" "svctest reg ping: 0"
line l "... no pingd before the first call" "before=0"
rx   l "... the first call starts it" ".*svctest call ping 1: 0 in [0-9]+ ms"
rx   l "... at its nice (0)" " +[0-9]+ +0 +16 +0 [a-z]+ +[0-9]+ +[0-9]+ pingd"
rx   l "... the log names the start" "svc: ping: started /bin/pingd \(pid [0-9]+, for pid [0-9]+ svctest\)"
rx   l "a quit while idle is a stop" "svc: ping: pingd \(pid [0-9]+\) stopped"
line l "... no pingd after it" "after-quit=0"
rx   l "a crash in a request: the client gets -104" ".*svctest call ping 7: -104 in [0-9]+ ms"
L=$OUT/l.log
t=$(tr -d '\r' < "$L" | grep -ao 'svctest call ping 7: -104 in [0-9]* ms' | grep -o '[0-9]* ms' | tr -d ' ms' | tr '\n' ' ')
set -- $t
if [ $# -ge 5 ] && [ $(( $3 - $2 )) -ge 700 ] && [ $(( $3 - $2 )) -le 1500 ] && [ $(( $4 - $2 )) -ge 1700 ] && [ $(( $4 - $2 )) -le 2500 ] \
   && [ $(( $5 - $2 )) -ge 3700 ] && [ $(( $5 - $2 )) -le 4600 ]; then echo "PASS the back-off: the 3rd, 4th and 5th start wait about 1, 2 and 4 s more ($t ms)"
else echo "FAIL the back-off: the 3rd, 4th and 5th start wait about 1, 2 and 4 s more ($t ms)"; pass=0; fi
line l "five ends in 60 s: given up" "svc: ping: given up after 5 ends in 60 s"
rx   l "... the next call fails at once" ".*svctest call ping 1: -104 in [0-9] ms"
if [ "$(grep -ac 'svctest call ping 1: 0 in' "$L")" -ge 3 ]; then echo "PASS registering it again clears the give-up"; else echo "FAIL registering it again clears the give-up"; pass=0; fi
line l "a start-up that calls its own port fails at once" "svc: own: its own server called it before it opened its port"
rx   l "... and its caller gets -104" ".*svctest call own 1: -104 in [0-9]+ ms"
rx   l "a loader killed in its back-off: the next call works" ".*svctest call k 1: 0 in [0-9]+ ms"
rx   l "a cycle of two services: the deadline ends it" "svc: c[12]: pingd did not open its port in 30 s"
rx   l "... and the caller gets -104 after about 30 s" ".*svctest call c1 1: -104 in (29|3[0-9])[0-9]{3} ms"
line l "the script ran to the end" "svc-l-done"
fi

# ---- f: a failed start ends the call (audit A1, A2) ----
if want_part f; then
run f fsuae/a1200-020-fast.fs-uae 120 tools/tests/svc-fail-test.txt
rx   f "a start that fails: the caller gets -104" ".*svctest call f 1: -104 in [0-9]+ ms"
line f "... a stop while it waits afterwards stops it" "a1-stopped=1"
line f "... and a kill ends it at once" "a1-left=0 a1-end"
line f "a loader killed in its back-off ends" "a1b-left=0 a1b-end"
if [ "$(grep -ac 'svctest call f 1: -104 in' "$OUT/f.log")" = 4 ]; then echo "PASS ... at once: it printed nothing after the kill"; else echo "FAIL ... at once: it printed nothing after the kill"; pass=0; fi
rx   f "a nice 19 loader whose start failed is back at its own level" " +[0-9]+ +[0-9]+ +31 +19 [a-z]+ +[0-9]+ +[0-9]+ svctest"
none f "... not at the queued nice -20 client's" " +[0-9]+ +[0-9]+ +[0-9] +19 [a-z]+ +[0-9]+ +[0-9]+ svctest"
line f "... and ends at a kill" "a2-left=0 a2-end"
rx   f "a crash in a request: the next start is told so (start 2 + 65536)" ".*svctest call m 3: 65538 in [0-9]+ ms"
rx   f "... a clean stop after it clears that: the start after it is not (start 3, audit A8)" ".*svctest call m 3: 3 in [0-9]+ ms"
line f "the script ran to the end" "svc-f-done"
fi

# ---- s: reboot with a dead server and a sleeping loader ----
if want_part s; then
cp $BD/zenix.hdf "$OUT/root.hdf"
conf s "$OUT/root.hdf"
run s "$OUT/s.fs-uae" 120 tools/tests/svc-sync-test.txt
rx   s "the loader sleeps in a back-off of 4 s (four ends)" "svc: ping: start 5 when next used"
line s "the root server was dead before the reboot" "svc: fs: fsd (pid 3) ended, status 130"
line s "the sync skipped it at once, without an error" "reboot: filesystems synced, disk writes stopped; resetting"
rx   s "... the machine reset and booted again" "klog: boot 2, .*"
if e2fsck -fn "$OUT/root.hdf" > "$OUT/e2fsck-s.log" 2>&1; then echo "PASS the host's e2fsck finds the root clean"; else echo "FAIL the host's e2fsck finds the root clean"; pass=0; fi
rm -f "$OUT/root.hdf"
fi

# ---- h: reboot with a server that hangs on its sync (audit A7) ----
if want_part h; then
cp $BD/zenix.hdf "$OUT/root.hdf"
conf h "$OUT/root.hdf"
run h "$OUT/h.fs-uae" 100 tools/tests/svc-hang-test.txt
rx   h "a fake filesystem that hangs on sync is mounted" ".*svctest mount /tmp/hang: 0"
rx   h "a mount whose server gives no root fails" ".*svctest mount /tmp/bad: -[0-9]+"
rx   h "... and the server was told to quit: its port is free again (audit A9)" ".*svctest port fs\.bad: [0-9]+"
rx   h "a mounted server that ended: its closed port gives an I/O error (audit A11: was -1, \"cannot open\")" "cat: /tmp/gone/x: input/output error"
rx   h "... and a new server on that port name is found again" "cat: /tmp/gone/x: not implemented"
line h "the reboot gives it 5 s, then resets, naming it" "reboot: fs.hang did not sync in 5 s; resetting"
rx   h "... the machine booted again" "klog: boot 2, .*"
if e2fsck -fn "$OUT/root.hdf" > "$OUT/e2fsck-h.log" 2>&1; then echo "PASS the host's e2fsck finds the root clean (it was synced first)"; else echo "FAIL the host's e2fsck finds the root clean (it was synced first)"; pass=0; fi
rm -f "$OUT/root.hdf"
fi

# ---- t: /etc/svctab ----
if want_part t; then
cp $BD/zenix.hdf "$OUT/root.hdf"
conf t "$OUT/root.hdf"
run t "$OUT/t.fs-uae" 160 tools/tests/svc-tab-test.txt
line t "a bad line is reported and skipped" "boot: /etc/svctab: line skipped: bad"
rx   t "... no pingd before the first call" "(.*# )?before=0"
rx   t "the first call starts it" ".*svctest call ping 1: 0 in [0-9]+ ms"
rx   t "... at the nice from the table (5)" " +[0-9]+ +0 +[0-9]+ +5 [a-z]+ +[0-9]+ +[0-9]+ pingd"
rx   t "... the log names the start" "svc: ping: started /bin/pingd \(pid [0-9]+, for pid [0-9]+ svctest\)"
rx   t "ping's quit is a stop" ".*svc: ping: pingd \(pid [0-9]+\) stopped"
rx   t "... none left after it" "(.*# )?after-quit=0"
if [ "$(grep -ac 'svc: ping: started /bin/pingd' "$OUT/t.log")" -ge 3 ]; then echo "PASS ... the next ping starts it again"; else echo "FAIL ... the next ping starts it again"; pass=0; fi
if [ "$(grep -acE '.*svctest call gone 1: -104 in [0-9] ms' "$OUT/t.log")" -ge 3 ]; then echo "PASS a missing program fails at once, every time"; else echo "FAIL a missing program fails at once, every time"; pass=0; fi
rx   t "... the log says why" "svc: gone: cannot start /bin/nosuchprog \(error -2\)"
none t "... and it is never counted (no give-up)" "svc: gone: given up.*"
rx   t "a crash during a request: that client gets -104" ".*svctest call q 7: -104 in [0-9]+ ms"
rx   t "... the client queued behind it is served by the next server" ".*svctest call q 1: 0 in ([6-9][0-9]{2}|[0-9]{4,}) ms"
rx   t "the root server stopped, a file service read from it: the load started the root server too" "svc: fs: started @fsd \(pid [0-9]+, for pid [0-9]+ svctest\)"
if [ "$(grep -acE '.*svctest call ping 1: 0 in [0-9]+ ms' "$OUT/t.log")" -ge 2 ]; then echo "PASS ... and the call worked"; else echo "FAIL ... and the call worked"; pass=0; fi
line t "the script ran to the end" "svc-t-done"
rm -f "$OUT/root.hdf"
fi

# ---- dmg: a damaged saved table stops the machine ----
if want_part dmg; then
DB=$OUT/dmg-build
make -s BUILD="$DB" DIAG=-DSVC_DAMAGE_TEST "$DB/zenix.adf" "$DB/zenix.hdf" >/dev/null 2>&1 || { echo "FAIL the test build"; exit 1; }
ZENIX_BUILD=$DB ZENIX_SERIAL=$SER tools/run-test.sh fsuae/a1200-020-fast.fs-uae 80 tools/tests/svc-damage-test.txt 2>&1 | clean > "$OUT/dmg.log"
line dmg "diskd refuses its damaged table" "diskd: the saved device table is damaged: stopping so no disk gets another's writes"
line dmg "... and the kernel stops" "*** kernel panic: svc: an essential server's saved table is damaged"
rm -rf "$DB"
fi
exit $((1 - pass))
