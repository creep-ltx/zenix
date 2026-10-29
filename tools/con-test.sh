#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The remote control's console ops in FS-UAE (DESIGN D24 6.1-6.3, step 3), on the stock A1200 (3 planes; `termtest
# depth` switches to 4, 2 and 1 for the same checks): termtest conops (CON_READ's grid and header against what it
# drew, the change counter, short buffers, the errors, a console never used is ENXIO twice, CON_LINE's pens and
# palette, root only), termtest rawkeys (raw key codes through CON_INPUT give tools/kbd-test.sh's bytes for the same
# key positions; modifiers and a repeating key released at the end of a call; refused codes; Amiga+F2 and back),
# termtest text (text through CON_INPUT, raw and cooked, a full queue), and the same text typed on the serial line
# (termtest readline, readraw) giving the very same bytes.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
rm -f $BD/zenix.hdf; make -s BUILD=$BD $BD/zenix.hdf >/dev/null 2>&1
OUT=${ZENIX_OUT:-/tmp/zenix-con}; mkdir -p $OUT
ZENIX_BUILD=$BD tools/run-test.sh fsuae/a1200-020.fs-uae 400 tools/tests/con-test.txt > $OUT/serial.log 2>&1
L=$OUT/serial.log
pass=1
tr -d '\r' < $L | grep -a '^FAIL' && pass=0
for t in conops rawkeys text; do
    n=$(tr -d '\r' < $L | grep -ac "^$t: 0 failed")
    want=1; [ $t = conops ] && want=4
    if [ "$n" = $want ]; then echo "PASS termtest $t ($want run(s), every check passed)"; else echo "FAIL termtest $t: $n of $want runs passed"; pass=0; fi
done
lines=$(tr -d '\r' < $L | grep -a '^line:' | sort | uniq -c | awk '{ print $1 " " $2 }')
raws=$(tr -d '\r' < $L | grep -a '^raw:' | sort | uniq -c | awk '{ print $1 " " $2 }')
if [ "$lines" = "2 line:61580a" ]; then echo "PASS the text path and the serial path give the same cooked line (abc BS BS X CR: aX)"; else echo "FAIL cooked lines: $lines"; pass=0; fi
if [ "$raws" = "2 raw:616203630d" ]; then echo "PASS the text path and the serial path give the same raw bytes (Ctrl-C a byte, CR kept)"; else echo "FAIL raw bytes: $raws"; pass=0; fi
grep -aq 'con-test-done' $L || { echo "FAIL the script did not reach its end"; pass=0; }
exit $((1 - pass))
