#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The service manager (DESIGN D11, docs/d11-service-manager.md section 9) in the emulator, headless. Boots:
#   a  the normal ext2 root: boot 1, diskd 2, fsd 3, no zfsd; registered names are refused to root too; fsd and diskd
#      killed while idle come back on the next use (the root is still writable: the restart used -root), dmesg says so
#   z  the ZenixFS root (zenix-zroot.hdf): zfsd serves it as pid 3, and comes back after a kill
#   b  two ext2 partitions, the second marked bootable: it is the root (rootpick's rule, fsd no longer picks)
#   n  a disk with no filesystem and no RAM disk: the panic text at once
cd "$(dirname "$0")/.." || exit 1
make -s all build/zenix-zroot.hdf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
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

# ---- z: the ZenixFS root ----
run z fsuae/zroot.fs-uae 120 tools/tests/svc-zroot-test.txt
rx   z "zfsd serves the ZenixFS root as pid 3" " +3 +0 +8 +-10 msg +[0-9]+ zfsd"
none z "no fsd on a ZenixFS boot" " +[0-9]+ +[0-9]+ +[0-9]+ +-?[0-9]+ [a-z]+ +[0-9]+ fsd"
line z "zfsd killed while idle: the log says it ended" "svc: fs: zfsd (pid 3) ended, status 130"
rx   z "... and the next use started it again" "svc: fs: started @zfsd \(pid [0-9]+, for pid [0-9]+ [a-z]+\)"
same z "... files are there after the restart" n1 n2
line z "... the root is still writable" "write-ok"
line z "the script ran to the end" "svc-z-done"

# ---- b: the bootable flag picks the root ----
B=$OUT/two.hdf
mke2fs -q -t ext2 -E revision=0 -b 1024 -F "$OUT/decoy.img" 2M || { echo "FAIL mke2fs"; exit 1; }
python3 tools/mkrdb-multi.py "$B" "$OUT/decoy.img:ext2:DH0:-" build/zenix.hdf:ext2:DH1:boot >/dev/null || { echo "FAIL mkrdb-multi"; exit 1; }
conf b "$B"
run b "$OUT/b.fs-uae" 100 tools/tests/svc-boot-test.txt
rx   b "with two ext2 partitions the bootable second one is the root" "ide0p2 +/ +ext2 .*"
line b "the script ran to the end" "svc-b-done"
rm -f "$B" "$OUT/decoy.img"

# ---- n: no root filesystem at all ----
N=$OUT/none.hdf
head -c 4194304 /dev/zero > "$N"
conf n "$N"
run n "$OUT/n.fs-uae" 40 ""
line n "fsd says why" "fsd: no writable ext2 filesystem on ide0 for the root"
line n "the panic names the reason at once" "*** kernel panic: svc: an essential server ended before it served"
rm -f "$N"
exit $((1 - pass))
