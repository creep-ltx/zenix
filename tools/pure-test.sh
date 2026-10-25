#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Shared program code (DESIGN D13, docs/d13-shared-code.md section 11) in the emulator, headless. The test images'
# /tests hold ZEX2 builds of hello and crash (hellopure, crashpure; tools/mkroot.sh), a hellopure2 whose text says
# HELLO, and corrupt ZEX2 files. Parts:
#   p  on the normal ext2 root: one, two and four processes of one program share one copy (free: the shared code stays,
#      what sharing saves grows; ps: SHR for each); the first one ends and the others run on; at the end the code is
#      freed; two started at once (by two shells) share; a crash in one leaves the others (and the code is intact); a
#      literal passed to system calls works, a read into one is refused with -14 and the literal is unchanged (a ZEXE
#      build reads into it); a running program's file cannot be opened for writing (cp, >>: text file busy); one renamed
#      over it is picked up by the next start while the running one keeps the old code; a file open for writing is
#      started as a private copy (SHR 0); a restart of the filesystem server is a new generation (the next start loads a
#      new copy); a damaged copy is found at the next start (checksum) and when a process running it faults (the log
#      names the other one still running it), and is not used again; the corrupt ZEX2 files are refused with reasons;
#      a ZEXE program still runs
#   m  a remount in the same slot: the same file (inode, size, time) on a floppy mounted again is not reused
# tools/pure-test.sh [PART...] runs only those parts (default: all).
cd "$(dirname "$0")/.." || exit 1
PARTS=" ${*:-p m} "
want_part() { case "$PARTS" in *" $1 "*) return 0;; esac; return 1; }
make -s all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f build/zenix.hdf; make -s build/zenix.hdf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-pure}; mkdir -p "$OUT"
SER=${ZENIX_SERIAL:-/tmp/zenix-pure-serial}
pass=1
clean() { tr -d '\r' | sed 's/\x1b\[K//g'; }
run() {    # run NAME CONFIG SECONDS SCRIPT
    ZENIX_SERIAL=$SER tools/run-test.sh "$2" "$3" "$4" 2>&1 | clean > "$OUT/$1.log"
}
ok()   { echo "PASS $1"; }
bad()  { echo "FAIL $1"; pass=0; }
line() { if grep -aqxF -- "$3" "$OUT/$1.log"; then ok "$2"; else bad "$2"; fi; }
rx()   { if grep -aqxE -- "$3" "$OUT/$1.log"; then ok "$2"; else bad "$2"; fi; }
count() {  # count LOG WHAT N REGEX: exactly N lines match
    local n; n=$(grep -acxE -- "$4" "$OUT/$1.log")
    if [ "$n" = "$3" ]; then ok "$2"; else bad "$2 ($n lines, not $3)"; fi
}
# the free line after the marker ==NAME: "shared code: S KB, sharing saves V KB" -> S V
# (typed characters may land in front of it)
fv() { awk -v m="==$2" '$0 == m { f = 1; next }
    f && match($0, /shared code: [0-9]+ KB, sharing saves [0-9]+ KB/) { split(substr($0, RSTART, RLENGTH), a, " "); print a[3], a[7]; exit }' "$OUT/$1.log"; }
S() { set -- $(fv "$1" "$2"); echo "${1:--1}"; }
V() { set -- $(fv "$1" "$2"); echo "${2:--1}"; }
cmp_() {   # cmp_ WHAT A OP B
    if [ "$2" -ge 0 ] && [ "$4" -ge 0 ] && [ "$2" "$3" "$4" ]; then ok "$1 ($2 $3 $4)"; else bad "$1 ($2 $3 $4)"; fi
}
codekb() { python3 -c 'import struct, sys; print(struct.unpack(">I", open(sys.argv[1], "rb").read()[8:12])[0] // 1024)' "$1"; }

if want_part p; then
run p fsuae/a1200-020-fast.fs-uae 360 tools/tests/pure-test.txt
L=p
CK=$(codekb build/crash.pzx)
cmp_ "one process of a ZEX2 program: its code is shared code" "$(S $L one)" -gt "$(S $L base)"
cmp_ "... nothing saved yet" "$(V $L one)" -eq "$(V $L base)"
cmp_ "two processes: still one copy" "$(S $L two)" -eq "$(S $L one)"
cmp_ "... sharing saves the second copy" "$(V $L two)" -gt "$(V $L one)"
cmp_ "four processes: still one copy" "$(S $L four)" -eq "$(S $L one)"
cmp_ "... sharing saves three copies" "$(V $L four)" -gt "$(V $L two)"
count $L "ps shows the shared code ($CK KB) for each of the four" 4 " +[0-9]+ +[0-9]+ +[0-9]+ +-?[0-9]+ [a-z]+ +[0-9]+ +$CK crashpure"
cmp_ "the first one ended: the others still share the copy" "$(S $L after-first)" -eq "$(S $L one)"
count $L "... and all three ran to their end" 3 ".*crash: slept (30|25) s"
cmp_ "the last one ended: the code is freed" "$(S $L all-done)" -eq "$(S $L base)"
cmp_ "... nothing saved any more" "$(V $L all-done)" -eq "$(V $L base)"
cmp_ "two started at once by two shells share one copy" "$(S $L together)" -eq "$(S $L one)"
rx   $L "a crash in one: the kernel names where in the code" "\[kernel\] pid [0-9]+ \(crashpure\) killed: vector 4 pc=0x[0-9a-f]+ addr=0x0 \(code\+[0-9a-f]+\)"
rx   $L "... and says the shared code is intact" "\[kernel\] pid [0-9]+ \(crashpure\): its shared code is intact"
rx   $L "... the other one ran to its end" ".*crash: slept 3 s"
line $L "a literal written, a literal path opened, a spawn by a literal path; a read into a literal: -14, unchanged" \
     "crash: open of a literal path 1, read 4, read into a literal -14, it says: literal text, spawn ok"
rx   $L "... a ZEXE build reads into its literal" "crash: open of a literal path 1, read 4, read into a literal 4, it says: .*, spawn ok"
line $L "a running program's file opened for writing (cp): text file busy" "cp: /tmp/hp: text file busy"
line $L "... cp fails" "cp=1"
rx   $L "... appending to it (>>) is refused too" "sh: .*text file busy.*"
cmp_ "renamed over while it runs: the next start loads the new file (two copies)" "$(S $L versions)" -gt "$(S $L one)"
line $L "... the running one kept its old code to the end, the new one ran the new code" "old=30 new=10"
cmp_ "... both freed" "$(S $L gone)" -eq "$(S $L base)"
rx   $L "a file open for writing is started as a private copy (SHR 0, its code in MEM)" " +[0-9]+ +[0-9]+ +[0-9]+ +-?[0-9]+ [a-z]+ +([5-9]|[1-9][0-9]+) +0 hw"
cmp_ "... no shared copy of it" "$(S $L private)" -eq "$(S $L base)"
rx   $L "the filesystem server restarted" "svc: fs: started @fsd \(pid [0-9]+, for pid [0-9]+ [a-z]+\)"
cmp_ "... a start after it is a new generation: a new copy" "$(S $L gen2)" -gt "$(S $L gen1)"
line $L "a damaged copy is found at the next start" "exec: /tests/crashpure: its shared code in memory is damaged (checksum): loading a fresh copy"
cmp_ "... a fresh copy is loaded" "$(S $L poked)" -gt "$(S $L one)"
rx   $L "a process that damaged it and faulted: the log says so and names the one still running it" \
     "\[kernel\] pid [0-9]+ \(crashpure\): its shared code is DAMAGED \(checksum\): not used again; still running it: [0-9]+"
cmp_ "... it is not used again" "$(S $L damaged)" -gt "$(S $L one)"
for k in $(seq 1 14); do
    rx $L "corrupt ZEX2 file $k refused, with its reason" "exec: /tests/badzex2_$k refused: [a-z].*"
done
cmp_ "at the end no shared code is left" "$(S $L end)" -eq "$(S $L base)"
rx   $L "a ZEXE program still runs" "hello from pid [0-9]+, round 0"
line $L "the script ran to the end" "pure-p-done"
fi

if want_part m; then
head -c 901120 /dev/zero > "$OUT/df1.adf"
mke2fs -q -t ext2 -E revision=0 -b 1024 -F "$OUT/df1.adf" >/dev/null 2>&1 || { echo "FAIL mke2fs"; exit 1; }
debugfs -w -R "write build/hello.pzx hp" "$OUT/df1.adf" >/dev/null 2>&1
debugfs -w -R "set_inode_field hp mode 0100755" "$OUT/df1.adf" >/dev/null 2>&1
sed "s#^floppy_drive_0 = .*#&\nfloppy_drive_1 = $OUT/df1.adf\nfloppy_drive_count = 2#" fsuae/a1200-020-fast.fs-uae > "$OUT/m.fs-uae"
run m "$OUT/m.fs-uae" 120 tools/tests/pure-mount-test.txt
line m "umount works while the program runs (it holds no file open)" "umount=0"
cmp_ "mounted again in the same slot: the same file is loaded again, not reused" "$(S m m2)" -gt "$(S m m1)"
line m "the script ran to the end" "pure-m-done"
fi

[ $pass = 1 ] && { echo "pure-test: all passed"; exit 0; }
echo "pure-test: FAILED"; exit 1
