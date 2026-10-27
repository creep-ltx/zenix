#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The crypto library on the m68k in FS-UAE (DESIGN D21 10 step 2): `cryptotest` (the known answers of
# tools/host/crypto-vectors.h) on the stock A1200 (68EC020, 2 MB chip RAM) and on the matrix's 030, 040 and 060
# machines (tools/crypto68k-test.sh [CONFIG...]); on the 020 also `cryptotest time 1` and `cryptotest mul`, printed
# as EMULATOR figures (the machine's own come from the owner's A1200 and 060: docs/a1200-checklist-network.md).
# Checked: every known answer on every CPU, the timing and multiplier runs complete.
cd "$(dirname "$0")/.." || exit 1
BD=${ZENIX_BUILD:-build}
make -s BUILD=$BD all >/dev/null 2>&1 || { echo "FAIL the build"; exit 1; }
OUT=${ZENIX_OUT:-/tmp/zenix-crypto68k}; mkdir -p $OUT
[ $# = 0 ] && set -- fsuae/a1200-020.fs-uae fsuae/matrix/a1200-030.fs-uae fsuae/matrix/a1200-040.fs-uae fsuae/matrix/a1200-060.fs-uae
pass=1
for conf in "$@"; do
    name=$(basename "$conf" .fs-uae)
    extra=""
    [ "$name" = a1200-020 ] && extra='cryptotest time 1; cryptotest mul; '
    cat > $OUT/$name.txt <<EOS
wait zenix:/# 120
sleep 1
send cryptotest; echo ct=\$?; ${extra}echo ==do""ne\r
wait ==done 900
stop
EOS
    ZENIX_BUILD=$BD tools/run-test.sh "$conf" 1000 $OUT/$name.txt 2>&1 | tr -d '\r' > $OUT/$name.log
    if grep -q '^cryptotest: [0-9]* checks, 0 failed: ok$' $OUT/$name.log && grep -q '^ct=0$' $OUT/$name.log; then
        echo "PASS ($name) cryptotest: $(grep -o '^cryptotest: [0-9]* checks' $OUT/$name.log | head -1 | cut -d' ' -f2) known answers"
    else
        echo "FAIL ($name) cryptotest: $(grep -a 'FAIL\|cryptotest:' $OUT/$name.log | head -5)"; pass=0
    fi
    if [ -n "$extra" ]; then
        n=$(grep -c '^time ' $OUT/$name.log); m=$(grep -c '^mul m' $OUT/$name.log)
        if [ "$n" = 7 ] && [ "$m" -ge 10 ]; then echo "PASS ($name) the timing and multiplier runs (EMULATOR figures, not the machine's):"
        else echo "FAIL ($name) the timing runs: $n time lines, $m mul lines"; pass=0; fi
        grep -a '^time \|^mul \|E-clock' $OUT/$name.log | sed 's/^/    /'
    fi
done
exit $((1 - pass))
