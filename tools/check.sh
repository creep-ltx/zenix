#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# `make check` (and `make test`): the gate. Builds every image, then runs the strong tests one after another - the host
# tests first (seconds), then the emulator tests (FS-UAE headless, about 1.5 hours in all) - each with a serial port
# and an output directory of its own, prints a table and exits non-zero if anything failed.
#   tools/check.sh                 everything
#   tools/check.sh host            the host tests only
#   tools/check.sh NAME...         only these (names as in the table, e.g. matrix users-test)
# A test fails if it exits non-zero or prints a line starting with FAIL (matrix.sh reports per machine that way).
# Not run, on purpose: stage0-68k-test when the Python module unicorn is missing (named as skipped), audio/klog/joystick (weak or
# covered by diag-test). Needs ZENIX_KICKSTART_DIR (tools/fsuae_conf.py) and, for kbd-test, cu-test and diag-test,
# ZENIX_CAGE_RIG. Stops if the machine's load is above 16 (never two emulator runs at once: one at a time here).
cd "$(dirname "$0")/.." || exit 1
RUN=$(mktemp -d /tmp/zenix-check.XXXXXX)
HOST="docs-check ext2-host-test disk-host-test zenixfs-test mfm-test emmc-test serial-test mem-test a3000-test stage0-test stage0-68k-test"
EMU="matrix cmds-test cmds2-test users-test jobs-test prio-test kbd-test cu-test diag-test memtest mounttest zenixfstest zroot-test edit-regress disktools mbr-test floppy-test zenixboot-test svc-test"
case "$1" in
"") LIST="$HOST $EMU" ;;
host) LIST="$HOST" ;;
*) LIST="$*" ;;
esac

load_ok() { awk '{ exit ($1 > 16) }' /proc/loadavg; }

echo "check: building every image (log $RUN/build.log)"
if ! make -s all build/zenix-ram.adf build/zenix-mount.adf >"$RUN/build.log" 2>&1; then
    grep -v 'RWX permissions' "$RUN/build.log" | tail -20
    echo "check: build FAILED"; exit 1
fi

declare -a rows
failed=0
for t in $LIST; do
    case "$t" in
    docs-check) cmd=(tools/docs-check.sh) ;;
    zenixfs-test) cmd=(make -s -C zenixfs test) ;;
    stage0-68k-test)
        if ! python3 -c 'import unicorn' 2>/dev/null; then rows+=("$(printf '%-16s %-8s %6s  %s' "$t" SKIPPED - "Python module unicorn missing")"); continue; fi
        cmd=(python3 tools/stage0-68k-test.py) ;;
    *) [ -x "tools/$t.sh" ] || { echo "check: no test called $t"; exit 2; }; cmd=("tools/$t.sh") ;;
    esac
    case " $EMU " in *" $t "*)
        until load_ok; do echo "check: load above 16, waiting"; sleep 60; done ;;
    esac
    mkdir -p "$RUN/$t"
    log="$RUN/$t.log"
    echo "check: $t ..."
    start=$(date +%s)
    ZENIX_SERIAL="/tmp/zenix-check-$$-$t" ZENIX_OUT="$RUN/$t" USERS_OUT="$RUN/$t" "${cmd[@]}" >"$log" 2>&1
    rc=$?
    secs=$(( $(date +%s) - start ))
    nfail=$(grep -ac '^FAIL' "$log")
    npass=$(grep -ac '^PASS' "$log")
    if [ $rc = 0 ] && [ "$nfail" = 0 ]; then res=PASS; else res=FAIL; failed=$((failed + 1)); fi
    if [ "$npass" -gt 0 ]; then detail="$npass checks passed"; else detail="last line: $(tail -n 1 "$log" | cut -c1-48)"; fi
    [ "$nfail" = 0 ] || detail="$nfail FAIL lines, $detail"; [ $rc = 0 ] || detail="exit $rc, $detail"
    rows+=("$(printf '%-16s %-8s %5ss  %s' "$t" "$res" "$secs" "$detail")")
    [ $res = FAIL ] && grep -a '^FAIL' "$log" | head -5 | sed 's/^/    /'
done

echo
echo "check summary ($(date '+%Y-%m-%d %H:%M'), logs in $RUN)"
printf '%-16s %-8s %6s  %s\n' test result time detail
for r in "${rows[@]}"; do echo "$r"; done
[ $failed = 0 ] && { echo "check: all passed"; exit 0; }
echo "check: $failed FAILED"; exit 1
