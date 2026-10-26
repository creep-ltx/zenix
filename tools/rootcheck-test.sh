#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The ext2 clean mark and the check at boot (DESIGN D17, docs/d17-ext2-clean-mark.md; audit 2026-10-26 F5, F6), in the
# emulator, headless (the fast A1200, the root on the IDE disk):
#   r  a root with leaks put in by debugfs (a count too high, an unnamed file with data, an unnamed empty one, blocks
#      marked used that nothing holds) and its state "not clean": boot checks it with fsck.ext2 before anything else,
#      repairs it, the data comes back in /lost+found, the empty one is gone; then e2fsck -fn clean, state clean
#   b  the emulator killed while files are written: the root says "not clean" (at worst leaks); the next start checks
#      and repairs it, and the root is then e2fsck clean and clean again
#   i  killed 4 s after the last write (reading meanwhile): the root says clean and e2fsck finds nothing (FFS's rule:
#      an idle machine may be switched off); reboot after writing: the root is clean when the reset comes
#   h  /home in /etc/fstab, a second ext2 partition with a leak and "not clean": mount -a checks and repairs it before
#      mounting it
# tools/rootcheck-test.sh [PART...] runs only those parts (default: all).
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
PARTS=" ${*:-r b i h} "
want_part() { case "$PARTS" in *" $1 "*) return 0;; esac; return 1; }
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "FAIL build"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-rootcheck}; mkdir -p "$OUT"
SER=${ZENIX_SERIAL:-/tmp/zenix-rootcheck-serial}
pass=1
ok()  { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
run() {    # run NAME HDF SECONDS SCRIPT
    sed "s#^hard_drive_0 = .*#hard_drive_0 = $2#" fsuae/a1200-020-fast.fs-uae > "$OUT/$1.fs-uae"
    ZENIX_SERIAL=$SER tools/run-test.sh "$OUT/$1.fs-uae" "$3" "$4" 2>&1 | tr -d '\r' | sed 's/\x1b\[[0-9;:]*[mK]//g' > "$OUT/$1.log"
}
has()   { grep -aqE -- "$2" "$OUT/$1.log" && echo ok || echo bad; }
hasnt() { grep -aqE -- "$2" "$OUT/$1.log" && echo bad || echo ok; }
before() { # LOG A B: a line matching A comes before the first matching B
    local a b; a=$(grep -anE -- "$2" "$OUT/$1.log" | head -1 | cut -d: -f1); b=$(grep -anE -- "$3" "$OUT/$1.log" | head -1 | cut -d: -f1)
    [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ] && echo ok || echo bad
}
e2clean() { e2fsck -fn "$1" > "$1.e2fsck" 2>&1 && echo ok || echo bad; }
state() { dumpe2fs -h "$1" 2>/dev/null | sed -n 's/^Filesystem state: *//p'; }
isclean() { [ "$(state "$1")" = clean ] && echo ok || echo bad; }
leaky() {  # IMG: e2fsck finds only leaks, and the state says "not clean"
    e2fsck -fn "$1" > "$1.e2fsck" 2>&1 || python3 -I tools/e2safe.py "$1.e2fsck" >"$1.e2safe" || { echo bad; return; }
    [ "$(state "$1")" = "not clean" ] && echo ok || echo bad
}
# leaks IMG: what a power cut may leave, put in by debugfs, the state "not clean"
leaks() {
    printf 'unnamed data\n' > "$OUT/data.txt"; : > "$OUT/empty.txt"
    debugfs -w -f - "$1" >/dev/null 2>&1 <<EOF
sif /etc/motd links_count 3
write $OUT/data.txt /leakdata
write $OUT/empty.txt /leakempty
unlink /leakdata
unlink /leakempty
setb 3000 4
ssv state 0
EOF
    e2fsck -fn "$1" >/dev/null 2>&1 && echo "FAIL the leaks were not put in" && pass=0
}
P='wait zenix:/# 120'

# ---- r: leaks on the root, repaired at boot ----
if want_part r; then
img=$OUT/r.hdf; cp $BD/zenix.hdf $img; leaks $img
printf '%s\nsend ls /lost+found; cat /lost+found/*; echo ==r\\r\nwait (?m)^==r 20\nsleep 4\nstop\n' "$P" > $OUT/r.txt
run r $img 200 $OUT/r.txt
ok "r: boot says the root was not shut down cleanly and checks it" "$(has r 'boot: the root was not shut down cleanly: fsck.ext2 -y ide0')"
ok "r: ... fsck.ext2 repairs it, before the login" "$(before r 'fsck.ext2: [0-9]+ repair\(s\) made' 'zenix:/#')"
ok "r: ... then the writable root server (the check server stopped, not crashed)" "$(has r 'svc: fs: fsd \(pid [0-9]+\) stopped')"
ok "r: ... the unnamed file's data is in /lost+found" "$(has r '^unnamed data$')"
ok "r: ... the empty unnamed file was cleared, not attached" "$(has r 'and empty: cleared')"
ok "r: the root afterwards: e2fsck -fn clean" "$(e2clean $img)"
ok "r: ... and marked clean" "$(isclean $img)"
fi

# ---- b: killed while writing, then the next start ----
if want_part b; then
img=$OUT/b.hdf; cp $BD/zenix.hdf $img
printf '%s\nsend for i in 1 2 3 4 5 6 7 8 9 10 11 12; do cat /bin/sh > /tmp/w$i; rm /tmp/w$i; done; echo ==b\\r\nsleep 3\nstop\n' "$P" > $OUT/b1.txt
run b1 $img 200 $OUT/b1.txt
ok "b: killed while writing: the root says not clean, at worst leaks" "$(leaky $img)"
printf '%s\nsend echo ==b2\\r\nwait (?m)^==b2 20\nsleep 4\nstop\n' "$P" > $OUT/b2.txt
run b2 $img 200 $OUT/b2.txt
ok "b: the next start checks the root first" "$(before b2 'boot: the root was not shut down cleanly: fsck.ext2 -y ide0' 'zenix:/#')"
ok "b: ... with a result, no failure" "$([ "$(has b2 'fsck.ext2: ([0-9]+ repair\(s\) made|clean)$')" = ok ] && [ "$(hasnt b2 'boot: fsck.ext2 ended with status')" = ok ] && echo ok || echo bad)"
ok "b: the root afterwards: e2fsck -fn clean" "$(e2clean $img)"
ok "b: ... and marked clean" "$(isclean $img)"
fi

# ---- i: idle, and reboot ----
if want_part i; then
img=$OUT/i.hdf; cp $BD/zenix.hdf $img
# (FS-UAE keeps the last sectors written in a host buffer until the next disk access, and a kill loses them, as a
# disk cache without a flush would: some reads after the quiet moment make it write them out before the cut)
printf '%s\nsend echo idle-file > /tmp/idle; echo ==i\\r\nwait (?m)^==i 20\nsleep 4\nsend cat /bin/sh /bin/edit /bin/gzip > /dev/null; echo ==j\\r\nwait (?m)^==j 30\nsleep 1\nstop\n' "$P" > $OUT/i1.txt
run i1 $img 200 $OUT/i1.txt
ok "i: killed 4 s after the last write (only reads since): the root says clean" "$(isclean $img)"
ok "i: ... e2fsck -fn clean, the file there" "$([ "$(e2clean $img)" = ok ] && [ "$(debugfs -R 'cat /tmp/idle' $img 2>/dev/null)" = idle-file ] && echo ok || echo bad)"
img=$OUT/i2.hdf; cp $BD/zenix.hdf $img
printf '%s\nsend echo reboot-file > /tmp/rb; reboot\\r\nwait reboot: filesystems synced 30\nstop\n' "$P" > $OUT/i2.txt
run i2 $img 200 $OUT/i2.txt
ok "i: reboot right after a write: the root is clean when the reset comes" "$([ "$(isclean $img)" = ok ] && [ "$(debugfs -R 'cat /tmp/rb' $img 2>/dev/null)" = reboot-file ] && echo ok || echo bad)"
ok "i: ... e2fsck -fn clean" "$(e2clean $img)"
fi

# ---- h: a marked /home, checked by mount -a ----
if want_part h; then
rm -f $OUT/home.img; mke2fs -q -t ext2 -E revision=0 -b 1024 -F $OUT/home.img 4M
leaks $OUT/home.img
cp $BD/zenix.hdf $OUT/hroot.img
printf 'ide0p2 /home ext2 defaults\n' > $OUT/fstab
debugfs -w -f - $OUT/hroot.img >/dev/null 2>&1 <<EOF
rm /etc/fstab
write $OUT/fstab /etc/fstab
sif /etc/fstab uid 0
sif /etc/fstab gid 0
EOF
python3 tools/mkrdb-multi.py $OUT/h.hdf $OUT/hroot.img:ext2:DH0:boot $OUT/home.img:ext2:DH1:- >/dev/null || { echo "FAIL mkrdb-multi"; exit 1; }
printf '%s\nsend ls /home; cat /home/lost+found/*; echo ==h\\r\nwait (?m)^==h 20\nsleep 4\nstop\n' "$P" > $OUT/h.txt
run h $OUT/h.hdf 200 $OUT/h.txt
python3 tools/ffsread.py $OUT/h.hdf --part DH1 dump $OUT/home-after.img
ok "h: the root was clean: no check at boot" "$(hasnt h 'boot: the root .* was not shut down cleanly')"
ok "h: mount -a checks the marked /home before mounting it" "$(before h 'mount: ide0p2 was not unmounted cleanly: fsck.ext2 -y ide0p2' 'zenix:/#')"
ok "h: ... the unnamed file's data is in /home/lost+found" "$(has h '^unnamed data$')"
ok "h: /home afterwards: e2fsck -fn clean and clean" "$([ "$(e2clean $OUT/home-after.img)" = ok ] && [ "$(isclean $OUT/home-after.img)" = ok ] && echo ok || echo bad)"
fi

echo "logs in $OUT"
[ $pass = 1 ]
