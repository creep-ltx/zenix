#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# `make check` (and `make test`): the gate. Builds every image once, runs the host tests one after another (seconds),
# then the emulator tests (FS-UAE headless) side by side: up to ZENIX_EMU_SLOTS (default 6) at once, each with its own
# serial port, output directory and copy of the build directory (ZENIX_BUILD), so no two share an image. Prints a
# table in the order of the list below and exits non-zero if anything failed.
#   tools/check.sh                 everything
#   tools/check.sh host            the host tests only
#   tools/check.sh NAME...         only these (names as in the table, e.g. matrix users-test)
# A test fails if it exits non-zero or prints a line starting with FAIL (matrix.sh reports per machine that way).
# Not run, on purpose: stage0-68k-test when the Python module unicorn is missing (named as skipped), audio/klog/joystick (weak or
# covered by diag-test). Needs ZENIX_KICKSTART_DIR (tools/fsuae_conf.py) and, for kbd-test, cu-test and diag-test,
# ZENIX_CAGE_RIG. A new emulator test starts only while the machine's 1-minute load is 16 or less; every FS-UAE run
# also takes one of the machine's emulator slots (tools/run-test.sh), shared with other sessions on this PC.
cd "$(dirname "$0")/.." || exit 1
RUN=$(mktemp -d /tmp/zenix-check.XXXXXX)
HOST="docs-check mkzexe-test ext2-host-test disk-host-test zenixfs-test mfm-test emmc-test serial-test mem-test a3000-test stage0-test stage0-68k-test"
EMU="matrix cmds-test cmds2-test users-test jobs-test prio-test kbd-test cu-test diag-test memtest mounttest zenixfstest zroot-test edit-regress disktools mbr-test floppy-test zenixboot-test svc-test pure-test"
case "$1" in
"") LIST="$HOST $EMU" ;;
host) LIST="$HOST" ;;
*) LIST="$*" ;;
esac

load_ok() { awk '{ exit ($1 > 16) }' /proc/loadavg; }
JOBS=${ZENIX_EMU_SLOTS:-6}
# the emulator tests start longest first (minutes, from a serial run), so the long ones do not end up last
LONGEST="diag-test cmds2-test svc-test pure-test matrix zroot-test zenixboot-test users-test mounttest zenixfstest prio-test floppy-test"

echo "check: building every image (log $RUN/build.log)"
if ! make -s all build/zenix-ram.adf build/zenix-mount.adf >"$RUN/build.log" 2>&1 ||
   ! make -s -C zenixfs build/zenixfs-tool >>"$RUN/build.log" 2>&1; then
    grep -v 'RWX permissions' "$RUN/build.log" | tail -20
    echo "check: build FAILED"; exit 1
fi
# a snapshot of it: every emulator test copies this one, so a build made in the tree meanwhile changes no test's images
cp -a build "$RUN/build" || { echo "check: could not copy build/"; exit 1; }

# run_one NAME: run one test, write its table row to $RUN/NAME.row (and its first FAIL lines to $RUN/NAME.fails)
run_one() {
    local t=$1 cmd rc secs nfail npass res detail start log="$RUN/$t.log" env=()
    case "$t" in
    docs-check) cmd=(tools/docs-check.sh) ;;
    zenixfs-test) cmd=(make -s -C zenixfs test) ;;
    stage0-68k-test)
        if ! python3 -c 'import unicorn' 2>/dev/null; then
            printf '%-16s %-8s %6s  %s\n' "$t" SKIPPED - "Python module unicorn missing" >"$RUN/$t.row"; return; fi
        cmd=(python3 tools/stage0-68k-test.py) ;;
    *) cmd=("tools/$t.sh") ;;
    esac
    mkdir -p "$RUN/$t"
    case " $EMU " in *" $t "*)                 # its own copy of the images: a test writes to its disks
        cp -a "$RUN/build" "$RUN/$t.build" || { printf '%-16s %-8s %5ss  %s\n' "$t" FAIL 0 "could not copy the build" >"$RUN/$t.row"; return; }
        env=(ZENIX_BUILD="$RUN/$t.build") ;;
    esac
    start=$(date +%s)
    env "${env[@]}" ZENIX_SERIAL="/tmp/zenix-check-$$-$t" ZENIX_OUT="$RUN/$t" USERS_OUT="$RUN/$t" "${cmd[@]}" >"$log" 2>&1
    rc=$?
    secs=$(( $(date +%s) - start ))
    rm -f "/tmp/zenix-check-$$-$t"
    nfail=$(grep -ac '^FAIL' "$log")
    npass=$(grep -ac '^PASS' "$log")
    if [ $rc = 0 ] && [ "$nfail" = 0 ]; then res=PASS; rm -rf "$RUN/$t.build"; else res=FAIL; fi   # a failed test keeps its images
    if [ "$npass" -gt 0 ]; then detail="$npass checks passed"; else detail="last line: $(tail -n 1 "$log" | cut -c1-48)"; fi
    [ "$nfail" = 0 ] || detail="$nfail FAIL lines, $detail"; [ $rc = 0 ] || detail="exit $rc, $detail"
    printf '%-16s %-8s %5ss  %s\n' "$t" "$res" "$secs" "$detail" >"$RUN/$t.row"
    [ $res = FAIL ] && grep -a '^FAIL' "$log" | head -5 | sed 's/^/    /' >"$RUN/$t.fails"
    echo "check: $t $res (${secs}s)"
}

for t in $LIST; do
    case "$t" in docs-check|zenixfs-test|stage0-68k-test) ;; *) [ -x "tools/$t.sh" ] || { echo "check: no test called $t"; exit 2; } ;; esac
done
T0=$(date +%s)
hostl=""; emul=""
for t in $LIST; do case " $EMU " in *" $t "*) emul="$emul $t" ;; *) hostl="$hostl $t" ;; esac; done
for t in $hostl; do echo "check: $t ..."; run_one "$t"; done
# the emulator tests: a pool of up to $JOBS at once, the long ones first
order=""
for t in $LONGEST; do case " $emul " in *" $t "*) order="$order $t" ;; esac; done
for t in $emul; do case " $order " in *" $t "*) ;; *) order="$order $t" ;; esac; done
running=0
for t in $order; do
    while [ $running -ge "$JOBS" ]; do wait -n; running=$((running - 1)); done
    until load_ok; do echo "check: load above 16, waiting"; sleep 60; done
    echo "check: $t started ($((running + 1)) running)"
    run_one "$t" &
    running=$((running + 1))
    sleep 5                                    # the copies and builds of one test do not all start in the same second
done
wait

echo
rm -rf "$RUN/build"
W=$(( $(date +%s) - T0 ))
echo "check summary ($(date '+%Y-%m-%d %H:%M'), $((W / 60)) min $((W % 60)) s, up to $JOBS emulator tests at once, logs in $RUN)"
printf '%-16s %-8s %6s  %s\n' test result time detail
failed=0
for t in $LIST; do
    if [ -f "$RUN/$t.row" ]; then cat "$RUN/$t.row"; else printf '%-16s %-8s %6s  %s\n' "$t" FAIL - "no result (stopped?)"; fi
    grep -q "^$t  *FAIL" "$RUN/$t.row" 2>/dev/null && { failed=$((failed + 1)); cat "$RUN/$t.fails" 2>/dev/null; }
    [ -f "$RUN/$t.row" ] || failed=$((failed + 1))
done
[ $failed = 0 ] && { echo "check: all passed"; exit 0; }
echo "check: $failed FAILED"; exit 1
