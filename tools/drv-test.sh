#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The network's driver protocol in FS-UAE (DESIGN D19 step 7, section 8.2 "the driver protocol before any card"):
# the stock A1200 with FS-UAE's A2065 board (only so that a claim has a device), and the test driver /sbin/net/fake
# put on the image for this test (it answers ARP and ICMP itself, no wire). The image's /etc/network/interfaces has
# eth0 (fake, with three wrong numbers in its first transfer) and eth1 (fake with DMA memory: the kernel's claim,
# DMA region, quiesce program and cache calls, and their refusals), both brought up at boot by init's ifup -a. A
# script started from the console then checks: ifconfig, ping through both, SYS_drv refused to everyone but netd's
# children (nettest drvperm), a driver killed in the middle of a ping (netd logs its end, starts it again, ping works
# again), the DMA driver killed (its claim released: device stopped, DMA memory freed), five ends in a minute (given
# up; ifup starts it again), and netd killed (every claim released, the drivers see their doorbell close and end;
# ifup -a brings it all back). Results are files read from the image afterwards.
#   tools/drv-test.sh [PART...]      p: all of the above (the only part)
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-drv}; mkdir -p $OUT
OUT=$(cd "$OUT" && pwd)
pass=1
get() { debugfs -R "cat $1" $BD/zenix.hdf 2>/dev/null | tr -d '\r'; }
put() {     # put FILE PATH MODE: a file on the image, root's
    debugfs -w -R "rm $2" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "write $1 $2" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 uid 0" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 gid 0" $BD/zenix.hdf >/dev/null 2>&1
    debugfs -w -R "sif $2 mode 0100$3" $BD/zenix.hdf >/dev/null 2>&1
}
mkd() { debugfs -w -R "mkdir $1" $BD/zenix.hdf >/dev/null 2>&1; debugfs -w -R "sif $1 uid 0" $BD/zenix.hdf >/dev/null 2>&1; }
# the A1200 config with FS-UAE's A2065 (a Zorro II board, 0x202/112 in the hardware table's board list)
a2065_conf() { sed '/^\[fs-uae\]/a network_card = a2065' fsuae/a1200-020.fs-uae > "$OUT/a1200-a2065.fs-uae"; }
check() {   # check NAME TEXT-OK MESSAGE-FAIL
    if [ "$2" = 1 ]; then echo "PASS $1"; else echo "FAIL $1: $3"; pass=0; fi
}

part_p() {
    rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
    mkd /sbin/net; mkd /etc/network
    put $BD/drv-fake.zx /sbin/net/fake 755
    printf 'noprobe\nauto eth0\niface eth0 inet static\n    driver fake bad\n    address 10.9.0.15/24\n\nauto eth1\niface eth1 inet static\n    driver fake dma\n    address 10.8.0.15/24\n' > $OUT/interfaces
    put $OUT/interfaces /etc/network/interfaces 644
    cat > $OUT/drv.sh <<'EOS'
sleep 2
drvpid() { dmesg | grep "net: $1: started" | tail -1 | sed 's/.*(pid \([0-9]*\)).*/\1/'; }
ifconfig > /tmp/d-ifc.txt 2>&1
ping -c 3 10.9.0.1 > /tmp/d-ping0.txt 2>&1; echo st=$? >> /tmp/d-ping0.txt
ping -c 3 10.8.0.1 > /tmp/d-ping1.txt 2>&1; echo st=$? >> /tmp/d-ping1.txt
nettest drvperm > /tmp/d-perm.txt 2>&1
ping -c 20 -i 0.2 10.9.0.1 > /tmp/d-pingk.txt 2>&1 &
sleep 1
kill -9 $(drvpid eth0)
wait
sleep 1
ping -c 2 10.9.0.1 > /tmp/d-ping2.txt 2>&1; echo st=$? >> /tmp/d-ping2.txt
kill -9 $(drvpid eth1)
sleep 2
ping -c 2 10.8.0.1 > /tmp/d-ping3.txt 2>&1; echo st=$? >> /tmp/d-ping3.txt
for i in 1 2 3 4 5; do kill -9 $(drvpid eth0); sleep 5; done
ifconfig eth0 > /tmp/d-gone.txt 2>&1; echo st=$? >> /tmp/d-gone.txt
ifup eth0 > /tmp/d-ifup.txt 2>&1; echo st=$? >> /tmp/d-ifup.txt
ping -c 2 10.9.0.1 > /tmp/d-ping4.txt 2>&1; echo st=$? >> /tmp/d-ping4.txt
kill -9 $(pgrep -x netd)
sleep 3
ifup -a > /tmp/d-ifup2.txt 2>&1; echo st=$? >> /tmp/d-ifup2.txt
sleep 3
ping -c 2 10.8.0.1 > /tmp/d-ping5.txt 2>&1; echo st=$? >> /tmp/d-ping5.txt
ps > /tmp/d-ps.txt; free > /tmp/d-free.txt
dmesg > /tmp/d-k.txt
sync
EOS
    put $OUT/drv.sh /tests/drv.sh 755
    cat > $OUT/p.txt <<'EOS'
wait zenix:/# 90
sleep 1
send sh /tests/drv.sh > /tmp/d.out 2>&1; echo drv-do""ne\r
wait drv-done 400
stop
EOS
    a2065_conf
    ZENIX_BUILD=$BD tools/run-test.sh $OUT/a1200-a2065.fs-uae 500 $OUT/p.txt > $OUT/p.log 2>&1
    local rc=$? f k
    [ $rc = 0 ] || { echo "FAIL p: the script (run-test.sh exit $rc)"; pass=0; }
    for f in ifc ping0 ping1 perm pingk ping2 ping3 gone ifup ping4 ifup2 ping5 ps free k; do get /tmp/d-$f.txt > $OUT/d-$f.txt; done
    get /tmp/d.out > $OUT/d.out
    k=$OUT/d-k.txt
    local ok
    ok=0; grep -aq '^eth0: flags=<UP,BROADCAST,RUNNING>  mtu 1500' $OUT/d-ifc.txt && grep -aq 'ether 02:00:00:00:00:01' $OUT/d-ifc.txt &&
        grep -aq 'inet 10.9.0.15  netmask 255.255.255.0' $OUT/d-ifc.txt && grep -aq '^eth1: flags=<UP,BROADCAST,RUNNING>' $OUT/d-ifc.txt && ok=1
    check "ifconfig: eth0 and eth1 up at boot (init's ifup -a), their MAC and address" $ok "$(cat $OUT/d-ifc.txt)"
    ok=0; grep -aq '3 packets transmitted, 3 received, 0% packet loss' $OUT/d-ping0.txt && grep -aqx 'st=0' $OUT/d-ping0.txt && ok=1
    check "ping 10.9.0.1 through the fake driver (ARP and ICMP answered by it): $(grep -a '^rtt' $OUT/d-ping0.txt)" $ok "$(cat $OUT/d-ping0.txt)"
    ok=0; grep -aq '3 packets transmitted, 3 received, 0% packet loss' $OUT/d-ping1.txt && ok=1
    check "ping 10.8.0.1 through the DMA fake driver (netd's buffers in its DMA memory)" $ok "$(cat $OUT/d-ping1.txt)"
    ok=0; grep -aq '^eth1: kernel checks PASS' $k && ok=1
    check "the kernel's refusals: $(grep -a 'eth1: kernel checks' $k | head -1 | cut -c1-200)" $ok "$(grep -a 'eth1: kernel checks' $k)"
    ok=0; grep -aq 'eth0: the driver gave a buffer that is not its own: 40: ignored' $k && grep -aq 'eth0: the driver gave a wrong length: 9000: ignored' $k &&
        grep -aq 'eth0: the driver gave a tag it does not hold: 77: ignored' $k && ok=1
    check "netd refused the bad driver's wrong buffer number, length and tag, and went on" $ok "$(grep -a 'the driver gave' $k)"
    ok=0; grep -aq '^nettest drvperm: PASS' $OUT/d-perm.txt && ok=1
    check "$(head -1 $OUT/d-perm.txt | cut -c1-220)" $ok "$(cat $OUT/d-perm.txt)"
    ok=0; grep -aqE 'net: eth0: /sbin/net/fake \(pid [0-9]+\) ended, status 137' $k && grep -aqE '[1-9][0-9]* received' $OUT/d-pingk.txt &&
        grep -aq '2 packets transmitted, 2 received, 0% packet loss' $OUT/d-ping2.txt && ok=1
    check "a driver killed in the middle of a ping: its end logged, started again, ping works again ($(grep -a 'packets transmitted' $OUT/d-pingk.txt))" $ok "$(cat $OUT/d-pingk.txt $OUT/d-ping2.txt; grep -a 'net: eth0' $k)"
    ok=0; grep -aqE 'drv: claim of 0x?e90000 \(pid [0-9]+\) released \(the driver ended\): 2 quiesce steps run, its DMA memory freed when the driver is reaped' $k &&
        grep -aq '2 packets transmitted, 2 received, 0% packet loss' $OUT/d-ping3.txt && ok=1
    check "the DMA driver killed: its claim released (the device stopped, the DMA memory kept until netd reaps the driver, audit 2026-10-28 S2), started again, ping works" $ok "$(grep -a 'drv:' $k; cat $OUT/d-ping3.txt)"
    ok=0; grep -aq 'net: eth0: /sbin/net/fake given up after 5 ends in 60 s' $k && grep -aqx 'st=1' $OUT/d-gone.txt &&
        grep -aqx 'st=0' $OUT/d-ifup.txt && grep -aq '2 received, 0% packet loss' $OUT/d-ping4.txt && ok=1
    check "five ends in a minute: the driver given up; ifup eth0 starts it again and ping works" $ok "$(grep -a 'net: eth0' $k | tail -8; cat $OUT/d-gone.txt $OUT/d-ifup.txt $OUT/d-ping4.txt)"
    ok=0; grep -aqE 'drv: claim of 0x?e90000 \(pid [0-9]+\) released \(the network server ended\)' $k &&
        [ "$(grep -ac 'netd is gone: stopping' $k)" -ge 2 ] && grep -aqx 'st=0' $OUT/d-ifup2.txt &&
        grep -aq '2 received, 0% packet loss' $OUT/d-ping5.txt && ok=1
    check "netd killed: every claim released, both drivers saw their doorbell close and ended; ifup -a brought it all back" $ok "$(grep -a 'drv:\|netd is gone\|serving net' $k; cat $OUT/d-ifup2.txt $OUT/d-ping5.txt)"
    echo "drv-test: memory: $(grep -a 'chip RAM free' $OUT/d-free.txt); $(grep -a ' netd$\| fake$' $OUT/d-ps.txt | tr -s ' ' | tr '\n' ';')"
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1 && echo "PASS the host's e2fsck finds the root clean" || { echo "FAIL e2fsck"; pass=0; }
}

[ $# = 0 ] && set -- p
for p in "$@"; do
    case $p in p) "part_$p" ;; *) echo "drv-test: unknown part $p"; exit 2 ;; esac
done
exit $((1 - pass))
