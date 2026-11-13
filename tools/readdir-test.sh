#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Directories read in batches (DESIGN D49, docs/d49-readdir.md 9), in FS-UAE on the stock A1200 with 4 MB of fast RAM,
# the ext2 root of revision 0 (no file types in its records). A folder of 500 files, a folder, a link and a mount point
# (msgpingd -fsdir: a server that knows only FS_READDIR, the fallback) in /tmp/mid with 40 files and links:
#  - bench dir /tmp/big: one record a call against the batches, the same records both ways, every stat a batch gave
#    equal to lstat's, SYS_readdirs' own refusals; the two times printed
#  - bench dir on the old server's mount: libz's old path, the same records
#  - ls, ls -l and ls -laF of /tmp/mid (a folder small enough for its names as arguments) byte for byte the same as ls of the names as operands (whose rows are made by
#    lstat of each name, the old way); the counts of ls, of the old server's names, of sh's globbing
#  - the times of ls, ls -l and find on the folder (emulator figures: printed, never a claim about a real A1200)
#  - the host's e2fsck -fn clean afterwards
# Timed with the E-clock and the tick, the machine's own clocks: FS-UAE's fast mode does not change them.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-${TMPDIR:-/tmp}/zenix-readdir}; mkdir -p "$OUT"
SER=${ZENIX_SERIAL:-/tmp/zenix-readdir-serial}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
clean() { tr -d '\r' | sed 's/\x1b\[[0-9;:]*[mK]//g'; }
cp $BD/zenix.hdf $BD/readdir.hdf || exit 1
sed 's#\.\./build/zenix\.hdf#../build/readdir.hdf#' fsuae/a1200-020-fast.fs-uae > $OUT/readdir.fs-uae
ZENIX_SERIAL=$SER tools/run-test.sh $OUT/readdir.fs-uae 3000 tools/tests/readdir-test.txt 2>&1 | clean > $OUT/run.log
L=$OUT/run.log
grep -aq '^M-0-done' $L && check "the old server (msgpingd -fsdir) mounted on /tmp/big/mnt" ok || check "the old server mounted" bad
grep -aq '^bench dir: identical, 504 records, 502 stats$' $L && grep -aq '^B-0-done' $L &&
    check "bench dir: the batches give what one record a call gives, every stat is lstat's (504 records, 502 stats: not . and ..)" ok ||
    { check "bench dir on /tmp/big" bad; grep -a 'bench dir' $L | head -5; }
grep -aq '^bench dir: identical, 46 records, 43 stats$' $L && grep -aq '^D-0-done' $L &&
    check "bench dir on a folder with a mount point, a dangling link, a file of mode 750: 46 records, 43 stats (not . .. mnt)" ok ||
    { check "bench dir on /tmp/mid" bad; grep -a 'bench dir' $L | head -5; }
grep -aq "^bench dir: the directory's server reads one record a call" $L && grep -aq '^bench dir: identical, 52 records, 0 stats$' $L &&
    grep -aq '^F-0-done' $L && check "the old server: libz's old path, the same 52 records" ok || { check "the old server's listing" bad; grep -a 'bench dir\|F-' $L | tail -4; }
for c in 1 2 3; do
    grep -aq "^C$c-0-done" $L && check "ls $(case $c in 1) echo;; 2) echo -l;; 3) echo -laF;; esac) of the folder: byte for byte its names' rows made by lstat" ok ||
        { check "ls output C$c" bad; grep -a "C$c-" $L; }
done
grep -aq '^W-502-50-500-50-done' $L && check "counts: ls 502 names, the old server's 50, sh's f* 500, ls -l of the old server's 50 files" ok ||
    { check "counts" bad; grep -a '^W-' $L; }
grep -aq '^END-2' $L && check "the run ended (umount, sync)" ok || check "the run ended" bad
echo "emulator figures (FS-UAE, stock A1200 + 4 MB fast; not a real machine's):"
grep -a 'us total' $L | sed 's/^/  /'
awk '/^T-[a-z]+$/ { t = substr($0, 3) } /^real\t/ && t { printf "  %-6s %s\n", t, $2; t = "" }' $L
dd if=$BD/readdir.hdf of=$OUT/r.img bs=512 status=none
e2fsck -fn $OUT/r.img > $OUT/fsck.log 2>&1 && check "host e2fsck -fn: clean" ok || { check "host e2fsck -fn" bad; tail -5 $OUT/fsck.log; }
rm -f $OUT/r.img $BD/readdir.hdf
exit $((1 - pass))
