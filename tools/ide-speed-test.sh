#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Disk throughput through the Gayle IDE port (docs/d42-faster-ide.md), and that the data is right. One boot per
# machine: ext2-020 (ext2 root, 1 KB blocks, a 68EC020 with 4 MB fast), zfs-020 and zfs-060 (a ZenixFS root with 4 KB
# blocks, as on a big CF card; the 68060 run is the closest FS-UAE has to the Apollo 1260). Each boot writes 1 MB of
# random data, then times: writing 1 MB in 64 KB pieces, reading the file with cat (1 KB reads), with dd (64 KB
# reads), copying it (1 KB reads and writes), ls -lR of /bin and /etc. The checks: the IDE line says the driver took
# the long moves, every timed command ran, the copy's and the original's SHA-256 agree, the zeros read back as zeros,
# the host reads the copy from the disk image with the same SHA-256 (what was written really is on the disk), and the
# host's e2fsck -fn / zenixfs-tool fsck are clean. The times are the emulator's (FS-UAE's IDE has no PIO timing): printed, never
# compared to a limit, and never a claim about a real A1200.
# usage: tools/ide-speed-test.sh [ext2-020|zfs-020|zfs-060 ...]   (default: all three)
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
OUT=${ZENIX_OUT:-${TMPDIR:-/tmp}/zenix-ide-speed}; mkdir -p "$OUT"
SER=${ZENIX_SERIAL:-/tmp/zenix-ide-speed-serial}
RUNS="${*:-ext2-020 zfs-020 zfs-060}"
make -s BUILD=$BD all $BD/zenix.adf $BD/zenix-zfs.adf >/dev/null 2>&1 || { echo "build failed"; exit 1; }
make -s -C zenixfs build/zenixfs-tool >/dev/null 2>&1 || { echo "host tool build failed"; exit 1; }
PROGS="$(make -s BUILD=$BD -f Makefile --eval 'pa:;@echo $(UAPPS) $(SBINAPPS)' pa 2>/dev/null)"
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
clean() { tr -d '\r' | sed 's/\x1b\[[0-9;:]*[mK]//g'; }
for run in $RUNS; do
    case $run in
    ext2-*) cp $BD/zenix.hdf $BD/ide-speed-ext2.hdf || exit 1
            # the run writes three 1 MB files (/r, /z, /r2) at once: a root too full for them shows only as a wrong
            # SHA-256 (2026-11-14: ZINE's test files had taken the room), so the room is checked first, on the host
            fb=$(dumpe2fs -h $BD/ide-speed-ext2.hdf 2>/dev/null | awk -F: '/^Free blocks/ { print $2 + 0 }')
            bs=$(dumpe2fs -h $BD/ide-speed-ext2.hdf 2>/dev/null | awk -F: '/^Block size/ { print $2 + 0 }')
            [ $(( ${fb:-0} * ${bs:-1024} / 1024 )) -ge 3400 ] && check "$run: the root has room for the three 1 MB files ($(( ${fb:-0} * ${bs:-1024} / 1024 )) KB free, 3,400 needed)" ok ||
                { check "$run: the root has room for the three 1 MB files ($(( ${fb:-0} * ${bs:-1024} / 1024 )) KB free, 3,400 needed)" bad; continue; } ;;
    zfs-*)  [ -s $OUT/zfs-fresh.hdf ] || ZFS_BSHIFT=12 tools/mkroot-zenixfs.sh $OUT/zfs-fresh.hdf $BD "$PROGS" 24 >/dev/null ||
                { echo "ZenixFS disk build failed"; exit 1; }
            cp $OUT/zfs-fresh.hdf $BD/ide-speed-zfs.hdf || exit 1 ;;
    *) echo "unknown run $run"; exit 2 ;;
    esac
    ZENIX_SERIAL=$SER tools/run-test.sh fsuae/ide-speed/$run.fs-uae 1500 tools/tests/ide-speed-test.txt 2>&1 | clean > $OUT/$run.log
    # the figures, in the emulator's own seconds
    awk -v run=$run '/^T-[a-z0-9]+$/ { t = substr($0, 3) } /^real\t/ && t { printf "  %s %-8s %s\n", run, t, $2; t = "" }' $OUT/$run.log | tee $OUT/$run.times
    grep -aqE '^ide: A1200/A600 port .*, long reads$' $OUT/$run.log && check "$run: the driver checked and took the long moves (boot line)" ok || check "$run: the driver checked and took the long moves (boot line)" bad
    [ "$(wc -l < $OUT/$run.times)" = 5 ] && check "$run: all five timed commands ran" ok || check "$run: all five timed commands ran" bad
    sr=$(grep -aE '^[0-9a-f]{64}  /r$' $OUT/$run.log | cut -c1-64)
    sr2=$(grep -aE '^[0-9a-f]{64}  /r2$' $OUT/$run.log | cut -c1-64)
    sz=$(grep -aE '^[0-9a-f]{64}  /z$' $OUT/$run.log | cut -c1-64)
    grep -aq 'write failed' $OUT/$run.log && check "$run: no write failed (the disk full?)" bad
    [ -n "$sr" ] && [ "$sr" = "$sr2" ] && check "$run: the copy reads back the same as the original (SHA-256)" ok || check "$run: the copy reads back the same as the original (SHA-256)" bad
    [ "$sz" = "$(head -c 1048576 /dev/zero | sha256sum | cut -c1-64)" ] && check "$run: 1 MB of zeros written in 64 KB pieces reads back right" ok || check "$run: 1 MB of zeros written in 64 KB pieces reads back right" bad
    grep -aqx END-DONE $OUT/$run.log && check "$run: the run ended (sync after rm)" ok || check "$run: the run ended (sync after rm)" bad
    # the host reads the copy off the disk image
    case $run in
    ext2-*)
        start=0                         # zenix.hdf is a bare ext2 volume (no partition table)
        ;;
    zfs-*)
        start=$(python3 - "$BD/ide-speed-zfs.hdf" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
off = 0
while True:
    i = d.find(b'ZNFS', off)
    if i < 0: break
    if i % 512 == 32 and d[i-28:i-26] == b'ZB':
        print(i - 32); break
    off = i + 1
PY
)
        ;;
    esac
    host=
    if [ -n "$start" ]; then
        dd if=$BD/ide-speed-${run%-*}.hdf of=$OUT/$run.img bs=512 skip=$(( start / 512 )) status=none
        case $run in
        ext2-*) host=$(debugfs -R "cat /r2" $OUT/$run.img 2>/dev/null | sha256sum | cut -c1-64)
                e2fsck -fn $OUT/$run.img > $OUT/$run.fsck 2>&1 && check "$run: host e2fsck -fn: clean" ok || { check "$run: host e2fsck -fn: clean" bad; tail -5 $OUT/$run.fsck; } ;;
        zfs-*)  host=$(zenixfs/build/zenixfs-tool cat $OUT/$run.img /r2 2>/dev/null | sha256sum | cut -c1-64)
                zenixfs/build/zenixfs-tool fsck $OUT/$run.img > $OUT/$run.fsck 2>&1 && check "$run: host zenixfs-tool fsck: clean" ok || { check "$run: host zenixfs-tool fsck: clean" bad; tail -5 $OUT/$run.fsck; } ;;
        esac
        rm -f $OUT/$run.img
    fi
    [ -n "$sr" ] && [ "$host" = "$sr" ] && check "$run: the host reads the same copy off the disk image" ok || check "$run: the host reads the same copy off the disk image" bad
    rm -f $BD/ide-speed-${run%-*}.hdf
done
rm -f $OUT/zfs-fresh.hdf
exit $((1 - pass))
