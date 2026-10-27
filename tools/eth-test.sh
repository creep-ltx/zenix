#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Ethernet in FS-UAE (DESIGN D19 step 8, section 8.2 "Ethernet in the emulator"): the stock A1200 with FS-UAE's A2065
# board (network_card = a2065, FS-UAE's own slirp behind it: no root, no tap), the driver /sbin/net/a2065. The image's
# /etc/network/interfaces says "auto eth0 / iface eth0 inet dhcp / driver a2065" and there is no /etc/resolv.conf, so
# the address, the route and the name server all come from slirp's DHCP server. FS-UAE runs in a user and network
# namespace (unshare -rnm, as tools/slip-test.sh runs zenix-slirp) with the PC's test servers of tools/slip-host.py on
# its 127.0.0.1, which the Amiga reaches as 10.0.2.2, and the test DNS server behind slirp's 10.0.2.3: nothing leaves
# the PC. A script started from the console checks: the lease (ifconfig), the route, ping 10.0.2.2, TCP echo, 100 KB
# checked byte by byte, UDP, a name through DHCP's name server (host), the driver killed (netd starts it again, DHCP
# again, ping works), ifdown (the LANCE stopped, the claim released). Results are files read from the image.
#   tools/eth-test.sh [PART...]      e: all of the above (the only part)
# Needs unprivileged user namespaces (unshare -rn), as slip-test.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-eth}; mkdir -p $OUT
OUT=$(cd "$OUT" && pwd)
BD=$(cd "$BD" && pwd)
unshare -rn true 2>/dev/null || { echo "FAIL unprivileged user namespaces are off (unshare -rn)"; exit 1; }
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

# run CONFIG SECONDS SCRIPT: FS-UAE in a namespace of its own, the PC's test servers on its loopback
run() {
    local rc=$OUT/resolv.conf
    echo "nameserver 127.0.0.1" > $rc
    rm -f $OUT/ready
    ZENIX_BUILD=$BD unshare -rnm bash -c 'ip link set lo up && mount --bind "$0" /etc/resolv.conf || exit 9
        python3 -I tools/slip-host.py - none none "$1/ready" > "$1/host.log" 2>&1 &
        srv=$!
        for i in $(seq 100); do [ -e "$1/ready" ] && break; sleep 0.1; done
        tools/run-test.sh "$2" "$3" "$4"; r=$?
        kill $srv 2>/dev/null; wait $srv 2>/dev/null
        exit $r' "$rc" "$OUT" "$1" "$2" "$3"
}

part_e() {
    rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "mkdir /etc/network" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif /etc/network uid 0" $BD/zenix.hdf >/dev/null 2>&1
    printf 'auto eth0\niface eth0 inet dhcp\n    driver a2065\n    hostname amiga\n' > $OUT/interfaces
    put $OUT/interfaces /etc/network/interfaces 644
    cat > $OUT/eth.sh <<'EOS'
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    ifconfig eth0 2>/dev/null | grep -q 'lease [0-9]' && break
    sleep 1
done
ifconfig > /tmp/e-ifc.txt 2>&1
route > /tmp/e-route.txt 2>&1
ping -c 3 10.0.2.2 > /tmp/e-ping.txt 2>&1; echo st=$? >> /tmp/e-ping.txt
nettest sliptcp slipget slipudp slipdns > /tmp/e-nt.txt 2>&1; echo nt=$? >> /tmp/e-nt.txt
host amiga.zenix.test > /tmp/e-host.txt 2>&1; echo st=$? >> /tmp/e-host.txt
kill -9 $(dmesg | grep 'net: eth0: started' | tail -1 | sed 's/.*(pid \([0-9]*\)).*/\1/')
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    sleep 1
    ifconfig eth0 2>/dev/null | grep -q 'lease [0-9]' && break
done
ping -c 2 10.0.2.2 > /tmp/e-ping2.txt 2>&1; echo st=$? >> /tmp/e-ping2.txt
ifconfig eth0 > /tmp/e-ifc2.txt 2>&1
ps > /tmp/e-ps.txt; free > /tmp/e-free.txt
ifdown eth0; echo st=$? > /tmp/e-down.txt
sleep 2
ifconfig >> /tmp/e-down.txt 2>&1
dmesg > /tmp/e-k.txt
sync
EOS
    put $OUT/eth.sh /tests/eth.sh 755
    cat > $OUT/e.txt <<'EOS'
wait zenix:/# 90
sleep 1
send sh /tests/eth.sh > /tmp/e.out 2>&1; echo eth-do""ne\r
wait eth-done 400
stop
EOS
    sed '/^\[fs-uae\]/a network_card = a2065' fsuae/a1200-020.fs-uae > "$OUT/a1200-a2065.fs-uae"
    run "$OUT/a1200-a2065.fs-uae" 500 "$OUT/e.txt" > $OUT/e.log 2>&1
    local rc=$? f k=$OUT/e-k.txt ok
    [ $rc = 0 ] || { echo "FAIL e: the run (exit $rc)"; tail -3 $OUT/e.log; pass=0; }
    for f in ifc route ping nt host ping2 ifc2 ps free down k; do get /tmp/e-$f.txt > $OUT/e-$f.txt; done
    ok=0; grep -aq '^eth0: flags=<UP,BROADCAST,RUNNING,DEFAULT,DHCP>  mtu 1500' $OUT/e-ifc.txt && grep -aq 'ether 00:80:10:' $OUT/e-ifc.txt &&
        grep -aq 'inet 10.0.2.15  netmask 255.255.255.0  gateway 10.0.2.2' $OUT/e-ifc.txt && grep -aqE 'lease [0-9]+ s left, from 10.0.2.2' $OUT/e-ifc.txt &&
        grep -aq '^nameservers 10.0.2.3' $OUT/e-ifc.txt && ok=1
    check "DHCP over the A2065: eth0 10.0.2.15/24, the gateway and the name server from slirp's DHCP ($(grep -a 'ether\|lease' $OUT/e-ifc.txt | tr -s ' ' | tr '\n' ' '))" $ok "$(cat $OUT/e-ifc.txt; grep -a 'a2065\|eth0\|drv:' $k)"
    ok=0; grep -aqE '^0\.0\.0\.0 +10\.0\.2\.2 +0\.0\.0\.0 +UG +eth0' $OUT/e-route.txt && ok=1
    check "route: the default through 10.0.2.2 on eth0 (DHCP's)" $ok "$(cat $OUT/e-route.txt)"
    ok=0; grep -aq '3 packets transmitted, 3 received, 0% packet loss' $OUT/e-ping.txt && ok=1
    check "ping 10.0.2.2 over Ethernet: $(grep -a '^rtt' $OUT/e-ping.txt)" $ok "$(cat $OUT/e-ping.txt)"
    local c line
    for c in sliptcp slipget slipudp slipdns; do
        line=$(grep -a "^nettest $c: " $OUT/e-nt.txt | head -1)
        case "$line" in *": PASS "*) echo "PASS over Ethernet: $c ${line#*PASS }" ;; "") echo "FAIL $c: no result"; pass=0 ;; *) echo "FAIL $c ${line#*: }"; pass=0 ;; esac
    done
    ok=0; [ "$(tr '\n' '|' < $OUT/e-host.txt)" = "amiga.zenix.test has address 10.1.2.3|st=0|" ] && ok=1
    check "host amiga.zenix.test through DHCP's name server (no /etc/resolv.conf)" $ok "$(cat $OUT/e-host.txt)"
    ok=0; grep -aqE 'net: eth0: /sbin/net/a2065 \(pid [0-9]+\) ended, status 137' $k && grep -aqE 'drv: claim of 0x?e90000 \(pid [0-9]+\) released \(the driver ended\): 2 quiesce steps run' $k &&
        grep -aq '2 received, 0% packet loss' $OUT/e-ping2.txt && grep -aqE 'lease [0-9]+ s left' $OUT/e-ifc2.txt && ok=1
    check "the driver killed: the LANCE stopped (its claim released), netd started it again, DHCP again, ping works" $ok "$(grep -a 'eth0\|drv:\|a2065' $k | tail -12; cat $OUT/e-ping2.txt)"
    ok=0; grep -aqx 'st=0' $OUT/e-down.txt && ! grep -aq '^eth0' $OUT/e-down.txt && grep -aq 'a2065: eth0: the doorbell closed (ifdown, or netd is gone): the LANCE stopped' $k && ok=1
    check "ifdown eth0: the driver stopped its LANCE and ended, the interface is gone" $ok "$(cat $OUT/e-down.txt; grep -a 'a2065' $k | tail -3)"
    echo "eth-test: memory with Ethernet up (2 MB chip RAM): $(grep -a 'chip RAM free' $OUT/e-free.txt); $(grep -a ' netd$\| a2065$' $OUT/e-ps.txt | tr -s ' ' | tr '\n' ';')"
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1 && echo "PASS the host's e2fsck finds the root clean" || { echo "FAIL e2fsck"; pass=0; }
}

[ $# = 0 ] && set -- e
for p in "$@"; do
    case $p in e) "part_$p" ;; *) echo "eth-test: unknown part $p"; exit 2 ;; esac
done
exit $((1 - pass))
