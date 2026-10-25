#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# dmesg shows the kernel log: the boot messages appear a second time, in the output of the command.
# usage: tools/klog-test.sh   (ZENIX_SERIAL defaults to /tmp/zenix-klog-serial so it can run beside other tests)
cd "$(dirname "$0")/.." || exit 1
make -s all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-klog-serial}
OUT=/tmp/zenix-klog.log
tools/run-test.sh fsuae/a1200-020-fast.fs-uae 45 tools/tests/klog-test.txt > $OUT 2>&1
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
[ "$(grep -c '^ide: ' $OUT)" -ge 2 ] && check "dmesg repeats the boot messages (ide line twice)" ok || check "dmesg repeats the boot messages" bad
[ "$(grep -c '^zorro: 0: ' $OUT)" -ge 2 ] && check "dmesg shows the Zorro board line" ok || check "dmesg shows the Zorro board line" bad
grep -qE '^ +[0-9]+ +[0-9]+ +[0-9]+' $OUT && check "dmesg output can be piped" ok || check "dmesg output can be piped" bad
exit $((1 - pass))
