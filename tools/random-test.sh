#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The random devices in FS-UAE (DESIGN D19 9.3, kernel/random.c, user/randseed.h). Parts (arguments; none: all):
#   f  a fresh image (no seed file): boot writes one (mode 600, flag 0); /dev/random waits; keys typed on the emulated
#      keyboard (the cage rig, tools/random-test.py) seed the pool, the waiting reader gets its bytes, boot rewrites the
#      file from the seeded pool (flag 1); reboot writes it once more
#   s  the same image's next boot: the seed is credited (the pool is seeded at once, /dev/random answers), the seed
#      file is rewritten (it differs from part f's), /dev/urandom gives other bytes than at part f's boot
#   r  the RAM floppy with a seed file put on its RAM disk (flag 1: from a seeded boot): never credited there,
#      /dev/random keeps waiting
# ZENIX_SERIAL, ZENIX_BUILD and ZENIX_OUT as every emulator test. Part f types for about 10 s after the reader got its
# bytes (half the keys seed it): the script waits for that before it types again.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all $BD/zenix-ram.adf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-random}; mkdir -p $OUT
SERIAL=${ZENIX_SERIAL:-/tmp/zenix-serial}
PARTS="${*:-f s r}"
want_part() { case " $PARTS " in *" $1 "*) return 0;; esac; return 1; }
pass=1
ok() { echo "PASS $1"; }
bad() { echo "FAIL $1"; pass=0; }
get() { debugfs -R "cat $1" "$OUT/root.hdf" 2>/dev/null | tr -d '\r'; }
has() { if grep -aqE -- "$2" <<<"$3"; then ok "$1"; else bad "$1"; fi; }

if want_part f || want_part s; then
    rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
    cp $BD/zenix.hdf "$OUT/root.hdf"
    debugfs -R "stat /var/lib/random-seed" "$OUT/root.hdf" 2>&1 | grep -q 'not found' && ok "the image ships no random seed" || bad "the image ships a random seed"
    sed "s#../build/zenix.hdf#$OUT/root.hdf#" fsuae/a1200-020-fast.fs-uae > "$OUT/r.fs-uae.in"
    ZENIX_SERIAL=$SERIAL ZENIX_BUILD=$BD python3 tools/fsuae_conf.py "$OUT/r.fs-uae.in" "$OUT/r.fs-uae" || exit 1
fi

if want_part f; then
    cat > $OUT/f.txt <<'EOS'
wait zenix:/# 90
sleep 1
send ls -l /var/lib/random-seed > /tmp/f-ls.txt; hexdump /var/lib/random-seed > /tmp/f-s1.txt; head -c 16 /dev/urandom | hexdump > /tmp/f-u.txt\r
sleep 2
send timeout 3 head -c 8 /dev/random > /dev/null; echo rs=$? > /tmp/f-w.txt; echo waited-f\r
wait (?m)^waited-f 30
send sh -c 'head -c 8 /dev/random > /tmp/f-r.bin; echo got-ra""ndom' &\r
sleep 2
send echo type-no""w\r
wait got-random 150
sleep 15
send hexdump /var/lib/random-seed > /tmp/f-s2.txt; dmesg | grep random: > /tmp/f-k.txt; sync; echo part-f-done\r
wait (?m)^part-f-done 30
send reboot\r
wait reboot: filesystems synced 30
sleep 1
stop
EOS
    rm -f "$SERIAL.stop"
    python3 tools/random-test.py "$OUT/r.fs-uae" "$SERIAL" "$OUT/f.txt" "$OUT/f.log" 300 > "$OUT/f.run" 2>&1
    grep -q '^FAIL: serial-tap' "$OUT/f.log.tap" && { bad "part f's script: $(grep '^FAIL: serial-tap' "$OUT/f.log.tap" | head -1)"; }
    has "boot wrote a seed file, root's, mode 600" '^-rw------- +1 root +root +40 ' "$(get /tmp/f-ls.txt)"
    has "... from an unseeded pool (flag 0)" '^00000000  5a 52 53 31 00 00 00 00' "$(get /tmp/f-s1.txt)"
    has "/dev/random waits on a fresh image (timeout ended it)" '^rs=124$' "$(get /tmp/f-w.txt)"
    [ "$(debugfs -R "cat /tmp/f-r.bin" "$OUT/root.hdf" 2>/dev/null | wc -c)" = 8 ] && ok "keys typed seed it: the waiting reader got its 8 bytes" || bad "the waiting reader got no bytes"
    has "... the kernel says it was seeded by keys" 'random: pool seeded \([0-9]+ bits\) after .*keys' "$(get /tmp/f-k.txt)"
    has "boot rewrote the seed from the seeded pool (flag 1)" '^00000000  5a 52 53 31 01 00 00 00' "$(get /tmp/f-s2.txt)"
    get /tmp/f-u.txt > "$OUT/f-u.txt"
    debugfs -R "cat /var/lib/random-seed" "$OUT/root.hdf" 2>/dev/null > "$OUT/f-seed.bin"
    [ "$(od -An -tx1 -j4 -N1 "$OUT/f-seed.bin" | tr -d ' ')" = 01 ] && ok "reboot wrote a seed with flag 1" || bad "reboot's seed: $(od -An -tx1 "$OUT/f-seed.bin" | head -1)"
    e2fsck -fn "$OUT/root.hdf" > "$OUT/e2fsck-f.log" 2>&1 && ok "the host's e2fsck finds the root clean after part f" || bad "e2fsck after part f"
fi

if want_part s; then
    [ -s "$OUT/f-seed.bin" ] || bad "part s needs part f's image"
    cat > $OUT/s.txt <<'EOS'
wait zenix:/# 90
sleep 1
send timeout 3 head -c 8 /dev/random > /dev/null; echo rs=$? > /tmp/s-w.txt; head -c 16 /dev/urandom | hexdump > /tmp/s-u.txt; dmesg | grep random: > /tmp/s-k.txt; sync; echo part-s-done\r
wait (?m)^part-s-done 30
sleep 1
stop
EOS
    ZENIX_SERIAL=$SERIAL ZENIX_BUILD=$BD tools/run-test.sh "$OUT/r.fs-uae" 120 $OUT/s.txt > "$OUT/s.log" 2>&1 || bad "part s's script"
    has "the next boot credits the seed: /dev/random answers at once" '^rs=0$' "$(get /tmp/s-w.txt)"
    has "... seeded by the seed root wrote" 'random: pool seeded \([0-9]+ bits\) after .*by a seed written by root' "$(get /tmp/s-k.txt)"
    debugfs -R "cat /var/lib/random-seed" "$OUT/root.hdf" 2>/dev/null > "$OUT/s-seed.bin"
    [ -s "$OUT/s-seed.bin" ] && ! cmp -s "$OUT/f-seed.bin" "$OUT/s-seed.bin" && ok "the seed file was rewritten at the start (it differs)" || bad "the seed file was not rewritten"
    [ "$(od -An -tx1 -j4 -N1 "$OUT/s-seed.bin" | tr -d ' ')" = 01 ] && ok "... flag 1 (the credited seed came from a seeded pool)" || bad "part s's seed flag"
    if [ -s "$OUT/f-u.txt" ] && [ -n "$(get /tmp/s-u.txt)" ] && [ "$(get /tmp/s-u.txt)" != "$(cat "$OUT/f-u.txt")" ]; then ok "two boots give different /dev/urandom bytes"; else bad "two boots' /dev/urandom: '$(cat "$OUT/f-u.txt")' and '$(get /tmp/s-u.txt)'"; fi
    e2fsck -fn "$OUT/root.hdf" > "$OUT/e2fsck-s.log" 2>&1 && ok "the host's e2fsck finds the root clean after part s" || bad "e2fsck after part s"
fi

if want_part r; then
    # the RAM floppy, with a seed file (flag 1, as a seeded boot writes it) put on its RAM disk by the host
    RB="$OUT/ramb"; rm -rf "$RB"; mkdir -p "$RB"
    cp $BD/root-ram.img "$RB/root-ram.img"
    { printf 'ZRS1\001\000\000\000'; head -c 32 /dev/urandom; } > "$OUT/seed.bin"
    debugfs -w -R "mkdir /var" "$RB/root-ram.img" >/dev/null 2>&1
    debugfs -w -R "mkdir /var/lib" "$RB/root-ram.img" >/dev/null 2>&1
    debugfs -w -R "write $OUT/seed.bin /var/lib/random-seed" "$RB/root-ram.img" >/dev/null 2>&1
    python3 tools/mkadf.py $BD/bootblock.bin $BD/kernel.bin "$RB/zenix-ram.adf" "$RB/root-ram.img" >/dev/null || bad "the RAM floppy with a seed"
    sed "s#../../build/zenix-ram.adf#$RB/zenix-ram.adf#; s#^hard_drive_0.*##" fsuae/tools/disktools.fs-uae > "$OUT/ram.fs-uae.in"
    ZENIX_SERIAL=$SERIAL ZENIX_BUILD=$BD python3 tools/fsuae_conf.py "$OUT/ram.fs-uae.in" "$OUT/ram.fs-uae" || exit 1
    cat > $OUT/r.txt <<'EOS'
wait zenix:/# 90
sleep 1
send ls /var/lib; sh -c 'head -c 8 /dev/random > /tmp/x; echo got-ra""ndom' & sleep 6; echo r-wai""ted; dmesg | grep random\r
wait (?m)^r-waited 40
sleep 2
stop
EOS
    ZENIX_SERIAL=$SERIAL ZENIX_BUILD=$BD tools/run-test.sh "$OUT/ram.fs-uae" 100 $OUT/r.txt > "$OUT/r.log" 2>&1 || bad "part r's script"
    grep -aqx 'random-seed' "$OUT/r.log" && ok "the RAM disk holds a seed file (flag 1)" || bad "the RAM disk's seed file"
    grep -aq 'boot: random seed not credited (the root is a RAM disk or read-only)' "$OUT/r.log" && ok "a RAM-disk root never credits its seed (boot says so)" || bad "boot's word on the RAM disk's seed"
    grep -aq 'got-random' "$OUT/r.log" && bad "/dev/random answered on the RAM-disk root" || ok "... /dev/random still waits after 6 s"
    grep -aq 'random: pool seeded' "$OUT/r.log" && bad "the pool was seeded on the RAM-disk root" || ok "... and the pool is not seeded"
fi
exit $((1 - pass))
