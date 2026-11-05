#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The remote control in FS-UAE (DESIGN D24 12.2, docs/man/wasabid.txt): wasabid on Zenix, the PC's checks
# (tools/host/wasabi-remote.py, with wasabi-check.py's frames) through zenix-slirp's forwards on the PC's 127.0.0.1
# (TCP 21236 and UDP 21237 to the Amiga's 1236), all inside a user and network namespace as tools/slip-test.sh runs
# it: nothing leaves the PC. The serial port is the network's here (SLIP, the stock A1200's cable): FS-UAE's own slirp
# (the A2065) takes no port forwards (its slirp_redir is accepted and never opened: checked with FS-UAE 3.2.35), so
# D24 12.2's "else the SLIP path" it is. The image carries the script, run once per boot at the autologin through
# /etc/profile; its results are files read from the image.
#   tools/remote-test.sh [PART...]
#   c  the stock A1200 (D24 12.2 parts c, d and r): wasabid started by hand while netd is missing (it waits out
#      ENETDOWN, then listens once ifup registers the network again); then everything of D24 4 but the screen: INFO,
#      HEALTH, 100 KB both ways, a PUT cut half-way, the refusals, LS against ls -l, RUN's streams and statuses, two
#      slots and a third refused, FREE with and without bit 0 and its "freed" (a command stuck in a hung server's
#      call), a detached run, PS and KILL's refusals, a wrong key and the pause, the fifth connection, the 10 s rule,
#      discovery; netd killed under it (it listens again); the kernel log stream (a line through /dev/kmsg,
#      heartbeats, a wrapped log said); REBOOT's guard while a zup runs, then REBOOT to AmigaOS; after the reboot the
#      previous boot's ending replayed and `dmesg -p` showing the boot note; then the owner's own client (when
#      ZENIX_WASABI names a checkout of Wasabi, 0.11b3 or later: ping, run, put, get, ls, ps, info, health; skipped
#      otherwise)
#   s  the screen (part s, D24 step 3): TEXT and its wait, KEY text and raw keys, INPUTS, SCREEN, WINDOWS, SUM, GRAB
#      against termtest's own CON_LINE picture of the same moment at 3 and 4 planes, a console never used
#   l  the 1 MB machine (part l): 100 KB both ways, the memory with the daemon idle and with a session
#   b  init's daemons (part b, D24 7): /etc/daemons starts wasabid at boot (its lines in the kernel log, /dev/kmsg
#      root's and write only), RESTART starts it again at once (status 75), five more RESTARTs each after 5 s of
#      running are not given up but five quick ends with status 75 are (B2-5), a missing configuration is a stop, a
#      daemon killed five times in a minute is given up
# Needs zenix-slirp's libslirp and GLib (pkg-config slirp) and unprivileged user namespaces, as slip-test.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-remote}; mkdir -p $OUT
SERIAL="${ZENIX_SERIAL:-/tmp/zenix-serial}"
pkg-config --exists slirp || { echo "FAIL libslirp (with GLib) is not installed: pkg-config slirp fails"; exit 1; }
unshare -rn true 2>/dev/null || { echo "FAIL unprivileged user namespaces are off (unshare -rn)"; exit 1; }
OUT=$(cd "$OUT" && pwd)
BD=$(cd "$BD" && pwd)
make -s -C tools/host/zenix-slirp OUT="$OUT" >/dev/null || { echo "FAIL zenix-slirp does not build"; exit 1; }
KEY=$(sed -n 's/^KEY = "\(.*\)"$/\1/p' tools/host/wasabi-check.py)
FWD=21236                                       # inside the namespace: never one of the ports the client probes
pass=1
get() { debugfs -R "cat $1" $BD/zenix.hdf 2>/dev/null | tr -d '\r'; }
put() {     # put FILE PATH MODE: a file on the image, root's
    debugfs -w -R "rm $2" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "write $1 $2" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 uid 0" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 gid 0" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 mode 0100$3" $BD/zenix.hdf >/dev/null 2>&1
}
check() { if [ "$2" = 1 ]; then echo "PASS $1"; else echo "FAIL $1: $3"; pass=0; fi; }

# run CONFIG SECONDS RUN: boot with zenix-slirp on the line and the PC's checks (wasabi-remote.py RUN) beside it, in
# one namespace; the run ends when the script's marker reaches the console log
run() {
    local conf=$1 secs=$2 what=$3 host watch
    rm -f $OUT/console.log $OUT/ready "$SERIAL" $OUT/pc-$what.status
    unshare -rnm bash -c 'ip link set lo up || exit 9
        python3 -I "$1/tools/slip-host.py" "$2/zenix-slirp" "$3" "$2/console.log" "$2/ready" \
            -f tcp:127.0.0.1:'$FWD':1236 -f udp:127.0.0.1:'$((FWD + 1))':1236 > "$2/host.log" 2>&1 &
        h=$!
        trap "kill $h 2>/dev/null; exit 0" TERM
        for i in $(seq 100); do [ -e "$2/ready" ] && break; sleep 0.1; done
        python3 -I "$1/tools/host/wasabi-remote.py" '$FWD' "$4" "$2/pc-$4" "$5" > "$2/pc-$4.log" 2>&1
        echo $? > "$2/pc-$4.status"
        wait $h' sh "$PWD" "$OUT" "$SERIAL" "$what" "${ZENIX_WASABI:-}" &
    host=$!
    for _ in $(seq 100); do [ -e $OUT/ready ] && break; sleep 0.1; done
    [ -e $OUT/ready ] || { echo "FAIL the PC's side did not start: $(cat $OUT/host.log)"; pass=0; kill $host; return 1; }
    ( for _ in $(seq $secs); do grep -aq 'remote-test-done' $OUT/console.log 2>/dev/null && break; sleep 1; done
      sleep 3; touch "$SERIAL.stop" ) &
    watch=$!
    ZENIX_NOTAP=1 ZENIX_BUILD=$BD tools/run-test.sh $conf $((secs + 30)) > $OUT/run-$what.log 2>&1
    kill $watch 2>/dev/null
    for _ in $(seq 50); do [ -e $OUT/pc-$what.status ] && break; sleep 0.2; done
    kill $host 2>/dev/null; wait $host 2>/dev/null
    rm -f "$SERIAL.stop"
    grep -a '^PASS \|^FAIL \|^mem: \|^wasabi-remote: \|^Traceback\|Error' $OUT/pc-$what.log
    [ "$(cat $OUT/pc-$what.status 2>/dev/null)" = 0 ] || { echo "FAIL the PC's checks ($what): $OUT/pc-$what.log"; pass=0; }
    grep -aq 'remote-test-done' $OUT/console.log || { echo "FAIL the script's end never reached the console log"; tail -5 $OUT/console.log; pass=0; }
}

image() {   # the test image: SLIP configured, /etc/wasabid.conf, the script run once a boot at the autologin
    rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "mkdir /etc/network" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif /etc/network uid 0" $BD/zenix.hdf >/dev/null 2>&1
    printf 'auto sl0\niface sl0 inet static\n    driver slip /dev/ttyS0 115200\n    address 10.0.2.15/24\n    gateway 10.0.2.2\n' > $OUT/interfaces
    printf '# the test key (tools/host/wasabi-check.py)\nkey %s\n' "$KEY" > $OUT/wasabid.conf
    printf 'sh /tests/remote.sh >> /tmp/remote.out 2>&1\necho remote-test-""done\n' > $OUT/profile
    put $OUT/interfaces /etc/network/interfaces 644
    put $OUT/wasabid.conf /etc/wasabid.conf 600
    put $OUT/profile /etc/profile 644
    put $OUT/remote.sh /tests/remote.sh 755
}

part_c() {
    cat > $OUT/remote.sh <<'EOS'
if [ ! -e /var/r-boot1 ]; then
    touch /var/r-boot1
    /sbin/wasabid 2> /tmp/wd1.log &
    trap '' INT     # until netd owns the line, the PC's SLIP frames reach this shell: a 0x03 in them was a ^C (2026-11-05)
    sleep 5
    ps > /tmp/r-ps-idle.txt; free > /tmp/r-free-idle.txt
    mv /sbin/netd.off /sbin/netd
    ifup -a > /tmp/r-ifup.txt 2>&1
else
    /sbin/wasabid 2> /tmp/wd2.log &
fi
i=0; while [ ! -e /tmp/pc-done ] && [ $i -lt 1200 ]; do sleep 1; i=$((i+1)); done
dmesg > /tmp/r-k.txt
ifdown sl0
sync
EOS
    image
    put $BD/netd.zx /sbin/netd.off 755        # no network service at boot: the daemon starts on a down network
    debugfs -w -R "rm /sbin/netd" $BD/zenix.hdf >/dev/null 2>&1
    run fsuae/a1200-020.fs-uae 1500 c
    local f
    for f in ps-idle free-idle ifup k; do get /tmp/r-$f.txt > $OUT/r-$f.txt; done
    get /tmp/wd2.log > $OUT/wd2.log
    echo "remote-test: the stock A1200, wasabid idle, the network down (emulator figure): $(grep -a 'chip RAM free' $OUT/r-free-idle.txt); $(grep -a ' wasabid$' $OUT/r-ps-idle.txt | tr -s ' ')"
    local ok=0
    grep -aq 'svc: net: started /sbin/netd' $OUT/r-k.txt && grep -aq 'wasabid: started: port 1236, name zenix, started by hand' $OUT/wd2.log && ok=1
    check "the second boot: the network at boot, the daemon by hand again" $ok "$(cat $OUT/wd2.log)"
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck-c.log 2>&1 && echo "PASS the host's e2fsck finds the root clean (c)" || { echo "FAIL e2fsck (c)"; pass=0; }
}

part_s() {
    cat > $OUT/remote.sh <<'EOS'
/sbin/wasabid 2> /tmp/wd.log &
i=0; while [ ! -e /tmp/pc-done ] && [ $i -lt 900 ]; do sleep 1; i=$((i+1)); done
ifdown sl0
sync
EOS
    image
    run fsuae/a1200-020.fs-uae 900 s
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck-s.log 2>&1 && echo "PASS the host's e2fsck finds the root clean (s)" || { echo "FAIL e2fsck (s)"; pass=0; }
}

part_l() {
    cat > $OUT/remote.sh <<'EOS'
i=0; while ! dmesg | grep -q 'no card for /sbin/net/plipbox' && [ $i -lt 30 ]; do sleep 1; i=$((i+1)); done    # the card probe's drivers gone (D19 6.3)
free > /tmp/l-free0.txt
/sbin/wasabid 2> /tmp/wd.log &
sleep 8
free > /tmp/l-free1.txt; ps > /tmp/l-ps1.txt
i=0; while [ ! -e /tmp/pc-done ] && [ $i -lt 900 ]; do sleep 1; i=$((i+1)); done
ifdown sl0
sync
EOS
    image
    run fsuae/matrix/a1200-chip1m.fs-uae 900 l
    local f
    for f in free0 free1 ps1; do get /tmp/l-$f.txt > $OUT/l-$f.txt; done
    echo "remote-test: the 1 MB machine, SLIP up (emulator figures): before wasabid $(grep -a 'chip RAM free' $OUT/l-free0.txt | sed 's/chip RAM free: //'), with it idle $(grep -a 'chip RAM free' $OUT/l-free1.txt | sed 's/chip RAM free: //'); $(grep -a ' wasabid$' $OUT/l-ps1.txt | tr -s ' ')"
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck-l.log 2>&1 && echo "PASS the host's e2fsck finds the root clean (l)" || { echo "FAIL e2fsck (l)"; pass=0; }
}

part_b() {
    cat > $OUT/remote.sh <<'EOS'
i=0; while [ ! -e /tmp/pc-done ] && [ $i -lt 900 ]; do sleep 1; i=$((i+1)); done
mv /etc/wasabid.conf /etc/wasabid.conf.off
pkill -KILL -x wasabid
sleep 5
pgrep -x wasabid > /tmp/b-after.txt; echo st=$? >> /tmp/b-after.txt
for i in 1 2 3 4 5; do pkill -KILL -x dsleep; sleep 5; done
pgrep -x dsleep > /tmp/b-ds.txt; echo st=$? >> /tmp/b-ds.txt
dmesg > /tmp/b-k.txt
ifdown sl0
sync
EOS
    image
    printf '# the test daemons\nwasabid 0 /sbin/wasabid --init\n  dsleep\t5  /tests/dsleep 600\nq75 0 /bin/sh /tests/q75.sh\n' > $OUT/daemons
    put $OUT/daemons /etc/daemons 644
    put $BD/sleep.zx /tests/dsleep 755
    printf 'exit 75\n' > $OUT/q75.sh
    put $OUT/q75.sh /tests/q75.sh 755
    run fsuae/a1200-020.fs-uae 900 b
    local f k=$OUT/b-k.txt ok
    for f in after ds k; do get /tmp/b-$f.txt > $OUT/b-$f.txt; done
    ok=0; grep -aqE 'init: wasabid: /sbin/wasabid \(pid [0-9]+\) ended, status 137$' $k &&
        grep -aq 'wasabid: /etc/wasabid.conf' $k && grep -aqE 'init: wasabid: /sbin/wasabid \(pid [0-9]+\) stopped \(status 0\): not started again until the next boot' $k &&
        grep -aqx 'st=1' $OUT/b-after.txt && ok=1
    check "a missing configuration: the daemon stops (status 0), init does not start it again" $ok "$(grep -a 'wasabid' $k | tail -6; cat $OUT/b-after.txt)"
    ok=0; [ "$(grep -acE 'init: dsleep: /tests/dsleep \(pid [0-9]+\) ended, status 137$' $k)" = 5 ] &&
        grep -aq 'init: dsleep: given up after 5 ends in 60 s' $k && grep -aqx 'st=1' $OUT/b-ds.txt &&
        [ "$(grep -ac 'init: dsleep: started /tests/dsleep 600' $k)" = 5 ] && ok=1
    check "a daemon killed five times in a minute: started again after each (0, 1, 2, 4 s), then given up" $ok "$(grep -a 'dsleep' $k | tail -12; cat $OUT/b-ds.txt)"
    ok=0; [ "$(grep -acE 'init: q75: /bin/sh \(pid [0-9]+\) ended, status 75 \(start me again\)$' $k)" = 5 ] &&
        grep -aq 'init: q75: given up after 5 ends in 60 s' $k && ok=1
    check "a daemon ending with status 75 at once, five times: still given up (B2-5)" $ok "$(grep -a 'q75' $k | tail -8)"
    ok=0; grep -aq 'init: wasabid: given up' $k || ok=1
    check "wasabid RESTARTed six times in a minute after 5 s each: never given up (B2-5)" $ok "$(grep -a 'init: wasabid' $k | tail -8)"
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck-b.log 2>&1 && echo "PASS the host's e2fsck finds the root clean (b)" || { echo "FAIL e2fsck (b)"; pass=0; }
}

[ $# = 0 ] && set -- c s l b
for p in "$@"; do
    case $p in c|s|l|b) "part_$p" ;; *) echo "remote-test: unknown part $p"; exit 2 ;; esac
done
exit $((1 - pass))
