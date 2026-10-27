#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# SLIP in FS-UAE (DESIGN D19 step 5, sections 8.1 and 8.2 "SLIP"): Zenix on the emulated serial port, zenix-slirp on
# the PC's end, as the stock A1200 on its null-modem cable. The serial port is the network's here, so nothing is typed
# into it: the image carries /etc/network/interfaces (auto sl0, static 10.0.2.15), /etc/resolv.conf (10.0.2.3) and an
# /etc/profile that runs /tests/slip.sh once at the autologin. init's `ifup -a` brings sl0 up at boot; the script
# runs ifconfig, route, ping 10.0.2.2, nettest's checks through the PC (TCP echo, 100 KB from a server checked by
# its sum, UDP echo, a name from the PC's test DNS server through 10.0.2.3), netstat, free and ps, then `ifdown sl0`,
# and prints a marker: with netd off the port the console's copy reaches the wire again, and zenix-slirp passes it
# (as every byte that is not a SLIP frame) to its log, where this script waits for it (choice 12 of the host-only
# parts: zenix-slirp's --log is how the console's bytes reach the test). The results are read from the disk image.
#   tools/slip-test.sh [PART...]    s: the stock A1200 (2 MB chip RAM), everything above
#                                   m: 1 MB of chip RAM: SLIP up, ping, free and ps (the memory with the network)
#                                   n: noise and a flood (D19 8.2): zenix-slirp damages every 20th frame to the Amiga
#                                      and sends garbage between frames (--damage); the 100 KB still arrive whole,
#                                      the damaged frames are counted (ifconfig's "bad"), netd never ends; then a SYN
#                                      flood from the PC on four listeners that never accept (nettest slipflood): the
#                                      pool limits hold, the machine stays responsive
# Needs zenix-slirp's libslirp and GLib (pkg-config slirp) and unprivileged user namespaces, as tools/host/net-test.sh.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-slip}; mkdir -p $OUT
SERIAL="${ZENIX_SERIAL:-/tmp/zenix-serial}"
pkg-config --exists slirp || { echo "FAIL libslirp (with GLib) is not installed: pkg-config slirp fails"; exit 1; }
unshare -rn true 2>/dev/null || { echo "FAIL unprivileged user namespaces are off (unshare -rn)"; exit 1; }
OUT=$(cd "$OUT" && pwd)
make -s -C tools/host/zenix-slirp OUT="$OUT" >/dev/null || { echo "FAIL zenix-slirp does not build"; exit 1; }
pass=1
get() { debugfs -R "cat $1" $BD/zenix.hdf 2>/dev/null | tr -d '\r'; }
put() {     # put FILE PATH MODE: a file on the image, root's
    debugfs -w -R "rm $2" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "write $1 $2" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 uid 0" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 gid 0" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 mode 0100$3" $BD/zenix.hdf >/dev/null 2>&1
}

# run CONFIG SECONDS: boot with zenix-slirp on the line; the run ends when the marker reaches the console log
run() {
    local conf=$1 secs=$2 rc=$OUT/resolv.conf host
    rm -f $OUT/console.log $OUT/ready "$SERIAL"
    echo "nameserver 127.0.0.1" > $rc
    # shellcheck disable=SC2086
    unshare -rnm bash -c 'ip link set lo up && echo "0 0" >/proc/sys/net/ipv4/ping_group_range &&
        mount --bind "$0" /etc/resolv.conf && exec python3 -I "$@"' "$rc" "$PWD/tools/slip-host.py" "$OUT/zenix-slirp" \
        "$SERIAL" "$OUT/console.log" "$OUT/ready" $ZS_OPTS > $OUT/host.log 2>&1 &
    host=$!
    for _ in $(seq 100); do [ -e $OUT/ready ] && break; sleep 0.1; done
    [ -e $OUT/ready ] || { echo "FAIL the PC's side did not start: $(cat $OUT/host.log)"; pass=0; kill $host; return 1; }
    ( for _ in $(seq $secs); do grep -aq 'slip-test-done' $OUT/console.log 2>/dev/null && break; sleep 1; done
      sleep 3; touch "$SERIAL.stop" ) &
    local watch=$!
    ZENIX_NOTAP=1 ZENIX_BUILD=$BD tools/run-test.sh $conf $((secs + 30)) > $OUT/run.log 2>&1
    kill $watch $host 2>/dev/null; wait $host 2>/dev/null
    rm -f "$SERIAL.stop"
    grep -aq 'slip-test-done' $OUT/console.log || { echo "FAIL the script's end never reached the console log"; tail -5 $OUT/console.log; pass=0; }
}

image() {   # the test image: the network configured, the script run once at the login
    rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "mkdir /etc/network" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif /etc/network uid 0" $BD/zenix.hdf >/dev/null 2>&1
    printf 'auto sl0\niface sl0 inet static\n    driver slip /dev/ttyS0 115200\n    address 10.0.2.15/24\n    gateway 10.0.2.2\n' > $OUT/interfaces
    printf 'nameserver 10.0.2.3\n' > $OUT/resolv
    printf 'if [ ! -e /tmp/slip-ran ]; then\n  touch /tmp/slip-ran\n  sh /tests/slip.sh > /tmp/slip.out 2>&1\n  echo slip-test-""done\nfi\n' > $OUT/profile
    put $OUT/interfaces /etc/network/interfaces 644
    put $OUT/resolv /etc/resolv.conf 644
    put $OUT/profile /etc/profile 644
    put $OUT/slip.sh /tests/slip.sh 755
}

part_s() {
    cat > $OUT/slip.sh <<'EOS'
sleep 2
ifconfig > /tmp/sl-ifc.txt 2>&1
route > /tmp/sl-route.txt 2>&1
ping -c 3 10.0.2.2 > /tmp/sl-ping.txt 2>&1; echo ping=$? >> /tmp/sl-ping.txt
ping -c 3 -i 0.2 127.0.0.1 > /tmp/sl-pinglo.txt 2>&1
nettest slip > /tmp/sl-nt.txt 2>&1; echo nt=$? >> /tmp/sl-nt.txt
free > /tmp/sl-free.txt; ps > /tmp/sl-ps.txt
ifconfig sl0 > /tmp/sl-ifc2.txt 2>&1
ifdown sl0; echo ifdown=$? > /tmp/sl-down.txt
ifconfig >> /tmp/sl-down.txt 2>&1
dmesg | grep -E 'netd|svc: net|serial' > /tmp/sl-k.txt
sync
EOS
    image
    run fsuae/a1200-020.fs-uae 300
    local f
    for f in ifc route ping pinglo nt free ps ifc2 down k; do get /tmp/sl-$f.txt > $OUT/sl-$f.txt; done
    if grep -aq '^sl0: flags=<UP,POINTOPOINT,RUNNING,DEFAULT>  mtu 1006' $OUT/sl-ifc.txt &&
       grep -aq 'inet 10.0.2.15  netmask 255.255.255.0  gateway 10.0.2.2' $OUT/sl-ifc.txt &&
       grep -aq '^lo: flags=<UP,LOOPBACK,RUNNING>' $OUT/sl-ifc.txt && grep -aq '^nameservers 10.0.2.3' $OUT/sl-ifc.txt; then
        echo "PASS ifconfig: sl0 up at boot (init's ifup -a), 10.0.2.15/24, MTU 1006, the loopback, the name server"
    else echo "FAIL ifconfig: $(cat $OUT/sl-ifc.txt)"; pass=0; fi
    if grep -aqE '^0\.0\.0\.0 +10\.0\.2\.2 +0\.0\.0\.0 +UG +sl0' $OUT/sl-route.txt &&
       grep -aqE '^10\.0\.2\.0 +0\.0\.0\.0 +255\.255\.255\.0 +U +sl0' $OUT/sl-route.txt &&
       grep -aqE '^127\.0\.0\.0 +0\.0\.0\.0 +255\.0\.0\.0 +U +lo' $OUT/sl-route.txt; then
        echo "PASS route: the default through 10.0.2.2, sl0's net, the loopback"
    else echo "FAIL route: $(cat $OUT/sl-route.txt)"; pass=0; fi
    if [ "$(grep -ac '^64 bytes from 10.0.2.2: icmp_seq=[123] time=' $OUT/sl-ping.txt)" = 3 ] &&
       grep -aq '3 packets transmitted, 3 received, 0% packet loss' $OUT/sl-ping.txt && grep -aqx 'ping=0' $OUT/sl-ping.txt; then
        echo "PASS ping 10.0.2.2: three replies through SLIP ($(grep -a '^rtt' $OUT/sl-ping.txt))"
    else echo "FAIL ping: $(cat $OUT/sl-ping.txt)"; pass=0; fi
    local c line
    for c in sliptcp slipget slipudp slipdns; do
        line=$(grep -a "^nettest $c: " $OUT/sl-nt.txt | head -1)
        case "$line" in *": PASS "*) echo "PASS $c ${line#*PASS }" ;; "") echo "FAIL $c: no result"; pass=0 ;; *) echo "FAIL $c ${line#*: }"; pass=0 ;; esac
    done
    grep -aq 'RX frames [1-9]' $OUT/sl-ifc2.txt && echo "PASS sl0's counters: $(grep -a 'RX frames' $OUT/sl-ifc2.txt | tr -s ' ')" || { echo "FAIL sl0's counters: $(cat $OUT/sl-ifc2.txt)"; pass=0; }
    grep -aqx 'ifdown=0' $OUT/sl-down.txt && ! grep -aq '^sl0' $OUT/sl-down.txt && echo "PASS ifdown sl0, and the port is the console's again (the marker reached the log)" ||
        { echo "FAIL ifdown: $(cat $OUT/sl-down.txt)"; pass=0; }
    grep -aq 'netd: sl0 up: SLIP on /dev/ttyS0 at 115200, 10.0.2.15' $OUT/sl-k.txt && echo "PASS the log: sl0 up" || { echo "FAIL the log: $(cat $OUT/sl-k.txt)"; pass=0; }
    # bytes the 1 KB receive ring lost (netd not reading for 89 ms: a busy program at its level, D19 5.3): TCP sends
    # them again; more than 1% of what came in is a fault (an unpaced line lost a fifth)
    local lost rx
    lost=$(grep -aoE 'serial: [0-9]+ bytes lost' $OUT/sl-k.txt | awk '{ s += $2 } END { print s + 0 }')
    rx=$(grep -aoE 'RX frames [0-9]+  bytes [0-9]+' $OUT/sl-ifc2.txt | awk '{ print $5 }')
    if [ -n "$rx" ] && [ $((lost * 100)) -le "$rx" ]; then echo "PASS the receive ring lost $lost bytes of $rx (at most 1%)"
    else echo "FAIL the receive ring lost $lost bytes of ${rx:-?}"; pass=0; fi
    echo "slip-test: memory with SLIP up (2 MB chip RAM): $(grep -a 'chip RAM free' $OUT/sl-free.txt); netd: $(grep -a ' netd$' $OUT/sl-ps.txt | tr -s ' ')"
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1 && echo "PASS the host's e2fsck finds the root clean" || { echo "FAIL e2fsck"; pass=0; }
}

part_m() {
    cat > $OUT/slip.sh <<'EOS'
sleep 2
ping -c 2 10.0.2.2 > /tmp/sl-ping.txt 2>&1; echo ping=$? >> /tmp/sl-ping.txt
free > /tmp/sl-free.txt; ps > /tmp/sl-ps.txt
ifdown sl0
sync
EOS
    image
    run fsuae/matrix/a1200-chip1m.fs-uae 300
    local f
    for f in ping free ps; do get /tmp/sl-$f.txt > $OUT/m-$f.txt; done
    grep -aqx 'ping=0' $OUT/m-ping.txt && echo "PASS 1 MB of chip RAM: ping through SLIP" || { echo "FAIL 1 MB: $(cat $OUT/m-ping.txt)"; pass=0; }
    echo "slip-test: memory with SLIP up (1 MB chip RAM): $(grep -a 'chip RAM free' $OUT/m-free.txt); netd: $(grep -a ' netd$' $OUT/m-ps.txt | tr -s ' ')"
}

part_n() {
    cat > $OUT/slip.sh <<'EOS'
sleep 2
nettest slipget > /tmp/sl-nt.txt 2>&1
ifconfig sl0 > /tmp/sl-ifc.txt 2>&1
nettest slipflood >> /tmp/sl-nt.txt 2>&1; echo nt=$? >> /tmp/sl-nt.txt
ps > /tmp/sl-ps.txt
ifdown sl0
dmesg | grep -E 'netd|svc: net|serial' > /tmp/sl-k.txt
sync
EOS
    image
    ZS_OPTS="--damage 20 -f tcp:7200:7200 -f tcp:7201:7201 -f tcp:7202:7202 -f tcp:7203:7203" run fsuae/a1200-020.fs-uae 400
    local f c line bad
    for f in nt ifc ps k; do get /tmp/sl-$f.txt > $OUT/n-$f.txt; done
    for c in slipget slipflood; do
        line=$(grep -a "^nettest $c: " $OUT/n-nt.txt | head -1)
        case "$line" in *": PASS "*) echo "PASS $c ${line#*PASS }" ;; "") echo "FAIL $c: no result"; pass=0 ;; *) echo "FAIL $c ${line#*: }"; pass=0 ;; esac
    done
    bad=$(grep -aoE 'bad [0-9]+' $OUT/n-ifc.txt | awk '{ print $2 }')
    echo "slip-test: $(grep -a 'noise sent' $OUT/host.log | sed 's/^zenix-slirp: //')"
    if [ -n "$bad" ] && [ "$bad" -gt 0 ]; then echo "PASS the damaged frames are counted: sl0 $(grep -a 'RX frames' $OUT/n-ifc.txt | tr -s ' ')"
    else echo "FAIL no damaged frame counted: $(cat $OUT/n-ifc.txt)"; pass=0; fi
    if ! grep -aqE 'svc: net: .*ended' $OUT/n-k.txt && [ "$(grep -ac 'netd: serving net' $OUT/n-k.txt)" = 1 ]; then
        echo "PASS netd never ended (one start in the log)"
    else echo "FAIL netd ended: $(grep -a 'svc: net\|serving' $OUT/n-k.txt)"; pass=0; fi
    echo "slip-test: netd after the flood: $(grep -a ' netd$' $OUT/n-ps.txt | tr -s ' ')"
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1 && echo "PASS the host's e2fsck finds the root clean" || { echo "FAIL e2fsck"; pass=0; }
}

[ $# = 0 ] && set -- s m n
for p in "$@"; do
    case $p in s|m|n) "part_$p" ;; *) echo "slip-test: unknown part $p"; exit 2 ;; esac
done
exit $((1 - pass))
