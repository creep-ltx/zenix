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
# Part g (DESIGN D19 step 9): the CM4's Ethernet driver /sbin/net/genet where there is no GENET (the normal case in
# the emulator: no Emu68, so the hardware table names none): it says so and ends without touching anything (no
# claim), netd starts it again and gives it up after 5 ends, the rest of the network (the loopback) keeps working.
#   tools/eth-test.sh [PART...]      e: the A2065 (all of the above), g: genet without a GENET, p i m: the PC Card
#                                    NE2000 driver with an empty slot, an IDE card, memory in the slot's addresses
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

part_g() {
    rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "mkdir /etc/network" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif /etc/network uid 0" $BD/zenix.hdf >/dev/null 2>&1
    printf 'auto eth0\niface eth0 inet dhcp\n    driver genet\n' > $OUT/interfaces-g
    put $OUT/interfaces-g /etc/network/interfaces 644
    cat > $OUT/genet.sh <<'EOS'
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    dmesg | grep -q 'genet given up' && break
    sleep 1
done
ifconfig > /tmp/g-ifc.txt 2>&1; echo st=$? >> /tmp/g-ifc.txt
ping -c 2 127.0.0.1 > /tmp/g-ping.txt 2>&1; echo st=$? >> /tmp/g-ping.txt
dmesg > /tmp/g-k.txt
sync
EOS
    put $OUT/genet.sh /tests/genet.sh 755
    cat > $OUT/g.txt <<'EOS'
wait zenix:/# 90
sleep 1
send sh /tests/genet.sh > /tmp/g.out 2>&1; echo genet-do""ne\r
wait genet-done 120
stop
EOS
    ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020.fs-uae 200 "$OUT/g.txt" > $OUT/g.log 2>&1
    local rc=$? f k=$OUT/g-k.txt ok n
    [ $rc = 0 ] || { echo "FAIL g: the run (exit $rc)"; tail -3 $OUT/g.log; pass=0; }
    for f in ifc ping k; do get /tmp/g-$f.txt > $OUT/g-$f.txt; done
    n=$(grep -ac 'genet: eth0: no GENET in the hardware table (this is not a Pi 4 or CM4 under Emu68)' $k)
    ok=0; [ "$n" = 5 ] && [ "$(grep -acE 'net: eth0: /sbin/net/genet \(pid [0-9]+\) ended, status 1$' $k)" = 5 ] &&
        grep -aq 'net: eth0: /sbin/net/genet given up after 5 ends in 60 s (ifup eth0 starts it again)' $k && ok=1
    check "genet without a GENET: \"no GENET in the hardware table\" and status 1, five times, then given up ($n lines)" $ok "$(grep -a 'genet\|eth0' $k | tail -12)"
    ok=0; ! grep -aq 'drv: pid\|drv: claim\|\[kernel\]' $k && ! grep -aq '^eth0' $OUT/g-ifc.txt && grep -aqx 'st=0' $OUT/g-ifc.txt && ok=1
    check "it touched nothing: no claim, no fault, no eth0" $ok "$(grep -a 'drv:\|kernel' $k | tail -5; cat $OUT/g-ifc.txt)"
    ok=0; grep -aq '2 packets transmitted, 2 received, 0% packet loss' $OUT/g-ping.txt && ok=1
    check "netd still serves: ping 127.0.0.1" $ok "$(cat $OUT/g-ping.txt)"
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck-g.log 2>&1 && echo "PASS the host's e2fsck finds the root clean (g)" || { echo "FAIL e2fsck (g)"; pass=0; }
}

# Parts p, i, m (DESIGN D36): the PC Card NE2000 driver /sbin/net/ne2000. FS-UAE 3.2 emulates no PC Card network
# card (only memory and IDE cards in Gayle's slot), so these are its refusals, each a clean end that netd gives up
# after 5 tries while the rest of the network keeps working: p an empty slot (the slot claimed, then "no card", the
# claim released with no quiesce step, no stray interrupt); i FS-UAE's PC Card IDE disk in the slot (its CIS read
# through the emulated Gayle: function 4, "not a network card", left alone); m 8 MB of Zorro II RAM (0x200000-0x9fffff,
# in the slot's addresses: the kernel refuses the claim and says where the memory is). The chip and the frames are
# tested on the PC against a model (tools/ne2000-test.sh).
part_pc() {     # part_pc NAME CONFIG: the driver's five tries and their kernel log
    local nm=$1 conf=$2
    rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "mkdir /etc/network" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif /etc/network uid 0" $BD/zenix.hdf >/dev/null 2>&1
    printf 'auto eth0\niface eth0 inet dhcp\n    driver ne2000\n' > $OUT/interfaces-$nm
    put $OUT/interfaces-$nm /etc/network/interfaces 644
    cat > $OUT/pc-$nm.sh <<EOS
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
    dmesg | grep -q 'ne2000 given up' && break
    sleep 1
done
ifconfig > /tmp/$nm-ifc.txt 2>&1; echo st=\$? >> /tmp/$nm-ifc.txt
ping -c 2 127.0.0.1 > /tmp/$nm-ping.txt 2>&1; echo st=\$? >> /tmp/$nm-ping.txt
dmesg > /tmp/$nm-k.txt
sync
EOS
    put $OUT/pc-$nm.sh /tests/pc.sh 755
    cat > $OUT/$nm.txt <<EOS
wait zenix:/# 90
sleep 1
send sh /tests/pc.sh > /tmp/pc.out 2>&1; echo pc-do""ne\r
wait pc-done 120
stop
EOS
    ZENIX_BUILD=$BD tools/run-test.sh "$conf" 200 "$OUT/$nm.txt" > $OUT/$nm.log 2>&1
    local rc=$? f
    [ $rc = 0 ] || { echo "FAIL $nm: the run (exit $rc)"; tail -3 $OUT/$nm.log; pass=0; }
    for f in ifc ping k; do get /tmp/$nm-$f.txt > $OUT/$nm-$f.txt; done
}
pc_common() {   # pc_common NAME LINE: LINE five times, status 1 five times, given up; no fault, no eth0, ping works
    local nm=$1 line=$2 k=$OUT/$1-k.txt ok n
    n=$(grep -acF "ne2000: eth0: $line" $k)
    ok=0; [ "$n" = 5 ] && [ "$(grep -acE 'net: eth0: /sbin/net/ne2000 \(pid [0-9]+\) ended, status 1$' $k)" = 5 ] &&
        grep -aq 'net: eth0: /sbin/net/ne2000 given up after 5 ends in 60 s (ifup eth0 starts it again)' $k && ok=1
    check "$nm: \"$line\" and status 1, five times, then given up ($n lines)" $ok "$(grep -a 'ne2000\|eth0\|drv:' $k | tail -12)"
    ok=0; ! grep -aq '\[kernel\]\|stray\|panic' $k && ! grep -aq '^eth0' $OUT/$nm-ifc.txt && grep -aqx 'st=0' $OUT/$nm-ifc.txt &&
        grep -aq '2 packets transmitted, 2 received, 0% packet loss' $OUT/$nm-ping.txt && ok=1
    check "$nm: no fault, no stray interrupt, no eth0; netd still serves (ping 127.0.0.1)" $ok "$(grep -a 'kernel\|stray\|panic' $k | tail -5; cat $OUT/$nm-ifc.txt $OUT/$nm-ping.txt)"
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck-$nm.log 2>&1 && echo "PASS the host's e2fsck finds the root clean ($nm)" || { echo "FAIL e2fsck ($nm)"; pass=0; }
}
part_p() {
    part_pc p fsuae/a1200-020.fs-uae
    pc_common p "no card in the PC Card slot"
    local k=$OUT/p-k.txt ok
    ok=0; [ "$(grep -acE 'drv: pid [0-9]+ \(ne2000\) claims the device at 0x?a00000 \(320 KB\)' $k)" = 5 ] &&
        [ "$(grep -acE 'drv: claim of 0x?a00000 \(pid [0-9]+\) released \(the driver ended\): 0 quiesce steps run' $k)" = 5 ] && ok=1
    check "p: the slot claimed (320 KB at 0xa00000) and released with no quiesce step, each time" $ok "$(grep -a 'drv:' $k | tail -6)"
}
part_i() {
    dd if=/dev/zero of=$OUT/card.hdf bs=1k count=2048 2>/dev/null
    sed "/^\[fs-uae\]/a hard_drive_1 = $OUT/card.hdf\nhard_drive_1_controller = scide" fsuae/a1200-020.fs-uae > "$OUT/a1200-pcide.fs-uae"
    part_pc i "$OUT/a1200-pcide.fs-uae"
    pc_common i "not a network card (function 4): left alone"
    local k=$OUT/i-k.txt ok
    ok=0; [ "$(grep -acE 'ne2000: eth0: card: .*function 4$' $k)" = 5 ] && ok=1
    check "i: FS-UAE's PC Card IDE disk: its CIS read through Gayle ($(grep -a 'ne2000: eth0: card:' $k | head -1 | sed 's/.*card: //'))" $ok "$(grep -a 'ne2000' $k | tail -6)"
}
part_m() {
    sed 's/^fast_memory = 0/fast_memory = 8192/' fsuae/a1200-020.fs-uae > "$OUT/a1200-z2ram8.fs-uae"
    part_pc m "$OUT/a1200-z2ram8.fs-uae"
    pc_common m "memory lies in the PC Card slot's addresses (0x600000-0xa4ffff): a card cannot be used on this machine"
    local k=$OUT/m-k.txt ok
    ok=0; [ "$(grep -acE "drv: memory at 0x?[0-9a-f]+-0x?9fffff is in the PC Card slot's addresses: the slot is not claimed" $k)" = 5 ] &&
        ! grep -aq 'claims the device at 0x\?a00000' $k && ok=1
    check "m: 8 MB of Zorro II RAM: the kernel refuses the slot and says where the memory is, nothing claimed" $ok "$(grep -a 'drv:\|ne2000' $k | tail -6)"
}

[ $# = 0 ] && set -- e g p i m
for p in "$@"; do
    case $p in e|g|p|i|m) "part_$p" ;; *) echo "eth-test: unknown part $p"; exit 2 ;; esac
done
exit $((1 - pass))
