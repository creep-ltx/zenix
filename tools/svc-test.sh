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
#   dmg  a test-only kernel damages diskd's saved table: diskd stops with its message, the kernel panics
# tools/svc-test.sh [PART...] runs only those parts (default: all).
cd "$(dirname "$0")/.." || exit 1
PARTS=" ${*:-a z b n r d dmg} "
want_part() { case "$PARTS" in *" $1 "*) return 0;; esac; return 1; }
make -s all build/zenix-zroot.hdf build/zenix-ram.adf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-svc}; mkdir -p "$OUT"
SER=${ZENIX_SERIAL:-/tmp/zenix-svc-serial}
pass=1
clean() { tr -d '\r' | sed 's/\x1b\[K//g'; }
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
rx   a "boot is pid 1" " +1 +0 +16 +0 [a-z]+ +[0-9]+ boot"
rx   a "diskd is pid 2, started by boot's first call, at nice -10" " +2 +0 +8 +-10 msg +[0-9]+ diskd"
rx   a "fsd is pid 3, at nice -10" " +3 +0 +8 +-10 msg +[0-9]+ fsd"
none a "no zfsd on an ext2 boot" " +[0-9]+ +[0-9]+ +[0-9]+ +-?[0-9]+ [a-z]+ +[0-9]+ zfsd"
line a "the log: diskd started for boot" "svc: disk: started @diskd (pid 2, for pid 1 boot)"
line a "the log: fsd started for boot" "svc: fs: started @fsd (pid 3, for pid 1 boot)"
line a "root cannot create the registered port fs" "svctest port fs: -16"
line a "... nor disk" "svctest port disk: -16"
rx   a "an unregistered fs.* name is still root's to create" "svctest port fs\.x: [0-9]+"
line a "fsd killed while idle: the log says it ended" "svc: fs: fsd (pid 3) ended, status 130"
rx   a "... and the next use started it again" "svc: fs: started @fsd \(pid [0-9]+, for pid [0-9]+ [a-z]+\)"
same a "... files are there after the restart" n1 n2
line a "... the root is still writable (the restart used -root)" "write-ok"
rx   a "... the new fsd runs at nice -10" " +[0-9]{2,} +0 +8 +-10 msg +[0-9]+ fsd"
line a "diskd killed while idle: the log says it ended" "svc: disk: diskd (pid 2) ended, status 130"
rx   a "... and the next use started it again" "svc: disk: started @diskd \(pid [0-9]+, for pid [0-9]+ [a-z]+\)"
same a "... files are there after the restart" n1 n3
line a "the script ran to the end" "svc-a-done"
fi

# ---- z: the ZenixFS root ----
if want_part z; then
run z fsuae/zroot.fs-uae 120 tools/tests/svc-zroot-test.txt
rx   z "zfsd serves the ZenixFS root as pid 3" " +3 +0 +8 +-10 msg +[0-9]+ zfsd"
none z "no fsd on a ZenixFS boot" " +[0-9]+ +[0-9]+ +[0-9]+ +-?[0-9]+ [a-z]+ +[0-9]+ fsd"
line z "zfsd killed while idle: the log says it ended" "svc: fs: zfsd (pid 3) ended, status 130"
rx   z "... and the next use started it again" "svc: fs: started @zfsd \(pid [0-9]+, for pid [0-9]+ [a-z]+\)"
same z "... files are there after the restart" n1 n2
line z "... the root is still writable" "write-ok"
line z "the script ran to the end" "svc-z-done"
fi

# ---- b: the bootable flag picks the root ----
if want_part b; then
B=$OUT/two.hdf
mke2fs -q -t ext2 -E revision=0 -b 1024 -F "$OUT/decoy.img" 2M || { echo "FAIL mke2fs"; exit 1; }
python3 tools/mkrdb-multi.py "$B" "$OUT/decoy.img:ext2:DH0:-" build/zenix.hdf:ext2:DH1:boot >/dev/null || { echo "FAIL mkrdb-multi"; exit 1; }
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
cp build/zenix.hdf "$OUT/root.hdf"
head -c 901120 /dev/zero > "$OUT/df1.adf"
conf d "$OUT/root.hdf"
printf 'floppy_drive_1 = %s\nfloppy_drive_count = 2\n' "$OUT/df1.adf" >> "$OUT/d.fs-uae"
run d "$OUT/d.fs-uae" 150 tools/tests/svc-disk-test.txt
line d "the mounted root cannot be opened raw" "svctest raw ide0: -16"
rx   d "diskd killed while idle comes back" "svc: disk: started @diskd \(pid [0-9]+, for pid [0-9]+ [a-z]+\)"
if [ "$(grep -acx 'svctest raw ide0: -16' "$OUT/d.log")" -ge 2 ]; then echo "PASS ... the claims survived: still refused after the restart"; else echo "FAIL ... the claims survived: still refused after the restart"; pass=0; fi
line d "... and mount says the root's device is in use" "mount: ide0 is already in use"
rx   d "a floppy probed on demand (df1) keeps its handle across the restart" ".*svctest hold df1: handle [0-9]+, read 0, after the wait 0"
rx   d "diskd killed twice during a copy ends at once" "svc: disk: diskd \(pid [0-9]+\) ended, status 130(, during request [0-9]+ for pid [0-9]+)?"
line d "... the copy is right (cmp)" "cmp-ok"
line d "the script ran to the end" "svc-d-done"
if e2fsck -fn "$OUT/root.hdf" > "$OUT/e2fsck.log" 2>&1; then echo "PASS the host's e2fsck finds the root clean after the restarts"; else echo "FAIL the host's e2fsck finds the root clean after the restarts (see $OUT/e2fsck.log)"; pass=0; fi
rm -f "$OUT/root.hdf" "$OUT/df1.adf"
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
