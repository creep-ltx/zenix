#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The kernel's socket layer in FS-UAE (DESIGN D19 step 3, kernel/sock.c): socktestd(8) registered as the "net"
# service in netd's place, socktest(1)'s checks against it (docs/man/socktest.txt: data, fstat's S_IFSOCK, the clamps,
# pointer checks, blocking and no spinning, poll without a message, SA_RESTART and timeouts, partial sends and their
# order, O_NONBLOCK, the connect rule, EOF and SIGPIPE, the server's death waking a blocked child with -ENETDOWN, the
# close that starts nothing, the next socket starting it again), then ^C on a blocked recv. Then audit O9: socktestd
# made to run on without taking requests (as S22's loop): a socket call queued on it ends at once at kill -9 and at ^C;
# the watchdog ends the stuck server (-ENETDOWN, a new epoch), also one stuck inside a request, whose killed caller
# ends with it. Then the kernel log.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-sock}; mkdir -p $OUT
cat > $OUT/script.txt <<'EOS'
wait zenix:/# 90
sleep 1
send svctest reg net 0 /bin/socktestd; echo reg-do""ne\r
wait reg-done 30
send socktest basic ptr block poll restart partial connect sigpipe down > /tmp/st.out 2>&1; echo st=$? >> /tmp/st.out; echo checks-do""ne\r
wait checks-done 300
send socktest hang\r
wait (?m)^hang: waiting 30
sleep 2
send \x03
sleep 2
send echo hang-st=$? > /tmp/st-h.txt; socktest stuck > /tmp/st-o9.out 2>&1; echo stuck-do""ne\r
wait stuck-done 60
send socktest _netcall\r
wait (?m)^netcall: calling 30
sleep 2
send \x03
sleep 2
send echo nc-st=$? > /tmp/st-n.txt; socktest watchdog >> /tmp/st-o9.out 2>&1; echo wd-do""ne\r
wait wd-done 240
send dmesg | grep 'svc: net' > /tmp/st-k.txt; dmesg | grep socktestd: >> /tmp/st-k.txt; sync; echo all-do""ne\r
wait all-done 30
stop
EOS
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020-fast.fs-uae 720 $OUT/script.txt > $OUT/run.log 2>&1
rc=$?
get() { debugfs -R "cat $1" $BD/zenix.hdf 2>/dev/null | tr -d '\r'; }
get /tmp/st.out > $OUT/st.out
pass=1
[ $rc = 0 ] || { echo "FAIL the script (run-test.sh exit $rc)"; grep 'serial-tap' $OUT/run.log; pass=0; }
for c in basic fstat data readwrite fionread addrclamp bufclamp countclamp ptr notsock block nospin poll pollnomsg pollout \
         restart eintr timeonorestart rcvtimeo partial partialorder nonblock nonblockmisc connect connectnb connectintr \
         connectrestart backlog eof sigpipe down dead noclosestart again; do
    line=$(grep -a "^socktest $c: " $OUT/st.out | head -1)
    case "$line" in
    *": PASS "*) echo "PASS $c ${line#*PASS }" ;;
    "") echo "FAIL $c: no result"; pass=0 ;;
    *) echo "FAIL $c ${line#*: }"; pass=0 ;;
    esac
done
# audit O9: a call waiting on a stuck server ends at a kill -9 or a ^C; the watchdog ends the stuck server
get /tmp/st-o9.out > $OUT/st-o9.out
for c in stuckkill watchdog watchrestart stuckserve watchagain; do
    line=$(grep -a "^socktest $c: " $OUT/st-o9.out | head -1)
    case "$line" in
    *": PASS "*) echo "PASS $c ${line#*PASS }" ;;
    "") echo "FAIL $c: no result"; pass=0 ;;
    *) echo "FAIL $c ${line#*: }"; pass=0 ;;
    esac
done
grep -aqx 'nc-st=130' <<<"$(get /tmp/st-n.txt)" && echo "PASS ^C ends a socket call queued on the stuck server (status 130)" || { echo "FAIL ^C on a queued socket call: $(get /tmp/st-n.txt)"; pass=0; }
grep -aqx 'st=0' $OUT/st.out && echo "PASS socktest ended with status 0" || { echo "FAIL socktest's status: $(grep -a '^st=' $OUT/st.out)"; pass=0; }
grep -aqx 'hang-st=130' <<<"$(get /tmp/st-h.txt)" && echo "PASS ^C ends a recv that waits (status 130)" || { echo "FAIL ^C on a blocked recv: $(get /tmp/st-h.txt)"; pass=0; }
k=$(get /tmp/st-k.txt)
grep -aqE 'svc: net: socktestd \(pid [0-9]+\) ended, status 139, during request 10 for pid [0-9]+' <<<"$k" && echo "PASS the server died in the middle of the request (the log)" || { echo "FAIL the log of the server's death: $k"; pass=0; }
[ "$(grep -ac 'svc: net: started /bin/socktestd' <<<"$k")" -ge 2 ] && echo "PASS the next socket started it again (the log)" || { echo "FAIL restarts in the log"; pass=0; }
[ "$(grep -acE 'svc: net: socktestd \(pid [0-9]+\) answered nothing in 10 s of its own time while calls waited: ended \(audit O9\)' <<<"$k")" = 2 ] && echo "PASS the watchdog said so twice (the log)" || { echo "FAIL the watchdog's log lines: $k"; pass=0; }
grep -aqE 'socktestd: serving net, epoch 4' <<<"$k" && echo "PASS ... and the server came back in epoch 4" || { echo "FAIL epoch 4: $k"; pass=0; }
grep -aqE 'socktestd: serving net, epoch 2' <<<"$k" && echo "PASS ... in a new epoch (2)" || { echo "FAIL the new epoch: $k"; pass=0; }
e2fsck -fn $BD/zenix.hdf > $OUT/e2fsck.log 2>&1 && echo "PASS the host's e2fsck finds the root clean" || { echo "FAIL e2fsck"; pass=0; }
exit $((1 - pass))
