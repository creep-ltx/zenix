#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Floppy drives in the emulator, checked against the disk images on the host:
#   df0 is the Zenix boot disk (build/zenix.adf), df1 a 880 KB image of random data.
#   1. `fdisk list` shows df0 and, once it has been used (drives past df0 are probed on first use), df1.
#   2. the first 200 sectors of df0 read through the MFM driver have the same SHA-256 as the host's bytes.
#   3. the whole of df1 reads back identical (1760 sectors).
#   4. 100 sectors written to df1 at sector 500 read back identically, and after the emulator has stopped the
#      image file on the host holds exactly those bytes there.
# usage: tools/floppy-test.sh   (private serial port; waits for other FS-UAE runs)
cd "$(dirname "$0")/.." || exit 1
make -s all >/dev/null 2>&1 || { echo "build failed"; exit 1; }
export ZENIX_SERIAL=${ZENIX_SERIAL:-/tmp/zenix-floppy-serial}
OUT=/tmp/zenix-floppy; mkdir -p $OUT
python3 - <<PY
import os
open("$OUT/df1.adf", "wb").write(os.urandom(901120))
PY
cp build/zenix.adf $OUT/df0.adf
cp $OUT/df1.adf $OUT/df1.orig
sed -e "s#floppy_drive_0 = .*#floppy_drive_0 = $OUT/df0.adf\nfloppy_drive_1 = $OUT/df1.adf\nfloppy_drive_count = 2\nwritable_floppy_images = 1#" fsuae/a1200-020-fast.fs-uae > fsuae/floppy-test.fs-uae
while pgrep -x fs-uae >/dev/null; do sleep 1; done
tools/run-test.sh fsuae/floppy-test.fs-uae 190 tools/tests/floppy-test.txt > $OUT/log 2>&1
rm -f fsuae/floppy-test.fs-uae
pass=1
check() { if [ "$2" = ok ]; then echo "PASS $1"; else echo "FAIL $1"; pass=0; fi; }
grep -qE '^df0 +' $OUT/log && grep -qE '^df1 +1760' $OUT/log && check "fdisk list shows df0 and df1" ok || check "fdisk list shows df0 and df1" bad
want0=$(head -c $((200*512)) build/zenix.adf | sha256sum | cut -d' ' -f1)
grep -q "$want0" $OUT/log && check "df0: first 200 sectors identical to the host's" ok || check "df0: first 200 sectors identical to the host's" bad
want1=$(sha256sum < $OUT/df1.orig | cut -d' ' -f1)
grep -q "$want1" $OUT/log && check "df1: all 1760 sectors read back identical" ok || check "df1: all 1760 sectors read back identical" bad
wantc=$(head -c $((100*512)) build/zenix.adf | sha256sum | cut -d' ' -f1)
[ "$(grep -c "$wantc" $OUT/log)" -ge 1 ] && check "df1: 100 sectors written at sector 500 read back identical" ok || check "df1: 100 sectors written at sector 500 read back identical" bad
got=$(dd if=$OUT/df1.adf bs=512 skip=500 count=100 2>/dev/null | sha256sum | cut -d' ' -f1)
[ "$got" = "$wantc" ] && check "the image file on the host holds the written sectors" ok || check "the image file on the host holds the written sectors" bad
rest=$(( $(cmp -l $OUT/df1.orig $OUT/df1.adf | awk '{ if ($1 <= 500*512 || $1 > 600*512) n++ } END { print n+0 }') ))
[ "$rest" = 0 ] && check "no other sector of the image changed" ok || check "no other sector of the image changed ($rest bytes)" bad
exit $((1 - pass))
