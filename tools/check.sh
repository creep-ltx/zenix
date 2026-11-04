#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# `make check` (and `make test`): the gate. Builds every image once, runs the host tests one after another (seconds),
# then the emulator tests (FS-UAE headless) side by side: up to ZENIX_EMU_SLOTS (default 6) at once, each with its own
# serial port, output directory and copy of the build directory (ZENIX_BUILD), so no two share an image. Prints a
# table in the order of the list below and exits non-zero if anything failed.
#   tools/check.sh                 everything, at stock speed (the nightly run, before a merge)
#   tools/check.sh host            the host tests only
#   tools/check.sh NAME...         only these (names as in the table, e.g. matrix users-test), in fast mode
#   tools/check.sh --fast|--stock [host|NAME...]   choose the speed (ZENIX_FAST=1 or 0 in the environment does too)
# Fast mode (docs/README.md "Fast mode"): every FS-UAE runs the same machine, cycle-exact, in warp mode
# (tools/fsuae_conf.py make_fast), its emulated time running ahead of the clock: measured on 2026-11-04, zinstall-test f
# 586 s -> 134 s, sumi-test 78 s -> 7 s, update-test 3856 s -> 1907 s (jobs added up; the table in docs/README.md). The
# tests in STOCK below always run at stock speed, each with its reason.
# Scratch: everything of a run is in its folder /tmp/zenix-check.XXXXXX ($RUN): each job's output folder, its copy of the
# build, its serial port and its TMPDIR (so mktemp and FS-UAE's run folders land there too). A job that passes frees its
# folders at once; a failed one keeps them, of its build copy only the disks and kernel*.elf (with more than 2
# failures the images go at the end too). A run that passes
# leaves only check.log (every job's log) and summary.txt. A run stopped by a signal ends its jobs (each runs in its
# own process group) and frees their images, keeping the logs. A job starts only while /tmp has ZENIX_MIN_TMP_MB
# (default 2048) MB free: else check.sh waits up to 5 minutes, saying why, then marks the job as not started.
# A test fails if it exits non-zero or prints a line starting with FAIL (matrix.sh reports per machine that way), or
# if a `wait` in one of its serial scripts timed out or was cut short by the end of the run (ZENIX_TIMEOUTS,
# tools/serial-tap.py; a `waitopt` may time out).
# Needs for zinstall-test: e2fsck, sfdisk, debugfs, amitools' xdftool if installed, and part n the namespaces as zpkg-emu-test.
# Not run, on purpose: stage0-68k-test when the Python module unicorn is missing (named as skipped), audio/klog/joystick (weak or
# covered by diag-test). Needs ZENIX_KICKSTART_DIR (tools/fsuae_conf.py) and, for kbd-test, cu-test, diag-test, colour-test, gcon-test, dpy-test, zdesk-test, zdesk-draw-test and
# random-test and bbs-test, ZENIX_CAGE_RIG; net-test and slip-test need libslirp (GLib) and unprivileged user namespaces (eth-test
# the namespaces too, and ctop-test's part n; ssh-test also OpenSSH's sshd and ssh-keygen, openssl, and unshare --map-auto; wget-test openssl and the namespaces;
# zine-test's tables part ZENIX_MANUALS (the manuals as text; SKIPPED without it); zpkg-emu-test and remote-test the namespaces and libslirp; remote-test also runs the owner's Wasabi client when
# ZENIX_WASABI names a checkout of it). A new
# emulator test starts only while the machine's 1-minute load is 16 or less; every FS-UAE run also takes one of the
# machine's emulator slots (tools/run-test.sh), shared with other sessions on this PC.
cd "$(dirname "$0")/.." || exit 1
MODE=""
while :; do case "$1" in --fast) MODE=fast; shift ;; --stock) MODE=stock; shift ;; *) break ;; esac; done
RUN=$(mktemp -d /tmp/zenix-check.XXXXXX) || exit 1
HOST="docs-check tap-test usize-test signal-host-test mkzexe-test zup-host-test ext2-host-test disk-host-test fat-host-test ffs-host-test pfs3-host-test arc-host-test zenixfs-test mfm-test emmc-test genet-test ne2000-test plipbox-test serial-test term-test confont-test mem-test a3000-test stage0-test stage0-68k-test net-test random-host-test sock-host-test telnet-test crypto-test clock-host-test zpkg-test tls-test nfs-test wasabi-test libc-host-test libc-size zmalloc-test dos-host-test zine-test zinstall-host-test cache-host-test"
EMU="matrix cmds-test cmds2-test users-test jobs-test signal-test pty-test gcon-test dpy-test zdesk-test zdesk-draw-test prio-test kbd-test con-test cu-test diag-test colour-test memtest mounttest zenixfstest fat-test ffs-test pfs3-test arc-test paths-test zroot-test zroot-card-test edit-regress disktools mbr-test floppy-test zenixboot-test svc-test pure-test update-test useradd-test rootcheck-test random-test sock-test netd-test slip-test drv-test eth-test bbs-test time-test crypto68k-test ssh-test wget-test zpkg-emu-test sysupdate-test remote-test nfs-emu-test libc-test tui-test sumi-test maki-test markdown-test ahiru-test ctop-test zine-emu-test zinstall-test halt-test lineedit-test scroll-test"
DEFMODE=fast
case "$1" in
"") LIST="$HOST $EMU"; DEFMODE=stock ;;          # the full run (the nightly, before a merge): stock speed
host) LIST="$HOST" ;;
*) LIST="$*" ;;
esac
if [ -z "$MODE" ]; then case "${ZENIX_FAST-}" in "") MODE=$DEFMODE ;; 0) MODE=stock ;; *) MODE=fast ;; esac; fi

# STOCK: the tests, or parts of them (TEST:PART, a part being one of the test's arguments as in SPLIT below; a matrix
# part is a machine), that run at stock speed in fast mode too, each with its reason. A job holding one of them runs
# at stock speed as a whole. This is the one list of them (docs/README.md "Fast mode" points here).
declare -A STOCK
STOCK[kbd-test]="the cage rig types with wtype in the PC's time: a held key's repeat, typing during output"
STOCK[cu-test]="the cage rig types in the PC's time; the other end of the serial line is the PC"
STOCK[diag-test]="screenshots by the cage rig at times on the PC's clock"
STOCK[colour-test]="screenshots by the cage rig (shot.py) at times on the PC's clock"
STOCK[scroll-test]="screenshots by the cage rig (shot.py) at times on the PC's clock"
STOCK[gcon-test]="screenshots by the cage rig (shot.py) at times on the PC's clock; the 1 s deadline against the script's pace"
STOCK[dpy-test]="screenshots by the cage rig (shot.py) at times on the PC's clock"
STOCK[zdesk-test]="screenshots by the cage rig (shot.py) at times on the PC's clock; a double click inside half a second"
STOCK[zdesk-draw-test]="screenshots by the cage rig (shot.py) at times on the PC's clock; time seq at the stock speed"
STOCK[random-test]="keys typed by the cage rig seed the pool: their timing in the PC's time is the input"
STOCK[bbs-test]="the BBS simulator on the PC paces its bytes in the PC's time (serial noise, a flood, pause codes)"
STOCK[slip-test]="SLIP over the serial port to zenix-slirp on the PC: noise and a flood in the PC's time, lost bytes counted"
STOCK[time-test]="the clock against the PC's: sntp, the RTC, alarms in seconds of the wall clock"
STOCK[crypto68k-test:fsuae/a1200-020.fs-uae]="measures the crypto's speed on the stock 68020 (the times)"
STOCK[svc-test:f]="the service manager's back-off and retry windows (emulated seconds) against the script's pace (the PC's)"
STOCK[rootcheck-test:h]="cut 4 s of the PC's clock after the last command; in warp mode a write lands first that e2fsck rejects (a deleted inode's dtime 1 with no clock, read as an orphan-list link: fsck.ext2's, fixed 2026-11-04; h passed once in warp mode since)"
STOCK[jobs-test]="lines typed ahead while sleep 12-16 runs: the typing (the PC's time) must fit inside the sleep (the Amiga's)"
STOCK[zinstall-test:m]="the stock A1200 with 4 MB of fast RAM (the machine Zenix is made for): the lowest free memory held to a floor"
STOCK[zinstall-test:z]="the same machine with a ZenixFS root: its memory floor"
STOCK[zinstall-test:b]="2 MB of chip RAM alone (best effort): where memory runs out"
STOCK[netd-test:s]="the stock A1200's small profile: memory and the network on the stock machine"
STOCK[zpkg-emu-test:m]="1 MB of chip RAM: where memory runs out"
STOCK[remote-test:l]="1 MB of chip RAM: where memory runs out"
STOCK[sysupdate-test:m]="1 MB of chip RAM: where memory runs out"
STOCK[update-test:powercut]="the cut: the emulator killed some seconds of the PC's clock after zup starts (in warp mode zup is done by then)"
STOCK[update-test:zfscut]="the same cut on a ZenixFS root"
STOCK[zpkg-emu-test:e]="downloads from a server on the PC: in warp mode zpkg's time-outs (emulated seconds) end before the PC's server answers"
STOCK[zpkg-emu-test:s]="the same over SLIP"
STOCK[sysupdate-test:z]="the same downloads (zpkg -Syu over the A2065)"
STOCK[eth-test:e]="UDP echo and DNS from servers on the PC over the A2065: the Amiga's time-outs run in emulated seconds"
STOCK[wget-test]="HTTPS and sntp against servers on the PC: wget's and sntp's time-outs run in emulated seconds"
STOCK[ssh-test]="ssh against sshd on the PC: the same time-outs"
STOCK[nfs-emu-test]="the NFS server on the PC: RPC time-outs and retries in emulated seconds (soft, hard, the server killed)"
STOCK[remote-test]="Wasabi clients on the PC talk to wasabid (PUT/GET, restarts after seconds of the PC's clock)"
STOCK[matrix:a1200-020]="the stock A1200, chip RAM only: how many logins fit, the display depth by memory"
STOCK[matrix:a1200-chip1m]="1 MB of chip RAM only: where memory runs out (consoles 5 and 6)"
STOCK[matrix:a1200-rdb]="the stock A1200, chip RAM only"
STOCK[matrix:a600-68000]="refuses a 68000 at once: nothing to gain"

load_ok() { awk '{ exit ($1 > 16) }' /proc/loadavg; }
JOBS=${ZENIX_EMU_SLOTS:-6}
MIN_TMP_MB=${ZENIX_MIN_TMP_MB:-2048}
# The emulator tests start longest first (from a run on 2026-10-26), so the long ones do not end up last. The two
# longest run as several jobs at once, each with some of the test's parts (the script's arguments; | between jobs);
# the table still shows one line per test (its time: the jobs' times added up).
LONGEST="zinstall-test sysupdate-test ssh-test wget-test zpkg-emu-test remote-test nfs-emu-test update-test rootcheck-test svc-test matrix crypto68k-test diag-test colour-test bbs-test cmds2-test pure-test prio-test zenixboot-test zroot-test mounttest zenixfstest floppy-test users-test"
declare -A SPLIT
SPLIT[svc-test]="a z zs b n r d|o zo u l|f|s h t dmg c"   # f alone: it stays at stock speed (STOCK)
SPLIT[fat-test]="16|32"                             # the two rounds, FAT16 and FAT32
SPLIT[colour-test]="1 4 2|3 s4 s16 s7"    # the 1-plane run first (its edges are the reference); s*: the setting (D18)
SPLIT[rootcheck-test]="r b|i|h|u"                 # u: the choices when a repair fails (man rescue)
SPLIT[netd-test]="s|l"                              # the stock A1200 (small profile) and 4 MB of fast RAM
SPLIT[bbs-test]="e s|b|b16|z"                       # the Unix mode and SLIP; the BBS modes at 3 and 4 planes
SPLIT[crypto68k-test]="fsuae/a1200-020.fs-uae|fsuae/matrix/a1200-030.fs-uae fsuae/matrix/a1200-040.fs-uae fsuae/matrix/a1200-060.fs-uae"   # the 020 with the times
SPLIT[slip-test]="s|m|n"                            # 2 MB and 1 MB of chip RAM; noise and a flood
SPLIT[ctop-test]="c|n"                              # the boxes and keys; the net box with traffic (the A2065)
SPLIT[eth-test]="e|g p|i m|l"                       # the A2065; genet where there is no GENET; ne2000 without a card (D36); plipbox without a box (D39)
SPLIT[zpkg-emu-test]="e|s|m"                       # D22 8.2: the A2065, SLIP, 1 MB of chip RAM
SPLIT[sysupdate-test]="b|m|z"                         # D22 amendment 2: the owner's way from 2026103000; the card layout
SPLIT[nfs-emu-test]="m|b|w|r|u"                     # D23: reading, hard, soft, killed; the lazy fstab boot; writing; reboot; users
SPLIT[remote-test]="c|s|l|b"                        # D24: the stock A1200 (c, d, r), the screen, 1 MB of chip RAM, init's daemons
SPLIT[zinstall-test]="f r|a|m|z|b|c|n|o h|k"               # D35: from the floppies, refusals; beside AmigaOS; 2+4 MB; ZenixFS there; 2 MB chip alone; the card; the network
SPLIT[update-test]="install bad|cold previous|panic hang|powercut zfscut|refused nozup|zfsroot|zfsreal slowzup"   # the two cuts in one (STOCK)
# the matrix as two jobs: the machines that stay at stock speed (STOCK) in one, the others in the other
m=($(ls fsuae/*.fs-uae fsuae/matrix/*.fs-uae | sed 's#.*/##; s#\.fs-uae##' | grep -vx 'mounts\|zroot'))   # as matrix.sh
m1=""; m2=""; for n in "${m[@]}"; do if [ -n "${STOCK[matrix:$n]}" ]; then m1="$m1 $n"; else m2="$m2 $n"; fi; done
SPLIT[matrix]="${m1# }|${m2# }"

# stock_reason TEST [ARG...]: why this job runs at stock speed (nothing: it may run fast)
stock_reason() {
    local t=$1 a; shift
    [ -n "${STOCK[$t]}" ] && { echo "${STOCK[$t]}"; return; }
    for a in "$@"; do [ -n "${STOCK[$t:$a]}" ] && { echo "$t $a: ${STOCK[$t:$a]}"; return; }; done
}
tmp_free_mb() { df -Pm "$RUN" | awk 'NR == 2 { print $4 }'; }
# stop_jobs: end every running job (each is its own process group, its id in $RUN/JOB.pid): TERM, then KILL
stop_jobs() {
    local f p pids=() i alive
    for f in "$RUN"/*.pid; do
        [ -f "$f" ] || continue
        read -r p <"$f" && [ -n "$p" ] && kill -TERM -- "-$p" 2>/dev/null && pids+=("$p")
    done
    [ ${#pids[@]} = 0 ] && return
    for ((i = 0; i < 20; i++)); do
        alive=0; for p in "${pids[@]}"; do kill -0 -- "-$p" 2>/dev/null && alive=1; done
        [ $alive = 0 ] && return; sleep 0.5
    done
    for p in "${pids[@]}"; do kill -KILL -- "-$p" 2>/dev/null; done
}
# free_images: the big things of the run go (the copies of the build, the jobs' folders and scratch, serial links);
# the logs stay
free_images() {
    find "${RUN:?}" -mindepth 1 -maxdepth 1 \( -type d -o -name '*.ser*' -o -name '*.pid' -o -name '*.timeouts' \) \
        -exec rm -rf {} + 2>/dev/null
}
DONE=0
on_exit() {
    [ $DONE = 1 ] && return
    DONE=1; trap - EXIT INT TERM
    echo; echo "check: stopped before the end: ending the running jobs; the logs stay in $RUN"
    stop_jobs; wait; free_images
}
trap on_exit EXIT
trap 'on_exit; exit 130' INT
trap 'on_exit; exit 143' TERM

echo "check: building every image (log $RUN/build.log)"
if ! make -s all build/zenix-ram.adf build/zenix-mount.adf build/zenix-zfs.adf >"$RUN/build.log" 2>&1 ||
   ! make -s -C zenixfs build/zenixfs-tool >>"$RUN/build.log" 2>&1; then
    grep -v 'RWX permissions' "$RUN/build.log" | tail -20
    echo "check: build FAILED"; DONE=1; free_images; exit 1
fi
# a snapshot of it: every emulator test copies this one, so a build made in the tree meanwhile changes no test's images
cp -a build "$RUN/build" || { echo "check: could not copy build/"; DONE=1; free_images; exit 1; }

# room_ok JOB: /tmp (a RAM disk shared by every session) has MIN_TMP_MB free for the job's copies and disks; a full
# /tmp spoils every test running at once (2026-11-03). Waits up to 5 minutes for it (running jobs end and free
# theirs), saying why; 1 if there is still no room.
room_ok() {
    local free tries=0
    while free=$(tmp_free_mb); [ "${free:-0}" -lt "$MIN_TMP_MB" ]; do
        [ $tries -ge 10 ] && return 1
        echo "check: /tmp has $free MB free, $1 needs $MIN_TMP_MB MB (ZENIX_MIN_TMP_MB): waiting 30 s (at most 5 min)"
        tries=$((tries + 1)); sleep 30
    done
}
# not_started JOB: no room for it after all (room_ok): a FAIL in the table that says why
not_started() {
    echo "check: not started: /tmp had $(tmp_free_mb) MB free after 5 minutes of waiting; a job needs $MIN_TMP_MB MB (ZENIX_MIN_TMP_MB)" >"$RUN/$1.log"
    echo "4 0" >"$RUN/$1.res"; echo "check: $1 NOT STARTED (no room in /tmp)"
}

# run_job JOB TEST [ARG...]: run a test (or some of its parts) as job JOB; its log is $RUN/JOB.log, its result
# "rc secs" in $RUN/JOB.res. Its scratch is in $RUN: JOB/ (ZENIX_OUT), JOB.build (ZENIX_BUILD), JOB.tmp (TMPDIR),
# JOB.ser* (ZENIX_SERIAL and the ports named after it).
run_job() {
    local j=$1 t=$2 cmd rc start pid fast=0 why env=(); shift 2
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
    mkdir -p "$RUN/$j" "$RUN/$j.tmp"
    case " $EMU " in *" $t "*)                 # its own copy of the images: a test writes to its disks
        cp -a "$RUN/build" "$RUN/$j.build" || { echo "check: could not copy the build" >"$RUN/$j.log"; echo "1 0" >"$RUN/$j.res"; return; }
        env=(ZENIX_BUILD="$RUN/$j.build")
        if [ "$MODE" = fast ]; then
            why=$(stock_reason "$t" "$@")
            if [ -z "$why" ]; then fast=1; else echo "check: $j at stock speed: $why"; fi
        fi ;;
    esac
    start=$(date +%s)
    # its own process group (setsid), so that a stopped run ends the job and everything the job started
    setsid env "${env[@]}" ZENIX_FAST=$fast ZENIX_SERIAL="$RUN/$j.ser" ZENIX_OUT="$RUN/$j" USERS_OUT="$RUN/$j" \
        ZENIX_TIMEOUTS="$RUN/$j.timeouts" TMPDIR="$RUN/$j.tmp" "${cmd[@]}" >"$RUN/$j.log" 2>&1 &
    pid=$!; echo "$pid" >"$RUN/$j.pid"
    wait "$pid"; rc=$?
    rm -f "${RUN:?}/${j:?}.pid"
    [ $fast = 1 ] && echo "check: ran in fast mode (ZENIX_FAST=1)" >>"$RUN/$j.log"
    # a script's `wait` that timed out (or was still waiting when the run ended) fails the test, whatever the test
    # script made of its output (tools/serial-tap.py, audit 2026-10-26 F7): its lines join the job's log
    if [ -s "$RUN/$j.timeouts" ]; then cat "$RUN/$j.timeouts" >>"$RUN/$j.log"; [ $rc = 0 ] && rc=3; fi
    echo "$rc $(( $(date +%s) - start ))" >"$RUN/$j.res"
    rm -rf "${RUN:?}/${j:?}.ser"* "${RUN:?}/${j:?}.timeouts"
    if [ $rc = 0 ] && ! grep -aq '^FAIL' "$RUN/$j.log"; then            # a passed job frees its folders at once
        rm -rf "${RUN:?}/${j:?}" "${RUN:?}/${j:?}.build" "${RUN:?}/${j:?}.tmp"; echo "check: $j PASS"
    else                       # a failed one keeps its output and scratch for a look, and of its build copy the disks
        find "$RUN/$j.build" -type f ! -name '*.hdf' ! -name '*.adf' ! -name '*.img' ! -name 'kernel*.elf' -delete 2>/dev/null
        echo "check: $j FAIL"
    fi
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
        last=$(grep -av '^check: ran in fast mode' "$RUN/$j.log" | tail -n 1 | cut -c1-48)
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
    case "$t" in docs-check|zenixfs-test|stage0-68k-test|confont-test|net-test|telnet-test|crypto-test|zpkg-test|zine-test|nfs-test|tls-test|wasabi-test) ;; *) [ -x "tools/$t.sh" ] || { echo "check: no test called $t"; DONE=1; free_images; exit 2; } ;; esac
done
if [ $MODE = fast ]; then echo "check: fast mode: the emulator tests in FS-UAE's warp mode, except the ones in STOCK"
else echo "check: stock mode: every emulator test at stock speed"; fi
T0=$(date +%s)
hostl=""; emul=""
for t in $LIST; do case " $EMU " in *" $t "*) emul="$emul $t" ;; *) hostl="$hostl $t" ;; esac; done
for t in $hostl; do
    [ $t = stage0-68k-test ] && ! python3 -c 'import unicorn' 2>/dev/null && continue
    if room_ok "$t"; then run_job "$t" "$t"; else not_started "$t"; fi
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
        room_ok "$j" || { not_started "$j"; continue; }
        echo "check: $j started ($((running + 1)) running)"
        # shellcheck disable=SC2086
        run_job "$j" "$t" $args &
        running=$((running + 1))
        sleep 5                                # the copies and builds of the jobs do not all start in the same second
    done
done
wait
DONE=1

echo
rm -rf "${RUN:?}/build"
W=$(( $(date +%s) - T0 ))
{
    echo "check summary ($(date '+%Y-%m-%d %H:%M'), $((W / 60)) min $((W % 60)) s, $MODE mode, up to $JOBS emulator jobs at once, logs in $RUN)"
    printf '%-16s %-8s %6s  %s\n' test result time detail
    for t in $LIST; do row "$t" || echo "@failed"; done
} >"$RUN/summary.raw"
failed=$(grep -cx '@failed' "$RUN/summary.raw"); grep -vx '@failed' "$RUN/summary.raw" >"$RUN/summary.txt"; rm -f "${RUN:?}/summary.raw"
cat "$RUN/summary.txt"
# every test passed: the run leaves its summary and one log (the build's and every job's, in the table's order)
if [ "$failed" = 0 ]; then
    { cat "$RUN/build.log"
      for t in $LIST; do for j in $(jobs_of "$t"); do [ -f "$RUN/$j.log" ] && { echo "=== $j"; cat "$RUN/$j.log"; }; done; done
    } >"$RUN/check.log"
    find "${RUN:?}" -mindepth 1 -maxdepth 1 ! -name check.log ! -name summary.txt -exec rm -rf {} +
    echo "check: all passed (left: $RUN/check.log and summary.txt)"; exit 0
fi
# many failed (often the full /tmp itself, 2026-11-01: 45 at once): their images go too, the logs stay
if [ "$failed" -gt 2 ]; then free_images; else rm -rf "${RUN:?}"/*.ser* "${RUN:?}"/*.pid; fi
echo "check: $failed FAILED (logs, and the images of a failed job, in $RUN)"; exit 1
