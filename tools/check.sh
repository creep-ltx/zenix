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
# covered by diag-test). Needs ZENIX_KICKSTART_DIR (tools/fsuae_conf.py) and, for kbd-test, cu-test, diag-test and colour-test,
# ZENIX_CAGE_RIG. A new emulator test starts only while the machine's 1-minute load is 16 or less; every FS-UAE run
# also takes one of the machine's emulator slots (tools/run-test.sh), shared with other sessions on this PC.
cd "$(dirname "$0")/.." || exit 1
RUN=$(mktemp -d /tmp/zenix-check.XXXXXX)
HOST="docs-check tap-test usize-test mkzexe-test zup-host-test ext2-host-test disk-host-test zenixfs-test mfm-test emmc-test serial-test term-test mem-test a3000-test stage0-test stage0-68k-test"
EMU="matrix cmds-test cmds2-test users-test jobs-test prio-test kbd-test cu-test diag-test colour-test memtest mounttest zenixfstest zroot-test edit-regress disktools mbr-test floppy-test zenixboot-test svc-test pure-test update-test useradd-test"
case "$1" in
"") LIST="$HOST $EMU" ;;
host) LIST="$HOST" ;;
*) LIST="$*" ;;
esac

load_ok() { awk '{ exit ($1 > 16) }' /proc/loadavg; }
JOBS=${ZENIX_EMU_SLOTS:-6}
# The emulator tests start longest first (from a run on 2026-10-26), so the long ones do not end up last. The two
# longest run as several jobs at once, each with some of the test's parts (the script's arguments; | between jobs);
# the table still shows one line per test (its time: the jobs' times added up).
LONGEST="update-test svc-test matrix diag-test colour-test cmds2-test pure-test prio-test zenixboot-test zroot-test mounttest zenixfstest floppy-test users-test"
declare -A SPLIT
SPLIT[svc-test]="a z b n r d|o zo u l|f s h t dmg"
SPLIT[colour-test]="1 4|3"                     # the 1-plane run first: its edges are the 4-plane run's reference
SPLIT[update-test]="install bad|cold previous|panic hang|powercut slowzup|refused nozup"
m=($(ls fsuae/*.fs-uae fsuae/matrix/*.fs-uae | sed 's#.*/##; s#\.fs-uae##' | grep -vx 'mounts\|zroot'))   # as matrix.sh
m1=""; m2=""; for ((i = 0; i < ${#m[@]}; i++)); do if ((i % 2)); then m2="$m2 ${m[i]}"; else m1="$m1 ${m[i]}"; fi; done
SPLIT[matrix]="${m1# }|${m2# }"

echo "check: building every image (log $RUN/build.log)"
if ! make -s all build/zenix-ram.adf build/zenix-mount.adf >"$RUN/build.log" 2>&1 ||
   ! make -s -C zenixfs build/zenixfs-tool >>"$RUN/build.log" 2>&1; then
    grep -v 'RWX permissions' "$RUN/build.log" | tail -20
    echo "check: build FAILED"; exit 1
fi
# a snapshot of it: every emulator test copies this one, so a build made in the tree meanwhile changes no test's images
cp -a build "$RUN/build" || { echo "check: could not copy build/"; exit 1; }

# run_job JOB TEST [ARG...]: run a test (or some of its parts) as job JOB; its log is $RUN/JOB.log, its result
# "rc secs" in $RUN/JOB.res
run_job() {
    local j=$1 t=$2 cmd rc start env=(); shift 2
    case "$t" in
    docs-check) cmd=(tools/docs-check.sh) ;;
    zenixfs-test) cmd=(make -s -C zenixfs test) ;;
    stage0-68k-test) cmd=(python3 tools/stage0-68k-test.py) ;;
    *) cmd=("tools/$t.sh" "$@") ;;
    esac
    mkdir -p "$RUN/$j"
    case " $EMU " in *" $t "*)                 # its own copy of the images: a test writes to its disks
        cp -a "$RUN/build" "$RUN/$j.build" || { echo "check: could not copy the build" >"$RUN/$j.log"; echo "1 0" >"$RUN/$j.res"; return; }
        env=(ZENIX_BUILD="$RUN/$j.build") ;;
    esac
    start=$(date +%s)
    env "${env[@]}" ZENIX_SERIAL="/tmp/zenix-check-$$-$j" ZENIX_OUT="$RUN/$j" USERS_OUT="$RUN/$j" "${cmd[@]}" >"$RUN/$j.log" 2>&1
    rc=$?
    echo "$rc $(( $(date +%s) - start ))" >"$RUN/$j.res"
    rm -f "/tmp/zenix-check-$$-$j"
    if [ $rc = 0 ] && ! grep -aq '^FAIL' "$RUN/$j.log"; then rm -rf "$RUN/$j.build"; echo "check: $j PASS"   # a failed one keeps its images
    else echo "check: $j FAIL"; fi
}
jobs_of() {    # the job names of a test: NAME, or NAME.1 NAME.2 ... when it is split
    if [ -n "${SPLIT[$1]}" ]; then local n; n=$(echo "${SPLIT[$1]}" | awk -F'|' '{ print NF }'); for ((i = 1; i <= n; i++)); do echo "$1.$i"; done
    else echo "$1"; fi
}
# row TEST: its line in the table, from its jobs' results (a test fails if a job exits non-zero or prints a FAIL line)
row() {
    local t=$1 j rc secs sum=0 res=PASS npass=0 nfail=0 bad=0 detail last=""
    if [ "$t" = stage0-68k-test ] && [ ! -f "$RUN/$t.res" ]; then printf '%-16s %-8s %6s  %s\n' "$t" SKIPPED - "Python module unicorn missing"; return 0; fi
    for j in $(jobs_of "$t"); do
        [ -f "$RUN/$j.res" ] || { printf '%-16s %-8s %6s  %s\n' "$t" FAIL - "no result for $j (stopped?)"; return 1; }
        read -r rc secs <"$RUN/$j.res"; sum=$((sum + secs))
        [ "$rc" = 0 ] || bad=$rc
        npass=$((npass + $(grep -ac '^PASS' "$RUN/$j.log"))); nfail=$((nfail + $(grep -ac '^FAIL' "$RUN/$j.log")))
        last=$(tail -n 1 "$RUN/$j.log" | cut -c1-48)
    done
    { [ "$bad" = 0 ] && [ "$nfail" = 0 ]; } || res=FAIL
    if [ "$npass" -gt 0 ]; then detail="$npass checks passed"; else detail="last line: $last"; fi
    [ "$nfail" = 0 ] || detail="$nfail FAIL lines, $detail"; [ "$bad" = 0 ] || detail="exit $bad, $detail"
    printf '%-16s %-8s %5ss  %s\n' "$t" "$res" "$sum" "$detail"
    [ $res = PASS ] && return 0
    for j in $(jobs_of "$t"); do grep -a '^FAIL' "$RUN/$j.log"; done | head -5 | sed 's/^/    /'
    return 1
}

for t in $LIST; do
    case "$t" in docs-check|zenixfs-test|stage0-68k-test) ;; *) [ -x "tools/$t.sh" ] || { echo "check: no test called $t"; exit 2; } ;; esac
done
T0=$(date +%s)
hostl=""; emul=""
for t in $LIST; do case " $EMU " in *" $t "*) emul="$emul $t" ;; *) hostl="$hostl $t" ;; esac; done
for t in $hostl; do
    [ $t = stage0-68k-test ] && ! python3 -c 'import unicorn' 2>/dev/null && continue
    run_job "$t" "$t"
done
# the emulator tests: a pool of up to $JOBS jobs at once, the long ones first
order=""
for t in $LONGEST; do case " $emul " in *" $t "*) order="$order $t" ;; esac; done
for t in $emul; do case " $order " in *" $t "*) ;; *) order="$order $t" ;; esac; done
running=0
for t in $order; do
    i=0
    for j in $(jobs_of "$t"); do
        i=$((i + 1))
        args=""; [ -n "${SPLIT[$t]}" ] && args=$(echo "${SPLIT[$t]}" | cut -d'|' -f$i)
        while [ $running -ge "$JOBS" ]; do wait -n; running=$((running - 1)); done
        until load_ok; do echo "check: load above 16, waiting"; sleep 60; done
        echo "check: $j started ($((running + 1)) running)"
        # shellcheck disable=SC2086
        run_job "$j" "$t" $args &
        running=$((running + 1))
        sleep 5                                # the copies and builds of the jobs do not all start in the same second
    done
done
wait

echo
rm -rf "$RUN/build"
W=$(( $(date +%s) - T0 ))
echo "check summary ($(date '+%Y-%m-%d %H:%M'), $((W / 60)) min $((W % 60)) s, up to $JOBS emulator jobs at once, logs in $RUN)"
printf '%-16s %-8s %6s  %s\n' test result time detail
failed=0
for t in $LIST; do row "$t" || failed=$((failed + 1)); done
[ $failed = 0 ] && { echo "check: all passed"; exit 0; }
echo "check: $failed FAILED"; exit 1
