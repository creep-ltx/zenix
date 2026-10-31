#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Paths (docs/architecture.md section 7, "Paths"): trees deeper than the old 63 and 159 bytes on ext2, FFS, PFS3 and ZenixFS
# through cp -r, find, ls, tar, rm -r, du, grep, diff; 255 bytes go and 256 say "file name too long", by a system call, by a
# tool's joined path and by a working directory's; a folder that cannot be read is never taken for an empty one (rm, cp,
# find, tar, du, mv); the volumes judged afterwards on the PC.
# usage: tools/paths-test.sh
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-paths-test}; mkdir -p "$OUT"
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-paths-serial}
export LC_ALL=C.UTF-8 SOURCE_DATE_EPOCH=1700000000
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
has() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
hasnot() { if tr -d '\r' < "$L" | grep -aqE -- "$2"; then echo "FAIL $1"; pass=0; else echo "PASS $1"; fi; }
is() { has "$1" "^(.*# )?$2=$3\$"; }
isnot0() { if tr -d '\r' < "$L" | grep -aqE "^(.*# )?$2=[1-9]"; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
L=$OUT/paths.log
CARD=$OUT/paths-card.hdf
python3 -B tools/paths-test-images.py "$BD/zenix.hdf" "$CARD" "$OUT" || { echo "FAIL the images"; exit 1; }
sed -e "s#hard_drive_0 = .*#hard_drive_0 = $(realpath "$CARD")\nhard_drive_0_type = rdb#" fsuae/a1200-020-fast.fs-uae > "$OUT/paths.fs-uae"
tools/run-test.sh "$OUT/paths.fs-uae" 900 tools/tests/paths-test.txt > "$L" 2>&1
TL='file name too long'
echo "--- ext2, nothing else mounted"
has "the script ran to the end" '^(.*# )?paths-test-done$'
is "mkdir -p, 216 bytes deep" a1 0
is "... a file there" a2 deep-file
is "cp -r" a3 0
is "find sees it all" a4 9
is "ls -R" a5 1
is "ls" a6 fil.txt
is "tar c" a7 0
is "tar t" a8 9
is "tar x" a9 0
is "diff -r: the same" a10 0
is "du" a11 1
is "grep -r" a12 1
is "chown -R" a13 0
is "tree" a14 1
is "cd, pwd" a15 233
is "... a relative name there" a16 deep-file
is "rm -r" a17 0
is "... gone" a18 1
echo "--- the limit"
is "255 bytes: created" b1 0
is "... read" b2 limit
is "... listed" b3 256
is "256 bytes: cat" b4 1
has "... says so" "^cat: /tmp/a+/a+/a+/a+/b{47}: $TL\$"
isnot0 "... a redirection" b5
isnot0 "... mkdir" b6
has "... mkdir says so" "^mkdir: /tmp/a+/a+/a+/a+/b{47}: $TL\$"
isnot0 "... ls" b7
has "... ls says so" "^ls: /tmp/a+/a+/a+/a+/b{47}: $TL\$"
isnot0 "... rm -f too (it hides only \"not there\")" b8
has "... rm says so" "^rm: /tmp/a+/a+/a+/a+/b{47}: $TL\$"
isnot0 "... find" b9
isnot0 "... cd" b10
isnot0 "... stat" b11
isnot0 "... touch" b12
is "a link target of 255 bytes" c1 0
is "... read back" c2 256
is "... followed" c3 limit
isnot0 "... of 256 refused" c4
isnot0 "rename to 256 bytes refused" c5
has "... says so" "^mv: cannot move .*: $TL\$"
isnot0 "... to 257" c7
is "... the file still there" c8 limit
is "below the limit by a relative name" d1 0
isnot0 "... cd past the limit refused" d2
is "... ls by the relative name" d3 0
isnot0 "find past the limit: not 0" d4
has "... names the path and says why" "^find: /tmp/a+/a+/a+/a+/a+/f: $TL\$"
isnot0 "cp -r: not 0" d5
has "... says why" "^cp: .*/a+/f: $TL\$"
isnot0 "tar c: not 0" d6
has "... says why" "^tar: .*/a+/f: $TL\$"
isnot0 "rm -r: not 0" d7
has "... says why" "^rm: .*/a+/f: $TL\$"
is "... and it is still there" d9 0
is "from inside it goes" d10 0
is "... and the rest" d11 0
echo "--- with mounts"
is "FFS read-write" e1 0
is "PFS3" e2 0
has "FFS listed" 'ide0p3 +/mnt +ffs '
has "PFS3 listed" 'ide0p4 +/p +pfs3 '
is "255 bytes still go" e3 limit
isnot0 "256 refused" e4
isnot0 "the working directory counts now" e5
isnot0 "... past the limit by a relative name" e6
echo "--- FFS"
is "cp -r out" f1 0
is "... all of it" f2 10
is "ls -R" f3 1
is "ls" f4 fil.txt
is "tar c" f5 0
is "tar t" f6 10
is "cp -r on the volume" f7 0
is "diff -r: the same" f8 0
is "rm -r" f9 0
is "... gone" f10 1
isnot0 "find over the limit: not 0" f11
has "... says why" "^find: /mnt/over/.*: $TL\$"
isnot0 "cp -r: not 0" f12
is "cd, pwd" f13 233
is "a path through a link across the mount" g1 deep-file
is "rename through it" g2 0
is "... back" g3 0
is "... the file as it was" g4 deep-file
isnot0 "rm through a link loop: not 0" g5
has "... the loop's own error" '^rm: /mnt/la/x: too many (levels of )?symbolic links$'
is "... and the files the loop's raw text names stay" g6 keepkeep
echo "--- a folder that cannot be read"
isnot0 "rm -r: not 0" h1
has "... the real reason" '^rm: /mnt/skadad: input/output error$'
hasnot "... not \"not empty\"" '^rm: /mnt/skadad: .*not empty'
is "... the file in it still there" h2 0
isnot0 "rm -rf too" h3
isnot0 "cp -r: not 0" h4
isnot0 "find: not 0" h5
isnot0 "tar: not 0" h6
isnot0 "du: not 0" h7
isnot0 "mv to another volume: not 0" h8
is "... the file in it still there" h9 0
echo "--- PFS3"
is "cp -r out" i1 0
is "... the same as FFS's" i2 0
is "find" i3 10
is "ls -R" i4 1
is "tar c" i5 0
is "tar t" i6 10
isnot0 "find over the limit: not 0" i7
has "... says why" "^find: /p/over/.*: $TL\$"
isnot0 "rm -r refused: read-only" i8
is "cd, pwd" i9 233
echo "--- ZenixFS"
is "made" j1 0
is "mounted" j2 0
is "mkdir -p" j3 0
is "cp -r out" j4 0
is "find" j5 9
is "rm -r" j6 0
isnot0 "... gone" j7
is "umount" k1 0
hasnot "nothing crashed or ran out of stack" '(overflowed|exception|bus error|address error|PANIC|panic|Guru)'
echo "--- back on the PC"
read -r s1 n1 s2 a < "$OUT/layout"
dd if="$CARD" of="$OUT/p1.img" bs=512 skip="$s1" count="$n1" status=none
dd if="$CARD" of="$OUT/p3.img" bs=512 skip="$s2" count=$((a / 512)) status=none
if e2fsck -fn "$OUT/p1.img" > "$OUT/e2fsck.log" 2>&1; then check "e2fsck: the root is clean" ok; else check "e2fsck: the root" bad; tail -5 "$OUT/e2fsck.log"; fi
if python3 -B tools/paths-test-images.py --judge "$OUT" > "$OUT/judge.log" 2>&1; then check "the FFS volume is whole, its deep file as written" ok; else check "the FFS volume" bad; head -5 "$OUT/judge.log"; fi
if dd if="$CARD" bs=512 skip=$((s2 + a / 512)) count=$((a / 512)) status=none | cmp -s - "$OUT/b.img"; then check "PFS3: not one byte changed" ok; else check "PFS3: changed" bad; fi
rm -f "${OUT:?}/paths.fs-uae" "${CARD:?}" "${OUT:?}"/p[13].img "${OUT:?}"/[abc].img "${OUT:?}/rdb.img"
exit $((1 - pass))
