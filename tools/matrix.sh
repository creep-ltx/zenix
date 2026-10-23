#!/usr/bin/env bash
# Boot Zenix in every FS-UAE setup under fsuae/ and fsuae/matrix/, run
# tools/tests/matrix-test.txt over the serial port, and report PASS/FAIL per setup.
# usage: tools/matrix.sh [name...]     (default: all)
cd "$(dirname "$0")/.." || exit 1
OUT=$(mktemp -d /tmp/zenix-matrix.XXXXXX)      # this run only: two runs (two sessions) never share logs
make -s all 2>&1 | grep -v 'RWX permissions' | grep -i 'error\|check-68020-60\|too big\|\*\*\*' ; [ "${PIPESTATUS[0]}" = 0 ] || { echo "build failed"; exit 1; }
CONFS="$*"
# mounts: its own floppy (tools/mounttest.sh); zroot: its own ZenixFS root disk, which this script does not build
# (tools/zroot-test.sh)
[ -z "$CONFS" ] && CONFS="$(ls fsuae/*.fs-uae fsuae/matrix/*.fs-uae | sed 's#.*/##; s#\.fs-uae##' | grep -vx 'mounts\|zroot')"
for n in $CONFS; do
    f=fsuae/$n.fs-uae; [ -f "$f" ] || f=fsuae/matrix/$n.fs-uae
    rm -f build/zenix.hdf build/zenix-rdb.hdf
    make -s build/zenix.hdf build/zenix-rdb.hdf build/zenix-ram.adf >$OUT/build.log 2>&1 || { echo "FAIL $n: build failed:"; grep -v 'RWX permissions' $OUT/build.log | tail -15; exit 1; }
    tools/run-test.sh "$f" 85 tools/tests/matrix-test.txt > $OUT/$n.log 2>&1
    if [ "$n" = a600-68000 ]; then
        grep -q 'needs a 68020' $OUT/$n.log && echo "PASS $n (refuses cleanly)" || echo "FAIL $n"
        continue
    fi
    ok=1
    for want in 'M0 ok' ; do :; done
    grep -q 'display: 640x' $OUT/$n.log || ok=0           # 640x256 on PAL, 640x200 on NTSC
    case $n in *ntsc*) grep -q 'display: 640x200, 1 bitplane, 80x25 text' $OUT/$n.log || ok=0;;
               *) grep -q 'display: 640x256, 1 bitplane, 80x32 text' $OUT/$n.log || ok=0;; esac
    grep -q '^ *4 *15 *85$' $OUT/$n.log || ok=0
    grep -q '^ok$' $OUT/$n.log || ok=0
    grep -q 'hello from pid' $OUT/$n.log || ok=0
    grep -q 'killed' $OUT/$n.log || ok=0
    grep -q 'Zenix 0.2' $OUT/$n.log || ok=0
    grep -q 'Filesystem  1K-blocks' $OUT/$n.log || ok=0
    grep -q 'zenix login: ' $OUT/$n.log || ok=0   # console 2 shows its own login prompt
    if [ "$n" = a1200-chip1m ]; then              # 1 MB chip, no fast RAM: room for one session (man/login.txt), and it says so
        grep -q 'login: cannot start /bin/sh: out of memory' $OUT/$n.log || ok=0
    else
        grep -q '^con2ok$' $OUT/$n.log || ok=0   # ... and a login there gives a shell
    fi
    cpu=$(grep -m1 '^cpu:' $OUT/$n.log)
    [ $ok = 1 ] && echo "PASS $n  ($cpu)" || echo "FAIL $n  ($cpu)  see $OUT/$n.log"
done
