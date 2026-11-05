#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# netd over the loopback in FS-UAE (DESIGN D19 step 4, section 8.2 "loopback"): the real network server, /sbin/netd,
# started by the kernel at the first socket call, and libz's BSD calls as a program uses them (nettest(1): TCP and
# UDP echo over 127.0.0.1, one big send, poll and select, O_NONBLOCK and fcntl, names, six connections at once, the
# root-only ports, the assembler against C), netstat(1) showing a listener and a connection, ^C on a blocked accept,
# and netd killed: a blocked recv returns ENETDOWN at once and the next socket starts a new netd (epoch 2).
#   tools/netd-test.sh [PART...]     s: the stock A1200 (2 MB chip RAM, netd's small profile), all of the above
#                                    l: 4 MB of fast RAM (the large profile): TCP, UDP, many connections
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-netd}; mkdir -p $OUT
get() { debugfs -R "cat $1" $BD/zenix.hdf 2>/dev/null | tr -d '\r'; }
# no card probe at boot (D19 6.3): a file without an auto line starts no netd at boot, as before the probe
noprobe_file() {    # IMG
    printf '# the card probe is off in this test\nnoprobe\n' > "$OUT/noprobe-interfaces"
    debugfs -w -f - "$1" >/dev/null 2>&1 <<EOD
mkdir /etc/network
sif /etc/network uid 0
write $OUT/noprobe-interfaces /etc/network/interfaces
sif /etc/network/interfaces uid 0
sif /etc/network/interfaces gid 0
sif /etc/network/interfaces mode 0100644
EOD
}
pass=1
check_lines() {     # FILE CHECK...: each "nettest CHECK: PASS" line
    local f=$1 c line; shift
    for c in "$@"; do
        line=$(grep -a "^nettest $c: " "$f" | head -1)
        case "$line" in
        *": PASS "*) echo "PASS $c ${line#*PASS }" ;;
        "") echo "FAIL $c: no result"; pass=0 ;;
        *) echo "FAIL $c ${line#*: }"; pass=0 ;;
        esac
    done
}

part_s() {
    rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
    noprobe_file $BD/zenix.hdf                  # netd starts at the first socket call here, not at boot
    cat > $OUT/s.txt <<'EOS'
wait zenix:/# 90
sleep 1
send nettest tcp udp poll nonblock names many perm restart partial sigpipe asm > /tmp/nt.out 2>&1; echo nt=$? >> /tmp/nt.out; echo checks-do""ne\r
wait checks-done 400
send nettest listen > /tmp/nl.out &\r
sleep 2
send cat /tmp/nl.out; echo cat-do""ne\r
wait (?m)^listen: ready 60
send netstat -a > /tmp/ns.txt; netstat -tl > /tmp/ns-l.txt; netstat > /tmp/ns-c.txt; echo ns-do""ne\r
wait ns-done 60
send kill %1; nettest down > /tmp/nd.out 2>&1; echo down-do""ne\r
wait down-done 120
send nettest hang\r
wait (?m)^hang: waiting 60
sleep 2
send \x03
sleep 2
send echo hang-st=$? > /tmp/nh.txt; nettest giveup > /tmp/ng.out 2>&1; ifup -a 2> /tmp/ng-ifup.txt; echo ifup=$? >> /tmp/ng-ifup.txt; nettest tcp >> /tmp/ng.out 2>&1; echo give-do""ne\r
wait give-done 240
send dmesg | grep 'svc: net' > /tmp/nk.txt; dmesg | grep netd: >> /tmp/nk.txt; ps > /tmp/nps.txt; free > /tmp/nfree.txt; sync; echo all-do""ne\r
wait all-done 30
stop
EOS
    ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020.fs-uae 800 $OUT/s.txt > $OUT/s.log 2>&1
    local rc=$?
    [ $rc = 0 ] || { echo "FAIL s: the script (run-test.sh exit $rc)"; grep 'serial-tap' $OUT/s.log; pass=0; }
    get /tmp/nt.out > $OUT/nt.out
    check_lines $OUT/nt.out tcp udp poll nonblock names many perm restart partial sigpipe asm
    grep -aqx 'nt=0' $OUT/nt.out && echo "PASS nettest ended with status 0" || { echo "FAIL nettest's status: $(grep -a '^nt=' $OUT/nt.out)"; pass=0; }
    grep -aq '^nettest many: PASS (4 connections at once, the next refused: no buffer space' $OUT/nt.out &&
        echo "PASS the small profile's limit: 8 TCP ends (4 loopback connections), the next socket refused at once" ||
        { echo "FAIL the small profile's limit: $(grep -a '^nettest many' $OUT/nt.out)"; pass=0; }
    get /tmp/nd.out > $OUT/nd.out
    check_lines $OUT/nd.out down
    local ns nsl nsc k
    ns=$(get /tmp/ns.txt); nsl=$(get /tmp/ns-l.txt); nsc=$(get /tmp/ns-c.txt)
    printf '%s\n----\n%s\n----\n%s\n' "$ns" "$nsl" "$nsc" > $OUT/netstat.txt
    if grep -aqE '^tcp +0 +0 0\.0\.0\.0:7010 +0\.0\.0\.0:\* +LISTEN +root$' <<<"$ns" &&
       grep -aqE '^tcp +0 +0 127\.0\.0\.1:7010 +127\.0\.0\.1:[0-9]+ +ESTABLISHED +root$' <<<"$ns" &&
       grep -aqE '^tcp +0 +0 127\.0\.0\.1:[0-9]+ +127\.0\.0\.1:7010 +ESTABLISHED +root$' <<<"$ns" &&
       grep -aqE '^udp +0 +0 127\.0\.0\.1:7011 +0\.0\.0\.0:\* +root$' <<<"$ns"; then
        echo "PASS netstat -a: the listener, both ends of the connection, the UDP socket"
    else echo "FAIL netstat -a: $ns"; pass=0; fi
    if grep -aq ':7010 .*LISTEN' <<<"$nsl" && ! grep -aq 'ESTABLISHED\|^udp' <<<"$nsl" &&
       grep -aq 'ESTABLISHED' <<<"$nsc" && ! grep -aq 'LISTEN\|^udp' <<<"$nsc"; then
        echo "PASS netstat -tl shows only the TCP listener, netstat only the connections"
    else echo "FAIL netstat -tl / netstat: $nsl // $nsc"; pass=0; fi
    grep -aqx 'hang-st=130' <<<"$(get /tmp/nh.txt)" && echo "PASS ^C ends an accept that waits (status 130)" || { echo "FAIL ^C on a blocked accept: $(get /tmp/nh.txt)"; pass=0; }
    k=$(get /tmp/nk.txt)
    echo "$k" > $OUT/dmesg.txt
    grep -aq 'svc: net: started /sbin/netd' <<<"$k" && echo "PASS the first socket call started /sbin/netd (the log)" || { echo "FAIL netd's start: $k"; pass=0; }
    grep -aqE 'netd: serving net, epoch 1, small profile' <<<"$k" && echo "PASS ... with the small profile on 2 MB of chip RAM" || { echo "FAIL the profile: $k"; pass=0; }
    grep -aqE 'svc: net: netd \(pid [0-9]+\) ended, status 137' <<<"$k" && echo "PASS the kill is in the log" || { echo "FAIL the kill's log line: $k"; pass=0; }
    grep -aqE 'netd: serving net, epoch 2' <<<"$k" && echo "PASS the next socket started netd again, epoch 2" || { echo "FAIL the new epoch: $k"; pass=0; }
    # five ends in a minute (D19 4.3 rule 12): given up, sockets get ENETDOWN, root's ifup registers netd again
    get /tmp/ng.out > $OUT/ng.out
    check_lines $OUT/ng.out giveup
    grep -aqE 'svc: net: given up after 5 ends in 60 s' <<<"$k" && echo "PASS the kernel gave the network server up (the log)" || { echo "FAIL no 'given up' in the log"; pass=0; }
    local gi; gi=$(get /tmp/ng-ifup.txt)
    if grep -aq 'the network server was not running: registered /sbin/netd again' <<<"$gi" && grep -aqx 'ifup=0' <<<"$gi" &&
       [ "$(grep -ac '^nettest tcp: PASS' $OUT/ng.out)" = 1 ]; then
        echo "PASS root's ifup -a registered netd again, and the network serves (nettest tcp)"
    else echo "FAIL ifup after giving up: $gi // $(grep -a '^nettest tcp' $OUT/ng.out)"; pass=0; fi
    get /tmp/nps.txt > $OUT/ps.txt; get /tmp/nfree.txt > $OUT/free.txt
    echo "netd-test: netd in ps (2 MB chip RAM): $(grep -a ' netd' $OUT/ps.txt | head -1)"
    e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1 && echo "PASS the host's e2fsck finds the root clean" || { echo "FAIL e2fsck"; pass=0; }
}

part_l() {
    rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
    noprobe_file $BD/zenix.hdf
    cat > $OUT/l.txt <<'EOS'
wait zenix:/# 90
sleep 1
send nettest tcp udp many > /tmp/nt.out 2>&1; echo nt=$? >> /tmp/nt.out; dmesg | grep netd: > /tmp/nk.txt; ps > /tmp/nps.txt; sync; echo checks-do""ne\r
wait checks-done 300
stop
EOS
    ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 400 $OUT/l.txt > $OUT/l.log 2>&1
    local rc=$?
    [ $rc = 0 ] || { echo "FAIL l: the script (run-test.sh exit $rc)"; grep 'serial-tap' $OUT/l.log; pass=0; }
    get /tmp/nt.out > $OUT/nt-l.out
    check_lines $OUT/nt-l.out tcp udp many
    grep -aq '^nettest many: PASS (12 connections at once, each' $OUT/nt-l.out && echo "PASS the large profile: 12 connections, no limit hit" ||
        { echo "FAIL the large profile's connections: $(grep -a '^nettest many' $OUT/nt-l.out)"; pass=0; }
    grep -aqE 'netd: serving net, epoch 1, large profile' <<<"$(get /tmp/nk.txt)" && echo "PASS the large profile with 4 MB of fast RAM" || { echo "FAIL the profile: $(get /tmp/nk.txt)"; pass=0; }
    echo "netd-test: netd in ps (4 MB fast RAM): $(get /tmp/nps.txt | grep -a ' netd' | head -1)"
}

[ $# = 0 ] && set -- s l
for p in "$@"; do
    case $p in s|l) "part_$p" ;; *) echo "netd-test: unknown part $p"; exit 2 ;; esac
done
exit $((1 - pass))
