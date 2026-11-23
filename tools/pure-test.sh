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
#      a ZEXE program still runs. With memory protection on (a 68030/040/060: tools/mmu-suite.sh, DESIGN D60 7.4) the
#      write into the shared code never lands: it ends the writer with the protection line ("its own code (read only)")
#      and the copy stays in use, intact
#   m  a remount in the same slot: the same file (inode, size, time) on a floppy mounted again is not reused
#   s  sh and login are ZEX2 (stage 1): one session's sh is the shared code (login steps aside once the shell runs,
#      DESIGN D18: no login process stays); logins on consoles 2, 3 and 4 add no copy (free: what sharing saves grows
#      by sh each; ps: SHR for every sh, no login); a login waiting on a console is shared code too; sh -c shells
#      share it too; idle shells hold little memory (ps MEM at most 112 KB: the arena grows and shrinks), also right
#      after a command that needed a big arena; an rm, mv or ln elsewhere on the mount keeps the sharing; a session that ends (console 2, then console 1's own) leaves the others running; a literal
#      echoed into a file by a shell
# tools/pure-test.sh [PART...] runs only those parts (default: all).
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}       # the build directory (make check gives every test a copy of its own)
PARTS=" ${*:-p m s} "
want_part() { case "$PARTS" in *" $1 "*) return 0;; esac; return 1; }
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-pure}; mkdir -p "$OUT"
SER=${ZENIX_SERIAL:-/tmp/zenix-pure-serial}
pass=1
clean() { tr -d '\r' | sed 's/\x1b\[[0-9;:]*[mK]//g'; }
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
scount() { # scount LOG FROM TO WHAT N REGEX: exactly N lines between the markers ==FROM and ==TO match
    local n; n=$(awk -v a="==$2" -v b="==$3" '$0 == a { f = 1; next } $0 == b { f = 0 } f' "$OUT/$1.log" | grep -acxE -- "$6")
    if [ "$n" = "$5" ]; then ok "$4"; else bad "$4 ($n lines, not $5)"; fi
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
CK=$(codekb $BD/crash.pzx)
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
# compared with the old file's copy running alone (==busy), not with crashpure's (==one): crashpure's code is larger
# than two of hellopure's since libz's printf went to -Os (2026-11-12), so that comparison failed with two copies. The
# old copy (12 s) is started, replaced and measured in one typed line: the typing runs ahead of the Amiga in fast mode
cmp_ "the running program alone: its one copy" "$(S $L busy)" -gt "$(S $L base)"
cmp_ "renamed over while it runs: the next start loads the new file (two copies)" "$(S $L versions)" -gt "$(S $L busy)"
line $L "... the running one kept its old code to the end, the new one ran the new code" "old=60 new=10"
cmp_ "... both freed" "$(S $L gone)" -eq "$(S $L base)"
rx   $L "a file open for writing is started as a private copy (SHR 0, its code in MEM)" " +[0-9]+ +[0-9]+ +[0-9]+ +-?[0-9]+ [a-z]+ +([5-9]|[1-9][0-9]+) +0 hw"
cmp_ "... no shared copy of it" "$(S $L private)" -eq "$(S $L base)"
rx   $L "the filesystem server restarted" "svc: fs: started @fsd \(pid [0-9]+, for pid [0-9]+ [a-z]+\)"
cmp_ "... a start after it is a new generation: a new copy" "$(S $L gen2)" -gt "$(S $L gen1)"
if grep -aq '^mmu: memory protection on' "$OUT/$L.log"; then      # D41/D60: the write is stopped
np=$(grep -aoE '\[kernel\] pid [0-9]+ \(crashpure\): memory protection: a write to 0x[0-9a-f]+ \(its own code \(read only\)\): killed' "$OUT/$L.log" | sort -u | wc -l)
[ "$np" = 2 ] && ok "with memory protection: both writes into the shared code end the writer (read only)" ||
    bad "with memory protection: both writes into the shared code end the writer ($np processes, not 2)"   # (the lines again in dmesg)
count $L "... the code was never changed (no checksum line)" 0 ".*(damaged|DAMAGED) \(checksum\).*"
cmp_ "... the copy stays in use after the first" "$(S $L poked)" -eq "$(S $L one)"
cmp_ "... and after the second" "$(S $L damaged)" -eq "$(S $L one)"
else
line $L "a damaged copy is found at the next start" "exec: /tests/crashpure: its shared code in memory is damaged (checksum): loading a fresh copy"
cmp_ "... a fresh copy is loaded" "$(S $L poked)" -gt "$(S $L one)"
rx   $L "a process that damaged it and faulted: the log says so and names the one still running it" \
     "\[kernel\] pid [0-9]+ \(crashpure\): its shared code is DAMAGED \(checksum\): not used again; still running it: [0-9]+"
cmp_ "... it is not used again" "$(S $L damaged)" -gt "$(S $L one)"
fi
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
debugfs -w -R "write $BD/hello.pzx hp" "$OUT/df1.adf" >/dev/null 2>&1
debugfs -w -R "set_inode_field hp mode 0100755" "$OUT/df1.adf" >/dev/null 2>&1
sed "s#^floppy_drive_0 = .*#&\nfloppy_drive_1 = $OUT/df1.adf\nfloppy_drive_count = 2#" fsuae/a1200-020-fast.fs-uae > "$OUT/m.fs-uae"
run m "$OUT/m.fs-uae" 120 tools/tests/pure-mount-test.txt
line m "umount works while the program runs (it holds no file open)" "umount=0"
cmp_ "mounted again in the same slot: the same file is loaded again, not reused" "$(S m m2)" -gt "$(S m m1)"
line m "the script ran to the end" "pure-m-done"
fi

if want_part s; then
run s fsuae/a1200-020-fast.fs-uae 200 tools/tests/pure-shells-test.txt
SK=$(codekb $BD/sh.zx)
both=$(python3 -c 'import struct, sys; print(sum(struct.unpack(">I", open(f, "rb").read()[8:12])[0] for f in sys.argv[1:]))' $BD/sh.zx $BD/login.zx)
cmp_ "one session: sh is the shared code (login stepped aside)" "$(S s s1)" -eq "$SK"
cmp_ "... nothing saved yet" "$(V s s1)" -eq 0
cmp_ "a second login (console 2): no new copy" "$(S s s2)" -eq "$(S s s1)"
cmp_ "... sharing saves its sh" "$(V s s2)" -eq "$SK"
cmp_ "four logins (consoles 1-4): still one copy" "$(S s s4)" -eq "$(S s s1)"
SK3=$(python3 -c 'import struct, sys; print(3 * struct.unpack(">I", open(sys.argv[1], "rb").read()[8:12])[0] // 1024)' $BD/sh.zx)
cmp_ "... sharing saves three sessions' code" "$(V s s4)" -eq "$SK3"     # free rounds the sum, not each copy
scount s s4 s4b "ps shows the shared code for each of the four shells ($SK KB)" 4 " +[0-9]+ +[0-9]+ +[0-9]+ +-?[0-9]+ [a-z]+ +[0-9]+ +$SK sh"
scount s s4 s4b "... and no login process stays (DESIGN D18)" 0 ".* login"
# the shells' parent is init (login handed each to it): ps's PPID column is init's pid
INIT=$(awk '$0 == "==s4" { f = 1; next } $0 == "==s4b" { f = 0 } f && $NF == "init" { print $1 }' "$OUT/s.log" | head -n 1)
scount s s4 s4b "... each of the four shells is init's child (PPID ${INIT:-?})" 4 " +[0-9]+ +${INIT:-x} +[0-9]+ +-?[0-9]+ [a-z]+ +[0-9]+ +$SK sh"
# the shell's arena (audit 2026-10-25, found while fixing): an idle shell holds only its first 8 KB chunk, so its MEM
# (data and BSS, its 24 KB stack, the arena, malloc's first 32 KB chunk) stays small: 100 KB measured on 2026-10-25;
# with the old fixed arena (an eighth of free memory, up to 256 KB) it was about 290. The limit: 112 KB.
IDLE=$(awk '$0 == "==s4" { f = 1; next } $0 == "==s4b" { f = 0 } f && $5 == "io" && $NF == "sh" { print $6 }' "$OUT/s.log")
imax=-1; for m in $IDLE; do [ "$m" -gt "$imax" ] && imax=$m; done
[ "$(echo $IDLE | wc -w)" = 3 ] && cmp_ "the three idle shells (consoles 2-4) hold little memory (ps MEM, KB, the largest)" "$imax" -le 112 ||
    bad "three idle shells in ps between ==s4 and ==s4b (found: $IDLE)"
line s "a command that needs a big arena ran (30000 words from a 168894-byte command substitution)" "big-ok"
# console 1's shell: the parent of that ps (its state wait, or ready on Amiberry's timing: not the state, the PPID)
BIG=$(awk '$0 == "==s4c" { f = 1; next } $0 == "==s4d" { f = 0 } f && $NF == "ps" { pp = $2 } f && $NF == "sh" { mem[$1] = $6 }
     END { print mem[pp] }' "$OUT/s.log" | head -n 1)
cmp_ "... and the shell gave the memory back afterwards (ps MEM of console 1's shell, KB)" "${BIG:--1}" -le 112
cmp_ "two more shells (sh -c) share it too" "$(V s s4b)" -gt "$(V s s4)"
cmp_ "... still one copy" "$(S s s4b)" -eq "$(S s s1)"
cmp_ "after an rm, mv and ln in /tmp (the same mount) a new sh -c still shares the copy (audit3 B2)" "$(S s rm)" -eq "$(S s s1)"
cmp_ "... sharing saves its sh too" "$(V s rm)" -gt "$(V s s4)"
rx   s "console 2's shell ran before it ended" ".*alive-on-2"
rx   s "a session ended: console 3's shell still runs" ".*alive-on-3"
cmp_ "... still one copy of sh, and the new login waiting on console 2 is shared code too" "$(S s s3)" -eq $((both / 1024))
line s "a shell writes a literal into a file" "a literal"
rx   s "console 1's (the first) session ended and came back: console 3's shell still runs" ".*alive-after-the-first"
cmp_ "... still one copy (sh, and console 2's waiting login)" "$(S s s5)" -eq $((both / 1024))
line s "the script ran to the end" "pure-s-done"
fi

[ $pass = 1 ] && { echo "pure-test: all passed"; exit 0; }
echo "pure-test: FAILED"; exit 1
