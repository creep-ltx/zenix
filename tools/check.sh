#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# `make check` (and `make test`): the gate. Builds every image once, runs the host tests one after another (seconds),
# then the emulator tests (FS-UAE headless) side by side: up to ZENIX_EMU_SLOTS (default 6) at once, each with its own
# serial port, output directory and copy of the build directory (ZENIX_BUILD), so no two share an image. Prints a
# table in the order of the list below and exits non-zero if anything failed.
#   tools/check.sh                 everything
#   tools/check.sh host            the host tests only
#   tools/check.sh NAME...         only these (names as in the table, e.g. matrix users-test)
# A test fails if it exits non-zero or prints a line starting with FAIL (matrix.sh reports per machine that way), or
# if a `wait` in one of its serial scripts timed out or was cut short by the end of the run (ZENIX_TIMEOUTS,
# tools/serial-tap.py; a `waitopt` may time out).
# Not run, on purpose: stage0-68k-test when the Python module unicorn is missing (named as skipped), audio/klog/joystick (weak or
# covered by diag-test). Needs ZENIX_KICKSTART_DIR (tools/fsuae_conf.py) and, for kbd-test, cu-test, diag-test, colour-test and
# random-test and bbs-test, ZENIX_CAGE_RIG; net-test and slip-test need libslirp (GLib) and unprivileged user namespaces (eth-test
# the namespaces too, and ctop-test's part n; ssh-test also OpenSSH's sshd and ssh-keygen, openssl, and unshare --map-auto; wget-test openssl and the namespaces;
# zine-test's tables part ZENIX_MANUALS (the manuals as text; SKIPPED without it); zpkg-emu-test and remote-test the namespaces and libslirp; remote-test also runs the owner's Wasabi client when
# ZENIX_WASABI names a checkout of it). A new
# emulator test starts only while the machine's 1-minute load is 16 or less; every FS-UAE run also takes one of the
# machine's emulator slots (tools/run-test.sh), shared with other sessions on this PC.
cd "$(dirname "$0")/.." || exit 1
RUN=$(mktemp -d /tmp/zenix-check.XXXXXX)
HOST="docs-check tap-test usize-test signal-host-test mkzexe-test zup-host-test ext2-host-test disk-host-test fat-host-test ffs-host-test pfs3-host-test arc-host-test zenixfs-test mfm-test emmc-test genet-test serial-test term-test confont-test mem-test a3000-test stage0-test stage0-68k-test net-test random-host-test sock-host-test telnet-test crypto-test clock-host-test zpkg-test tls-test nfs-test wasabi-test libc-host-test libc-size zmalloc-test dos-host-test zine-test"
EMU="matrix cmds-test cmds2-test users-test jobs-test signal-test prio-test kbd-test con-test cu-test diag-test colour-test memtest mounttest zenixfstest fat-test ffs-test pfs3-test arc-test paths-test zroot-test edit-regress disktools mbr-test floppy-test zenixboot-test svc-test pure-test update-test useradd-test rootcheck-test random-test sock-test netd-test slip-test drv-test eth-test bbs-test time-test crypto68k-test ssh-test wget-test zpkg-emu-test sysupdate-test remote-test nfs-emu-test libc-test tui-test sumi-test maki-test markdown-test ahiru-test ctop-test zine-emu-test"
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
LONGEST="sysupdate-test ssh-test wget-test zpkg-emu-test remote-test nfs-emu-test update-test rootcheck-test svc-test matrix crypto68k-test diag-test colour-test bbs-test cmds2-test pure-test prio-test zenixboot-test zroot-test mounttest zenixfstest floppy-test users-test"
declare -A SPLIT
SPLIT[svc-test]="a z zs b n r d|o zo u l|f s h t dmg c"
SPLIT[fat-test]="16|32"                             # the two rounds, FAT16 and FAT32
SPLIT[colour-test]="1 4 2|3 s4 s16 s7"    # the 1-plane run first (its edges are the reference); s*: the setting (D18)
SPLIT[rootcheck-test]="r b|i h|u"                   # u: the choices when a repair fails (man rescue)
SPLIT[netd-test]="s|l"                              # the stock A1200 (small profile) and 4 MB of fast RAM
SPLIT[bbs-test]="e s|b|b16|z"                       # the Unix mode and SLIP; the BBS modes at 3 and 4 planes
SPLIT[crypto68k-test]="fsuae/a1200-020.fs-uae|fsuae/matrix/a1200-030.fs-uae fsuae/matrix/a1200-040.fs-uae fsuae/matrix/a1200-060.fs-uae"   # the 020 with the times
SPLIT[slip-test]="s|m|n"                            # 2 MB and 1 MB of chip RAM; noise and a flood
SPLIT[ctop-test]="c|n"                              # the boxes and keys; the net box with traffic (the A2065)
SPLIT[eth-test]="e|g"                               # the A2065; genet where there is no GENET
SPLIT[zpkg-emu-test]="e|s|m"                       # D22 8.2: the A2065, SLIP, 1 MB of chip RAM
SPLIT[sysupdate-test]="b|m"                         # D22 amendment 2: the owner's way from 2026103000; the card layout
SPLIT[nfs-emu-test]="m|b|w|r|u"                     # D23: reading, hard, soft, killed; the lazy fstab boot; writing; reboot; users
SPLIT[remote-test]="c|s|l|b"                        # D24: the stock A1200 (c, d, r), the screen, 1 MB of chip RAM, init's daemons
SPLIT[update-test]="install bad|cold previous|panic hang|powercut slowzup|refused nozup|zfsroot"
m=($(ls fsuae/*.fs-uae fsuae/matrix/*.fs-uae | sed 's#.*/##; s#\.fs-uae##' | grep -vx 'mounts\|zroot'))   # as matrix.sh
m1=""; m2=""; for ((i = 0; i < ${#m[@]}; i++)); do if ((i % 2)); then m2="$m2 ${m[i]}"; else m1="$m1 ${m[i]}"; fi; done
SPLIT[matrix]="${m1# }|${m2# }"

echo "check: building every image (log $RUN/build.log)"
if ! make -s all build/zenix-ram.adf build/zenix-mount.adf build/zenix-zfs.adf >"$RUN/build.log" 2>&1 ||
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
    net-test) cmd=(tools/host/net-test.sh) ;;
    telnet-test) cmd=(tools/host/telnet-test.sh) ;;
    crypto-test) cmd=(tools/host/crypto-test.sh) ;;
    zpkg-test) cmd=(tools/host/zpkg-test.sh) ;;
    zine-test) cmd=(tools/host/zine-test.sh) ;;
    tls-test) cmd=(tools/host/tls-test.sh) ;;
    nfs-test) cmd=(tools/host/nfs-test.sh) ;;
    wasabi-test) cmd=(tools/host/wasabi-test.sh core check size) ;;
    zenixfs-test) cmd=(make -s -C zenixfs test) ;;
    stage0-68k-test) cmd=(python3 tools/stage0-68k-test.py) ;;
    confont-test) cmd=(python3 tools/mkconfont.py test) ;;
    *) cmd=("tools/$t.sh" "$@") ;;
    esac
    mkdir -p "$RUN/$j"
    case " $EMU " in *" $t "*)                 # its own copy of the images: a test writes to its disks
        cp -a "$RUN/build" "$RUN/$j.build" || { echo "check: could not copy the build" >"$RUN/$j.log"; echo "1 0" >"$RUN/$j.res"; return; }
        env=(ZENIX_BUILD="$RUN/$j.build") ;;
    esac
    start=$(date +%s)
    rm -f "$RUN/$j.timeouts"
    env "${env[@]}" ZENIX_SERIAL="/tmp/zenix-check-$$-$j" ZENIX_OUT="$RUN/$j" USERS_OUT="$RUN/$j" ZENIX_TIMEOUTS="$RUN/$j.timeouts" \
        "${cmd[@]}" >"$RUN/$j.log" 2>&1
    rc=$?
    # a script's `wait` that timed out (or was still waiting when the run ended) fails the test, whatever the test
    # script made of its output (tools/serial-tap.py, audit 2026-10-26 F7): its lines join the job's log
    if [ -s "$RUN/$j.timeouts" ]; then cat "$RUN/$j.timeouts" >>"$RUN/$j.log"; [ $rc = 0 ] && rc=3; fi
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
    case "$t" in docs-check|zenixfs-test|stage0-68k-test|confont-test|net-test|telnet-test|crypto-test|zpkg-test|zine-test|nfs-test|tls-test|wasabi-test) ;; *) [ -x "tools/$t.sh" ] || { echo "check: no test called $t"; exit 2; } ;; esac
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
# every test passed: its working folders go (2 GB or more a run fill the shared /tmp); the .log/.res files stay
[ $failed = 0 ] && { find "$RUN" -mindepth 1 -maxdepth 1 -type d -exec rm -rf {} +; echo "check: all passed"; exit 0; }
echo "check: $failed FAILED"; exit 1
